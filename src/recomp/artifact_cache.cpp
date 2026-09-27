#include "xenon/recomp/artifact_cache.hpp"
#include "xenon/recomp/compilation_graph.hpp"

#include "xenon/xbox/xex_crypto.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <span>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace xenon::recomp {
namespace {

constexpr const char* kManifestFileName = ".xenon-artifact-manifest";

std::string hex_encode(const xbox::crypto::Sha1Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    const auto value = std::to_integer<unsigned char>(byte);
    out.push_back(kHex[(value >> 4u) & 0xFu]);
    out.push_back(kHex[value & 0xFu]);
  }
  return out;
}

std::string current_iso8601_utc() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

struct ManifestData {
  std::string key_digest;
  std::string native_extension_relative;
  std::string built_at;
  std::string native_sha256;
};

bool write_manifest(const std::filesystem::path& dir, const std::string& key_digest,
                    const std::filesystem::path& native_extension_relative,
                    const std::string& built_at, std::string& error) {
  std::ofstream out(dir / kManifestFileName, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "failed to write artifact manifest in " + dir.string();
    return false;
  }
  out << "key_digest=" << key_digest << "\n";
  out << "native_extension_relative=" << native_extension_relative.generic_string() << "\n";
  out << "built_at=" << built_at << "\n";
  std::ifstream module(dir / native_extension_relative, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(module)), {});
  out << "native_sha256=" << graph::digest(bytes) << "\n";
  out.flush();
  if (!out) { error="failed to flush artifact manifest"; return false; }

  return true;
}

bool read_manifest(const std::filesystem::path& dir, ManifestData& out) {
  std::ifstream in(dir / kManifestFileName, std::ios::binary);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    const auto key = line.substr(0, pos);
    const auto value = line.substr(pos + 1);
    if (key == "native_sha256") out.native_sha256 = value;
    if (key == "key_digest") out.key_digest = value;
    else if (key == "native_extension_relative") out.native_extension_relative = value;
    else if (key == "built_at") out.built_at = value;
  }
  return !out.native_extension_relative.empty();
}

std::uint64_t random_component() {
  std::random_device rd;
  return (static_cast<std::uint64_t>(rd()) << 32u) | static_cast<std::uint64_t>(rd());
}

}  // namespace

std::string ArtifactCacheKey::digest() const {
  std::string canonical;
  canonical.reserve(256);
  const auto append = [&](std::string_view label, std::string_view value) {
    canonical += label;
    canonical += '=';
    canonical += std::to_string(value.size()) + ":" + std::string(value);
    canonical += ';';
  };
  append("title_id", std::to_string(title_id));
  append("media_id", std::to_string(media_id));
  append("effective_image_hash", effective_image_hash);
  append("xex_relative_path", xex_relative_path);
  append("module_id", module_id);
  append("module_compatibility_version", module_compatibility_version);
  append("hint_set_hash", std::to_string(hint_set_hash));
  append("adaptive_observation_hash", std::to_string(adaptive_observation_hash));
  append("knowledge_base_hash", std::to_string(knowledge_base_hash));
  append("abi_version", std::to_string(abi_version));
  append("preparation_identity", preparation_identity);
  append("target_arch", target_arch);
  append("build_config", build_config);

  const std::span<const std::byte> bytes(
      reinterpret_cast<const std::byte*>(canonical.data()), canonical.size());
  return hex_encode(xbox::crypto::sha1(bytes));
}

#if defined(_WIN32)
bool validate_native_module(const std::filesystem::path& path, std::string& error) {
  error.clear();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    error = "native module file does not exist: " + path.string();
    return false;
  }
  const HMODULE handle = LoadLibraryW(path.c_str());
  if (handle == nullptr) {
    error = "failed to load native module (LoadLibraryW error " +
            std::to_string(static_cast<unsigned long>(GetLastError())) + "): " + path.string();
    return false;
  }
  const auto proc = GetProcAddress(handle, "Xenon_BindCompiledRegistry");
  const bool ok = proc != nullptr;
  if (!ok) {
    error = "native module does not export Xenon_BindCompiledRegistry: " + path.string();
  }
  FreeLibrary(handle);
  return ok;
}
#else
bool validate_native_module(const std::filesystem::path& path, std::string& error) {
  error.clear();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    error = "native module file does not exist: " + path.string();
    return false;
  }
  dlerror();
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const auto* message = dlerror();
    error = std::string("failed to load native module: ") + (message ? message : "unknown error");
    return false;
  }
  dlerror();
  void* proc = dlsym(handle, "Xenon_BindCompiledRegistry");
  const bool ok = proc != nullptr;
  if (!ok) {
    error = "native module does not export Xenon_BindCompiledRegistry: " + path.string();
  }
  dlclose(handle);
  return ok;
}
#endif

ArtifactStagingBuild::ArtifactStagingBuild(std::filesystem::path store_root, std::string key_digest,
                                           std::filesystem::path directory) noexcept
    : store_root_(std::move(store_root)),
      key_digest_(std::move(key_digest)),
      directory_(std::move(directory)) {}

ArtifactStagingBuild::ArtifactStagingBuild(ArtifactStagingBuild&& other) noexcept
    : store_root_(std::move(other.store_root_)),
      key_digest_(std::move(other.key_digest_)),
      directory_(std::move(other.directory_)),
      resolved_(other.resolved_) {
  other.resolved_ = true;
}

