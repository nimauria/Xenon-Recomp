#include "github_module_catalog_provider.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
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
  static const QRegularExpression pattern{QStringLiteral("^[a-z0-9]+(?:[._-][a-z0-9]+)+$")};
  return pattern.match(value.trimmed()).hasMatch();
}

bool safeRepository(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")};
  return pattern.match(value.trimmed()).hasMatch();
}

bool safeReleaseAssetName(const QString& value) {
  const auto name = value.trimmed();
  if (name.isEmpty() || name.size() > 200 || name.contains(QLatin1Char('/')) ||
      name.contains(QLatin1Char('\\'))) {
    return false;
  }
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$")};
  return pattern.match(name).hasMatch();
}

QStringList stringList(const QVariant& value);

bool safeRegistryAssetPath(const QString& value) {
  const auto path = value.trimmed();
  if (path.isEmpty() || path.size() > 240 || path.startsWith(QLatin1Char('/')) ||
      path.contains(QStringLiteral("..")) || path.contains(QLatin1Char('\\'))) {
    return false;
  }
  static const QRegularExpression pattern{
      QStringLiteral("^assets/[A-Za-z0-9._-]+/[A-Za-z0-9][A-Za-z0-9._/-]*\\.(?:png|jpg|jpeg|webp|gif|bmp)$"),
      QRegularExpression::CaseInsensitiveOption};
  return pattern.match(path).hasMatch();
}

QVariantMap normalizedLauncherMetadata(const QVariantMap& source) {
  const auto raw = source.value(QStringLiteral("launcher")).toMap();
  QVariantMap launcher;
  for (const auto& key : {QStringLiteral("gameTitle"), QStringLiteral("moduleName"),
                          QStringLiteral("description"), QStringLiteral("renderer"),
                          QStringLiteral("mode"), QStringLiteral("contentStateLabel")}) {
    const auto value = raw.value(key).toString().trimmed();
    if (!value.isEmpty()) launcher.insert(key, value);
  }
  launcher.insert(QStringLiteral("showDlcCatalog"),
                  raw.value(QStringLiteral("showDlcCatalog"), true).toBool());
  launcher.insert(QStringLiteral("regions"), stringList(raw.value(QStringLiteral("regions"))));
  launcher.insert(QStringLiteral("compatibility"), raw.value(QStringLiteral("compatibility")).toMap());

  for (const auto& pair : {qMakePair(QStringLiteral("tileArt"), QStringLiteral("tileArtUrl")),
                           qMakePair(QStringLiteral("heroArt"), QStringLiteral("heroArtUrl"))}) {
    const auto path = raw.value(pair.first).toString().trimmed();
    if (!safeRegistryAssetPath(path)) continue;
    launcher.insert(pair.first, path);
    launcher.insert(pair.second,
                    QStringLiteral("https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/") + path);
  }
  return launcher;
}

QVariantMap normalizedGameMetadata(const QVariantMap& source) {
  const auto raw = source.value(QStringLiteral("game")).toMap();
  QVariantMap game;
  for (const auto& key : {QStringLiteral("title"), QStringLiteral("titleId"),
                          QStringLiteral("developer"), QStringLiteral("publisher"),
                          QStringLiteral("platform")}) {
    const auto value = raw.value(key).toString().trimmed();
    if (!value.isEmpty()) game.insert(key, value);
  }
  if (raw.contains(QStringLiteral("releaseYear")))
    game.insert(QStringLiteral("releaseYear"), raw.value(QStringLiteral("releaseYear")).toInt());
  game.insert(QStringLiteral("genres"), stringList(raw.value(QStringLiteral("genres"))));
  game.insert(QStringLiteral("supportedRegions"),
              stringList(raw.value(QStringLiteral("supportedRegions"))));
  return game;
}

