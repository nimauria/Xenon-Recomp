#include "application/launcher_service.hpp"
#include "core/launcher_update.hpp"
#include "include/cef_app.h"
#include "include/base/cef_compiler_specific.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_path_util.h"
#include "include/cef_request_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_display.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_message_router.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "platform/desktop.hpp"
#include <condition_variable>
#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <queue>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include "include/cef_sandbox_win.h"
#endif

namespace {
using xenon::launcher_next::Json;
using xenon::launcher_next::LauncherService;
namespace fs = std::filesystem;
bool restart_requested = false;
bool restart_safe = false;
fs::path update_receipt;
std::string update_token;
#ifdef __APPLE__
CefRefPtr<CefBrowser> primary_browser;
#endif
bool trusted(CefRefPtr<CefFrame> frame) {
  return frame && frame->IsMain() && frame->GetURL().ToString() == "xenon://app/";
}
class StateSignal : public std::enable_shared_from_this<StateSignal> {
public:
  CefRefPtr<CefBrowser> browser; // UI thread only; cleared before closing the browser.
  void notify(std::int64_t revision);
  void deliver() {
    scheduled_.store(false);
    if (!browser || !browser->IsValid())
      return;
    const auto frame = browser->GetMainFrame();
    if (trusted(frame))
      frame->ExecuteJavaScript(
          "window.dispatchEvent(new CustomEvent('xenon:revision',{detail:{version:1,revision:" +
              std::to_string(revision_.load()) + "}}));",
          "xenon://app/", 0);
  }

private:
  std::atomic_bool scheduled_{false};
  std::atomic<std::int64_t> revision_{0};
};
class StateSignalTask : public CefTask {
public:
  explicit StateSignalTask(std::shared_ptr<StateSignal> signal) : signal_(std::move(signal)) {}
  void Execute() override { signal_->deliver(); }

private:
  std::shared_ptr<StateSignal> signal_;
  IMPLEMENT_REFCOUNTING(StateSignalTask);
};
void StateSignal::notify(std::int64_t revision) {
  revision_.store(revision);
  if (!scheduled_.exchange(true) &&
      !CefPostDelayedTask(TID_UI, new StateSignalTask(shared_from_this()), 100))
    scheduled_.store(false);
}
fs::path executable_directory() {
  CefString path;
  CefGetPath(PK_DIR_EXE, path);
  return fs::u8path(path.ToString());
}
class ReplyTask : public CefTask {
public:
  ReplyTask(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, std::string text)
      : callback_(callback), text_(std::move(text)) {}
  void Execute() override { callback_->Success(text_); }

private:
  CefRefPtr<CefMessageRouterBrowserSide::Callback> callback_;
  std::string text_;
  IMPLEMENT_REFCOUNTING(ReplyTask);
};
class CloseTask : public CefTask {
public:
  explicit CloseTask(CefRefPtr<CefBrowser> browser, bool force = false)
      : browser_(browser), force_(force) {}
  void Execute() override {
    if (browser_->IsValid())
      browser_->GetHost()->CloseBrowser(force_);
  }

private:
  CefRefPtr<CefBrowser> browser_;
  bool force_;
  IMPLEMENT_REFCOUNTING(CloseTask);
};
class NavigateTask : public CefTask {
public:
  NavigateTask(CefRefPtr<CefBrowser> browser, std::string url)
      : browser_(browser), url_(std::move(url)) {}
  void Execute() override {
    if (browser_->IsValid())
      browser_->GetMainFrame()->LoadURL(url_);
  }

private:
  CefRefPtr<CefBrowser> browser_;
  std::string url_;
  IMPLEMENT_REFCOUNTING(NavigateTask);
};
class Assets : public CefSchemeHandlerFactory {
public:
  Assets(fs::path web, std::shared_ptr<LauncherService> service)
      : web_(fs::weakly_canonical(web)), service_(std::move(service)) {}
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                       const CefString &, CefRefPtr<CefRequest> request) override {
    CefURLParts parts;
    if (!CefParseURL(request->GetURL(), parts) || CefString(&parts.host) != "app")
      return nullptr;
    auto path = CefURIDecode(CefString(&parts.path), true,
                             static_cast<cef_uri_unescape_rule_t>(UU_NORMAL | UU_SPACES))
                    .ToString();
    if (path.empty() || path == "/")
      path = "/index.html";
    if (path.find('%') != std::string::npos || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos)
      return nullptr;
    const auto relative = fs::u8path(path.substr(1));
    for (const auto &component : relative)
      if (component == ".." || component == ".")
        return nullptr;
    std::error_code error;
    auto file = fs::weakly_canonical(web_ / relative, error);
    bool image_asset = false;
    for (const auto *category :
         {"captures", "avatars", "game-tile", "game-hero", "catalog-tile", "catalog-hero"}) {
      const auto prefix = std::string("/") + category + "/";
      if (path.rfind(prefix, 0) == 0) {
        try {
          const auto asset = service_->asset_path(category, path.substr(prefix.size()));
          if (!asset)
            return nullptr;
          file = *asset;
          image_asset = true;
        } catch (...) {
          return nullptr;
        }
      }
    }
    const auto within = file.lexically_relative(web_);
    if (error || (!image_asset && (within.empty() || *within.begin() == "..")) ||
        !fs::is_regular_file(file, error))
      return nullptr;
    auto stream = CefStreamReader::CreateForFile(xenon::launcher_next::path_string(file));
    if (!stream)
      return nullptr;
    auto ext = file.extension().string();
    for (auto &character : ext)
      if (character >= 'A' && character <= 'Z')
        character += 'a' - 'A';
    std::string mime = ext == ".html"                      ? "text/html"
                       : ext == ".js"                      ? "application/javascript"
                       : ext == ".css"                     ? "text/css"
                       : ext == ".svg"                     ? "image/svg+xml"
                       : ext == ".png"                     ? "image/png"
                       : (ext == ".jpg" || ext == ".jpeg") ? "image/jpeg"
                       : ext == ".webp"                    ? "image/webp"
                       : ext == ".bmp"                     ? "image/bmp"
                       : ext == ".json"                    ? "application/json"
                                                           : "application/octet-stream";
    CefResponse::HeaderMap headers;
    headers.insert({"Content-Security-Policy",
                    "default-src 'self'; script-src 'self'; style-src 'self' "
                    "'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'; "
                    "object-src 'none'; frame-src 'none'; base-uri 'none'"});
    headers.insert({"X-Content-Type-Options", "nosniff"});
    return new CefStreamResourceHandler(200, "OK", mime, headers, stream);
  }

private:
  fs::path web_;
  std::shared_ptr<LauncherService> service_;
  IMPLEMENT_REFCOUNTING(Assets);
};
class Worker {
public:
  Worker()
      : thread_([this] {
          for (;;) {
            std::function<void()> work;
            {
              std::unique_lock lock(mutex_);
              ready_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
              if (stopped_ && queue_.empty())
                return;
              work = std::move(queue_.front());
              queue_.pop();
            }
            work();
          }
        }) {}
  ~Worker() {
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    ready_.notify_one();
    thread_.join();
  }
  bool submit(std::function<void()> work) {
    {
      std::lock_guard lock(mutex_);
      if (stopped_ || queue_.size() >= 64)
        return false;
      queue_.push(std::move(work));
    }
    ready_.notify_one();
    return true;
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::queue<std::function<void()>> queue_;
  bool stopped_ = false;
  std::thread thread_;
};
class FileDialog : public CefRunFileDialogCallback {
public:
  FileDialog(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, std::string id)
      : callback_(callback), id_(std::move(id)) {}
  void OnFileDialogDismissed(const std::vector<CefString> &files) override {
    auto paths = Json::array();
    for (const auto &file : files)
      paths.push_back(file.ToString());
    callback_->Success(
        Json{{"version", 1}, {"id", id_}, {"ok", true}, {"data", {{"paths", paths}}}}.dump());
  }

private:
  CefRefPtr<CefMessageRouterBrowserSide::Callback> callback_;
  std::string id_;
  IMPLEMENT_REFCOUNTING(FileDialog);
};
class Client : public CefClient,
               public CefLifeSpanHandler,
               public CefRequestHandler,
               public CefDisplayHandler,
               public CefLoadHandler,
               public CefMessageRouterBrowserSide::Handler {
public:
  explicit Client(std::shared_ptr<LauncherService> service) : service_(std::move(service)) {
    router_ = CefMessageRouterBrowserSide::Create({});
    router_->AddHandler(this, false);
    const auto weak = std::weak_ptr<StateSignal>(state_signal_);
    service_->observe_changes([weak](std::int64_t revision) {
      if (const auto signal = weak.lock())
        signal->notify(revision);
    });
  }
  ~Client() override { router_->RemoveHandler(this); }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    state_signal_->browser = browser;
    browser->GetHost()->SetAccessibilityState(STATE_ENABLED);
#ifdef __APPLE__
    primary_browser = browser;
#endif
  }
  void show_failure(CefRefPtr<CefFrame> frame, const std::string &description) {
    fallback_ =
        "data:text/html;charset=utf-8," +
        CefURIEncode(
            "<!doctype html><html><body style='background:#07131d;color:#f4f8fa;font:16px "
            "system-ui;padding:48px'><h1>Xenon could not load the interface</h1><p>" +
                description +
                "</p><p>Your launcher data has been preserved. Check renderer.log in the launcher "
                "data directory.</p><p><a style='color:#35d7ea' "
                "href='xenon://app/'>Retry</a></p><p><a style='color:#35d7ea' "
                "href='xenon://recovery/restart-safe'>Restart in safe mode</a></p></body></html>",
            false)
            .ToString();
    frame->LoadURL(fallback_);
  }
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int code,
                                 const CefString &) override {
    router_->OnRenderProcessTerminated(browser);
    std::ofstream(service_->storage() / "renderer.log", std::ios::app)
        << "Renderer terminated: " << status << " (" << code << ")\n";
    const auto now = std::chrono::steady_clock::now();
    if (now - last_crash_ > std::chrono::seconds(60))
      crashes_ = 0;
    last_crash_ = now;
    if (++crashes_ <= 1)
      CefPostDelayedTask(TID_UI, new NavigateTask(browser, "xenon://app/"), 250);
    else
      show_failure(browser->GetMainFrame(),
                   "The interface stopped repeatedly. Try safe mode or restart the launcher.");
  }
  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code,
                   const CefString &text, const CefString &) override {
    if (!frame->IsMain() || code == ERR_ABORTED)
      return;
    std::ofstream(service_->storage() / "renderer.log", std::ios::app)
        << "Main page load failed: " << code << ": " << text.ToString() << '\n';
    show_failure(frame, "The packaged web assets could not be opened.");
  }
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString &message,
                        const CefString &, int) override {
    if (level >= LOGSEVERITY_WARNING)
      std::ofstream(service_->storage() / "renderer.log", std::ios::app)
          << message.ToString() << '\n';
    return false;
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
#ifdef __APPLE__
    primary_browser = nullptr;
#endif
    state_signal_->browser = nullptr;
    service_->observe_changes({});
    router_->OnBeforeClose(browser);
    CefQuitMessageLoop();
  }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool, bool) override {
    if (!fallback_.empty() && frame->IsMain() && frame->GetURL().ToString() == fallback_ &&
        request->GetURL() == "xenon://recovery/restart-safe") {
      restart_requested = true;
      restart_safe = true;
      CefPostTask(TID_UI, new CloseTask(browser));
      return true;
    }
    const bool allowed =
        request->GetURL() == "xenon://app/" || request->GetURL().ToString() == fallback_;
    if (!allowed)
      return true;
    if (!fallback_.empty() && frame->GetURL().ToString() == fallback_ &&
        request->GetURL() == "xenon://app/")
      crashes_ = 0;
    router_->OnBeforeBrowse(browser, frame);
    return false;
  }
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString &,
                     const CefString &, CefLifeSpanHandler::WindowOpenDisposition, bool,
                     const CefPopupFeatures &, CefWindowInfo &, CefRefPtr<CefClient> &,
                     CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *) override {
    return true;
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    return router_->OnProcessMessageReceived(browser, frame, source, message);
  }
  bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t,
               const CefString &raw, bool, CefRefPtr<Callback> callback) override {
    if (!trusted(frame) || raw.length() > 1024 * 1024) {
      callback->Failure(403, "Untrusted launcher request");
      return true;
    }
    try {
      const auto request = Json::parse(raw.ToString());
      if (request.at("version") != 1 || !request.at("id").is_string() ||
          !request.at("payload").is_object())
        throw std::runtime_error("Invalid envelope");
      if (request.at("command") == "launcher.action" &&
          request.at("payload").value("action", "") == "application.restart") {
        restart_requested = true;
        restart_safe = request.at("payload").value("data", Json::object()).value("safeMode", false);
        callback->Success(Json{
            {"version", 1}, {"id", request.at("id")}, {"ok", true}, {"data", service_->snapshot()}}
                              .dump());
        CefPostDelayedTask(TID_UI, new CloseTask(browser), 100);
      } else if (request.at("command") == "launcher.action" &&
                 request.at("payload").value("action", "") == "updates.install") {
        const auto service = service_;
        const auto parent = xenon::launcher_next::current_process_identity();
        if (!worker_.submit([service, request, callback, browser, parent] {
              try {
                const auto plan = service->prepare_update_installation(parent);
                if (!xenon::launcher_next::launch_detached(
                        plan.parent_path() / "updater.exe",
                        {xenon::launcher_next::path_string(plan)})) {
                  service->update_handoff_failed("Could not start the launcher update helper");
                  throw std::runtime_error("Could not start the launcher update helper");
                }
                const auto reply = Json{{"version", 1},
                                        {"id", request.at("id")},
                                        {"ok", true},
                                        {"data", service->snapshot()}};
                CefPostTask(TID_UI, new ReplyTask(callback, reply.dump()));
                CefPostDelayedTask(TID_UI, new CloseTask(browser, true), 250);
              } catch (const std::exception &error) {
                const auto reply = Json{{"version", 1},
                                        {"id", request.at("id")},
                                        {"ok", false},
                                        {"error", {{"message", error.what()}}}};
                CefPostTask(TID_UI, new ReplyTask(callback, reply.dump()));
              }
            }))
          callback->Failure(429, "Launcher command queue is full");
      } else if (request.at("command") == "native.dialog") {
        const auto mode = request.at("payload").value("mode", "open");
        const auto dialog_mode = mode == "folder"     ? FILE_DIALOG_OPEN_FOLDER
                                 : mode == "save"     ? FILE_DIALOG_SAVE
                                 : mode == "multiple" ? FILE_DIALOG_OPEN_MULTIPLE
                                                      : FILE_DIALOG_OPEN;
        browser->GetHost()->RunFileDialog(dialog_mode, "Xenon Launcher", "", {},
                                          new FileDialog(callback, request.at("id")));
      } else {
        const auto service = service_;
        if (!worker_.submit([service, request, callback] {
              auto result = service->request(request);
              if (result.value("ok", false) && !update_receipt.empty() &&
                  request.at("command") == "launcher.action" &&
                  request.at("payload").value("action", "") == "application.ready") {
                try {
                  xenon::launcher_next::acknowledge_launcher_update(update_receipt, update_token);
                  update_receipt.clear();
                } catch (const std::exception &error) {
                  result = {{"version", 1},
                            {"id", request.at("id")},
                            {"ok", false},
                            {"error", {{"message", error.what()}}}};
                }
              }
              CefPostTask(TID_UI, new ReplyTask(callback, result.dump()));
            }))
          callback->Failure(429, "Launcher command queue is full");
      }
    } catch (const std::exception &error) {
      callback->Failure(400, error.what());
    }
    return true;
  }

