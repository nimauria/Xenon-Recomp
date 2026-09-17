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