QVariantList normalizedDlc(const QVariantMap& source) {
  QVariantList result;
  QSet<QString> ids;
  const auto raw = source.value(QStringLiteral("dlc")).toList();
  if (raw.size() > 2048) return result;
  for (const auto& value : raw) {
    const auto item = value.toMap();
    const auto id = item.value(QStringLiteral("id")).toString().trimmed();
    const auto name = item.value(QStringLiteral("name")).toString().trimmed();
    if (id.isEmpty() || name.isEmpty() || ids.contains(id)) continue;
    static const QRegularExpression id_pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")};
    if (!id_pattern.match(id).hasMatch()) continue;
    ids.insert(id);

    QVariantMap normalized;
    normalized.insert(QStringLiteral("dlcId"), id);
    normalized.insert(QStringLiteral("name"), name);
    normalized.insert(QStringLiteral("description"), item.value(QStringLiteral("description")).toString());
    normalized.insert(QStringLiteral("category"), item.value(QStringLiteral("category")).toString());
    normalized.insert(QStringLiteral("group"), item.value(QStringLiteral("group")).toString());
    normalized.insert(QStringLiteral("optional"), item.value(QStringLiteral("optional"), true).toBool());
    normalized.insert(QStringLiteral("contentIds"), item.value(QStringLiteral("contentIds")).toList());
    normalized.insert(QStringLiteral("identityStatus"), item.value(QStringLiteral("identityStatus")).toString());
    normalized.insert(QStringLiteral("historical"), item.value(QStringLiteral("historical")).toMap());
    normalized.insert(QStringLiteral("metadata"), item);
    result.append(normalized);
  }
  return result;
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
  assets.insert(QStringLiteral("windows-x64"),
                QStringLiteral("project-gracemeria-windows-x64.xenonmod.zip"));

  QVariantMap item;
  item.insert(QStringLiteral("moduleId"), QStringLiteral("org.nimauria.project-gracemeria"));
  item.insert(QStringLiteral("moduleName"), QStringLiteral("Project Gracemeria"));
  item.insert(QStringLiteral("moduleType"), QStringLiteral("game"));
  item.insert(QStringLiteral("description"),
              QStringLiteral("Xenon module for Ace Combat 6: Fires of Liberation."));
  item.insert(QStringLiteral("repository"), QStringLiteral("nimauria/Project-Gracemeria"));
  item.insert(QStringLiteral("repositoryUrl"),
              QStringLiteral("https://github.com/nimauria/Project-Gracemeria"));
  item.insert(QStringLiteral("publisher"), QStringLiteral("Nimauria"));
  item.insert(QStringLiteral("verified"), true);
  item.insert(QStringLiteral("tags"),
              QVariantList{QStringLiteral("game"), QStringLiteral("ace-combat"),
                           QStringLiteral("flight")});
  item.insert(QStringLiteral("releaseAssets"), assets);
  item.insert(QStringLiteral("entryPath"),
              QStringLiteral("modules/org.nimauria.project-gracemeria.json"));
  item.insert(QStringLiteral("registryEntryUrl"),
              QStringLiteral("https://github.com/nimauria/Xenon-Modules/blob/main/modules/org.nimauria.project-gracemeria.json"));
  item.insert(QStringLiteral("source"), QStringLiteral("Bundled Xenon Modules registry fallback"));

  QVariantMap launcher;
  launcher.insert(QStringLiteral("gameTitle"), QStringLiteral("Ace Combat 6: Fires of Liberation"));
  launcher.insert(QStringLiteral("moduleName"), QStringLiteral("Project Gracemeria"));
  launcher.insert(QStringLiteral("description"),
                  QStringLiteral("Take to the skies of Strangereal in Ace Combat 6: Fires of Liberation through Project Gracemeria and the Xenon native recompilation runtime."));
  launcher.insert(QStringLiteral("renderer"), QStringLiteral("Automatic"));
  launcher.insert(QStringLiteral("mode"), QStringLiteral("Offline"));
  launcher.insert(QStringLiteral("regions"), QVariantList{QStringLiteral("NTSC-U"), QStringLiteral("PAL")});
  launcher.insert(QStringLiteral("contentStateLabel"), QStringLiteral("User-provided Xbox 360 content required"));
  launcher.insert(QStringLiteral("showDlcCatalog"), true);
  launcher.insert(QStringLiteral("tileArt"), QStringLiteral("assets/org.nimauria.project-gracemeria/game-art.webp"));
  launcher.insert(QStringLiteral("heroArt"), QStringLiteral("assets/org.nimauria.project-gracemeria/background.webp"));
  launcher.insert(QStringLiteral("tileArtUrl"), QStringLiteral("https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/assets/org.nimauria.project-gracemeria/game-art.webp"));
  launcher.insert(QStringLiteral("heroArtUrl"), QStringLiteral("https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/assets/org.nimauria.project-gracemeria/background.webp"));
  item.insert(QStringLiteral("launcher"), launcher);

  QVariantMap game;
  game.insert(QStringLiteral("title"), QStringLiteral("Ace Combat 6: Fires of Liberation"));
  game.insert(QStringLiteral("titleId"), QStringLiteral("4E4D07D1"));
  game.insert(QStringLiteral("developer"), QStringLiteral("Project Aces"));
  game.insert(QStringLiteral("publisher"), QStringLiteral("Bandai Namco Games"));
  game.insert(QStringLiteral("platform"), QStringLiteral("Xbox 360"));
  game.insert(QStringLiteral("releaseYear"), 2007);
  game.insert(QStringLiteral("genres"), QVariantList{QStringLiteral("combat flight simulation"), QStringLiteral("action")});
  game.insert(QStringLiteral("supportedRegions"), QVariantList{QStringLiteral("NTSC-U"), QStringLiteral("PAL")});
  item.insert(QStringLiteral("game"), game);
  item.insert(QStringLiteral("capabilities"), QVariantList{QStringLiteral("game-identification"),
                                                             QStringLiteral("dlc-catalog"),
                                                             QStringLiteral("save-data"),
                                                             QStringLiteral("offline-launch"),
                                                             QStringLiteral("module-settings")});
  return item;
}

}  // namespace

