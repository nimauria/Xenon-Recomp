#pragma once

#include <QString>
#include <QStringList>

namespace xenon::launcher::frontend_backend {

class SemanticVersion final {
 public:
  SemanticVersion() = default;

  [[nodiscard]] static SemanticVersion parse(const QString& value);

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] QString normalized() const;
  [[nodiscard]] int compare(const SemanticVersion& other) const;

 private:
  bool valid_ = false;
  int major_ = 0;
  int minor_ = 0;
  int patch_ = 0;
  QStringList prerelease_;
};

}  // namespace xenon::launcher::frontend_backend
