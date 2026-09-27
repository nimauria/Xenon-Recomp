#include "module_catalog_asset_cache.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QImageReader>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QUrl>

namespace xenon::launcher::frontend_backend {
namespace {
constexpr qint64 kMaxArtworkBytes = 24 * 1024 * 1024;
constexpr int kMaxArtworkDimension = 8192;
constexpr qint64 kMaxArtworkPixels = 40LL * 1024 * 1024;

QString stateKey(const QString& asset_key, const QString& suffix) {
  return QStringLiteral("%1%2").arg(asset_key, suffix);
}

bool validImagePayload(const QByteArray& payload) {
  QBuffer buffer;
  buffer.setData(payload);
  if (!buffer.open(QIODevice::ReadOnly)) return false;
  QImageReader reader{&buffer};
  reader.setDecideFormatFromContent(true);
  if (!reader.canRead()) return false;
  const auto size = reader.size();
  if (!size.isValid() || size.width() <= 0 || size.height() <= 0 ||
      size.width() > kMaxArtworkDimension || size.height() > kMaxArtworkDimension) {
    return false;
  }
  return static_cast<qint64>(size.width()) * static_cast<qint64>(size.height()) <=
         kMaxArtworkPixels;
}

QString versionedDestination(const QString& base_path, const QByteArray& payload) {
  const QFileInfo info{base_path};
  const auto digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256)
                          .toHex().left(16);
  const auto suffix = info.suffix();
  const auto file_name = suffix.isEmpty()
                             ? QStringLiteral("%1-%2").arg(info.completeBaseName(),
                                                           QString::fromLatin1(digest))
                             : QStringLiteral("%1-%2.%3").arg(info.completeBaseName(),
                                                               QString::fromLatin1(digest), suffix);
  return QDir{info.absolutePath()}.filePath(file_name);
}
}  // namespace

ModuleCatalogAssetCache::ModuleCatalogAssetCache(PathService& paths, QObject* parent)
    : QObject(parent), paths_(paths) {}

bool ModuleCatalogAssetCache::safeModuleId(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[a-z0-9]+(?:[._-][a-z0-9]+)+$")};
  return pattern.match(value.trimmed()).hasMatch();
}

bool ModuleCatalogAssetCache::safeAssetKey(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z][A-Za-z0-9_-]{0,31}$")};
  return pattern.match(value.trimmed()).hasMatch();
}

bool ModuleCatalogAssetCache::safeRemoteUrl(const QUrl& url) {
  if (!url.isValid() || url.scheme().toLower() != QStringLiteral("https")) return false;
  const auto host = url.host().toLower();
  return host == QStringLiteral("raw.githubusercontent.com") ||
         host == QStringLiteral("github.com") ||
         host == QStringLiteral("objects.githubusercontent.com");
}

bool ModuleCatalogAssetCache::looksLikeImage(const QByteArray& content_type,
                                              const QString& file_name) {
  const auto type = content_type.toLower();
  if (type.startsWith("image/")) return true;
  const auto suffix = QFileInfo{file_name}.suffix().toLower();
  return suffix == QStringLiteral("png") || suffix == QStringLiteral("jpg") ||
         suffix == QStringLiteral("jpeg") || suffix == QStringLiteral("webp") ||
         suffix == QStringLiteral("gif") || suffix == QStringLiteral("bmp");
}

QString ModuleCatalogAssetCache::moduleRoot(const QString& module_id) const {
  if (!safeModuleId(module_id)) return {};
  const auto cache_root = paths_.configuredPath(QStringLiteral("cache"));
  if (cache_root.trimmed().isEmpty()) return {};
  return QDir{cache_root}.filePath(QStringLiteral("ModuleCatalogAssets/%1").arg(module_id));
}

QString ModuleCatalogAssetCache::metadataPath(const QString& module_id,
                                               const QString& asset_key) const {
  if (!safeAssetKey(asset_key)) return {};
  const auto root = moduleRoot(module_id);
  return root.isEmpty() ? QString{} : QDir{root}.filePath(asset_key + QStringLiteral(".json"));
}

QVariantMap ModuleCatalogAssetCache::readMetadata(const QString& path) const {
  QFile file{path};
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
             ? document.object().toVariantMap()
             : QVariantMap{};
}

