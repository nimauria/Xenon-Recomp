#include "filesystem_service.hpp"

#include "path_service.hpp"

#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/filesystem/host_path_device.hpp"
#include "xenon/filesystem/read_only_content_device.hpp"
#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/stfs_package_source.hpp"

#include <QDir>
#include <QFileInfo>

namespace xenon::launcher {
namespace {

std::string toStdPath(const QString& path) {
#if defined(Q_OS_WIN)
  return path.toStdString();
#else
  return path.toStdString();
#endif
}

QString fromStdString(const std::string& str) {
#if defined(Q_OS_WIN)
  return QString::fromStdWString(std::filesystem::path(str).native());
#else
  return QString::fromStdString(str);
#endif
}

QString errorMessage(xenon::filesystem::FsError error) {
  using FsError = xenon::filesystem::FsError;
  switch (error) {
    case FsError::None: return QStringLiteral("Success");
    case FsError::NotFound: return QStringLiteral("Not found");
    case FsError::AlreadyExists: return QStringLiteral("Already exists");
    case FsError::AccessDenied: return QStringLiteral("Access denied");
    case FsError::InvalidPath: return QStringLiteral("Invalid path");
    case FsError::InvalidArgument: return QStringLiteral("Invalid argument");
    case FsError::IsDirectory: return QStringLiteral("Is a directory");
    case FsError::NotDirectory: return QStringLiteral("Not a directory");
    case FsError::DirectoryNotEmpty: return QStringLiteral("Directory not empty");
    case FsError::ReadOnly: return QStringLiteral("Read-only filesystem");
    case FsError::SharingViolation: return QStringLiteral("Sharing violation");
    case FsError::CrossDevice: return QStringLiteral("Cross-device operation");
    case FsError::TooManyLinks: return QStringLiteral("Too many symbolic links");
    case FsError::IoError: return QStringLiteral("I/O error");
    case FsError::Unsupported: return QStringLiteral("Operation not supported");
    default: return QStringLiteral("Unknown error");
  }
}

}  // namespace

FilesystemService::FilesystemService(PathService& paths, QObject* parent)
    : QObject(parent), paths_(paths) {
  vfs_ = std::make_shared<xenon::filesystem::VirtualFileSystem>();
}

FilesystemService::~FilesystemService() = default;

std::shared_ptr<xenon::filesystem::VirtualFileSystem> FilesystemService::vfs() {
  if (!vfs_) {
    vfs_ = std::make_shared<xenon::filesystem::VirtualFileSystem>();
  }
  return vfs_;
}

const std::shared_ptr<xenon::filesystem::VirtualFileSystem>& FilesystemService::vfs() const {
  return vfs_;
}

void FilesystemService::resetVfs() {
  vfs_ = std::make_shared<xenon::filesystem::VirtualFileSystem>();
  emit mountsChanged();
  emit symbolicLinksChanged();
  emit workingDirectoryChanged();
}

QVariantList FilesystemService::getMounts() const {
  QVariantList result;
  if (!vfs_) return result;

  const auto mounts = vfs_->mounts();
  for (const auto& mount : mounts) {
    QVariantMap info;
    info.insert(QStringLiteral("mountPoint"), fromStdString(mount.mount_point));
    info.insert(QStringLiteral("readOnly"), mount.read_only);
    result.append(info);
  }
  return result;
}

QVariantList FilesystemService::getSymbolicLinks() const {
  QVariantList result;
  if (!vfs_) return result;

  const auto links = vfs_->symbolic_links();
  for (const auto& link : links) {
    QVariantMap info;
    info.insert(QStringLiteral("alias"), fromStdString(link.alias));
    info.insert(QStringLiteral("target"), fromStdString(link.target));
    result.append(info);
  }
  return result;
}

QString FilesystemService::getWorkingDirectory() const {
  if (!vfs_) return {};
  return fromStdString(vfs_->working_directory());
}

ServiceResult FilesystemService::mountHostPath(const QString& mount_point,
                                               const QString& host_path,
                                               bool read_only) {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("VFS not initialized"));
  }

  const auto clean_path = QDir::cleanPath(host_path);
  const QFileInfo info(clean_path);
  if (!info.exists() || !info.isDir()) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("Host path does not exist or is not a directory:\n%1")
                                      .arg(clean_path));
  }

  xenon::filesystem::HostPathDeviceOptions options;
  options.read_only = read_only;
  auto device = std::make_shared<xenon::filesystem::HostPathDevice>(
      toStdPath(mount_point), toStdPath(clean_path), options);

  const auto error = vfs_->register_device(std::move(device));
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("Could not register device: %1").arg(errorMessage(error)));
  }

  emit mountsChanged();
  return ServiceResult::success(QStringLiteral("Mounted"),
                               QStringLiteral("Mounted %1 to %2").arg(mount_point, clean_path));
}

ServiceResult FilesystemService::mountGdfxImage(const QString& mount_point,
                                                const QString& image_path) {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("VFS not initialized"));
  }

  const auto clean_path = QDir::cleanPath(image_path);
  const QFileInfo info(clean_path);
  if (!info.exists() || !info.isFile()) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("GDFX image does not exist:\n%1").arg(clean_path));
  }

  auto source = std::make_shared<xenon::filesystem::GdfxImageSource>(toStdPath(clean_path));
  const auto init_error = source->initialize();
  if (init_error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("Could not open GDFX image: %1").arg(errorMessage(init_error)));
  }

  auto device = std::make_shared<xenon::filesystem::ReadOnlyContentDevice>(
      toStdPath(mount_point), std::move(source));

  const auto error = vfs_->register_device(std::move(device));
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("Could not register GDFX device: %1").arg(errorMessage(error)));
  }

  emit mountsChanged();
  return ServiceResult::success(QStringLiteral("Mounted GDFX"),
                               QStringLiteral("Mounted GDFX image %1 to %2").arg(clean_path, mount_point));
}

