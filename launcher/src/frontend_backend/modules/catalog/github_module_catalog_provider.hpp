#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QStringList>
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
  [[nodiscard]] static QString registryUrl();
  [[nodiscard]] static QString registryRawBaseUrl();
  [[nodiscard]] static QString hostKey();
  [[nodiscard]] static QVariantList fallbackEntries();

 signals:
  void changed();
  void refreshed();
  void refreshFailed(const QString& message);

 private:
  [[nodiscard]] QStringList parseCatalog(const QByteArray& payload, QString* error) const;
  [[nodiscard]] QVariantMap parseEntry(const QByteArray& payload, const QString& entry_path,
                                       QString* error) const;
  [[nodiscard]] QVariantMap normalizeEntry(const QVariantMap& source,
                                           const QString& entry_path) const;
  [[nodiscard]] static bool safeEntryPath(const QString& path);
  void fetchEntries(const QStringList& paths, quint64 generation);
  void finishEntryRefresh(quint64 generation);
  void setState(const QString& status, const QString& message = {});

  QNetworkAccessManager network_;
  QPointer<QNetworkReply> active_catalog_reply_;
  QList<QPointer<QNetworkReply>> active_entry_replies_;
  QVariantList entries_;
  QVariantMap state_;

  quint64 refresh_generation_ = 0;
  int pending_entry_count_ = 0;
  int expected_entry_count_ = 0;
  QVariantList loading_entries_;
  QStringList loading_errors_;
};

}  // namespace xenon::launcher::frontend_backend