GitHubModuleCatalogProvider::GitHubModuleCatalogProvider(QObject* parent) : QObject(parent) {
  entries_ = fallbackEntries();
  state_.insert(QStringLiteral("status"), QStringLiteral("fallback"));
  state_.insert(QStringLiteral("message"),
                QStringLiteral("Using the bundled registry fallback until Xenon Modules is refreshed."));
  state_.insert(QStringLiteral("sourceUrl"), catalogUrl());
  state_.insert(QStringLiteral("registryUrl"), registryUrl());
  state_.insert(QStringLiteral("hostKey"), hostKey());
  state_.insert(QStringLiteral("lastCheckedAt"), QString{});
  state_.insert(QStringLiteral("entryCount"), entries_.size());
  state_.insert(QStringLiteral("failedEntryCount"), 0);
}

QString GitHubModuleCatalogProvider::catalogUrl() {
  return QStringLiteral("https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/catalog.json");
}

QString GitHubModuleCatalogProvider::registryUrl() {
  return QStringLiteral("https://github.com/nimauria/Xenon-Modules");
}

QString GitHubModuleCatalogProvider::registryRawBaseUrl() {
  return QStringLiteral("https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/");
}

QString GitHubModuleCatalogProvider::hostKey() {
  return QStringLiteral("%1-%2").arg(platformToken(), architectureToken());
}

QVariantList GitHubModuleCatalogProvider::fallbackEntries() {
  auto fallback = projectGracemeriaFallback();
  const auto assets = fallback.value(QStringLiteral("releaseAssets")).toMap();
  auto asset_name = assets.value(hostKey()).toString().trimmed();
  if (asset_name.isEmpty()) asset_name = assets.value(QStringLiteral("all")).toString().trimmed();
  fallback.insert(QStringLiteral("hostKey"), hostKey());
  fallback.insert(QStringLiteral("assetName"), asset_name);
  fallback.insert(QStringLiteral("packageSupported"), !asset_name.isEmpty());
  return {fallback};
}

bool GitHubModuleCatalogProvider::safeEntryPath(const QString& path) {
  const auto trimmed = path.trimmed();
  if (trimmed.isEmpty() || trimmed.size() > 240 || trimmed.startsWith(QLatin1Char('/')) ||
      trimmed.contains(QStringLiteral("..")) || trimmed.contains(QLatin1Char('\\'))) {
    return false;
  }
  static const QRegularExpression pattern{
      QStringLiteral("^modules/[A-Za-z0-9][A-Za-z0-9._-]{0,127}\\.json$")};
  return pattern.match(trimmed).hasMatch();
}

void GitHubModuleCatalogProvider::setState(const QString& status, const QString& message) {
  state_.insert(QStringLiteral("status"), status);
  state_.insert(QStringLiteral("message"), message);
  state_.insert(QStringLiteral("sourceUrl"), catalogUrl());
  state_.insert(QStringLiteral("registryUrl"), registryUrl());
  state_.insert(QStringLiteral("hostKey"), hostKey());
  state_.insert(QStringLiteral("entryCount"), entries_.size());
  state_.insert(QStringLiteral("failedEntryCount"), loading_errors_.size());
  emit changed();
}

void GitHubModuleCatalogProvider::cancel() {
  ++refresh_generation_;
  if (active_catalog_reply_) active_catalog_reply_->abort();
  active_catalog_reply_.clear();
  for (const auto& reply : active_entry_replies_) {
    if (reply) reply->abort();
  }
  active_entry_replies_.clear();
  pending_entry_count_ = 0;
  expected_entry_count_ = 0;
  loading_entries_.clear();
  loading_errors_.clear();
}

