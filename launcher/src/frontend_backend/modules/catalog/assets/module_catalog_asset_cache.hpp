#pragma once

#include "../../../../services/path_service.hpp"

#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>

class QNetworkReply;

namespace xenon::launcher::frontend_backend {

// Caches launcher-presentation assets declared by the official Xenon Modules
// registry. QML only ever receives local file:// URLs; it never owns remote
// GitHub networking, cache paths, conditional requests, or validation.
class ModuleCatalogAssetCache final : public QObject {
  Q_OBJECT

 public:
  explicit ModuleCatalogAssetCache(PathService& paths, QObject* parent = nullptr);

  void sync(const QVariantList& catalog_entries);
  void cancel();

  [[nodiscard]] QString localAssetUrl(const QString& module_id,
                                      const QString& asset_key) const;
  [[nodiscard]] QVariantMap state(const QString& module_id) const;

 signals:
  void changed(const QString& module_id);

 private:
  void syncAsset(const QString& module_id, const QString& asset_key,
                 const QUrl& remote_url);
  [[nodiscard]] QString moduleRoot(const QString& module_id) const;
  [[nodiscard]] QString metadataPath(const QString& module_id,
                                     const QString& asset_key) const;
  [[nodiscard]] QVariantMap readMetadata(const QString& path) const;
  [[nodiscard]] bool writeMetadata(const QString& path,
                                   const QVariantMap& metadata) const;
  [[nodiscard]] QString destinationPath(const QString& module_id,
                                        const QString& asset_key,
                                        const QUrl& remote_url) const;
  [[nodiscard]] static bool safeModuleId(const QString& value);
  [[nodiscard]] static bool safeAssetKey(const QString& value);
  [[nodiscard]] static bool safeRemoteUrl(const QUrl& url);
  [[nodiscard]] static bool looksLikeImage(const QByteArray& content_type,
                                           const QString& file_name);

  PathService& paths_;
  QNetworkAccessManager network_;
  QList<QPointer<QNetworkReply>> replies_;
  QHash<QString, QPointer<QNetworkReply>> active_assets_;
  QHash<QString, QVariantMap> module_states_;
};

}  // namespace xenon::launcher::frontend_backend
