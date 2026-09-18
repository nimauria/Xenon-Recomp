#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

class QNetworkReply;

namespace xenon::launcher::frontend_backend {

class GitHubModuleCatalogProvider final : public QObject {
  Q_OBJECT

 public:
  explicit GitHubModuleCatalogProvider(QObject* parent = nullptr);

  void refresh();
  void cancel();

  [[nodiscard]] QVariantList entries() const { return entries_; }
  [[nodiscard]] QVariantMap state() const { return state_; }

  [[nodiscard]] static QString catalogUrl();
  [[nodiscard]] static QString hostKey();
  [[nodiscard]] static QVariantList fallbackEntries();

 signals:
  void changed();
  void refreshed();
  void refreshFailed(const QString& message);

 private:
  [[nodiscard]] QVariantList parse(const QByteArray& payload, QString* error) const;
  [[nodiscard]] QVariantMap normalizeEntry(const QVariantMap& source) const;
  void setState(const QString& status, const QString& message = {});

  QNetworkAccessManager network_;
  QPointer<QNetworkReply> active_reply_;
  QVariantList entries_;
  QVariantMap state_;
};

}  // namespace xenon::launcher::frontend_backend