void GitHubModuleCatalogProvider::refresh() {
  cancel();
  const auto generation = ++refresh_generation_;
  loading_errors_.clear();
  setState(QStringLiteral("loading"),
           QStringLiteral("Refreshing the official Xenon Modules registry from GitHub…"));

  QNetworkRequest request{QUrl{catalogUrl()}};
  request.setRawHeader("Accept", "application/json");
  request.setRawHeader("User-Agent", "Xenon-Launcher-Module-Catalog");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);

  auto* reply = network_.get(request);
  active_catalog_reply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
    const auto cleanup = qScopeGuard([this, reply]() {
      if (active_catalog_reply_ == reply) active_catalog_reply_.clear();
      reply->deleteLater();
    });
    if (generation != refresh_generation_) return;

    state_.insert(QStringLiteral("lastCheckedAt"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    if (reply->error() != QNetworkReply::NoError) {
      entries_ = fallbackEntries();
      setState(QStringLiteral("fallback"),
               QStringLiteral("Xenon Modules registry refresh failed: %1. The bundled fallback remains available.")
                   .arg(reply->errorString()));
      emit refreshFailed(state_.value(QStringLiteral("message")).toString());
      return;
    }

    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
      entries_ = fallbackEntries();
      setState(QStringLiteral("fallback"),
               QStringLiteral("GitHub returned HTTP %1 for the Xenon Modules registry. The bundled fallback remains available.")
                   .arg(status));
      emit refreshFailed(state_.value(QStringLiteral("message")).toString());
      return;
    }

    QString error;
    const auto paths = parseCatalog(reply->readAll(), &error);
    if (!error.isEmpty()) {
      entries_ = fallbackEntries();
      setState(QStringLiteral("fallback"),
               error + QStringLiteral(" The bundled fallback remains available."));
      emit refreshFailed(state_.value(QStringLiteral("message")).toString());
      return;
    }

    if (paths.isEmpty()) {
      entries_.clear();
      loading_errors_.clear();
      state_.insert(QStringLiteral("entryCount"), 0);
      state_.insert(QStringLiteral("failedEntryCount"), 0);
      setState(QStringLiteral("ready"),
               QStringLiteral("The official Xenon Modules registry is valid and currently contains no modules."));
      emit refreshed();
      return;
    }

    fetchEntries(paths, generation);
  });
}

QStringList GitHubModuleCatalogProvider::parseCatalog(const QByteArray& payload, QString* error) const {
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(payload, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error) *error = QStringLiteral("GitHub returned malformed Xenon Modules catalog JSON.");
    return {};
  }

  const auto object = document.object();
  if (object.value(QStringLiteral("schema")).toString() != QStringLiteral("xenon.module-catalog") ||
      object.value(QStringLiteral("version")).toInt() != 1) {
    if (error) *error = QStringLiteral("The Xenon Modules catalog schema is not supported by this launcher build.");
    return {};
  }

  const auto modules = object.value(QStringLiteral("modules"));
  if (!modules.isArray()) {
    if (error) *error = QStringLiteral("The Xenon Modules catalog does not contain a valid modules array.");
    return {};
  }

  QStringList paths;
  QSet<QString> seen;
  const auto array = modules.toArray();
  if (array.size() > 256) {
    if (error) *error = QStringLiteral("The Xenon Modules catalog contains more entries than this launcher accepts in one registry document.");
    return {};
  }
  for (const auto& value : array) {
    if (!value.isString()) {
      if (error) *error = QStringLiteral("The Xenon Modules catalog contains an entry that is not a repository-relative path.");
      return {};
    }
    const auto path = value.toString().trimmed();
    if (!safeEntryPath(path)) {
      if (error) *error = QStringLiteral("The Xenon Modules catalog contains an unsafe module entry path.");
      return {};
    }
    if (seen.contains(path)) {
      if (error) *error = QStringLiteral("The Xenon Modules catalog contains a duplicate module entry path.");
      return {};
    }
    seen.insert(path);
    paths.append(path);
  }
  return paths;
}

