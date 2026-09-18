#include "module_update_service.hpp"

#include "../../updates/version/semantic_version.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>

namespace xenon::launcher::frontend_backend {
namespace {

bool safeRepository(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")};
  return pattern.match(value.trimmed()).hasMatch();
}

}  // namespace

ModuleUpdateService::ModuleUpdateService(PackageService& packages, ModuleService& modules,
                                         QObject* parent)
    : QObject(parent), packages_(packages), modules_(modules), installer_(modules_) {}

QVariantMap ModuleUpdateService::state(const QString& module_id) const {
  auto value = states_.value(module_id);
  if (value.isEmpty()) {
    value.insert(QStringLiteral("moduleId"), module_id);
    value.insert(QStringLiteral("status"), QStringLiteral("idle"));
    value.insert(QStringLiteral("statusMessage"), QStringLiteral("Updates have not been checked yet."));
    value.insert(QStringLiteral("canDownload"), false);
    value.insert(QStringLiteral("canInstall"), false);
    value.insert(QStringLiteral("updateAvailable"), false);
    value.insert(QStringLiteral("downloadProgress"), 0.0);
  }
  return value;
}

QVariantMap ModuleUpdateService::states() const {
  QVariantMap result;
  for (auto it = states_.cbegin(); it != states_.cend(); ++it) result.insert(it.key(), it.value());
  return result;
}

void ModuleUpdateService::setState(const QString& module_id, const QString& status,
                                   const QString& message) {
  auto value = state(module_id);
  value.insert(QStringLiteral("status"), status);
  if (!message.isEmpty()) value.insert(QStringLiteral("statusMessage"), message);
  states_.insert(module_id, value);
  emit changed(module_id);
}

QNetworkRequest ModuleUpdateService::apiRequest(const QUrl& url) const {
  QNetworkRequest request{url};
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
  request.setRawHeader("User-Agent", "Xenon-Launcher-Module-Updater");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  return request;
}

QNetworkRequest ModuleUpdateService::assetRequest(const QUrl& url) const {
  QNetworkRequest request{url};
  request.setRawHeader("Accept", "application/octet-stream");
  request.setRawHeader("User-Agent", "Xenon-Launcher-Module-Updater");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  return request;
}

QString ModuleUpdateService::digestHex(const QString& digest) {
  const auto separator = digest.indexOf(QLatin1Char(':'));
  if (separator < 0) return {};
  if (digest.left(separator).compare(QStringLiteral("sha256"), Qt::CaseInsensitive) != 0) return {};
  return digest.mid(separator + 1).trimmed().toLower();
}

void ModuleUpdateService::check(const QString& module_id, const QVariantMap& catalog_entry,
                                const QString& installed_version, bool include_prerelease) {
  const auto repository = catalog_entry.value(QStringLiteral("repository")).toString().trimmed();
  const auto asset_name = catalog_entry.value(QStringLiteral("assetName")).toString().trimmed();
  if (!safeRepository(repository)) {
    auto value = state(module_id);
    value.insert(QStringLiteral("canDownload"), false);
    value.insert(QStringLiteral("canInstall"), false);
    value.insert(QStringLiteral("updateAvailable"), false);
    states_.insert(module_id, value);
    setState(module_id, QStringLiteral("catalog-missing"),
             QStringLiteral("This module does not have a valid GitHub release repository in the official catalog."));
    return;
  }
  if (asset_name.isEmpty()) {
    auto value = state(module_id);
    value.insert(QStringLiteral("repository"), repository);
    value.insert(QStringLiteral("canDownload"), false);
    value.insert(QStringLiteral("canInstall"), false);
    value.insert(QStringLiteral("updateAvailable"), false);
    states_.insert(module_id, value);
    setState(module_id, QStringLiteral("host-unsupported"),
             QStringLiteral("The catalog does not define a module package for this operating system and CPU architecture."));
    return;
  }

  catalog_entries_.insert(module_id, catalog_entry);
  auto value = state(module_id);
  value.insert(QStringLiteral("repository"), repository);
  value.insert(QStringLiteral("repositoryUrl"), catalog_entry.value(QStringLiteral("repositoryUrl")));
  value.insert(QStringLiteral("assetName"), asset_name);
  value.insert(QStringLiteral("installedVersion"), installed_version);
  value.insert(QStringLiteral("canDownload"), false);
  value.insert(QStringLiteral("canInstall"), false);
  value.insert(QStringLiteral("updateAvailable"), false);
  value.insert(QStringLiteral("downloadProgress"), 0.0);
  states_.insert(module_id, value);
  setState(module_id, QStringLiteral("checking"), QStringLiteral("Checking GitHub releases…"));

  const auto url = QUrl{QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(repository)};
  auto* reply = network_.get(apiRequest(url));
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, module_id, asset_name, include_prerelease, installed_version]() {
    const auto cleanup = qScopeGuard([reply]() { reply->deleteLater(); });
    if (reply->error() != QNetworkReply::NoError) {
      auto value = state(module_id);
      value.insert(QStringLiteral("lastError"), reply->errorString());
      states_.insert(module_id, value);
      setState(module_id, QStringLiteral("error"),
               QStringLiteral("GitHub release check failed: %1").arg(reply->errorString()));
      return;
    }
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
      setState(module_id, QStringLiteral("error"),
               QStringLiteral("GitHub returned HTTP %1 while checking this module.").arg(status));
      return;
    }

