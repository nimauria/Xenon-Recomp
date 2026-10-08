#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

#include "xbox/xex/xex_loader_internal.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_lzx.hpp"

namespace xenon::xbox {

using namespace detail;

bool apply_title_update(const XexImage& base_image, std::span<const std::byte> update_bytes,
                       XexImage& out_image, std::string* error) {
  out_image = {};
  if (base_image.effective_image.empty()) {
    if (error) *error = "No base executable image is available for title-update application.";
    return false;
  }
  if (base_image.header_bytes.empty()) {
    if (error) *error = "Base image has no header bytes available for title-update application.";
    return false;
  }
  if (update_bytes.empty()) {
    if (error) *error = "Title-update payload is empty.";
    return false;
  }
  if (update_bytes.size() < 0x18u) {
    if (error) *error = "Title-update payload is too small to contain a valid header.";
    return false;
  }
  if (detect_xex_format(update_bytes) == XexFormat::Unknown) {
    if (error) *error = "Title-update payload is not a recognized XEX1/XEX2 image.";
    return false;
  }

  // Preliminary pass over the *patch file's own* on-disk header: enough to
  // locate its delta-patch descriptor and file-format-info window size,
  // before deciding whether that on-disk header needs XEXP header-region
  // delta reconstruction against the base image's header
  // (see reconstruct_patch_header_bytes()).
  const auto patch_header_size = read_be32(update_bytes, 8u);
  const auto patch_optional_header_count = read_be32(update_bytes, 0x14u);
  if (patch_header_size < 0x18u || patch_header_size > update_bytes.size() ||
      patch_header_size > kMaxHeaderBytes) {
    if (error) *error = "Title-update header size is invalid.";
    return false;
  }

  std::vector<OptionalHeaderEntry> patch_entries;
  if (!enumerate_optional_headers(update_bytes, patch_header_size, patch_optional_header_count,
                                  patch_entries, error)) {
    return false;
  }

  XexDeltaPatchDescriptor patch_descriptor{};
  parse_delta_patch_descriptor(update_bytes, patch_header_size, patch_entries, patch_descriptor);

  FileFormatInfo patch_format_info{};
  parse_file_format_info(update_bytes, patch_header_size, patch_entries, patch_format_info);
  std::uint32_t header_window_bits = 17u;  // 128 KiB default, matches FileFormatInfo's own default.
  {
    std::uint32_t bits = 0u;
    for (auto size = patch_format_info.window_size; size > 1u; size >>= 1u) ++bits;
    if (lzx::valid_window_bits(bits)) header_window_bits = bits;
  }

  std::vector<std::byte> reconstructed_header;
  if (!reconstruct_patch_header_bytes(base_image.header_bytes, update_bytes.subspan(0u, patch_header_size),
                                      update_bytes, patch_descriptor, header_window_bits,
                                      reconstructed_header, error)) {
    return false;
  }

  // Reassemble a byte buffer where the (possibly reconstructed) header
  // replaces the patch's on-disk header verbatim; the body (compression/
  // encryption/PE parsing) is untouched by header reconstruction and is
  // still located via the patch *file's* own on-disk header_size.
  std::vector<std::byte> full_patch_bytes(reconstructed_header.begin(), reconstructed_header.end());
  const auto patch_body = update_bytes.subspan(patch_header_size);
  full_patch_bytes.insert(full_patch_bytes.end(), patch_body.begin(), patch_body.end());

  // The reconstructed header's own header_size field must reflect where we
  // actually placed the header/body boundary in full_patch_bytes, so
  // parse_xex_image() (which derives that boundary from the field, not from
  // an externally-tracked size) splits them at the same point. This must be
  // enforced unconditionally, not just when the reconstructed size differs
  // from the patch file's own on-disk header size: a header-delta
  // reconstruction that happens to produce a same-sized buffer can still
  // carry a *stale* header_size field value at content offset 8 (copied
  // verbatim from the base header, e.g. when neither the splice nor the LZX
  // record's target range covers that field) - the field must always be
  // re-derived from the buffer's actual size, never trusted as incidental
  // byte content.
  if (reconstructed_header.size() < 0x0Cu) {
    if (error) *error = "XEXP reconstructed header is too small to contain a valid header.";
    return false;
  }
  {
    const auto new_header_size = static_cast<std::uint32_t>(reconstructed_header.size());
    full_patch_bytes[8] = static_cast<std::byte>((new_header_size >> 24) & 0xFFu);
    full_patch_bytes[9] = static_cast<std::byte>((new_header_size >> 16) & 0xFFu);
    full_patch_bytes[10] = static_cast<std::byte>((new_header_size >> 8) & 0xFFu);
    full_patch_bytes[11] = static_cast<std::byte>(new_header_size & 0xFFu);
  }

  // Patch-kind/identity validation is performed against the patch *file's
  // own* on-disk declarations, never against the reconstructed header: real
  // hardware determines is_patch()/is_full_patch()/is_delta_patch() from the
  // patch XEX's own physical module_flags before any header-delta
  // reconstruction runs, and a header-region delta that splices a large
  // (even whole-header) range from the base is expected to also overwrite
  // module_flags/title_id/media_id with the base's own values in the
  // *reconstructed* buffer - that reconstructed state describes the
  // resulting effective title image (correctly no longer "a patch"), not
  // what the patch payload itself claimed to be.
  const auto patch_module_flags = read_be32(update_bytes, 4u);
  const bool patch_is_patch = (patch_module_flags & module_flags::kModulePatch) != 0u;
  const bool patch_is_full_patch = (patch_module_flags & module_flags::kPatchFull) != 0u;
  const bool patch_is_delta_patch = (patch_module_flags & module_flags::kPatchDelta) != 0u;

  if (!patch_is_patch) {
    if (error) *error = "Payload is not a XEX title-update (XEX_MODULE_MODULE_PATCH not set).";
    return false;
  }

  XexImage patch_declared{};
  parse_execution_info(update_bytes, patch_header_size, patch_entries, patch_declared);
  if (patch_declared.title_id != base_image.title_id) {
    if (error) *error = "Title-update title ID does not match the base image.";
    return false;
  }
  if (patch_declared.media_id != 0u && base_image.media_id != 0u &&
      patch_declared.media_id != base_image.media_id) {
    if (error) *error = "Title-update media ID does not match the base image.";
    return false;
  }
  if (patch_descriptor.present) {
    const auto base_signature_digest = crypto::sha1(base_image.security.rsa_signature);
    if (!std::equal(base_signature_digest.begin(), base_signature_digest.end(),
                    patch_descriptor.base_signature_digest.begin())) {
      if (error) *error = "Title-update base-signature digest does not match the base image.";
      return false;
    }
    if (patch_descriptor.source_version.value != base_image.execution_info.version.value) {
      if (error) *error = "Title-update source version does not match the base image's version.";
      return false;
    }
  }

  if (!patch_is_full_patch && !patch_is_delta_patch) {
    if (error) *error = "Title-update is neither a full nor delta patch (unsupported patch kind).";
    return false;
  }

  XexImage patch_image{};
  std::string parse_error;
  if (!parse_xex_image(full_patch_bytes, patch_image, &parse_error, base_image.effective_image)) {
    if (error) *error = "Title-update image failed to parse: " + parse_error;
    return false;
  }

  // For both patch kinds, parse_xex_image() has already produced the
  // correct effective image: XEX_MODULE_PATCH_DELTA bodies were LZXDELTA-
  // decoded against base_image.effective_image as reference data (via
  // reference_image above), and XEX_MODULE_PATCH_FULL bodies are a complete
  // replacement image in their own right.
  out_image = std::move(patch_image);
  return true;
}

}  // namespace xenon::xbox
