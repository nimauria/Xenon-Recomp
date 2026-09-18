#include "github_module_catalog_provider.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSysInfo>
#include <QUrl>

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

bool safeModuleId(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")};
  const auto trimmed = value.trimmed();
  return pattern.match(trimmed).hasMatch() && trimmed != QStringLiteral(".") &&
         trimmed != QStringLiteral("..") && !trimmed.contains(QStringLiteral(".."));
}

bool safeRepository(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")};
  return pattern.match(value.trimmed()).hasMatch();
}

QStringList stringList(const QVariant& value) {
  QStringList result;
  for (const auto& item : value.toList()) {
    const auto text = item.toString().trimmed();
    if (!text.isEmpty()) result.append(text);
  }
  if (result.isEmpty() && value.metaType().id() == QMetaType::QString) {
    const auto text = value.toString().trimmed();
    if (!text.isEmpty()) result.append(text);
  }
  return result;
}

QVariantMap projectGracemeriaFallback() {
  QVariantMap assets;
  assets.insert(QStringLiteral("windows-x64"), QStringLiteral("project-gracemeria-windows-x64.xenonmod.zip"));
  assets.insert(QStringLiteral("windows-arm64"), QStringLiteral("project-gracemeria-windows-arm64.xenonmod.zip"));
  assets.insert(QStringLiteral("linux-x64"), QStringLiteral("project-gracemeria-linux-x64.xenonmod.zip"));
  assets.insert(QStringLiteral("linux-arm64"), QStringLiteral("project-gracemeria-linux-arm64.xenonmod.zip"));

  QVariantMap item;
  item.insert(QStringLiteral("moduleId"), QStringLiteral("org.nimauria.project-gracemeria"));
  item.insert(QStringLiteral("moduleName"), QStringLiteral("Project Gracemeria"));
  item.insert(QStringLiteral("moduleType"), QStringLiteral("Game Module"));
  item.insert(QStringLiteral("description"),
              QStringLiteral("Native recompilation module for Ace Combat 6, powered by Xenon Recomp. Users provide their own legally obtained game content."));
  item.insert(QStringLiteral("repository"), QStringLiteral("nimauria/Project-Gracemeria"));
  item.insert(QStringLiteral("repositoryUrl"), QStringLiteral("https://github.com/nimauria/Project-Gracemeria"));
  item.insert(QStringLiteral("publisher"), QStringLiteral("Nimauria"));
  item.insert(QStringLiteral("license"), QStringLiteral("MIT"));
  item.insert(QStringLiteral("verified"), true);
  item.insert(QStringLiteral("tags"), QVariantList{QStringLiteral("Ace Combat 6"), QStringLiteral("Game Module")});
  item.insert(QStringLiteral("releaseAssets"), assets);
  item.insert(QStringLiteral("source"), QStringLiteral("Bundled official catalog fallback"));
  return item;
}

}  // namespace

GitHubModuleCatalogProvider::GitHubModuleCatalogProvider(QObject* parent) : QObject(parent) {
  entries_ = fallbackEntries();
  state_.insert(QStringLiteral("status"), QStringLiteral("fallback"));
  state_.insert(QStringLiteral("message"),
                QStringLiteral("Using the bundled official catalog until GitHub is refreshed."));
  state_.insert(QStringLiteral("sourceUrl"), catalogUrl());
  state_.insert(QStringLiteral("hostKey"), hostKey());
  state_.insert(QStringLiteral("lastCheckedAt"), QString{});
}

QString GitHubModuleCatalogProvider::catalogUrl() {
  return QStringLiteral("https://raw.githubusercontent.com/nimauria/Xenon-Recomp/main/catalog/modules.json");
}

QString GitHubModuleCatalogProvider::hostKey() {
  return QStringLiteral("%1-%2").arg(platformToken(), architectureToken());
}

QVariantList GitHubModuleCatalogProvider::fallbackEntries() {
  return {projectGracemeriaFallback()};
}

void GitHubModuleCatalogProvider::setState(const QString& status, const QString& message) {
  state_.insert(QStringLiteral("status"), status);
  state_.insert(QStringLiteral("message"), message);
  state_.insert(QStringLiteral("sourceUrl"), catalogUrl());
  state_.insert(QStringLiteral("hostKey"), hostKey());
  emit changed();
}

void GitHubModuleCatalogProvider::cancel() {
  if (active_reply_) active_reply_->abort();
  active_reply_.clear();
}

