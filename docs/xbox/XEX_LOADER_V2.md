# Xenon XEX Loader V2 - Retail Image Pipeline

## Purpose

XEX Loader V2 replaces the metadata-first XEX loader (`docs/xbox/XEX_LOADER.md`) with a
production pipeline that takes a real retail (or devkit) Xbox 360 XEX1/XEX2 file all the
way to the effective executable/data image Xenon's native recompilation and CPU V2/Memory
V2 runtime needs:

```
XEX file
 -> base header + optional headers + security info + page descriptors
 -> AES-128-CBC decryption (if encrypted)
 -> decompression (None / Basic / Normal-LZX / Delta-LZX)
 -> effective image (a real PE file, starting at offset 0)
 -> PE sections / imports / exports / TLS / relocations / exception metadata
 -> Memory V2 mapping with real per-page R/W/X protection
 -> structured import/export metadata for Runtime/ExportRegistry
```

Xenon owns this whole pipeline; game modules never implement any part of it.

## What changed from V1

V1 (`src/xbox/xex/xex_loader.cpp`, pre-rewrite) was a useful foundation but not a retail
loader:

- It located the PE image by **scanning for an `MZ` signature** after the header instead
  of trusting the XEX body location (`header_size`) and treated the raw file as the
  "effective image".
- It never parsed **security info** or **page descriptors** - section protections came
  from PE characteristics alone, and `image_flags`/`load_address`/`image_size` were
  unused.
- It had **no encryption support** at all (`XEX_ENCRYPTION_NORMAL` bodies were simply
  unreadable/garbage).
- It had **no real compression support**: `XEX_COMPRESSION_BASIC` and
  `XEX_COMPRESSION_NORMAL` (LZX) bodies were not decoded.
- `apply_title_update()` was a byte-level "if patch byte != 0, replace base byte" hack,
  not XEXP delta-patch semantics.
- Imports were parsed only from a (usually absent, for title EXEs) PE import directory;
  the XEX-native `XEX_HEADER_IMPORT_LIBRARIES` optional header was never read.

V2 fixes all of the above with real implementations (see below), including both the
XEXP header-region delta and the XEXP image/data delta's real `xex2_delta_patch`
record-chain framing (see "Compression" and "XEXP / title-update patching"). The one
remaining, narrower scope note (XEX1's "HVI" root-import-table mechanism is not resolved
into `XexImport` entries) is documented inline in "Header/security parsing".

## Header/security parsing

`parse_xex_image()` (`src/xbox/xex/xex_loader.cpp`) parses, from the real on-disk byte
layout (verified against XEX2 structures used by xenia-project/xenia for research; no
xenia source was copied):

- The XEX1/XEX2 base header (`module_flags`, `header_size`, `security_offset`,
  `optional_header_count`) - this 0x18-byte struct and the optional-header-table format
  are shared between XEX1 and XEX2.
- The optional header table (`xex2_opt_header[]`): each entry's low key byte selects
  whether its value is used inline (classes `0x00`/`0x01`) or as a byte offset to a
  structure (every other class, `0xFF` being self-sized).
- `XexSecurityInfo`: `image_size`, `rsa_signature`, `image_flags`, `load_address`,
  `encrypted_image_key`, `region`, `allowed_media_types`, and the full
  `page_descriptor[]` array (section type + page-count run + digest). **XEX1's
  security_info has a genuinely different on-disk layout from XEX2** (0x168 bytes vs
  0x184, different field order, no `header_digest`/`export_table`/
  `import_table_count` fields of its own - a `xex1_root_import_address` field takes
  their place), not merely the same struct under a different magic; `parse_security_info()`
  branches on `XexFormat` and decodes each format's real byte layout (verified against
  the real `xex1::SecurityInfo`/`xex2::SecurityInfo` structures for research; no source
  copied - see "Research rule" below). `tests/xbox/xex_loader_tests.cpp` builds and
  loads a real `'XEX1'`-magic fixture through the full production path (not just a
  relabelled XEX2 fixture) to prove this.
