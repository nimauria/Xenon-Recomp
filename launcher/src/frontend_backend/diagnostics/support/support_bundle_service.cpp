#include "support_bundle_service.hpp"

#include "../../library/library_feature.hpp"
#include "../../modules/modules_feature.hpp"
#include "../../runtime/runtime_feature.hpp"
#include "../../settings/settings_feature.hpp"
#include "../../launch/session/session_controller.hpp"
#include "../../../services/path_service.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QPair>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QStringList>

#include <array>

namespace xenon::launcher::frontend_backend {
namespace {

struct ZipEntry {
  QByteArray name;
  QByteArray data;
  quint32 crc = 0;
  quint32 offset = 0;
};

void appendU16(QByteArray& output, quint16 value) {
  output.append(static_cast<char>(value & 0xff));
  output.append(static_cast<char>((value >> 8) & 0xff));
}

void appendU32(QByteArray& output, quint32 value) {
  output.append(static_cast<char>(value & 0xff));
  output.append(static_cast<char>((value >> 8) & 0xff));
  output.append(static_cast<char>((value >> 16) & 0xff));
  output.append(static_cast<char>((value >> 24) & 0xff));
}

quint32 crc32(const QByteArray& data) {
  static const auto table = [] {
    std::array<quint32, 256> values{};
    for (quint32 i = 0; i < values.size(); ++i) {
      quint32 value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? (value >> 1u) ^ 0xedb88320u : value >> 1u;
      }
      values[i] = value;
    }
    return values;
  }();

  quint32 value = 0xffffffffu;
  for (const auto byte : data) {
    const auto index = (value ^ static_cast<quint8>(byte)) & 0xffu;
    value = table[index] ^ (value >> 8u);
  }
  return value ^ 0xffffffffu;
}

QPair<quint16, quint16> dosDateTime(const QDateTime& date_time) {
  const auto local = date_time.toLocalTime();
  const auto date = local.date();
  const auto time = local.time();
  const auto year = qBound(1980, date.year(), 2107);
  const auto dos_date = static_cast<quint16>(((year - 1980) << 9) | (date.month() << 5) | date.day());
  const auto dos_time = static_cast<quint16>((time.hour() << 11) | (time.minute() << 5) | (time.second() / 2));
  return {dos_date, dos_time};
}

QByteArray makeZip(QList<ZipEntry> entries) {
  QByteArray output;
  const auto zip_time = dosDateTime(QDateTime::currentDateTime());
  const auto dos_date = zip_time.first;
  const auto dos_time = zip_time.second;
  constexpr quint16 kUtf8Flag = 0x0800;

  for (auto& entry : entries) {
    entry.crc = crc32(entry.data);
    entry.offset = static_cast<quint32>(output.size());

    appendU32(output, 0x04034b50u);
    appendU16(output, 20);
    appendU16(output, kUtf8Flag);
    appendU16(output, 0);  // Store; support bundles favour portability over compression.
    appendU16(output, dos_time);
    appendU16(output, dos_date);
    appendU32(output, entry.crc);
    appendU32(output, static_cast<quint32>(entry.data.size()));
    appendU32(output, static_cast<quint32>(entry.data.size()));
    appendU16(output, static_cast<quint16>(entry.name.size()));
    appendU16(output, 0);
    output.append(entry.name);
    output.append(entry.data);
  }

  const auto central_offset = static_cast<quint32>(output.size());
  for (const auto& entry : entries) {
    appendU32(output, 0x02014b50u);
    appendU16(output, 20);
    appendU16(output, 20);
    appendU16(output, kUtf8Flag);
    appendU16(output, 0);
    appendU16(output, dos_time);
    appendU16(output, dos_date);
    appendU32(output, entry.crc);
    appendU32(output, static_cast<quint32>(entry.data.size()));
    appendU32(output, static_cast<quint32>(entry.data.size()));
    appendU16(output, static_cast<quint16>(entry.name.size()));
    appendU16(output, 0);
    appendU16(output, 0);
    appendU16(output, 0);
    appendU16(output, 0);
    appendU32(output, 0);
    appendU32(output, entry.offset);
    output.append(entry.name);
  }

  const auto central_size = static_cast<quint32>(output.size()) - central_offset;
  const auto count = static_cast<quint16>(qMin<qsizetype>(entries.size(), qsizetype{0xffff}));
  appendU32(output, 0x06054b50u);
  appendU16(output, 0);
  appendU16(output, 0);
  appendU16(output, count);
  appendU16(output, count);
  appendU32(output, central_size);
  appendU32(output, central_offset);
  appendU16(output, 0);
  return output;
}

QVariantMap selectedFields(const QVariantMap& source, const QStringList& fields) {
  QVariantMap result;
  for (const auto& field : fields) {
    if (source.contains(field)) result.insert(field, source.value(field));
  }
  return result;
}

QString safeLogPath(const QString& file_name) {
  auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.trimmed().isEmpty()) root = QDir::tempPath();
  return QDir{root}.filePath(file_name);
}

