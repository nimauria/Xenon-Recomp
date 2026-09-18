#include "semantic_version.hpp"

#include <QStringView>

namespace xenon::launcher::frontend_backend {
namespace {

bool parseNumber(const QString& token, int* value) {
  if (token.isEmpty()) return false;
  for (const auto ch : token) {
    if (!ch.isDigit()) return false;
  }
  bool ok = false;
  const auto parsed = token.toInt(&ok);
  if (!ok || parsed < 0) return false;
  *value = parsed;
  return true;
}

bool numericIdentifier(const QString& token, qulonglong* value) {
  if (token.isEmpty()) return false;
  for (const auto ch : token) {
    if (!ch.isDigit()) return false;
  }
  bool ok = false;
  const auto parsed = token.toULongLong(&ok);
  if (!ok) return false;
  *value = parsed;
  return true;
}

}  // namespace

SemanticVersion SemanticVersion::parse(const QString& value) {
  SemanticVersion result;
  auto text = value.trimmed();
  if (text.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) text.remove(0, 1);

  const auto build_separator = text.indexOf(QLatin1Char('+'));
  if (build_separator >= 0) text.truncate(build_separator);

  QString prerelease;
  const auto prerelease_separator = text.indexOf(QLatin1Char('-'));
  if (prerelease_separator >= 0) {
    prerelease = text.mid(prerelease_separator + 1);
    text.truncate(prerelease_separator);
    if (prerelease.isEmpty()) return result;
  }

  const auto core = text.split(QLatin1Char('.'), Qt::KeepEmptyParts);
  if (core.isEmpty() || core.size() > 3) return result;

  int parts[3] = {0, 0, 0};
  for (qsizetype i = 0; i < core.size(); ++i) {
    if (!parseNumber(core.at(i), &parts[i])) return result;
  }

  if (!prerelease.isEmpty()) {
    result.prerelease_ = prerelease.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    for (const auto& identifier : result.prerelease_) {
      if (identifier.isEmpty()) return SemanticVersion{};
      for (const auto ch : identifier) {
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('-'))) return SemanticVersion{};
      }
    }
  }

  result.major_ = parts[0];
  result.minor_ = parts[1];
  result.patch_ = parts[2];
  result.valid_ = true;
  return result;
}

QString SemanticVersion::normalized() const {
  if (!valid_) return {};
  auto result = QStringLiteral("%1.%2.%3").arg(major_).arg(minor_).arg(patch_);
  if (!prerelease_.isEmpty()) result += QLatin1Char('-') + prerelease_.join(QLatin1Char('.'));
  return result;
}

int SemanticVersion::compare(const SemanticVersion& other) const {
  if (!valid_ && !other.valid_) return 0;
  if (!valid_) return -1;
  if (!other.valid_) return 1;

  const int left_core[3] = {major_, minor_, patch_};
  const int right_core[3] = {other.major_, other.minor_, other.patch_};
  for (int i = 0; i < 3; ++i) {
    if (left_core[i] < right_core[i]) return -1;
    if (left_core[i] > right_core[i]) return 1;
  }

  if (prerelease_.isEmpty() && other.prerelease_.isEmpty()) return 0;
  if (prerelease_.isEmpty()) return 1;
  if (other.prerelease_.isEmpty()) return -1;

  const auto count = qMin(prerelease_.size(), other.prerelease_.size());
  for (qsizetype i = 0; i < count; ++i) {
    const auto& left = prerelease_.at(i);
    const auto& right = other.prerelease_.at(i);
    qulonglong left_number = 0;
    qulonglong right_number = 0;
    const auto left_numeric = numericIdentifier(left, &left_number);
    const auto right_numeric = numericIdentifier(right, &right_number);

    if (left_numeric && right_numeric) {
      if (left_number < right_number) return -1;
      if (left_number > right_number) return 1;
      continue;
    }
    if (left_numeric != right_numeric) return left_numeric ? -1 : 1;

    const auto lexical = QString::compare(left, right, Qt::CaseSensitive);
    if (lexical < 0) return -1;
    if (lexical > 0) return 1;
  }

  if (prerelease_.size() < other.prerelease_.size()) return -1;
  if (prerelease_.size() > other.prerelease_.size()) return 1;
  return 0;
}

}  // namespace xenon::launcher::frontend_backend
