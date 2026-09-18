#include "update_feature.hpp"

#include "../settings/settings_feature.hpp"
#include "providers/github_release_provider.hpp"
#include "providers/update_provider.hpp"
#include "version/semantic_version.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QTimer>

#ifndef XENON_LAUNCHER_VERSION
#define XENON_LAUNCHER_VERSION "0.0.0-dev"
#endif

#ifndef XENON_LAUNCHER_UPDATE_REPOSITORY
#define XENON_LAUNCHER_UPDATE_REPOSITORY "nimauria/Xenon-Recomp"
#endif

namespace xenon::launcher::frontend_backend {
namespace {
constexpr auto kLastCheckedKey = "updates/internalLastCheckedAt";
}

UpdateFeature::UpdateFeature(PackageService& packages, SettingsFeature& settings,
                             QObject* parent)
    : QObject(parent),
      packages_(packages),
      settings_(settings),
      provider_(std::make_unique<GitHubReleaseProvider>(
          QStringLiteral(XENON_LAUNCHER_UPDATE_REPOSITORY))) {
  launcher_state_.insert(QStringLiteral("currentVersion"), QStringLiteral(XENON_LAUNCHER_VERSION));
  launcher_state_.insert(QStringLiteral("status"), QStringLiteral("idle"));
  launcher_state_.insert(QStringLiteral("statusMessage"), QStringLiteral("Ready to check for updates."));
  launcher_state_.insert(QStringLiteral("availableVersion"), QString{});
  launcher_state_.insert(QStringLiteral("releaseName"), QString{});
  launcher_state_.insert(QStringLiteral("releaseNotes"), QString{});
  launcher_state_.insert(QStringLiteral("releaseUrl"), QString{});
  launcher_state_.insert(QStringLiteral("publishedAt"), QString{});
  launcher_state_.insert(QStringLiteral("assetName"), GitHubReleaseProvider::hostAssetName());
  launcher_state_.insert(QStringLiteral("assetSize"), 0);
  launcher_state_.insert(QStringLiteral("downloadedBytes"), 0);
  launcher_state_.insert(QStringLiteral("downloadTotalBytes"), 0);
  launcher_state_.insert(QStringLiteral("downloadProgress"), 0.0);
  launcher_state_.insert(QStringLiteral("stagedPath"), QString{});
  launcher_state_.insert(QStringLiteral("lastError"), QString{});
  launcher_state_.insert(QStringLiteral("repository"), QStringLiteral(XENON_LAUNCHER_UPDATE_REPOSITORY));
  launcher_state_.insert(QStringLiteral("installerSupported"), UpdateInstaller::platformSupported());
  launcher_state_.insert(QStringLiteral("canDownload"), false);
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  launcher_state_.insert(QStringLiteral("busy"), false);
  launcher_state_.insert(QStringLiteral("prerelease"), false);

  connectProvider();
  connect(&settings_, &SettingsFeature::changed, this,
          [this](const QString& key, const QVariant&) {
            if (key.startsWith(QStringLiteral("updates/"))) emit changed();
          });
}

UpdateFeature::~UpdateFeature() = default;

void UpdateFeature::initialize() {
  const auto saved = settings_.stringValue(QString::fromLatin1(kLastCheckedKey));
  launcher_state_.insert(QStringLiteral("lastCheckedAt"), saved);
  emit changed();

  if (!automaticCheckDue()) return;
  QTimer::singleShot(1200, this, [this]() {
    if (launcher_state_.value(QStringLiteral("busy")).toBool()) return;
    (void)checkLauncher(false);
  });
}

void UpdateFeature::connectProvider() {
  connect(provider_.get(), &UpdateProvider::checkSucceeded, this,
          &UpdateFeature::handleCheckSucceeded);
  connect(provider_.get(), &UpdateProvider::checkFailed, this,
          &UpdateFeature::handleCheckFailed);
  connect(provider_.get(), &UpdateProvider::downloadProgress, this,
          [this](qint64 received, qint64 total) {
            launcher_state_.insert(QStringLiteral("downloadedBytes"), received);
            launcher_state_.insert(QStringLiteral("downloadTotalBytes"), total);
            const auto progress = total > 0 ? static_cast<double>(received) / static_cast<double>(total) : 0.0;
            launcher_state_.insert(QStringLiteral("downloadProgress"), progress);
            emit changed();
          });
  connect(provider_.get(), &UpdateProvider::downloadSucceeded, this,
          &UpdateFeature::handleDownloadSucceeded);
  connect(provider_.get(), &UpdateProvider::downloadFailed, this,
          &UpdateFeature::handleDownloadFailed);
}

QVariantMap UpdateFeature::launcherState() const {
  auto state = launcher_state_;
  state.insert(QStringLiteral("automaticChecks"),
               settings_.boolValue(QStringLiteral("updates/automaticChecks"), true));
  state.insert(QStringLiteral("checkInterval"),
               settings_.stringValue(QStringLiteral("updates/checkInterval"), QStringLiteral("Daily")));
  state.insert(QStringLiteral("includePrerelease"),
               settings_.boolValue(QStringLiteral("updates/prerelease"), false));
  state.insert(QStringLiteral("moduleChecks"),
               settings_.boolValue(QStringLiteral("updates/modules"), true));
  return state;
}

void UpdateFeature::setStatus(const QString& status, const QString& message) {
  launcher_state_.insert(QStringLiteral("status"), status);
  launcher_state_.insert(QStringLiteral("statusMessage"), message);
  launcher_state_.insert(QStringLiteral("busy"),
                         status == QStringLiteral("checking") || status == QStringLiteral("downloading") ||
                             status == QStringLiteral("installing"));
  emit changed();
}

ServiceResult UpdateFeature::checkLauncher(bool manual) {
  const auto status = launcher_state_.value(QStringLiteral("status")).toString();
  if (status == QStringLiteral("checking") || status == QStringLiteral("downloading") ||
      status == QStringLiteral("installing")) {
    return ServiceResult::failure(QStringLiteral("Update operation in progress"),
                                  QStringLiteral("Wait for the current update operation to finish."));
  }

  manual_check_ = manual;
  launcher_state_.insert(QStringLiteral("lastError"), QString{});
  launcher_state_.insert(QStringLiteral("canDownload"), false);
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  setStatus(QStringLiteral("checking"), QStringLiteral("Checking GitHub Releases…"));
  provider_->check(settings_.boolValue(QStringLiteral("updates/prerelease"), false));
  return ServiceResult::success();
}

void UpdateFeature::handleCheckSucceeded(const UpdateRelease& release) {
  const auto checked_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  settings_.setValue(QString::fromLatin1(kLastCheckedKey), checked_at);
  launcher_state_.insert(QStringLiteral("lastCheckedAt"), checked_at);
  launcher_state_.insert(QStringLiteral("lastError"), QString{});

  available_release_ = release;
  if (!release.valid()) {
    launcher_state_.insert(QStringLiteral("availableVersion"), QString{});
    launcher_state_.insert(QStringLiteral("canDownload"), false);
    launcher_state_.insert(QStringLiteral("canInstall"), false);
    setStatus(QStringLiteral("no-releases"),
              QStringLiteral("The GitHub repository does not currently have a compatible published release."));
    if (manual_check_) {
      emit notificationRequested(QStringLiteral("No published launcher releases"),
                                 QStringLiteral("Xenon reached GitHub successfully, but there is no release to install yet."));
    }
    manual_check_ = false;
    return;
  }

  launcher_state_.insert(QStringLiteral("availableVersion"), release.version);
  launcher_state_.insert(QStringLiteral("releaseName"), release.name);
  launcher_state_.insert(QStringLiteral("releaseNotes"), release.notes);
  launcher_state_.insert(QStringLiteral("releaseUrl"), release.html_url.toString());
  launcher_state_.insert(QStringLiteral("publishedAt"), release.published_at);
  launcher_state_.insert(QStringLiteral("prerelease"), release.prerelease);
  launcher_state_.insert(QStringLiteral("assetName"), release.asset.name.isEmpty()
                                                       ? GitHubReleaseProvider::hostAssetName()
                                                       : release.asset.name);
  launcher_state_.insert(QStringLiteral("assetSize"), release.asset.size);

  const auto current = SemanticVersion::parse(QStringLiteral(XENON_LAUNCHER_VERSION));
  const auto candidate = SemanticVersion::parse(release.version);
  const auto newer = !current.valid() || (candidate.valid() && candidate.compare(current) > 0);

  if (!newer) {
    launcher_state_.insert(QStringLiteral("canDownload"), false);
    launcher_state_.insert(QStringLiteral("canInstall"), false);
    setStatus(QStringLiteral("up-to-date"),
              QStringLiteral("Xenon Launcher %1 is current.").arg(QStringLiteral(XENON_LAUNCHER_VERSION)));
    if (manual_check_) {
      emit notificationRequested(QStringLiteral("Xenon Launcher is up to date"),
                                 launcher_state_.value(QStringLiteral("statusMessage")).toString());
    }
    manual_check_ = false;
    return;
  }

  if (!release.asset.valid()) {
    launcher_state_.insert(QStringLiteral("canDownload"), false);
    launcher_state_.insert(QStringLiteral("canInstall"), false);
    setStatus(QStringLiteral("asset-unavailable"),
              QStringLiteral("Version %1 is available, but that release does not contain %2.")
                  .arg(release.version, GitHubReleaseProvider::hostAssetName()));
    if (manual_check_) {
      emit notificationRequested(QStringLiteral("Update package unavailable"),
                                 launcher_state_.value(QStringLiteral("statusMessage")).toString());
    }
    manual_check_ = false;
    return;
  }

  launcher_state_.insert(QStringLiteral("canDownload"), true);
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  setStatus(QStringLiteral("update-available"),
            QStringLiteral("Xenon Launcher %1 is available.").arg(release.version));
  emit notificationRequested(QStringLiteral("Launcher update available"),
                             QStringLiteral("Version %1 is ready to download.").arg(release.version));
  manual_check_ = false;
}

void UpdateFeature::handleCheckFailed(const QString& message) {
  launcher_state_.insert(QStringLiteral("lastError"), message);
  launcher_state_.insert(QStringLiteral("canDownload"), false);
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  setStatus(QStringLiteral("error"), message);
  if (manual_check_) emit notificationRequested(QStringLiteral("Update check failed"), message);
  manual_check_ = false;
}

ServiceResult UpdateFeature::downloadLauncher() {
  if (launcher_state_.value(QStringLiteral("status")).toString() != QStringLiteral("update-available") ||
      !available_release_.valid() || !available_release_.asset.valid()) {
    return ServiceResult::failure(QStringLiteral("No launcher update selected"),
                                  QStringLiteral("Check for updates before downloading a launcher package."));
  }

  const auto target = packages_.allocateStagingPath(QStringLiteral("launcher-update"),
                                                     available_release_.asset.name);
  if (!target.ok) return target;

  launcher_state_.insert(QStringLiteral("stagedPath"), QString{});
  launcher_state_.insert(QStringLiteral("downloadedBytes"), 0);
  launcher_state_.insert(QStringLiteral("downloadTotalBytes"), available_release_.asset.size);
  launcher_state_.insert(QStringLiteral("downloadProgress"), 0.0);
  launcher_state_.insert(QStringLiteral("canDownload"), false);
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  setStatus(QStringLiteral("downloading"),
            QStringLiteral("Downloading and verifying Xenon Launcher %1…").arg(available_release_.version));
  provider_->download(available_release_, target.data.toString());
  return ServiceResult::success();
}

void UpdateFeature::handleDownloadSucceeded(const QString& path, const UpdateRelease& release) {
  available_release_ = release;
  launcher_state_.insert(QStringLiteral("stagedPath"), path);
  launcher_state_.insert(QStringLiteral("downloadedBytes"), QFileInfo{path}.size());
  launcher_state_.insert(QStringLiteral("downloadTotalBytes"), QFileInfo{path}.size());
  launcher_state_.insert(QStringLiteral("downloadProgress"), 1.0);
  launcher_state_.insert(QStringLiteral("canDownload"), false);
  launcher_state_.insert(QStringLiteral("canInstall"), UpdateInstaller::platformSupported());
  setStatus(QStringLiteral("ready-to-install"),
            UpdateInstaller::platformSupported()
                ? QStringLiteral("Version %1 was downloaded and SHA-256 verified. Ready to install and restart.")
                      .arg(release.version)
                : QStringLiteral("Version %1 was downloaded and verified. Automatic installation is unavailable on this platform.")
                      .arg(release.version));
  emit notificationRequested(QStringLiteral("Launcher update ready"),
                             launcher_state_.value(QStringLiteral("statusMessage")).toString());
}

void UpdateFeature::handleDownloadFailed(const QString& message) {
  launcher_state_.insert(QStringLiteral("lastError"), message);
  launcher_state_.insert(QStringLiteral("canDownload"), available_release_.asset.valid());
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  launcher_state_.insert(QStringLiteral("stagedPath"), QString{});
  setStatus(QStringLiteral("error"), message);
  emit notificationRequested(QStringLiteral("Update download failed"), message);
}

ServiceResult UpdateFeature::cancelLauncherDownload() {
  if (launcher_state_.value(QStringLiteral("status")).toString() != QStringLiteral("downloading")) {
    return ServiceResult::failure(QStringLiteral("No download in progress"),
                                  QStringLiteral("There is no launcher update download to cancel."));
  }
  provider_->cancel();
  launcher_state_.insert(QStringLiteral("canDownload"), available_release_.asset.valid());
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  setStatus(QStringLiteral("update-available"), QStringLiteral("Update download cancelled."));
  return ServiceResult::success(QStringLiteral("Download cancelled"),
                                QStringLiteral("The staged update download was discarded."));
}

ServiceResult UpdateFeature::installLauncher() {
  if (!UpdateInstaller::platformSupported()) {
    return ServiceResult::failure(
        QStringLiteral("Automatic installation unavailable"),
        QStringLiteral("This Xenon build can check and download updates, but automatic replacement is currently Windows-only."));
  }

  const auto path = launcher_state_.value(QStringLiteral("stagedPath")).toString();
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    return ServiceResult::failure(QStringLiteral("Update package missing"),
                                  QStringLiteral("Download the launcher update again before installing it."));
  }