    QString error;
    const auto release = parseBestRelease(reply->readAll(), asset_name, include_prerelease, &error);
    if (!error.isEmpty()) {
      setState(module_id, QStringLiteral("error"), error);
      return;
    }
    handleCheckSucceeded(module_id, release, installed_version);
  });
}

UpdateRelease ModuleUpdateService::parseBestRelease(const QByteArray& payload,
                                                     const QString& expected_asset,
                                                     bool include_prerelease,
                                                     QString* error) const {
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(payload, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isArray()) {
    if (error) *error = QStringLiteral("GitHub returned malformed module release metadata.");
    return {};
  }

  UpdateRelease best;
  SemanticVersion best_version;
  for (const auto& value : document.array()) {
    const auto object = value.toObject();
    if (object.value(QStringLiteral("draft")).toBool()) continue;
    const auto prerelease = object.value(QStringLiteral("prerelease")).toBool();
    if (prerelease && !include_prerelease) continue;

    const auto tag = object.value(QStringLiteral("tag_name")).toString();
    const auto version = SemanticVersion::parse(tag);
    if (!version.valid()) continue;
    if (best.valid() && version.compare(best_version) <= 0) continue;

    UpdateAsset selected_asset;
    for (const auto& asset_value : object.value(QStringLiteral("assets")).toArray()) {
      const auto asset_object = asset_value.toObject();
      if (asset_object.value(QStringLiteral("name")).toString() != expected_asset) continue;
      selected_asset.name = expected_asset;
      selected_asset.download_url = QUrl{asset_object.value(QStringLiteral("browser_download_url")).toString()};
      selected_asset.digest = asset_object.value(QStringLiteral("digest")).toString();
      selected_asset.content_type = asset_object.value(QStringLiteral("content_type")).toString();
      selected_asset.size = static_cast<qint64>(asset_object.value(QStringLiteral("size")).toDouble());
      break;
    }

    best.tag = tag;
    best.version = version.normalized();
    best.name = object.value(QStringLiteral("name")).toString();
    if (best.name.trimmed().isEmpty()) best.name = tag;
    best.notes = object.value(QStringLiteral("body")).toString();
    best.html_url = QUrl{object.value(QStringLiteral("html_url")).toString()};
    best.published_at = object.value(QStringLiteral("published_at")).toString();
    best.prerelease = prerelease;
    best.asset = selected_asset;
    best_version = version;
  }
  return best;
}

