#include "content_probe.hpp"

#include <QFileInfo>
#include <QMetaType>
#include <QSet>
#include <QStringList>

#include <filesystem>
#include <memory>
#include <initializer_list>

#if defined(XENON_LAUNCHER_HAS_FILESYSTEM)
#include "xenon/filesystem/content_probe.hpp"
#include "xenon/filesystem/content_materializer.hpp"
#include "xenon/filesystem/stfs_package_source.hpp"
#endif

namespace xenon::launcher {
namespace {
ServiceResult unavailable(const QString& operation) {
  return ServiceResult::failure(
      QStringLiteral("Content probe unavailable"),
      QStringLiteral("%1 is ready at the launcher boundary, but Xbox 360 content "
                     "identification is waiting for the Xenon filesystem/content subsystem.")
          .arg(operation));
}

#if defined(XENON_LAUNCHER_HAS_FILESYSTEM)
std::filesystem::path localPath(const QUrl& source) {
#if defined(Q_OS_WIN)
  return std::filesystem::path(source.toLocalFile().toStdWString());
#else
  return std::filesystem::path(source.toLocalFile().toStdString());
#endif
}

QString pathString(const std::filesystem::path& path) {
#if defined(Q_OS_WIN)
  return QString::fromStdWString(path.native());
#else
  return QString::fromStdString(path.string());
#endif
}

QString firstString(const QVariantMap& map, std::initializer_list<const char*> keys,
                    const QString& fallback = {}) {
  for (const auto* key : keys) {
    const auto value = map.value(QString::fromLatin1(key));
    if (value.isValid() && !value.toString().trimmed().isEmpty()) {
      return value.toString().trimmed();
    }
  }
  return fallback;
}

QStringList strings(const QVariant& value) {
  QStringList result;
  if (!value.isValid() || value.isNull()) return result;
  if (value.metaType().id() == QMetaType::QStringList) return value.toStringList();
  const auto list = value.toList();
  if (!list.isEmpty()) {
    for (const auto& item : list) {
      const auto text = item.toString().trimmed();
      if (!text.isEmpty()) result.append(text);
    }
    return result;
  }
  const auto text = value.toString().trimmed();
  if (!text.isEmpty()) result.append(text);
  return result;
}

QString normalizeHexId(QString value) {
  value = value.trimmed().toUpper();
  if (value.startsWith(QStringLiteral("0X"))) value.remove(0, 2);
  if (value.size() != 8) return {};
  for (const auto ch : value) {
    if (!ch.isDigit() && !(ch >= QLatin1Char('A') && ch <= QLatin1Char('F'))) return {};
  }
  return value;
}

struct IdentityRule {
  QStringList title_ids;
  QStringList media_ids;
  QString version;
  int disc_number{-1};
};

void appendNormalizedIds(QStringList& destination, const QVariant& value) {
  for (const auto& raw : strings(value)) {
    const auto normalized = normalizeHexId(raw);
    if (!normalized.isEmpty() && !destination.contains(normalized)) {
      destination.append(normalized);
    }
  }
}

IdentityRule ruleFromMap(const QVariantMap& map) {
  IdentityRule rule{};
  appendNormalizedIds(rule.title_ids, map.value(QStringLiteral("titleId")));
  appendNormalizedIds(rule.title_ids, map.value(QStringLiteral("title_id")));
  appendNormalizedIds(rule.title_ids, map.value(QStringLiteral("titleIds")));
  appendNormalizedIds(rule.title_ids, map.value(QStringLiteral("title_ids")));
  appendNormalizedIds(rule.media_ids, map.value(QStringLiteral("mediaId")));
  appendNormalizedIds(rule.media_ids, map.value(QStringLiteral("media_id")));
  appendNormalizedIds(rule.media_ids, map.value(QStringLiteral("mediaIds")));
  appendNormalizedIds(rule.media_ids, map.value(QStringLiteral("media_ids")));
  rule.version = firstString(map, {"version", "xexVersion", "xex_version"});
  bool ok = false;
  const auto disc = firstString(map, {"discNumber", "disc_number"}).toInt(&ok);
  if (ok) rule.disc_number = disc;
  return rule;
}

QList<IdentityRule> manifestRules(const QVariantMap& manifest) {
  QList<IdentityRule> rules;
  const auto content = manifest.value(QStringLiteral("content")).toMap();
  QVariantList raw_rules = content.value(QStringLiteral("identities")).toList();
  if (raw_rules.isEmpty()) raw_rules = manifest.value(QStringLiteral("identities")).toList();
  for (const auto& value : raw_rules) {
    const auto rule = ruleFromMap(value.toMap());
    if (!rule.title_ids.isEmpty() || !rule.media_ids.isEmpty()) rules.append(rule);
  }

  auto fallback = ruleFromMap(manifest);
  appendNormalizedIds(fallback.title_ids, manifest.value(QStringLiteral("gameIds")));
  appendNormalizedIds(fallback.title_ids, manifest.value(QStringLiteral("game_ids")));
  appendNormalizedIds(fallback.title_ids, content.value(QStringLiteral("titleIds")));
  appendNormalizedIds(fallback.title_ids, content.value(QStringLiteral("title_ids")));
  appendNormalizedIds(fallback.media_ids, content.value(QStringLiteral("mediaIds")));
  appendNormalizedIds(fallback.media_ids, content.value(QStringLiteral("media_ids")));
  if (!fallback.title_ids.isEmpty() || !fallback.media_ids.isEmpty()) rules.append(fallback);
  return rules;
}

int ruleScore(const IdentityRule& rule, const QString& title_id, const QString& media_id,
              const QString& version, int disc_number) {
  int score = 0;
  if (!rule.title_ids.isEmpty()) {
    if (!rule.title_ids.contains(title_id)) return -1;
    score += 100;
  }
  if (!rule.media_ids.isEmpty()) {
    if (!rule.media_ids.contains(media_id)) return -1;
    score += 20;
  }
  if (!rule.version.isEmpty()) {
    if (rule.version.compare(version, Qt::CaseInsensitive) != 0) return -1;
    score += 5;
  }
  if (rule.disc_number >= 0) {
    if (rule.disc_number != disc_number) return -1;
    score += 2;
  }
  return score;
}

QVariantMap bestModuleMatch(const QVariantList& candidates, const QString& title_id,
                            const QString& media_id, const QString& version,
                            int disc_number, bool& ambiguous) {
  ambiguous = false;
  int best_score = -1;
  QVariantMap best;
  QSet<QString> best_ids;

  for (const auto& value : candidates) {
    const auto candidate = value.toMap();
    const auto manifest = candidate.value(QStringLiteral("manifest")).toMap();
    const auto module_id = candidate.value(QStringLiteral("moduleId")).toString().trimmed();
    if (module_id.isEmpty() || manifest.isEmpty()) continue;

    int candidate_score = -1;
    for (const auto& rule : manifestRules(manifest)) {
      candidate_score = qMax(candidate_score,
                             ruleScore(rule, title_id, media_id, version, disc_number));
    }
    if (candidate_score < 0) continue;
    if (candidate_score > best_score) {
      best_score = candidate_score;
      best = candidate;
      best_ids.clear();
      best_ids.insert(module_id);
      ambiguous = false;
    } else if (candidate_score == best_score && !best_ids.contains(module_id)) {
      best_ids.insert(module_id);
      ambiguous = true;
    }
  }
  return best;
}

// A module manifest may optionally pin which part of an artwork asset must
// remain visible when the launcher crops it to fit a tile/hero frame (e.g.
// `"heroArtFocal": {"x": 0.5, "y": 0.15}` to keep a logo near the top of a
// wide hero image instead of the launcher's generic center/left-anchored
// crop). Absent when the manifest declares none, so callers fall back to
// each frame's own default alignment rather than silently assuming center.
void insertFocalPoint(QVariantMap& fields, const QVariantMap& source,
                      const QString& manifest_key, const QString& field_prefix) {
  const auto focal = source.value(manifest_key).toMap();
  if (!focal.contains(QStringLiteral("x")) || !focal.contains(QStringLiteral("y"))) return;
  bool x_ok = false;
  bool y_ok = false;
  const auto x = focal.value(QStringLiteral("x")).toDouble(&x_ok);
  const auto y = focal.value(QStringLiteral("y")).toDouble(&y_ok);
  if (!x_ok || !y_ok) return;
  fields.insert(field_prefix + QStringLiteral("FocalX"), qBound(0.0, x, 1.0));
  fields.insert(field_prefix + QStringLiteral("FocalY"), qBound(0.0, y, 1.0));
}

QVariantMap moduleDisplayFields(const QVariantMap& module) {
  const auto manifest = module.value(QStringLiteral("manifest")).toMap();
  const auto launcher = manifest.value(QStringLiteral("launcher")).toMap();
  QVariantMap fields;
  fields.insert(QStringLiteral("moduleId"), module.value(QStringLiteral("moduleId")));
  fields.insert(QStringLiteral("moduleName"), module.value(QStringLiteral("moduleName")));
  fields.insert(QStringLiteral("moduleVersion"), module.value(QStringLiteral("version")));
  fields.insert(QStringLiteral("title"), firstString(manifest, {"name", "title"},
                                                      module.value(QStringLiteral("moduleName")).toString()));
  fields.insert(QStringLiteral("description"), firstString(manifest, {"description", "summary"}));
  fields.insert(QStringLiteral("renderer"), firstString(manifest, {"renderer"}, QStringLiteral("Automatic")));
  fields.insert(QStringLiteral("regions"), module.value(QStringLiteral("regions")));
  fields.insert(QStringLiteral("tileArt"),
               firstString(launcher, {"tileArt", "artwork", "icon"},
                          firstString(manifest, {"artwork", "icon"})));
  fields.insert(QStringLiteral("heroArt"), firstString(launcher, {"heroArt", "hero"}));
  insertFocalPoint(fields, launcher, QStringLiteral("tileArtFocal"), QStringLiteral("tileArt"));
  insertFocalPoint(fields, launcher, QStringLiteral("heroArtFocal"), QStringLiteral("heroArt"));
  return fields;
}
#endif
}  // namespace

QString UnavailableContentProbe::status() const {
  return QStringLiteral("Waiting for Xenon content subsystem");
}

ServiceResult UnavailableContentProbe::identifyGame(
    const QUrl&, const QVariantList&) const {
  return unavailable(QStringLiteral("Game import"));
}

ServiceResult UnavailableContentProbe::matchModule(
    const QString&, const QString&, const QString&, int, const QVariantList&) const {
  return unavailable(QStringLiteral("Module resolution"));
}

ServiceResult UnavailableContentProbe::identifyDlc(
    const QUrl&, const QString&, const QVariantList&) const {
  return unavailable(QStringLiteral("DLC import"));
}

ServiceResult UnavailableContentProbe::materializeDlc(
    const QUrl&, const QString&, const QVariantMap&) const {
  return unavailable(QStringLiteral("DLC installation"));
}

#if defined(XENON_LAUNCHER_HAS_FILESYSTEM)
QString XenonContentProbe::status() const {
  return QStringLiteral("XEX2 + GDFX game identification and STFS DLC import active");
}

ServiceResult XenonContentProbe::identifyGame(
    const QUrl& source, const QVariantList& module_candidates) const {
  if (!source.isLocalFile()) {
    return ServiceResult::failure(QStringLiteral("Game identification"),
                                  QStringLiteral("Only local content sources are supported."));
  }

  xenon::filesystem::ContentProbe probe;
  const auto result = probe.probe(localPath(source));
  if (!result.identified()) {
    return ServiceResult::failure(
        QStringLiteral("Game identification"),
        QString::fromStdString(result.message));
  }

  const auto& execution = *result.xex.execution_info;
  const auto title_id = QString::fromStdString(xenon::filesystem::format_xbox_id(execution.title_id));
  const auto media_id = QString::fromStdString(xenon::filesystem::format_xbox_id(execution.media_id));
  const auto xex_version = QString::fromStdString(execution.version.to_string());

  bool ambiguous = false;
  const auto module = bestModuleMatch(module_candidates, title_id, media_id, xex_version,
                                      execution.disc_number, ambiguous);
  // Genuine ambiguity (more than one installed module claims this exact
  // identity) is a hard failure - guessing between them would be wrong.
  // Zero matches is NOT a failure: the disc's own identity is real and
  // stable regardless of what happens to be installed right now, so the
  // title/source is still identified and can be added to the library.
  // Module resolution can happen later (installing a compatible module, or
  // matchModule() re-run when the user presses Play).
  if (ambiguous) {
    return ServiceResult::failure(
        QStringLiteral("Game identification"),
        QStringLiteral("More than one installed module declares the same Xbox content identity. "
                       "Tighten the modules' media/version identity rules before importing."));
  }

  QVariantMap identified;
  if (!module.isEmpty()) {
    const auto fields = moduleDisplayFields(module);
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) identified.insert(it.key(), it.value());
  } else {
    identified.insert(QStringLiteral("moduleId"), QString{});
    identified.insert(QStringLiteral("moduleName"), QString{});
    identified.insert(QStringLiteral("moduleVersion"), QString{});
    // Deliberately do NOT insert "title"/"description"/etc. here: leaving
    // them absent lets LibraryService::makeEntry()'s own filename-derived
    // defaults stand, rather than overwriting them with empty strings.
  }
  identified.insert(QStringLiteral("titleId"), title_id);
  identified.insert(QStringLiteral("mediaId"), media_id);
  identified.insert(QStringLiteral("xexVersion"), xex_version);
  identified.insert(QStringLiteral("discNumber"), execution.disc_number);
  identified.insert(QStringLiteral("discCount"), execution.disc_count);
  identified.insert(QStringLiteral("sourceType"),
                    QString::fromLatin1(xenon::filesystem::to_string(result.source_type).data(),
                                        static_cast<qsizetype>(xenon::filesystem::to_string(result.source_type).size())));
  const auto executable_path = !result.executable_path.empty()
                                   ? pathString(result.executable_path)
                                   : QString::fromStdString(result.executable_guest_path);
  identified.insert(QStringLiteral("executablePath"), executable_path);
  identified.insert(QStringLiteral("executableGuestPath"),
                    QString::fromStdString(result.executable_guest_path));
  if (!result.resolved_source_path.empty()) {
    identified.insert(QStringLiteral("resolvedSourcePath"),
                      pathString(result.resolved_source_path));
  }
  const bool module_matched = !module.isEmpty();
  if (module_matched) {
    identified.insert(QStringLiteral("contentState"),
                      result.source_type == xenon::filesystem::ContentSourceType::GdfxImage
                          ? QStringLiteral("GDFX/XEX2 identified")
                          : QStringLiteral("XEX2 identified"));
    identified.insert(QStringLiteral("status"), QStringLiteral("Ready"));
  } else {
    identified.insert(QStringLiteral("contentState"), QStringLiteral("Awaiting module installation"));
    identified.insert(QStringLiteral("status"), QStringLiteral("Module required"));
  }
  identified.insert(QStringLiteral("ready"), module_matched);

