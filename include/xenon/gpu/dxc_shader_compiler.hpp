#pragma once

#include <cstddef>
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

class ShaderCache {
 public:
  explicit ShaderCache(std::shared_ptr<DxcShaderCompiler> compiler);

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
};

}  // namespace xenon::gpu