QString recoveryDirectoryPath() {
  return QDir{QFileInfo{safeLogPath(QStringLiteral("placeholder"))}.absolutePath()}
      .filePath(QStringLiteral("recovery"));
}

QString latestRecoveryLogPath() {
  const QDir directory{recoveryDirectoryPath()};
  const auto logs = directory.entryInfoList(
      {QStringLiteral("xenon-launcher-unclean-*.log")}, QDir::Files, QDir::Time);
  return logs.isEmpty() ? QString{} : logs.first().absoluteFilePath();
}

QVariantMap recoverySnapshot() {
  QFile file{QDir{recoveryDirectoryPath()}.filePath(QStringLiteral("recovery-state.json"))};
  if (!file.open(QIODevice::ReadOnly)) return {};
  const auto document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) return {};
  const auto source = document.object().toVariantMap();
  return selectedFields(source, {QStringLiteral("previousUncleanShutdown"),
                                 QStringLiteral("lastIncidentAt"),
                                 QStringLiteral("previousPhase"),
                                 QStringLiteral("previousStartedAt"),
                                 QStringLiteral("consecutiveUncleanStarts"),
                                 QStringLiteral("currentSafeMode"),
                                 QStringLiteral("automaticSafeMode"),
                                 QStringLiteral("automaticSafeModeReason"),
                                 QStringLiteral("lastCleanExitAt")});
}

}  // namespace

SupportBundleService::SupportBundleService(PathService& paths, LibraryFeature& library,
                                           ModulesFeature& modules, SessionController& session,
                                           RuntimeFeature& runtime, SettingsFeature& settings)
    : paths_(paths),
      library_(library),
      modules_(modules),
      session_(session),
      runtime_(runtime),
      settings_(settings) {}

QString SupportBundleService::bundleDirectory() const {
  auto root = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (root.trimmed().isEmpty()) root = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (root.trimmed().isEmpty()) root = diagnosticsDirectory();
  root = QDir{root}.filePath(QStringLiteral("Xenon Support"));
  QDir{}.mkpath(root);
  return root;
}

QString SupportBundleService::diagnosticsDirectory() const {
  auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.trimmed().isEmpty()) root = QDir::tempPath();
  QDir{}.mkpath(root);
  return root;
}

QString SupportBundleService::startupLogPath() const {
  return safeLogPath(QStringLiteral("xenon-launcher-startup.log"));
}

QVariantMap SupportBundleService::settingsSnapshot() const {
  QVariantMap snapshot;
  for (const auto& category_value : settings_.categories()) {
    const auto category = category_value.toMap();
    const auto category_id = category.value(QStringLiteral("id")).toString();
    if (category_id == QStringLiteral("paths")) continue;

    QVariantMap values;
    for (const auto& key : settings_.keysForCategory(category_id)) {
      if (key.startsWith(QStringLiteral("paths/"))) continue;
      values.insert(key, settings_.value(key, settings_.defaultValue(key)));
    }
    if (!values.isEmpty()) snapshot.insert(category_id, values);
  }
  return snapshot;
}