  return ServiceResult::success(
      QStringLiteral("Game identified"),
      module_matched
          ? QStringLiteral("Identified Xbox title %1 / media %2 and matched %3.")
                .arg(title_id, media_id, identified.value(QStringLiteral("moduleName")).toString())
          : QStringLiteral("Identified Xbox title %1 / media %2. No installed module supports it "
                           "yet - install a compatible module, then press Play to prepare and "
                           "launch it.")
                .arg(title_id, media_id),
      identified);
}

ServiceResult XenonContentProbe::matchModule(
    const QString& title_id, const QString& media_id, const QString& xex_version,
    int disc_number, const QVariantList& module_candidates) const {
  bool ambiguous = false;
  const auto module =
      bestModuleMatch(module_candidates, title_id, media_id, xex_version, disc_number, ambiguous);
  if (ambiguous) {
    return ServiceResult::failure(
        QStringLiteral("Module resolution"),
        QStringLiteral("More than one installed module declares the same Xbox content identity. "
                       "Tighten the modules' media/version identity rules."));
  }
  if (module.isEmpty()) {
    return ServiceResult::failure(
        QStringLiteral("Module resolution"),
        QStringLiteral("No installed module declares a matching title/content identity for "
                       "title %1 / media %2.")
            .arg(title_id, media_id));
  }
  const auto fields = moduleDisplayFields(module);
  return ServiceResult::success(
      QStringLiteral("Module matched"),
      QStringLiteral("Matched %1.").arg(fields.value(QStringLiteral("moduleName")).toString()),
      fields);
}

