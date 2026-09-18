#pragma once

#include "update_provider.hpp"

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>

#include <memory>

class QNetworkReply;

namespace xenon::launcher::frontend_backend {

class GitHubReleaseProvider final : public UpdateProvider {
  Q_OBJECT

 public:
  explicit GitHubReleaseProvider(QString repository, QObject* parent = nullptr);

  void check(bool include_prerelease) override;
  void download(const UpdateRelease& release, const QString& target_path) override;
  void cancel() override;

  [[nodiscard]] QString repository() const noexcept { return repository_; }
  [[nodiscard]] static QString hostAssetName();

 private:
  [[nodiscard]] QNetworkRequest apiRequest(const QUrl& url) const;
  [[nodiscard]] QNetworkRequest assetRequest(const QUrl& url) const;
  [[nodiscard]] UpdateRelease parseBestRelease(const QByteArray& payload, bool include_prerelease,
                                               QString* error) const;

  QString repository_;
  QNetworkAccessManager network_;
  QPointer<QNetworkReply> active_reply_;
  std::unique_ptr<QSaveFile> download_file_;
  std::unique_ptr<QCryptographicHash> download_hash_;
  QString download_target_;
  UpdateRelease download_release_;
};

}  // namespace xenon::launcher::frontend_backend
