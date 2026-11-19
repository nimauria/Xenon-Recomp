// Deterministic mutation fuzzing of every parser that reads untrusted input
// (XEN-008): XEX images and metadata, LZX streams and delta patches, STFS
// packages, GDFX disc images (with extraction to the host), shader microcode
// (decoding and lowering), PM4 command streams, portable GPU captures (load
// and replay), launch configurations, module input and rendering profiles,
// analysis hint sets and JSON.
//
// Each target starts from valid seeds (asserted to be accepted, so mutations
// reach the real parsing paths) and runs a fixed, seeded sequence of mutated
// inputs. A target fails on a crash or sanitizer report (run the suite under
// the windows-x64-asan / linux sanitizer presets), an escaped exception from
// a parser whose contract is an error result, a single allocation above
// kAllocationLimit (an input-controlled allocation is a resource limit the
// parser must enforce), an extracted file outside its destination, or an
// input that takes longer than kInputTimeLimit.
//
// XENON_FUZZ_ITERATIONS (default 1500) and XENON_FUZZ_SEED override the run;
// a failing input is written next to the test's temporary directory.
// XENON_FUZZ_REPLAY=<file> with XENON_FUZZ_TARGET=<name> runs that one input
// through that target and prints the stack of an oversized allocation.

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/core/json.hpp"
#include "xenon/filesystem/content_materializer.hpp"
#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/stfs_package.hpp"
#include "xenon/filesystem/stfs_package_source.hpp"
#include "xenon/filesystem/xex_metadata.hpp"
#include "xenon/gpu/backend.hpp"
#include "xenon/gpu/capture.hpp"
#include "xenon/gpu/command_processor.hpp"
#include "xenon/gpu/graphics_system.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/shader_ir.hpp"
#include "xenon/gpu/shader_translation.hpp"
#include "xenon/gpu/temporal_profile.hpp"
#include "xenon/gpu/types.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/xex_crypto.hpp"
#include "xenon/xbox/xex_loader.hpp"
#include "xenon/xbox/xex_lzx.hpp"
#if XENON_FUZZ_HAS_INPUT
#include "xenon/input/module_input_profile.hpp"
#endif
#if XENON_FUZZ_HAS_RECOMP
#include "xenon/recomp/analysis_schema_json.hpp"
#endif
#if XENON_FUZZ_HAS_LAUNCH_CONFIG
#include "launch_config.hpp"
#endif

#include "../support/test_gdfx.hpp"
#include "../support/test_stfs.hpp"
#include "../support/test_xex.hpp"

// ---------------------------------------------------------------------------
// Allocation limit. Every input here is at most a few megabytes; a single
// allocation above this size can only come from a size field the parser
// trusted. (Guest physical memory is 512 MiB, so no legitimate structure of
// these formats needs more in one piece.)
// ---------------------------------------------------------------------------
namespace {
constexpr std::size_t kAllocationLimit = std::size_t{512} << 20;
std::atomic<bool> g_oversized_allocation{false};
std::atomic<std::size_t> g_oversized_bytes{0};
bool g_trace_oversized = false;

void print_allocation_stack() {
#if defined(_WIN32)
  void* frames[48];
  const auto count = CaptureStackBackTrace(1, 48, frames, nullptr);
  const HANDLE process = GetCurrentProcess();
  SymInitialize(process, nullptr, TRUE);
  alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
  for (USHORT i = 0; i < count; ++i) {
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, reinterpret_cast<DWORD64>(frames[i]), &displacement, symbol)) {
      std::fprintf(stderr, "  %s+0x%llx\n", symbol->Name, static_cast<unsigned long long>(displacement));
    }
  }
#endif
}
}  // namespace

