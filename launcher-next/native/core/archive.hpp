#pragma once
#include <filesystem>
#include <map>
#include <functional>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace xenon::launcher_next {
struct OperationCancelled : std::runtime_error {
  OperationCancelled() : std::runtime_error("Operation cancelled") {}
};
struct ExtractionControl {
  std::function<bool()> cancelled;
  std::function<void(std::uint64_t, std::uint64_t)> progress;
};
// Extraction is restricted to a fresh staging directory owned by the caller.
void extract_package(const std::filesystem::path &archive, const std::filesystem::path &destination,
                     const ExtractionControl &control = {});
void write_zip(const std::filesystem::path &destination,
               const std::map<std::string, std::string> &files);
} // namespace xenon::launcher_next
