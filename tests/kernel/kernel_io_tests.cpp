#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenon/filesystem/host_path_device.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/kernel/io_manager.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/logging/logger.hpp"

namespace fs = xenon::filesystem;
namespace kernel = xenon::kernel;

namespace {

class TempDirectory {
 public:
  TempDirectory() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_kernel_io_" + std::to_string(now));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_{};
};

void write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream file(path, std::ios::binary);
  assert(file);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  assert(file);
}

std::shared_ptr<fs::VirtualFileSystem> make_vfs(const std::filesystem::path& root) {
  auto vfs = std::make_shared<fs::VirtualFileSystem>();
  fs::HostPathDeviceOptions options{};
  options.read_only = false;
  options.create_root = true;
  auto device = std::make_shared<fs::HostPathDevice>(
      "\\Device\\Harddisk0\\Partition1", root, options);
  assert(vfs->register_device(device) == fs::FsError::None);
  assert(vfs->register_symbolic_link("game:", device->mount_point()) ==
         fs::FsError::None);
  return vfs;
}

fs::OpenOptions rw_open(fs::FileAccess extra = fs::FileAccess::None) {
  fs::OpenOptions options{};
  options.access = fs::FileAccess::Read | fs::FileAccess::Write | extra;
  options.share = fs::ShareAccess::All;
  options.disposition = fs::CreateDisposition::Open;
  return options;
}

void test_duplicate_handles_share_file_position_and_rights() {
  TempDirectory temp;
  write_text(temp.path() / "data.bin", "abcdef");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions open{};
  open.file = rw_open();
  kernel::Handle original = kernel::kInvalidHandle;
  assert(io.open("game:\\data.bin", open, original).succeeded());
  assert(original != kernel::kInvalidHandle);

  std::array<std::byte, 2> first{};
  auto status = io.read(original, first);
  assert(status.succeeded());
  assert(status.information == 2);
  assert(first[0] == std::byte{'a'} && first[1] == std::byte{'b'});
  assert(status.request_id != 0);

  kernel::DuplicateHandleOptions duplicate{};
  duplicate.same_access = false;
  duplicate.desired_access = static_cast<std::uint32_t>(fs::FileAccess::Read);
  kernel::Handle read_only_duplicate = kernel::kInvalidHandle;
  assert(io.duplicate(original, duplicate, read_only_duplicate) ==
         kernel::KernelIoCode::Success);

  std::array<std::byte, 2> second{};
  status = io.read(read_only_duplicate, second);
  assert(status.succeeded());
  assert(second[0] == std::byte{'c'} && second[1] == std::byte{'d'});

  const std::array<std::byte, 1> replacement{std::byte{'!'}};
  assert(io.write(read_only_duplicate, replacement).code ==
         kernel::KernelIoCode::AccessDenied);

  assert(io.close(original) == kernel::KernelIoCode::Success);
  std::array<std::byte, 2> final{};
  status = io.read(read_only_duplicate, final);
  assert(status.succeeded());
  assert(final[0] == std::byte{'e'} && final[1] == std::byte{'f'});
  assert(io.close(read_only_duplicate) == kernel::KernelIoCode::Success);
}

void test_independent_opens_have_independent_positions() {
  TempDirectory temp;
  write_text(temp.path() / "data.bin", "012345");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions open{};
  open.file = rw_open();
  kernel::Handle a{}, b{};
  assert(io.open("game:\\data.bin", open, a).succeeded());
  assert(io.open("game:\\data.bin", open, b).succeeded());

  std::array<std::byte, 1> one{};
  assert(io.read(a, one).succeeded());
  assert(one[0] == std::byte{'0'});
  assert(io.read(a, one).succeeded());
  assert(one[0] == std::byte{'1'});
  assert(io.read(b, one).succeeded());
  assert(one[0] == std::byte{'0'});

  assert(io.close(a) == kernel::KernelIoCode::Success);
  assert(io.close(b) == kernel::KernelIoCode::Success);
}

