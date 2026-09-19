#include "module_service.hpp"

#include "path_service.hpp"
#include "settings_service.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaType>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>

#include <initializer_list>

namespace xenon::launcher {
namespace {
QString firstString(const QVariantMap& map, std::initializer_list<const char*> keys,
                    const QString& fallback = {}) {
  for (const auto* key : keys) {
    const auto value = map.value(QString::fromLatin1(key));
    if (value.isValid() && !value.toString().trimmed().isEmpty()) return value.toString();
  }
  return fallback;
}

QString joinedValue(const QVariant& value, const QString& fallback = {}) {
  if (!value.isValid() || value.isNull()) return fallback;
  if (value.metaType().id() == QMetaType::QStringList) return value.toStringList().join(QStringLiteral(" • "));
  const auto list = value.toList();
  if (!list.isEmpty()) {
    QStringList strings;
    for (const auto& item : list) strings.append(item.toString());
    return strings.join(QStringLiteral(" • "));
  }
  return value.toString();
}

bool safeModuleId(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")};
  const auto trimmed = value.trimmed();
  return pattern.match(trimmed).hasMatch() && trimmed != QStringLiteral(".") &&
         trimmed != QStringLiteral("..") && !trimmed.contains(QStringLiteral(".."));
}

bool safeSettingId(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")};
  return pattern.match(value.trimmed()).hasMatch();
}


QVariantList manifestDlcDefinitions(const QVariantMap& manifest) {
  QVariant raw = manifest.value(QStringLiteral("dlc"));
  if (raw.toList().isEmpty()) raw = manifest.value(QStringLiteral("addOns"));
  if (raw.toList().isEmpty()) raw = manifest.value(QStringLiteral("addons"));
  if (raw.toList().isEmpty()) {
    const auto launcher = manifest.value(QStringLiteral("launcher")).toMap();
    raw = launcher.value(QStringLiteral("dlc"));
    if (raw.toList().isEmpty()) raw = launcher.value(QStringLiteral("addOns"));
  }
  if (raw.toList().isEmpty()) {
    const auto content = manifest.value(QStringLiteral("content")).toMap();
    raw = content.value(QStringLiteral("dlc"));
    if (raw.toList().isEmpty()) raw = content.value(QStringLiteral("addOns"));
  }
  return raw.toList();
}

bool safeDlcId(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")};
  return pattern.match(value.trimmed()).hasMatch();
}

QString generatedDlcId(const QString& name, int index) {
  auto id = name.trimmed().toLower();
  id.replace(QRegularExpression{QStringLiteral("[^a-z0-9._-]+")}, QStringLiteral("-"));
  id.replace(QRegularExpression{QStringLiteral("^-+|-+$")}, QString{});
  if (id.isEmpty()) id = QStringLiteral("dlc-%1").arg(index + 1);
  if (id.size() > 120) id = id.left(120);
  return id;
}

QVariantList manifestSettings(const QVariantMap& manifest) {
  auto raw = manifest.value(QStringLiteral("settings")).toList();
  if (raw.isEmpty()) raw = manifest.value(QStringLiteral("launcherSettings")).toList();
  return raw;
}
}

ModuleService::ModuleService(SettingsService& settings, PathService& paths, QObject* parent)
    : QObject(parent), settings_(settings), paths_(paths) {}

QVariantList ModuleService::modules() const {
  return modules_;
}

QVariantMap ModuleService::module(const QString& module_id) const {
  for (const auto& value : modules_) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("moduleId")).toString() == module_id) return item;
  }
  return {};
}

QVariantMap ModuleService::manifest(const QString& module_id) const {
  const auto item = module(module_id);
  if (item.isEmpty()) return {};
  return readManifest(item.value(QStringLiteral("path")).toString());
}