void GitHubModuleCatalogProvider::fetchEntries(const QStringList& paths, quint64 generation) {
  expected_entry_count_ = paths.size();
  pending_entry_count_ = paths.size();
  loading_entries_ = QVariantList(paths.size(), QVariant{});
  loading_errors_.clear();
  active_entry_replies_.clear();
  state_.insert(QStringLiteral("expectedEntryCount"), expected_entry_count_);
  state_.insert(QStringLiteral("loadedEntryCount"), 0);
  setState(QStringLiteral("loading-entries"),
           QStringLiteral("Loading %1 module registry entr%2…")
               .arg(paths.size())
               .arg(paths.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies")));

  for (qsizetype index = 0; index < paths.size(); ++index) {
    const auto path = paths.at(index);
    QNetworkRequest request{QUrl{registryRawBaseUrl() + path}};
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "Xenon-Launcher-Module-Catalog");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = network_.get(request);
    active_entry_replies_.append(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, index, path]() {
      const auto cleanup = qScopeGuard([this, reply]() {
        active_entry_replies_.removeAll(reply);
        reply->deleteLater();
      });
      if (generation != refresh_generation_) return;

      QString error;
      QVariantMap entry;
      if (reply->error() != QNetworkReply::NoError) {
        error = QStringLiteral("%1: %2").arg(path, reply->errorString());
      } else {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300) {
          error = QStringLiteral("%1: GitHub returned HTTP %2").arg(path).arg(status);
        } else {
          entry = parseEntry(reply->readAll(), path, &error);
        }
      }

      if (!error.isEmpty() || entry.isEmpty()) {
        loading_errors_.append(error.isEmpty() ? path + QStringLiteral(": invalid entry") : error);
      } else {
        loading_entries_[index] = entry;
      }

      --pending_entry_count_;
      int loaded = 0;
      for (const auto& value : loading_entries_) {
        if (!value.toMap().isEmpty()) ++loaded;
      }
      state_.insert(QStringLiteral("loadedEntryCount"), loaded);
      state_.insert(QStringLiteral("failedEntryCount"), loading_errors_.size());
      emit changed();
      if (pending_entry_count_ == 0) finishEntryRefresh(generation);
    });
  }
}

