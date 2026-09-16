#pragma once

namespace xenon {

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

 private:
  bool initialized_ = false;
  RuntimeConfig config_{};
};

}  // namespace xenon