private:
  std::shared_ptr<LauncherService> service_;
  CefRefPtr<CefMessageRouterBrowserSide> router_;
  Worker worker_;
  std::shared_ptr<StateSignal> state_signal_ = std::make_shared<StateSignal>();
  std::string fallback_;
  std::chrono::steady_clock::time_point last_crash_{};
  unsigned crashes_ = 0;
  IMPLEMENT_REFCOUNTING(Client);
};
class Window : public CefWindowDelegate {
public:
  Window(CefRefPtr<CefBrowserView> view, std::shared_ptr<LauncherService> service)
      : view_(view), service_(std::move(service)) {
    if (!service_)
      return;
    try {
      if (fs::exists(service_->storage() / "window.json"))
        saved_ = xenon::launcher_next::read_document(service_->storage() / "window.json", 4096);
      if (!saved_.is_object() ||
          (saved_.contains("maximized") && !saved_["maximized"].is_boolean()))
        saved_ = Json::object();
    } catch (...) {
      saved_ = Json::object();
    }
  }
  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    window->AddChildView(view_);
    window->SetTitle("Xenon Launcher");
    window->Show();
  }
  void OnWindowDestroyed(CefRefPtr<CefWindow>) override {
    if (!service_) {
      view_ = nullptr;
      return;
    }
    try {
      if (service_->presentation_settings().value("system/rememberWindowGeometry", true) &&
          normal_.width > 0)
        xenon::launcher_next::write_document(service_->storage() / "window.json",
                                             {{"x", normal_.x},
                                              {"y", normal_.y},
                                              {"width", normal_.width},
                                              {"height", normal_.height},
                                              {"maximized", maximized_}});
    } catch (const std::exception &error) {
      std::ofstream(service_->storage() / "renderer.log", std::ios::app)
          << "Window state could not be saved: " << error.what() << '\n';
    }
    view_ = nullptr;
  }
  void OnWindowBoundsChanged(CefRefPtr<CefWindow> window, const CefRect &bounds) override {
    if (!window->IsMaximized() && !window->IsMinimized() && !window->IsFullscreen())
      normal_ = bounds;
  }
  CefRect GetInitialBounds(CefRefPtr<CefWindow>) override {
    const auto settings = service_ ? service_->presentation_settings() : Json::object();
    CefRect bounds(0, 0, 1280, 800);
    if (settings.value("system/rememberWindowGeometry", true) && saved_.contains("width")) {
      try {
        bounds = {saved_.at("x").get<int>(), saved_.at("y").get<int>(),
                  saved_.at("width").get<int>(), saved_.at("height").get<int>()};
      } catch (...) {
        bounds = {0, 0, 1280, 800};
      }
    }
    const auto display = CefDisplay::GetDisplayMatchingBounds(bounds, false);
    if (display) {
      const auto work = display->GetWorkArea();
      bounds.width =
          std::clamp(bounds.width, std::min(640, std::max(1, work.width)), std::max(1, work.width));
      bounds.height = std::clamp(bounds.height, std::min(480, std::max(1, work.height)),
                                 std::max(1, work.height));
      bounds.x = std::clamp(bounds.x, work.x, work.x + std::max(0, work.width - bounds.width));
      bounds.y = std::clamp(bounds.y, work.y, work.y + std::max(0, work.height - bounds.height));
    }
    normal_ = bounds;
    return bounds;
  }
  cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow>) override {
    const auto settings = service_ ? service_->presentation_settings() : Json::object();
    if (settings.value("system/startMinimized", false))
      return CEF_SHOW_STATE_MINIMIZED;
    if (settings.value("system/rememberWindowGeometry", true) &&
        settings.value("system/restoreMaximized", true) && saved_.value("maximized", false))
      return CEF_SHOW_STATE_MAXIMIZED;
    return CEF_SHOW_STATE_NORMAL;
  }
  bool CanClose(CefRefPtr<CefWindow> window) override {
    maximized_ = window->IsMaximized();
    auto browser = view_->GetBrowser();
    return !browser || browser->GetHost()->TryCloseBrowser();
  }
  CefSize GetPreferredSize(CefRefPtr<CefView>) override { return {1280, 800}; }
  CefSize GetMinimumSize(CefRefPtr<CefView>) override { return {640, 480}; }