bool ModuleCatalogAssetCache::writeMetadata(const QString& path,
                                             const QVariantMap& metadata) const {
  QSaveFile file{path};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
  file.write(QJsonDocument{QJsonObject::fromVariantMap(metadata)}.toJson(QJsonDocument::Indented));
  return file.commit();
}

QString ModuleCatalogAssetCache::destinationPath(const QString& module_id,
                                                  const QString& asset_key,
                                                  const QUrl& remote_url) const {
  const auto root = moduleRoot(module_id);
  if (root.isEmpty() || !safeAssetKey(asset_key)) return {};
  auto suffix = QFileInfo{remote_url.path()}.suffix().toLower();
  if (suffix != QStringLiteral("png") && suffix != QStringLiteral("jpg") &&
      suffix != QStringLiteral("jpeg") && suffix != QStringLiteral("webp") &&
      suffix != QStringLiteral("gif") && suffix != QStringLiteral("bmp")) {
    suffix = QStringLiteral("img");
  }
  return QDir{root}.filePath(QStringLiteral("%1.%2").arg(asset_key, suffix));
}

void ModuleCatalogAssetCache::cancel() {
  for (const auto& reply : replies_) {
    if (reply) reply->abort();
  }
  replies_.clear();
  active_assets_.clear();
}

void ModuleCatalogAssetCache::sync(const QVariantList& catalog_entries) {
  for (const auto& value : catalog_entries) {
    const auto entry = value.toMap();
    const auto module_id = entry.value(QStringLiteral("moduleId")).toString().trimmed();
    if (!safeModuleId(module_id)) continue;
    const auto launcher = entry.value(QStringLiteral("launcher")).toMap();
    const auto tile = QUrl{launcher.value(QStringLiteral("tileArtUrl")).toString()};
    const auto hero = QUrl{launcher.value(QStringLiteral("heroArtUrl")).toString()};
    if (safeRemoteUrl(tile)) syncAsset(module_id, QStringLiteral("tileArt"), tile);
    if (safeRemoteUrl(hero)) syncAsset(module_id, QStringLiteral("heroArt"), hero);
  }
}