ServiceResult FilesystemService::mountStfsPackage(const QString& mount_point,
                                                  const QString& package_path) {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("VFS not initialized"));
  }

  const auto clean_path = QDir::cleanPath(package_path);
  const QFileInfo info(clean_path);
  if (!info.exists() || !info.isFile()) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("STFS package does not exist:\n%1").arg(clean_path));
  }

  auto source = std::make_shared<xenon::filesystem::StfsPackageSource>(toStdPath(clean_path));
  const auto init_error = source->initialize();
  if (init_error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("Could not open STFS package: %1").arg(errorMessage(init_error)));
  }

  auto device = std::make_shared<xenon::filesystem::ReadOnlyContentDevice>(
      toStdPath(mount_point), std::move(source));

  const auto error = vfs_->register_device(std::move(device));
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Mount failed"),
                                  QStringLiteral("Could not register STFS device: %1").arg(errorMessage(error)));
  }

  emit mountsChanged();
  return ServiceResult::success(QStringLiteral("Mounted STFS"),
                               QStringLiteral("Mounted STFS package %1 to %2").arg(clean_path, mount_point));
}

ServiceResult FilesystemService::unmount(const QString& mount_point) {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Unmount failed"),
                                  QStringLiteral("VFS not initialized"));
  }

  const auto error = vfs_->unregister_device(toStdPath(mount_point));
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Unmount failed"),
                                  QStringLiteral("Could not unmount: %1").arg(errorMessage(error)));
  }

  emit mountsChanged();
  return ServiceResult::success(QStringLiteral("Unmounted"),
                               QStringLiteral("Unmounted %1").arg(mount_point));
}

ServiceResult FilesystemService::registerSymbolicLink(const QString& alias,
                                                     const QString& target) {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Symbolic link failed"),
                                  QStringLiteral("VFS not initialized"));
  }

  const auto error = vfs_->register_symbolic_link(toStdPath(alias), toStdPath(target));
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Symbolic link failed"),
                                  QStringLiteral("Could not register: %1").arg(errorMessage(error)));
  }

  emit symbolicLinksChanged();
  return ServiceResult::success(QStringLiteral("Symbolic link registered"),
                               QStringLiteral("Registered %1 -> %2").arg(alias, target));
}

ServiceResult FilesystemService::unregisterSymbolicLink(const QString& alias) {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Symbolic link removal failed"),
                                  QStringLiteral("VFS not initialized"));
  }

  const auto error = vfs_->unregister_symbolic_link(toStdPath(alias));
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Symbolic link removal failed"),
                                  QStringLiteral("Could not unregister: %1").arg(errorMessage(error)));
  }

  emit symbolicLinksChanged();
  return ServiceResult::success(QStringLiteral("Symbolic link removed"),
                               QStringLiteral("Removed %1").arg(alias));
}

ServiceResult FilesystemService::setWorkingDirectory(const QString& guest_path) {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Working directory failed"),
                                  QStringLiteral("VFS not initialized"));
  }

  const auto error = vfs_->set_working_directory(toStdPath(guest_path));
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Working directory failed"),
                                  QStringLiteral("Could not set: %1").arg(errorMessage(error)));
  }

  emit workingDirectoryChanged();
  return ServiceResult::success(QStringLiteral("Working directory set"),
                               QStringLiteral("Set to %1").arg(guest_path));
}

QVariantMap FilesystemService::getFilesystemStatus() const {
  QVariantMap status;
  status.insert(QStringLiteral("initialized"), vfs_ != nullptr);
  
  if (vfs_) {
    status.insert(QStringLiteral("mountCount"), getMounts().size());
    status.insert(QStringLiteral("symbolicLinkCount"), getSymbolicLinks().size());
    status.insert(QStringLiteral("workingDirectory"), getWorkingDirectory());
  } else {
    status.insert(QStringLiteral("mountCount"), 0);
    status.insert(QStringLiteral("symbolicLinkCount"), 0);
    status.insert(QStringLiteral("workingDirectory"), QString{});
  }
  
  return status;
}

ServiceResult FilesystemService::testPath(const QString& guest_path) const {
  if (!vfs_) {
    return ServiceResult::failure(QStringLiteral("Path test"),
                                  QStringLiteral("VFS not initialized"));
  }

  xenon::filesystem::FileInfo info;
  const auto error = vfs_->stat(toStdPath(guest_path), info);
  
  if (error != xenon::filesystem::FsError::None) {
    return ServiceResult::failure(QStringLiteral("Path test"),
                                  QStringLiteral("Path %1: %2").arg(guest_path, errorMessage(error)));
  }

  QVariantMap data;
  data.insert(QStringLiteral("exists"), true);
  data.insert(QStringLiteral("isDirectory"), info.is_directory);
  data.insert(QStringLiteral("size"), static_cast<qint64>(info.size));
  
  return ServiceResult::success(QStringLiteral("Path test"),
                               QStringLiteral("Path %1 exists").arg(guest_path),
                               data);
}

}  // namespace xenon::launcher