ArtifactStagingBuild& ArtifactStagingBuild::operator=(ArtifactStagingBuild&& other) noexcept {
  if (this != &other) {
    if (!resolved_) discard();
    store_root_ = std::move(other.store_root_);
    key_digest_ = std::move(other.key_digest_);
    directory_ = std::move(other.directory_);
    resolved_ = other.resolved_;
    other.resolved_ = true;
  }
  return *this;
}

ArtifactStagingBuild::~ArtifactStagingBuild() {
  if (!resolved_) discard();
}

void ArtifactStagingBuild::discard() noexcept {
  if (resolved_) return;
  std::error_code ec;
  std::filesystem::remove_all(directory_, ec);
  resolved_ = true;
}

bool ArtifactStagingBuild::commit(const std::filesystem::path& native_extension_relative_file,
                                  ArtifactCacheEntry& out_entry, std::string& error) {
  out_entry = ArtifactCacheEntry{};
  if (resolved_) {
    error = "staging build was already committed or discarded";
    return false;
  }

  const auto module_path = directory_ / native_extension_relative_file;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(module_path, ec) || ec) {
    error = "expected compiled native module does not exist: " + module_path.string();
    return false;
  }
  if (!validate_native_module(module_path, error)) {
    return false;  // error already set by validate_native_module
  }

  const auto built_at = current_iso8601_utc();
  if (!write_manifest(directory_, key_digest_, native_extension_relative_file, built_at, error)) {
    return false;
  }

  const auto entries_root = store_root_ / "entries";
  std::filesystem::create_directories(entries_root, ec);
  const auto entry_path = entries_root / key_digest_;
  const auto backup_path =
      entries_root / (key_digest_ + ".rollback-" + std::to_string(random_component()));

  const bool had_old = std::filesystem::exists(entry_path, ec);
  if (had_old) {
    std::filesystem::rename(entry_path, backup_path, ec);
    if (ec) {
      error = "failed to stage the previous artifact aside for atomic replace: " + ec.message();
      return false;  // entry_path untouched; caller's staging dir is discarded by the destructor
    }
  }

  std::filesystem::rename(directory_, entry_path, ec);
  if (ec) {
    error = "failed to promote the staged artifact: " + ec.message();
    if (had_old) {
      std::error_code restore_ec;
      std::filesystem::rename(backup_path, entry_path, restore_ec);
      if (restore_ec) {
        error += "; additionally failed to roll back the previous artifact (left at " +
                 backup_path.string() + "): " + restore_ec.message();
      }
    }
    return false;
  }

  if (had_old) {
    std::error_code cleanup_ec;
    std::filesystem::remove_all(backup_path, cleanup_ec);  // best-effort; a leftover is harmless
  }

  resolved_ = true;
  out_entry.status = ArtifactCacheStatus::Fresh;
  out_entry.native_extension_path = entry_path / native_extension_relative_file;
  out_entry.built_at_iso8601 = built_at;
  return true;
}

ArtifactCacheStore::ArtifactCacheStore(std::filesystem::path root) : root_(std::move(root)) {
  std::error_code ec;
  std::filesystem::create_directories(root_ / "entries", ec);
}

ArtifactCacheEntry ArtifactCacheStore::lookup(const ArtifactCacheKey& key) const {
  ArtifactCacheEntry entry{};
  const auto digest = key.digest();
  const auto entry_dir = root_ / "entries" / digest;

  std::error_code ec;
  if (!std::filesystem::is_directory(entry_dir, ec) || ec) {
    entry.status = ArtifactCacheStatus::Missing;
    return entry;
  }

  ManifestData manifest;
  if (!read_manifest(entry_dir, manifest) || manifest.key_digest != digest) {
    entry.status = ArtifactCacheStatus::Invalid;
    return entry;
  }

  const auto module_path = entry_dir / std::filesystem::path(manifest.native_extension_relative);
  std::ifstream module(module_path, std::ios::binary);
  const std::string module_bytes((std::istreambuf_iterator<char>(module)), {});
  module.close();
  std::string validate_error;
  if (manifest.native_sha256 != graph::digest(module_bytes) || !validate_native_module(module_path, validate_error)) {
    entry.status = ArtifactCacheStatus::Invalid;
    return entry;
  }

  entry.status = ArtifactCacheStatus::Fresh;
  entry.native_extension_path = module_path;
  entry.built_at_iso8601 = manifest.built_at;
  return entry;
}

ArtifactStagingBuild ArtifactCacheStore::begin_staging(const ArtifactCacheKey& key) const {
  const auto digest = key.digest();
  const auto staging_root = root_ / "staging";
  std::error_code ec;
  std::filesystem::create_directories(staging_root, ec);

  const auto dir_name = digest + "-" + std::to_string(random_component());
  const auto dir = staging_root / dir_name;
  std::filesystem::create_directories(dir, ec);

  return ArtifactStagingBuild(root_, digest, dir);
}

void ArtifactCacheStore::clean_stale_staging() const {
  std::error_code ec;
  const auto staging_root = root_ / "staging";
  if (std::filesystem::exists(staging_root, ec)) {
    std::filesystem::remove_all(staging_root, ec);
  }

  const auto entries_root = root_ / "entries";
  if (!std::filesystem::exists(entries_root, ec)) return;
  for (auto it = std::filesystem::directory_iterator(entries_root, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (it->path().filename().string().find(".rollback-") != std::string::npos) {
      std::error_code remove_ec;
      std::filesystem::remove_all(it->path(), remove_ec);
    }
  }
}

}  // namespace xenon::recomp