void ModuleCatalogAssetCache::syncAsset(const QString& module_id, const QString& asset_key,
                                        const QUrl& remote_url) {
  if (!safeModuleId(module_id) || !safeAssetKey(asset_key) || !safeRemoteUrl(remote_url)) return;

  const auto root = moduleRoot(module_id);
  const auto meta_path = metadataPath(module_id, asset_key);
  const auto destination = destinationPath(module_id, asset_key, remote_url);
  if (root.isEmpty() || meta_path.isEmpty() || destination.isEmpty() ||
      !paths_.ensureDirectory(root)) {
    return;
  }

  const auto request_key = module_id + QLatin1Char('/') + asset_key;
  if (const auto active = active_assets_.value(request_key); active) {
    if (active->url() == remote_url) return;
    active_assets_.remove(request_key);
    active->abort();
  }

  const auto existing = readMetadata(meta_path);
  QNetworkRequest request{remote_url};
  request.setRawHeader("Accept", "image/*");
  request.setRawHeader("User-Agent", "Xenon-Launcher-Module-Artwork");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  if (existing.value(QStringLiteral("url")).toString() == remote_url.toString() &&
      QFileInfo::exists(existing.value(QStringLiteral("path")).toString())) {
    const auto etag = existing.value(QStringLiteral("etag")).toByteArray();
    const auto modified = existing.value(QStringLiteral("lastModified")).toByteArray();
    if (!etag.isEmpty()) request.setRawHeader("If-None-Match", etag);
    if (!modified.isEmpty()) request.setRawHeader("If-Modified-Since", modified);
  }

  auto state = module_states_.value(module_id);
  state.insert(stateKey(asset_key, QStringLiteral("Status")), QStringLiteral("checking"));
  module_states_.insert(module_id, state);

  auto* reply = network_.get(request);
  replies_.append(reply);
  active_assets_.insert(request_key, reply);
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, request_key, module_id, asset_key, remote_url, destination, meta_path, existing]() {
    const auto cleanup = qScopeGuard([this, reply, request_key]() {
      replies_.removeAll(reply);
      if (active_assets_.value(request_key) == reply) active_assets_.remove(request_key);
      reply->deleteLater();
    });
    if (active_assets_.value(request_key) != reply) return;

    auto state = module_states_.value(module_id);
    const auto status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::NoError && status_code == 304) {
      auto refreshed_metadata = existing;
      refreshed_metadata.insert(QStringLiteral("checkedAt"),
                                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
      (void)writeMetadata(meta_path, refreshed_metadata);
      state.insert(stateKey(asset_key, QStringLiteral("Status")), QStringLiteral("ready"));
      state.insert(stateKey(asset_key, QStringLiteral("Path")),
                   existing.value(QStringLiteral("path")));
      state.remove(stateKey(asset_key, QStringLiteral("Error")));
      module_states_.insert(module_id, state);
      emit changed(module_id);
      return;
    }

    if (reply->error() != QNetworkReply::NoError || status_code < 200 || status_code >= 300) {
      state.insert(stateKey(asset_key, QStringLiteral("Status")), QStringLiteral("error"));
      state.insert(stateKey(asset_key, QStringLiteral("Error")), reply->errorString());
      module_states_.insert(module_id, state);
      emit changed(module_id);
      return;
    }

    const auto content_length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if ((content_length > 0 && content_length > kMaxArtworkBytes) ||
        !looksLikeImage(reply->rawHeader("Content-Type"), destination)) {
      state.insert(stateKey(asset_key, QStringLiteral("Status")), QStringLiteral("error"));
      state.insert(stateKey(asset_key, QStringLiteral("Error")),
                   QStringLiteral("The registry artwork response was not an accepted image."));
      module_states_.insert(module_id, state);
      emit changed(module_id);
      return;
    }

    const auto payload = reply->readAll();
    if (payload.isEmpty() || payload.size() > kMaxArtworkBytes || !validImagePayload(payload)) {
      state.insert(stateKey(asset_key, QStringLiteral("Status")), QStringLiteral("error"));
      state.insert(stateKey(asset_key, QStringLiteral("Error")),
                   QStringLiteral("The registry artwork response was empty, too large, or not a valid bounded image."));
      module_states_.insert(module_id, state);
      emit changed(module_id);
      return;
    }

    const auto final_destination = versionedDestination(destination, payload);
    QSaveFile output{final_destination};
    if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size() ||
        !output.commit()) {
      state.insert(stateKey(asset_key, QStringLiteral("Status")), QStringLiteral("error"));
      state.insert(stateKey(asset_key, QStringLiteral("Error")),
                   QStringLiteral("Xenon could not write the registry artwork cache."));
      module_states_.insert(module_id, state);
      emit changed(module_id);
      return;
    }

    const auto previous_path = existing.value(QStringLiteral("path")).toString();
    if (!previous_path.isEmpty() && previous_path != final_destination && QFileInfo::exists(previous_path)) {
      QFile::remove(previous_path);
    }

    QVariantMap metadata;
    metadata.insert(QStringLiteral("url"), remote_url.toString());
    metadata.insert(QStringLiteral("path"), final_destination);
    metadata.insert(QStringLiteral("etag"), QString::fromUtf8(reply->rawHeader("ETag")));
    metadata.insert(QStringLiteral("lastModified"),
                    QString::fromUtf8(reply->rawHeader("Last-Modified")));
    metadata.insert(QStringLiteral("contentType"),
                    QString::fromUtf8(reply->rawHeader("Content-Type")));
    metadata.insert(QStringLiteral("size"), payload.size());
    metadata.insert(QStringLiteral("checkedAt"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    (void)writeMetadata(meta_path, metadata);

    state.insert(stateKey(asset_key, QStringLiteral("Status")), QStringLiteral("ready"));
    state.insert(stateKey(asset_key, QStringLiteral("Path")), final_destination);
    state.remove(stateKey(asset_key, QStringLiteral("Error")));
    module_states_.insert(module_id, state);
    emit changed(module_id);
  });
}

QString ModuleCatalogAssetCache::localAssetUrl(const QString& module_id,
                                                const QString& asset_key) const {
  if (!safeModuleId(module_id) || !safeAssetKey(asset_key)) return {};
  const auto metadata = readMetadata(metadataPath(module_id, asset_key));
  const auto path = metadata.value(QStringLiteral("path")).toString();
  if (path.isEmpty() || !QFileInfo::exists(path)) return {};
  return QUrl::fromLocalFile(path).toString();
}

QVariantMap ModuleCatalogAssetCache::state(const QString& module_id) const {
  return module_states_.value(module_id);
}

}  // namespace xenon::launcher::frontend_backend
