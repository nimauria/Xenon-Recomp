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

// The runtime host sends a run's probes to its run folder: a bare file name
// then lands there (created if missing), and a new run starts every file
// afresh.
void test_probes_follow_the_directory(const std::filesystem::path& directory) {
  xenon::logging::set_probe_logs_enabled(true);
  const auto run = directory / "errors" / "2026-10-30_21-00-00" / "probes";
  xenon::logging::set_probe_log_directory(run.string());
  assert(xenon::logging::probe_log_directory() == run.string());
  xenon::logging::append_probe_log("routed_diag.log", "in the run %d\n", 1);
  assert(read_file(run / "routed_diag.log") == "in the run 1\n");
  assert(!std::filesystem::exists("routed_diag.log"));

  xenon::logging::set_probe_log_directory(run.string());  // unchanged: keeps appending
  xenon::logging::append_probe_log("routed_diag.log", "still %d\n", 2);
  assert(read_file(run / "routed_diag.log") == "in the run 1\nstill 2\n");

  const auto next = directory / "errors" / "2026-10-30_21-05-00" / "probes";
  xenon::logging::set_probe_log_directory(next.string());
  xenon::logging::append_probe_log("routed_diag.log", "next run\n");
  assert(read_file(next / "routed_diag.log") == "next run\n");
  assert(read_file(run / "routed_diag.log") == "in the run 1\nstill 2\n");

  xenon::logging::set_probe_log_directory("");
  assert(xenon::logging::probe_log_directory().empty());
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
  test_probes_follow_the_directory(directory);
  xenon::logging::set_probe_logs_enabled(false);
  std::filesystem::remove_all(directory);
  std::cout << "All probe log tests passed!\n";
  return 0;
}