- `XexExecutionInfo` (`XEX_HEADER_EXECUTION_INFO`): media ID, version, base version,
  title ID, platform/disc/executable-table bytes, savegame ID.
- `XEX_HEADER_ORIGINAL_PE_NAME`, `XEX_HEADER_TLS_INFO` (native TLS descriptor, preferred
  over a PE TLS directory when present), `XEX_HEADER_ENTRY_POINT` /
  `XEX_HEADER_IMAGE_BASE_ADDRESS` (inline values), `XEX_HEADER_FILE_FORMAT_INFO`
  (encryption/compression type + compression parameters), `XEX_HEADER_IMPORT_LIBRARIES`,
  `XEX_HEADER_DELTA_PATCH_DESCRIPTOR`.
- Page descriptors are the ground truth for section R/W/X: after PE section parsing,
  every section's protection is cross-checked against the page-descriptor run covering
  its virtual address range, and the descriptor wins when it covers that range (matching
  what the real Xbox 360 loader enforces), with PE characteristics as the fallback.

## Encryption

`src/xbox/xex/xex_crypto.{hpp,cpp}` is a self-contained AES-128 (encrypt+decrypt,
ECB-single-block and CBC) and SHA-1 implementation, isolated from the rest of the
runtime (no dependency on memory/cpu/kernel) so it is independently unit-testable
(`tests/xbox/xex_crypto_tests.cpp`, FIPS-197/FIPS-180 known-answer vectors).

- `security_info.encrypted_image_key` is unwrapped (AES-128-ECB-decrypted) with a fixed
  128-bit key: the retail key or, for devkit/test-signed images, the all-zero devkit
  key. Both are long-public (leaked with the original Xbox 360 hypervisor/XeCrypt
  material) and are reproduced by every open-source XEX tool; they are not treated as
  secrets requiring protection here.
- The image body (everything from `header_size` to EOF) is then AES-128-CBC-decrypted
  with a zero IV using the unwrapped per-title key.
- Since neither `module_flags` nor `image_flags` reliably says which fixed key wraps a
  given image, the loader tries the retail key first, then the devkit key.

### Key-selection validation (real, not heuristic)

A wrong key must never be silently accepted just because its garbage output happens to
be the right size or coincidentally parses. `decompress_body()` verifies every
successfully decrypted+decompressed effective image against
`security_info.section_digest` (`verify_first_page_digest()`): this field is
`SHA1(first_page_descriptor_run_bytes, first_page_descriptor_entry)`, a real integrity
digest carried in the XEX header itself, checked uniformly for every compression type
(`None`/`Basic`/`Normal`/`Delta`) - not only `Normal`/`Delta`'s incidental per-block
SHA-1 checks, which previously left `None`/`Basic` bodies with no cryptographic
validation at all (a wrong key would only be caught, non-deterministically, if the
resulting garbage happened to fail PE parsing later). A digest mismatch on the retail
key triggers the devkit-key retry exactly like a decompression failure does; a mismatch
on both is a hard decode error, never a silent fallback to garbage data.

## Compression

`src/xbox/xex/xex_lzx.{hpp,cpp}` implements:

- **None**: the (decrypted) body truncated/validated to `security_info.image_size`.
- **Basic** (`XEX_COMPRESSION_BASIC`): the `(data_size, zero_size)` block table living in
  the header is walked, copying `data_size` decrypted bytes and synthesizing
  `zero_size` zero bytes per block.
- **Normal** (`XEX_COMPRESSION_NORMAL`): a real LZX decoder (original implementation
  against the published LZX bitstream format - block types verbatim/aligned/
  uncompressed, canonical Huffman main/length/aligned-offset trees transmitted via a
  20-symbol pretree with RLE/delta coding, recursively-derived position-slot table,
  R0-R2 repeated-offset cache, 32 KiB chunk realignment; x86 E8 call translation is not
  implemented as XEX images are PowerPC, never x86). The XEX container's
  `xex2_compressed_block_info` chain (block size + SHA-1 digest, inner 2-byte-length-
  prefixed chunks) is de-gathered into one continuous bitstream and decoded in one pass.