void ModuleUpdateService::handleCheckSucceeded(const QString& module_id,
                                               const UpdateRelease& release,
                                               const QString& installed_version) {
  auto value = state(module_id);
  if (!release.valid()) {
    releases_.remove(module_id);
    value.insert(QStringLiteral("availableVersion"), QString{});
    value.insert(QStringLiteral("canDownload"), false);
    value.insert(QStringLiteral("canInstall"), false);
    value.insert(QStringLiteral("updateAvailable"), false);
    states_.insert(module_id, value);
    setState(module_id, QStringLiteral("no-release"),
             QStringLiteral("This catalog module does not have a published semantic-version GitHub release yet."));
    return;
  }

  releases_.insert(module_id, release);
  value.insert(QStringLiteral("availableVersion"), release.version);
  value.insert(QStringLiteral("releaseName"), release.name);
  value.insert(QStringLiteral("releaseNotes"), release.notes);
  value.insert(QStringLiteral("releaseUrl"), release.html_url.toString());
  value.insert(QStringLiteral("publishedAt"), release.published_at);
  value.insert(QStringLiteral("prerelease"), release.prerelease);
  value.insert(QStringLiteral("assetName"), release.asset.name.isEmpty()
                                                   ? value.value(QStringLiteral("assetName"))
                                                   : release.asset.name);
  value.insert(QStringLiteral("assetSize"), release.asset.size);

  if (!release.asset.valid()) {
    value.insert(QStringLiteral("canDownload"), false);
    value.insert(QStringLiteral("canInstall"), false);
    value.insert(QStringLiteral("updateAvailable"), false);
    states_.insert(module_id, value);
    setState(module_id, QStringLiteral("package-unavailable"),
             QStringLiteral("Release %1 exists, but it does not contain the catalog package %2.")
                 .arg(release.version, value.value(QStringLiteral("assetName")).toString()));
    return;
  }

  const auto current = SemanticVersion::parse(installed_version);
  const auto candidate = SemanticVersion::parse(release.version);
  const auto installed = !installed_version.trimmed().isEmpty();
  const auto newer = !installed || !current.valid() || (candidate.valid() && candidate.compare(current) > 0);

  value.insert(QStringLiteral("canDownload"), newer);
  value.insert(QStringLiteral("canInstall"), false);
  value.insert(QStringLiteral("updateAvailable"), installed && newer);
  states_.insert(module_id, value);

  if (!installed) {
    setState(module_id, QStringLiteral("available"),
             QStringLiteral("Module %1 is available to download.").arg(release.version));
  } else if (!newer) {
    setState(module_id, QStringLiteral("up-to-date"),
             QStringLiteral("Installed module %1 is current.").arg(installed_version));
  } else {
    setState(module_id, QStringLiteral("update-available"),
             QStringLiteral("Module %1 is available; installed version is %2.")
                 .arg(release.version, installed_version));
    emit notificationRequested(QStringLiteral("Module update available"),
                               QStringLiteral("%1 can be updated to %2.")
                                   .arg(module_id, release.version));
  }
}

