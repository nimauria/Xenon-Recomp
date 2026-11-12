#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "xenon/gpu/shader_translation.hpp"

namespace xenon::gpu {

class DxcShaderCompiler {
 public:
  DxcShaderCompiler();
  ~DxcShaderCompiler();
  DxcShaderCompiler(DxcShaderCompiler&&) noexcept;
  DxcShaderCompiler& operator=(DxcShaderCompiler&&) noexcept;
  DxcShaderCompiler(const DxcShaderCompiler&) = delete;
  DxcShaderCompiler& operator=(const DxcShaderCompiler&) = delete;

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;
  // The DXC build compiling (major.minor plus commit), part of every
  // persistent cache key: another compiler may emit other code.
  [[nodiscard]] const std::string& version() const noexcept;
  [[nodiscard]] CompiledShader compile(
      const LoweredShader& shader, const ShaderCompileOptions& options = {}) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Compiles the fixed helper shaders backends build their transfer, resolve,
// clear and scanout pipelines from. Each depends only on its HLSL and
// options, so it is compiled once per process and served from memory after
// that: backends build those pipelines per transfer, and compiling there put
// DXC on every depth readback and upload (AC6's first cutscene ran at about
// 5 fps). Thread-safe; one DXC instance compiles under a lock.
class UtilityShaderCompiler {
 public:
  [[nodiscard]] CompiledShader compile(
      const LoweredShader& shader, const ShaderCompileOptions& options = {}) const;
  // Number of DXC compiles performed so far (cache misses), for tests.
  [[nodiscard]] static std::size_t compiles() noexcept;
};

// Compiled title shaders, in memory and - once set_disk_directory() names a
// directory - on disk, so a later session loads them instead of compiling
// them on the draw that first uses them (AC6 compiles take 50 ms to 1.3 s
// each). An entry holds its full input (HLSL, entry point, profile, options,
// DXC version) and is used only when that matches exactly; only successful
// compiles are stored.
class ShaderCache {
 public:
  explicit ShaderCache(std::shared_ptr<DxcShaderCompiler> compiler);

  // Empty: memory only (the "Session only" shader cache mode).
  void set_disk_directory(std::filesystem::path directory);
  [[nodiscard]] std::filesystem::path disk_directory() const;
  [[nodiscard]] std::size_t disk_hits() const;
  [[nodiscard]] std::size_t disk_writes() const;

  [[nodiscard]] std::shared_ptr<const CompiledShader> get_or_compile(
      const LoweredShader& shader, const ShaderCompileOptions& options = {});
  void clear();
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t hits() const;
  [[nodiscard]] std::size_t misses() const;

 private:
  std::shared_ptr<DxcShaderCompiler> compiler_;
  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>> entries_;
  std::size_t hits_{};
  std::size_t misses_{};
  std::filesystem::path disk_directory_{};
  std::size_t disk_hits_{};
  std::size_t disk_writes_{};
};

}  // namespace xenon::gpu
