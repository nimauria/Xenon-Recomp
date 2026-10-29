#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <filesystem>

namespace xenon::launcher_next {
struct HttpResponse {
  int status = 0;
  std::string etag;
  std::uint64_t bytes = 0;
};
struct HttpTransfer {
  std::uint64_t limit = 1024 * 1024;
  std::string etag;
  std::function<bool()> cancelled;
  std::function<void(std::string_view)> write;
  std::function<void(std::uint64_t, std::uint64_t)> progress;
};
class IHttpClient {
public:
  virtual ~IHttpClient() = default;
  virtual HttpResponse get(const std::string &url, const HttpTransfer &transfer) = 0;
  virtual bool fixture() const { return false; }
};
bool allowed_update_url(const std::string &url);
std::shared_ptr<IHttpClient> platform_http();
std::shared_ptr<IHttpClient> fixture_http(const std::filesystem::path &root);
} // namespace xenon::launcher_next
