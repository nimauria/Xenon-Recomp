#pragma once

#include "../../services/profile_service.hpp"
#include "../../services/service_result.hpp"

#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {
class LibraryService;
class ModuleService;
class PathService;
}

namespace xenon::launcher::frontend_backend {
class RuntimeFeature;
class SettingsFeature;

class ProfilesFeature final : public QObject {
  Q_OBJECT

 public:
  ProfilesFeature(ProfileService& profiles, PathService& paths, LibraryService& library,
                  ModuleService& modules, SettingsFeature& settings, RuntimeFeature& runtime,
                  bool test_mode, QObject* parent = nullptr);

  void initialize(const QString& primary_name);
  void reload();
  void refreshDerivedState();
  [[nodiscard]] QVariantList entries() const;
  [[nodiscard]] QVariantMap profile(int index) const;
  [[nodiscard]] QVariantMap activeProfile() const;
  [[nodiscard]] int activeIndex() const noexcept;
  [[nodiscard]] int profileNameLimit() const noexcept { return 48; }
  [[nodiscard]] int descriptionLimit() const noexcept { return 180; }
  [[nodiscard]] bool nameAvailable(const QString& name, int exclude_index = -1) const;

  [[nodiscard]] QVariantList actions(int index) const;
  [[nodiscard]] QVariantList backgroundActions() const;
  [[nodiscard]] QVariantList runtimeDefinitions() const;
  [[nodiscard]] QVariantMap runtimeDefaults() const;
  [[nodiscard]] QVariantMap resolvedRuntimeSettings(const QVariantMap& profile) const;
  [[nodiscard]] QString storagePath(int index) const;
  [[nodiscard]] ServiceResult ensureStorage(int index) const;

  [[nodiscard]] ServiceResult create(const QVariantMap& data);
  [[nodiscard]] ServiceResult update(int index, const QVariantMap& data);
  [[nodiscard]] ServiceResult duplicate(int index);
  [[nodiscard]] ServiceResult activate(int index);
  [[nodiscard]] ServiceResult remove(int index);
  [[nodiscard]] ServiceResult exportProfile(int index, const QUrl& destination) const;
  [[nodiscard]] ServiceResult importProfile(const QUrl& source);
  [[nodiscard]] ServiceResult persistToCurrentStorage();

  [[nodiscard]] ServiceResult importAvatar(const QString& profile_id, const QUrl& source_url);
  [[nodiscard]] bool removeAvatar(const QString& profile_id);
  [[nodiscard]] ServiceResult removeAvatarForProfile(int index);

 signals:
  void changed();

 private:
  [[nodiscard]] QVariantList sourceEntries() const;
  [[nodiscard]] QVariantMap decorated(QVariantMap profile) const;
  [[nodiscard]] QVariantMap fixtureProfile(const QString& name, const QString& description,
                                           bool active, const QString& id) const;
  [[nodiscard]] QString fixtureCopyName(const QString& base_name) const;
  [[nodiscard]] QString newProfileId() const;
  [[nodiscard]] ServiceResult prepareAvatar(QVariantMap& data, int existing_index = -1);
  [[nodiscard]] ServiceResult normalizeProfileData(QVariantMap& data) const;
  [[nodiscard]] int saveSetCount(const QString& path) const;
  void resetFixtures(const QString& primary_name);

  ProfileService& profiles_;
  PathService& paths_;
  LibraryService& library_;
  ModuleService& modules_;
  SettingsFeature& settings_;
  RuntimeFeature& runtime_;
  bool test_mode_ = false;
  QVariantList fixture_profiles_;
  int fixture_active_index_ = 0;
  QString primary_name_;
};

}  // namespace xenon::launcher::frontend_backend
