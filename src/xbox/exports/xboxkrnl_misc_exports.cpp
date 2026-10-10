#include "xenon/xbox/xboxkrnl_misc_exports.hpp"

#include <array>
#include <cstdint>
#include <vector>

#include "xenon/kernel/xbox_io.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
namespace status = xenon::kernel::xbox::status;

// Standard SHA-1 (FIPS 180-4), implemented directly since this is a pure,
// fully-specified algorithm with no hardware/security-key dependency -
// XeCryptSha is a plain hash function real titles use for content
// checksums, not console DRM/signing. XeKeysConsole* (below) is the
// console-private-key DRM/signing family that genuinely cannot be
// implemented correctly (no real key can exist here) - registered as an
// honest always-succeeds stub rather than left unimplemented, since AC6's
// own import table references it; see that pair's own comment for why.
class Sha1 {
 public:
  void update(const std::uint8_t* data, std::size_t length) {
    buffer_.insert(buffer_.end(), data, data + length);
    bit_length_ += static_cast<std::uint64_t>(length) * 8u;
    while (buffer_.size() >= 64u) {
      process_block(buffer_.data());
      buffer_.erase(buffer_.begin(), buffer_.begin() + 64);
    }
  }

  std::array<std::uint8_t, 20> finalize() {
    std::vector<std::uint8_t> padded = buffer_;
    padded.push_back(0x80u);
    while (padded.size() % 64u != 56u) padded.push_back(0u);
    for (int i = 7; i >= 0; --i) {
      padded.push_back(static_cast<std::uint8_t>((bit_length_ >> (i * 8)) & 0xFFu));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64u) {
      process_block(padded.data() + offset);
    }

    std::array<std::uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
      digest[i * 4 + 0] = static_cast<std::uint8_t>((h_[i] >> 24) & 0xFFu);
      digest[i * 4 + 1] = static_cast<std::uint8_t>((h_[i] >> 16) & 0xFFu);
      digest[i * 4 + 2] = static_cast<std::uint8_t>((h_[i] >> 8) & 0xFFu);
      digest[i * 4 + 3] = static_cast<std::uint8_t>(h_[i] & 0xFFu);
    }
    return digest;
  }

 private:
  static std::uint32_t rotl(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
  }

  void process_block(const std::uint8_t* block) {
    std::array<std::uint32_t, 80> w{};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = temp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
  }

  std::array<std::uint32_t, 5> h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                                  0xC3D2E1F0u};
  std::vector<std::uint8_t> buffer_{};
  std::uint64_t bit_length_{0};
};

void append_guest_bytes(Sha1& sha, ExportCallContext& context, cpu::GuestAddress address,
                        std::uint32_t length) {
  if (address == 0u || length == 0u) return;
  std::vector<std::uint8_t> bytes(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    bytes[i] = context.memory.read8(address + i);
  }
  sha.update(bytes.data(), bytes.size());
}

// XeCryptSha (ordinal 0x192 / 402)
// Guest ABI: r3/r4 = input1 ptr/size, r5/r6 = input2 ptr/size (optional,
// nullable), r7/r8 = input3 ptr/size (optional, nullable), r9 = output
// digest ptr, r10 = output size (real hardware always writes the full
// 20-byte SHA-1 digest regardless of this value - verified against
// rexglue-sdk's XeCryptSha_entry, which ignores output_size entirely) ->
// r3 = 0 (STATUS_SUCCESS). Concatenates up to three optional input buffers
// (a real, common SHA-1 API shape - e.g. hashing a header and body
// separately without requiring the caller to first concatenate them) and
// writes the digest.
bool xe_crypt_sha_export(ExportCallContext& context) {
  Sha1 sha;
  append_guest_bytes(sha, context, static_cast<cpu::GuestAddress>(context.cpu.gpr[3]),
                     static_cast<std::uint32_t>(context.cpu.gpr[4]));
  append_guest_bytes(sha, context, static_cast<cpu::GuestAddress>(context.cpu.gpr[5]),
                     static_cast<std::uint32_t>(context.cpu.gpr[6]));
  append_guest_bytes(sha, context, static_cast<cpu::GuestAddress>(context.cpu.gpr[7]),
                     static_cast<std::uint32_t>(context.cpu.gpr[8]));
  const auto digest = sha.finalize();

  const auto output_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[9]);
  if (output_ptr != 0u) {
    for (std::size_t i = 0; i < digest.size(); ++i) {
      context.memory.write8(output_ptr + static_cast<cpu::GuestAddress>(i), digest[i]);
    }
  }
  context.cpu.gpr[3] = 0u;
  return true;
}

