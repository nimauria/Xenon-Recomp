#include "xenon/kernel/file_object.hpp"

#include "xenon/kernel/completion_port.hpp"

#include <algorithm>
#include <limits>

namespace xenon::kernel {
namespace {

[[nodiscard]] bool can_access(filesystem::FileAccess granted,
                              filesystem::FileAccess required) noexcept {
  return filesystem::has_access(granted, required);
}

}  // namespace

KernelFileObject::KernelFileObject(
    filesystem::ResolvedPath resolved, filesystem::FileAccess open_access,
    filesystem::ShareAccess share_access, bool synchronous, bool directory,
    std::unique_ptr<filesystem::FileHandle> file,
    std::shared_ptr<FileObjectLease> lease)
    : KernelObject(ObjectType::File),
      resolved_(std::move(resolved)),
      open_access_(open_access),
      share_access_(share_access),
      synchronous_(synchronous),
      directory_(directory),
      file_(std::move(file)),
      lease_(std::move(lease)) {}

KernelFileObject::~KernelFileObject() {
  if (!closed()) on_last_handle_closed();
}

std::string KernelFileObject::path() const {
  std::scoped_lock lock(io_mutex_);
  return resolved_.canonical_path;
}

std::uint64_t KernelFileObject::position() const {
  std::scoped_lock lock(io_mutex_);
  return position_;
}

IoStatus KernelFileObject::set_position(std::uint64_t value) {
  std::scoped_lock lock(io_mutex_);
  if (closed()) return {KernelIoCode::InvalidHandle};
  if (directory_) return {KernelIoCode::InvalidObjectType};
  position_ = value;
  return {KernelIoCode::Success};
}

filesystem::ResolvedPath KernelFileObject::resolved_path() const {
  std::scoped_lock lock(io_mutex_);
  return resolved_;
}

IoStatus KernelFileObject::seek(std::int64_t offset,
                                filesystem::SeekOrigin origin) {
  std::scoped_lock lock(io_mutex_);
  if (closed()) return {KernelIoCode::InvalidHandle};
  if (directory_ || !file_) return {KernelIoCode::InvalidObjectType};

  std::uint64_t base = 0;
  if (origin == filesystem::SeekOrigin::Current) {
    base = position_;
  } else if (origin == filesystem::SeekOrigin::End) {
    auto error = file_->size(base);
    if (error != filesystem::FsError::None) return from_filesystem_error(error);
  }

  if (offset < 0) {
    const auto magnitude =
        static_cast<std::uint64_t>(-(offset + 1)) + std::uint64_t{1};
    if (magnitude > base) return {KernelIoCode::InvalidParameter};
    position_ = base - magnitude;
  } else {
    const auto positive = static_cast<std::uint64_t>(offset);
    if (positive > std::numeric_limits<std::uint64_t>::max() - base) {
      return {KernelIoCode::InvalidParameter};
    }
    position_ = base + positive;
  }
  return {KernelIoCode::Success, filesystem::FsError::None,
          static_cast<std::size_t>(std::min<std::uint64_t>(
              position_, std::numeric_limits<std::size_t>::max()))};
}

std::shared_ptr<IoRequest> KernelFileObject::begin_request(
    IoOperation operation, std::uint64_t context) {
  const auto id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
  auto request = std::make_shared<IoRequest>(id, operation, context);
  std::scoped_lock lock(request_mutex_);
  if (closed()) {
    static_cast<void>(request->cancel());
    return request;
  }
  requests_.emplace(id, request);
  return request;
}

KernelIoCode KernelFileObject::complete_request(std::uint64_t request_id,
                                                IoStatus result) {
  std::shared_ptr<IoRequest> request;
  {
    std::scoped_lock lock(request_mutex_);
    const auto it = requests_.find(request_id);
    if (it == requests_.end()) return KernelIoCode::InvalidParameter;
    request = it->second;
    requests_.erase(it);
  }
  return request->complete(result) ? KernelIoCode::Success
                                   : KernelIoCode::InvalidParameter;
}

KernelIoCode KernelFileObject::cancel_request(std::uint64_t request_id) {
  std::shared_ptr<IoRequest> request;
  {
    std::scoped_lock lock(request_mutex_);
    const auto it = requests_.find(request_id);
    if (it == requests_.end()) return KernelIoCode::InvalidParameter;
    request = it->second;
    requests_.erase(it);
  }
  return request->cancel() ? KernelIoCode::Success
                           : KernelIoCode::InvalidParameter;
}

std::size_t KernelFileObject::active_request_count() const {
  std::scoped_lock lock(request_mutex_);
  return requests_.size();
}

IoStatus KernelFileObject::finish_synchronous(
    const std::shared_ptr<IoRequest>& request, IoStatus status) {
  status.request_id = request->id();
  const auto completion = complete_request(request->id(), status);
  if (completion != KernelIoCode::Success) {
    const auto snapshot = request->snapshot();
    if (snapshot.state == IoRequestState::Cancelled) return snapshot.result;
    if (snapshot.state == IoRequestState::Completed) return snapshot.result;
    return {completion, filesystem::FsError::None, 0, request->id()};
  }

  std::shared_ptr<IoCompletionPort> port;
  std::uint64_t key = 0;
  {
    std::scoped_lock lock(completion_mutex_);
    port = completion_port_.lock();
    key = completion_key_;
  }
  if (port) {
    port->post({key, request->context(), status});
  }
  return status;
}

IoStatus KernelFileObject::read(std::span<std::byte> destination,
                                std::optional<std::uint64_t> byte_offset,
                                bool update_position, std::uint64_t context) {
  auto request = begin_request(IoOperation::Read, context);
  if (request->snapshot().state == IoRequestState::Cancelled) {
    return request->snapshot().result;
  }

  IoStatus result{};
  {
    std::scoped_lock lock(io_mutex_);
    if (closed()) {
      result = {KernelIoCode::InvalidHandle};
    } else if (directory_ || !file_) {
      result = {KernelIoCode::InvalidObjectType};
    } else if (!can_access(open_access_, filesystem::FileAccess::Read)) {
      result = {KernelIoCode::AccessDenied};
    } else {
      const auto offset = byte_offset.value_or(position_);
      std::size_t bytes_read = 0;
      const auto error = file_->read_at(offset, destination, bytes_read);
      result = from_filesystem_error(error, bytes_read);
      if (result.succeeded() && update_position) {
        if (bytes_read > std::numeric_limits<std::uint64_t>::max() - offset) {
          result = {KernelIoCode::InvalidParameter};
        } else {
          position_ = offset + bytes_read;
        }
      }
    }
  }
  return finish_synchronous(request, result);
}

IoStatus KernelFileObject::write(std::span<const std::byte> source,
                                 std::optional<std::uint64_t> byte_offset,
                                 bool update_position, std::uint64_t context) {
  auto request = begin_request(IoOperation::Write, context);
  if (request->snapshot().state == IoRequestState::Cancelled) {
    return request->snapshot().result;
  }

  IoStatus result{};
  {
    std::scoped_lock lock(io_mutex_);
    if (closed()) {
      result = {KernelIoCode::InvalidHandle};
    } else if (directory_ || !file_) {
      result = {KernelIoCode::InvalidObjectType};
    } else if (!can_access(open_access_, filesystem::FileAccess::Write)) {
      result = {KernelIoCode::AccessDenied};
    } else {
      const auto offset = byte_offset.value_or(position_);
      std::size_t bytes_written = 0;
      const auto error = file_->write_at(offset, source, bytes_written);
      result = from_filesystem_error(error, bytes_written);
      if (result.succeeded() && update_position) {
        if (bytes_written > std::numeric_limits<std::uint64_t>::max() - offset) {
          result = {KernelIoCode::InvalidParameter};
        } else {
          position_ = offset + bytes_written;
        }
      }
    }
  }
  return finish_synchronous(request, result);
}

IoStatus KernelFileObject::resize(std::uint64_t new_size,
                                  std::uint64_t context) {
  auto request = begin_request(IoOperation::Resize, context);
  if (request->snapshot().state == IoRequestState::Cancelled) {
    return request->snapshot().result;
  }

  IoStatus result{};
  {
    std::scoped_lock lock(io_mutex_);
    if (closed()) {
      result = {KernelIoCode::InvalidHandle};
    } else if (directory_ || !file_) {
      result = {KernelIoCode::InvalidObjectType};
    } else if (!can_access(open_access_, filesystem::FileAccess::Write)) {
      result = {KernelIoCode::AccessDenied};
    } else {
      result = from_filesystem_error(file_->resize(new_size));
      if (result.succeeded() && position_ > new_size) position_ = new_size;
    }
  }
  return finish_synchronous(request, result);
}

IoStatus KernelFileObject::flush(std::uint64_t context) {
  auto request = begin_request(IoOperation::Flush, context);
  if (request->snapshot().state == IoRequestState::Cancelled) {
    return request->snapshot().result;
  }

  IoStatus result{};
  {
    std::scoped_lock lock(io_mutex_);
    if (closed()) {
      result = {KernelIoCode::InvalidHandle};
    } else if (directory_ || !file_) {
      result = {KernelIoCode::InvalidObjectType};
    } else {
      result = from_filesystem_error(file_->flush());
    }
  }
  return finish_synchronous(request, result);
}

IoStatus KernelFileObject::stat(filesystem::FileInfo& out_info) const {
  std::scoped_lock lock(io_mutex_);
  if (closed()) return {KernelIoCode::InvalidHandle};
  return from_filesystem_error(
      resolved_.device->stat(resolved_.relative_path, out_info));
}

IoStatus KernelFileObject::query_directory(
    const filesystem::DirectoryQuery* query, bool restart,
    std::size_t max_entries, std::vector<filesystem::DirectoryEntry>& out,
    bool& out_end, std::uint64_t context) {
  out.clear();
  out_end = false;
  auto request = begin_request(IoOperation::QueryDirectory, context);
  if (request->snapshot().state == IoRequestState::Cancelled) {
    return request->snapshot().result;
  }

  IoStatus result{};
  {
    std::scoped_lock lock(io_mutex_);
    if (closed()) {
      result = {KernelIoCode::InvalidHandle};
    } else if (!directory_) {
      result = {KernelIoCode::InvalidObjectType};
    } else if (!can_access(open_access_, filesystem::FileAccess::Read)) {
      result = {KernelIoCode::AccessDenied};
    } else {
      if (query) {
        directory_query_ = *query;
        directory_query_.max_entries = 0;
        directory_cursor_.reset();
      }
      if (!directory_cursor_) {
        if (directory_query_.pattern.empty()) directory_query_.pattern = "*";
        const auto error = resolved_.device->open_directory_cursor(
            resolved_.relative_path, directory_query_, directory_cursor_);
        if (error != filesystem::FsError::None) {
          result = from_filesystem_error(error);
        }
      } else if (restart) {
        directory_cursor_->restart();
      }

      if (result.code == KernelIoCode::Success && directory_cursor_) {
        if (directory_cursor_->exhausted()) {
          out_end = true;
          result = {KernelIoCode::NoMoreFiles};
        } else {
          const auto error = directory_cursor_->read(max_entries, out, out_end);
          result = from_filesystem_error(error, out.size());
        }
      }
    }
  }
  return finish_synchronous(request, result);
}

KernelIoCode KernelFileObject::set_delete_pending(bool value) {
  if (!lease_) return KernelIoCode::InvalidParameter;
  if (closed()) return KernelIoCode::InvalidHandle;
  if (!can_access(open_access_, filesystem::FileAccess::Delete)) {
    return KernelIoCode::AccessDenied;
  }
  if (value && resolved_.device && resolved_.device->read_only()) {
    return KernelIoCode::AccessDenied;
  }
  return lease_->set_delete_pending(value);
}

IoStatus KernelFileObject::set_basic_information(const FileBasicInformation& info) {
  std::scoped_lock lock(io_mutex_);
  if (closed()) return {KernelIoCode::InvalidHandle};
  if (!can_access(open_access_, filesystem::FileAccess::Write)) {
    return {KernelIoCode::AccessDenied};
  }
  if (!resolved_.device || resolved_.device->read_only()) {
    return {KernelIoCode::AccessDenied};
  }

  if (info.set_attributes) {
    filesystem::FileAttributeUpdate update{};
    update.mask = filesystem::FileAttributeReadOnly;
    update.value = info.attributes & filesystem::FileAttributeReadOnly;
    const auto error = resolved_.device->set_attributes(resolved_.relative_path, update);
    if (error != filesystem::FsError::None) return from_filesystem_error(error);
  }
  if (info.set_last_write_time) {
    const auto error = resolved_.device->set_last_write_time(
        resolved_.relative_path, info.last_write_time);
    if (error != filesystem::FsError::None) return from_filesystem_error(error);
  }
  return {KernelIoCode::Success};
}

IoStatus KernelFileObject::rename_to(const filesystem::ResolvedPath& target,
                                     bool replace_existing) {
  std::scoped_lock lock(io_mutex_);
  if (closed()) return {KernelIoCode::InvalidHandle};
  if (!can_access(open_access_, filesystem::FileAccess::Delete)) {
    return {KernelIoCode::AccessDenied};
  }
  if (!lease_ || !resolved_.device || !target.device) {
    return {KernelIoCode::InvalidParameter};
  }
  if (resolved_.device.get() != target.device.get()) {
    return {KernelIoCode::CrossDevice};
  }
  const auto result = lease_->rename_to(target, replace_existing);
  if (result != KernelIoCode::Success) return {result};
  resolved_ = target;
  directory_cursor_.reset();
  return {KernelIoCode::Success};
}

void KernelFileObject::associate_completion_port(
    std::shared_ptr<IoCompletionPort> port, std::uint64_t key) {
  std::scoped_lock lock(completion_mutex_);
  completion_port_ = std::move(port);
  completion_key_ = key;
}

void KernelFileObject::clear_completion_port() {
  std::scoped_lock lock(completion_mutex_);
  completion_port_.reset();
  completion_key_ = 0;
}

bool KernelFileObject::delete_pending() const {
  return lease_ && lease_->delete_pending();
}

void KernelFileObject::cancel_all_requests() noexcept {
  std::vector<std::shared_ptr<IoRequest>> requests;
  {
    std::scoped_lock lock(request_mutex_);
    requests.reserve(requests_.size());
    for (auto& [id, request] : requests_) {
      static_cast<void>(id);
      requests.push_back(std::move(request));
    }
    requests_.clear();
  }
  for (const auto& request : requests) {
    if (request) static_cast<void>(request->cancel());
  }
}

void KernelFileObject::on_last_handle_closed() noexcept {
  bool expected = false;
  if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }

  cancel_all_requests();
  {
    std::scoped_lock lock(io_mutex_);
    file_.reset();
    directory_cursor_.reset();
  }
  if (lease_) lease_->release();
}

}  // namespace xenon::kernel
