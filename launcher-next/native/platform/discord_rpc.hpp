#pragma once
#include "core/state_file.hpp"
#include <memory>
#include <vector>
#include <optional>
namespace xenon::launcher_next {
struct RpcFrame {
  std::uint32_t opcode;
  Json payload;
  std::optional<std::string> raw = {};
};
std::vector<unsigned char> encode_rpc_frame(const RpcFrame &frame);
std::vector<RpcFrame> decode_rpc_frames(std::vector<unsigned char> &buffer);
// Owned exclusively by the presence worker. No renderer/native UI thread waits on IPC.
class IPresenceTransport {
public:
  virtual ~IPresenceTransport() = default;
  virtual bool open() = 0;
  virtual void send(const RpcFrame &frame) = 0;
  virtual std::vector<RpcFrame> poll() = 0;
  virtual void close() = 0;
};
// Non-empty endpoints are an explicit test seam, never a renderer command.
std::unique_ptr<IPresenceTransport> discord_rpc_transport(std::vector<std::string> endpoints = {});
} // namespace xenon::launcher_next
