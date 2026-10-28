#include "state_file.hpp"
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
  if (!fs::is_regular_file(path) || fs::file_size(path) > limit)
    throw std::runtime_error("Invalid or oversized JSON file");
  std::ifstream input(path);
  Json result;
  input >> result;
  input >> std::ws;
  if (!input.eof())
    throw std::runtime_error("Unexpected trailing data in JSON file");
  return result;
}
void write_document(const fs::path &path, const Json &value) {
  fs::create_directories(path.parent_path());
  auto temporary = path;
  temporary += "." + make_id() + ".tmp";
  try {
    {
      std::ofstream output(temporary, std::ios::binary);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output << value.dump(2) << '\n';
      output.flush();
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Could not commit launcher state");
#else
    fs::rename(temporary, path);
#endif
  } catch (...) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    throw;
  }
}
} // namespace xenon::launcher_next