ServiceResult XenonContentProbe::identifyDlc(
    const QUrl& source, const QString&, const QVariantList& catalogue) const {
  if (!source.isLocalFile()) {
    return ServiceResult::failure(QStringLiteral("DLC identification"),
                                  QStringLiteral("Only local STFS packages are supported."));
  }

  xenon::filesystem::ContentProbe probe;
  const auto result = probe.probe(localPath(source));
  if (!result.identified() ||
      result.source_type != xenon::filesystem::ContentSourceType::StfsPackage ||
      !result.stfs.has_value()) {
    return ServiceResult::failure(QStringLiteral("DLC identification"),
                                  QString::fromStdString(result.message));
  }

  const auto& package = *result.stfs;
  const auto content_id = QString::fromStdString(package.content_id_hex);
  const auto title_id = QString::fromStdString(
      xenon::filesystem::format_xbox_id(package.execution_info.title_id));
  const auto media_id = QString::fromStdString(
      xenon::filesystem::format_xbox_id(package.execution_info.media_id));
  const auto package_version =
      QString::fromStdString(package.execution_info.version.to_string());

  auto normalizeContentId = [](QString value) {
    value = value.trimmed().toUpper();
    if (value.startsWith(QStringLiteral("0X"))) value.remove(0, 2);
    QString compact;
    compact.reserve(value.size());
    for (const auto ch : value) {
      if (ch.isDigit() || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'))) {
        compact.append(ch);
      } else if (!ch.isSpace() && ch != QLatin1Char('-') && ch != QLatin1Char(':')) {
        return QString{};
      }
    }
    return compact.size() == 40 ? compact : QString{};
  };

  auto idsFor = [&](const QVariantMap& item) {
    QStringList ids;
    auto append = [&](const QVariant& value) {
      for (const auto& raw : strings(value)) {
        const auto normalized = normalizeContentId(raw);
        if (!normalized.isEmpty() && !ids.contains(normalized)) ids.append(normalized);
      }
    };
    append(item.value(QStringLiteral("contentIds")));
    append(item.value(QStringLiteral("content_ids")));
    append(item.value(QStringLiteral("contentId")));
    append(item.value(QStringLiteral("content_id")));
    const auto metadata = item.value(QStringLiteral("metadata")).toMap();
    append(metadata.value(QStringLiteral("contentIds")));
    append(metadata.value(QStringLiteral("content_ids")));
    append(metadata.value(QStringLiteral("contentId")));
    append(metadata.value(QStringLiteral("content_id")));
    return ids;
  };

  QVariantMap matched;
  int matched_count = 0;
  for (const auto& value : catalogue) {
    const auto item = value.toMap();
    if (!idsFor(item).contains(content_id)) continue;

    const auto metadata = item.value(QStringLiteral("metadata")).toMap();
    const auto declared_title = normalizeHexId(
        firstString(metadata, {"titleId", "title_id"}));
    const auto declared_media = normalizeHexId(
        firstString(metadata, {"mediaId", "media_id"}));
    const auto declared_version = firstString(
        metadata, {"packageVersion", "xexVersion", "xex_version"});
    if (!declared_title.isEmpty() && declared_title != title_id) continue;
    if (!declared_media.isEmpty() && declared_media != media_id) continue;
    if (!declared_version.isEmpty() &&
        declared_version.compare(package_version, Qt::CaseInsensitive) != 0) continue;

    matched = item;
    ++matched_count;
  }

  if (matched_count == 0) {
    return ServiceResult::failure(
        QStringLiteral("DLC identification"),
        QStringLiteral("STFS content %1 was validated, but the selected module does not declare that package content ID.")
            .arg(content_id));
  }
  if (matched_count > 1) {
    return ServiceResult::failure(
        QStringLiteral("DLC identification"),
        QStringLiteral("More than one DLC catalogue entry declares STFS content ID %1. The module manifest must make package ownership unambiguous.")
            .arg(content_id));
  }

  QVariantMap receipt;
  receipt.insert(QStringLiteral("sourceType"), QStringLiteral("stfs-package"));
  receipt.insert(QStringLiteral("packageType"),
                 QString::fromLatin1(xenon::filesystem::to_string(package.package_type).data(),
                                     static_cast<qsizetype>(xenon::filesystem::to_string(package.package_type).size())));
  receipt.insert(QStringLiteral("contentType"),
                 QString::fromLatin1(xenon::filesystem::to_string(package.content_type).data(),
                                     static_cast<qsizetype>(xenon::filesystem::to_string(package.content_type).size())));
  receipt.insert(QStringLiteral("contentId"), content_id);
  receipt.insert(QStringLiteral("titleId"), title_id);
  receipt.insert(QStringLiteral("mediaId"), media_id);
  receipt.insert(QStringLiteral("packageVersion"), package_version);
  receipt.insert(QStringLiteral("packageDisplayName"), QString::fromStdString(package.display_name));
  receipt.insert(QStringLiteral("packageTitleName"), QString::fromStdString(package.title_name));
  receipt.insert(QStringLiteral("publisher"), QString::fromStdString(package.publisher));
  receipt.insert(QStringLiteral("materialization"), QStringLiteral("extracted-stfs"));

  QVariantMap identified = matched;
  identified.insert(QStringLiteral("contentId"), content_id);
  identified.insert(QStringLiteral("titleId"), title_id);
  identified.insert(QStringLiteral("mediaId"), media_id);
  identified.insert(QStringLiteral("packageVersion"), package_version);
  identified.insert(QStringLiteral("sourceType"), QStringLiteral("stfs-package"));
  identified.insert(QStringLiteral("receipt"), receipt);
  return ServiceResult::success(
      QStringLiteral("DLC identified"),
      QStringLiteral("Validated STFS package %1 and matched %2.")
          .arg(content_id, matched.value(QStringLiteral("name")).toString()),
      identified);
}

