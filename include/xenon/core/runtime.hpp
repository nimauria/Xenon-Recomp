#pragma once

#include <memory>

namespace xenon {

namespace filesystem {
class VirtualFileSystem;
}

struct RuntimeConfig {
  bool enable_logging = true;
  bool enable_graphics = false;
  bool enable_audio = false;
  bool enable_input = false;
  bool enable_network = false;
};

class Runtime {
 public:
  Runtime();
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  bool initialize(const RuntimeConfig& config);
  void shutdown();
  bool is_initialized() const noexcept;

  // VFS integration for guest file I/O
  void set_filesystem(std::shared_ptr<filesystem::VirtualFileSystem> vfs);
  std::shared_ptr<filesystem::VirtualFileSystem> filesystem() const;

 private:
  bool initialized_ = false;
  RuntimeConfig config_{};
  std::shared_ptr<filesystem::VirtualFileSystem> vfs_;
};

}  // namespace xenon
