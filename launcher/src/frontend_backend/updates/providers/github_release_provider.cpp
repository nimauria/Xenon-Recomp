#include "github_release_provider.hpp"

#include "../version/semantic_version.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QSysInfo>

namespace xenon::launcher::frontend_backend {
namespace {

QString platformToken() {
#if defined(Q_OS_WIN)
  return QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
  return QStringLiteral("linux");
#elif defined(Q_OS_MACOS)
  return QStringLiteral("macos");
#else
  return QStringLiteral("unknown");
#endif
}

QString architectureToken() {
  const auto architecture = QSysInfo::currentCpuArchitecture().trimmed().toLower();
  if (architecture == QStringLiteral("x86_64") || architecture == QStringLiteral("amd64") ||
      architecture == QStringLiteral("x64")) {
    return QStringLiteral("x64");
  }
  if (architecture == QStringLiteral("arm64") || architecture == QStringLiteral("aarch64")) {
    return QStringLiteral("arm64");
  }
  return architecture.isEmpty() ? QStringLiteral("unknown") : architecture;
}

QString digestHex(const QString& digest) {
  const auto separator = digest.indexOf(QLatin1Char(':'));
  if (separator < 0) return {};
  if (digest.left(separator).compare(QStringLiteral("sha256"), Qt::CaseInsensitive) != 0) return {};
  return digest.mid(separator + 1).trimmed().toLower();
}

}  // namespace

GitHubReleaseProvider::GitHubReleaseProvider(QString repository, QObject* parent)
    : UpdateProvider(parent), repository_(std::move(repository)) {}

QString GitHubReleaseProvider::hostAssetName() {
  return QStringLiteral("xenon-launcher-%1-%2.zip").arg(platformToken(), architectureToken());
}

QNetworkRequest GitHubReleaseProvider::apiRequest(const QUrl& url) const {
  QNetworkRequest request{url};
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
  request.setRawHeader("User-Agent", "Xenon-Launcher-Updater");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  return request;
}

QNetworkRequest GitHubReleaseProvider::assetRequest(const QUrl& url) const {
  QNetworkRequest request{url};
  request.setRawHeader("Accept", "application/octet-stream");
  request.setRawHeader("User-Agent", "Xenon-Launcher-Updater");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  return request;
}

void GitHubReleaseProvider::check(bool include_prerelease) {
  cancel();
  if (repository_.trimmed().isEmpty() || !repository_.contains(QLatin1Char('/'))) {
    emit checkFailed(QStringLiteral("The configured GitHub update repository is invalid."));
    return;
  }

  const auto url = QUrl{QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30")
                            .arg(repository_.trimmed())};
  auto* reply = network_.get(apiRequest(url));
  active_reply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, include_prerelease]() {
    const auto cleanup = qScopeGuard([this, reply]() {
      if (active_reply_ == reply) active_reply_.clear();
      reply->deleteLater();
    });

    if (reply->error() != QNetworkReply::NoError) {
      emit checkFailed(QStringLiteral("GitHub release check failed: %1").arg(reply->errorString()));
      return;
    }

    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
      emit checkFailed(QStringLiteral("GitHub returned HTTP %1 while checking releases.").arg(status));
      return;
    }

    QString error;
    const auto release = parseBestRelease(reply->readAll(), include_prerelease, &error);
    if (!error.isEmpty()) {
      emit checkFailed(error);
      return;
    }
    emit checkSucceeded(release);
  });
}

UpdateRelease GitHubReleaseProvider::parseBestRelease(const QByteArray& payload,
                                                       bool include_prerelease,
                                                       QString* error) const {
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(payload, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isArray()) {
    if (error) *error = QStringLiteral("GitHub returned malformed release metadata.");
    return {};
  }

  const auto expected_asset = hostAssetName();
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

void GitHubReleaseProvider::download(const UpdateRelease& release, const QString& target_path) {
  cancel();
  if (!release.valid() || !release.asset.valid()) {
    emit downloadFailed(QStringLiteral("This release does not contain a compatible launcher package for this host."));
    return;
  }

  const auto expected_digest = digestHex(release.asset.digest);
  if (expected_digest.size() != 64) {
    emit downloadFailed(QStringLiteral("The release asset does not provide a valid SHA-256 digest, so Xenon will not install it."));
    return;
  }

  download_file_ = std::make_unique<QSaveFile>(target_path);
  if (!download_file_->open(QIODevice::WriteOnly)) {
    emit downloadFailed(QStringLiteral("Xenon could not create the update staging file: %1")
                            .arg(download_file_->errorString()));
    download_file_.reset();
    return;
  }

  download_hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
  download_target_ = target_path;
  download_release_ = release;

  auto* reply = network_.get(assetRequest(release.asset.download_url));
  active_reply_ = reply;

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
  connect(reply, &QNetworkReply::downloadProgress, this, &GitHubReleaseProvider::downloadProgress);
  connect(reply, &QNetworkReply::finished, this, [this, reply, expected_digest]() {
    const auto target = download_target_;
    const auto release = download_release_;

    if (active_reply_ == reply) active_reply_.clear();

    if (download_file_ && reply->bytesAvailable() > 0 && download_hash_) {
      const auto tail = reply->readAll();
      if (!tail.isEmpty()) {
        if (download_file_->write(tail) == tail.size()) download_hash_->addData(tail);
      }
    }

    const auto fail = [this, reply](const QString& message) {
      if (download_file_) download_file_->cancelWriting();
      download_file_.reset();
      download_hash_.reset();
      download_target_.clear();
      download_release_ = {};
      reply->deleteLater();
      emit downloadFailed(message);
    };

    if (reply->error() != QNetworkReply::NoError) {
      fail(QStringLiteral("Update download failed: %1").arg(reply->errorString()));
      return;
    }
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
      fail(QStringLiteral("GitHub returned HTTP %1 while downloading the update.").arg(status));
      return;
    }
    if (!download_file_ || !download_hash_) {
      fail(QStringLiteral("The update staging stream was lost before verification completed."));
      return;
    }

    const auto actual_digest = QString::fromLatin1(download_hash_->result().toHex()).toLower();
    if (actual_digest != expected_digest) {
      fail(QStringLiteral("The downloaded launcher package failed SHA-256 verification and was discarded."));
      return;
    }

    if (!download_file_->commit()) {
      fail(QStringLiteral("Xenon could not commit the verified update package to staging."));
      return;
    }

    download_file_.reset();
    download_hash_.reset();
    download_target_.clear();
    download_release_ = {};
    reply->deleteLater();
    emit downloadSucceeded(target, release);
  });
}

void GitHubReleaseProvider::cancel() {
  if (active_reply_) {
    QObject::disconnect(active_reply_, nullptr, this, nullptr);
    active_reply_->abort();
    active_reply_->deleteLater();
    active_reply_.clear();
  }
  if (download_file_) download_file_->cancelWriting();
  download_file_.reset();
  download_hash_.reset();
  download_target_.clear();
  download_release_ = {};
}

}  // namespace xenon::launcher::frontend_backend