// ExGetXConfigSetting (ordinal 0x10 / 16)
// Guest ABI: r3 = category (WORD), r4 = setting (WORD), r5 = buffer ptr,
// r6 = buffer size (WORD), r7 = out required-size ptr (optional) ->
// r3 = NTSTATUS. Real hardware reads from an on-console XConfig settings
// store. Xenon implements the stable retail defaults supported by Xenia's
// researched compatibility path, including its exact category/setting and
// buffer-parameter validation. Multi-byte values are written in guest big
// endian through MemoryPort rather than copied in host byte order.
bool ex_get_xconfig_setting_export(ExportCallContext& context) {
  constexpr std::uint32_t kStatusInvalidParameter1 = 0xC00000EFu;
  constexpr std::uint32_t kStatusInvalidParameter2 = 0xC00000F0u;
  constexpr std::uint32_t kStatusInvalidParameter3 = 0xC00000F1u;
  const auto category = static_cast<std::uint16_t>(context.cpu.gpr[3]);
  const auto setting = static_cast<std::uint16_t>(context.cpu.gpr[4]);
  const auto buffer = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);
  const auto buffer_size = static_cast<std::uint16_t>(context.cpu.gpr[6]);
  const auto required_size_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[7]);
  std::uint16_t setting_size = 0u;
  std::uint32_t value = 0u;
  bool byte_value = false;

  if (category == 0x0002u) {  // XCONFIG_SECURED_CATEGORY
    if (setting != 0x0002u) {  // XCONFIG_SECURED_AV_REGION
      // Real hardware leaves *required_size_ptr untouched for an
      // unrecognized category/setting - only a request that resolves to a
      // real setting reports a size, on any outcome of that request.
      context.cpu.gpr[3] = kStatusInvalidParameter2;
      return true;
    }
    setting_size = 4u;
    value = 0x00001000u;  // USA/Canada AV region.
  } else if (category == 0x0003u) {  // XCONFIG_USER_CATEGORY
    switch (setting) {
      case 0x0001u:  // TIME_ZONE_BIAS
      case 0x0002u:  // TIME_ZONE_STD_NAME compatibility value
      case 0x0003u:  // TIME_ZONE_DLT_NAME compatibility value
      case 0x0004u:  // TIME_ZONE_STD_DATE compatibility value
      case 0x0005u:  // TIME_ZONE_DLT_DATE compatibility value
      case 0x0006u:  // TIME_ZONE_STD_BIAS
      case 0x0007u:  // TIME_ZONE_DLT_BIAS
      case 0x000Cu:  // RETAIL_FLAGS
        setting_size = 4u;
        value = 0u;
        break;
      case 0x0009u:  // LANGUAGE (English)
        setting_size = 4u;
        value = 1u;
        break;
      case 0x000Au:  // VIDEO_FLAGS
        setting_size = 4u;
        value = 0x00040000u;
        break;
      case 0x000Eu:  // COUNTRY (United States)
        setting_size = 1u;
        value = 103u;
        byte_value = true;
        break;
      default:
        context.cpu.gpr[3] = kStatusInvalidParameter2;
        return true;
    }
  } else {
    context.cpu.gpr[3] = kStatusInvalidParameter1;
    return true;
  }

  // Real hardware only reports *required_size_ptr on a path that would
  // otherwise return success (including the buffer==NULL/buffer_size==0
  // size-query pattern) - not on STATUS_BUFFER_TOO_SMALL or
  // STATUS_INVALID_PARAMETER_3, so a caller cannot use it to retry with a
  // resized buffer after either of those failures.
  if (buffer == 0u) {
    if (buffer_size != 0u) {
      context.cpu.gpr[3] = kStatusInvalidParameter3;
      return true;
    }
    if (required_size_ptr) context.memory.write16_be(required_size_ptr, setting_size);
    context.cpu.gpr[3] = status::Success;
    return true;
  }
  if (buffer_size < setting_size) {
    context.cpu.gpr[3] = status::BufferTooSmall;
    return true;
  }
  if (byte_value) {
    context.memory.write8(buffer, static_cast<std::uint8_t>(value));
  } else {
    context.memory.write32_be(buffer, value);
  }
  if (required_size_ptr) context.memory.write16_be(required_size_ptr, setting_size);
  context.cpu.gpr[3] = status::Success;
  return true;
}