void test_directory_cursor_is_file_object_state() {
  TempDirectory temp;
  std::filesystem::create_directory(temp.path() / "Dir");
  write_text(temp.path() / "Dir" / "a.bin", "a");
  write_text(temp.path() / "Dir" / "b.bin", "b");
  write_text(temp.path() / "Dir" / "c.txt", "c");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions open{};
  open.kind = kernel::OpenKind::Directory;
  open.file.access = fs::FileAccess::Read;
  open.file.share = fs::ShareAccess::All;
  open.file.disposition = fs::CreateDisposition::Open;
  kernel::Handle dir{};
  assert(io.open("game:\\Dir", open, dir).succeeded());

  fs::DirectoryQuery query{};
  query.pattern = "*.bin";
  std::vector<fs::DirectoryEntry> entries;
  bool end = false;
  auto status = io.query_directory(dir, &query, false, 1, entries, end);
  assert(status.succeeded());
  assert(entries.size() == 1);
  assert(entries[0].name == "a.bin");
  assert(!end);

  kernel::Handle duplicate{};
  assert(io.duplicate(dir, {}, duplicate) == kernel::KernelIoCode::Success);
  status = io.query_directory(duplicate, nullptr, false, 1, entries, end);
  assert(status.succeeded());
  assert(entries.size() == 1);
  assert(entries[0].name == "b.bin");
  assert(end);

  status = io.query_directory(dir, nullptr, false, 1, entries, end);
  assert(status.code == kernel::KernelIoCode::NoMoreFiles);
  assert(end);

  status = io.query_directory(dir, nullptr, true, 1, entries, end);
  assert(status.succeeded());
  assert(entries.size() == 1 && entries[0].name == "a.bin");

  assert(io.close(dir) == kernel::KernelIoCode::Success);
  assert(io.close(duplicate) == kernel::KernelIoCode::Success);
}


void test_kernel_share_violations_are_explicit() {
  TempDirectory temp;
  write_text(temp.path() / "share.bin", "share");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions first{};
  first.file.access = fs::FileAccess::Read;
  first.file.share = fs::ShareAccess::Read;
  first.file.disposition = fs::CreateDisposition::Open;
  kernel::Handle a{};
  assert(io.open("game:\\share.bin", first, a).succeeded());

  kernel::KernelOpenOptions second{};
  second.file.access = fs::FileAccess::Write;
  second.file.share = fs::ShareAccess::All;
  second.file.disposition = fs::CreateDisposition::Open;
  kernel::Handle b{};
  const auto status = io.open("game:\\share.bin", second, b);
  assert(status.code == kernel::KernelIoCode::SharingViolation);
  assert(b == kernel::kInvalidHandle);
  assert(io.close(a) == kernel::KernelIoCode::Success);
}

void test_delete_pending_waits_for_last_open_object() {
  TempDirectory temp;
  const auto host_path = temp.path() / "delete.me";
  write_text(host_path, "payload");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions owner_open{};
  owner_open.file = rw_open(fs::FileAccess::Delete);
  kernel::Handle owner{};
  assert(io.open("game:\\delete.me", owner_open, owner).succeeded());

  kernel::KernelOpenOptions peer_open{};
  peer_open.file.access = fs::FileAccess::Read;
  peer_open.file.share = fs::ShareAccess::All;
  peer_open.file.disposition = fs::CreateDisposition::Open;
  kernel::Handle peer{};
  assert(io.open("game:\\delete.me", peer_open, peer).succeeded());

  assert(io.set_delete_pending(owner, true) == kernel::KernelIoCode::Success);
  assert(io.delete_pending(owner));

  kernel::Handle blocked{};
  const auto blocked_status = io.open("game:\\delete.me", peer_open, blocked);
  assert(blocked_status.code == kernel::KernelIoCode::DeletePending);
  assert(blocked == kernel::kInvalidHandle);

  assert(io.close(owner) == kernel::KernelIoCode::Success);
  assert(std::filesystem::exists(host_path));
  assert(io.close(peer) == kernel::KernelIoCode::Success);
  assert(!std::filesystem::exists(host_path));
}

void test_delete_on_close_and_handle_protection() {
  TempDirectory temp;
  const auto host_path = temp.path() / "temporary.bin";
  write_text(host_path, "temp");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions open{};
  open.file = rw_open(fs::FileAccess::Delete);
  open.delete_on_close = true;
  open.handle_flags = kernel::HandleFlags::ProtectFromClose;
  kernel::Handle handle{};
  assert(io.open("game:\\temporary.bin", open, handle).succeeded());
  assert(io.close(handle) == kernel::KernelIoCode::ProtectedHandle);
  assert(std::filesystem::exists(host_path));
  assert(io.close(handle, true) == kernel::KernelIoCode::Success);
  assert(!std::filesystem::exists(host_path));
}