  const auto result = installer_.installAndRestart(path, available_release_.version);
  if (!result.ok) return result;
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  setStatus(QStringLiteral("installing"),
            QStringLiteral("Closing Xenon Launcher so the verified update can be installed."));
  QTimer::singleShot(100, this, [this]() { emit restartRequested(); });
  return result;
}

ServiceResult UpdateFeature::stageLauncherPackage(const QUrl& source) {
  const auto result = packages_.stage(source, QStringLiteral("launcher-update"));
  if (!result.ok) return result;
  launcher_state_.insert(QStringLiteral("status"), QStringLiteral("staged-manual"));
  launcher_state_.insert(QStringLiteral("statusMessage"),
                         QStringLiteral("A local package was staged. Local packages are not auto-installed because they have no GitHub release digest."));
  launcher_state_.insert(QStringLiteral("stagedPath"), result.data.toString());
  launcher_state_.insert(QStringLiteral("canInstall"), false);
  emit changed();
  return ServiceResult::success(
      QStringLiteral("Launcher package staged"),
      QStringLiteral("The local package was copied to Xenon's staging area but was not trusted for automatic installation."),
      result.data);
}

qint64 UpdateFeature::intervalSeconds(const QString& interval) {
  if (interval == QStringLiteral("Daily")) return 24 * 60 * 60;
  if (interval == QStringLiteral("Weekly")) return 7 * 24 * 60 * 60;
  if (interval == QStringLiteral("At startup")) return 0;
  return -1;
}

bool UpdateFeature::automaticCheckDue() const {
  if (!settings_.boolValue(QStringLiteral("updates/automaticChecks"), true)) return false;
  const auto interval = settings_.stringValue(QStringLiteral("updates/checkInterval"), QStringLiteral("Daily"));
  const auto seconds = intervalSeconds(interval);
  if (seconds < 0) return false;
  if (seconds == 0) return true;

  const auto saved = settings_.stringValue(QString::fromLatin1(kLastCheckedKey));
  const auto last = QDateTime::fromString(saved, Qt::ISODateWithMs);
  if (!last.isValid()) return true;
  return last.secsTo(QDateTime::currentDateTimeUtc()) >= seconds;
}

}  // namespace xenon::launcher::frontend_backend