- **Delta** (`XEX_COMPRESSION_DELTA`, XEXP title updates, `apply_image_delta()`): the
  real XEXP image/data delta algorithm - **not** LZX/LZXDELTA framing over a continuous
  bitstream. `working`, a fresh buffer seeded from a copy of `reference_image` (the base
  image's own `effective_image` - read-only, never mutated) sized to
  `max(reference_image.size(), security_info.image_size)`, optionally receives one
  whole-region splice from `XEX_HEADER_DELTA_PATCH_DESCRIPTOR`'s
  `delta_image_source_offset/target_offset/source_size` (`splice_delta_region()`,
  shared with the header-region delta below), is zero-padded past the target size if
  shrinking, and then has every block of the *same* `xex2_compressed_block_info` outer
  chain `XEX_COMPRESSION_NORMAL` uses applied as a `xex2_delta_patch` record chain
  directly onto it (`lzx::apply_delta_patch_records()`, see "XEXP delta-patch record
  format" below) - the two compression types share only that outer block/hash
  container; their block *payloads* are structurally different framings
  (chunk-table-over-a-continuous-LZX-bitstream vs. a record chain), and this loader
  dispatches on `compression_type` before ever interpreting a payload, never guesses.

Malformed compressed data (truncated block chains, bad block sizes, SHA-1 digest
mismatches, invalid LZX block types/window sizes, out-of-bounds matches, out-of-range
delta-record offsets/lengths, integer-overflow-prone target-size arithmetic) is
rejected with a descriptive error, never partially/garbage-loaded - the result is
always assembled in a separate output buffer, so a rejected or partially-processed
delta application can never leave the base image's bytes (or a caller's output
parameter) mutated.

## XEXP / title-update patching

`apply_title_update(base_image, update_bytes, out_image, error)`:

1. Reads the patch file's own on-disk `module_flags` (is_patch/is_full_patch/
   is_delta_patch) and its own `XEX_HEADER_EXECUTION_INFO` (title_id/media_id) *before*
   any header reconstruction, exactly like real hardware's `is_patch()`/title-identity
   checks - these must reflect what the patch payload itself declared, not whatever a
   header-region delta later produces (see step 3; a delta that splices a large range
   from the base is expected to also carry over the base's own module_flags/title_id/
   media_id into the *reconstructed* header, which correctly describes the resulting
   effective title image, no longer "a patch").
2. Rejects payloads without `XEX_MODULE_MODULE_PATCH` set, and validates title ID
   (always) and media ID (when both sides declare one) against the base image using
   those pre-reconstruction values.
3. **XEXP header-region delta reconstruction** (`reconstruct_patch_header_bytes()`):
   when `XEX_HEADER_DELTA_PATCH_DESCRIPTOR` is present and its
   `delta_headers_source_size` is non-zero, splices `delta_headers_source_size` bytes
   from the *base image's own header* (`delta_headers_source_offset`) into a
   `size_of_target_headers`-sized buffer (seeded from a copy of the base header) at
   `delta_headers_target_offset`, then applies the descriptor's embedded single
   `xex2_delta_patch` record (`lzx::apply_delta_patch_records()`, see below) on top -
   reproducing the real Xbox 360 XEXP header-patch algorithm (base header + header delta
   -> effective target header), not merely substituting the patch's own on-disk header.
   Every offset/size is validated (source/target range bounds, record payload bounds)
   before any bytes are touched. When `delta_headers_source_size == 0` (no header
   *content* actually changed - the common case for code/data-only updates), the
   patch's own on-disk header is used unchanged, which is itself a complete, valid
   header for that case.