QVariantList ModuleService::dlcCatalog(const QString& module_id) const {
  const auto raw_manifest = manifest(module_id);
  if (raw_manifest.isEmpty()) return {};

  QVariantList result;
  QSet<QString> ids;
  int index = 0;
  for (const auto& value : manifestDlcDefinitions(raw_manifest)) {
    const auto source = value.toMap();
    auto name = firstString(source, {"name", "title", "displayName"});
    auto id = firstString(source, {"id", "dlcId", "contentId", "content_id"});
    if (name.trimmed().isEmpty() && id.trimmed().isEmpty()) {
      ++index;
      continue;
    }
    if (name.trimmed().isEmpty()) name = id;
    if (!safeDlcId(id)) id = generatedDlcId(name, index);
    auto unique_id = id;
    int suffix = 2;
    while (ids.contains(unique_id)) unique_id = QStringLiteral("%1-%2").arg(id).arg(suffix++);
    ids.insert(unique_id);

    QVariantMap item;
    item.insert(QStringLiteral("dlcId"), unique_id);
    item.insert(QStringLiteral("name"), name.trimmed());
    item.insert(QStringLiteral("description"), firstString(source, {"description", "summary"}));
    item.insert(QStringLiteral("version"), firstString(source, {"version"}));
    item.insert(QStringLiteral("optional"), source.value(QStringLiteral("optional"), true).toBool());
    QVariant content_ids = source.value(QStringLiteral("contentIds"));
    if (!content_ids.isValid() || content_ids.isNull()) {
      content_ids = source.value(QStringLiteral("content_ids"));
    }
    if (!content_ids.isValid() || content_ids.isNull()) {
      content_ids = source.value(QStringLiteral("contentId"));
    }
    if (!content_ids.isValid() || content_ids.isNull()) {
      content_ids = source.value(QStringLiteral("content_id"));
    }
    item.insert(QStringLiteral("contentIds"), content_ids);
    item.insert(QStringLiteral("metadata"), source);
    result.append(item);
    ++index;
  }
  return result;
}

ServiceResult ModuleService::refresh() {
  modules_.clear();
  const auto root = paths_.configuredPath(QStringLiteral("modules"));
  if (!paths_.ensureDirectory(root)) {
    return ServiceResult::failure(QStringLiteral("Module discovery"),
                                  QStringLiteral("Xenon could not access the configured modules folder."));
  }

  QDir dir{root};
  const auto directories = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const auto& directory : directories) {
    const auto manifest = readManifest(directory.absoluteFilePath());
    if (manifest.isEmpty()) continue;
    const auto item = manifestToUi(manifest, directory.absoluteFilePath());
    if (!safeModuleId(item.value(QStringLiteral("moduleId")).toString())) continue;
    modules_.append(item);
  }
  emit changed();
  return ServiceResult::success(QStringLiteral("Modules refreshed"),
                                QStringLiteral("Found %1 installed Xenon module(s).").arg(modules_.size()));
}

ServiceResult ModuleService::setEnabled(const QString& module_id, bool enabled) {
  const auto item = module(module_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module state"),
                                  QStringLiteral("The selected module is no longer installed."));
  }
  settings_.setValue(QStringLiteral("modules/enabled/") + module_id, enabled);
  (void)refresh();
  return ServiceResult::success(enabled ? QStringLiteral("Module enabled") : QStringLiteral("Module disabled"),
                                QStringLiteral("%1 is now %2.")
                                    .arg(item.value(QStringLiteral("moduleName")).toString(),
                                         enabled ? QStringLiteral("enabled") : QStringLiteral("disabled")));
}

