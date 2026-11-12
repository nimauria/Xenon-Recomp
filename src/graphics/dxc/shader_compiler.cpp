#include "xenon/gpu/dxc_shader_compiler.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <cstring>
#include <system_error>
#include <stdexcept>
#include <utility>

#include "xenon/logging/activity_trace.hpp"
#include "xenon/logging/probe_log.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ObjIdl.h>
#include <OleAuto.h>
#include <Unknwn.h>
#endif
#include <dxc/dxcapi.h>
#if defined(_WIN32)
#include <wrl/client.h>
#endif

namespace xenon::gpu {
namespace {
#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#else
template <typename T>
using ComPtr = CComPtr<T>;
#endif

std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    hash ^= static_cast<std::uint8_t>(value >> (i * 8u));
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint64_t cache_key(const LoweredShader& shader,
                        const ShaderCompileOptions& options) noexcept {
  auto key = mix(14695981039346656037ull, shader.translation_hash);
  key = mix(key, static_cast<std::uint64_t>(shader.stage));
  key = mix(key, static_cast<std::uint64_t>(options.format));
  key = mix(key, options.debug);
  key = mix(key, options.optimize);
  key = mix(key, options.warnings_as_errors);
  key = mix(key, options.spirv_stencil_export);
  for (unsigned char c : options.spirv_environment) key = mix(key, c);
  return key;
}

std::wstring widen_ascii(std::string_view value) {
  return std::wstring(value.begin(), value.end());
}
}  // namespace

class DxcShaderCompiler::Impl {
 public:
  Impl() {
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hr)) {
      error = "DxcCreateInstance(CLSID_DxcUtils) failed";
      return;
    }
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr)) {
      error = "DxcCreateInstance(CLSID_DxcCompiler) failed";
      utils = nullptr;
      return;
    }
    version = "dxc";
    ComPtr<IDxcVersionInfo> info;
    if (SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(&info)))) {
      UINT32 major = 0, minor = 0;
      if (SUCCEEDED(info->GetVersion(&major, &minor)))
        version += " " + std::to_string(major) + "." + std::to_string(minor);
    }
    ComPtr<IDxcVersionInfo2> commit_info;
    if (SUCCEEDED(compiler->QueryInterface(IID_PPV_ARGS(&commit_info)))) {
      UINT32 commits = 0;
      char* hash = nullptr;
      if (SUCCEEDED(commit_info->GetCommitInfo(&commits, &hash)) && hash) {
        version += " " + std::to_string(commits) + " " + hash;
        CoTaskMemFree(hash);
      }
    }
  }

  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcCompiler3> compiler;
  std::string error;
  std::string version;
};

DxcShaderCompiler::DxcShaderCompiler() : impl_(std::make_unique<Impl>()) {}
DxcShaderCompiler::~DxcShaderCompiler() = default;
DxcShaderCompiler::DxcShaderCompiler(DxcShaderCompiler&&) noexcept = default;
DxcShaderCompiler& DxcShaderCompiler::operator=(DxcShaderCompiler&&) noexcept = default;

bool DxcShaderCompiler::available() const noexcept {
  return impl_ && impl_->compiler && impl_->utils;
}
const std::string& DxcShaderCompiler::error() const noexcept { return impl_->error; }
const std::string& DxcShaderCompiler::version() const noexcept { return impl_->version; }

