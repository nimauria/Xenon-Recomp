#pragma once

#include <QMetaType>
#include <QString>
#include <QUrl>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

struct UpdateAsset {
  QString name;
  QUrl download_url;
  QString digest;
  QString content_type;
  qint64 size = 0;

  [[nodiscard]] bool valid() const noexcept {
    return !name.trimmed().isEmpty() && download_url.isValid();
  }
  [[nodiscard]] QVariantMap toVariantMap() const;
};

struct UpdateRelease {
  QString tag;
  QString version;
  QString name;
  QString notes;
  QUrl html_url;
  QString published_at;
  bool prerelease = false;
  UpdateAsset asset;

  [[nodiscard]] bool valid() const noexcept { return !version.trimmed().isEmpty(); }
  [[nodiscard]] QVariantMap toVariantMap() const;
};

}  // namespace xenon::launcher::frontend_backend

Q_DECLARE_METATYPE(xenon::launcher::frontend_backend::UpdateAsset)
Q_DECLARE_METATYPE(xenon::launcher::frontend_backend::UpdateRelease)