ServiceResult ModuleService::remove(const QString& module_id) {
  const auto item = module(module_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module removal"),
                                  QStringLiteral("The selected module is no longer installed."));
  }

  const auto root = QDir::cleanPath(QDir{paths_.configuredPath(QStringLiteral("modules"))}.absolutePath());
  const auto expected = QDir::cleanPath(QDir{root}.filePath(module_id));
  const auto actual = QDir::cleanPath(QFileInfo{item.value(QStringLiteral("path")).toString()}.absoluteFilePath());
  if (actual != expected || !QFileInfo{actual}.isDir()) {
    return ServiceResult::failure(QStringLiteral("Module removal"),
                                  QStringLiteral("Xenon refused to remove a module outside its managed modules directory."));
  }

  if (!QDir{actual}.removeRecursively()) {
    return ServiceResult::failure(QStringLiteral("Module removal"),
                                  QStringLiteral("Xenon could not remove the module directory."));
  }
  settings_.remove(QStringLiteral("modules/enabled/") + module_id);
  (void)refresh();
  return ServiceResult::success(
      QStringLiteral("Module removed"),
      QStringLiteral("%1 was removed. Library entries that depend on it remain visible and will show that the module is missing.")
          .arg(item.value(QStringLiteral("moduleName")).toString()));
}

QVariantMap ModuleService::inspectDirectory(const QString& module_dir) const {
  return readManifest(module_dir);
}

ServiceResult ModuleService::installFromDirectory(const QString& expected_module_id,
                                                  const QString& source_directory,
                                                  bool replace_existing) {
  if (!safeModuleId(expected_module_id)) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("The expected module identifier is invalid."));
  }

  const QFileInfo source_info{source_directory};
  if (!source_info.exists() || !source_info.isDir() || source_info.isSymLink()) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("The extracted module directory is unavailable."));
  }

  const auto manifest = readManifest(source_info.absoluteFilePath());
  if (manifest.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("The extracted package does not contain a supported Xenon module manifest."));
  }
  const auto actual_id = firstString(manifest, {"id", "moduleId", "module_id"}, source_info.fileName());
  if (actual_id != expected_module_id) {
    return ServiceResult::failure(
        QStringLiteral("Module install"),
        QStringLiteral("The package identifies itself as %1 rather than the catalog module %2.")
            .arg(actual_id, expected_module_id));
  }

  const auto root = paths_.configuredPath(QStringLiteral("modules"));
  if (!paths_.ensureDirectory(root)) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("Xenon could not access the managed modules directory."));
  }

  const auto destination = QDir{root}.filePath(expected_module_id);
  const auto source = QDir::cleanPath(source_info.absoluteFilePath());
  if (QDir::cleanPath(QFileInfo{destination}.absoluteFilePath()) == source) {
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("The package source is already the installed module directory."));
  }

  QString backup;
  if (QFileInfo::exists(destination)) {
    if (!replace_existing) {
      return ServiceResult::failure(QStringLiteral("Module install"),
                                    QStringLiteral("That module is already installed."));
    }
    backup = QStringLiteral("%1.xenon-backup-%2")
                 .arg(destination, QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz")));
    if (!QDir{}.rename(destination, backup)) {
      return ServiceResult::failure(QStringLiteral("Module install"),
                                    QStringLiteral("Xenon could not create a rollback backup of the installed module."));
    }
  }

  const auto rollback = [&]() {
    if (QFileInfo::exists(destination)) QDir{destination}.removeRecursively();
    if (!backup.isEmpty() && QFileInfo::exists(backup)) (void)QDir{}.rename(backup, destination);
    (void)refresh();
  };

  if (!copyDirectory(source, destination)) {
    rollback();
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("Xenon could not copy the verified module package into place. The previous installation was restored."));
  }

  (void)refresh();
  const auto installed = module(expected_module_id);
  if (installed.isEmpty() || readManifest(destination).isEmpty()) {
    rollback();
    return ServiceResult::failure(QStringLiteral("Module install"),
                                  QStringLiteral("The replacement module failed validation. The previous installation was restored."));
  }

  if (!backup.isEmpty() && QFileInfo::exists(backup)) QDir{backup}.removeRecursively();
  return ServiceResult::success(QStringLiteral("Module installed"),
                                QStringLiteral("%1 was installed successfully.")
                                    .arg(installed.value(QStringLiteral("moduleName")).toString()));
}

