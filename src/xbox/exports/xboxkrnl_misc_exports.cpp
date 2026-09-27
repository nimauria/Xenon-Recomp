#include "xenon/xbox/xboxkrnl_misc_exports.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;

// Standard SHA-1 (FIPS 180-4), implemented directly since this is a pure,
// fully-specified algorithm with no hardware/security-key dependency -
// XeCryptSha is a plain hash function real titles use for content
// checksums, not console DRM/signing (that is XeKeysConsole*, explicitly
// NOT implemented - see xboxkrnl_rtl_exports.cpp's neighboring exports for
// the project's general crypto-scope policy).
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
// store (region, language, AV pack, parental controls, etc. - see
// rexglue-sdk's xeExGetXConfigSetting) Xenon does not model. Rather than
// fabricate plausible-looking configuration data, this honestly reports
// STATUS_NOT_FOUND for every category/setting - the same answer real
// hardware gives for an unrecognized setting - and always writes a
// required-size of 0. A title that treats this failure as "use my own
// built-in default" (the documented/common pattern) behaves correctly;
// one that requires a specific real XConfig value to boot would need that
// value modeled for real, which this deliberately does not guess at.
bool ex_get_xconfig_setting_export(ExportCallContext& context) {
  const auto required_size_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[7]);
  if (required_size_ptr != 0u) {
    context.memory.write16_be(required_size_ptr, 0u);
  }
  constexpr std::uint32_t kStatusNotFound = 0xC0000225u;
  context.cpu.gpr[3] = kStatusNotFound;
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

}  // namespace

bool register_xboxkrnl_misc_exports(core::ExportRegistry& registry) {
  bool ok = true;

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
    desc.partial = true;
    desc.partial_note =
        "always reports STATUS_NOT_FOUND rather than modeling a real "
        "XConfig settings store - correct for titles that fall back to "
        "their own defaults on failure, wrong for one that requires a "
        "specific real setting value to boot";
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
