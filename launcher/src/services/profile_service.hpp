#pragma once

#include "service_result.hpp"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {

class PathService;
class SettingsService;

class ProfileService final : public QObject {
  Q_OBJECT

 public:
  ProfileService(SettingsService& settings, PathService& paths, QObject* parent = nullptr);

  void initialize(const QString& primary_name);
  [[nodiscard]] QVariantList profiles() const;
  [[nodiscard]] QVariantMap profile(int index) const;
  [[nodiscard]] QVariantMap activeProfile() const;
  [[nodiscard]] int activeIndex() const noexcept;
  [[nodiscard]] bool nameAvailable(const QString& name, int exclude_index = -1) const;
  [[nodiscard]] QString newProfileId() const;

  [[nodiscard]] ServiceResult create(const QVariantMap& data);
  [[nodiscard]] ServiceResult update(int index, const QVariantMap& data);
  [[nodiscard]] ServiceResult duplicate(int index);
  [[nodiscard]] ServiceResult activate(int index);
  [[nodiscard]] ServiceResult remove(int index);
  [[nodiscard]] ServiceResult exportProfile(int index, const QUrl& destination) const;
  [[nodiscard]] ServiceResult importProfile(const QUrl& source);

  [[nodiscard]] QString loadState() const;
  [[nodiscard]] ServiceResult saveState(const QString& json);
  [[nodiscard]] ServiceResult importAvatar(const QString& profile_id, const QUrl& source_url);
  [[nodiscard]] bool removeAvatar(const QString& profile_id) const;

 signals:
  void changed();

 private:
  [[nodiscard]] QString profileRoot() const;
  [[nodiscard]] QVariantMap defaultProfile(const QString& name, bool active,
                                           const QString& profile_id = {}) const;
  [[nodiscard]] QString uniqueCopyName(const QString& base_name) const;
  [[nodiscard]] bool persistProfiles();
  void normalizeLoadedProfiles(const QVariantList& parsed, const QString& primary_name);

  SettingsService& settings_;
  PathService& paths_;
  QVariantList profiles_;
  int active_index_ = 0;
};

}  // namespace xenon::launcher
