#pragma once

// Private to KernelIoManager: the per-path open/share/delete-pending state
// shared by open, close, rename and delete handling.

#include "xenon/kernel/io_manager.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include "xenon/filesystem/path.hpp"

namespace xenon::kernel {

struct KernelIoManager::OpenShareState
    : public std::enable_shared_from_this<KernelIoManager::OpenShareState> {
  struct Record {
    std::uint64_t id{};
    filesystem::FileAccess access{filesystem::FileAccess::None};
    filesystem::ShareAccess share{filesystem::ShareAccess::All};
    bool delete_pending{};
  };

  struct PathState {
    std::vector<Record> records{};
    bool sticky_delete_pending{};
    std::shared_ptr<filesystem::Device> device{};
    std::string relative_path{};
  };

  mutable std::mutex mutex{};
  std::unordered_map<std::string, PathState> paths{};
  std::uint64_t next_id{1};

  class Lease final : public FileObjectLease {
   public:
    Lease(std::shared_ptr<OpenShareState> state, std::string key, std::uint64_t id)
        : state_(std::move(state)), key_(std::move(key)), id_(id) {}
    ~Lease() override { release(); }

    KernelIoCode set_delete_pending(bool value) override {
      std::scoped_lock lock(mutex_);
      const auto state = state_.lock();
      if (!state || released_) return KernelIoCode::InvalidHandle;
      return state->set_delete_pending(key_, id_, value);
    }

    bool delete_pending() const override {
      std::scoped_lock lock(mutex_);
      const auto state = state_.lock();
      return state && !released_ && state->delete_pending(key_, id_);
    }

    KernelIoCode rename_to(const filesystem::ResolvedPath& target,
                           bool replace_existing) override {
      std::scoped_lock lock(mutex_);
      const auto state = state_.lock();
      if (!state || released_) return KernelIoCode::InvalidHandle;
      std::string new_key;
      const auto result =
          state->rename(key_, id_, target, replace_existing, new_key);
      if (result == KernelIoCode::Success) key_ = std::move(new_key);
      return result;
    }

    void release() noexcept override {
      std::string key;
      {
        std::scoped_lock lock(mutex_);
        if (released_) return;
        released_ = true;
        key = key_;
      }
      const auto state = state_.lock();
      if (state) state->release(std::move(key), id_);
    }

   private:
    std::weak_ptr<OpenShareState> state_{};
    mutable std::mutex mutex_{};
    std::string key_{};
    std::uint64_t id_{};
    bool released_{};
  };

  [[nodiscard]] KernelIoCode reserve(
      const filesystem::ResolvedPath& resolved,
      filesystem::FileAccess access, filesystem::ShareAccess share,
      std::shared_ptr<FileObjectLease>& out_lease);
  [[nodiscard]] KernelIoCode set_delete_pending(std::string_view key,
                                                std::uint64_t id,
                                                bool value);
  [[nodiscard]] bool delete_pending(std::string_view key,
                                    std::uint64_t id) const;
  [[nodiscard]] KernelIoCode rename(std::string_view key, std::uint64_t id,
                                    const filesystem::ResolvedPath& target,
                                    bool replace_existing,
                                    std::string& out_new_key);
  void release(std::string key, std::uint64_t id) noexcept;
};

}  // namespace xenon::kernel
