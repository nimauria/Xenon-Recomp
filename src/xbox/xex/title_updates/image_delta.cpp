// XEXP delta application to the image body and the header region.

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace xenon::xbox::detail {
namespace {

// Validates then applies `dest[target_offset, target_offset+size) =
// source[source_offset, source_offset+size)`, snapshotting the source range
// first so this is safe even when `source` and `dest` alias the same
// buffer. Shared by the XEXP header-region delta (which splices within a
// single working buffer seeded from the base header) and the XEXP
// image-region delta (which splices from a separate, read-only base image
// into a fresh working buffer).
bool splice_delta_region(std::span<const std::byte> source, std::uint32_t source_offset, std::uint32_t size,
                         std::span<std::byte> dest, std::uint32_t target_offset, const char* what,
                         std::string* error) {
  if (source_offset > source.size() || size > source.size() - source_offset) {
    if (error) *error = std::string("XEXP ") + what + " source range is outside its source data.";
    return false;
  }
  if (target_offset > dest.size() || size > dest.size() - target_offset) {
    if (error) *error = std::string("XEXP ") + what + " target range is outside its target buffer.";
    return false;
  }
  std::vector<std::byte> snapshot(source.begin() + source_offset, source.begin() + source_offset + size);
  std::copy(snapshot.begin(), snapshot.end(), dest.begin() + target_offset);
  return true;
}

}  // namespace

// XEX_COMPRESSION_DELTA (XEXP image/data delta): reconstructs the target
// effective image from `reference_image` (the base image's own
// effective_image - read-only, never mutated; the result is always built in
// a separate `working` buffer) plus the title update's
// XEX_HEADER_DELTA_PATCH_DESCRIPTOR (`delta_image_source_offset/target_
// offset/source_size`, for one optional whole-region splice - the same
// "copy this range from the base, then patch on top" shape the header
// delta uses) and the xex2_compressed_block_info chain in `body`. Each
// block's payload here is a xex2_delta_patch record chain (fill/copy/
// LZXDELTA-compressed records - see xex_lzx::apply_delta_patch_records()),
// applied directly onto `working`, NOT the 2-byte-length-prefixed chunk
// table degather_compressed_blocks() reassembles into a single continuous
// bitstream for XEX_COMPRESSION_NORMAL - this is a structurally different
// payload framing that happens to share the same outer block/hash
// container. Independent reimplementation against the real Xbox 360 XEXP
// image-patch algorithm (research: xenia-project/xenia's
// XexModule::ApplyPatch()/lzxdelta_apply_patch(), read for understanding
// only, not copied - see "Research rule").
bool apply_image_delta(std::span<const std::byte> body, std::uint32_t window_bits,
                       std::span<const std::byte> reference_image, const XexDeltaPatchDescriptor& delta_patch,
                       std::uint32_t target_size, std::vector<std::byte>& out_effective, std::string* error) {
  if (reference_image.empty()) {
    if (error) *error = "XEX_COMPRESSION_DELTA requires a base reference image.";
    return false;
  }
  if (target_size == 0u || target_size > kMaxReasonableImageBytes) {
    if (error) *error = "XEX_COMPRESSION_DELTA target image size is invalid.";
    return false;
  }

  const auto working_size = std::max<std::uint64_t>(reference_image.size(), target_size);
  if (working_size > kMaxReasonableImageBytes) {
    if (error) *error = "XEX_COMPRESSION_DELTA working image size is invalid.";
    return false;
  }
  std::vector<std::byte> working(static_cast<std::size_t>(working_size), std::byte{0});
  std::copy(reference_image.begin(), reference_image.end(), working.begin());

  if (delta_patch.delta_image_source_size != 0u) {
    if (!splice_delta_region(reference_image, delta_patch.delta_image_source_offset,
                             delta_patch.delta_image_source_size, working, delta_patch.delta_image_target_offset,
                             "image-delta", error)) {
      return false;
    }
  }

  if (working_size > target_size) {
    std::fill(working.begin() + static_cast<std::size_t>(target_size), working.end(), std::byte{0});
  }

  std::string chain_error;
  const bool ok = walk_compressed_block_chain(
      body,
      [&](std::span<const std::byte> payload) {
        const auto result = lzx::apply_delta_patch_records(payload, window_bits, working);
        if (!result.ok) {
          chain_error = "XEXP image-delta record chain failed: " + result.error;
          return false;
        }
        return true;
      },
      &chain_error);
  if (!ok) {
    if (error) *error = chain_error;
    return false;
  }

  working.resize(static_cast<std::size_t>(target_size));
  out_effective = std::move(working);
  return true;
}

// Splices XexDeltaPatchDescriptor::delta_headers_source_offset/size from
// `base_header_bytes` into a target_size-sized buffer at
// delta_headers_target_offset, then applies the descriptor's embedded
// xex2_delta_patch record on top - reproducing the real Xbox 360 XEXP
// header-patch algorithm (validated against xenia-project/xenia's
// XexModule::ApplyPatch(), read for research purposes only; this is an
// independent reimplementation against Xenon's own XexDeltaPatchDescriptor,
// not copied source - see docs/xbox/XEX_LOADER_V2.md "Research rule").
//
// When the descriptor is absent or delta_headers_source_size == 0 (no header
// *content* actually changed - the common case for code/data-only updates),
// `patch_header_bytes` is returned unchanged, exactly matching the prior,
// documented "known limitation" behaviour for that case.
bool reconstruct_patch_header_bytes(std::span<const std::byte> base_header_bytes,
                                    std::span<const std::byte> patch_header_bytes,
                                    std::span<const std::byte> patch_file_bytes,
                                    const XexDeltaPatchDescriptor& descriptor,
                                    std::uint32_t window_bits, std::vector<std::byte>& out_header,
                                    std::string* error) {
  if (!descriptor.present || descriptor.delta_headers_source_size == 0u) {
    out_header.assign(patch_header_bytes.begin(), patch_header_bytes.end());
    return true;
  }

  if (descriptor.delta_headers_source_offset > base_header_bytes.size()) {
    if (error) *error = "XEXP header-delta source range is outside the base image's header.";
    return false;
  }
  const auto header_size_available = base_header_bytes.size() - descriptor.delta_headers_source_offset;
  if (descriptor.delta_headers_source_size > header_size_available) {
    if (error) *error = "XEXP header-delta source range is too large for the base header.";
    return false;
  }

  // 64-bit intermediate: delta_headers_target_offset is attacker-controlled
  // and unbounded at this point, so target_offset + source_size must not be
  // allowed to silently wrap a 32-bit sum before it is range-checked.
  const std::uint64_t target_size_wide =
      descriptor.size_of_target_headers != 0u
          ? static_cast<std::uint64_t>(descriptor.size_of_target_headers)
          : static_cast<std::uint64_t>(descriptor.delta_headers_target_offset) +
                static_cast<std::uint64_t>(descriptor.delta_headers_source_size);
  if (target_size_wide == 0u || target_size_wide > kMaxHeaderBytes) {
    if (error) *error = "XEXP header-delta target header size is invalid.";
    return false;
  }
  const auto target_size = static_cast<std::uint32_t>(target_size_wide);
  if (descriptor.delta_headers_target_offset > target_size) {
    if (error) *error = "XEXP header-delta target range is outside the target header.";
    return false;
  }
  const auto delta_target_size = target_size - descriptor.delta_headers_target_offset;
  if (descriptor.delta_headers_source_size > delta_target_size) {
    if (error) *error = "XEXP header-delta source range does not fit at its target offset.";
    return false;
  }

  // Working buffer: a copy of the base header, grown (never shrunk yet) to
  // cover both it and the target header, so the splice below always has
  // valid source bytes to read even when the target header is larger.
  const auto working_size = std::max<std::size_t>(base_header_bytes.size(), target_size);
  std::vector<std::byte> working(working_size, std::byte{0});
  std::copy(base_header_bytes.begin(), base_header_bytes.end(), working.begin());

  // The bounds above are already tighter than what splice_delta_region()
  // itself re-checks (source vs base_header_bytes.size(), target vs
  // working.size() which may exceed target_size while growing) - both are
  // kept since splice_delta_region() is the shared primitive with the
  // image-region delta below, which relies on its own checks alone.
  if (!splice_delta_region(base_header_bytes, descriptor.delta_headers_source_offset,
                           descriptor.delta_headers_source_size, working, descriptor.delta_headers_target_offset,
                           "header-delta", error)) {
    return false;
  }

  if (working_size > target_size) {
    std::fill(working.begin() + target_size, working.end(), std::byte{0});
  }

  const bool has_record = descriptor.header_patch_compressed_len != 0u ||
                          descriptor.header_patch_uncompressed_len != 0u ||
                          descriptor.header_patch_old_addr != 0u || descriptor.header_patch_new_addr != 0u;
  if (has_record) {
    if (!range_valid(descriptor.header_patch_data_offset, descriptor.header_patch_compressed_len,
                     patch_file_bytes.size())) {
      if (error) *error = "XEXP header-delta LZX record payload is truncated.";
      return false;
    }
    std::array<std::byte, kDeltaPatchRecordHeaderSize> record_header{};
    const auto put_be32 = [&](std::size_t off, std::uint32_t v) {
      record_header[off + 0] = static_cast<std::byte>((v >> 24) & 0xFFu);
      record_header[off + 1] = static_cast<std::byte>((v >> 16) & 0xFFu);
      record_header[off + 2] = static_cast<std::byte>((v >> 8) & 0xFFu);
      record_header[off + 3] = static_cast<std::byte>(v & 0xFFu);
    };
    const auto put_be16 = [&](std::size_t off, std::uint16_t v) {
      record_header[off + 0] = static_cast<std::byte>((v >> 8) & 0xFFu);
      record_header[off + 1] = static_cast<std::byte>(v & 0xFFu);
    };
    put_be32(0u, descriptor.header_patch_old_addr);
    put_be32(4u, descriptor.header_patch_new_addr);
    put_be16(8u, descriptor.header_patch_uncompressed_len);
    put_be16(10u, descriptor.header_patch_compressed_len);

    std::vector<std::byte> record_bytes(record_header.begin(), record_header.end());
    const auto payload = patch_file_bytes.subspan(descriptor.header_patch_data_offset,
                                                   descriptor.header_patch_compressed_len);
    record_bytes.insert(record_bytes.end(), payload.begin(), payload.end());

    const auto patched = lzx::apply_delta_patch_records(record_bytes, window_bits, working);
    if (!patched.ok) {
      if (error) *error = "XEXP header-delta LZX patch failed: " + patched.error;
      return false;
    }
  }

  working.resize(target_size);
  out_header = std::move(working);
  return true;
}

}  // namespace xenon::xbox::detail