QVariantList SupportBundleService::librarySnapshot() const {
  QVariantList result;
  const QStringList fields{QStringLiteral("gameId"), QStringLiteral("title"),
                           QStringLiteral("moduleId"), QStringLiteral("moduleName"),
                           QStringLiteral("moduleVersion"), QStringLiteral("status"),
                           QStringLiteral("ready"), QStringLiteral("contentState"),
                           QStringLiteral("renderer"), QStringLiteral("regions"),
                           QStringLiteral("playCount"), QStringLiteral("totalPlayTimeMs"),
                           QStringLiteral("lastSessionOutcome")};
  for (const auto& value : library_.entries()) result.append(selectedFields(value.toMap(), fields));
  return result;
}

QVariantList SupportBundleService::moduleSnapshot() const {
  QVariantList result;
  const QStringList fields{QStringLiteral("moduleId"), QStringLiteral("moduleName"),
                           QStringLiteral("moduleType"), QStringLiteral("version"),
                           QStringLiteral("status"), QStringLiteral("active"),
                           QStringLiteral("updateAvailable"), QStringLiteral("renderer"),
                           QStringLiteral("runtimeDependency"), QStringLiteral("capabilities")};
  for (const auto& value : modules_.entries()) result.append(selectedFields(value.toMap(), fields));
  return result;
}

QVariantList SupportBundleService::sessionSnapshot() const {
  QVariantList result;
  const auto history = session_.history();
  const auto limit = qMin<qsizetype>(history.size(), qsizetype{10});
  for (qsizetype index = 0; index < limit; ++index) {
    const auto source = history.at(index).toMap();
    auto item = selectedFields(source, {QStringLiteral("sessionId"), QStringLiteral("gameId"),
                                        QStringLiteral("title"), QStringLiteral("moduleId"),
                                        QStringLiteral("moduleName"), QStringLiteral("state"),
                                        QStringLiteral("outcome"), QStringLiteral("requestedAt"),
                                        QStringLiteral("startedAt"), QStringLiteral("endedAt"),
                                        QStringLiteral("elapsedMs")});
    const auto error = source.value(QStringLiteral("error")).toMap();
    if (!error.isEmpty()) {
      auto safe_error = selectedFields(error, {QStringLiteral("code"), QStringLiteral("title"),
                                                QStringLiteral("message")});
      safe_error.insert(QStringLiteral("message"), redactText(safe_error.value(QStringLiteral("message")).toString()));
      item.insert(QStringLiteral("error"), safe_error);
    }
    result.append(item);
  }
  return result;
}

QString SupportBundleService::redactText(QString text) const {
  QStringList private_paths{
      QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
      paths_.appDataPath(), paths_.configPath(), paths_.cachePath(),
      paths_.configuredPath(QStringLiteral("games")),
      paths_.configuredPath(QStringLiteral("saves")),
      paths_.configuredPath(QStringLiteral("profiles")),
      paths_.configuredPath(QStringLiteral("modules")),
      paths_.configuredPath(QStringLiteral("screenshots"))};

  int index = 0;
  for (const auto& path : private_paths) {
    const auto clean = QDir::cleanPath(path.trimmed());
    if (clean.isEmpty()) continue;
    const auto token = index == 0 ? QStringLiteral("<home>")
                                  : QStringLiteral("<xenon-path-%1>").arg(index);
    text.replace(clean, token, Qt::CaseInsensitive);
    auto alternate = clean;
    alternate.replace(u'\\', u'/');
    text.replace(alternate, token, Qt::CaseInsensitive);
    alternate = clean;
    alternate.replace(u'/', u'\\');
    text.replace(alternate, token, Qt::CaseInsensitive);
    ++index;
  }

  text.replace(QRegularExpression{QStringLiteral(R"(([A-Za-z]:[\\/]+Users[\\/]+)[^\\/\s]+)"),
                                  QRegularExpression::CaseInsensitiveOption},
               QStringLiteral("\\1<user>"));
  text.replace(QRegularExpression{QStringLiteral(R"((/(?:home|Users)/)[^/\s]+)"),
                                  QRegularExpression::CaseInsensitiveOption},
               QStringLiteral("\\1<user>"));
  text.replace(QRegularExpression{QStringLiteral(R"(\b[A-Z0-9._%+\-]+@[A-Z0-9.\-]+\.[A-Z]{2,}\b)"),
                                  QRegularExpression::CaseInsensitiveOption},
               QStringLiteral("<email>"));
  return text;
}