void* operator new(std::size_t size) {
  if (size > kAllocationLimit) {
    g_oversized_allocation.store(true);
    g_oversized_bytes.store(size);
    if (g_trace_oversized) {
      std::fprintf(stderr, "oversized allocation of %zu bytes from:\n", size);
      print_allocation_stack();
    }
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return ::operator new(size, std::nothrow);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace fs = xenon::filesystem;
namespace gpu = xenon::gpu;
namespace xbox = xenon::xbox;
using Bytes = std::vector<std::byte>;

constexpr auto kInputTimeLimit = std::chrono::seconds(20);

std::filesystem::path g_temp;

std::size_t env_size(const char* name, std::size_t fallback) {
  if (const char* value = std::getenv(name); value && *value) {
    return static_cast<std::size_t>(std::strtoull(value, nullptr, 0));
  }
  return fallback;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  assert(out.good());
}

Bytes text_bytes(std::string_view text) {
  Bytes out(text.size());
  std::memcpy(out.data(), text.data(), text.size());
  return out;
}

std::string_view as_text(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// ---------------------------------------------------------------------------
// Mutation engine (splitmix64; byte, word, structure and splice mutations).
// ---------------------------------------------------------------------------
class Mutator {
 public:
  explicit Mutator(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::size_t below(std::size_t n) { return n ? static_cast<std::size_t>(next() % n) : 0; }
  bool chance(unsigned percent) { return below(100) < percent; }

  // `regions`: byte ranges mutations prefer (headers, directory sectors);
  // empty means the whole input.
  Bytes mutate(std::span<const std::byte> seed,
               std::span<const std::pair<std::size_t, std::size_t>> regions = {},
               bool word_granular = false) {
    Bytes data(seed.begin(), seed.end());
    const std::size_t operations = 1 + below(chance(20) ? 32 : 6);
    for (std::size_t i = 0; i < operations; ++i) apply(data, regions, word_granular);
    return data;
  }

 private:
  std::size_t offset(const Bytes& data, std::span<const std::pair<std::size_t, std::size_t>> regions,
                     std::size_t width) {
    if (data.size() < width) return 0;
    const std::size_t limit = data.size() - width + 1;
    if (!regions.empty() && chance(85)) {
      const auto& region = regions[below(regions.size())];
      const auto begin = std::min(region.first, limit - 1);
      const auto end = std::min(region.second, limit);
      if (end > begin) return begin + below(end - begin);
    }
    return below(limit);
  }

  std::uint32_t interesting32(std::size_t size) {
    static constexpr std::uint32_t kValues[] = {
        0u,          1u,          2u,          3u,          4u,          7u,          8u,
        0xFu,        0x10u,       0x1Fu,       0x20u,       0x3Fu,       0x40u,       0x7Fu,
        0x80u,       0xFFu,       0x100u,      0x3FFu,      0x400u,      0x7FFu,      0x800u,
        0xFFFu,      0x1000u,     0x7FFFu,     0x8000u,     0xFFFFu,     0x10000u,    0x100000u,
        0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x82000000u, 0x0A000000u};
    switch (below(6)) {
      case 0: return static_cast<std::uint32_t>(size);
      case 1: return static_cast<std::uint32_t>(size + 1);
      case 2: return static_cast<std::uint32_t>(size ? size - 1 : 0);
      case 3: return static_cast<std::uint32_t>(next());
      default: return kValues[below(std::size(kValues))];
    }
  }

  void apply(Bytes& data, std::span<const std::pair<std::size_t, std::size_t>> regions, bool word_granular) {
    if (data.empty()) {
      data.resize(1 + below(64));
      for (auto& b : data) b = static_cast<std::byte>(next());
      return;
    }
    const auto kind = below(word_granular ? 6 : 11);
    switch (kind) {
      case 0: {  // flip one bit
        const auto at = offset(data, regions, 1);
        data[at] ^= static_cast<std::byte>(1u << below(8));
        break;
      }
      case 1:
      case 2: {  // interesting 32-bit value, big or little endian
        if (data.size() < 4) break;
        auto at = offset(data, regions, 4);
        if (word_granular || chance(60)) at &= ~std::size_t{3};
        const auto v = interesting32(data.size());
        const bool big = word_granular ? false : kind == 1;
        for (int b = 0; b < 4; ++b) {
          const auto shift = big ? (24 - 8 * b) : (8 * b);
          data[at + b] = static_cast<std::byte>((v >> shift) & 0xFFu);
        }
        break;
      }
      case 3: {  // copy a chunk over another place
        const auto len = 1 + below(std::min<std::size_t>(data.size(), 64));
        const auto from = below(data.size() - len + 1);
        const auto to = offset(data, regions, len);
        std::memmove(data.data() + to, data.data() + from, len);
        break;
      }
      case 4: {  // truncate
        data.resize(below(data.size()) & (word_granular ? ~std::size_t{3} : ~std::size_t{0}));
        break;
      }
      case 5: {  // extend
        const auto extra = (1 + below(64)) * (word_granular ? 4 : 1);
        for (std::size_t i = 0; i < extra; ++i) data.push_back(static_cast<std::byte>(chance(50) ? 0 : next()));
        break;
      }
      case 6: {  // interesting byte
        static constexpr std::uint8_t kBytes[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x2E, 0x2F, 0x5C, 0x3A};
        data[offset(data, regions, 1)] = static_cast<std::byte>(kBytes[below(std::size(kBytes))]);
        break;
      }
      case 7: {  // random byte
        data[offset(data, regions, 1)] = static_cast<std::byte>(next());
        break;
      }
      case 8: {  // erase a chunk
        const auto len = 1 + below(std::min<std::size_t>(data.size(), 32));
        const auto at = below(data.size() - len + 1);
        data.erase(data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + len));
        break;
      }
      case 9: {  // insert a chunk copied from elsewhere
        const auto len = 1 + below(std::min<std::size_t>(data.size(), 32));
        const auto from = below(data.size() - len + 1);
        const Bytes chunk(data.begin() + static_cast<std::ptrdiff_t>(from),
                          data.begin() + static_cast<std::ptrdiff_t>(from + len));
        data.insert(data.begin() + static_cast<std::ptrdiff_t>(below(data.size() + 1)), chunk.begin(),
                    chunk.end());
        break;
      }
      default: {  // 16-bit value
        if (data.size() < 2) break;
        const auto at = offset(data, regions, 2);
        const auto v = static_cast<std::uint16_t>(chance(50) ? interesting32(data.size()) : next());
        data[at] = static_cast<std::byte>(v >> 8);
        data[at + 1] = static_cast<std::byte>(v & 0xFFu);
        break;
      }
    }
  }

  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Target driver.
// ---------------------------------------------------------------------------
struct Seed {
  Bytes bytes;
  std::vector<std::pair<std::size_t, std::size_t>> regions{};
};

enum class Outcome { Accepted, Rejected };

std::uint64_t g_seed_base = 0x58454E4F4E465A5Aull;  // "XENONFZZ"
std::size_t g_iterations = 1500;
bool g_failed = false;
std::string g_replay_target;
Bytes g_replay_input;

void report_failure(std::string_view target, std::size_t iteration, std::span<const std::byte> input,
                    const std::string& what) {
  g_failed = true;
  const auto path = g_temp.parent_path() /
                    ("xenon_fuzz_failure_" + std::string(target) + "_" + std::to_string(iteration) + ".bin");
  std::error_code ec;
  write_file(path, input);
  std::cerr << "FUZZ FAILURE [" << target << "] iteration " << iteration << ": " << what
            << "\n  input (" << input.size() << " bytes) saved to " << path.string() << "\n";
}

// `run` parses one input. It returns Accepted/Rejected; an exception escaping
// it fails the target unless `exceptions_reject` (a parser whose documented
// error report is an exception).
void fuzz_target(std::string_view name, const std::vector<Seed>& seeds, std::size_t iterations,
                 const std::function<Outcome(std::span<const std::byte>)>& run,
                 bool exceptions_reject = false, bool word_granular = false) {
  if (!g_replay_target.empty()) {
    if (name != g_replay_target) return;
    g_trace_oversized = true;
    g_oversized_allocation.store(false);
    try {
      const auto outcome = run(g_replay_input);
      std::cout << "[replay] " << name << ": " << (outcome == Outcome::Accepted ? "accepted" : "rejected") << "\n";
    } catch (const std::exception& e) {
      std::cout << "[replay] " << name << ": exception " << e.what() << "\n";
    }
    if (g_oversized_allocation.load()) g_failed = true;
    return;
  }
  const auto started = std::chrono::steady_clock::now();
  std::size_t accepted = 0;
  std::size_t rejected_by_exception = 0;
  auto execute = [&](std::span<const std::byte> input, std::size_t iteration) -> std::optional<Outcome> {
    g_oversized_allocation.store(false);
    const auto begin = std::chrono::steady_clock::now();
    std::optional<Outcome> outcome;
    try {
      outcome = run(input);
    } catch (const std::bad_alloc&) {
      if (!g_oversized_allocation.load()) {
        report_failure(name, iteration, input, "std::bad_alloc escaped the parser");
        return std::nullopt;
      }
    } catch (const std::exception& e) {
      if (!exceptions_reject) {
        report_failure(name, iteration, input, std::string("exception escaped the parser: ") + e.what());
        return std::nullopt;
      }
      ++rejected_by_exception;
      outcome = Outcome::Rejected;
    }
    if (g_oversized_allocation.load()) {
      report_failure(name, iteration, input,
                     "input-controlled allocation of " + std::to_string(g_oversized_bytes.load()) +
                         " bytes (limit " + std::to_string(kAllocationLimit) + ")");
      return std::nullopt;
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    if (elapsed > kInputTimeLimit) {
      report_failure(name, iteration, input,
                     "input took " +
                         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) +
                         " ms");
      return std::nullopt;
    }
    return outcome;
  };

  for (std::size_t i = 0; i < seeds.size(); ++i) {
    const auto outcome = execute(seeds[i].bytes, ~std::size_t{0} - i);
    if (outcome != Outcome::Accepted) {
      std::cerr << "FUZZ FAILURE [" << name << "] seed " << i << " is not accepted\n";
      g_failed = true;
      return;
    }
  }

  std::uint64_t name_hash = 1469598103934665603ull;
  for (const char c : name) name_hash = (name_hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  Mutator mutator(g_seed_base ^ name_hash);
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const auto& seed = seeds[mutator.below(seeds.size())];
    const auto input = mutator.mutate(seed.bytes, seed.regions, word_granular);
    const auto outcome = execute(input, iteration);
    if (!outcome) return;
    if (*outcome == Outcome::Accepted) ++accepted;
  }
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "[fuzz] " << name << ": " << iterations << " inputs, " << accepted << " accepted, "
            << rejected_by_exception << " rejected by exception, " << seconds << " s\n";
}

Outcome ok_if(bool accepted) { return accepted ? Outcome::Accepted : Outcome::Rejected; }

// ---------------------------------------------------------------------------
// XEX
// ---------------------------------------------------------------------------
namespace xexf = xenon::test::xex;

struct XexSeeds {
  std::vector<Seed> files;
  // The Normal-compressed fixture's parts, for the LZX-body target.
  xexf::XexBuildResult normal_fixture;
  Bytes normal_header;
  Bytes normal_lzx;
};

Bytes ffi_body(std::uint16_t encryption, std::uint16_t compression, std::span<const std::uint32_t> extra) {
  xexf::ByteWriter ffi;
  ffi.append_be32(static_cast<std::uint32_t>(8u + extra.size() * 4u));
  ffi.append_be16(encryption);
  ffi.append_be16(compression);
  for (const auto v : extra) ffi.append_be32(v);
  return ffi.take();
}

Bytes join(std::span<const std::byte> a, std::span<const std::byte> b) {
  Bytes out(a.begin(), a.end());
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

XexSeeds make_xex_seeds() {
  XexSeeds seeds;
  const auto header_regions = [](const xexf::XexBuildResult& fixture, std::size_t file_size) {
    return std::vector<std::pair<std::size_t, std::size_t>>{{0, fixture.header_size + 64},
                                                            {fixture.header_size, file_size}};
  };
  // Uncompressed title with imports, TLS, exports and a stack size.
  {
    auto pe = xexf::build_pe_body(xexf::kImageBase, true);
    xexf::patch_import_placeholders(pe.bytes, pe.data_rva, 0x0042u, 0x0099u, 0u, 1u);
    xexf::XexBuildOptions opts{};
    opts.default_stack_size = 0x40000u;
    auto fixture = xexf::build_base_xex(pe, opts);
    seeds.files.push_back({fixture.file, {{0, fixture.header_size}}});
  }
  // XEX1 layout.
  {
    auto pe = xexf::build_pe_body(xexf::kImageBase, false);
    xexf::XexBuildOptions opts{};
    opts.use_xex1_format = true;
    opts.with_import_libraries = false;
    auto fixture = xexf::build_base_xex(pe, opts);
    seeds.files.push_back({fixture.file, {{0, fixture.header_size}}});
  }
  // Basic compression (two data blocks).
  {
    auto pe = xexf::build_pe_body(xexf::kImageBase, false);
    xexf::XexBuildOptions opts{};
    opts.with_import_libraries = false;
    auto fixture = xexf::build_base_xex(pe, opts);
    const auto half = static_cast<std::uint32_t>(fixture.plain_body.size() / 2u);
    const std::uint32_t blocks[] = {half, 0u, static_cast<std::uint32_t>(fixture.plain_body.size()) - half, 0u};
    auto header = xexf::rebuild_header_with_file_format_info(fixture, ffi_body(0u, 1u, blocks));
    auto file = join(header, fixture.plain_body);
    seeds.files.push_back({file, header_regions(fixture, file.size())});
  }
  // Normal (LZX, Huffman blocks) compression, plain and encrypted.
  {
    auto pe = xexf::build_pe_body(xexf::kImageBase, true);
    xexf::patch_import_placeholders(pe.bytes, pe.data_rva, 0x0042u, 0x0099u);
    xexf::XexBuildOptions opts{};
    auto fixture = xexf::build_base_xex(pe, opts);
    constexpr std::uint32_t kWindowBits = 17u;
    Bytes lzx;
    bool aligned = false;
    for (std::size_t offset = 0; offset < fixture.plain_body.size(); offset += 0x8000u) {
      const auto len = std::min<std::size_t>(0x8000u, fixture.plain_body.size() - offset);
      const auto block = xbox::lzx::encode_literal_huffman_block(
          std::span<const std::byte>(fixture.plain_body).subspan(offset, len), kWindowBits, aligned, offset == 0);
      lzx.insert(lzx.end(), block.begin(), block.end());
      aligned = !aligned;
    }
    const std::uint32_t window[] = {1u << kWindowBits};
    auto header = xexf::rebuild_header_with_file_format_info(fixture, ffi_body(0u, 2u, window));
    const auto chain = xexf::build_compressed_block_chain(lzx);
    auto file = join(header, chain);
    seeds.files.push_back({file, header_regions(fixture, file.size())});
    seeds.normal_fixture = fixture;
    seeds.normal_header = header;
    seeds.normal_lzx = lzx;

    auto padded = chain;
    padded.resize((padded.size() + 15u) & ~std::size_t{15});
    xbox::crypto::AesKey image_key{};
    for (std::size_t i = 0; i < image_key.size(); ++i) image_key[i] = static_cast<std::byte>(i * 7u + 1u);
    xbox::crypto::AesBlock wrapped{};
    xbox::crypto::aes128_encrypt_block(xbox::crypto::retail_key(), image_key, wrapped);
    Bytes encrypted(padded.size());
    xbox::crypto::aes128_cbc_encrypt(image_key, padded, encrypted);
    auto enc_header = xexf::rebuild_header_with_file_format_info(fixture, ffi_body(1u, 2u, window));
    const auto security = (std::to_integer<std::uint32_t>(enc_header[0x10]) << 24) |
                          (std::to_integer<std::uint32_t>(enc_header[0x11]) << 16) |
                          (std::to_integer<std::uint32_t>(enc_header[0x12]) << 8) |
                          std::to_integer<std::uint32_t>(enc_header[0x13]);
    for (std::size_t i = 0; i < 16u; ++i) enc_header[security + 0x150u + i] = wrapped[i];
    auto enc_file = join(enc_header, encrypted);
    seeds.files.push_back({enc_file, header_regions(fixture, enc_file.size())});
  }
  return seeds;
}

Outcome run_xex(std::span<const std::byte> input) {
  xbox::XexImage image{};
  std::string error;
  const bool ok = xbox::parse_xex_image(input, image, &error);
  if (!ok) assert(!error.empty());
  fs::XexMetadata metadata{};
  (void)fs::parse_xex_metadata(input, metadata);
  return ok_if(ok);
}

// ---------------------------------------------------------------------------
// LZX
// ---------------------------------------------------------------------------
struct LzxSeed {
  std::uint32_t window_bits;
  std::size_t output_size;
};

Bytes sample_data(std::size_t size, std::uint32_t salt) {
  Bytes out(size);
  std::uint32_t x = 0x12345678u ^ salt;
  for (std::size_t i = 0; i < size; ++i) {
    x = x * 1103515245u + 12345u;
    // Text-like: a small alphabet with runs, so the Huffman coder has work.
    out[i] = static_cast<std::byte>(((x >> 16) % 7u == 0u) ? 0 : 'a' + (x >> 20) % 12u);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Filesystem sources (STFS, GDFX) and extraction.
// ---------------------------------------------------------------------------
void walk_source(const fs::ReadOnlyContentSource& source, const std::string& path, std::size_t depth,
                 std::size_t& budget) {
  if (depth > 64 || budget == 0) return;
  std::vector<fs::DirectoryEntry> entries;
  if (source.list(path, entries) != fs::FsError::None) return;
  for (const auto& entry : entries) {
    if (budget == 0) return;
    --budget;
    const auto child = path.empty() ? entry.name : path + "\\" + entry.name;
    fs::FileInfo info{};
    if (source.stat(child, info) != fs::FsError::None) continue;
    if (info.is_directory) {
      walk_source(source, child, depth + 1, budget);
      continue;
    }
    std::array<std::byte, 4096> buffer{};
    for (std::uint64_t offset = 0; offset < std::min<std::uint64_t>(info.size, 1u << 20);
         offset += buffer.size()) {
      std::size_t read = 0;
      if (source.read_at(child, offset, buffer, read) != fs::FsError::None || read == 0) break;
      assert(read <= buffer.size());
    }
    // Reads past the end and at absurd offsets must fail or return nothing.
    std::size_t read = 0;
    if (source.read_at(child, ~std::uint64_t{0} - 8u, buffer, read) == fs::FsError::None) assert(read == 0);
  }
}

// Extraction writes only inside the destination.
void check_extraction(const fs::ReadOnlyContentSource& source) {
  const auto destination = g_temp / "extract";
  std::error_code ec;
  std::filesystem::remove_all(destination, ec);
  std::filesystem::remove_all(g_temp / "escape", ec);
  std::filesystem::create_directories(destination);
  (void)fs::materialize_content_source(source, destination);
  const auto root = std::filesystem::weakly_canonical(destination);
  for (auto it = std::filesystem::recursive_directory_iterator(destination, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    const auto canonical = std::filesystem::weakly_canonical(it->path());
    const auto relative = canonical.lexically_relative(root);
    assert(!relative.empty() && *relative.begin() != "..");
    assert(!it->is_symlink());
  }
  // Nothing appeared next to the destination.
  for (const auto& entry : std::filesystem::directory_iterator(g_temp)) {
    const auto name = entry.path().filename().string();
    assert(name == "extract" || name == "input.bin" || name == "capture.bin" || name == "launch.json");
  }
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
std::array<std::uint32_t, 3> pack_cf(std::uint32_t a0, std::uint16_t a1, std::uint32_t b0 = 0,
                                     std::uint16_t b1 = 0) {
  return {a0, std::uint32_t(a1) | ((b0 & 0xFFFFu) << 16), (b0 >> 16) | (std::uint32_t(b1) << 16)};
}

Bytes dwords_to_bytes(std::span<const std::uint32_t> words) {
  Bytes out(words.size() * 4u);
  std::memcpy(out.data(), words.data(), out.size());
  return out;
}

std::vector<std::uint32_t> bytes_to_dwords(std::span<const std::byte> bytes) {
  std::vector<std::uint32_t> words(bytes.size() / 4u);
  std::memcpy(words.data(), bytes.data(), words.size() * 4u);
  return words;
}

std::vector<Seed> make_shader_seeds() {
  using namespace xenon::gpu;
  std::vector<Seed> seeds;
  // Vertex fetch + ALU (shader_decoder tests' mixed exec program).
  {
    const auto cf = pack_cf(1u | (2u << 12) | (1u << 16),
                            std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));
    const std::uint32_t vf0 = (3u << 5) | (5u << 12) | (7u << 20) | (1u << 25);
    const std::uint32_t vf1 = 0x688u | (12u << 16);
    const std::uint32_t vf2 = 4u | (8u << 8);
    const std::uint32_t alu0 = 2u | (1u << 15) | (0xFu << 16) | (2u << 26);
    const std::uint32_t alu2 = 9u | (4u << 8) | (1u << 16) | (3u << 24);
    const std::vector<std::uint32_t> words = {cf[0], cf[1], cf[2], vf0, vf1, vf2, alu0, 0, alu2};
    seeds.push_back({dwords_to_bytes(words)});
  }
  // Two exec blocks, texture fetch and exports.
  {
    const auto cf0 = pack_cf(2u | (1u << 12), std::uint16_t(std::uint16_t(ControlFlowOpcode::Exec) << 12),
                             4u | (1u << 12), std::uint16_t(std::uint16_t(ControlFlowOpcode::ExecEnd) << 12));
    const std::uint32_t tf0 = 1u | (1u << 5) | (2u << 12) | (0xFu << 20);
    const std::uint32_t tf1 = 0x688u;
    const std::uint32_t tf2 = 0u;
    const std::uint32_t alu0 = 0u | (0xFu << 16) | (1u << 31);
    const std::uint32_t alu1 = 0u;
    const std::uint32_t alu2 = (1u << 24);
    const std::vector<std::uint32_t> words = {cf0[0], cf0[1], cf0[2], 0u, 0u, 0u, tf0, tf1, tf2,
                                              alu0,   alu1,   alu2,   alu0, alu1, alu2};
    seeds.push_back({dwords_to_bytes(words)});
  }
  return seeds;
}

// ---------------------------------------------------------------------------
// PM4
// ---------------------------------------------------------------------------
constexpr std::uint32_t kRingBase = 0x01000000u;
constexpr std::uint32_t kAuxBase = 0x01010000u;

void store_be_words(xenon::memory::AddressSpace& memory, std::uint32_t address,
                    std::span<const std::uint32_t> words) {
  Bytes bytes(words.size() * 4u);
  for (std::size_t i = 0; i < words.size(); ++i) {
    bytes[i * 4 + 0] = static_cast<std::byte>(words[i] >> 24);
    bytes[i * 4 + 1] = static_cast<std::byte>(words[i] >> 16);
    bytes[i * 4 + 2] = static_cast<std::byte>(words[i] >> 8);
    bytes[i * 4 + 3] = static_cast<std::byte>(words[i]);
  }
  if (!bytes.empty()) assert(memory.write_physical(address, bytes));
}

std::vector<Seed> make_pm4_seeds(xenon::memory::AddressSpace& memory) {
  using gpu::make_packet_type0;
  using gpu::make_packet_type1;
  using gpu::make_packet_type2;
  using gpu::make_packet_type3;
  using gpu::Type3Opcode;
  // An indirect buffer target: register writes and a NOP.
  const std::uint32_t aux[] = {make_packet_type0(0x51, 2), 0x11111111u, 0x22222222u, make_packet_type2()};
  store_be_words(memory, kAuxBase, aux);
  const std::uint32_t shader_code[3] = {0x01020304u, 0x11121314u, 0x21222324u};
  const std::uint32_t draw = static_cast<std::uint32_t>(gpu::PrimitiveType::TriangleList) |
                             (static_cast<std::uint32_t>(gpu::DrawSource::AutoIndex) << 6) | (3u << 16);
  std::vector<Seed> seeds;
  const auto add = [&](std::initializer_list<std::uint32_t> words) {
    seeds.push_back({dwords_to_bytes(std::vector<std::uint32_t>(words))});
  };
  add({make_packet_type0(0x50, 1), 0xCAFED00Du, make_packet_type1(0x60, 0x61), 1u, 2u, make_packet_type2(),
       make_packet_type3(Type3Opcode::Nop, 1), 0u});
  add({make_packet_type3(Type3Opcode::ImLoadImmediate, 5), 0u, 3u, shader_code[0], shader_code[1],
       shader_code[2], make_packet_type3(Type3Opcode::DrawIndx2, 1), draw});
  add({make_packet_type3(Type3Opcode::MemWrite, 2), kAuxBase + 0x100u, 0xA1B2C3D4u,
       make_packet_type3(Type3Opcode::IndirectBuffer, 2), kAuxBase, 4u});
  add({make_packet_type3(Type3Opcode::WaitRegMem, 5), 0x13u, kAuxBase + 0x100u, 0xA1B2C3D4u, 0xFFFFFFFFu, 1u,
       make_packet_type3(Type3Opcode::RegRmw, 3), 0x50u, 0xFFu, 0x100u});
  add({make_packet_type3(Type3Opcode::SetConstant, 2), 0x00000004u, 0x3F800000u,
       make_packet_type3(Type3Opcode::EventWrite, 1), 0x16u,
       make_packet_type3(Type3Opcode::LoadAluConstant, 3), kAuxBase, 0u, 4u});
  return seeds;
}

// ---------------------------------------------------------------------------
// Portable GPU captures
// ---------------------------------------------------------------------------
Seed make_capture_seed(xenon::memory::AddressSpace& memory) {
  using gpu::make_packet_type0;
  using gpu::make_packet_type3;
  using gpu::Type3Opcode;
  gpu::GraphicsSystem graphics(memory);
  gpu::NullBackend backend;
  constexpr std::uint32_t shader_cmd = 0x0100C000;
  constexpr std::uint32_t capture_cmd = 0x0100C100;
  constexpr std::uint32_t side_effect = 0x01420000;
  constexpr std::uint32_t vertex_data = 0x01421000;
  constexpr std::uint32_t fetch_register = gpu::ResourceStateTracker::kFetchConstantBase + 3u * 6u + 2u * 2u;
  const std::uint32_t shader_words[] = {make_packet_type3(Type3Opcode::ImLoadImmediate, 5), 0u, 3u,
                                        0x01020304u, 0x11121314u, 0x21222324u};
  store_be_words(memory, shader_cmd, shader_words);
  graphics.submit_buffer(shader_cmd, 6u);
  const std::uint32_t draw = static_cast<std::uint32_t>(gpu::PrimitiveType::TriangleList) |
                             (static_cast<std::uint32_t>(gpu::DrawSource::AutoIndex) << 6) | (3u << 16);
  std::array<std::byte, 256> vertex_bytes{};
  for (std::size_t i = 0; i < vertex_bytes.size(); ++i) vertex_bytes[i] = static_cast<std::byte>(i);
  assert(memory.write_physical(vertex_data, vertex_bytes));
  const std::uint32_t words[] = {make_packet_type3(Type3Opcode::MemWrite, 2), side_effect, 0xA1B2C3D4u,
                                 make_packet_type0(fetch_register, 2), 3u | vertex_data, 64u << 2u,
                                 make_packet_type3(Type3Opcode::DrawIndx2, 1), draw};
  store_be_words(memory, capture_cmd, words);
  gpu::PortableSubmissionCapture capture;
  std::string error;
  assert(graphics.capture_portable_buffer(backend, capture_cmd, 8u, capture, &error));
  const auto path = g_temp / "capture.bin";
  assert(gpu::save_portable_capture(capture, path, &error));
  std::ifstream in(path, std::ios::binary);
  Seed seed;
  seed.bytes.resize(static_cast<std::size_t>(std::filesystem::file_size(path)));
  in.read(reinterpret_cast<char*>(seed.bytes.data()), static_cast<std::streamsize>(seed.bytes.size()));
  // Mutate the structure, not the 10 MiB EDRAM image: everything before it
  // and the tail after it.
  std::size_t edram_at = 0;
  for (std::size_t tail = 0; tail < 256 && !edram_at; ++tail) {
    if (seed.bytes.size() < gpu::Edram::kSize + 8 + tail) break;
    const auto at = seed.bytes.size() - tail - gpu::Edram::kSize - 8;
    std::uint64_t size = 0;
    for (int b = 0; b < 8; ++b) size |= std::uint64_t(std::to_integer<std::uint8_t>(seed.bytes[at + b])) << (8 * b);
    if (size == gpu::Edram::kSize) edram_at = at;
  }
  assert(edram_at);
  seed.regions = {{0, edram_at + 8}, {edram_at + 8 + gpu::Edram::kSize, seed.bytes.size()}};
  return seed;
}

// ---------------------------------------------------------------------------
// JSON documents
// ---------------------------------------------------------------------------
constexpr std::string_view kLaunchConfig = R"json({
  "configVersion": 1, "sessionId": "session-123", "sessionDir": "C:/sessions/session-123",
  "gameId": "halo3", "title": "Halo 3", "contentPath": "C:/Games/Halo3",
  "moduleId": "org.example.module", "moduleName": "Module", "modulePath": "C:/Xenon/modules/m",
  "moduleVersion": "1.2.0", "moduleSettings": {"difficulty": "ace", "hud": true},
  "runtimeApiRequirements": {"input": {"version": 1, "required": true}},
  "renderer": "Vulkan", "shaderCache": false, "inputDeadzone": 0.15,
  "inputUserSources": [{"userIndex": 0, "sources": ["xinput:controller-0"]}],
  "audioMasterVolume": 0.75, "dlc": [{"type": "DLC", "path": "C:/Games/DLC/map1"}],
  "savePath": "C:/Xenon/saves", "offline": false
})json";

constexpr std::string_view kInputProfile = R"({
  "schema": "xenon.input.v1",
  "keyboard": {
    "initialContext": "menu",
    "contexts": [
      { "name": "menu", "keys": [["W", "DpadUp"], ["Space", "ButtonA"]] },
      { "name": "flight",
        "keys": [["W", "RightTrigger"], ["A", "LeftStickLeft"], ["Q", "LeftShoulder", 0.5]],
        "mouseButtons": [["Left", "ButtonA"]],
        "mouse": { "stick": "left", "sensitivityX": 2, "sensitivityY": 2, "invertY": true,
                   "smoothing": 0, "antiDeadzone": 0.15, "alternateStickKey": "LeftAlt" } }
    ]
  }
})";

constexpr std::string_view kRenderingProfile = R"({
  "schema": "xenon.rendering.v1",
  "temporal": {
    "camera": { "vertexConstant": 4, "layout": "columns", "positionConstant": 9 },
    "scene": { "colorFormats": ["R16G16B16A16Float", "R10G10B10A2"] },
    "jitter": { "lateSceneDraws": true },
    "hud": { "vertexShaders": ["0x00000000000000AB"], "pixelShaders": ["ffffffffffffffff"] },
    "cameraCut": { "degrees": 30, "distance": 250.5 }
  }
})";

constexpr std::string_view kNested = R"({"a":[1,2.5e-3,-0,true,false,null,"\u00e9\ud83d\ude00\n"],"b":{"c":{"d":[[[]]]}}})";

Outcome run_json(std::span<const std::byte> input) {
  xenon::core::JsonValue value;
  std::string error;
  if (!xenon::core::JsonValue::parse(as_text(input), value, &error)) return Outcome::Rejected;
  // What the writer produces parses back to the same document.
  const auto dumped = value.dump();
  xenon::core::JsonValue again;
  assert(xenon::core::JsonValue::parse(dumped, again, &error));
  assert(again.dump() == dumped);
  return Outcome::Accepted;
}

}  // namespace

int main() {
  g_iterations = env_size("XENON_FUZZ_ITERATIONS", 1500);
  g_seed_base = env_size("XENON_FUZZ_SEED", g_seed_base);
  if (const char* replay = std::getenv("XENON_FUZZ_REPLAY"); replay && *replay) {
    const char* target = std::getenv("XENON_FUZZ_TARGET");
    g_replay_target = target ? target : "";
    std::ifstream in(replay, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    g_replay_input = text_bytes(text);
  }
  g_temp = std::filesystem::temp_directory_path() /
           ("xenon_input_fuzz_" + std::to_string(static_cast<unsigned long long>(
                                      std::chrono::steady_clock::now().time_since_epoch().count())));
  std::filesystem::create_directories(g_temp);
  const auto iterations = g_iterations;
  // File-backed and replaying targets are slower per input.
  const auto file_iterations = std::max<std::size_t>(1, iterations / 3);
  const auto capture_iterations = std::max<std::size_t>(1, iterations / 15);

  // --- XEX images and metadata -------------------------------------------
  const auto xex = make_xex_seeds();
  fuzz_target("xex_image", xex.files, iterations, run_xex);
  // The LZX body behind a valid block hash, so mutations reach decompression
  // and PE parsing instead of stopping at the hash check.
  fuzz_target("xex_lzx_body", {Seed{xex.normal_lzx}}, iterations, [&](std::span<const std::byte> lzx) {
    const auto chain = xexf::build_compressed_block_chain(lzx);
    return run_xex(join(xex.normal_header, chain));
  });

  // --- LZX ------------------------------------------------------------------
  {
    std::vector<Seed> seeds;
    std::vector<LzxSeed> params;
    for (const auto window_bits : {15u, 17u, 21u}) {
      for (const auto size : {std::size_t{1}, std::size_t{4000}, std::size_t{0x8000}, std::size_t{70000}}) {
        const auto data = sample_data(size, window_bits);
        seeds.push_back({xbox::lzx::encode_uncompressed(data, window_bits)});
        params.push_back({window_bits, size});
        if (size <= 0x8000) {
          seeds.push_back({xbox::lzx::encode_literal_huffman_block(data, window_bits, size % 2 == 0, true)});
          params.push_back({window_bits, size});
        }
      }
    }
    // Each seed carries its parameters in a 5-byte prefix (window bits and
    // output size) so mutations can reach them too.
    for (std::size_t i = 0; i < seeds.size(); ++i) {
      Bytes prefixed(5);
      prefixed[0] = static_cast<std::byte>(params[i].window_bits);
      for (int b = 0; b < 4; ++b) prefixed[1 + b] = static_cast<std::byte>(params[i].output_size >> (8 * b));
      prefixed.insert(prefixed.end(), seeds[i].bytes.begin(), seeds[i].bytes.end());
      seeds[i].bytes = std::move(prefixed);
      seeds[i].regions = {{0, 5}, {5, seeds[i].bytes.size()}};
    }
    fuzz_target("lzx_decode", seeds, iterations, [](std::span<const std::byte> input) {
      if (input.size() < 5) return Outcome::Rejected;
      const auto window_bits = std::to_integer<std::uint32_t>(input[0]);
      std::uint32_t size = 0;
      for (int b = 0; b < 4; ++b) size |= std::to_integer<std::uint32_t>(input[1 + b]) << (8 * b);
      size = std::min<std::uint32_t>(size, 1u << 20);  // the caller bounds its output buffer
      Bytes output(size);
      const auto result = xbox::lzx::decode(input.subspan(5), window_bits, {}, output);
      if (!result.ok) assert(!result.error.empty());
      return ok_if(result.ok);
    });

    std::vector<Seed> delta_seeds;
    {
      // Copy-from-old record, then a literal LZX record, over a 64 KiB image.
      auto records = xexf::build_delta_patch_record(0x100u, 0x2000u, 0x80u, 0u);
      const auto literal = sample_data(0x40, 3);
      const auto encoded = xbox::lzx::encode_uncompressed(literal, 15u);
      const auto second = xexf::build_delta_patch_record(0u, 0x3000u, 0x40u,
                                                         static_cast<std::uint16_t>(encoded.size()), encoded);
      records.insert(records.end(), second.begin(), second.end());
      delta_seeds.push_back({records});
      delta_seeds.push_back({xexf::build_delta_patch_record(0x10u, 0x20u, 0x40u, 0u)});
    }
    fuzz_target("lzx_delta_patch", delta_seeds, iterations, [](std::span<const std::byte> records) {
      Bytes image = sample_data(0x10000, 9);
      const auto result = xbox::lzx::apply_delta_patch_records(records, 15u, image);
      return ok_if(result.ok);
    });
  }

  // --- STFS -----------------------------------------------------------------
  {
    const auto package = xenon::test::make_test_stfs(0x41415858u, 0x12345678u);
    const std::vector<Seed> seeds{{package, {{0, 0x400}, {0x22C, 0x400}, {0xA000, 0xB000}, {0xB000, package.size()}}}};
    fuzz_target("stfs_metadata", seeds, iterations, [](std::span<const std::byte> input) {
      fs::StfsPackageMetadata metadata{};
      return ok_if(fs::parse_stfs_metadata(input, metadata) == fs::FsError::None);
    });
    fuzz_target("stfs_package", seeds, file_iterations, [](std::span<const std::byte> input) {
      const auto path = g_temp / "input.bin";
      write_file(path, input);
      fs::StfsPackageSource source(path);
      if (source.initialize() != fs::FsError::None) return Outcome::Rejected;
      std::size_t budget = 4096;
      walk_source(source, "", 0, budget);
      check_extraction(source);
      return Outcome::Accepted;
    });
  }

  // --- GDFX / XDVDFS ----------------------------------------------------------
  {
    constexpr std::size_t kSector = 2048;
    std::vector<Seed> seeds;
    for (const std::uint64_t game_offset : {std::uint64_t{0}, std::uint64_t{0xFB20}}) {
      const auto image = xenon::test::gdfx::make_test_gdfx(0x41415858u, 0x12345678u, game_offset);
      const auto base = static_cast<std::size_t>(game_offset);
      seeds.push_back({image,
                       {{base + 32 * kSector, base + 32 * kSector + 32},
                        {base + 40 * kSector, base + 40 * kSector + 64},
                        {base + 41 * kSector, base + 41 * kSector + 32},
                        {base + 50 * kSector, base + 50 * kSector + 0x80}}});
    }
    fuzz_target("gdfx_image", seeds, file_iterations, [](std::span<const std::byte> input) {
      const auto path = g_temp / "input.bin";
      write_file(path, input);
      fs::GdfxImageSource source(path);
      if (source.initialize() != fs::FsError::None) return Outcome::Rejected;
      std::size_t budget = 4096;
      walk_source(source, "", 0, budget);
      check_extraction(source);
      return Outcome::Accepted;
    });
  }

  // --- Shader microcode -------------------------------------------------------
  fuzz_target("shader_microcode", make_shader_seeds(), iterations, [](std::span<const std::byte> input) {
    const auto words = bytes_to_dwords(input);
    bool any_complete = false;
    for (const auto stage : {gpu::ShaderStage::Vertex, gpu::ShaderStage::Pixel}) {
      const gpu::ShaderProgram program(stage, words, static_cast<std::uint32_t>(words.size() % 7u));
      const auto decoded = gpu::ShaderDecoder::decode(program);
      if (!decoded.complete) continue;
      any_complete = true;
      (void)gpu::HlslShaderLowerer::lower(decoded);
    }
    return ok_if(any_complete);
  }, false, true);

  // --- PM4 command streams ----------------------------------------------------
  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  fuzz_target("pm4_ring", make_pm4_seeds(memory), iterations, [&](std::span<const std::byte> input) {
    auto words = bytes_to_dwords(input);
    if (words.empty()) return Outcome::Rejected;
    if (words.size() > 4096) words.resize(4096);
    store_be_words(memory, kRingBase, words);
    gpu::RegisterFile registers;
    gpu::ir::Stream stream;
    gpu::CommandProcessor processor(memory, registers, stream);
    const auto capacity = static_cast<std::uint32_t>(words.size() + 1);
    // The command processor reports corrupt streams by exception (its
    // documented contract); that rejects the input.
    const auto read = processor.execute_ring(kRingBase, capacity, 0, static_cast<std::uint32_t>(words.size()));
    assert(read <= capacity);
    return Outcome::Accepted;
  }, true, true);

  // --- Portable GPU captures (load and replay) -------------------------------
  {
    const std::vector<Seed> seeds{make_capture_seed(memory)};
    // Regression: a capture declaring an 8 GiB EDRAM blob (or any size or
    // count beyond what the file holds) is rejected before allocating it.
    {
      const auto edram_size_at = seeds[0].regions[0].second - 8;
      Bytes bomb(seeds[0].bytes.begin(), seeds[0].bytes.begin() + static_cast<std::ptrdiff_t>(edram_size_at));
      const std::uint64_t declared = std::uint64_t{1} << 33;
      for (int b = 0; b < 8; ++b) bomb.push_back(static_cast<std::byte>(declared >> (8 * b)));
      bomb.resize(bomb.size() + 64);
      const auto path = g_temp / "capture.bin";
      write_file(path, bomb);
      g_oversized_allocation.store(false);
      std::string error;
      assert(!gpu::load_portable_capture(path, &error) && !error.empty());
      assert(!g_oversized_allocation.load());
    }
    fuzz_target("gpu_capture", seeds, capture_iterations, [&](std::span<const std::byte> input) {
      const auto path = g_temp / "capture.bin";
      write_file(path, input);
      std::string error;
      const auto capture = gpu::load_portable_capture(path, &error);
      if (!capture) return Outcome::Rejected;
      gpu::GraphicsSystem graphics(memory);
      gpu::NullBackend backend;
      if (!graphics.replay_portable_capture(backend, *capture, &error)) return Outcome::Rejected;
      return Outcome::Accepted;
    });
  }

  // --- Configuration and module documents -------------------------------------
  {
    std::vector<Seed> json_seeds{{text_bytes(kLaunchConfig)}, {text_bytes(kInputProfile)},
                                 {text_bytes(kRenderingProfile)}, {text_bytes(kNested)}};
    fuzz_target("json", json_seeds, iterations, run_json);
    // Deep nesting is bounded, not a stack overflow.
    {
      std::string deep(1u << 20, '[');
      xenon::core::JsonValue value;
      std::string error;
      assert(!xenon::core::JsonValue::parse(deep, value, &error));
    }
#if XENON_FUZZ_HAS_LAUNCH_CONFIG
    fuzz_target("launch_config", {{text_bytes(kLaunchConfig)}}, file_iterations, [](std::span<const std::byte> input) {
      const auto path = g_temp / "launch.json";
      write_file(path, input);
      xenon::runtime_host::LaunchConfig config{};
      std::string error;
      return ok_if(xenon::runtime_host::LaunchConfig::load_from_file(path.string(), config, error));
    });
#endif
#if XENON_FUZZ_HAS_INPUT
    fuzz_target("module_input_profile", {{text_bytes(kInputProfile)}}, iterations, [](std::span<const std::byte> input) {
      std::string error;
      return ok_if(xenon::input::parse_module_input_profile(as_text(input), &error).has_value());
    });
#endif
    fuzz_target("rendering_profile", {{text_bytes(kRenderingProfile)}}, iterations, [](std::span<const std::byte> input) {
      std::string error;
      return ok_if(gpu::parse_temporal_render_profile(as_text(input), &error).has_value());
    });
#if XENON_FUZZ_HAS_RECOMP
    {
      const auto hints = xenon::recomp::analysis::to_json(xenon::recomp::analysis::AnalysisHintSetV2{}).dump(2);
      fuzz_target("analysis_hints", {{text_bytes(hints)}}, iterations, [](std::span<const std::byte> input) {
        xenon::core::JsonValue value;
        std::string error;
        if (!xenon::core::JsonValue::parse(as_text(input), value, &error)) return Outcome::Rejected;
        xenon::recomp::analysis::AnalysisHintSetV2 set;
        return ok_if(xenon::recomp::analysis::from_json(value, set, error));
      });
    }
#endif
  }

  std::error_code ec;
  std::filesystem::remove_all(g_temp, ec);
  if (g_failed) {
    std::cerr << "input fuzzing found failures\n";
    return 1;
  }
  std::cout << "input fuzzing: no failures\n";
  return 0;
}