void test_async_ready_request_lifecycle() {
  TempDirectory temp;
  write_text(temp.path() / "async.bin", "request");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions open{};
  open.file = rw_open();
  kernel::Handle handle{};
  assert(io.open("game:\\async.bin", open, handle).succeeded());

  std::shared_ptr<kernel::IoRequest> request;
  auto status = io.begin_request(handle, kernel::IoOperation::Read, 0x1234, request);
  assert(status.code == kernel::KernelIoCode::Pending);
  assert(request);
  assert(request->snapshot().state == kernel::IoRequestState::Pending);
  assert(request->snapshot().context == 0x1234);

  kernel::IoStatus completion{};
  completion.information = 17;
  assert(io.complete_request(handle, request->id(), completion) ==
         kernel::KernelIoCode::Success);
  const auto completed = request->wait();
  assert(completed.succeeded());
  assert(completed.information == 17);
  assert(completed.request_id == request->id());

  std::shared_ptr<kernel::IoRequest> cancelled;
  assert(io.begin_request(handle, kernel::IoOperation::Write, 9, cancelled).code ==
         kernel::KernelIoCode::Pending);
  assert(io.close(handle) == kernel::KernelIoCode::Success);
  const auto cancelled_result = cancelled->wait();
  assert(cancelled_result.code == kernel::KernelIoCode::Cancelled);
}

// Part 11 of the AC6 Runtime Readiness pass ("Filesystem / Content / Async
// I/O"): async submit and completion must be individually observable
// (request sequence number, operation, host thread), not just their net
// effect on IoRequest's own state - so a real ordering issue (a request
// completed out of the sequence it was submitted in, or completed on a
// different host thread than expected) is diagnosable after the fact.
void test_async_request_submit_and_completion_are_logged() {
  using xenon::logging::Level;
  using xenon::logging::Logger;

  TempDirectory temp;
  write_text(temp.path() / "diag.bin", "diagnostics");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions open{};
  open.file = rw_open();
  kernel::Handle handle{};
  assert(io.open("game:\\diag.bin", open, handle).succeeded());

  struct Captured {
    Level level;
    std::string category;
    std::string message;
  };
  std::vector<Captured> captured;
  auto& logger = Logger::instance();
  const auto previous_level = logger.min_level();
  logger.set_min_level(Level::Debug);
  logger.set_sink([&](Level level, std::string_view category, std::string_view message) {
    captured.push_back(Captured{level, std::string(category), std::string(message)});
  });

  std::shared_ptr<kernel::IoRequest> request;
  assert(io.begin_request(handle, kernel::IoOperation::Read, 0xAAu, request).code ==
         kernel::KernelIoCode::Pending);
  kernel::IoStatus completion{};
  assert(io.complete_request(handle, request->id(), completion) ==
         kernel::KernelIoCode::Success);

  logger.set_sink(nullptr);
  logger.set_min_level(previous_level);

  bool saw_submit = false, saw_completion = false;
  const auto id_text = std::to_string(request->id());
  for (const auto& entry : captured) {
    if (entry.category != "io") continue;
    if (entry.message.find("async submit: request=" + id_text) == 0) saw_submit = true;
    if (entry.message.find("async completion: request=" + id_text) == 0)
      saw_completion = true;
  }
  assert(saw_submit && "async submit must be logged with its request sequence number");
  assert(saw_completion && "async completion must be logged with its request sequence number");

  assert(io.close(handle) == kernel::KernelIoCode::Success);
}

void test_directory_create_and_kind_checks() {
  TempDirectory temp;
  write_text(temp.path() / "plain.bin", "x");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions as_directory{};
  as_directory.kind = kernel::OpenKind::Directory;
  as_directory.file.access = fs::FileAccess::Read;
  as_directory.file.share = fs::ShareAccess::All;
  as_directory.file.disposition = fs::CreateDisposition::Open;
  kernel::Handle handle{};
  auto status = io.open("game:\\plain.bin", as_directory, handle);
  assert(status.code == kernel::KernelIoCode::NotDirectory);
  assert(status.filesystem_error == fs::FsError::NotDirectory);

  as_directory.file.disposition = fs::CreateDisposition::Create;
  status = io.open("game:\\Created", as_directory, handle);
  assert(status.succeeded());
  assert(std::filesystem::is_directory(temp.path() / "Created"));
  assert(io.close(handle) == kernel::KernelIoCode::Success);
}

