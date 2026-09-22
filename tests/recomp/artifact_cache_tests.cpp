#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "xenon/recomp/artifact_cache.hpp"

namespace {

std::string quote(const std::filesystem::path& p) { return "\"" + p.string() + "\""; }

// Builds a minimal shared library exporting `Xenon_BindCompiledRegistry` -
// enough for ArtifactStagingBuild's real load-and-validate check without
// paying for a full recomp-driver generated project + nested Xenon-Recomp
// build (that end-to-end path is already covered by
// tests/recomp/recomp_driver_tests.cpp and the preparation worker's own
// tests).
std::filesystem::path build_fake_module(const std::filesystem::path& root, const char* marker) {
  const auto project_dir = root;
  std::filesystem::create_directories(project_dir);
  {
    std::ofstream src(project_dir / "export.cpp");
    src << "#if defined(_WIN32)\n"
           "#define XENON_EXPORT extern \"C\" __declspec(dllexport)\n"
           "#else\n"
           "#define XENON_EXPORT extern \"C\" __attribute__((visibility(\"default\")))\n"
           "#endif\n"
           "XENON_EXPORT int Xenon_FakeModuleMarker = "
        << marker
        << ";\n"
           "XENON_EXPORT void Xenon_BindCompiledRegistry() {}\n";
  }
  {
    std::ofstream cmake(project_dir / "CMakeLists.txt");
    cmake << "cmake_minimum_required(VERSION 3.20)\n"
             "project(fakemod LANGUAGES CXX)\n"
             "add_library(fakemod SHARED export.cpp)\n";
  }
  const auto build_dir = project_dir / "build";
  const auto configure = "cmake -S " + quote(project_dir) + " -B " + quote(build_dir);
  assert(std::system(configure.c_str()) == 0);
  const auto compile =
      "cmake --build " + quote(build_dir) + " --target fakemod --config Release";
  assert(std::system(compile.c_str()) == 0);

  for (const auto& candidate :
       {build_dir / "fakemod.dll", build_dir / "Release" / "fakemod.dll",
        build_dir / "Debug" / "fakemod.dll", build_dir / "libfakemod.so",
        build_dir / "fakemod.so", build_dir / "libfakemod.dylib"}) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  assert(false && "fake module build did not produce a locatable shared library");
  return {};
}

xenon::recomp::ArtifactCacheKey make_key(const char* image_hash) {
  xenon::recomp::ArtifactCacheKey key;
  key.title_id = 0x41435036;
  key.media_id = 1;
  key.effective_image_hash = image_hash;
  key.module_id = "gracemeria";
  key.module_compatibility_version = "1";
  key.hint_set_hash = 0xC0FFEEu;
  key.target_arch = "x86_64";
  key.build_config = "Release";
  return key;
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / "xenon_artifact_cache_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const auto cache_root = root / "cache";
  xenon::recomp::ArtifactCacheStore store(cache_root);
  const auto key = make_key("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

  // 1. Missing before anything is built.
  assert(store.lookup(key).status == xenon::recomp::ArtifactCacheStatus::Missing);

  // 2. A staged build that is never committed or discarded must still be
  //    cleaned up (RAII safety net) and never observed by lookup().
  {
    auto staging = store.begin_staging(key);
    assert(std::filesystem::exists(staging.directory()));
  }
  assert(store.lookup(key).status == xenon::recomp::ArtifactCacheStatus::Missing);

  // 3. A successful commit becomes Fresh and reports a loadable module path.
  const auto module_a = build_fake_module(root / "build_a", "1");
  {
    auto staging = store.begin_staging(key);
    std::filesystem::copy_file(module_a, staging.directory() / "module_export.dll",
                               std::filesystem::copy_options::overwrite_existing);
    xenon::recomp::ArtifactCacheEntry entry;
    std::string error;
    assert(staging.commit("module_export.dll", entry, error));
    assert(entry.status == xenon::recomp::ArtifactCacheStatus::Fresh);
    assert(std::filesystem::exists(entry.native_extension_path));
  }
  assert(store.lookup(key).status == xenon::recomp::ArtifactCacheStatus::Fresh);

  // 4. Cancelling (discard()) an in-progress replacement build must leave the
  //    previously committed entry completely untouched (Part 18/19).
  {
    auto staging = store.begin_staging(key);
    { std::ofstream partial(staging.directory() / "partial.txt"); partial << "incomplete"; }
    staging.discard();
    assert(store.lookup(key).status == xenon::recomp::ArtifactCacheStatus::Fresh);
  }

  // 5. A validated real replacement build atomically promotes over the old
  //    one - the old artifact is fully replaced, and no rollback directory
  //    is left behind after a clean success.
  const auto module_b = build_fake_module(root / "build_b", "2");
  std::filesystem::path second_native_path;
  {
    auto staging = store.begin_staging(key);
    std::filesystem::copy_file(module_b, staging.directory() / "module_export.dll",
                               std::filesystem::copy_options::overwrite_existing);
    xenon::recomp::ArtifactCacheEntry entry;
    std::string error;
    assert(staging.commit("module_export.dll", entry, error));
    second_native_path = entry.native_extension_path;
  }
  {
    const auto entry = store.lookup(key);
    assert(entry.status == xenon::recomp::ArtifactCacheStatus::Fresh);
    assert(entry.native_extension_path == second_native_path);
    std::size_t rollback_dirs = 0;
    for (const auto& item : std::filesystem::directory_iterator(cache_root / "entries")) {
      if (item.path().filename().string().find(".rollback-") != std::string::npos) ++rollback_dirs;
    }
    assert(rollback_dirs == 0);
  }

  // 6. Committing a staging directory whose file fails to load/export the
  //    required symbol must fail cleanly and must NOT disturb the existing
  //    valid entry (Part 18: never destroy the last known-good module until
  //    a replacement has been verified).
  {
    auto staging = store.begin_staging(key);
    { std::ofstream bogus(staging.directory() / "not_a_library.dll"); bogus << "not a PE/ELF image"; }
    xenon::recomp::ArtifactCacheEntry entry;
    std::string error;
    assert(!staging.commit("not_a_library.dll", entry, error));
    assert(!error.empty());
  }
  {
    const auto entry = store.lookup(key);
    assert(entry.status == xenon::recomp::ArtifactCacheStatus::Fresh);
    assert(entry.native_extension_path == second_native_path);
  }

  // 7. Any field change in the key produces a different, independent digest
  //    (Parts 15-17 invalidation semantics) rather than colliding with the
  //    original.
  {
    auto other_key = key;
    other_key.effective_image_hash = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    assert(other_key.digest() != key.digest());
    assert(store.lookup(other_key).status == xenon::recomp::ArtifactCacheStatus::Missing);

    auto hint_changed_key = key;
    hint_changed_key.hint_set_hash = key.hint_set_hash + 1;
    assert(hint_changed_key.digest() != key.digest());

    auto abi_changed_key = key;
    abi_changed_key.abi_version = key.abi_version + 1;
    assert(abi_changed_key.digest() != key.digest());

    // module_compatibility_version is the EXPLICIT, author-controlled
    // invalidation identity (Part 16) - changing it changes the digest even
    // though it is deliberately NOT the module's cosmetic display version
    // (ArtifactCacheKey has no display-version field at all: a metadata/
    // artwork-only module update has nothing in this struct to bump, so it
    // structurally cannot invalidate the cache).
    auto compat_changed_key = key;
    compat_changed_key.module_compatibility_version = "2";
    assert(compat_changed_key.digest() != key.digest());
  }

  // 8. clean_stale_staging() removes abandoned staging/rollback directories
  //    without disturbing the current valid entry.
  {
    auto staging = store.begin_staging(key);
    const auto leaked_dir = staging.directory();
    staging.discard();
    std::filesystem::create_directories(leaked_dir);
    { std::ofstream leftover(leaked_dir / "leftover.txt"); leftover << "leftover"; }
    assert(std::filesystem::exists(leaked_dir));

    store.clean_stale_staging();
    assert(!std::filesystem::exists(leaked_dir));
    assert(store.lookup(key).status == xenon::recomp::ArtifactCacheStatus::Fresh);
  }

  std::filesystem::remove_all(root);
  return 0;
}
