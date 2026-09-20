#pragma once

// Self-contained AES-128 primitive used by the XEX loader to unwrap the
// per-title image key from XEX security info and to decrypt XEX_ENCRYPTION_NORMAL
// image bodies. Deliberately isolated from the rest of the runtime: this file
// has no dependency on memory/cpu/kernel subsystems so it can be unit tested
// against the standard FIPS-197 test vectors independently of XEX parsing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xenon::xbox::crypto {

using AesKey = std::array<std::byte, 16>;
using AesBlock = std::array<std::byte, 16>;

// The two fixed 128-bit keys retail/devkit Xbox 360 XEX images use to wrap
// (AES-ECB encrypt) the per-title image key stored in
// XexSecurityInfo::encrypted_image_key. These are not secrets: they have been
// public since the original Xbox 360 hypervisor/XeCrypt key leaks and are
// reproduced by every open-source XEX tool (xenia, xextool, abgx360, ...).
// Retail-signed titles use kRetailKey; devkit/test-signed images use
// kDevkitKey (all-zero).
[[nodiscard]] const AesKey& retail_key() noexcept;
[[nodiscard]] const AesKey& devkit_key() noexcept;

// Single AES-128 block operations (FIPS-197). Used directly to unwrap the
// 16-byte encrypted image key (a single ECB block, no chaining).
void aes128_encrypt_block(const AesKey& key, const AesBlock& plaintext,
                          AesBlock& out_ciphertext) noexcept;
void aes128_decrypt_block(const AesKey& key, const AesBlock& ciphertext,
                          AesBlock& out_plaintext) noexcept;

// AES-128-CBC with a zero IV, matching the XEX image-body encryption scheme
// (session key produced by unwrapping XexSecurityInfo::encrypted_image_key).
// `input` must be a multiple of 16 bytes; `output` must be sized identically.
// Returns false (and leaves output untouched) if the size is not block
// aligned - callers must not silently truncate malformed encrypted payloads.
[[nodiscard]] bool aes128_cbc_decrypt(const AesKey& key,
                                     std::span<const std::byte> input,
                                     std::span<std::byte> output) noexcept;
void aes128_cbc_encrypt(const AesKey& key, std::span<const std::byte> input,
                        std::span<std::byte> output) noexcept;

// Unwraps the per-title AES image key: AES-128-ECB-decrypt the 16-byte
// `encrypted_image_key` (XexSecurityInfo::encrypted_image_key) using
// `wrapping_key` (retail_key() or devkit_key()).
[[nodiscard]] AesKey unwrap_image_key(const AesKey& wrapping_key,
                                      const AesKey& encrypted_image_key) noexcept;

// SHA-1, used for XEX compressed-block digests and delta-patch base-image
// signature validation. Self-contained, deterministic, and independently
// testable against the standard FIPS-180 vectors.
using Sha1Digest = std::array<std::byte, 20>;
[[nodiscard]] Sha1Digest sha1(std::span<const std::byte> data) noexcept;

}  // namespace xenon::xbox::crypto
