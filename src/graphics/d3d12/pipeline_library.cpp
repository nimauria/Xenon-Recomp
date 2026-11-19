#include "xenon/gpu/d3d12/pipeline_library.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace xenon::gpu::d3d12 {

std::shared_ptr<PipelineLibrary> PipelineLibrary::open(ID3D12Device* device,
                                                       const std::filesystem::path& path,
                                                       std::uint64_t salt) {
  Microsoft::WRL::ComPtr<ID3D12Device1> device1;
  if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return nullptr;
  std::shared_ptr<PipelineLibrary> library(new PipelineLibrary());
  library->path_ = path;
  library->salt_ = salt;
  if (!path.empty()) {
    std::ifstream file(path, std::ios::binary);
    if (file) {
      const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
      library->blob_.resize(bytes.size());
      if (!bytes.empty()) std::memcpy(library->blob_.data(), bytes.data(), bytes.size());
    }
  }
  // A library serialized by another driver or adapter, or damaged, is
  // refused (D3D12_ERROR_DRIVER_VERSION_MISMATCH, ..._ADAPTER_NOT_FOUND,
  // E_INVALIDARG): start an empty one, which replaces it when saved.
  if (!library->blob_.empty() &&
      FAILED(device1->CreatePipelineLibrary(library->blob_.data(), library->blob_.size(),
                                            IID_PPV_ARGS(&library->library_)))) {
    library->blob_.clear();
    library->library_.Reset();
  }
  // DXGI_ERROR_UNSUPPORTED: the driver has no pipeline libraries (its own
  // shader cache is all there is).
  if (!library->library_ &&
      FAILED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library->library_))))
    return nullptr;
  static_cast<void>(library->library_.As(&library->library1_));
  return library;
}

PipelineLibrary::~PipelineLibrary() = default;

Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineLibrary::load(
    const std::wstring& name, const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  std::scoped_lock lock(mutex_);
  if (!library1_ ||
      FAILED(library1_->LoadPipeline(name.c_str(), &desc, IID_PPV_ARGS(&pipeline))))
    return nullptr;
  ++loaded_;
  return pipeline;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineLibrary::load(
    const std::wstring& name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  std::scoped_lock lock(mutex_);
  if (!library_ ||
      FAILED(library_->LoadGraphicsPipeline(name.c_str(), &desc, IID_PPV_ARGS(&pipeline))))
    return nullptr;
  ++loaded_;
  return pipeline;
}

void PipelineLibrary::store(const std::wstring& name, ID3D12PipelineState* pipeline) {
  std::scoped_lock lock(mutex_);
  // E_INVALIDARG when the name is taken (a stale entry the driver refused to
  // load): the pipeline is simply not stored.
  if (!library_ || !pipeline || FAILED(library_->StorePipeline(name.c_str(), pipeline))) return;
  dirty_ = true;
  ++stored_;
}

void PipelineLibrary::save() {
  std::scoped_lock lock(mutex_);
  if (!library_ || !dirty_ || path_.empty()) return;
  const auto size = library_->GetSerializedSize();
  if (!size) return;
  std::vector<std::byte> data(size);
  if (FAILED(library_->Serialize(data.data(), size))) return;
  std::error_code ignored;
  std::filesystem::create_directories(path_.parent_path(), ignored);
  auto temporary = path_;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
    if (!file) return;
  }
  std::filesystem::rename(temporary, path_, ignored);
  if (!ignored) dirty_ = false;
}

std::wstring PipelineNameHash::name() const {
  wchar_t text[24];
  std::swprintf(text, std::size(text), L"xenon-%016llX", static_cast<unsigned long long>(value));
  return text;
}

}  // namespace xenon::gpu::d3d12
