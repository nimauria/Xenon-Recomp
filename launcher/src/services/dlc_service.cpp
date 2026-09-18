#include "dlc_service.hpp"

#include "library_service.hpp"
#include "module_service.hpp"
#include "path_service.hpp"
#include "settings_service.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace xenon::launcher {
namespace {
constexpr auto kReceiptName = ".xenon-dlc.json";

QString safePathSegment(QString value, const QString& fallback) {
  value = value.trimmed();
  value.replace(QRegularExpression{QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])")},
                QStringLiteral("_"));
  value.replace(QRegularExpression{QStringLiteral(R"(\s+)")}, QStringLiteral(" "));
  while (value.endsWith(QLatin1Char(' ')) || value.endsWith(QLatin1Char('.'))) value.chop(1);
  while (value.startsWith(QLatin1Char(' '))) value.remove(0, 1);
  if (value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral("..")) {
    value = fallback;
  }

#if defined(Q_OS_WIN)
  static const QSet<QString> reserved{
      QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
      QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
      QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
      QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
      QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
      QStringLiteral("LPT8"), QStringLiteral("LPT9")};
  if (reserved.contains(value.toUpper())) value.prepend(QLatin1Char('_'));
#endif

  if (value.size() > 96) value = value.left(96).trimmed();
  return value;
}

QString canonicalOrAbsolute(const QString& path) {
  const QFileInfo info{path};
  const auto canonical = info.canonicalFilePath();
  return QDir::fromNativeSeparators(
      QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical));
}
}  // namespace

DlcService::DlcService(SettingsService& settings, PathService& paths, LibraryService& library,
                       ModuleService& modules, QObject* parent)
    : QObject(parent), settings_(settings), paths_(paths), library_(library), modules_(modules) {}

QVariantList DlcService::entries(const QString& game_id) const {
  auto result = mergedEntries(game_id);
  if (settings_.stringValue(QStringLiteral("frontend/library/missingContent"),
                            QStringLiteral("Show in catalogue")) ==
      QStringLiteral("Hide missing content")) {
    QVariantList installed_only;
    for (const auto& value : result) {
      if (value.toMap().value(QStringLiteral("installed")).toBool()) installed_only.append(value);
    }
    return installed_only;
  }
  return result;
}

QVariantMap DlcService::entry(const QString& game_id, const QString& dlc_id) const {
  for (const auto& value : mergedEntries(game_id)) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("dlcId")).toString() == dlc_id) return item;
  }
  return {};
}

QVariantList DlcService::launchEntries(const QString& game_id) const {
  QVariantList result;
  for (const auto& value : mergedEntries(game_id)) {
    const auto item = value.toMap();
    if (!item.value(QStringLiteral("installed")).toBool()) continue;
    const auto path = item.value(QStringLiteral("path")).toString();
    if (!isSafeManagedPath(game_id, path)) continue;

    QVariantMap mount;
    mount.insert(QStringLiteral("dlcId"), item.value(QStringLiteral("dlcId")));
    mount.insert(QStringLiteral("name"), item.value(QStringLiteral("name")));
    mount.insert(QStringLiteral("path"), path);
    mount.insert(QStringLiteral("version"), item.value(QStringLiteral("version")));
    mount.insert(QStringLiteral("contentIds"), item.value(QStringLiteral("contentIds")));
    mount.insert(QStringLiteral("receipt"), item.value(QStringLiteral("receipt")));
    result.append(mount);
  }
  return result;
}

QString DlcService::rootPath(const QString& game_id) const {
  const auto managed = library_.managedPath(game_id);
  return managed.isEmpty() ? QString{} : QDir{managed}.filePath(QStringLiteral("DLC"));
}

QString DlcService::ensureRootPath(const QString& game_id) const {
  const auto root = rootPath(game_id);
  return !root.isEmpty() && paths_.ensureDirectory(root) ? root : QString{};
}

QString DlcService::itemPath(const QString& game_id, const QString& dlc_id) const {
  const auto definition = moduleDefinition(game_id, dlc_id);
  if (definition.isEmpty()) return {};
  const auto root = rootPath(game_id);
  if (root.isEmpty()) return {};
  const auto path = QDir{root}.filePath(folderName(game_id, definition));
  if (QFileInfo::exists(path) && !isSafeManagedPath(game_id, path)) return {};
  return path;
}