CompiledShader DxcShaderCompiler::compile(const LoweredShader& shader,
                                          const ShaderCompileOptions& options) const {
  CompiledShader output{};
  output.stage = shader.stage;
  output.format = options.format;
  output.source_hash = shader.source_hash;
  output.cache_key = cache_key(shader, options);
  output.entry_point = shader.entry_point;
  output.profile = shader.profile;
  if (!shader.complete) {
    output.diagnostics.emplace_back("DXC refused an incomplete HLSL translation");
    output.diagnostics.insert(output.diagnostics.end(), shader.diagnostics.begin(),
                              shader.diagnostics.end());
    return output;
  }
  if (!available()) {
    output.diagnostics.push_back(impl_ ? impl_->error : "DXC compiler is unavailable");
    return output;
  }

  const auto entry = widen_ascii(shader.entry_point);
  const auto profile = widen_ascii(shader.profile);
  const auto environment = widen_ascii("-fspv-target-env=" + options.spirv_environment);
  std::vector<LPCWSTR> args{L"xenon_generated.hlsl", L"-E", entry.c_str(), L"-T",
                            profile.c_str(), L"-HV", L"2021", L"-Ges",
                            L"-all-resources-bound"};
  if (options.warnings_as_errors) args.push_back(L"-WX");
  if (options.debug) {
    args.push_back(L"-Zi");
    args.push_back(L"-Qembed_debug");
  } else {
    args.push_back(L"-Qstrip_debug");
  }
  args.push_back(options.optimize ? L"-O3" : L"-Od");
  if (options.format == ShaderBinaryFormat::Spirv) {
    args.push_back(L"-spirv");
    args.push_back(environment.c_str());
    args.push_back(L"-fvk-use-dx-layout");
    if (options.spirv_stencil_export)
      args.push_back(L"-fspv-extension=SPV_EXT_shader_stencil_export");
    // Do not request DXC's Google reflection extensions. Project Xenon owns
    // the descriptor ABI explicitly, and enabling reflection metadata here
    // makes otherwise valid modules require VK_GOOGLE_* device extensions.
    // Keep HLSL register classes in non-overlapping Vulkan binding ranges.
    // Set zero is the common Xenon graphics ABI used by ResourceLayout.
    args.push_back(L"-fvk-b-shift"); args.push_back(L"0"); args.push_back(L"0");
    args.push_back(L"-fvk-t-shift"); args.push_back(L"16"); args.push_back(L"0");
    args.push_back(L"-fvk-s-shift"); args.push_back(L"160"); args.push_back(L"0");
    args.push_back(L"-fvk-u-shift"); args.push_back(L"192"); args.push_back(L"0");
  } else {
    args.push_back(L"-Qstrip_reflect");
  }

  DxcBuffer source{};
  source.Ptr = shader.hlsl.data();
  source.Size = shader.hlsl.size();
  source.Encoding = DXC_CP_UTF8;
  ComPtr<IDxcResult> result;
  HRESULT hr = impl_->compiler->Compile(&source, args.data(),
      static_cast<UINT32>(args.size()), nullptr, IID_PPV_ARGS(&result));
  if (FAILED(hr) || !result) {
    output.diagnostics.emplace_back("IDxcCompiler3::Compile failed before producing a result");
    return output;
  }

  ComPtr<IDxcBlobUtf8> errors;
  if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
      errors && errors->GetStringLength()) {
    output.diagnostics.emplace_back(errors->GetStringPointer(), errors->GetStringLength());
  }
  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status)) return output;

  ComPtr<IDxcBlob> object;
  if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object) {
    output.diagnostics.emplace_back("DXC succeeded but returned no shader object");
    return output;
  }
  output.binary.resize(object->GetBufferSize());
  std::memcpy(output.binary.data(), object->GetBufferPointer(), object->GetBufferSize());
  output.succeeded = !output.binary.empty();
  return output;
}

namespace {
struct UtilityShaderStore {
  std::mutex mutex;
  std::unique_ptr<DxcShaderCompiler> compiler;
  // Keyed by everything compile() reads, HLSL text included, so two shaders
  // can never share an entry.
  std::unordered_map<std::string, CompiledShader> entries;
  std::size_t compiles{};
};

UtilityShaderStore& utility_shader_store() {
  static UtilityShaderStore store;
  return store;
}

std::string utility_shader_key(const LoweredShader& shader,
                               const ShaderCompileOptions& options) {
  std::string key;
  key.reserve(shader.hlsl.size() + shader.entry_point.size() + shader.profile.size() +
              options.spirv_environment.size() + 16u);
  key.push_back(static_cast<char>(shader.stage));
  key.push_back(static_cast<char>(options.format));
  key.push_back(static_cast<char>((options.debug ? 1 : 0) | (options.optimize ? 2 : 0) |
                                  (options.warnings_as_errors ? 4 : 0) |
                                  (options.spirv_stencil_export ? 8 : 0) |
                                  (shader.complete ? 16 : 0)));
  for (const auto* part : {&options.spirv_environment, &shader.profile, &shader.entry_point}) {
    key.append(*part);
    key.push_back('\0');
  }
  key.append(shader.hlsl);
  return key;
}
}  // namespace