ServiceResult ModuleService::verify(const QString& module_id) const {
  const auto item = module(module_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module verification"),
                                  QStringLiteral("The selected module is no longer installed."));
  }
  const auto path = item.value(QStringLiteral("path")).toString();
  if (readManifest(path).isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module verification"),
                                  QStringLiteral("The module manifest is missing or invalid."));
  }
  return ServiceResult::success(QStringLiteral("Module verified"),
                                QStringLiteral("The module manifest is readable. Runtime/API compatibility checks will run when the module ABI is connected."));
}

QString ModuleService::modulePath(const QString& module_id) const {
  return module(module_id).value(QStringLiteral("path")).toString();
}

QVariantList ModuleService::settingsSchema(const QString& module_id) const {
  const auto item = module(module_id);
  if (item.isEmpty()) return {};

  const auto manifest = readManifest(item.value(QStringLiteral("path")).toString());
  QVariantList result;
  for (const auto& value : manifestSettings(manifest)) {
    auto definition = value.toMap();
    const auto id = definition.value(QStringLiteral("id")).toString().trimmed();
    auto type = definition.value(QStringLiteral("type"), QStringLiteral("string")).toString().trimmed().toLower();
    if (!safeSettingId(id)) continue;
    if (type != QStringLiteral("bool") && type != QStringLiteral("choice") &&
        type != QStringLiteral("string")) {
      continue;
    }
    if (type == QStringLiteral("choice") && definition.value(QStringLiteral("options")).toList().isEmpty())
      continue;

    definition.insert(QStringLiteral("id"), id);
    definition.insert(QStringLiteral("type"), type);
    if (!definition.contains(QStringLiteral("label"))) definition.insert(QStringLiteral("label"), id);

    const auto default_value = definition.value(QStringLiteral("defaultValue"));
    const auto stored_key = QStringLiteral("modules/settings/%1/%2").arg(module_id, id);
    const auto current_value = settings_.value(stored_key, default_value);
    definition.insert(QStringLiteral("manifestDefaultValue"), default_value);
    definition.insert(QStringLiteral("defaultValue"), current_value);
    result.append(definition);
  }
  return result;
}

QVariantMap ModuleService::settingsValues(const QString& module_id) const {
  QVariantMap values;
  for (const auto& value : settingsSchema(module_id)) {
    const auto definition = value.toMap();
    const auto id = definition.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) {
      values.insert(id, definition.value(QStringLiteral("defaultValue")));
    }
  }
  return values;
}

ServiceResult ModuleService::setSetting(const QString& module_id, const QString& setting_id,
                                        const QVariant& value) {
  if (module(module_id).isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module setting"),
                                  QStringLiteral("The selected module is no longer installed."));
  }
  if (!safeSettingId(setting_id)) {
    return ServiceResult::failure(QStringLiteral("Module setting"),
                                  QStringLiteral("The module setting identifier is invalid."));
  }

  QVariantMap definition;
  for (const auto& candidate : settingsSchema(module_id)) {
    const auto map = candidate.toMap();
    if (map.value(QStringLiteral("id")).toString() == setting_id) {
      definition = map;
      break;
    }
  }
  if (definition.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Module setting"),
                                  QStringLiteral("The module does not declare this setting."));
  }

  const auto type = definition.value(QStringLiteral("type")).toString();
  QVariant normalized = value;
  if (type == QStringLiteral("bool")) {
    normalized = value.toBool();
  } else if (type == QStringLiteral("choice")) {
    const auto options = definition.value(QStringLiteral("options")).toList();
    bool allowed = false;
    for (const auto& option : options) {
      if (option == value || option.toString() == value.toString()) {
        normalized = option;
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      return ServiceResult::failure(QStringLiteral("Module setting"),
                                    QStringLiteral("The selected value is not allowed by the module manifest."));
    }
  } else {
    normalized = value.toString();
  }

  settings_.setValue(QStringLiteral("modules/settings/%1/%2").arg(module_id, setting_id), normalized);
  return ServiceResult::success(QStringLiteral("Module setting saved"),
                                QStringLiteral("%1 was updated.")
                                    .arg(definition.value(QStringLiteral("label"), setting_id).toString()));
}