void GitHubModuleCatalogProvider::finishEntryRefresh(quint64 generation) {
  if (generation != refresh_generation_) return;

  QVariantList parsed;
  QSet<QString> ids;
  for (const auto& value : loading_entries_) {
    const auto entry = value.toMap();
    if (entry.isEmpty()) continue;
    const auto id = entry.value(QStringLiteral("moduleId")).toString();
    if (ids.contains(id)) {
      loading_errors_.append(QStringLiteral("Duplicate module ID in registry: %1").arg(id));
      continue;
    }
    ids.insert(id);
    parsed.append(entry);
  }

  if (parsed.isEmpty()) {
    entries_ = fallbackEntries();
    setState(QStringLiteral("fallback"),
             QStringLiteral("The Xenon Modules catalog was reachable, but no module entries could be validated. The bundled fallback remains available."));
    emit refreshFailed(state_.value(QStringLiteral("message")).toString());
    return;
  }

  entries_ = parsed;
  state_.insert(QStringLiteral("entryCount"), entries_.size());
  state_.insert(QStringLiteral("failedEntryCount"), loading_errors_.size());
  if (loading_errors_.isEmpty()) {
    setState(QStringLiteral("ready"),
             entries_.size() == 1
                 ? QStringLiteral("Loaded 1 module from the official Xenon Modules registry.")
                 : QStringLiteral("Loaded %1 modules from the official Xenon Modules registry.").arg(entries_.size()));
  } else {
    setState(QStringLiteral("partial"),
             QStringLiteral("Loaded %1 of %2 Xenon Modules registry entries; %3 entr%4 failed validation or download.")
                 .arg(entries_.size())
                 .arg(expected_entry_count_)
                 .arg(loading_errors_.size())
                 .arg(loading_errors_.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
  }
  emit refreshed();
}

QVariantMap GitHubModuleCatalogProvider::parseEntry(const QByteArray& payload,
                                                     const QString& entry_path,
                                                     QString* error) const {
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(payload, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error) *error = QStringLiteral("%1: malformed module entry JSON").arg(entry_path);
    return {};
  }

  const auto source = document.object().toVariantMap();
  if (source.value(QStringLiteral("schema")).toString() != QStringLiteral("xenon.module-entry") ||
      source.value(QStringLiteral("version")).toInt() != 1) {
    if (error) *error = QStringLiteral("%1: unsupported module-entry schema").arg(entry_path);
    return {};
  }
  const auto normalized = normalizeEntry(source, entry_path);
  if (normalized.isEmpty() && error) {
    *error = QStringLiteral("%1: module entry failed Xenon registry validation").arg(entry_path);
  }
  return normalized;
}

QVariantMap GitHubModuleCatalogProvider::normalizeEntry(const QVariantMap& source,
                                                         const QString& entry_path) const {
  const auto id = source.value(QStringLiteral("id")).toString().trimmed();
  if (!safeModuleId(id)) return {};

  const auto display_name = source.value(QStringLiteral("name")).toString().trimmed();
  const auto module_type = source.value(QStringLiteral("type")).toString().trimmed();
  const auto description = source.value(QStringLiteral("description")).toString().trimmed();
  if (display_name.isEmpty() || display_name.size() > 100 ||
      module_type != QStringLiteral("game") || description.isEmpty() || description.size() > 500) {
    return {};
  }

  const auto repository_object = source.value(QStringLiteral("repository")).toMap();
  if (repository_object.value(QStringLiteral("provider")).toString().trimmed().toLower() !=
      QStringLiteral("github")) {
    return {};
  }
  const auto owner = repository_object.value(QStringLiteral("owner")).toString().trimmed();
  const auto name = repository_object.value(QStringLiteral("name")).toString().trimmed();
  const auto repository = owner + QLatin1Char('/') + name;
  if (!safeRepository(repository)) return {};

  const auto publisher_object = source.value(QStringLiteral("publisher")).toMap();
  const auto publisher = publisher_object.value(QStringLiteral("name")).toString().trimmed();
  if (publisher.isEmpty() || publisher.size() > 100 ||
      publisher_object.value(QStringLiteral("verified")).metaType().id() != QMetaType::Bool) {
    return {};
  }

  const auto assets = source.value(QStringLiteral("releaseAssets")).toMap();
  if (assets.isEmpty()) return {};
  static const QRegularExpression host_key_pattern{
      QStringLiteral("^[a-z0-9]+-[a-z0-9_]+$")};
  for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
    if (!host_key_pattern.match(it.key()).hasMatch() || !safeReleaseAssetName(it.value().toString())) {
      return {};
    }
  }
  const auto asset_name = assets.value(hostKey()).toString().trimmed();

  QVariantMap item;
  item.insert(QStringLiteral("moduleId"), id);
  item.insert(QStringLiteral("moduleName"), display_name);
  item.insert(QStringLiteral("moduleType"), module_type);
  item.insert(QStringLiteral("description"), description);
  item.insert(QStringLiteral("repository"), repository);
  item.insert(QStringLiteral("repositoryUrl"), QStringLiteral("https://github.com/") + repository);
  item.insert(QStringLiteral("repositoryDefaultBranch"),
              repository_object.value(QStringLiteral("defaultBranch"), QStringLiteral("main")).toString());
  item.insert(QStringLiteral("publisher"), publisher);
  item.insert(QStringLiteral("verified"), publisher_object.value(QStringLiteral("verified"), false).toBool());
  item.insert(QStringLiteral("license"), source.value(QStringLiteral("license")).toString());
  item.insert(QStringLiteral("tags"), stringList(source.value(QStringLiteral("tags"))));
  item.insert(QStringLiteral("releaseAssets"), assets);
  item.insert(QStringLiteral("hostKey"), hostKey());
  item.insert(QStringLiteral("assetName"), asset_name);
  item.insert(QStringLiteral("packageSupported"), !asset_name.isEmpty());
  item.insert(QStringLiteral("entryPath"), entry_path);
  item.insert(QStringLiteral("registryEntryUrl"),
              QStringLiteral("https://github.com/nimauria/Xenon-Modules/blob/main/") + entry_path);
  item.insert(QStringLiteral("registryRawUrl"), registryRawBaseUrl() + entry_path);
  item.insert(QStringLiteral("source"), QStringLiteral("Official Xenon Modules registry"));

  item.insert(QStringLiteral("launcher"), normalizedLauncherMetadata(source));
  item.insert(QStringLiteral("game"), normalizedGameMetadata(source));
  item.insert(QStringLiteral("capabilities"), stringList(source.value(QStringLiteral("capabilities"))));
  item.insert(QStringLiteral("dlc"), normalizedDlc(source));
  item.insert(QStringLiteral("historicalUnreleased"),
              source.value(QStringLiteral("historicalUnreleased")).toList());
  item.insert(QStringLiteral("sources"), source.value(QStringLiteral("sources")).toList());
  return item;
}

}  // namespace xenon::launcher::frontend_backend
