#include "state_file.hpp"
#include "platform/file_path.hpp"
#include <fstream>
#include <random>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace xenon::launcher_next {
namespace fs = std::filesystem;
std::string make_id() {
  std::random_device random;
  std::ostringstream out;
  out << std::hex;
  for (int i = 0; i < 4; ++i)
    out << random() << '-';
  return out.str();
}
Json read_document(const fs::path &path, std::uintmax_t limit) {
  if (!fs::is_regular_file(io_path(path)) || fs::file_size(io_path(path)) > limit)
    throw std::runtime_error("Invalid or oversized JSON file");
  std::ifstream input(io_path(path));
  Json result;
  input >> result;
  input >> std::ws;
  if (!input.eof())
    throw std::runtime_error("Unexpected trailing data in JSON file");
  return result;
}
void write_document(const fs::path &path, const Json &value) {
  fs::create_directories(io_path(path.parent_path()));
  auto temporary = path;
  temporary += "." + make_id() + ".tmp";
  try {
    {
      std::ofstream output(io_path(temporary), std::ios::binary);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output << value.dump(2) << '\n';
      output.flush();
    }
#ifdef _WIN32
    if (!MoveFileExW(io_path(temporary).c_str(), io_path(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Could not commit launcher state");
#else
    fs::rename(io_path(temporary), io_path(path));
#endif
  } catch (...) {
    std::error_code ignored;
    fs::remove(io_path(temporary), ignored);
    throw;
  }
}
} // namespace xenon::launcher_next