void test_rooted_relative_open_and_file_information() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "Root" / "Child");
  write_text(temp.path() / "Root" / "Child" / "data.bin", "abcdef");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions dir_open{};
  dir_open.kind = kernel::OpenKind::Directory;
  dir_open.file.access = fs::FileAccess::Read;
  dir_open.file.share = fs::ShareAccess::All;
  dir_open.file.disposition = fs::CreateDisposition::Open;
  kernel::Handle root{};
  assert(io.open("game:\\Root", dir_open, root).succeeded());

  kernel::KernelOpenOptions file_open{};
  file_open.file = rw_open(fs::FileAccess::Delete);
  kernel::Handle file{};
  assert(io.open_at(root, "Child\\data.bin", file_open, file).succeeded());

  kernel::FileInformation info = kernel::FilePositionInformation{};
  assert(io.query_information(file, kernel::FileInformationClass::Position, info)
             .succeeded());
  assert(std::get<kernel::FilePositionInformation>(info).current_byte_offset == 0);

  info = kernel::FilePositionInformation{3};
  assert(io.set_information(file, kernel::FileInformationClass::Position, info)
             .succeeded());
  std::array<std::byte, 1> byte{};
  assert(io.read(file, byte).succeeded());
  assert(byte[0] == std::byte{'d'});

  assert(io.query_information(file, kernel::FileInformationClass::Standard, info)
             .succeeded());
  const auto standard = std::get<kernel::FileStandardInformation>(info);
  assert(standard.end_of_file == 6);
  assert(!standard.directory);

  info = kernel::FileEndOfFileInformation{4};
  assert(io.set_information(file, kernel::FileInformationClass::EndOfFile, info)
             .succeeded());
  assert(std::filesystem::file_size(temp.path() / "Root" / "Child" / "data.bin") == 4);

  kernel::VolumeInformation volume{};
  assert(io.query_volume_information(file, volume).succeeded());
  assert(!volume.name.empty());
  assert(volume.space.capacity >= volume.space.available);

  kernel::Handle rejected{};
  assert(io.open_at(root, "game:\\Root\\Child\\data.bin", file_open, rejected).code ==
         kernel::KernelIoCode::InvalidParameter);
  assert(rejected == kernel::kInvalidHandle);

  assert(io.close(file) == kernel::KernelIoCode::Success);
  assert(io.close(root) == kernel::KernelIoCode::Success);
}

void test_rename_information_with_root_directory() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "A");
  std::filesystem::create_directories(temp.path() / "B");
  write_text(temp.path() / "A" / "old.bin", "rename");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions dir_open{};
  dir_open.kind = kernel::OpenKind::Directory;
  dir_open.file.access = fs::FileAccess::Read;
  dir_open.file.share = fs::ShareAccess::All;
  dir_open.file.disposition = fs::CreateDisposition::Open;
  kernel::Handle target_root{};
  assert(io.open("game:\\B", dir_open, target_root).succeeded());

  kernel::KernelOpenOptions source_open{};
  source_open.file = rw_open(fs::FileAccess::Delete);
  kernel::Handle source{};
  assert(io.open("game:\\A\\old.bin", source_open, source).succeeded());

  kernel::FileInformation rename = kernel::FileRenameInformation{
      false, target_root, "new.bin"};
  assert(io.set_information(source, kernel::FileInformationClass::Rename, rename)
             .succeeded());
  assert(!std::filesystem::exists(temp.path() / "A" / "old.bin"));
  assert(std::filesystem::exists(temp.path() / "B" / "new.bin"));

  kernel::FileInformation name = kernel::FileNameInformation{};
  assert(io.query_information(source, kernel::FileInformationClass::Name, name)
             .succeeded());
  assert(std::get<kernel::FileNameInformation>(name).name.find("new.bin") !=
         std::string::npos);

  assert(io.close(source) == kernel::KernelIoCode::Success);
  assert(io.close(target_root) == kernel::KernelIoCode::Success);
}