private:
  CefRefPtr<CefBrowserView> view_;
  std::shared_ptr<LauncherService> service_;
  Json saved_ = Json::object();
  CefRect normal_;
  bool maximized_ = false;
  IMPLEMENT_REFCOUNTING(Window);
};
std::string html_text(const std::string &value) {
  std::string result;
  for (const auto c : value) {
    switch (c) {
    case '&':
      result += "&amp;";
      break;
    case '<':
      result += "&lt;";
      break;
    case '>':
      result += "&gt;";
      break;
    case '"':
      result += "&quot;";
      break;
    default:
      result += c;
    }
  }
  return result;
}
class StartupFailure : public CefClient, public CefLifeSpanHandler, public CefRequestHandler {
public:
  explicit StartupFailure(std::string url) : url_(std::move(url)) {}
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  void OnBeforeClose(CefRefPtr<CefBrowser>) override { CefQuitMessageLoop(); }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool, bool) override {
    if (frame->IsMain() && frame->GetURL().ToString() == url_ &&
        (request->GetURL() == "xenon://startup/retry" ||
         request->GetURL() == "xenon://startup/retry-safe")) {
      restart_requested = true;
      restart_safe = request->GetURL() == "xenon://startup/retry-safe";
      CefPostTask(TID_UI, new CloseTask(browser));
      return true;
    }
    return request->GetURL().ToString() != url_;
  }

