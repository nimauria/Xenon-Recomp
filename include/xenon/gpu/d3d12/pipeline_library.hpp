#pragma once

// Persistent D3D12 pipeline library (GPU V3): the graphics pipelines this
// device built in earlier sessions, loaded from their serialized driver
// state instead of being compiled again - the D3D12 counterpart of the
// Vulkan backend's VkPipelineCache. Saved next to the shader cache.
//
// A pipeline is stored under a name hashed from everything that defines it
// (shader bytecode, every state field, the root signature), so a changed
// pipeline gets a new name. A load the driver refuses - a library from
// another driver or device, a stale or colliding entry - falls back to
// creating the pipeline, which is then stored when its name is free.
//
// The library and the bytes it was created from stay alive while any
// pipeline loaded from it does: each GraphicsPipeline holds a reference.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

namespace xenon::gpu::d3d12 {

class PipelineLibrary {
 public:
  // An empty library when `path` is empty or holds nothing this device
  // accepts; null when the device has no pipeline libraries at all.
  // `salt` goes into every pipeline name: the root signature's hash, which
  // every pipeline of the library shares.
  static std::shared_ptr<PipelineLibrary> open(ID3D12Device* device,
                                               const std::filesystem::path& path,
                                               std::uint64_t salt);
  ~PipelineLibrary();
  PipelineLibrary(const PipelineLibrary&) = delete;
  PipelineLibrary& operator=(const PipelineLibrary&) = delete;

  // Pipelines from the library; null (and nothing changed) when it has no
  // pipeline of that name or the driver refuses it.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> load(const std::wstring& name,
                                                   const D3D12_PIPELINE_STATE_STREAM_DESC& desc);
  Microsoft::WRL::ComPtr<ID3D12PipelineState> load(const std::wstring& name,
                                                   const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
  // Adds a pipeline created outside the library, saved with the rest.
  void store(const std::wstring& name, ID3D12PipelineState* pipeline);
  // Writes the library to its path when it has new pipelines (written
  // beside, then renamed over: a crash mid-write keeps the previous file).
  void save();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t salt() const noexcept { return salt_; }
  [[nodiscard]] std::uint64_t loaded() const noexcept { return loaded_; }
  [[nodiscard]] std::uint64_t stored() const noexcept { return stored_; }
  [[nodiscard]] std::size_t initial_bytes() const noexcept { return blob_.size(); }

 private:
  PipelineLibrary() = default;
  std::mutex mutex_;
  Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> library_{};
  Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1> library1_{};
  // The serialized library it was created from: D3D12 reads it for the
  // library's lifetime.
  std::vector<std::byte> blob_{};
  std::filesystem::path path_{};
  std::uint64_t salt_{};
  bool dirty_{};
  std::uint64_t loaded_{};
  std::uint64_t stored_{};
};

// FNV-1a over bytes, for pipeline names.
struct PipelineNameHash {
  std::uint64_t value{0xCBF29CE484222325ull};
  void add(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
      value ^= bytes[i];
      value *= 0x100000001B3ull;
    }
  }
  template <typename T>
  void add(const T& value_to_add) noexcept {
    add(&value_to_add, sizeof(T));
  }
  [[nodiscard]] std::wstring name() const;
};

}  // namespace xenon::gpu::d3d12