void test_completion_port_receives_synchronous_results() {
  TempDirectory temp;
  write_text(temp.path() / "complete.bin", "abcd");
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::KernelOpenOptions open{};
  open.file = rw_open();
  kernel::Handle file{};
  assert(io.open("game:\\complete.bin", open, file).succeeded());

  kernel::Handle port{};
  assert(io.create_completion_port(port) == kernel::KernelIoCode::Success);
  kernel::FileInformation completion =
      kernel::FileCompletionInformation{port, 0xABCDEF};
  assert(io.set_information(file, kernel::FileInformationClass::Completion,
                            completion)
             .succeeded());

  std::array<std::byte, 2> buffer{};
  const auto read = io.read(file, buffer, {}, true, 0x12345678);
  assert(read.succeeded());
  kernel::CompletionPacket packet{};
  assert(io.remove_completion(port, packet) == kernel::KernelIoCode::Success);
  assert(packet.key == 0xABCDEF);
  assert(packet.context == 0x12345678);
  assert(packet.status.succeeded());
  assert(packet.status.information == 2);
  assert(packet.status.request_id == read.request_id);
  assert(io.remove_completion(port, packet) == kernel::KernelIoCode::Timeout);

  assert(io.close(file) == kernel::KernelIoCode::Success);
  assert(io.close(port) == kernel::KernelIoCode::Success);
}

void test_kernel_events_manual_and_auto_reset() {
  TempDirectory temp;
  kernel::KernelIoManager io(make_vfs(temp.path()));

  kernel::Handle manual{};
  assert(io.create_event(true, false, manual) == kernel::KernelIoCode::Success);
  bool signaled = true;
  assert(io.wait_event(manual, std::chrono::milliseconds{0}, signaled) ==
         kernel::KernelIoCode::Timeout);
  assert(!signaled);
  assert(io.set_event(manual) == kernel::KernelIoCode::Success);
  assert(io.wait_event(manual, std::chrono::milliseconds{0}, signaled) ==
         kernel::KernelIoCode::Success);
  assert(signaled);
  assert(io.wait_event(manual, std::chrono::milliseconds{0}, signaled) ==
         kernel::KernelIoCode::Success);
  assert(io.reset_event(manual) == kernel::KernelIoCode::Success);

  kernel::Handle automatic{};
  assert(io.create_event(false, true, automatic) == kernel::KernelIoCode::Success);
  assert(io.wait_event(automatic, std::chrono::milliseconds{0}, signaled) ==
         kernel::KernelIoCode::Success);
  assert(io.wait_event(automatic, std::chrono::milliseconds{0}, signaled) ==
         kernel::KernelIoCode::Timeout);

  assert(io.close(manual) == kernel::KernelIoCode::Success);
  assert(io.close(automatic) == kernel::KernelIoCode::Success);
}


void test_xbox_facade_rexglue_compatible_create_and_status() {
  TempDirectory temp;
  kernel::KernelIoManager io(make_vfs(temp.path()));
  kernel::xbox::IoFacade xbox(io);

  kernel::xbox::CreateFileRequest request{};
  request.path = "game:\\created.bin";
  request.desired_access = kernel::xbox::access::GenericRead |
                           kernel::xbox::access::GenericWrite |
                           kernel::xbox::access::Delete;
  request.share_access = kernel::xbox::share::All;
  request.creation_disposition = 3;  // FILE_OPEN_IF
  request.create_options = kernel::xbox::create_option::NonDirectoryFile |
                           kernel::xbox::create_option::SynchronousIoNonAlert;

  kernel::Handle file{};
  kernel::xbox::IoStatusBlock iosb{};
  assert(xbox.create_file(request, file, iosb) == kernel::xbox::status::Success);
  assert(file != kernel::kInvalidHandle);
  assert(iosb.information ==
         static_cast<std::uint32_t>(kernel::xbox::FileAction::Created));

  assert(io.close(file) == kernel::KernelIoCode::Success);
  assert(xbox.create_file(request, file, iosb) == kernel::xbox::status::Success);
  assert(iosb.information ==
         static_cast<std::uint32_t>(kernel::xbox::FileAction::Opened));
  assert(io.close(file) == kernel::KernelIoCode::Success);

  request.creation_disposition = 2;  // FILE_CREATE
  assert(xbox.create_file(request, file, iosb) ==
         kernel::xbox::status::ObjectNameCollision);
  assert(iosb.information ==
         static_cast<std::uint32_t>(kernel::xbox::FileAction::Exists));

  request.path = "game:\\missing.bin";
  request.creation_disposition = 1;  // FILE_OPEN
  assert(xbox.create_file(request, file, iosb) ==
         kernel::xbox::status::ObjectNameNotFound);
  assert(iosb.information ==
         static_cast<std::uint32_t>(kernel::xbox::FileAction::DoesNotExist));

  request.path = "game:\\created.bin";
  request.creation_disposition = 3;
  request.create_options = kernel::xbox::create_option::DirectoryFile |
                           kernel::xbox::create_option::NonDirectoryFile;
  assert(xbox.create_file(request, file, iosb) ==
         kernel::xbox::status::InvalidParameter);
  assert(file == kernel::kInvalidHandle);
}

