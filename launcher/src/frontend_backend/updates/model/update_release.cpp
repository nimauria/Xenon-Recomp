#include "update_release.hpp"

namespace xenon::launcher::frontend_backend {

QVariantMap UpdateAsset::toVariantMap() const {
  return QVariantMap{{QStringLiteral("name"), name},
                     {QStringLiteral("downloadUrl"), download_url.toString()},
                     {QStringLiteral("digest"), digest},
                     {QStringLiteral("contentType"), content_type},
                     {QStringLiteral("size"), size}};
}

QVariantMap UpdateRelease::toVariantMap() const {
  return QVariantMap{{QStringLiteral("tag"), tag},
                     {QStringLiteral("version"), version},
                     {QStringLiteral("name"), name},
                     {QStringLiteral("notes"), notes},
                     {QStringLiteral("htmlUrl"), html_url.toString()},
                     {QStringLiteral("publishedAt"), published_at},
                     {QStringLiteral("prerelease"), prerelease},
                     {QStringLiteral("asset"), asset.toVariantMap()}};
}

}  // namespace xenon::launcher::frontend_backend