ServiceResult XenonContentProbe::materializeDlc(
    const QUrl& source, const QString& destination,
    const QVariantMap& identification) const {
  if (!source.isLocalFile() || destination.trimmed().isEmpty()) {
    return ServiceResult::failure(QStringLiteral("DLC installation"),
                                  QStringLiteral("A local STFS package and managed destination are required."));
  }

  auto package = std::make_shared<xenon::filesystem::StfsPackageSource>(
      localPath(source));
  const auto initialize_error = package->initialize();
  if (initialize_error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(
        QStringLiteral("DLC installation"),
        QStringLiteral("The STFS package changed or failed validation before extraction."));
  }

  const auto expected_id = identification.value(QStringLiteral("contentId")).toString().trimmed().toUpper();
  const auto actual_id = QString::fromStdString(package->info().metadata.content_id_hex);
  if (expected_id.isEmpty() || expected_id != actual_id) {
    return ServiceResult::failure(
        QStringLiteral("DLC installation"),
        QStringLiteral("The package identity changed after identification; Xenon refused to install it."));
  }

  const auto destination_path = std::filesystem::path(
#if defined(Q_OS_WIN)
      destination.toStdWString()
#else
      destination.toStdString()
#endif
  );
  std::error_code ec;
  if (std::filesystem::exists(destination_path, ec)) {
    for (std::filesystem::directory_iterator it(destination_path, ec), end;
         !ec && it != end; it.increment(ec)) {
      return ServiceResult::failure(
          QStringLiteral("DLC installation"),
          QStringLiteral("The managed destination is not empty; Xenon refused to merge package files into stale content."));
    }
    if (ec) {
      return ServiceResult::failure(QStringLiteral("DLC installation"),
                                    QStringLiteral("The managed destination could not be inspected safely."));
    }
  }

  const auto materialize_error =
      xenon::filesystem::materialize_content_source(*package, destination_path);
  if (materialize_error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(
        QStringLiteral("DLC installation"),
        QStringLiteral("STFS extraction failed while materializing the validated package."));
  }

  return ServiceResult::success(
      QStringLiteral("DLC package extracted"),
      QStringLiteral("Validated STFS payload was extracted into Xenon's managed DLC directory."));
}
#endif

}  // namespace xenon::launcher