ServiceResult DlcService::verify(const QString& game_id, const QString& dlc_id) const {
  const auto item = entry(game_id, dlc_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("DLC verification"),
                                  QStringLiteral("The selected add-on is not declared by this game's module."));
  }
  if (!item.value(QStringLiteral("installed")).toBool()) {
    return ServiceResult::failure(QStringLiteral("DLC not installed"),
                                  QStringLiteral("%1 is not installed locally.")
                                      .arg(item.value(QStringLiteral("name")).toString()));
  }

  const auto path = item.value(QStringLiteral("path")).toString();
  if (!isSafeManagedPath(game_id, path) || !QDir{path}.exists() || !directoryHasPayload(path)) {
    return ServiceResult::failure(QStringLiteral("DLC verification"),
                                  QStringLiteral("The managed add-on folder is missing or empty."));
  }

  const auto receipt = readReceipt(path);
  if (!receipt.isEmpty()) {
    const auto receipt_id = receipt.value(QStringLiteral("dlcId")).toString();
    if (!receipt_id.isEmpty() && receipt_id != dlc_id) {
      return ServiceResult::failure(QStringLiteral("DLC verification"),
                                    QStringLiteral("The install receipt does not match the module DLC identifier."));
    }
  }

  return ServiceResult::success(QStringLiteral("DLC verified"),
                                QStringLiteral("%1 is present in Xenon's managed DLC directory.")
                                    .arg(item.value(QStringLiteral("name")).toString()));
}

ServiceResult DlcService::remove(const QString& game_id, const QString& dlc_id) {
  const auto item = entry(game_id, dlc_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Remove DLC"),
                                  QStringLiteral("The selected add-on is not declared by this game's module."));
  }
  const auto path = item.value(QStringLiteral("path")).toString();
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    return ServiceResult::failure(QStringLiteral("Remove DLC"),
                                  QStringLiteral("The selected add-on is not installed locally."));
  }
  if (!isSafeManagedPath(game_id, path)) {
    return ServiceResult::failure(QStringLiteral("Remove DLC"),
                                  QStringLiteral("Xenon refused to remove content outside its managed DLC directory."));
  }
  if (!QDir{path}.removeRecursively()) {
    return ServiceResult::failure(QStringLiteral("Remove DLC"),
                                  QStringLiteral("Xenon could not remove the managed add-on directory."));
  }
  emit changed(game_id);
  return ServiceResult::success(QStringLiteral("DLC removed"),
                                QStringLiteral("%1 was removed from this game's managed DLC folder.")
                                    .arg(item.value(QStringLiteral("name")).toString()));
}

ServiceResult DlcService::prepareInstall(const QString& game_id, const QString& dlc_id) const {
  const auto item = entry(game_id, dlc_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Prepare DLC install"),
                                  QStringLiteral("The module does not declare the requested DLC identifier."));
  }
  const auto path = item.value(QStringLiteral("path")).toString();
  if (!isSafeManagedPath(game_id, path) || !paths_.ensureDirectory(path)) {
    return ServiceResult::failure(QStringLiteral("Prepare DLC install"),
                                  QStringLiteral("Xenon could not create the managed DLC destination."));
  }
  return ServiceResult::success(QStringLiteral("DLC destination ready"), {}, path);
}

ServiceResult DlcService::commitInstall(const QString& game_id, const QString& dlc_id,
                                        const QVariantMap& receipt) {
  const auto item = entry(game_id, dlc_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Commit DLC install"),
                                  QStringLiteral("The module does not declare the requested DLC identifier."));
  }
  const auto path = item.value(QStringLiteral("path")).toString();
  if (!isSafeManagedPath(game_id, path) || !QDir{path}.exists() || !directoryHasPayload(path)) {
    return ServiceResult::failure(QStringLiteral("Commit DLC install"),
                                  QStringLiteral("The managed DLC destination does not contain imported content."));
  }

  QVariantMap normalized = receipt;
  normalized.insert(QStringLiteral("schemaVersion"), 1);
  normalized.insert(QStringLiteral("gameId"), game_id);
  normalized.insert(QStringLiteral("dlcId"), dlc_id);
  normalized.insert(QStringLiteral("name"), item.value(QStringLiteral("name")));
  normalized.insert(QStringLiteral("installedAt"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  if (!writeReceipt(path, normalized)) {
    return ServiceResult::failure(QStringLiteral("Commit DLC install"),
                                  QStringLiteral("The DLC files are present, but Xenon could not write the install receipt."));
  }
  emit changed(game_id);
  return ServiceResult::success(QStringLiteral("DLC installed"),
                                QStringLiteral("%1 is now registered as installed.")
                                    .arg(item.value(QStringLiteral("name")).toString()));
}

QVariantList DlcService::mergedEntries(const QString& game_id) const {
  const auto game = library_.entry(game_id);
  if (game.isEmpty()) return {};
  const auto module_id = game.value(QStringLiteral("moduleId")).toString();
  const auto definitions = modules_.dlcCatalog(module_id);

  QVariantList result;
  for (const auto& value : definitions) {
    auto item = value.toMap();
    const auto dlc_id = item.value(QStringLiteral("dlcId")).toString();
    const auto path = QDir{rootPath(game_id)}.filePath(folderName(game_id, item));
    const auto installed = isSafeManagedPath(game_id, path) && QDir{path}.exists() &&
                           directoryHasPayload(path);
    const auto receipt = installed ? readReceipt(path) : QVariantMap{};

    item.insert(QStringLiteral("installed"), installed);
    item.insert(QStringLiteral("state"), installed ? QStringLiteral("Installed")
                                                    : QStringLiteral("Not installed"));
    item.insert(QStringLiteral("path"), path);
    item.insert(QStringLiteral("folderName"), QFileInfo{path}.fileName());
    item.insert(QStringLiteral("managed"), true);
    item.insert(QStringLiteral("receipt"), receipt);
    item.insert(QStringLiteral("gameId"), game_id);
    item.insert(QStringLiteral("moduleId"), module_id);
    item.insert(QStringLiteral("canVerify"), installed);
    item.insert(QStringLiteral("canRemove"), installed);
    item.insert(QStringLiteral("canOpen"), installed);
    item.insert(QStringLiteral("dlcId"), dlc_id);
    result.append(item);
  }
  return result;
}

QVariantMap DlcService::moduleDefinition(const QString& game_id, const QString& dlc_id) const {
  const auto game = library_.entry(game_id);
  if (game.isEmpty()) return {};
  for (const auto& value : modules_.dlcCatalog(game.value(QStringLiteral("moduleId")).toString())) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("dlcId")).toString() == dlc_id) return item;
  }
  return {};
}

