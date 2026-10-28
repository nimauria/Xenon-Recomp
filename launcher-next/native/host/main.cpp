#include "application/launcher_service.hpp"
#include "include/cef_app.h"
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
#ifdef _WIN32
#include "include/cef_sandbox_win.h"
#endif

namespace {
using xenon::launcher_next::Json;
using xenon::launcher_next::LauncherService;
namespace fs = std::filesystem;
bool restart_requested = false;
bool restart_safe = false;
bool trusted(CefRefPtr<CefFrame> frame) {
  return frame && frame->IsMain() && frame->GetURL().ToString() == "xenon://app/";
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
  explicit CloseTask(CefRefPtr<CefBrowser> browser) : browser_(browser) {}
  void Execute() override {
    if (browser_->IsValid())
      browser_->GetHost()->CloseBrowser(false);
  }

private:
  CefRefPtr<CefBrowser> browser_;
  IMPLEMENT_REFCOUNTING(CloseTask);
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
    for (const auto *category : {"captures", "avatars", "game-tile", "game-hero"}) {
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
  }
  ~Client() override { router_->RemoveHandler(this); }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code,
                   const CefString &text, const CefString &) override {
    if (!frame->IsMain() || code == ERR_ABORTED)
      return;
    std::ofstream(service_->storage() / "renderer.log", std::ios::app)
        << "Main page load failed: " << code << ": " << text.ToString() << '\n';
    fallback_ = "data:text/html;charset=utf-8," +
                CefURIEncode("<!doctype html><html><body "
                             "style='background:#07131d;color:#f4f8fa;font:16px "
                             "system-ui;padding:48px'><h1>Xenon could not load the "
                             "interface</h1><p>The packaged web assets could not be opened. "
                             "Your launcher data has been preserved.</p><p>Check renderer.log "
                             "in the launcher data directory.</p><a style='color:#35d7ea' "
                             "href='xenon://app/'>Retry</a></body></html>",
                             false)
                    .ToString();
    frame->LoadURL(fallback_);
  }
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString &message,
                        const CefString &, int) override {
    if (level >= LOGSEVERITY_WARNING)
      std::ofstream(service_->storage() / "renderer.log", std::ios::app)
          << message.ToString() << '\n';
    return false;
  }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    router_->OnBeforeClose(browser);
    CefQuitMessageLoop();
  }
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool, bool) override {
    router_->OnBeforeBrowse(browser, frame);
    return request->GetURL() != "xenon://app/" && request->GetURL().ToString() != fallback_;
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
              const auto result = service->request(request).dump();
              CefPostTask(TID_UI, new ReplyTask(callback, result));
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
  std::string fallback_;
  IMPLEMENT_REFCOUNTING(Client);
};
class Window : public CefWindowDelegate {
public:
  Window(CefRefPtr<CefBrowserView> view, std::shared_ptr<LauncherService> service)
      : view_(view), service_(std::move(service)) {
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
    const auto settings = service_->presentation_settings();
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
    const auto settings = service_->presentation_settings();
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
    try {
      auto directory = executable_directory();
      auto storage = xenon::launcher_next::default_storage();
      auto args = CefCommandLine::GetGlobalCommandLine();
      if (args->HasSwitch("data-dir"))
        storage = fs::u8path(args->GetSwitchValue("data-dir").ToString());
      fs::create_directories(storage);
      fs::copy_file(directory / "settings-schema.json", storage / "settings-schema.json",
                    fs::copy_options::overwrite_existing);
      auto service = std::make_shared<LauncherService>(
          storage, args->HasSwitch("safe-mode"),
          args->HasSwitch("fixture-host")
              ? fs::u8path(args->GetSwitchValue("fixture-host").ToString())
              : fs::path{});
      CefRegisterSchemeHandlerFactory("xenon", "app", new Assets(directory / "web", service));
      auto client = CefRefPtr<Client>(new Client(service));
      CefBrowserSettings browser_settings;
      browser_settings.background_color = CefColorSetARGB(255, 7, 19, 29);
      auto view = CefBrowserView::CreateBrowserView(client, "xenon://app/", browser_settings,
                                                    nullptr, nullptr, nullptr);
      CefWindow::CreateTopLevelWindow(new Window(view, service));
    } catch (const std::exception &error) {
      std::ofstream(executable_directory() / "startup-error.txt") << error.what();
      CefQuitMessageLoop();
    }
  }

private:
  CefRefPtr<CefMessageRouterRendererSide> renderer_;
  IMPLEMENT_REFCOUNTING(App);
};
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
  const auto cache = storage / "cef-cache";
  CefString(&settings.root_cache_path) = xenon::launcher_next::path_string(cache);
  settings.log_severity = LOGSEVERITY_WARNING;
  if (!CefInitialize(args, settings, app, sandbox))
    return CefGetExitCode();
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
    if (!xenon::launcher_next::launch_detached(fs::u8path(executable_path.ToString()), arguments))
      return 1;
  }
  return 0;
}
} // namespace
#ifdef _WIN32
CEF_BOOTSTRAP_EXPORT int RunWinMain(HINSTANCE instance, LPWSTR, int, void *sandbox,
                                    cef_version_info_t *) {
  return run(CefMainArgs(instance), sandbox);
}
#else
int main(int argc, char **argv) { return run(CefMainArgs(argc, argv), nullptr); }
#endif
