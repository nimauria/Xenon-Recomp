#include "xenon/memory/address_space.hpp"

#include "memory/guest/address_space_internal.hpp"

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace xenon::memory {
namespace {

#if defined(_WIN32) && defined(_DEBUG)
// A debug-CRT assertion/abort (vector bounds check, heap corruption, etc.)
// otherwise shows a blocking "Debug Error!" dialog via _CrtDbgReport. This
// runtime host process normally runs detached with no visible console or
// window (see runtime_host/src/main.cpp) - a modal dialog nobody can ever
// click hangs the process forever instead of failing. The dynamic debug CRT
// (ucrtbased.dll, used by a Debug-configured game module) is one shared
// instance per process, so running this once, from any Xenon library linked
// into that module, redirects every report from every module sharing it to
// stderr (already redirected to this session's log file) instead of a
// dialog - the process still aborts on a real assertion, but does so
// promptly and leaves the reason in the log rather than hanging silently.
struct SuppressBlockingCrtDialogs {
  SuppressBlockingCrtDialogs() noexcept {
    for (int report_type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
      _CrtSetReportMode(report_type, _CRTDBG_MODE_FILE);
      _CrtSetReportFile(report_type, _CRTDBG_FILE_STDERR);
    }
  }
};
const SuppressBlockingCrtDialogs g_suppress_blocking_crt_dialogs{};
#endif

}  // namespace

AddressSpace::AddressSpace(GuestTranslationMode mode)
    : physical_(std::make_unique<PhysicalBacking>()),
      guest_aperture_(std::make_unique<GuestAperture>()),
      physical_allocator_(std::make_unique<PhysicalRangeAllocator>()),
      physical_reverse_mappings_(std::make_unique<PhysicalReverseMappings>()),
      pages_(kPageCount),
      hot_pages_(kPageCount),
      physical_page_used_(kPhysicalPageCount),
      physical_mapping_refs_(kPhysicalPageCount),
      physical_page_metadata_(kPhysicalPageCount),
      reservation_seen_bitmap_(kReservationBitmapWordCount),
      executable_page_generations_(kPhysicalPageCount),
      direct_aperture_requested_(
          mode == GuestTranslationMode::DirectAperture ||
          (mode == GuestTranslationMode::Auto &&
#if defined(XENON_MEMORY_DEFAULT_DIRECT_APERTURE) && XENON_MEMORY_DEFAULT_DIRECT_APERTURE
           true
#else
           false
#endif
           )) {
  for (auto& entry : hot_pages_) entry.store(0u, std::memory_order_relaxed);
  for (auto& generation : executable_page_generations_) {
    generation.store(0u, std::memory_order_relaxed);
  }
  for (auto& slot : reservation_slots_) slot.store(0u, std::memory_order_relaxed);
  for (auto& word : reservation_seen_bitmap_) {
    word.store(0u, std::memory_order_relaxed);
  }
}

AddressSpace::~AddressSpace() = default;

bool AddressSpace::direct_aperture_active() const noexcept {
  return initialized_ && guest_aperture_ && guest_aperture_->active();
}

bool AddressSpace::direct_aperture_maps(GuestAddress address) const noexcept {
  if (!direct_aperture_active()) return false;
  const auto page = address >> kPageShift;
  if (page >= hot_pages_.size()) return false;
  return (hot_pages_[page].load(std::memory_order_acquire) &
          xenon::cpu::fast_memory::kDirectAperture) != 0u;
}

bool AddressSpace::initialize() {
  std::lock_guard lock(mutex_);
  if (initialized_) return true;
  if (!physical_->initialize()) return false;
  if (direct_aperture_requested_ && guest_aperture_) {
    // The aperture is an optional acceleration layer. Failure must never make
    // Xbox-visible memory initialization fail; compact translation remains the
    // portable fallback.
    (void)guest_aperture_->initialize(physical_->shared());
  }
  initialized_ = true;
  reset();
  return true;
}

void AddressSpace::reset() {
  std::lock_guard lock(mutex_);
  if (!initialized_) return;
  physical_->reset();
  std::fill(pages_.begin(), pages_.end(), Page{});
  std::fill(physical_page_used_.begin(), physical_page_used_.end(), kPhysicalFree);
  std::fill(physical_mapping_refs_.begin(), physical_mapping_refs_.end(), 0u);
  std::fill(physical_page_metadata_.begin(), physical_page_metadata_.end(),
            PhysicalPageMetadata{});
  for (std::uint32_t i = 0; i < executable_page_generations_.size(); ++i) {
    // Executable generations are lifetime-sticky for AddressSpace. Resetting
    // mappings must invalidate a native cache that outlives the guest reset;
    // returning a generation to zero would create an ABA when the same
    // physical frame later hosts executable code again.
    if (executable_page_generations_[i].load(std::memory_order_relaxed) != 0u) {
      advance_executable_generation(i);
    }
  }
  physical_reverse_mappings_->clear();
  for (auto& slot : reservation_slots_) slot.store(0u, std::memory_order_relaxed);
  for (auto& word : reservation_seen_bitmap_) {
    word.store(0u, std::memory_order_relaxed);
  }
  reservation_next_generation_.store(1u, std::memory_order_relaxed);
  reservation_commit_gate_->reset();
  active_reservation_ops_.store(0u, std::memory_order_relaxed);
  for (auto& entry : hot_pages_) entry.store(0u, std::memory_order_relaxed);
  if (guest_aperture_ && guest_aperture_->active() &&
      active_fast_readers_.load(std::memory_order_acquire) == 0u &&
      !guest_aperture_->reset()) {
    guest_aperture_->dispose();
  }

  // The first 16 MiB are the GPU writeback/XPS physical window. The final
  // 64 KiB are also reserved by the retail physical-memory contract and must
  // never be returned by MmAllocatePhysicalMemory-style allocations.
  const auto system_page_count = kPhysicalSystemReserveSize / kBasePageSize;
  const auto top_reserved_page_count =
      kPhysicalTopReservedSize / kBasePageSize;
  const auto allocatable_end_page =
      kPhysicalAllocatableEndExclusive / kBasePageSize;
  std::fill_n(physical_page_used_.begin(), system_page_count, kPhysicalSystem);
  std::fill_n(physical_page_used_.begin() + allocatable_end_page,
              top_reserved_page_count, kPhysicalSystem);
  for (std::uint32_t page = allocatable_end_page;
       page < kPhysicalPageCount; ++page) {
    physical_page_metadata_[page].allocation_protect = Protect::None;
    physical_page_metadata_[page].current_protect = Protect::None;
  }
  physical_allocator_->reset(
      system_page_count, allocatable_end_page - system_page_count);

  // Match the retail user virtual behavior: the low 64 KiB exists as a
  // committed no-access guard. It intentionally has no physical backing; the
  // structured fault path reports protection before backing state.
  const auto guard_pages = 0x10000u / kBasePageSize;
  for (std::uint32_t i = 0; i < guard_pages; ++i) {
    auto& page = pages_[i];
    page.state = PageState::Committed;
    page.allocation_base_page = 0;
    page.allocation_page_count = guard_pages;
    page.allocation_protect = Protect::None;
    page.current_protect = Protect::None;
    page.kind = RegionKind::Virtual;
    page.permanent_guard = true;
  }
  rebuild_hot_pages();
  coherency_.mark_all_dirty();
}

std::span<const RegionDescriptor> AddressSpace::regions() noexcept { return kRegions; }

}  // namespace xenon::memory
