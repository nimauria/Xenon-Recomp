#include "xenon/xbox/string_format.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>

namespace xenon::xbox::format {
namespace {

// First stack argument slot (argument index 8): 8-byte big-endian slots at
// r1 + 0x54 + 8 * (index - 8), as the Xbox 360 compiler lays out varargs.
constexpr std::uint32_t kStackSlotBase = 0x54u;
constexpr std::uint32_t kFirstStackIndex = 8u;
constexpr int kMaxPrecision = 512;
// A guest-controlled width beyond this is treated as a failed format (a real
// CRT would fail long before allocating it).
constexpr int kMaxWidth = 1 << 20;

struct Spec {
  bool left{}, plus{}, zero{}, space{}, alt{};
  int width{};
  int precision{-1};
  bool is_short{}, is_long{}, is_long_long{}, is_wide{};
};

std::u16string ascii(std::string_view text) {
  return std::u16string(text.begin(), text.end());
}

// Digits of `value` in `radix`, at least `min_digits` long (a zero value with
// min_digits == 0 produces no digits, as printf("%.0d", 0) does).
std::u16string digits(std::uint64_t value, unsigned radix, bool upper, int min_digits) {
  const char* alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  std::string reversed;
  while (value != 0u || static_cast<int>(reversed.size()) < min_digits) {
    reversed.push_back(alphabet[value % radix]);
    value /= radix;
  }
  std::reverse(reversed.begin(), reversed.end());
  return ascii(reversed);
}

// C99 hexadecimal floating point (%a/%A) of a finite, non-negative magnitude:
// "0x1.8p+1". Implemented here rather than delegated to the host's snprintf,
// whose digit count and exponent form differ between C runtimes; the guest must
// see the same text on every host. With no precision the mantissa is printed
// with trailing zero digits removed; an explicit precision rounds to nearest,
// ties to even (a carry out renormalizes, so 0x1.ffp+0 at precision 1 is
// "0x1.0p+1").
std::string hex_float(double magnitude, int precision, bool upper, bool alt) {
  const auto bits = std::bit_cast<std::uint64_t>(magnitude);
  const auto biased = static_cast<int>((bits >> 52) & 0x7FFu);
  std::uint64_t mantissa = bits & ((std::uint64_t{1} << 52) - 1u);
  int lead = biased == 0 ? 0 : 1;
  int exponent = biased == 0 ? (mantissa == 0 ? 0 : -1022) : biased - 1023;

  // 52 mantissa bits = 13 hex digits.
  int digits_count = 13;
  if (precision >= 0 && precision < 13) {
    const int drop_bits = (13 - precision) * 4;
    const std::uint64_t half = std::uint64_t{1} << (drop_bits - 1);
    const std::uint64_t remainder = mantissa & ((std::uint64_t{1} << drop_bits) - 1u);
    mantissa >>= drop_bits;
    const bool odd = ((precision == 0 ? static_cast<std::uint64_t>(lead) : mantissa) & 1u) != 0u;
    if (remainder > half || (remainder == half && odd)) {
      ++mantissa;
      if (precision == 0 ? mantissa != 0u : mantissa >> (precision * 4) != 0u) {
        // The fraction overflowed into the leading digit.
        if (precision == 0) {
          lead += static_cast<int>(mantissa);
          mantissa = 0;
        } else {
          mantissa &= (std::uint64_t{1} << (precision * 4)) - 1u;
          ++lead;
        }
        if (lead == 2) {
          lead = 1;
          ++exponent;
        }
      }
    }
    digits_count = precision;
  } else if (precision < 0) {
    while (digits_count > 0 && (mantissa & 0xFu) == 0u) {
      mantissa >>= 4;
      --digits_count;
    }
  }

  const char* alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  std::string text = upper ? "0X" : "0x";
  text.push_back(static_cast<char>('0' + lead));
  if (digits_count > 0 || alt) text.push_back('.');
  for (int i = digits_count - 1; i >= 0; --i) {
    text.push_back(alphabet[(mantissa >> (i * 4)) & 0xFu]);
  }
  if (precision > 13) text.append(static_cast<std::size_t>(precision - 13), '0');
  text.push_back(upper ? 'P' : 'p');
  text.push_back(exponent < 0 ? '-' : '+');
  text += std::to_string(exponent < 0 ? -exponent : exponent);
  return text;
}

// %Z: a guest ANSI_STRING (Length u16, MaximumLength u16, Buffer u32); with the
// `w` size prefix a UNICODE_STRING, whose Length is in bytes.
std::u16string read_counted_string(cpu::MemoryPort& memory, cpu::GuestAddress address,
                                   bool unicode) {
  std::u16string text;
  if (address == 0u) return u"(null)";
  const auto length = memory.read16_be(address + 0u);
  const auto buffer = memory.read32_be(address + 4u);
  if (buffer == 0u) return text;
  if (unicode) {
    for (std::uint32_t i = 0; i + 1u < length; i += 2u) {
      text.push_back(static_cast<char16_t>(memory.read16_be(buffer + i)));
    }
  } else {
    for (std::uint32_t i = 0; i < length; ++i) {
      text.push_back(static_cast<char16_t>(memory.read8(buffer + i)));
    }
  }
  return text;
}

}  // namespace

std::uint64_t RegisterArgumentSource::next64() {
  const auto index = index_++;
  if (index < kFirstStackIndex) return cpu_.gpr[3u + index];
  return memory_.read64_be(static_cast<cpu::GuestAddress>(cpu_.gpr[1]) + kStackSlotBase +
                           8u * (index - kFirstStackIndex));
}

std::uint64_t ArrayArgumentSource::next64() {
  return memory_.read64_be(array_ + 8u * index_++);
}

std::u16string read_guest_string(cpu::MemoryPort& memory, cpu::GuestAddress address, bool wide,
                                 std::size_t limit) {
  std::u16string text;
  for (std::size_t i = 0; i < limit; ++i) {
    char16_t c;
    if (wide) {
      c = static_cast<char16_t>(memory.read16_be(address + static_cast<std::uint32_t>(i) * 2u));
    } else {
      c = static_cast<char16_t>(memory.read8(address + static_cast<std::uint32_t>(i)));
    }
    if (c == 0) break;
    text.push_back(c);
  }
  return text;
}

Result format(cpu::MemoryPort& memory, std::u16string_view fmt, ArgumentSource& args, bool wide) {
  Result result;
  std::u16string& out = result.text;
  const auto fail = [&result]() {
    result.count = -1;
    result.text.clear();
    return result;
  };
  // A narrow (printf) function cannot represent a character above 0xFF.
  const auto representable = [wide](char16_t c) { return wide || c <= 0xFFu; };

  std::size_t i = 0;
  const auto peek = [&](std::size_t offset) -> char16_t {
    return i + offset < fmt.size() ? fmt[i + offset] : u'\0';
  };

  while (i < fmt.size()) {
    char16_t c = fmt[i++];
    if (c != u'%') {
      if (!representable(c)) return fail();
      out.push_back(c);
      continue;
    }
    if (i >= fmt.size()) return fail();  // a dangling '%'
    c = fmt[i++];
    if (c == u'%') {
      out.push_back(u'%');
      continue;
    }

    Spec spec{};
    // Flags.
    for (;; c = fmt[i++]) {
      if (c == u'-') spec.left = true;
      else if (c == u'+') spec.plus = true;
      else if (c == u'0') spec.zero = true;
      else if (c == u' ') spec.space = true;
      else if (c == u'#') spec.alt = true;
      else break;
      if (i >= fmt.size()) return fail();
    }
    // Width.
    if (c == u'*') {
      spec.width = static_cast<std::int32_t>(args.next32());
      if (spec.width < 0) {
        spec.left = true;
        spec.width = -spec.width;
      }
      if (i >= fmt.size()) return fail();
      c = fmt[i++];
    } else {
      while (c >= u'0' && c <= u'9') {
        spec.width = spec.width * 10 + (c - u'0');
        if (spec.width > kMaxWidth) return fail();
        if (i >= fmt.size()) return fail();
        c = fmt[i++];
      }
    }
    // Precision.
    if (c == u'.') {
      spec.precision = 0;
      if (i >= fmt.size()) return fail();
      c = fmt[i++];
      if (c == u'*') {
        spec.precision = static_cast<std::int32_t>(args.next32());
        if (spec.precision < 0) spec.precision = -1;
        if (i >= fmt.size()) return fail();
        c = fmt[i++];
      } else {
        while (c >= u'0' && c <= u'9') {
          spec.precision = spec.precision * 10 + (c - u'0');
          if (spec.precision > kMaxWidth) return fail();
          if (i >= fmt.size()) return fail();
          c = fmt[i++];
        }
      }
    }
    // Size prefixes: l, ll, h, w, I64, I32 (and L, which the Xbox CRT ignores).
    for (bool more = true; more;) {
      switch (c) {
        case u'l':
          if (peek(0) == u'l') {
            ++i;
            spec.is_long_long = true;
          } else {
            spec.is_long = true;
          }
          break;
        case u'h': spec.is_short = true; break;
        case u'w': spec.is_wide = true; break;
        case u'L': break;
        case u'I':
          if (peek(0) == u'6' && peek(1) == u'4') {
            i += 2;
            spec.is_long_long = true;
          } else if (peek(0) == u'3' && peek(1) == u'2') {
            i += 2;
          }
          break;
        default: more = false; break;
      }
      if (!more) break;
      if (i >= fmt.size()) return fail();
      c = fmt[i++];
    }

    if (spec.width > kMaxWidth) return fail();
    std::u16string prefix;
    std::u16string body;
    bool numeric = false;
    bool integer = false;
    bool finite = true;

    const auto string_is_wide = [&](bool inverted) {
      if (spec.is_long || spec.is_wide) return true;
      if (spec.is_short) return false;
      return inverted != wide;
    };
    const auto integer_conversion = [&](bool is_signed, unsigned radix, bool upper) {
      numeric = true;
      integer = true;
      const auto raw = args.next64();
      std::uint64_t magnitude;
      bool negative = false;
      if (spec.is_long_long) {
        magnitude = raw;
        if (is_signed && static_cast<std::int64_t>(raw) < 0) {
          negative = true;
          magnitude = 0u - raw;
        }
      } else if (spec.is_short) {
        const auto v = static_cast<std::int16_t>(raw);
        magnitude = is_signed ? static_cast<std::uint64_t>(v < 0 ? -static_cast<std::int64_t>(v) : v)
                              : static_cast<std::uint16_t>(raw);
        negative = is_signed && v < 0;
      } else {
        const auto v = static_cast<std::int32_t>(raw);
        magnitude = is_signed ? static_cast<std::uint64_t>(v < 0 ? -static_cast<std::int64_t>(v) : v)
                              : static_cast<std::uint32_t>(raw);
        negative = is_signed && v < 0;
      }
      const int min_digits =
          spec.precision >= 0 ? std::min(spec.precision, kMaxPrecision) : 1;
      body = digits(magnitude, radix, upper, min_digits);
      if (radix == 8 && spec.alt && (body.empty() || body.front() != u'0')) {
        body.insert(body.begin(), u'0');
      }
      if (radix == 16 && spec.alt && magnitude != 0u) prefix = upper ? u"0X" : u"0x";
      if (is_signed) {
        if (negative) prefix = u"-";
        else if (spec.plus) prefix = u"+";
        else if (spec.space) prefix = u" ";
      }
    };
    const auto float_conversion = [&](char conversion) {
      numeric = true;
      const double value = std::bit_cast<double>(args.next64());
      const bool negative = std::signbit(value);
      const double magnitude = std::fabs(value);
      finite = std::isfinite(magnitude);
      const bool upper_case = conversion == 'E' || conversion == 'F' || conversion == 'G' ||
                              conversion == 'A';
      // Non-finite and hex-float text is produced here, not by the host CRT, so
      // the guest sees identical output on every host.
      if (!finite || conversion == 'a' || conversion == 'A') {
        if (!finite) {
          body = ascii(std::isnan(magnitude) ? (upper_case ? "NAN" : "nan")
                                             : (upper_case ? "INF" : "inf"));
        } else {
          body = ascii(hex_float(magnitude, spec.precision < 0 ? -1 : std::min(spec.precision, kMaxPrecision),
                                 upper_case, spec.alt));
        }
        if (negative) prefix = u"-";
        else if (spec.plus) prefix = u"+";
        else if (spec.space) prefix = u" ";
        return;
      }
      char spec_text[8];
      char* p = spec_text;
      *p++ = '%';
      if (spec.alt) *p++ = '#';
      const bool has_precision = conversion != 'a' && conversion != 'A';
      if (has_precision) {
        *p++ = '.';
        *p++ = '*';
      }
      *p++ = conversion;
      *p = '\0';
      const int precision = spec.precision < 0 ? 6 : std::min(spec.precision, kMaxPrecision);
      int needed = has_precision ? std::snprintf(nullptr, 0, spec_text, precision, magnitude)
                                 : std::snprintf(nullptr, 0, spec_text, magnitude);
      if (needed < 0) needed = 0;
      std::string text(static_cast<std::size_t>(needed) + 1u, '\0');
      if (has_precision) {
        std::snprintf(text.data(), text.size(), spec_text, precision, magnitude);
      } else {
        std::snprintf(text.data(), text.size(), spec_text, magnitude);
      }
      text.resize(static_cast<std::size_t>(needed));
      body = ascii(text);
      if (negative) prefix = u"-";
      else if (spec.plus) prefix = u"+";
      else if (spec.space) prefix = u" ";
    };

    switch (c) {
      case u'C':
      case u'c': {
        const bool is_wide = string_is_wide(c == u'C');
        const auto value = args.next32();
        body.push_back(is_wide ? static_cast<char16_t>(value & 0xFFFFu)
                               : static_cast<char16_t>(value & 0xFFu));
        break;
      }
      case u'd':
      case u'i': integer_conversion(true, 10, false); break;
      case u'u': integer_conversion(false, 10, false); break;
      case u'o': integer_conversion(false, 8, false); break;
      case u'x': integer_conversion(false, 16, false); break;
      case u'X': integer_conversion(false, 16, true); break;
      case u'e':
      case u'E':
      case u'f':
      case u'F':
      case u'g':
      case u'G':
      case u'a':
      case u'A': float_conversion(static_cast<char>(c)); break;
      case u'p': {
        // Pointers are 32-bit on this platform: eight uppercase hex digits.
        numeric = true;
        integer = true;
        body = digits(args.next32(), 16, true, 8);
        spec.precision = 8;
        break;
      }
      case u'n': {
        const auto target = args.next32();
        if (spec.is_short) {
          memory.write16_be(target, static_cast<std::uint16_t>(out.size()));
        } else {
          memory.write32_be(target, static_cast<std::uint32_t>(out.size()));
        }
        continue;
      }
      case u'S':
      case u's': {
        const auto pointer = args.next32();
        const std::size_t cap =
            spec.precision < 0 ? static_cast<std::size_t>(INT_MAX)
                               : static_cast<std::size_t>(spec.precision);
        if (pointer == 0u) {
          body = u"(null)";
          if (body.size() > cap) body.resize(cap);
        } else {
          body = read_guest_string(memory, pointer, string_is_wide(c == u'S'), cap);
        }
        break;
      }
      case u'Z': {
        body = read_counted_string(memory, args.next32(), spec.is_wide);
        if (spec.precision >= 0 && body.size() > static_cast<std::size_t>(spec.precision)) {
          body.resize(static_cast<std::size_t>(spec.precision));
        }
        break;
      }
      default:
        return fail();  // an unknown conversion
    }

    // Padding. A '-' overrides '0'; an explicit integer precision also
    // suppresses '0'; a non-finite float pads with spaces.
    const bool zero_pad = spec.zero && !spec.left && !(integer && spec.precision >= 0) &&
                          !(numeric && !integer && !finite);
    const auto used = static_cast<int>(body.size() + prefix.size());
    const int padding = spec.width > used ? spec.width - used : 0;
    const auto emit = [&](std::u16string_view text) {
      for (const char16_t ch : text) {
        if (!representable(ch)) return false;
        out.push_back(ch);
      }
      return true;
    };
    if (!spec.left && !zero_pad) out.append(static_cast<std::size_t>(padding), u' ');
    if (!emit(prefix)) return fail();
    if (zero_pad) out.append(static_cast<std::size_t>(padding), u'0');
    if (!emit(body)) return fail();
    if (spec.left) out.append(static_cast<std::size_t>(padding), u' ');
  }

  result.count = static_cast<std::int32_t>(out.size());
  return result;
}

}  // namespace xenon::xbox::format
