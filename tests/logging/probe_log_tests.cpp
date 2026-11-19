// Investigation probe logs (xenon/logging/probe_log.hpp): off unless enabled,
// truncated by the first record of a run, and capped per file per run.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

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

// Regression: a probe file stays open for the run, so a record costs a write,
// not a file open and close under the lock every probe writer shares (AC6's
// main thread waited 63 ms in an NtResumeThread that logged a line, and lost
// a startup race to the thread it resumed). Every record still reaches the
// file as it is written, readable while the file is open, and a new run
// directory releases the old files.
void test_open_probe_files_keep_every_record_and_release_on_a_new_run(
    const std::filesystem::path& directory) {
  xenon::logging::set_probe_logs_enabled(true);
  const auto run = directory / "kept_open" / "probes";
  xenon::logging::set_probe_log_directory(run.string());
  std::string expected;
  for (int i = 0; i < 20000; ++i) {
    xenon::logging::append_probe_log("kept_diag.log", "record %d\n", i);
    expected += "record " + std::to_string(i) + "\n";
    if (i == 0 || i == 9999) assert(read_file(run / "kept_diag.log") == expected);
  }
  assert(read_file(run / "kept_diag.log") == expected);
  // The next run's directory closes this run's files: they can be removed.
  xenon::logging::set_probe_log_directory((directory / "after_kept_open").string());
  std::error_code error;
  assert(std::filesystem::remove(run / "kept_diag.log", error) && !error);
  xenon::logging::set_probe_log_directory("");
}

// Regression (xenon_write_trap_tests): turning probe logging off releases the
// open files, so their directory can be removed, and a record after it is
// turned back on appends instead of truncating what the run already wrote.
void test_disabling_probes_releases_files_and_keeps_records(
    const std::filesystem::path& directory) {
  const auto run = directory / "toggled";
  xenon::logging::set_probe_log_directory(run.string());
  xenon::logging::set_probe_logs_enabled(true);
  xenon::logging::append_probe_log("toggled_diag.log", "before\n");
  xenon::logging::set_probe_logs_enabled(false);
  std::error_code error;
  const auto copy = directory / "toggled_copy.log";
  assert(std::filesystem::copy_file(run / "toggled_diag.log", copy, error) && !error);
  assert(std::filesystem::remove(run / "toggled_diag.log", error) && !error);
  assert(std::filesystem::copy_file(copy, run / "toggled_diag.log", error) && !error);
  xenon::logging::set_probe_logs_enabled(true);
  xenon::logging::append_probe_log("toggled_diag.log", "after\n");
  assert(read_file(run / "toggled_diag.log") == "before\nafter\n");
  xenon::logging::set_probe_logs_enabled(false);
  std::filesystem::remove_all(run, error);
  assert(!error && !std::filesystem::exists(run));
  xenon::logging::set_probe_log_directory("");
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
  test_open_probe_files_keep_every_record_and_release_on_a_new_run(directory);
  test_disabling_probes_releases_files_and_keeps_records(directory);
  xenon::logging::set_probe_logs_enabled(false);
  std::filesystem::remove_all(directory);
  std::cout << "All probe log tests passed!\n";
  return 0;
}