private:
  std::string url_;
  IMPLEMENT_REFCOUNTING(StartupFailure);
};
void show_startup_failure(const std::string &message, const fs::path &storage) {
  const auto document =
      std::string("<!doctype html><html><head><meta charset='utf-8'>") +
      "<meta http-equiv='Content-Security-Policy' content=\"default-src 'none'; style-src "
      "'unsafe-inline'\">" +
      "<title>Xenon Launcher startup problem</title></head>" +
      "<body style='background:#0d1114;color:#eef4f8;font:17px "
      "system-ui;padding:36px;line-height:1.6'>" +
      "<h1>The launcher could not start</h1><p>Saved data has been kept. Close another launcher "
      "using this data folder, or review the startup log and any saved-data backup before trying "
      "again.</p>" +
      "<pre style='white-space:pre-wrap;overflow-wrap:anywhere'>" + html_text(message) + "</pre>" +
      "<p>Data folder: <code>" + html_text(xenon::launcher_next::path_string(storage)) +
      "</code></p>" +
      "<p><a style='color:#19d2c0' href='xenon://startup/retry'>Retry startup</a> &nbsp; " +
      "<a style='color:#19d2c0' href='xenon://startup/retry-safe'>Restart in safe mode</a></p>" +
      "<p>Safe mode does not reset or replace saved data.</p></body></html>";
  const auto url = "data:text/html;charset=utf-8," + CefURIEncode(document, false).ToString();
  CefBrowserSettings settings;
  settings.background_color = CefColorSetARGB(255, 13, 17, 20);
  auto view = CefBrowserView::CreateBrowserView(new StartupFailure(url), url, settings, nullptr,
                                                nullptr, nullptr);
  CefWindow::CreateTopLevelWindow(new Window(view, nullptr));
}
class App : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
    registrar->AddCustomScheme("xenon", CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                                            CEF_SCHEME_OPTION_CORS_ENABLED |
                                            CEF_SCHEME_OPTION_FETCH_ENABLED);
  }
  void OnWebKitInitialized() override { renderer_ = CefMessageRouterRendererSide::Create({}); }
  void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    renderer_->OnContextCreated(browser, frame, context);
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    renderer_->OnContextReleased(browser, frame, context);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                CefProcessId source,
                                CefRefPtr<CefProcessMessage> message) override {
    return renderer_ && renderer_->OnProcessMessageReceived(browser, frame, source, message);
  }
  void OnContextInitialized() override {
    fs::path startup_storage;
    try {
      auto directory = executable_directory();
#ifdef __APPLE__
      directory = directory.parent_path() / "Resources";
#endif
      auto storage = xenon::launcher_next::default_storage();
      auto args = CefCommandLine::GetGlobalCommandLine();
      if (args->HasSwitch("data-dir"))
        storage = fs::u8path(args->GetSwitchValue("data-dir").ToString());
      startup_storage = storage;
      fs::create_directories(storage);
      fs::copy_file(directory / "settings-schema.json", storage / "settings-schema.json",
                    fs::copy_options::overwrite_existing);
      fs::copy_file(directory / "module-catalog-fallback.json",
                    storage / "module-catalog-fallback.json", fs::copy_options::overwrite_existing);
      auto service = std::make_shared<LauncherService>(
          storage, args->HasSwitch("safe-mode"),
          args->HasSwitch("fixture-host")
              ? fs::u8path(args->GetSwitchValue("fixture-host").ToString())
              : fs::path{},
          std::function<void(const std::string &, const std::string &)>{},
          args->HasSwitch("offline") ? nullptr
          : args->HasSwitch("fixture-network")
              ? xenon::launcher_next::fixture_http(
                    fs::u8path(args->GetSwitchValue("fixture-network").ToString()))
              : xenon::launcher_next::platform_http());
      service->configure_update_installation(directory);
      CefRegisterSchemeHandlerFactory("xenon", "app", new Assets(directory / "web", service));
      auto client = CefRefPtr<Client>(new Client(service));
      CefBrowserSettings browser_settings;
      browser_settings.background_color = CefColorSetARGB(255, 7, 19, 29);
      auto view = CefBrowserView::CreateBrowserView(client, "xenon://app/", browser_settings,
                                                    nullptr, nullptr, nullptr);
      CefWindow::CreateTopLevelWindow(new Window(view, service));
    } catch (const std::exception &error) {
      if (!startup_storage.empty())
        std::ofstream(startup_storage / "startup.log", std::ios::app)
            << "Startup failed: " << error.what() << '\n';
      else
        std::ofstream(executable_directory() / "startup-error.txt") << error.what();
      show_startup_failure(error.what(), startup_storage);
    }
  }

