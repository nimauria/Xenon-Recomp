#include "discord_rpc.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
namespace xenon::launcher_next {
namespace {
constexpr std::size_t frame_limit = 64 * 1024;
std::uint32_t integer(const unsigned char *p) {
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
         std::uint32_t(p[3]) << 24;
}
std::vector<std::string> default_endpoints() {
  std::vector<std::string> paths;
#ifdef _WIN32
  for (unsigned i = 0; i < 10; ++i)
    paths.push_back("\\\\?\\pipe\\discord-ipc-" + std::to_string(i));
#else
  std::vector<std::string> roots;
  for (const auto *name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
    if (const auto *value = std::getenv(name); value && *value && std::strlen(value) < 512)
      if (std::find(roots.begin(), roots.end(), value) == roots.end())
        roots.push_back(value);
  roots.push_back("/tmp");
  for (const auto &root : roots)
    for (unsigned i = 0; i < 10; ++i)
      paths.push_back(root + "/discord-ipc-" + std::to_string(i));
#endif
  return paths;
}
class Transport final : public IPresenceTransport {
public:
  explicit Transport(std::vector<std::string> endpoints)
      : endpoints_(endpoints.empty() ? default_endpoints() : std::move(endpoints)) {}
  ~Transport() override { close(); }
  bool open() override {
    close();
    for (const auto &endpoint : endpoints_) {
#ifdef _WIN32
      const auto path = std::filesystem::u8path(endpoint).wstring();
      handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
      if (handle_ != INVALID_HANDLE_VALUE)
        return true;
#else
      sockaddr_un address{};
      if (endpoint.size() >= sizeof(address.sun_path))
        continue;
      struct stat metadata{};
      if (::stat(endpoint.c_str(), &metadata) != 0 || !S_ISSOCK(metadata.st_mode) ||
          metadata.st_uid != geteuid())
        continue;
      handle_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (handle_ < 0)
        continue;
      if (fcntl(handle_, F_SETFL, fcntl(handle_, F_GETFL) | O_NONBLOCK) < 0) {
        close();
        continue;
      }
#ifdef __APPLE__
      int one = 1;
      setsockopt(handle_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
      if (::connect(handle_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
        return true;
      if (errno == EINPROGRESS) {
        pollfd descriptor{handle_, POLLOUT, 0};
        int error = 0;
        socklen_t size = sizeof(error);
        if (::poll(&descriptor, 1, 25) > 0 &&
            getsockopt(handle_, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error)
          return true;
      }
      close();
#endif
    }
    return false;
  }
  void close() override {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (handle_ >= 0) {
      ::close(handle_);
      handle_ = -1;
    }
#endif
    incoming_.clear();
  }
  void send(const RpcFrame &frame) override {
    const auto bytes = encode_rpc_frame(frame);
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (offset < bytes.size()) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Discord IPC write timed out");
#ifdef _WIN32
      offset += transfer(false, const_cast<unsigned char *>(bytes.data()) + offset,
                         static_cast<DWORD>(bytes.size() - offset));
#else
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto count = ::send(handle_, bytes.data() + offset, bytes.size() - offset, flags);
      if (count > 0)
        offset += static_cast<std::size_t>(count);
      else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        pollfd descriptor{handle_, POLLOUT, 0};
        ::poll(&descriptor, 1, 10);
      } else
        throw std::runtime_error("Discord IPC connection closed");
#endif
    }
  }
  std::vector<RpcFrame> poll() override {
    std::array<unsigned char, 8192> data{};
    for (unsigned reads = 0; reads < 16; ++reads) {
      std::size_t count = 0;
#ifdef _WIN32
      DWORD available = 0;
      if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr))
        throw std::runtime_error("Discord IPC connection closed");
      if (!available)
        break;
      count =
          transfer(true, data.data(), std::min<DWORD>(available, static_cast<DWORD>(data.size())));
#else
      const auto received = ::recv(handle_, data.data(), data.size(), 0);
      if (received == 0)
        throw std::runtime_error("Discord IPC connection closed");
      if (received < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        throw std::runtime_error("Discord IPC read failed");
      }
      count = static_cast<std::size_t>(received);
#endif
      incoming_.insert(incoming_.end(), data.begin(), data.begin() + count);
      if (incoming_.size() > 2 * frame_limit + 8)
        throw std::runtime_error("Discord IPC receive buffer exceeded its limit");
    }
    return decode_rpc_frames(incoming_);
  }

private:
#ifdef _WIN32
  DWORD transfer(bool reading, unsigned char *buffer, DWORD length) {
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent)
      throw std::runtime_error("Could not create the Discord IPC event");
    DWORD count = 0;
    const auto started = reading ? ReadFile(handle_, buffer, length, &count, &operation)
                                 : WriteFile(handle_, buffer, length, &count, &operation);
    bool ok = started != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      if (WaitForSingleObject(operation.hEvent, 100) != WAIT_OBJECT_0) {
        CancelIoEx(handle_, &operation);
        GetOverlappedResult(handle_, &operation, &count,
                            TRUE); // Drain cancellation before releasing the operation/buffer.
      } else
        ok = GetOverlappedResult(handle_, &operation, &count, FALSE) != FALSE;
    }
    CloseHandle(operation.hEvent);
    if (!ok || !count)
      throw std::runtime_error("Discord IPC transfer failed or timed out");
    return count;
  }
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int handle_ = -1;
#endif
  std::vector<std::string> endpoints_;
  std::vector<unsigned char> incoming_;
};
} // namespace
std::vector<unsigned char> encode_rpc_frame(const RpcFrame &frame) {
  const auto body = frame.raw ? *frame.raw : frame.payload.dump();
  if (body.size() > frame_limit || frame.opcode > 4)
    throw std::runtime_error("Invalid Discord IPC frame");
  std::vector<unsigned char> bytes;
  for (const auto value : {frame.opcode, static_cast<std::uint32_t>(body.size())})
    for (unsigned shift = 0; shift < 32; shift += 8)
      bytes.push_back(static_cast<unsigned char>(value >> shift));
  bytes.insert(bytes.end(), body.begin(), body.end());
  return bytes;
}
std::vector<RpcFrame> decode_rpc_frames(std::vector<unsigned char> &buffer) {
  std::vector<RpcFrame> result;
  std::size_t offset = 0;
  while (buffer.size() - offset >= 8) {
    const auto opcode = integer(buffer.data() + offset),
               length = integer(buffer.data() + offset + 4);
    if (opcode > 4 || length > frame_limit)
      throw std::runtime_error("Invalid Discord IPC frame header");
    if (buffer.size() - offset < length + 8)
      break;
    if (opcode == 3 || opcode == 4)
      result.push_back(
          {opcode, nullptr,
           std::string(buffer.begin() + offset + 8, buffer.begin() + offset + 8 + length)});
    else {
      auto value = Json::parse(buffer.begin() + offset + 8, buffer.begin() + offset + 8 + length);
      result.push_back({opcode, std::move(value)});
    }
    offset += length + 8;
  }
  buffer.erase(buffer.begin(), buffer.begin() + offset);
  return result;
}
std::unique_ptr<IPresenceTransport> discord_rpc_transport(std::vector<std::string> endpoints) {
  return std::make_unique<Transport>(std::move(endpoints));
}
} // namespace xenon::launcher_next
