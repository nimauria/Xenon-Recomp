// Asynchronous I/O requests, completion ports and kernel events.

#include "kernel/handles/io_manager/io_manager_internal.hpp"

namespace xenon::kernel {

IoStatus KernelIoManager::begin_request(Handle handle, IoOperation operation,
                                        std::uint64_t context,
                                        std::shared_ptr<IoRequest>& out_request) {
  out_request.reset();
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status;
  out_request = file->begin_request(operation, context);
  const auto snapshot = out_request->snapshot();
  if (snapshot.state == IoRequestState::Cancelled) return snapshot.result;
  return {KernelIoCode::Pending, filesystem::FsError::None, 0, out_request->id()};
}

KernelIoCode KernelIoManager::complete_request(Handle handle,
                                               std::uint64_t request_id,
                                               IoStatus result) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  const auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status.code;
  return file->complete_request(request_id, result);
}

KernelIoCode KernelIoManager::cancel_request(Handle handle,
                                             std::uint64_t request_id) {
  HandleView view;
  std::shared_ptr<KernelFileObject> file;
  const auto status = lookup_file(handle, view, file);
  if (!status.succeeded()) return status.code;
  return file->cancel_request(request_id);
}

KernelIoCode KernelIoManager::create_completion_port(Handle& out_handle) {
  auto port = std::make_shared<IoCompletionPort>();
  return table().insert(port, 0xFFFFFFFFu, HandleFlags::None, out_handle);
}

KernelIoCode KernelIoManager::associate_completion_port(
    Handle file_handle, Handle port_handle, std::uint64_t key) {
  HandleView file_view;
  std::shared_ptr<KernelFileObject> file;
  const auto file_status = lookup_file(file_handle, file_view, file);
  if (!file_status.succeeded()) return file_status.code;

  HandleView port_view;
  const auto lookup = table().lookup(port_handle, port_view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!port_view.object || port_view.object->type() != ObjectType::IoCompletionPort) {
    return KernelIoCode::InvalidObjectType;
  }
  file->associate_completion_port(
      std::static_pointer_cast<IoCompletionPort>(port_view.object), key);
  return KernelIoCode::Success;
}

KernelIoCode KernelIoManager::remove_completion(
    Handle port_handle, CompletionPacket& out_packet,
    std::chrono::milliseconds timeout) {
  HandleView view;
  const auto lookup = table().lookup(port_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::IoCompletionPort) {
    return KernelIoCode::InvalidObjectType;
  }
  auto port = std::static_pointer_cast<IoCompletionPort>(view.object);
  const bool removed = timeout.count() == 0
                           ? port->try_remove(out_packet)
                           : port->remove_for(timeout, out_packet);
  return removed ? KernelIoCode::Success : KernelIoCode::Timeout;
}

KernelIoCode KernelIoManager::create_event(bool manual_reset,
                                            bool initial_state,
                                            Handle& out_handle) {
  auto event = std::make_shared<KernelEvent>(manual_reset, initial_state);
  return table().insert(event, 0xFFFFFFFFu, HandleFlags::None, out_handle);
}

KernelIoCode KernelIoManager::set_event(Handle event_handle) {
  HandleView view;
  const auto lookup = table().lookup(event_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::Event) {
    return KernelIoCode::InvalidObjectType;
  }
  std::static_pointer_cast<KernelEvent>(view.object)->set();
  return KernelIoCode::Success;
}

KernelIoCode KernelIoManager::reset_event(Handle event_handle) {
  HandleView view;
  const auto lookup = table().lookup(event_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::Event) {
    return KernelIoCode::InvalidObjectType;
  }
  std::static_pointer_cast<KernelEvent>(view.object)->reset();
  return KernelIoCode::Success;
}

KernelIoCode KernelIoManager::wait_event(Handle event_handle,
                                         std::chrono::milliseconds timeout,
                                         bool& out_signaled) {
  out_signaled = false;
  HandleView view;
  const auto lookup = table().lookup(event_handle, view);
  if (lookup != KernelIoCode::Success) return lookup;
  if (!view.object || view.object->type() != ObjectType::Event) {
    return KernelIoCode::InvalidObjectType;
  }
  out_signaled =
      std::static_pointer_cast<KernelEvent>(view.object)->wait_for(timeout);
  return out_signaled ? KernelIoCode::Success : KernelIoCode::Timeout;
}

}  // namespace xenon::kernel