ServiceResult ModuleUpdateService::download(const QString& module_id) {
  if (active_download_) {
    return ServiceResult::failure(QStringLiteral("Module download"),
                                  QStringLiteral("Another module package is already downloading."));
  }
  const auto release = releases_.value(module_id);
  auto value = state(module_id);
  if (!release.valid() || !release.asset.valid() || !value.value(QStringLiteral("canDownload")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Module download"),
                                  QStringLiteral("Check this module for updates before downloading a package."));
  }

  const auto expected_digest = digestHex(release.asset.digest);
  if (expected_digest.size() != 64) {
    return ServiceResult::failure(
        QStringLiteral("Module download"),
        QStringLiteral("The GitHub release asset does not provide a valid SHA-256 digest, so Xenon will not install it."));
  }

  const auto target = packages_.allocateStagingPath(
      QStringLiteral("modules/updates/") + module_id, release.asset.name);
  if (!target.ok) return target;

  download_file_ = std::make_unique<QSaveFile>(target.data.toString());
  if (!download_file_->open(QIODevice::WriteOnly)) {
    download_file_.reset();
    return ServiceResult::failure(QStringLiteral("Module download"),
                                  QStringLiteral("Xenon could not create the module update staging file."));
  }

  download_hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
  download_target_ = target.data.toString();
  download_release_ = release;
  active_download_module_ = module_id;
  cancel_requested_ = false;

  value.insert(QStringLiteral("canDownload"), false);
  value.insert(QStringLiteral("canInstall"), false);
  value.insert(QStringLiteral("downloadProgress"), 0.0);
  value.insert(QStringLiteral("downloadedBytes"), 0);
  value.insert(QStringLiteral("downloadTotalBytes"), release.asset.size);
  states_.insert(module_id, value);
  setState(module_id, QStringLiteral("downloading"),
           QStringLiteral("Downloading and SHA-256 verifying %1…").arg(release.asset.name));

  auto* reply = network_.get(assetRequest(release.asset.download_url));
  active_download_ = reply;
  connect(reply, &QIODevice::readyRead, this, [this, reply]() {
    if (!download_file_ || !download_hash_) return;
    const auto chunk = reply->readAll();
    if (chunk.isEmpty()) return;
    if (download_file_->write(chunk) != chunk.size()) {
      reply->abort();
      return;
    }
    download_hash_->addData(chunk);
  });
  connect(reply, &QNetworkReply::downloadProgress, this,
          [this, module_id](qint64 received, qint64 total) {
    auto value = state(module_id);
    value.insert(QStringLiteral("downloadedBytes"), received);
    value.insert(QStringLiteral("downloadTotalBytes"), total);
    value.insert(QStringLiteral("downloadProgress"), total > 0 ? double(received) / double(total) : 0.0);
    states_.insert(module_id, value);
    emit changed(module_id);
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, module_id, expected_digest]() {
    if (active_download_ == reply) active_download_.clear();

    if (download_file_ && reply->bytesAvailable() > 0 && download_hash_) {
      const auto tail = reply->readAll();
      if (!tail.isEmpty() && download_file_->write(tail) == tail.size()) download_hash_->addData(tail);
    }

    const auto fail = [this, reply, module_id](const QString& message) {
      if (download_file_) download_file_->cancelWriting();
      download_file_.reset();
      download_hash_.reset();
      download_target_.clear();
      download_release_ = {};
      active_download_module_.clear();
      reply->deleteLater();
      auto value = state(module_id);
      value.insert(QStringLiteral("canDownload"), releases_.value(module_id).asset.valid());
      value.insert(QStringLiteral("canInstall"), false);
      value.insert(QStringLiteral("lastError"), message);
      states_.insert(module_id, value);
      setState(module_id, QStringLiteral("error"), message);
    };

    if (cancel_requested_) {
      if (download_file_) download_file_->cancelWriting();
      download_file_.reset();
      download_hash_.reset();
      download_target_.clear();
      download_release_ = {};
      active_download_module_.clear();
      cancel_requested_ = false;
      reply->deleteLater();
      auto value = state(module_id);
      value.insert(QStringLiteral("canDownload"), releases_.value(module_id).asset.valid());
      value.insert(QStringLiteral("canInstall"), false);
      value.insert(QStringLiteral("downloadProgress"), 0.0);
      value.insert(QStringLiteral("lastError"), QString{});
      states_.insert(module_id, value);
      setState(module_id, QStringLiteral("cancelled"),
               QStringLiteral("Module package download was cancelled."));
      return;
    }

    if (reply->error() != QNetworkReply::NoError) {
      fail(QStringLiteral("Module package download failed: %1").arg(reply->errorString()));
      return;
    }
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
      fail(QStringLiteral("GitHub returned HTTP %1 while downloading the module package.").arg(status));
      return;
    }
    if (!download_file_ || !download_hash_) {
      fail(QStringLiteral("The module download staging state was lost."));
      return;
    }

    const auto actual_digest = QString::fromLatin1(download_hash_->result().toHex()).toLower();
    if (actual_digest != expected_digest) {
      fail(QStringLiteral("The downloaded module package failed SHA-256 verification."));
      return;
    }
    if (!download_file_->commit()) {
      fail(QStringLiteral("Xenon could not commit the verified module package to staging."));
      return;
    }

    const auto path = download_target_;
    download_file_.reset();
    download_hash_.reset();
    download_target_.clear();
    download_release_ = {};
    active_download_module_.clear();
    reply->deleteLater();

    auto value = state(module_id);
    value.insert(QStringLiteral("stagedPath"), path);
    value.insert(QStringLiteral("downloadProgress"), 1.0);
    value.insert(QStringLiteral("canDownload"), false);
    value.insert(QStringLiteral("canInstall"), ModulePackageInstaller::platformSupported());
    states_.insert(module_id, value);
    setState(module_id, QStringLiteral("ready-to-install"),
             ModulePackageInstaller::platformSupported()
                 ? QStringLiteral("The module package was downloaded and SHA-256 verified. Ready to install.")
                 : QStringLiteral("The module package was downloaded and verified, but automatic installation is unavailable on this platform."));
    emit notificationRequested(QStringLiteral("Module package ready"),
                               state(module_id).value(QStringLiteral("statusMessage")).toString());
  });

  return ServiceResult::success(QStringLiteral("Module download started"),
                                QStringLiteral("The package will be verified before installation is allowed."));
}

