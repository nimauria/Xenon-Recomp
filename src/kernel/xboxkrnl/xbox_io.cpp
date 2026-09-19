#include "xenon/kernel/xbox_io.hpp"

#include <algorithm>
#include <limits>

namespace xenon::kernel::xbox {
namespace {

[[nodiscard]] filesystem::FileAccess decode_access(std::uint32_t raw) noexcept {
  filesystem::FileAccess result = filesystem::FileAccess::None;
  if (raw & (access::GenericRead | access::FileReadData |
             access::FileReadAttributes | access::FileExecute |
             access::GenericExecute | access::GenericAll)) {
    result = result | filesystem::FileAccess::Read;
  }
  if (raw & (access::GenericWrite | access::FileWriteData |
             access::FileAppendData | access::FileWriteAttributes |
             access::GenericAll)) {
    result = result | filesystem::FileAccess::Write;
  }
  if (raw & (access::Delete | access::FileDeleteChild | access::GenericAll)) {
    result = result | filesystem::FileAccess::Delete;
  }
  // Metadata-only opens are common. Keep the object readable internally so
  // stat/query operations have a portable backing handle model.
  if (result == filesystem::FileAccess::None) result = filesystem::FileAccess::Read;
  return result;
}

[[nodiscard]] filesystem::ShareAccess decode_share(std::uint32_t raw) noexcept {
  filesystem::ShareAccess result = filesystem::ShareAccess::None;
  if (raw & share::Read) result = result | filesystem::ShareAccess::Read;
  if (raw & share::Write) result = result | filesystem::ShareAccess::Write;
  if (raw & share::Delete) result = result | filesystem::ShareAccess::Delete;
  return result;
}

[[nodiscard]] std::uint32_t encode_action(filesystem::OpenAction action) noexcept {
  switch (action) {
    case filesystem::OpenAction::Superseded:
      return static_cast<std::uint32_t>(FileAction::Superseded);
    case filesystem::OpenAction::Opened:
      return static_cast<std::uint32_t>(FileAction::Opened);
    case filesystem::OpenAction::Created:
      return static_cast<std::uint32_t>(FileAction::Created);
    case filesystem::OpenAction::Overwritten:
      return static_cast<std::uint32_t>(FileAction::Overwritten);
    case filesystem::OpenAction::None:
      return 0;
  }
  return 0;
}

[[nodiscard]] bool is_async_handle(KernelIoManager& io, Handle handle) {
  FileInformation info = FileModeInformation{};
  const auto query = io.query_information(handle, ::xenon::kernel::FileInformationClass::Mode, info);
  if (!query.succeeded()) return false;
  const auto* mode = std::get_if<FileModeInformation>(&info);
  return mode && !mode->synchronous;
}

}  // namespace

Status map_status(KernelIoCode value) noexcept {
  switch (value) {
    case KernelIoCode::Success: return status::Success;
    case KernelIoCode::Pending: return status::Pending;
    case KernelIoCode::Cancelled: return status::Cancelled;
    case KernelIoCode::Timeout: return status::Timeout;
    case KernelIoCode::InvalidHandle: return status::InvalidHandle;
    case KernelIoCode::InvalidObjectType: return status::ObjectTypeMismatch;
    case KernelIoCode::InvalidParameter: return status::InvalidParameter;
    case KernelIoCode::AccessDenied: return status::AccessDenied;
    case KernelIoCode::ProtectedHandle: return status::AccessDenied;
    case KernelIoCode::NotFound: return status::ObjectNameNotFound;
    case KernelIoCode::AlreadyExists: return status::ObjectNameCollision;
    case KernelIoCode::NotDirectory: return status::NotADirectory;
    case KernelIoCode::IsDirectory: return status::FileIsDirectory;
    case KernelIoCode::DirectoryNotEmpty: return status::DirectoryNotEmpty;
    case KernelIoCode::SharingViolation: return status::SharingViolation;
    case KernelIoCode::Unsupported: return status::NotSupported;
    case KernelIoCode::CrossDevice: return status::NotSameDevice;
    case KernelIoCode::NoMoreFiles: return status::NoMoreFiles;
    case KernelIoCode::DeletePending: return status::DeletePending;
    case KernelIoCode::FilesystemError: return status::Unsuccessful;
  }
  return status::Unsuccessful;
}

Status map_status(const IoStatus& value) noexcept { return map_status(value.code); }

bool valid_path(std::string_view path, bool pattern) noexcept {
  bool got_asterisk = false;
  for (const unsigned char c : path) {
    if (c <= 31 || c >= 127) return false;
    if (got_asterisk) {
      if (c != '.') return false;
      got_asterisk = false;
    }
    switch (c) {
      case '"':
      case '+':
      case ',':
      case '<':
      case '>':
      case '|':
        return false;
      case '*':
        if (!pattern) return false;
        got_asterisk = true;
        break;
      case '?':
        if (!pattern) return false;
        break;
      default:
        break;
    }
  }
  return true;
}

bool IoFacade::decode_open_options(const CreateFileRequest& request,
                                   KernelOpenOptions& out_options,
                                   Status& out_error) {
  out_options = {};
  out_error = status::Success;
  if (request.creation_disposition > 5 || (request.share_access & ~share::All) != 0) {
    out_error = status::InvalidParameter;
    return false;
  }
  const bool directory = (request.create_options & create_option::DirectoryFile) != 0;
  const bool non_directory =
      (request.create_options & create_option::NonDirectoryFile) != 0;
  if (directory && non_directory) {
    out_error = status::InvalidParameter;
    return false;
  }
  if ((request.create_options & create_option::SequentialOnly) &&
      (request.create_options & create_option::RandomAccess)) {
    out_error = status::InvalidParameter;
    return false;
  }

  constexpr std::uint32_t kAcceptedOptions =
      create_option::DirectoryFile | create_option::WriteThrough |
      create_option::SequentialOnly | create_option::NoIntermediateBuffering |
      create_option::SynchronousIoAlert | create_option::SynchronousIoNonAlert |
      create_option::NonDirectoryFile | create_option::RandomAccess |
      create_option::DeleteOnClose;
  if ((request.create_options & ~kAcceptedOptions) != 0) {
    out_error = status::NotSupported;
    return false;
  }

  out_options.file.access = decode_access(request.desired_access);
  out_options.file.share = decode_share(request.share_access);
  out_options.file.disposition =
      static_cast<filesystem::CreateDisposition>(request.creation_disposition);
  out_options.kind = directory ? OpenKind::Directory
                               : (non_directory ? OpenKind::File : OpenKind::Any);
  out_options.synchronous =
      (request.create_options & (create_option::SynchronousIoAlert |
                                 create_option::SynchronousIoNonAlert)) != 0;
  out_options.delete_on_close =
      (request.create_options & create_option::DeleteOnClose) != 0;
  out_options.handle_flags = request.handle_flags;
  return true;
}

Status IoFacade::create_file(const CreateFileRequest& request,
                             Handle& out_handle, IoStatusBlock& iosb) {
  out_handle = kInvalidHandle;
  iosb = {};
  if (request.path.empty() || !valid_path(request.path, false)) {
    iosb.status = status::ObjectNameInvalid;
    return iosb.status;
  }

  KernelOpenOptions options;
  Status decode_error{};
  if (!decode_open_options(request, options, decode_error)) {
    iosb.status = decode_error;
    return iosb.status;
  }

  filesystem::OpenAction action = filesystem::OpenAction::None;
  IoStatus result{};
  const Handle root = request.root_directory == ObDosDevices
                          ? kInvalidHandle
                          : request.root_directory;
  if (root == kInvalidHandle) {
    result = io_.open(request.path, options, out_handle, &action);
  } else {
    result = io_.open_at(root, request.path, options, out_handle, &action);
  }
  iosb.status = map_status(result);
  if (result.succeeded()) {
    iosb.information = encode_action(action);
  } else if (result.code == KernelIoCode::AlreadyExists &&
             request.creation_disposition == 2) {
    iosb.information = static_cast<std::uint32_t>(FileAction::Exists);
  } else if (result.code == KernelIoCode::NotFound &&
             (request.creation_disposition == 1 ||
              request.creation_disposition == 4)) {
    iosb.information = static_cast<std::uint32_t>(FileAction::DoesNotExist);
  } else {
    iosb.information = 0;
  }
  return iosb.status;
}

Status IoFacade::open_file(const CreateFileRequest& request,
                           Handle& out_handle, IoStatusBlock& iosb) {
  auto open = request;
  open.creation_disposition = 1;
  return create_file(open, out_handle, iosb);
}

Status IoFacade::signal_event_if_present(Handle event, Status current) {
  if (event == kInvalidHandle) return current;
  const auto event_status = io_.set_event(event);
  return event_status == KernelIoCode::Success ? current : map_status(event_status);
}

Status IoFacade::finish_io_status(const IoStatus& result, IoStatusBlock& iosb,
                                  bool asynchronous_handle, Handle event) {
  iosb.status = map_status(result);
  iosb.information = static_cast<std::uint32_t>(std::min<std::size_t>(
      result.information, std::numeric_limits<std::uint32_t>::max()));
  const auto signaled = signal_event_if_present(event, iosb.status);
  if (signaled != iosb.status) {
    iosb.status = signaled;
    iosb.information = 0;
    return signaled;
  }
  // Match the ReXGlue/Xenia contract used by recomp titles today: the host I/O
  // may complete immediately while a non-synchronous Xbox file handle returns
  // STATUS_PENDING. The IOSB still contains the completed operation status.
  if (asynchronous_handle && result.code == KernelIoCode::Success) {
    return status::Pending;
  }
  return iosb.status;
}

Status IoFacade::read_file(Handle handle, std::span<std::byte> destination,
                           std::optional<std::uint64_t> byte_offset,
                           IoStatusBlock& iosb, Handle event,
                           std::uint64_t context) {
  const auto result = io_.read(handle, destination, byte_offset, true, context);
  return finish_io_status(result, iosb, is_async_handle(io_, handle), event);
}

Status IoFacade::write_file(Handle handle, std::span<const std::byte> source,
                            std::optional<std::uint64_t> byte_offset,
                            IoStatusBlock& iosb, Handle event,
                            std::uint64_t context) {
  const auto result = io_.write(handle, source, byte_offset, true, context);
  return finish_io_status(result, iosb, is_async_handle(io_, handle), event);
}

Status IoFacade::flush_buffers_file(Handle handle, IoStatusBlock& iosb,
                                    std::uint64_t context) {
  const auto result = io_.flush(handle, context);
  iosb.status = map_status(result);
  iosb.information = 0;
  return iosb.status;
}

Status IoFacade::query_directory_file(
    Handle handle, std::string_view pattern, bool restart_scan,
    std::size_t max_entries, std::vector<DirectoryInformation>& out,
    IoStatusBlock& iosb, Handle event, std::uint64_t context) {
  out.clear();
  iosb = {};
  if (!pattern.empty() && !valid_path(pattern, true)) {
    iosb.status = status::InvalidParameter;
    return iosb.status;
  }
  filesystem::DirectoryQuery query{};
  query.pattern = pattern.empty() ? "*" : std::string(pattern);
  std::vector<filesystem::DirectoryEntry> entries;
  bool end = false;
  const filesystem::DirectoryQuery* query_ptr = pattern.empty() && !restart_scan
                                                    ? nullptr
                                                    : &query;
  const auto result = io_.query_directory(handle, query_ptr, restart_scan,
                                           max_entries, entries, end, context);
  if (!result.succeeded()) {
    iosb.status = map_status(result);
    iosb.information = 0;
    return signal_event_if_present(event, iosb.status);
  }
  out.reserve(entries.size());
  for (auto& entry : entries) {
    out.push_back({directory_index_++, std::move(entry)});
  }
  iosb.status = status::Success;
  iosb.information = static_cast<std::uint32_t>(out.size());
  return signal_event_if_present(event, iosb.status);
}

bool IoFacade::decode_file_information_class(
    std::uint32_t raw, ::xenon::kernel::FileInformationClass& out_class,
    bool for_set) noexcept {
  switch (static_cast<FileInformationClass>(raw)) {
    case FileInformationClass::Basic:
      out_class = ::xenon::kernel::FileInformationClass::Basic;
      return true;
    case FileInformationClass::Standard:
      if (for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Standard;
      return true;
    case FileInformationClass::Internal:
      if (for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Internal;
      return true;
    case FileInformationClass::Name:
      if (for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Name;
      return true;
    case FileInformationClass::Rename:
      if (!for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Rename;
      return true;
    case FileInformationClass::Disposition:
      if (!for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Disposition;
      return true;
    case FileInformationClass::Position:
      out_class = ::xenon::kernel::FileInformationClass::Position;
      return true;
    case FileInformationClass::Mode:
      if (for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Mode;
      return true;
    case FileInformationClass::Alignment:
      if (for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Alignment;
      return true;
    case FileInformationClass::Allocation:
      if (!for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Allocation;
      return true;
    case FileInformationClass::EndOfFile:
      if (!for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::EndOfFile;
      return true;
    case FileInformationClass::Completion:
      if (!for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::Completion;
      return true;
    case FileInformationClass::NetworkOpen:
      if (for_set) return false;
      out_class = ::xenon::kernel::FileInformationClass::NetworkOpen;
      return true;
    default:
      return false;
  }
}

Status IoFacade::query_information_file(Handle handle, std::uint32_t info_class,
                                        FileInformation& out,
                                        IoStatusBlock& iosb) {
  ::xenon::kernel::FileInformationClass mapped{};
  if (!decode_file_information_class(info_class, mapped, false)) {
    iosb = {status::InvalidInfoClass, 0};
    return iosb.status;
  }
  const auto result = io_.query_information(handle, mapped, out);
  iosb.status = map_status(result);
  iosb.information = result.succeeded()
                         ? static_cast<std::uint32_t>(std::min<std::size_t>(
                               result.information,
                               std::numeric_limits<std::uint32_t>::max()))
                         : 0;
  return iosb.status;
}

Status IoFacade::set_information_file(Handle handle, std::uint32_t info_class,
                                      const FileInformation& value,
                                      IoStatusBlock& iosb,
                                      std::uint64_t context) {
  ::xenon::kernel::FileInformationClass mapped{};
  if (!decode_file_information_class(info_class, mapped, true)) {
    iosb = {status::InvalidInfoClass, 0};
    return iosb.status;
  }
  const auto result = io_.set_information(handle, mapped, value, context);
  iosb.status = map_status(result);
  iosb.information = result.succeeded()
                         ? static_cast<std::uint32_t>(std::min<std::size_t>(
                               result.information,
                               std::numeric_limits<std::uint32_t>::max()))
                         : 0;
  return iosb.status;
}

Status IoFacade::query_volume_information_file(
    Handle handle, std::uint32_t info_class, VolumeInformation& out,
    IoStatusBlock& iosb) {
  kernel::VolumeInformation base{};
  const auto result = io_.query_volume_information(handle, base);
  if (!result.succeeded()) {
    iosb = {map_status(result), 0};
    return iosb.status;
  }

  switch (static_cast<FsInformationClass>(info_class)) {
    case FsInformationClass::Volume:
      out = FsVolumeInformation{0, 0, false, {}};
      break;
    case FsInformationClass::Size: {
      const std::uint64_t unit =
          static_cast<std::uint64_t>(base.sectors_per_allocation_unit) *
          base.bytes_per_sector;
      FsSizeInformation value{};
      value.sectors_per_allocation_unit = base.sectors_per_allocation_unit;
      value.bytes_per_sector = base.bytes_per_sector;
      if (unit != 0) {
        value.total_allocation_units = base.space.capacity / unit;
        value.available_allocation_units = base.space.available / unit;
      }
      out = value;
      break;
    }
    case FsInformationClass::Device:
      out = FsDeviceInformation{};
      break;
    case FsInformationClass::Attribute:
      out = FsAttributeInformation{base.filesystem_attributes,
                                   static_cast<std::int32_t>(
                                       base.component_name_max_length),
                                   base.name};
      break;
    default:
      iosb = {status::InvalidInfoClass, 0};
      return iosb.status;
  }
  iosb = {status::Success, 1};
  return iosb.status;
}

Status IoFacade::query_full_attributes_file(
    Handle root_directory, std::string_view path,
    FileNetworkOpenInformation& out, IoStatusBlock& iosb) {
  iosb = {};
  if (path.empty() || !valid_path(path, false)) {
    iosb.status = status::ObjectNameInvalid;
    return iosb.status;
  }
  filesystem::FileInfo info{};
  const Handle root = root_directory == ObDosDevices ? kInvalidHandle : root_directory;
  const auto result = root == kInvalidHandle
                          ? io_.stat_path(path, info)
                          : io_.stat_path_at(root, path, info);
  iosb.status = map_status(result);
  if (!result.succeeded()) return iosb.status;
  out = FileNetworkOpenInformation{info};
  iosb.information = sizeof(FileNetworkOpenInformation);
  return iosb.status;
}

}  // namespace xenon::kernel::xbox
