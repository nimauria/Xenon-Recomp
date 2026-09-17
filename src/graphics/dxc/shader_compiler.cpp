#include "xenon/gpu/dxc_shader_compiler.hpp"

#include <cstring>
#include <stdexcept>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ObjIdl.h>
#include <OleAuto.h>
#include <Unknwn.h>
#include <dxc/dxcapi.h>
#include <wrl/client.h>

namespace xenon::gpu {
namespace {
using Microsoft::WRL::ComPtr;

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
      utils.Reset();
    }
  }

  ComPtr<IDxcUtils> utils;
  ComPtr<IDxcCompiler3> compiler;
  std::string error;
};

DxcShaderCompiler::DxcShaderCompiler() : impl_(std::make_unique<Impl>()) {}
DxcShaderCompiler::~DxcShaderCompiler() = default;
DxcShaderCompiler::DxcShaderCompiler(DxcShaderCompiler&&) noexcept = default;
DxcShaderCompiler& DxcShaderCompiler::operator=(DxcShaderCompiler&&) noexcept = default;

bool DxcShaderCompiler::available() const noexcept {
  return impl_ && impl_->compiler && impl_->utils;
}
const std::string& DxcShaderCompiler::error() const noexcept { return impl_->error; }

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
    args.push_back(L"-fspv-reflect");
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
  auto compiled = std::make_shared<const CompiledShader>(compiler_->compile(shader, options));
  std::scoped_lock lock(mutex_);
  const auto [iterator, inserted] = entries_.emplace(key, compiled);
  if (inserted) ++misses_;
  else ++hits_;
  return iterator->second;
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