void GitHubModuleCatalogProvider::refresh() {
  cancel();
  setState(QStringLiteral("loading"), QStringLiteral("Refreshing the official module catalog from GitHub…"));

  QNetworkRequest request{QUrl{catalogUrl()}};
  request.setRawHeader("Accept", "application/json");
  request.setRawHeader("User-Agent", "Xenon-Launcher-Module-Catalog");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);

  auto* reply = network_.get(request);
  active_reply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    const auto cleanup = qScopeGuard([this, reply]() {
      if (active_reply_ == reply) active_reply_.clear();
      reply->deleteLater();
    });

    state_.insert(QStringLiteral("lastCheckedAt"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    if (reply->error() != QNetworkReply::NoError) {
      setState(QStringLiteral("fallback"),
               QStringLiteral("GitHub catalog refresh failed: %1. The bundled catalog remains available.")
                   .arg(reply->errorString()));
      emit refreshFailed(state_.value(QStringLiteral("message")).toString());
      return;
    }

    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
      setState(QStringLiteral("fallback"),
               QStringLiteral("GitHub returned HTTP %1 for the module catalog. The bundled catalog remains available.")
                   .arg(status));
      emit refreshFailed(state_.value(QStringLiteral("message")).toString());
      return;
    }

    QString error;
    const auto parsed = parse(reply->readAll(), &error);
    if (!error.isEmpty() || parsed.isEmpty()) {
      setState(QStringLiteral("fallback"),
               error.isEmpty() ? QStringLiteral("The GitHub module catalog was empty. The bundled catalog remains available.")
                               : error + QStringLiteral(" The bundled catalog remains available."));
      emit refreshFailed(state_.value(QStringLiteral("message")).toString());
      return;
    }

    entries_ = parsed;
    setState(QStringLiteral("ready"),
             entries_.size() == 1
                 ? QStringLiteral("Loaded 1 official module catalog entry from GitHub.")
                 : QStringLiteral("Loaded %1 official module catalog entries from GitHub.").arg(entries_.size()));
    emit refreshed();
  });
}

QVariantList GitHubModuleCatalogProvider::parse(const QByteArray& payload, QString* error) const {
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(payload, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error) *error = QStringLiteral("GitHub returned malformed module catalog JSON.");
    return {};
  }

  const auto object = document.object();
  if (object.value(QStringLiteral("schema")).toString() != QStringLiteral("xenon.module-catalog") ||
      object.value(QStringLiteral("version")).toInt() != 1) {
    if (error) *error = QStringLiteral("The module catalog schema is not supported by this Xenon Launcher build.");
    return {};
  }

  QVariantList result;
  QSet<QString> ids;
  for (const auto& value : object.value(QStringLiteral("modules")).toArray()) {
    const auto normalized = normalizeEntry(value.toObject().toVariantMap());
    const auto id = normalized.value(QStringLiteral("moduleId")).toString();
    if (normalized.isEmpty() || ids.contains(id)) continue;
    ids.insert(id);
    result.append(normalized);
  }
  return result;
}

QVariantMap GitHubModuleCatalogProvider::normalizeEntry(const QVariantMap& source) const {
  const auto id = source.value(QStringLiteral("id"), source.value(QStringLiteral("moduleId"))).toString().trimmed();
  const auto repository = source.value(QStringLiteral("repository")).toString().trimmed();
  if (!safeModuleId(id) || !safeRepository(repository)) return {};

  const auto assets = source.value(QStringLiteral("releaseAssets")).toMap();
  auto asset_name = assets.value(hostKey()).toString().trimmed();
  if (asset_name.isEmpty()) asset_name = assets.value(QStringLiteral("all")).toString().trimmed();

  QVariantMap item;
  item.insert(QStringLiteral("moduleId"), id);
  item.insert(QStringLiteral("moduleName"), source.value(QStringLiteral("name"), id).toString());
  item.insert(QStringLiteral("moduleType"), source.value(QStringLiteral("type"), QStringLiteral("Module")).toString());
  item.insert(QStringLiteral("description"), source.value(QStringLiteral("description")).toString());
  item.insert(QStringLiteral("repository"), repository);
  item.insert(QStringLiteral("repositoryUrl"), QStringLiteral("https://github.com/") + repository);
  item.insert(QStringLiteral("publisher"), source.value(QStringLiteral("publisher")).toString());
  item.insert(QStringLiteral("license"), source.value(QStringLiteral("license")).toString());
  item.insert(QStringLiteral("verified"), source.value(QStringLiteral("verified"), false).toBool());
  item.insert(QStringLiteral("tags"), stringList(source.value(QStringLiteral("tags"))));
  item.insert(QStringLiteral("releaseAssets"), assets);
  item.insert(QStringLiteral("assetName"), asset_name);
  item.insert(QStringLiteral("packageSupported"), !asset_name.isEmpty());
  item.insert(QStringLiteral("source"), QStringLiteral("Official GitHub catalog"));
  return item;
}

}  // namespace xenon::launcher::frontend_backend