CompiledShader UtilityShaderCompiler::compile(const LoweredShader& shader,
                                              const ShaderCompileOptions& options) const {
  auto key = utility_shader_key(shader, options);
  auto& store = utility_shader_store();
  std::scoped_lock lock(store.mutex);
  if (const auto found = store.entries.find(key); found != store.entries.end())
    return found->second;
  if (!store.compiler) store.compiler = std::make_unique<DxcShaderCompiler>();
  auto compiled = store.compiler->compile(shader, options);
  ++store.compiles;
  // A failure is cached as well: the same input fails the same way, and the
  // caller reports its diagnostics.
  store.entries.emplace(std::move(key), compiled);
  return compiled;
}

std::size_t UtilityShaderCompiler::compiles() noexcept {
  auto& store = utility_shader_store();
  std::scoped_lock lock(store.mutex);
  return store.compiles;
}

namespace {

// A persistent entry: magic, then the full key and the binary, each
// length-prefixed.
constexpr std::array<char, 8> kDiskMagic{'X', 'S', 'H', 'C', 'A', 'C', 'H', '1'};

std::string disk_key(const LoweredShader& shader, const ShaderCompileOptions& options,
                     const std::string& compiler_version) {
  std::string key = compiler_version;
  key.push_back('\0');
  key.append(utility_shader_key(shader, options));
  return key;
}

std::filesystem::path disk_path(const std::filesystem::path& directory, const std::string& key) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : key) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.xsc", static_cast<unsigned long long>(hash));
  return directory / name;
}

void put_blob(std::string& out, std::string_view bytes) {
  const auto size = static_cast<std::uint64_t>(bytes.size());
  out.append(reinterpret_cast<const char*>(&size), sizeof(size));
  out.append(bytes);
}

bool get_blob(std::string_view& in, std::string_view& bytes) {
  std::uint64_t size{};
  if (in.size() < sizeof(size)) return false;
  std::memcpy(&size, in.data(), sizeof(size));
  in.remove_prefix(sizeof(size));
  if (in.size() < size) return false;
  bytes = in.substr(0, static_cast<std::size_t>(size));
  in.remove_prefix(static_cast<std::size_t>(size));
  return true;
}

// The stored binary for `key`, if an entry for exactly that input exists.
std::optional<CompiledShader> read_disk_entry(const std::filesystem::path& path,
                                              const std::string& key,
                                              const LoweredShader& shader,
                                              const ShaderCompileOptions& options) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::string_view in(data);
  if (in.size() < kDiskMagic.size() ||
      std::memcmp(in.data(), kDiskMagic.data(), kDiskMagic.size()) != 0)
    return std::nullopt;
  in.remove_prefix(kDiskMagic.size());
  std::string_view stored_key, binary;
  if (!get_blob(in, stored_key) || stored_key != key || !get_blob(in, binary) || binary.empty() ||
      !in.empty())
    return std::nullopt;
  CompiledShader compiled{};
  compiled.stage = shader.stage;
  compiled.format = options.format;
  compiled.source_hash = shader.source_hash;
  compiled.cache_key = cache_key(shader, options);
  compiled.entry_point = shader.entry_point;
  compiled.profile = shader.profile;
  compiled.binary.resize(binary.size());
  std::memcpy(compiled.binary.data(), binary.data(), binary.size());
  compiled.succeeded = true;
  return compiled;
}

// Written to a temporary name and renamed, so a reader never sees half a file.
bool write_disk_entry(const std::filesystem::path& path, const std::string& key,
                      const CompiledShader& compiled) {
  std::string out(kDiskMagic.begin(), kDiskMagic.end());
  put_blob(out, key);
  put_blob(out, std::string_view(reinterpret_cast<const char*>(compiled.binary.data()),
                                 compiled.binary.size()));
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!file) return false;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) std::filesystem::remove(temporary, ec);
  return !ec;
}

}  // namespace

ShaderCache::ShaderCache(std::shared_ptr<DxcShaderCompiler> compiler)
    : compiler_(std::move(compiler)) {
  if (!compiler_) throw std::invalid_argument("ShaderCache requires a compiler");
}

