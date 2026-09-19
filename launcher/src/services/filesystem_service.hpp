#pragma once

#include "service_result.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace xenon::filesystem {
class VirtualFileSystem;
}

namespace xenon::launcher {

class PathService;

// Xenon VFS management service for the launcher. Provides mount/unmount operations,
// symbolic link management, and filesystem status queries for the frontend.
//
// This service bridges the Qt launcher and the host-independent Xenon VFS. Device
// registration (like game content, DLC folders, cache directories) typically happens
// at launch time through LaunchService, while this service exposes diagnostics and
// manual mount operations for advanced users.
class FilesystemService final : public QObject {
  Q_OBJECT

 public:
  explicit FilesystemService(PathService& paths, QObject* parent = nullptr);
  ~FilesystemService() override;

  FilesystemService(const FilesystemService&) = delete;
  FilesystemService& operator=(const FilesystemService&) = delete;

  // Get or create the shared VFS instance. The launcher creates one VFS per runtime
  // session; multiple services may reference it but ownership remains here.
  [[nodiscard]] std::shared_ptr<xenon::filesystem::VirtualFileSystem> vfs();
  [[nodiscard]] const std::shared_ptr<xenon::filesystem::VirtualFileSystem>& vfs() const;

  // Reset the VFS (clear all devices/mounts). Used when starting a new session.
  void resetVfs();

  // Query current mounts and symbolic links
  [[nodiscard]] QVariantList getMounts() const;
  [[nodiscard]] QVariantList getSymbolicLinks() const;
  [[nodiscard]] QString getWorkingDirectory() const;

  // Mount management
  [[nodiscard]] ServiceResult mountHostPath(const QString& mount_point,
                                           const QString& host_path, 
                                           bool read_only = false);
  [[nodiscard]] ServiceResult mountGdfxImage(const QString& mount_point,
                                            const QString& image_path);
  [[nodiscard]] ServiceResult mountStfsPackage(const QString& mount_point,
                                              const QString& package_path);
  [[nodiscard]] ServiceResult unmount(const QString& mount_point);

  // Symbolic link management
  [[nodiscard]] ServiceResult registerSymbolicLink(const QString& alias,
                                                  const QString& target);
  [[nodiscard]] ServiceResult unregisterSymbolicLink(const QString& alias);

  // Working directory
  [[nodiscard]] ServiceResult setWorkingDirectory(const QString& guest_path);

  // Diagnostics
  [[nodiscard]] QVariantMap getFilesystemStatus() const;
  [[nodiscard]] ServiceResult testPath(const QString& guest_path) const;

 signals:
  void mountsChanged();
  void symbolicLinksChanged();
  void workingDirectoryChanged();

 private:
  PathService& paths_;
  std::shared_ptr<xenon::filesystem::VirtualFileSystem> vfs_;
};

}  // namespace xenon::launcher