// ExRegisterTitleTerminateNotification (ordinal 0x15 / 21)
// Guest ABI: r3 = X_EX_TITLE_TERMINATE_REGISTRATION* reg, r4 = create
// (nonzero = register, zero = unregister) -> r3 = NTSTATUS. Real hardware
// invokes the registered callback when the running title is about to be
// replaced/terminated (returning to dashboard, launching another title).
// Xenon has no such title-replacement flow yet (docs/runtime/
// RUNTIME_SESSION.md: one XenonSession runs exactly one title for its
// entire process lifetime), so there is no real event for a stored
// callback to ever fire on - accepting the registration and reporting
// success (without actually storing/invoking anything) matches real
// guest-visible behavior for this process's entire lifetime, since the
// notification this API promises genuinely never becomes due.
bool ex_register_title_terminate_notification_export(ExportCallContext& context) {
  context.cpu.gpr[3] = 0u;
  return true;
}

// XeKeysConsolePrivateKeySign/XeKeysConsoleSignatureVerification (ordinals
// 0x256/0x257) - sign/verify a hash with the console's fused hardware
// private key. That key is a real per-console secret burned into retail
// silicon; it cannot exist in this (or any) software recompilation, so
// byte-correct real signing/verification is permanently impossible here -
// not a gap more research or effort closes. Every available reference
// (xenia, rexglue-sdk's XeKeysConsolePrivateKeySign_entry/
// XeKeysConsoleSignatureVerification_entry) independently arrives at the
// same answer: report success unconditionally, without writing a
// plausible-looking fake signature. That is the one behavior that is both
// implementable and does not actively mislead a caller - a title that signs
// then immediately verifies its own data via these two calls observes a
// self-consistent "it worked," and nothing else in this codebase (no real
// Xbox Live/disc-authentication consumer) ever checks the signature bytes
// XeKeysConsolePrivateKeySign would have produced, so there is no later
// consumer to actively deceive either. Registered (rather than left
// unresolved) because AC6's own import table references both ordinals - an
// unhandled-import fault if either is actually called would be strictly
// worse than this honest, reference-matched stub.
bool xe_keys_console_private_key_sign_export(ExportCallContext& context) {
  context.cpu.gpr[3] = status::Success;
  return true;
}
bool xe_keys_console_signature_verification_export(ExportCallContext& context) {
  context.cpu.gpr[3] = status::Success;
  return true;
}

}  // namespace

bool register_xboxkrnl_misc_exports(core::ExportRegistry& registry) {
  bool ok = true;

  {
    core::ExportDescriptor desc{};
    desc.library = "xboxkrnl.exe";
    desc.name = "XeKeysConsolePrivateKeySign";
    desc.ordinal = 0x256u;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "no real console hardware private key can exist in a software "
        "recomp - always reports success without producing a real "
        "signature, matching xenia/rexglue-sdk's identical precedent";
    desc.handler = &xe_keys_console_private_key_sign_export;
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xboxkrnl.exe";
    desc.name = "XeKeysConsoleSignatureVerification";
    desc.ordinal = 0x257u;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.partial = true;
    desc.partial_note =
        "no real console hardware private key can exist in a software "
        "recomp - always reports the signature as valid, matching xenia/"
        "rexglue-sdk's identical precedent";
    desc.handler = &xe_keys_console_signature_verification_export;
    ok = registry.register_export(std::move(desc)) && ok;
  }

  {
    core::ExportDescriptor desc{};
    desc.library = "xboxkrnl.exe";
    desc.name = "XeCryptSha";
    desc.ordinal = 0x192u;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = &xe_crypt_sha_export;
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xboxkrnl.exe";
    desc.name = "ExGetXConfigSetting";
    desc.ordinal = 0x10u;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = &ex_get_xconfig_setting_export;
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xboxkrnl.exe";
    desc.name = "ExRegisterTitleTerminateNotification";
    desc.ordinal = 0x15u;
    desc.requirement = core::ExportRequirement::Required;
    desc.partial = true;
    desc.partial_note =
        "accepts registration/unregistration and reports success but never "
        "actually invokes the callback - Xenon has no title-replacement "
        "event for it to fire on yet (one session runs exactly one title)";
    desc.handler = &ex_register_title_terminate_notification_export;
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xbox
