#include "branding_feature.hpp"

#include <QColor>
#include <QFile>
#include <QHash>

namespace xenon::launcher::frontend_backend {

QString BrandingFeature::themedDataUrl(const QString& asset_name, const QString& color) const {
  static const QHash<QString, QString> assets{
      {QStringLiteral("mark"), QStringLiteral(":/branding/xenon-mark.svg")},
      {QStringLiteral("icon"), QStringLiteral(":/branding/xenon-icon.svg")},
      {QStringLiteral("wordmark"), QStringLiteral(":/branding/xenon-wordmark.svg")},
      {QStringLiteral("lockup"), QStringLiteral(":/branding/xenon-lockup.svg")},
      {QStringLiteral("lockup-full"), QStringLiteral(":/branding/xenon-lockup-full.svg")},
      {QStringLiteral("profile-mark"), QStringLiteral(":/branding/xenon-profile-mark.svg")},
  };
  const auto path = assets.value(asset_name, assets.value(QStringLiteral("mark")));
  QFile file{path};
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  auto svg = file.readAll();
  const auto normalized = QColor{color}.isValid() ? QColor{color}.name(QColor::HexRgb)
                                                   : QStringLiteral("#35D7EA");
  svg.replace("currentColor", normalized.toUtf8());
  return QStringLiteral("data:image/svg+xml;base64,%1").arg(QString::fromLatin1(svg.toBase64()));
}

}  // namespace xenon::launcher::frontend_backend
