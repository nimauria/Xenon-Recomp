#include "launcher_update.hpp"
#include "archive.hpp"
#include "file_transfer.hpp"
#include "releases.hpp"
#include "platform/storage_lock.hpp"
#include "picosha2/picosha2.h"
#include <array>
#include <chrono>
#include <fstream>
#include <thread>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
constexpr auto product = "xenon-launcher-next";
std::string text(const fs::path &path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
bool within(const fs::path &path, const fs::path &root) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}
std::string hash_file(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Cannot read update file");
  std::array<char, 256 * 1024> buffer{};
  picosha2::hash256_one_by_one hash;
  while (stream) {
    stream.read(buffer.data(), buffer.size());
    hash.process(buffer.data(), buffer.data() + stream.gcount());
  }
  if (!stream.eof())
    throw std::runtime_error("Cannot verify update file");
  hash.finish();
  return picosha2::get_hash_hex_string(hash);
}
void plain_root(const fs::path &path) {
  if (!path.is_absolute() || path == path.root_path() || path != path.lexically_normal() ||
      fs::weakly_canonical(path) != path || redirected_path(path))
    throw std::runtime_error(
        "Update location was redirected or is not an absolute installation folder");
}
void validate_package(const fs::path &root, const std::string &version = "") {
  plain_root(root);
  check_transfer_tree(root);
  const auto manifest = read_document(root / "launcher-package.json", 65536);
  if (manifest.value("product", "") != product || manifest.value("format", 0) != 1 ||
      manifest.value("stateSchema", 0) != 1 || manifest.value("platform", "") != host_key() ||
      !Version::parse(manifest.value("version", "")) ||
      (!version.empty() && manifest.value("version", "") != version))
    throw std::runtime_error(
        "Package product, platform, format or version does not match this update");
#ifdef _WIN32
  for (const auto *file :
       {"xenon_launcher_next.exe", "xenon_launcher_next.dll", "xenon_launcher_updater.exe",
        "libcef.dll", "icudtl.dat", "resources.pak", "v8_context_snapshot.bin",
        "chrome_100_percent.pak", "locales/en-US.pak", "web/index.html", "settings-schema.json",
        "module-catalog-fallback.json"})
    if (!fs::is_regular_file(root / file) || fs::file_size(root / file) == 0)
      throw std::runtime_error(std::string("Incomplete CEF launcher package: ") + file);
#else
  throw std::runtime_error(
      "Automatic launcher replacement is currently available for Windows packages only");
#endif
}
Json files(const fs::path &root) {
  Json result = Json::array();
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file())
      result.push_back({{"path", text(entry.path().lexically_relative(root))},
                        {"sha256", hash_file(entry.path())},
                        {"size", entry.file_size()}});
  std::sort(result.begin(), result.end(),
            [](const auto &a, const auto &b) { return a.at("path") < b.at("path"); });
  return result;
}
struct Plan {
  Json data;
  fs::path work, installation, storage, payload, backup, receipt;
  std::string id, version;
  explicit Plan(const fs::path &file) {
    if (file.filename() != "plan.json")
      throw std::runtime_error("Invalid update plan filename");
    work = fs::absolute(file.parent_path()).lexically_normal();
    plain_root(work);
    data = read_document(work / "plan.json", 16 * 1024 * 1024);
    id = data.at("id").get<std::string>();
    version = data.at("version").get<std::string>();
    if (id.empty() || id.size() > 100 ||
        id.find_first_not_of("0123456789abcdef-") != std::string::npos ||
        work.filename() != fs::u8path(id) || data.value("product", "") != product ||
        data.value("format", 0) != 1)
      throw std::runtime_error("Invalid update plan identity");
    installation = fs::u8path(data.at("installation").get<std::string>());
    storage = fs::u8path(data.at("storage").get<std::string>());
    if (!installation.is_absolute() || installation != installation.lexically_normal() ||
        installation == installation.root_path() ||
        work.parent_path() != installation.parent_path() / ".xenon-updates")
      throw std::runtime_error("Update plan does not belong to this installation");
    if (!storage.is_absolute() || storage != storage.lexically_normal() ||
        within(storage, installation) || within(storage, work))
      throw std::runtime_error("User data must be outside the installation and update workspace");
    if (fs::weakly_canonical(work.parent_path()) != work.parent_path() ||
        fs::weakly_canonical(installation.parent_path()) != installation.parent_path())
      throw std::runtime_error("Update parent directory changed");
    if (read_document(work / "owner.json", 65536) !=
        Json{{"product", product}, {"id", id}, {"installation", text(installation)}})
      throw std::runtime_error("Update workspace ownership changed");
    payload = work / "payload";
    backup = work / "backup";
    receipt = work / "ready.json";
  }
};
void rename_directory(const fs::path &source, const fs::path &destination) {
  // Chromium child handles can take a moment to close after the browser exits.
  for (unsigned i = 0;; ++i) {
    if (fs::weakly_canonical(source) != source || fs::exists(destination))
      throw std::runtime_error("Update rename boundary changed; folders were left untouched");
    std::error_code error;
    fs::rename(source, destination, error);
    if (!error)
      return;
    if (i >= 50)
      throw std::runtime_error("Could not replace launcher folder: " + error.message());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
} // namespace
bool update_installation_supported(const fs::path &installation) {
  try {
    validate_package(installation);
    return true;
  } catch (...) {
    return false;
  }
}
Json launcher_update_status(const fs::path &file) {
  const Plan plan(file);
  return read_document(plan.work / "status.json", 65536);
}
fs::path prepare_launcher_update(const fs::path &package, const std::string &digest,
                                 const std::string &version, const fs::path &installation,
                                 const fs::path &storage, ProcessIdentity parent) {
  validate_package(installation);
  plain_root(storage);
  if (!parent.pid || !parent.created || !Version::parse(version))
    throw std::runtime_error("Invalid update version or parent identity");
  if (within(storage, installation) || within(installation, storage))
    throw std::runtime_error("Move user data outside the installation before updating");
  if (sha256_digest(digest).empty() || redirected_path(package) || hash_file(package) != digest)
    throw std::runtime_error("Staged launcher package failed SHA-256 verification");
  const auto id = make_id();
  const auto parent_folder = installation.parent_path() / ".xenon-updates";
  fs::create_directories(parent_folder);
  plain_root(parent_folder);
  const auto work = parent_folder / id;
  if (!fs::create_directory(work))
    throw std::runtime_error("Cannot create a fresh update workspace");
  write_document(work / "owner.json",
                 {{"product", product}, {"id", id}, {"installation", text(installation)}});
  // Failed preparation is kept for diagnosis; no current installation files have changed.
  extract_package(package, work / "payload");
  validate_package(work / "payload", version);
  if (hash_file(package) != digest)
    throw std::runtime_error("Update package changed during extraction");
  fs::copy_file(installation / "xenon_launcher_updater.exe", work / "updater.exe");
  write_document(work / "plan.json",
                 {{"format", 1},
                  {"product", product},
                  {"id", id},
                  {"version", version},
                  {"installation", text(installation)},
                  {"storage", text(storage)},
                  {"files", files(work / "payload")},
                  {"parent", {{"pid", parent.pid}, {"created", parent.created}}}});
  write_document(work / "status.json", {{"state", "prepared"}});
  return work / "plan.json";
}
void apply_launcher_update(const fs::path &plan_file, const UpdateHooks &hooks) {
  const Plan plan(plan_file);
  StorageLock lock(plan.work);
  if (fs::exists(plan.backup) || fs::exists(plan.work / "failed"))
    throw std::runtime_error(
        "This update was already attempted; its recovery folders were retained");
  const ProcessIdentity parent{plan.data.at("parent").at("pid"),
                               plan.data.at("parent").at("created")};
  ProcessIdentity replacement{};
  bool moved_old = false, moved_new = false;
  const auto phase = [&](const std::string &value) {
    write_document(plan.work / "status.json",
                   {{"state", value},
                    {"replacement", {{"pid", replacement.pid}, {"created", replacement.created}}}});
    std::ofstream(plan.storage / "update-install.log", std::ios::app)
        << "Launcher update: " << value << '\n';
    if (hooks.phase)
      hooks.phase(value);
  };
  const auto wait_until = [&](const std::function<bool()> &done, unsigned timeout) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    do {
      if (done())
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  };
  try {
    phase("waiting-for-launcher");
    if (!wait_until([&] { return !hooks.running(parent); }, hooks.parent_wait_ms))
      throw std::runtime_error("The current launcher did not close; its files were preserved");
    validate_package(plan.installation);
    validate_package(plan.payload, plan.version);
    if (files(plan.payload) != plan.data.at("files"))
      throw std::runtime_error("Prepared update files changed");
    phase("replacing");
    rename_directory(plan.installation, plan.backup);
    moved_old = true;
    phase("previous-version-saved");
    rename_directory(plan.payload, plan.installation);
    moved_new = true;
    phase("starting-replacement");
    replacement =
        hooks.launch(plan.installation / "xenon_launcher_next.exe",
                     {"--data-dir=" + text(plan.storage), "--update-receipt=" + text(plan.receipt),
                      "--update-token=" + plan.id});
    if (!replacement.pid || !replacement.created)
      throw std::runtime_error("Cannot identify the replacement launcher");
    phase("waiting-for-interface");
    const bool ready = wait_until(
        [&] {
          if (!hooks.running(replacement))
            throw std::runtime_error("Replacement launcher exited before its interface was ready");
          if (!fs::is_regular_file(plan.receipt))
            return false;
          if (redirected_path(plan.receipt))
            throw std::runtime_error("Update receipt was redirected");
          const auto receipt = read_document(plan.receipt, 65536);
          return receipt.value("token", "") == plan.id &&
                 receipt.value("version", "") == plan.version &&
                 receipt.value("phase", "") == "interface-ready" &&
                 receipt.value("pid", std::uint64_t{}) == replacement.pid &&
                 receipt.value("created", std::uint64_t{}) == replacement.created;
        },
        hooks.ready_wait_ms);
    if (!ready)
      throw std::runtime_error("Replacement interface did not become ready in time");
    phase("completed");
  } catch (const std::exception &error) {
    const auto message = std::string(error.what());
    // Never restore over a running replacement. PID reuse is checked by the platform adapter.
    if (replacement.pid && hooks.running(replacement) && !hooks.stop(replacement)) {
      write_document(plan.work / "status.json",
                     {{"state", "recovery-required"}, {"error", message}});
      throw std::runtime_error(
          "Could not stop the replacement; backup retained for manual recovery");
    }
    try {
      if (moved_new)
        rename_directory(plan.installation, plan.work / "failed");
      if (moved_old)
        rename_directory(plan.backup, plan.installation);
      if (moved_old)
        hooks.launch(plan.installation / "xenon_launcher_next.exe",
                     {"--data-dir=" + text(plan.storage)});
      write_document(plan.work / "status.json",
                     {{"state", moved_old ? "rolled-back" : "failed"}, {"error", message}});
      std::ofstream(plan.storage / "update-install.log", std::ios::app)
          << "Launcher update failed: " << message << '\n';
    } catch (const std::exception &recovery) {
      write_document(
          plan.work / "status.json",
          {{"state", "recovery-required"}, {"error", message}, {"recoveryError", recovery.what()}});
      throw;
    }
    throw std::runtime_error(message);
  }
}
void acknowledge_launcher_update(const fs::path &receipt, const std::string &token) {
  if (receipt.filename() != "ready.json")
    throw std::runtime_error("Invalid update receipt location");
  const Plan plan(receipt.parent_path() / "plan.json");
  if (plan.id != token || plan.version != launcher_version || fs::exists(receipt))
    throw std::runtime_error("Update receipt token or version does not match this application");
  const auto identity = current_process_identity();
  write_document(receipt, {{"token", token},
                           {"version", launcher_version},
                           {"phase", "interface-ready"},
                           {"pid", identity.pid},
                           {"created", identity.created}});
}
} // namespace xenon::launcher_next