QVariantMap ModuleService::readManifest(const QString& module_dir) const {
  constexpr qint64 kMaxManifestBytes = 1024 * 1024;
  static const QStringList names{
      QStringLiteral("xenon-module.json"), QStringLiteral("module.json"),
      QStringLiteral("manifest.json")};
  for (const auto& name : names) {
    QFile file{QDir{module_dir}.filePath(name)};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
    if (file.size() <= 0 || file.size() > kMaxManifestBytes) continue;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) {
      auto map = document.object().toVariantMap();
      map.insert(QStringLiteral("_manifestPath"), file.fileName());
      return map;
    }
  }
  return {};
}

QVariantMap ModuleService::manifestToUi(const QVariantMap& manifest,
                                        const QString& module_dir) const {
  QVariantMap item;
  const auto module_id = firstString(manifest, {"id", "moduleId", "module_id"},
                                     QFileInfo{module_dir}.fileName());
  const auto active = settings_.boolValue(QStringLiteral("modules/enabled/") + module_id, true);

  item.insert(QStringLiteral("moduleName"), firstString(manifest, {"name", "title"}, module_id));
  item.insert(QStringLiteral("moduleType"), firstString(manifest, {"type", "moduleType"}, QStringLiteral("Game Module")));
  item.insert(QStringLiteral("version"), firstString(manifest, {"version"}, QStringLiteral("Unknown")));
  item.insert(QStringLiteral("status"), active ? QStringLiteral("Active") : QStringLiteral("Disabled"));
  item.insert(QStringLiteral("active"), active);
  item.insert(QStringLiteral("updateAvailable"), false);
  item.insert(QStringLiteral("description"), firstString(manifest, {"description", "summary"}, QStringLiteral("Installed Xenon module.")));
  item.insert(QStringLiteral("moduleId"), module_id);
  item.insert(QStringLiteral("regions"), joinedValue(manifest.value(QStringLiteral("regions")), QStringLiteral("Module-defined")));
  item.insert(QStringLiteral("gameIds"), joinedValue(manifest.value(QStringLiteral("gameIds")),
                                                       joinedValue(manifest.value(QStringLiteral("game_ids")), QStringLiteral("Module-defined"))));
  item.insert(QStringLiteral("runtimeDependency"), firstString(manifest, {"runtimeDependency", "runtime"}, QStringLiteral("Xenon Recomp")));
  item.insert(QStringLiteral("renderer"), firstString(manifest, {"renderer"}, QStringLiteral("Automatic")));
  item.insert(QStringLiteral("artwork"), firstString(manifest, {"artwork", "icon"}));
  item.insert(QStringLiteral("capabilities"), joinedValue(manifest.value(QStringLiteral("capabilities"))));
  item.insert(QStringLiteral("linkedGame"), QString{});
  item.insert(QStringLiteral("path"), module_dir);
  item.insert(QStringLiteral("manifestPath"), manifest.value(QStringLiteral("_manifestPath")));
  return item;
}

bool ModuleService::copyDirectory(const QString& source, const QString& destination) const {
  const QDir source_dir{source};
  if (!source_dir.exists() || QFileInfo::exists(destination)) return false;
  if (!QDir{}.mkpath(destination)) return false;

  const auto entries = source_dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
  for (const auto& entry : entries) {
    if (entry.isSymLink()) return false;
    const auto target = QDir{destination}.filePath(entry.fileName());
    if (entry.isDir()) {
      if (!copyDirectory(entry.absoluteFilePath(), target)) return false;
    } else if (!QFile::copy(entry.absoluteFilePath(), target)) {
      return false;
    }
  }
  return true;
}

}  // namespace xenon::launcher