4. Validates the patch's `digest_source` against SHA-1(`base_image.security.
   rsa_signature`) and its declared source version against `base_image.execution_info.
   version` when `XEX_HEADER_DELTA_PATCH_DESCRIPTOR` is present.
5. Re-parses the (possibly header-reconstructed) patch through the normal
   `parse_xex_image()` pipeline, with `reference_image = base_image.effective_image` so
   a delta-compressed body decodes directly against the base image.
6. For `XEX_MODULE_PATCH_FULL`, the update's own fully-decoded effective image *is* the
   new effective image (a full replacement title update).
7. For `XEX_MODULE_PATCH_DELTA`, the update's effective image was already reconstructed
   through step 5's LZXDELTA decode against the base image and is used directly.

The base image's bytes (`base_image.effective_image`, `base_image.header_bytes`, and the
original input span) are never mutated - `apply_title_update()` always produces a new,
independent `XexImage`.

### XEXP delta-patch record format (`xex_lzx::apply_delta_patch_records()`)

The header-region delta's embedded patch and the image/data delta's per-block payloads
both use the real `xex2_delta_patch` record format - one shared parser/applier, not two
separate interpretations of the same on-disk structure. Each record is a 12-byte header
(`old_addr`, `new_addr`, `uncompressed_len`, `compressed_len`, all big-endian) optionally
followed by `compressed_len` bytes of LZX-compressed payload:

- **Record chain termination**: a record whose four header fields are all zero
  terminates the chain early (checked before anything else, so it never needs a
  payload); otherwise the chain is walked until its byte span is exhausted.
- **`compressed_len == 0`** ("fill"): `uncompressed_len` zero bytes are written at
  `dest[new_addr]`. No payload bytes follow.
- **`compressed_len == 1`** ("copy"): `uncompressed_len` bytes are copied verbatim from
  `dest[old_addr]` to `dest[new_addr]` (snapshotted first, so this is safe even when the
  ranges overlap). No payload bytes follow, and this is *not* LZX-compressed.
- **Any other `compressed_len`**: a real LZX-compressed chunk, decoded via the same LZX
  core `decode()` uses. `dest[old_addr, old_addr+uncompressed_len)` is snapshotted and
  used as the LZXDELTA reference window (matching real LZXDELTA's relaxation of plain
  LZX's append-only constraint - the reference here is a same-length window taken from
  elsewhere in the buffer being reconstructed, not a separately-supplied base image),
  and the decoded result is written to `dest[new_addr, new_addr+uncompressed_len)`.
- **Bounds/overflow**: every `old_addr`/`new_addr`/`uncompressed_len` is checked against
  `dest.size()` using 64-bit intermediates (never a 32-bit `addr + length` that could
  wrap), and a real-LZX record's `compressed_len` payload is checked against the
  remaining record-chain bytes before it is read, so a truncated chain fails cleanly
  rather than reading past either buffer.
- **Overlap**: records are applied strictly in order, and later records may freely
  overlap earlier ones' `new_addr` ranges (the format has no non-overlap invariant to
  enforce here - a later write simply wins, exactly like real hardware's straightforward
  sequential application).
- **Target/output size**: established by the *caller* before any record is applied -
  the header-region delta sizes its buffer from `size_of_target_headers` (or the
  `delta_headers_target_offset + delta_headers_source_size` fallback), the image-region
  delta from `security_info.image_size` - never inferred from the record chain itself.

Verified against the real Xbox 360 XEXP algorithm (research: xenia-project/xenia's
`XexModule::ApplyPatch()`/`lzxdelta_apply_patch()` and emoose/idaxex's (BSD-3-Clause)
`xex2_delta_patch`/`xex2_opt_delta_patch_descriptor` struct definitions, both read for
understanding only, not copied - see "Research rule" below) and by a loader-level test
fixture containing all three record kinds (`tests/xbox/xex_loader_tests.cpp`, see
"Tests").

## Effective executable identity and runtime integration

`apply_title_update()` produces a fully-parsed `XexImage` (effective bytes,
sections, imports, exports, TLS - the same shape `parse_xex_image()`
produces for any image), not on-disk file bytes. Two small additions expose
this to the rest of Xenon without introducing a second XEXP implementation:

- `map_xex_image(memory, image, out_loaded, ...)` is `load_xex()`'s mapping
  half, factored out so a caller with an already-parsed/patched `XexImage`
  (`XenonSession::load_game()`, after `apply_title_update()`) can map it
  directly instead of re-serializing it back into file bytes just to call
  `load_xex()` again. `load_xex()` itself is now `parse_xex_image()` +
  `map_xex_image()` - behavior-preserving, verified by
  `test_map_xex_image_matches_load_xex()`.
- `compute_effective_image_hash(image)` / `compute_effective_identity(base_image,
  patched_image)` give the rest of Xenon a canonical "which exact executable
  is this" identity (SHA1 of the effective bytes, plus title/media ID and
  base/effective version) - used by `XenonSession` to publish
  `effective_identity()`, by native-extension module-compatibility gating,
  and by `xenon::recomp::generate_project()` to emit a module's
  `Xenon_SupportedExecutableRevisions()` declaration automatically. See
  `docs/runtime/RUNTIME_SESSION.md`'s "Title Update Integration" for the full
  production flow (`ContentManager` selects -> `XenonSession::load_game()`
  applies -> effective image is what actually runs).

## Imports / exports / TLS / relocations

- **Imports**: `XEX_HEADER_IMPORT_LIBRARIES` (the XEX-native mechanism title EXEs
  actually use) is parsed first: library string table, each `xex2_import_library`
  record, and its `import_table[]` guest-address entries. Each entry's target 32-bit
  placeholder value (read from the effective image once decompressed) is decoded as
  `ordinal = value & 0xFFFF`, `attributes = value >> 16`. A classic PE import directory
  is parsed as a fallback only when no native import-libraries header is present.
  Resolution against the running export registry remains Runtime/ExportRegistry's job -
  the parser only exposes structure (library, ordinal, name where known, guest thunk,
  attributes).
- **Exports**: standard PE export directory (name/ordinal/address).
- **TLS**: the native `XEX_HEADER_TLS_INFO` descriptor is preferred; a PE TLS directory
  is parsed as a fallback when no native TLS header is present.
- **Relocations**: standard PE base-relocation blocks.
- **Function/exception metadata**: the PE exception-directory-shaped table of
  `(begin, end, unwind_data)` triples used for static-recompilation function
  boundaries.

## Memory V2 integration

`load_xex()` maps every PE section into `memory::AddressSpace` with the page-descriptor-
resolved protection (falling back to PE characteristics where no descriptor covers a
section), using the existing `reserve_fixed`/`commit_fixed`/`write_bytes` contracts - no
separate XEX-specific allocator. Writing section bytes through `AddressSpace::
write_bytes()` already advances Memory V2's executable-page generation tracking, so
`load_xex()` needs no separate "register executable pages" step; executable sections are
additionally surfaced via `LoadedXex::executable_ranges` for CPU V2 executable-generation
consumers.

## Multi-module support

Nothing in `parse_xex_image()`/`load_xex()` assumes a single `default.xex`: both take a
byte span and a caller-chosen load address/reference image, so later-loaded XEX/DLL
modules use the identical pipeline. `XexImage::is_patch`/`module_flags` and
`XEX_MODULE_DLL_MODULE` are exposed so callers can distinguish title EXEs, DLL modules,
and title updates without the loader hard-coding any of that policy itself.

## Tests

- `tests/xbox/xex_crypto_tests.cpp`: FIPS-197 AES-128 known-answer vector,
  encrypt/decrypt and CBC round trips, misaligned-input rejection, image-key
  wrap/unwrap round trip, FIPS-180 SHA-1 known-answer vectors.
- `tests/xbox/xex_lzx_tests.cpp`: an internal self-test of the canonical-Huffman table
  builder (including over-subscribed-code rejection) and the recursively-derived
  position-slot table against the well-known LZX slot-count table; uncompressed-block
  round trips (small/empty/odd-sized/multi-chunk, exercising the 32 KiB chunk-boundary
  realignment and multi-block chaining); rejection of invalid window bits, truncated
  streams, oversized block sizes, and invalid block types; real canonical-Huffman-coded
  VERBATIM and ALIGNED block round trips (single-block and multi-block/cross-chunk,
  exercising the cross-block code-length persistence LZX's delta-coding depends on) via
  `encode_literal_huffman_block()`, an independent from-spec test-only encoder (see
  below).
- `tests/xbox/xex_loader_tests.cpp`: malformed-header and missing-security-info
  rejection; a full synthetic retail-shaped XEX2 (security info, page descriptors,
  execution info, native TLS, native import-libraries, PE export directory) parsed and
  mapped into Memory V2, including proving page descriptors override PE section
  characteristics; a real `'XEX1'`-magic fixture through the same full pipeline,
  proving XEX1's distinct security_info layout is decoded correctly, not just XEX2's
  bytes under a different tag; `XEX_COMPRESSION_BASIC` round trip; `XEX_COMPRESSION_NORMAL`
  round trip through the real container framing + LZX decode (SHA-1-digest-mismatch
  rejection path); a **retail-shaped pipeline test**
  (`test_retail_shaped_pipeline_encrypted_huffman_normal_compression`) combining
  AES-128-CBC encryption, `XEX_COMPRESSION_NORMAL` over real canonical-Huffman-coded LZX
  blocks (alternating VERBATIM/ALIGNED across chunks), imports, TLS, exports, and
  page-descriptor-resolved section permissions, decoded through the public `load_xex()`
  entry point and verified in Memory V2 - not just `parse_xex_image()` in isolation;
  proof a wrong decryption key is rejected deterministically even for
  `XEX_COMPRESSION_NONE` (`test_wrong_key_rejected_even_for_none_compression`); full-patch
  and delta-patch title updates; a XEXP **header-region-delta** regression test that
  reconstructs a header field from a base image via `delta_headers_*` splicing plus the
  embedded LZX record (`test_title_update_header_delta_reconstructs_media_id`), plus
  malformed/truncated/out-of-range/integer-overflow rejection tests for that path; a
  XEXP **image/data-delta** regression test
  (`test_title_update_delta_patch_reconstructs_with_real_record_framing`) with a
  format-accurate fixture exercising all three record kinds in one chain (a fill record,
  a real LZX-compressed literal-write record, plus an independent descriptor-level
  whole-region splice for the "copy" case), verifying the reconstructed effective image
  **byte-for-byte** against the base image with exactly those edits applied - not merely
  that it round-trips; wrong-base-version rejection
  (`test_title_update_delta_patch_rejects_wrong_base_version`, with proof the base image
  is never mutated) and malformed-record-chain rejection
  (`test_title_update_delta_patch_rejects_malformed_records`, oversized `compressed_len`
  and a corrupted outer block hash); truncated-file rejection. All of the above run
  through the public `apply_title_update()`/`parse_xex_image()` entry points, never
  `lzx::apply_delta_patch_records()` directly. (The pre-existing title-update tests were,
  prior to this pass, defined but never actually registered in `main()` - dead code the
  test binary never ran - and are now wired in alongside the new ones.)

## Files

- `include/xenon/xbox/xex_loader.hpp`, `src/xbox/xex/xex_loader.cpp` - header/security/
  compression/PE orchestration, `load_xex()`, `apply_title_update()`.
- `include/xenon/xbox/xex_crypto.hpp`, `src/xbox/xex/xex_crypto.cpp` - AES-128, SHA-1.
- `include/xenon/xbox/xex_lzx.hpp`, `src/xbox/xex/xex_lzx.cpp` - LZX/LZXDELTA decoder.
