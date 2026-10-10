// Investigation probe logs (xenon/logging/probe_log.hpp): off unless enabled,
// truncated by the first record of a run, and capped per file per run.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xenon/logging/probe_log.hpp"

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void test_disabled_probes_write_nothing(const std::filesystem::path& file) {
  xenon::logging::set_probe_logs_enabled(false);
  xenon::logging::append_probe_log(file.string().c_str(), "never %d\n", 1);
  assert(!std::filesystem::exists(file));
}

void test_first_record_of_a_run_replaces_an_earlier_file(const std::filesystem::path& file) {
  { std::ofstream(file) << "left over from an earlier run\n"; }
  xenon::logging::set_probe_logs_enabled(true);
  xenon::logging::append_probe_log(file.string().c_str(), "first %s\n", "record");
  xenon::logging::append_probe_log(file.string().c_str(), "second %d\n", 2);
  assert(read_file(file) == "first record\nsecond 2\n");
}

void test_each_file_is_capped(const std::filesystem::path& file) {
  xenon::logging::set_probe_logs_enabled(true);
  const std::string line(1000, 'x');
  const auto records = xenon::logging::kProbeLogByteLimit / (line.size() + 1) + 100;
  for (std::uint64_t i = 0; i < records; ++i)
    xenon::logging::append_probe_log(file.string().c_str(), "%s\n", line.c_str());
  const auto size = std::filesystem::file_size(file);
  assert(size <= xenon::logging::kProbeLogByteLimit + 64);
  const auto text = read_file(file);
  assert(text.ends_with("[probe log truncated: per-run limit reached]\n"));
}

}  // namespace

int main() {
  std::cout << "Testing probe logs...\n";
  const auto directory = std::filesystem::temp_directory_path() / "xenon_probe_log_tests";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  test_disabled_probes_write_nothing(directory / "disabled_diag.log");
  test_first_record_of_a_run_replaces_an_earlier_file(directory / "fresh_diag.log");
  test_each_file_is_capped(directory / "capped_diag.log");
  xenon::logging::set_probe_logs_enabled(false);
  std::filesystem::remove_all(directory);
  std::cout << "All probe log tests passed!\n";
  return 0;
}