QString DlcService::folderName(const QString& game_id, const QVariantMap& definition) const {
  const auto requested = safePathSegment(definition.value(QStringLiteral("name")).toString(),
                                         definition.value(QStringLiteral("dlcId")).toString());

  // Keep human-readable DLC names as requested. If a module declares two DLC
  // entries that collapse to the same filesystem-safe name, only the later
  // collision gets a stable ID suffix.
  QSet<QString> used;
  const auto game = library_.entry(game_id);
  const auto definitions = modules_.dlcCatalog(game.value(QStringLiteral("moduleId")).toString());
  for (const auto& value : definitions) {
    const auto item = value.toMap();
    const auto candidate = safePathSegment(item.value(QStringLiteral("name")).toString(),
                                           item.value(QStringLiteral("dlcId")).toString());
    if (item.value(QStringLiteral("dlcId")) == definition.value(QStringLiteral("dlcId"))) {
      if (!used.contains(candidate.toLower())) return candidate;
      const auto suffix = safePathSegment(definition.value(QStringLiteral("dlcId")).toString(),
                                          QStringLiteral("dlc"));
      return QStringLiteral("%1 [%2]").arg(candidate, suffix);
    }
    used.insert(candidate.toLower());
  }
  return requested;
}

QString DlcService::installReceiptPath(const QString& item_path) const {
  return QDir{item_path}.filePath(QString::fromLatin1(kReceiptName));
}

bool DlcService::directoryHasPayload(const QString& item_path) const {
  const QDir dir{item_path};
  if (!dir.exists()) return false;
  const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
  for (const auto& entry : entries) {
    if (entry.fileName() != QString::fromLatin1(kReceiptName)) return true;
  }
  return false;
}

bool DlcService::isSafeManagedPath(const QString& game_id, const QString& candidate) const {
  const auto root = rootPath(game_id);
  if (root.isEmpty() || candidate.isEmpty()) return false;
  const auto clean_root = canonicalOrAbsolute(root);
  const auto clean_candidate = canonicalOrAbsolute(candidate);
#if defined(Q_OS_WIN)
  const auto root_cmp = clean_root.toLower();
  const auto candidate_cmp = clean_candidate.toLower();
#else
  const auto& root_cmp = clean_root;
  const auto& candidate_cmp = clean_candidate;
#endif
  return candidate_cmp == root_cmp || candidate_cmp.startsWith(root_cmp + QLatin1Char('/'));
}

QVariantMap DlcService::readReceipt(const QString& item_path) const {
  QFile file{installReceiptPath(item_path)};
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  return error.error == QJsonParseError::NoError && document.isObject()
             ? document.object().toVariantMap()
             : QVariantMap{};
}

bool DlcService::writeReceipt(const QString& item_path, const QVariantMap& receipt) const {
  QSaveFile file{installReceiptPath(item_path)};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
  file.write(QJsonDocument{QJsonObject::fromVariantMap(receipt)}.toJson(QJsonDocument::Indented));
  return file.commit();
}

}  // namespace xenon::launcher