std::shared_ptr<const CompiledShader> ShaderCache::get_or_compile(
    const LoweredShader& shader, const ShaderCompileOptions& options) {
  const auto key = cache_key(shader, options);
  {
    std::scoped_lock lock(mutex_);
    if (auto found = entries_.find(key); found != entries_.end()) {
      ++hits_;
      return found->second;
    }
  }
  static const auto activity_key = xenon::logging::intern_activity(
      {"gpu", "shader compile", {"source", "hlsl_bytes", "spirv", ""}, false});
  std::optional<xenon::logging::ActivityScope> activity;
  if (xenon::logging::activity_trace_enabled())
    activity.emplace(activity_key,
                     std::array<std::uint64_t, 4>{shader.source_hash, shader.hlsl.size(),
                                                  options.format == ShaderBinaryFormat::Spirv,
                                                  0u});
  // A previous session's compile of exactly this input.
  const auto persistent_directory = disk_directory();
  std::string persistent_key;
  std::filesystem::path persistent_path;
  if (!persistent_directory.empty()) {
    persistent_key = disk_key(shader, options, compiler_->version());
    persistent_path = disk_path(persistent_directory, persistent_key);
    if (auto stored = read_disk_entry(persistent_path, persistent_key, shader, options)) {
      auto loaded = std::make_shared<const CompiledShader>(std::move(*stored));
      std::scoped_lock lock(mutex_);
      ++disk_hits_;
      const auto [iterator, inserted] = entries_.emplace(key, loaded);
      if (!inserted) ++hits_;
      return iterator->second;
    }
  }
  const auto compile_start = std::chrono::steady_clock::now();
  auto compiled = std::make_shared<const CompiledShader>(compiler_->compile(shader, options));
  if (!persistent_path.empty() && compiled->succeeded &&
      write_disk_entry(persistent_path, persistent_key, *compiled)) {
    std::scoped_lock lock(mutex_);
    ++disk_writes_;
  }
  const auto compile_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - compile_start)
                              .count();
  // Compiles run on the GPU pump thread; a slow one delays everything the
  // pump does, vsync interrupts included.
  if (compile_ms >= 50) {
    xenon::logging::append_probe_log(
        "shader_compile_diag.log", "DXC compile %lld ms: %s %s, %zu HLSL bytes, source 0x%016llX\n",
        static_cast<long long>(compile_ms), shader.profile.c_str(), shader.entry_point.c_str(),
        shader.hlsl.size(), static_cast<unsigned long long>(shader.source_hash));
  }
  std::scoped_lock lock(mutex_);
  const auto [iterator, inserted] = entries_.emplace(key, compiled);
  if (inserted) ++misses_;
  else ++hits_;
  return iterator->second;
}

void ShaderCache::set_disk_directory(std::filesystem::path directory) {
  if (!directory.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    // An unusable directory leaves the cache memory-only, and says so.
    if (ec || !std::filesystem::is_directory(directory, ec)) {
      xenon::logging::append_probe_log("shader_compile_diag.log",
                                       "shader cache directory %s is unusable; memory only\n",
                                       directory.string().c_str());
      directory.clear();
    }
  }
  std::scoped_lock lock(mutex_);
  disk_directory_ = std::move(directory);
}

std::filesystem::path ShaderCache::disk_directory() const {
  std::scoped_lock lock(mutex_);
  return disk_directory_;
}

std::size_t ShaderCache::disk_hits() const {
  std::scoped_lock lock(mutex_);
  return disk_hits_;
}

std::size_t ShaderCache::disk_writes() const {
  std::scoped_lock lock(mutex_);
  return disk_writes_;
}

void ShaderCache::clear() {
  std::scoped_lock lock(mutex_);
  entries_.clear();
  hits_ = misses_ = 0;
}
std::size_t ShaderCache::size() const {
  std::scoped_lock lock(mutex_);
  return entries_.size();
}
std::size_t ShaderCache::hits() const {
  std::scoped_lock lock(mutex_);
  return hits_;
}
std::size_t ShaderCache::misses() const {
  std::scoped_lock lock(mutex_);
  return misses_;
}

}  // namespace xenon::gpu