void test_xbox_facade_async_contract_and_event_signal() {
  TempDirectory temp;
  write_text(temp.path() / "async-xbox.bin", "abcd");
  kernel::KernelIoManager io(make_vfs(temp.path()));
  kernel::xbox::IoFacade xbox(io);

  kernel::xbox::CreateFileRequest request{};
  request.path = "game:\\async-xbox.bin";
  request.desired_access = kernel::xbox::access::GenericRead;
  request.share_access = kernel::xbox::share::All;
  request.creation_disposition = 1;
  request.create_options = kernel::xbox::create_option::NonDirectoryFile;

  kernel::Handle file{};
  kernel::xbox::IoStatusBlock iosb{};
  assert(xbox.create_file(request, file, iosb) == kernel::xbox::status::Success);

  kernel::Handle event{};
  assert(io.create_event(false, false, event) == kernel::KernelIoCode::Success);
  std::array<std::byte, 2> buffer{};
  const auto returned = xbox.read_file(file, buffer, {}, iosb, event, 0x1234);
  assert(returned == kernel::xbox::status::Pending);
  assert(iosb.status == kernel::xbox::status::Success);
  assert(iosb.information == 2);
  assert(buffer[0] == std::byte{'a'} && buffer[1] == std::byte{'b'});

  bool signaled = false;
  assert(io.wait_event(event, std::chrono::milliseconds{0}, signaled) ==
         kernel::KernelIoCode::Success);
  assert(signaled);
  assert(io.close(file) == kernel::KernelIoCode::Success);
  assert(io.close(event) == kernel::KernelIoCode::Success);
}

void test_xbox_facade_information_volume_and_rooted_attributes() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "Root");
  write_text(temp.path() / "Root" / "info.bin", "12345678");
  kernel::KernelIoManager io(make_vfs(temp.path()));
  kernel::xbox::IoFacade xbox(io);

  kernel::xbox::CreateFileRequest root_request{};
  root_request.path = "game:\\Root";
  root_request.desired_access = kernel::xbox::access::GenericRead;
  root_request.share_access = kernel::xbox::share::All;
  root_request.creation_disposition = 1;
  root_request.create_options = kernel::xbox::create_option::DirectoryFile |
                                kernel::xbox::create_option::SynchronousIoNonAlert;
  kernel::Handle root{};
  kernel::xbox::IoStatusBlock iosb{};
  assert(xbox.create_file(root_request, root, iosb) == kernel::xbox::status::Success);

  kernel::xbox::CreateFileRequest file_request{};
  file_request.root_directory = root;
  file_request.path = "info.bin";
  file_request.desired_access = kernel::xbox::access::GenericRead |
                                kernel::xbox::access::GenericWrite;
  file_request.share_access = kernel::xbox::share::All;
  file_request.creation_disposition = 1;
  file_request.create_options = kernel::xbox::create_option::NonDirectoryFile |
                                kernel::xbox::create_option::SynchronousIoNonAlert;
  kernel::Handle file{};
  assert(xbox.create_file(file_request, file, iosb) == kernel::xbox::status::Success);

  kernel::FileInformation info = kernel::FilePositionInformation{};
  assert(xbox.query_information_file(
             file, static_cast<std::uint32_t>(kernel::xbox::FileInformationClass::Position),
             info, iosb) == kernel::xbox::status::Success);
  assert(std::get<kernel::FilePositionInformation>(info).current_byte_offset == 0);

  info = kernel::FilePositionInformation{4};
  assert(xbox.set_information_file(
             file, static_cast<std::uint32_t>(kernel::xbox::FileInformationClass::Position),
             info, iosb) == kernel::xbox::status::Success);
  std::array<std::byte, 1> one{};
  assert(xbox.read_file(file, one, {}, iosb) == kernel::xbox::status::Success);
  assert(one[0] == std::byte{'5'});

  kernel::xbox::VolumeInformation volume{};
  assert(xbox.query_volume_information_file(
             file, static_cast<std::uint32_t>(kernel::xbox::FsInformationClass::Size),
             volume, iosb) == kernel::xbox::status::Success);
  const auto size = std::get<kernel::xbox::FsSizeInformation>(volume);
  assert(size.bytes_per_sector == 0x200);
  assert(size.sectors_per_allocation_unit >= 1);

  assert(xbox.query_volume_information_file(
             file, static_cast<std::uint32_t>(kernel::xbox::FsInformationClass::Attribute),
             volume, iosb) == kernel::xbox::status::Success);
  const auto attrs = std::get<kernel::xbox::FsAttributeInformation>(volume);
  assert(attrs.component_name_max_length == 255);
  assert(attrs.name == "XENON");

  kernel::FileNetworkOpenInformation full{};
  assert(xbox.query_full_attributes_file(root, "info.bin", full, iosb) ==
         kernel::xbox::status::Success);
  assert(full.file.size == 8);

  assert(xbox.query_information_file(file, 0xFFFF, info, iosb) ==
         kernel::xbox::status::InvalidInfoClass);

  assert(io.close(file) == kernel::KernelIoCode::Success);
  assert(io.close(root) == kernel::KernelIoCode::Success);
}