ServiceResult ModuleUpdateService::cancelDownload(const QString& module_id) {
  if (!active_download_ || active_download_module_ != module_id) {
    return ServiceResult::failure(QStringLiteral("Module download"),
                                  QStringLiteral("That module does not have a download in progress."));
  }
  cancel_requested_ = true;
  active_download_->abort();
  return ServiceResult::success(QStringLiteral("Module download cancelled"),
                                QStringLiteral("The partial module package will be discarded."));
}

ServiceResult ModuleUpdateService::install(const QString& module_id) {
  auto value = state(module_id);
  const auto path = value.value(QStringLiteral("stagedPath")).toString();
  if (!value.value(QStringLiteral("canInstall")).toBool() || path.isEmpty() || !QFileInfo::exists(path)) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("Download and verify the module package before installing it."));
  }

  value.insert(QStringLiteral("canInstall"), false);
  states_.insert(module_id, value);
  setState(module_id, QStringLiteral("installing"), QStringLiteral("Installing the verified module package…"));

  const auto result = installer_.install(module_id, path);
  if (!result.ok) {
    value = state(module_id);
    value.insert(QStringLiteral("canInstall"), true);
    value.insert(QStringLiteral("lastError"), result.message);
    states_.insert(module_id, value);
    setState(module_id, QStringLiteral("error"), result.message);
    return result;
  }

  const auto release = releases_.value(module_id);
  QFile::remove(path);
  value = state(module_id);
  value.insert(QStringLiteral("installedVersion"), release.version);
  value.insert(QStringLiteral("stagedPath"), QString{});
  value.insert(QStringLiteral("canDownload"), false);
  value.insert(QStringLiteral("canInstall"), false);
  value.insert(QStringLiteral("updateAvailable"), false);
  states_.insert(module_id, value);
  setState(module_id, QStringLiteral("up-to-date"),
           QStringLiteral("Module %1 was installed successfully and is current.").arg(release.version));
  emit moduleInstalled(module_id);
  emit notificationRequested(QStringLiteral("Module installed"), result.message);
  return result;
}

}  // namespace xenon::launcher::frontend_backend