private:
  CefRefPtr<CefMessageRouterRendererSide> renderer_;
  IMPLEMENT_REFCOUNTING(App);
};
// Linux zygotes rotate the stack guard inside CefExecuteProcess. Its callers
// must follow CEF's entry-point annotation, while other functions stay protected.
#if defined(OS_LINUX)
NO_STACK_PROTECTOR
#endif
int run(CefMainArgs args, void *sandbox) {
  CefRefPtr<App> app(new App);
  const auto result = CefExecuteProcess(args, app, sandbox);
  if (result >= 0)
    return result;
  CefSettings settings;
  auto args_line = CefCommandLine::CreateCommandLine();
#ifdef _WIN32
  args_line->InitFromString(GetCommandLineW());
#else
  args_line->InitFromArgv(args.argc, args.argv);
#endif
  auto storage = xenon::launcher_next::default_storage();
  if (args_line->HasSwitch("data-dir"))
    storage = fs::u8path(args_line->GetSwitchValue("data-dir").ToString());
  const auto remote_port = args_line->GetSwitchValue("remote-debugging-port").ToString();
  if (args_line->HasSwitch("update-receipt") && args_line->HasSwitch("update-token")) {
    update_receipt = fs::u8path(args_line->GetSwitchValue("update-receipt").ToString());
    update_token = args_line->GetSwitchValue("update-token").ToString();
  }
  const bool offline = args_line->HasSwitch("offline");
  const auto network_fixture = args_line->GetSwitchValue("fixture-network").ToString();
  const auto runtime_fixture = args_line->GetSwitchValue("fixture-host").ToString();
  const auto cache = storage / "cef-cache";
  CefString(&settings.root_cache_path) = xenon::launcher_next::path_string(cache);
  settings.log_severity = LOGSEVERITY_WARNING;
  CefString(&settings.log_file) = xenon::launcher_next::path_string(storage / "cef.log");
  if (!CefInitialize(args, settings, app, sandbox)) {
#ifdef _WIN32
    const auto message =
        fs::u8path("The launcher browser runtime could not initialize. Check cef.log in " +
                   xenon::launcher_next::path_string(storage) +
                   ". Exit code: " + std::to_string(CefGetExitCode()))
            .wstring();
    MessageBoxW(nullptr, message.c_str(), L"Xenon Launcher startup problem", MB_OK | MB_ICONERROR);
#endif
    return CefGetExitCode();
  }
  CefRunMessageLoop();
  CefString executable_path;
  CefGetPath(PK_FILE_EXE, executable_path);
  args_line = nullptr;
  app = nullptr;
  CefClearSchemeHandlerFactories();
  CefShutdown();
  if (restart_requested) {
    std::vector<std::string> arguments{"--data-dir=" + xenon::launcher_next::path_string(storage)};
    if (restart_safe)
      arguments.push_back("--safe-mode");
    if (!remote_port.empty())
      arguments.push_back("--remote-debugging-port=" + remote_port);
    if (offline)
      arguments.push_back("--offline");
    if (!network_fixture.empty())
      arguments.push_back("--fixture-network=" + network_fixture);
    if (!runtime_fixture.empty())
      arguments.push_back("--fixture-host=" + runtime_fixture);
    if (!xenon::launcher_next::launch_detached(fs::u8path(executable_path.ToString()), arguments))
      return 1;
  }
  return 0;
}
} // namespace
#ifdef _WIN32
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance, LPWSTR, int, void *sandbox,
                                    cef_version_info_t *) {
  try {
    return run(CefMainArgs(instance), sandbox);
  } catch (const std::exception &error) {
    const auto message = fs::u8path(error.what()).wstring();
    MessageBoxW(nullptr, message.c_str(), L"Xenon Launcher startup problem", MB_OK | MB_ICONERROR);
    return 1;
  }
}
#elif defined(__APPLE__)
int xenon_launcher_main(int argc, char **argv) { return run(CefMainArgs(argc, argv), nullptr); }
void xenon_launcher_quit() {
  if (primary_browser) primary_browser->GetHost()->CloseBrowser(false);
}
void xenon_launcher_reopen() {
  if (!primary_browser) return;
  const auto view = CefBrowserView::GetForBrowser(primary_browser);
  if (view && view->GetWindow()) {
    view->GetWindow()->Show();
    view->GetWindow()->Restore();
    view->GetWindow()->Activate();
  }
}
#else
#if defined(OS_LINUX)
NO_STACK_PROTECTOR
#endif
int main(int argc, char **argv) { return run(CefMainArgs(argc, argv), nullptr); }
#endif