QByteArray SupportBundleService::sanitizedLog(const QString& path, qint64 max_bytes) const {
  QFile file{path};
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) return {};
  if (file.size() > max_bytes) file.seek(file.size() - max_bytes);
  return redactText(QString::fromUtf8(file.readAll())).toUtf8();
}

ServiceResult SupportBundleService::create(const QString& user_summary,
                                           const QString& developer_summary) const {
  const auto output_dir = bundleDirectory();
  if (output_dir.trimmed().isEmpty() || !QDir{}.mkpath(output_dir)) {
    return ServiceResult::failure(QStringLiteral("Support bundle"),
                                  QStringLiteral("Xenon could not create the support bundle folder."));
  }

  QVariantMap root;
  root.insert(QStringLiteral("schema"), QStringLiteral("xenon.support-bundle"));
  root.insert(QStringLiteral("schemaVersion"), 1);
  root.insert(QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  root.insert(QStringLiteral("system"), QVariantMap{
      {QStringLiteral("productName"), QSysInfo::prettyProductName()},
      {QStringLiteral("kernelType"), QSysInfo::kernelType()},
      {QStringLiteral("kernelVersion"), QSysInfo::kernelVersion()},
      {QStringLiteral("cpuArchitecture"), QSysInfo::currentCpuArchitecture()}});
  root.insert(QStringLiteral("runtimeStatus"), runtime_.status());
  root.insert(QStringLiteral("runtimeCapabilities"), runtime_.capabilities());
  const auto game_status = runtime_.gameStatus();
  if (game_status.value(QStringLiteral("available"), false).toBool()) {
    // Selected fields only: status.json (docs/runtime/RUNTIME_HOST.md) can include
    // absolute paths and titles the rest of this bundle deliberately omits.
    root.insert(QStringLiteral("gameStatus"),
               selectedFields(game_status, {QStringLiteral("stateName"), QStringLiteral("initialized"),
                                            QStringLiteral("running"), QStringLiteral("executionActive"),
                                            QStringLiteral("nativeExtension"), QStringLiteral("unresolvedImports"),
                                            QStringLiteral("subsystems"), QStringLiteral("requestedRenderer")}));
  }
  root.insert(QStringLiteral("settings"), settingsSnapshot());
  root.insert(QStringLiteral("library"), librarySnapshot());
  root.insert(QStringLiteral("modules"), moduleSnapshot());
  root.insert(QStringLiteral("recentSessions"), sessionSnapshot());
  const auto recovery = recoverySnapshot();
  if (!recovery.isEmpty()) root.insert(QStringLiteral("recovery"), recovery);
  const auto current = session_.currentSession();
  if (!current.isEmpty() && current.value(QStringLiteral("state")).toString() != QStringLiteral("idle")) {
    auto current_safe = selectedFields(current, {QStringLiteral("sessionId"), QStringLiteral("gameId"),
                                                  QStringLiteral("title"), QStringLiteral("moduleId"),
                                                  QStringLiteral("moduleName"), QStringLiteral("state"),
                                                  QStringLiteral("stateLabel"), QStringLiteral("requestedAt"),
                                                  QStringLiteral("startedAt"), QStringLiteral("endedAt"),
                                                  QStringLiteral("elapsedMs"), QStringLiteral("outcome")});
    const auto error = current.value(QStringLiteral("error")).toMap();
    if (!error.isEmpty()) {
      auto safe_error = selectedFields(error, {QStringLiteral("code"), QStringLiteral("title"),
                                                QStringLiteral("message")});
      safe_error.insert(QStringLiteral("message"), redactText(safe_error.value(QStringLiteral("message")).toString()));
      current_safe.insert(QStringLiteral("error"), safe_error);
    }
    root.insert(QStringLiteral("currentSession"), current_safe);
  }

  const auto summary = redactText(user_summary).toUtf8();
  const auto developer = redactText(developer_summary).toUtf8();
  const auto startup_log = sanitizedLog(startupLogPath(), 192 * 1024);
  const auto recovery_log = sanitizedLog(latestRecoveryLogPath(), 192 * 1024);
  const auto update_log = sanitizedLog(safeLogPath(QStringLiteral("update-install.log")), 96 * 1024);
  // Tail of the current/last runtime host's log.txt (docs/runtime/RUNTIME_HOST.md) -
  // covers crash output and missing-export diagnostics from guest execution.
  const auto runtime_host_log = redactText(runtime_.runtimeLog()).toUtf8();

  const QByteArray privacy = QByteArrayLiteral(
      "Project Xenon support bundle\n\n"
      "This bundle is generated locally. It intentionally omits configured game/save/module/profile paths, "
      "profile names, avatar locations and launch configuration paths. Known home/application/configured paths "
      "and email-shaped strings are redacted from included log excerpts. If Xenon detected an unclean "
      "shutdown, the most recent preserved startup log may also be included.\n\n"
      "Review the files before sharing them if the logs may contain information entered by third-party modules.\n");

  QList<ZipEntry> entries{
      {QByteArrayLiteral("privacy.txt"), privacy},
      {QByteArrayLiteral("system-summary.txt"), summary},
      {QByteArrayLiteral("developer-diagnostics.txt"), developer},
      {QByteArrayLiteral("support.json"), QJsonDocument{QJsonObject::fromVariantMap(root)}.toJson(QJsonDocument::Indented)}};
  if (!startup_log.isEmpty())
    entries.append(ZipEntry{QByteArrayLiteral("logs/launcher-startup.log"), startup_log});
  if (!recovery_log.isEmpty())
    entries.append(ZipEntry{QByteArrayLiteral("logs/last-unclean-startup.log"), recovery_log});
  if (!runtime_host_log.isEmpty())
    entries.append(ZipEntry{QByteArrayLiteral("logs/runtime-host.log"), runtime_host_log});
  if (!update_log.isEmpty())
    entries.append(ZipEntry{QByteArrayLiteral("logs/update-install.log"), update_log});

  const auto file_name = QStringLiteral("xenon-support-%1.zip")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
  const auto target = QDir{output_dir}.filePath(file_name);
  QSaveFile file{target};
  if (!file.open(QIODevice::WriteOnly)) {
    return ServiceResult::failure(QStringLiteral("Support bundle"),
                                  QStringLiteral("Xenon could not create the support bundle file."));
  }
  const auto archive = makeZip(std::move(entries));
  if (file.write(archive) != archive.size() || !file.commit()) {
    return ServiceResult::failure(QStringLiteral("Support bundle"),
                                  QStringLiteral("Xenon could not finish writing the support bundle."));
  }

  return ServiceResult::success(
      QStringLiteral("Support bundle created"),
      QStringLiteral("A privacy-sanitized Xenon support bundle was created. Review it before sharing it publicly."),
      target);
}

}  // namespace xenon::launcher::frontend_backend
