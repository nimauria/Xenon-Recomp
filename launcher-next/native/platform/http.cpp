#include "http.hpp"
#include "core/archive.hpp"
#include "core/state_file.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <mutex>
#include <vector>
#include <cctype>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif
namespace xenon::launcher_next {
namespace {
struct Url {
  std::string host, path;
};
Url parse(const std::string &url) {
  if (url.rfind("https://", 0) != 0 || url.size() > 16384 ||
      url.find_first_of("\r\n\t #\\") != std::string::npos)
    throw std::runtime_error("Only valid HTTPS update URLs are accepted");
  const auto slash = url.find('/', 8);
  auto host = url.substr(8, slash == std::string::npos ? url.size() - 8 : slash - 8);
  if (host.empty() ||
      host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") !=
          std::string::npos)
    throw std::runtime_error("Invalid update host");
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
  return {host, slash == std::string::npos ? "/" : url.substr(slash)};
}
void check(const HttpTransfer &transfer) {
  if (transfer.cancelled && transfer.cancelled())
    throw OperationCancelled();
}
class FixtureHttp final : public IHttpClient {
  std::filesystem::path root_;

public:
  explicit FixtureHttp(std::filesystem::path root)
      : root_(std::filesystem::weakly_canonical(std::move(root))) {}
  bool fixture() const override { return true; }
  HttpResponse get(const std::string &url, const HttpTransfer &transfer) override {
    check(transfer);
    if (!allowed_update_url(url))
      throw std::runtime_error("Untrusted update URL");
    const auto mapping = read_document(root_ / "responses.json");
    if (!mapping.contains(url))
      throw std::runtime_error("Network fixture has no response for this URL");
    const auto row = mapping.at(url);
    HttpResponse response{row.value("status", 200), row.value("etag", ""), 0};
    if (!transfer.etag.empty() && transfer.etag == response.etag) {
      response.status = 304;
      return response;
    }
    if (response.status < 200 || response.status >= 300)
      return response;
    if (!row.contains("file"))
      return response;
    const auto file = std::filesystem::weakly_canonical(
        root_ / std::filesystem::u8path(row.at("file").get<std::string>()));
    const auto relative = file.lexically_relative(root_);
    if (relative.empty() || *relative.begin() == ".." || !std::filesystem::is_regular_file(file))
      throw std::runtime_error("Invalid network fixture body");
    const auto total = std::filesystem::file_size(file);
    if (total > transfer.limit)
      throw std::runtime_error("Response exceeds its size limit");
    std::ifstream input(file, std::ios::binary);
    std::array<char, 65536> bytes{};
    while (input) {
      check(transfer);
      input.read(bytes.data(), bytes.size());
      const auto count = input.gcount();
      if (count) {
        response.bytes += count;
        if (transfer.write)
          transfer.write({bytes.data(), static_cast<std::size_t>(count)});
        if (transfer.progress)
          transfer.progress(response.bytes, total);
      }
    }
    if (!input.eof())
      throw std::runtime_error("Could not read fixture response");
    return response;
  }
};
#ifdef _WIN32
std::wstring wide(const std::string &text) {
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (!size && !text.empty())
    throw std::runtime_error("Invalid HTTP text");
  std::wstring result(size, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), size);
  return result;
}
std::string utf8(const std::wstring &text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string result(size, 0);
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}
struct InternetHandle {
  HINTERNET value = nullptr;
  explicit InternetHandle(HINTERNET handle) : value(handle) {
    if (!value)
      throw std::runtime_error("Could not open an HTTPS connection");
  }
  ~InternetHandle() { WinHttpCloseHandle(value); }
  InternetHandle(const InternetHandle &) = delete;
};
struct AsyncRequest {
  std::mutex mutex;
  std::condition_variable changed;
  DWORD event = 0, error = 0, received = 0;
  bool closed = false, callback = false;
  std::array<char, 65536> buffer{};
  HINTERNET handle = nullptr;
  explicit AsyncRequest(HINTERNET connection, const std::string &path) {
    handle = WinHttpOpenRequest(connection, L"GET", wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!handle)
      throw std::runtime_error("Could not create an HTTPS request");
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
    if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(
            handle, status, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
            0) == WINHTTP_INVALID_STATUS_CALLBACK) {
      WinHttpCloseHandle(handle);
      handle = nullptr;
      throw std::runtime_error("Could not initialize HTTPS callbacks");
    }
    callback = true;
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) {
      close();
      throw std::runtime_error("Could not enforce HTTPS redirect policy");
    }
  }
  void close() noexcept {
    if (handle) {
      WinHttpCloseHandle(handle);
      handle = nullptr;
      if (callback) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return closed; });
      }
    }
  }
  ~AsyncRequest() { close(); }
  static void CALLBACK status(HINTERNET, DWORD_PTR context, DWORD code, LPVOID information,
                              DWORD length) noexcept {
    auto *self = reinterpret_cast<AsyncRequest *>(context);
    if (!self)
      return;
    std::lock_guard lock(self->mutex);
    if (code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
      self->closed = true;
    else if (code == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
      self->error = static_cast<WINHTTP_ASYNC_RESULT *>(information)->dwError;
    else {
      self->event |= code;
      if (code == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
        self->received = length;
    }
    self->changed.notify_all();
  }
  void begin() {
    std::lock_guard lock(mutex);
    event = error = received = 0;
  }
  void submitted(BOOL result) {
    if (!result && GetLastError() != ERROR_IO_PENDING)
      throw std::runtime_error("HTTPS request failed (Windows error " +
                               std::to_string(GetLastError()) + ")");
  }
  void wait(DWORD expected, const HttpTransfer &transfer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::unique_lock lock(mutex);
    for (;;) {
      check(transfer);
      if (error)
        throw std::runtime_error("HTTPS transfer failed (Windows error " + std::to_string(error) +
                                 ")");
      if (event & expected)
        return;
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("HTTPS transfer timed out");
      changed.wait_for(lock, std::chrono::milliseconds(100));
    }
  }
  std::string header(DWORD field) {
    DWORD size = 0;
    WinHttpQueryHeaders(handle, field, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      return {};
    std::wstring text(size / sizeof(wchar_t), 0);
    if (!WinHttpQueryHeaders(handle, field, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size,
                             WINHTTP_NO_HEADER_INDEX))
      return {};
    text.resize(size / sizeof(wchar_t));
    while (!text.empty() && text.back() == 0)
      text.pop_back();
    return utf8(text);
  }
};
class PlatformHttp final : public IHttpClient {
public:
  HttpResponse get(const std::string &original, const HttpTransfer &transfer) override {
    std::string url = original;
    InternetHandle session(WinHttpOpen(L"Xenon-Launcher-Next/0.1",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    WinHttpSetTimeouts(session.value, 10000, 10000, 10000, 10000);
    for (unsigned redirect = 0; redirect < 6; ++redirect) {
      check(transfer);
      if (!allowed_update_url(url))
        throw std::runtime_error("Update redirect points outside trusted HTTPS hosts");
      const auto parsed = parse(url);
      InternetHandle connection(
          WinHttpConnect(session.value, wide(parsed.host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
      AsyncRequest request(connection.value, parsed.path);
      std::wstring headers = L"Accept: application/vnd.github+json\r\n";
      if (!transfer.etag.empty()) {
        if (transfer.etag.find_first_of("\r\n") != std::string::npos)
          throw std::runtime_error("Invalid cache validator");
        headers += L"If-None-Match: " + wide(transfer.etag) + L"\r\n";
      }
      request.begin();
      request.submitted(
          WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(headers.size()),
                             WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(&request)));
      request.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, transfer);
      request.begin();
      request.submitted(WinHttpReceiveResponse(request.handle, nullptr));
      request.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, transfer);
      DWORD status = 0, length = sizeof(status);
      if (!WinHttpQueryHeaders(
              request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
              WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("Missing HTTPS response status");
      if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        url = request.header(WINHTTP_QUERY_LOCATION);
        continue;
      }
      HttpResponse response{static_cast<int>(status), request.header(WINHTTP_QUERY_ETAG), 0};
      if (status < 200 || status >= 300)
        return response;
      const auto size = request.header(WINHTTP_QUERY_CONTENT_LENGTH);
      std::uint64_t total = 0;
      if (!size.empty()) {
        try {
          total = std::stoull(size);
        } catch (...) {
          throw std::runtime_error("Invalid response length");
        }
        if (total > transfer.limit)
          throw std::runtime_error("Response exceeds its size limit");
      }
      for (;;) {
        request.begin();
        request.submitted(WinHttpReadData(request.handle, request.buffer.data(),
                                          static_cast<DWORD>(request.buffer.size()), nullptr));
        request.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, transfer);
        const auto count = request.received;
        if (!count)
          break;
        if (count > transfer.limit - response.bytes)
          throw std::runtime_error("Response exceeds its size limit");
        response.bytes += count;
        if (transfer.write)
          transfer.write({request.buffer.data(), count});
        if (transfer.progress)
          transfer.progress(response.bytes, total);
      }
      if (total && response.bytes != total)
        throw std::runtime_error("Incomplete HTTPS response");
      return response;
    }
    throw std::runtime_error("Too many HTTPS redirects");
  }
};
#else
struct CurlTransfer {
  const HttpTransfer &transfer;
  HttpResponse response;
  std::exception_ptr error;
};
class PlatformHttp final : public IHttpClient {
public:
  HttpResponse get(const std::string &original, const HttpTransfer &transfer) override {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK)
      throw std::runtime_error("Could not initialize HTTPS");
    std::string url = original;
    for (unsigned redirect = 0; redirect < 6; ++redirect) {
      check(transfer);
      if (!allowed_update_url(url))
        throw std::runtime_error("Untrusted update redirect");
      const auto handle =
          std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
      if (!handle)
        throw std::runtime_error("Could not create HTTPS request");
      CurlTransfer state{transfer, {}, {}};
      curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
      curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt(handle.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(handle.get(), CURLOPT_LOW_SPEED_TIME, 30L);
      curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "Xenon-Launcher-Next/0.1");
      curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
      curl_slist *raw = nullptr;
      raw = curl_slist_append(raw, "Accept: application/vnd.github+json");
      if (!transfer.etag.empty()) {
        if (transfer.etag.find_first_of("\r\n") != std::string::npos)
          throw std::runtime_error("Invalid cache validator");
        raw = curl_slist_append(raw, ("If-None-Match: " + transfer.etag).c_str());
      }
      const auto headers =
          std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(raw, curl_slist_free_all);
      curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, raw);
      curl_easy_setopt(
          handle.get(), CURLOPT_WRITEFUNCTION,
          +[](char *bytes, size_t size, size_t count, void *opaque) noexcept -> size_t {
            auto &s = *static_cast<CurlTransfer *>(opaque);
            const auto length = size * count;
            try {
              check(s.transfer);
              if (s.response.status < 200 || s.response.status >= 300)
                return length;
              if (length > s.transfer.limit - s.response.bytes)
                throw std::runtime_error("Response exceeds its size limit");
              s.response.bytes += length;
              if (s.transfer.write)
                s.transfer.write({bytes, length});
              return length;
            } catch (...) {
              s.error = std::current_exception();
              return 0;
            }
          });
      curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);
      curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(
          handle.get(), CURLOPT_XFERINFOFUNCTION,
          +[](void *opaque, curl_off_t total, curl_off_t done, curl_off_t,
              curl_off_t) noexcept -> int {
            auto &s = *static_cast<CurlTransfer *>(opaque);
            try {
              check(s.transfer);
              if (total > static_cast<curl_off_t>(s.transfer.limit))
                throw std::runtime_error("Response exceeds its size limit");
              if (s.transfer.progress)
                s.transfer.progress(done, total);
              return 0;
            } catch (...) {
              s.error = std::current_exception();
              return 1;
            }
          });
      curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &state);
      curl_easy_setopt(
          handle.get(), CURLOPT_HEADERFUNCTION,
          +[](char *bytes, size_t size, size_t count, void *opaque) noexcept -> size_t {
            auto &s = *static_cast<CurlTransfer *>(opaque);
            try {
              std::string line(bytes, size * count);
              if (line.rfind("HTTP/", 0) == 0) {
                const auto space = line.find(' ');
                if (space != std::string::npos)
                  s.response.status = std::stoi(line.substr(space + 1, 3));
                s.response.etag.clear();
              }
              auto key = line.substr(0, line.find(':'));
              std::transform(key.begin(), key.end(), key.begin(),
                             [](unsigned char c) { return std::tolower(c); });
              if (key == "etag") {
                auto value = line.substr(line.find(':') + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of("\r\n \t") + 1);
                s.response.etag = value;
              }
              return size * count;
            } catch (...) {
              s.error = std::current_exception();
              return 0;
            }
          });
      curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &state);
      const auto result = curl_easy_perform(handle.get());
      if (state.error)
        std::rethrow_exception(state.error);
      if (result != CURLE_OK)
        throw std::runtime_error(std::string("HTTPS transfer failed: ") +
                                 curl_easy_strerror(result));
      long status = 0;
      curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
      state.response.status = status;
      if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        char *location = nullptr;
        curl_easy_getinfo(handle.get(), CURLINFO_REDIRECT_URL, &location);
        if (!location)
          throw std::runtime_error("Missing HTTPS redirect location");
        url = location;
        continue;
      }
      return state.response;
    }
    throw std::runtime_error("Too many HTTPS redirects");
  }
};
#endif
} // namespace
bool allowed_update_url(const std::string &url) {
  try {
    const auto host = parse(url).host;
    return host == "api.github.com" || host == "raw.githubusercontent.com" ||
           host == "github.com" || host == "objects.githubusercontent.com" ||
           host == "release-assets.githubusercontent.com";
  } catch (...) {
    return false;
  }
}
std::shared_ptr<IHttpClient> platform_http() { return std::make_shared<PlatformHttp>(); }
std::shared_ptr<IHttpClient> fixture_http(const std::filesystem::path &root) {
  return std::make_shared<FixtureHttp>(root);
}
} // namespace xenon::launcher_next