void test_xbox_facade_directory_query_and_path_validation() {
  TempDirectory temp;
  std::filesystem::create_directories(temp.path() / "Dir");
  write_text(temp.path() / "Dir" / "a.bin", "a");
  write_text(temp.path() / "Dir" / "b.txt", "b");
  kernel::KernelIoManager io(make_vfs(temp.path()));
  kernel::xbox::IoFacade xbox(io);

  kernel::xbox::CreateFileRequest request{};
  request.path = "game:\\Dir";
  request.desired_access = kernel::xbox::access::GenericRead;
  request.share_access = kernel::xbox::share::All;
  request.creation_disposition = 1;
  request.create_options = kernel::xbox::create_option::DirectoryFile |
                           kernel::xbox::create_option::SynchronousIoNonAlert;
  kernel::Handle dir{};
  kernel::xbox::IoStatusBlock iosb{};
  assert(xbox.create_file(request, dir, iosb) == kernel::xbox::status::Success);

  std::vector<kernel::xbox::DirectoryInformation> entries;
  assert(xbox.query_directory_file(dir, "*.bin", true, 1, entries, iosb) ==
         kernel::xbox::status::Success);
  assert(entries.size() == 1 && entries[0].entry.name == "a.bin");
  assert(xbox.query_directory_file(dir, {}, false, 1, entries, iosb) ==
         kernel::xbox::status::NoMoreFiles);

  assert(!kernel::xbox::valid_path("bad|name", false));
  assert(kernel::xbox::valid_path("*.bin", true));
  assert(!kernel::xbox::valid_path("*.bin", false));
  assert(io.close(dir) == kernel::KernelIoCode::Success);
}

}  // namespace

int main() {
  test_duplicate_handles_share_file_position_and_rights();
  test_independent_opens_have_independent_positions();
  test_directory_cursor_is_file_object_state();
  test_kernel_share_violations_are_explicit();
  test_delete_pending_waits_for_last_open_object();
  test_delete_on_close_and_handle_protection();
  test_async_ready_request_lifecycle();
  test_async_request_submit_and_completion_are_logged();
  test_directory_create_and_kind_checks();
  test_rooted_relative_open_and_file_information();
  test_rename_information_with_root_directory();
  test_completion_port_receives_synchronous_results();
  test_kernel_events_manual_and_auto_reset();
  test_xbox_facade_rexglue_compatible_create_and_status();
  test_xbox_facade_async_contract_and_event_signal();
  test_xbox_facade_information_volume_and_rooted_attributes();
  test_xbox_facade_directory_query_and_path_validation();
  return 0;
}
