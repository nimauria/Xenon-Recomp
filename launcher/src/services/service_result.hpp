#pragma once

#include <QString>
#include <QVariant>

#include <utility>

namespace xenon::launcher {

struct ServiceResult {
  bool ok = false;
  QString title;
  QString message;
  QVariant data;

  static ServiceResult success(QString title = {}, QString message = {}, QVariant data = {}) {
    return ServiceResult{true, std::move(title), std::move(message), std::move(data)};
  }

  static ServiceResult failure(QString title, QString message, QVariant data = {}) {
    return ServiceResult{false, std::move(title), std::move(message), std::move(data)};
  }
};

}  // namespace xenon::launcher
