#include "archive.hpp"
#include <fstream>
#include <miniz/miniz.h>
#include <set>
#include <stdexcept>
#include <vector>
#include <exception>

namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string folded(std::string value) {
  for (auto &c : value)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return value;
}
size_t read_at(void *opaque, mz_uint64 offset, void *buffer, size_t size) {
  auto &input = *static_cast<std::ifstream *>(opaque);
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset));
  input.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
  return static_cast<size_t>(input.gcount());
}
size_t write_at(void *opaque, mz_uint64 offset, const void *buffer, size_t size) {
  auto &output = *static_cast<std::ofstream *>(opaque);
  output.seekp(static_cast<std::streamoff>(offset));
  output.write(static_cast<const char *>(buffer), static_cast<std::streamsize>(size));
  return output ? size : 0;
}
struct ExtractOutput {
  std::ofstream &file;
  const ExtractionControl &control;
  std::uint64_t completed, total;
  std::exception_ptr error;
};
size_t extract_at(void *opaque, mz_uint64 offset, const void *buffer, size_t size) noexcept {
  auto &output = *static_cast<ExtractOutput *>(opaque);
  try {
    if (output.control.cancelled && output.control.cancelled())
      throw OperationCancelled();
    const auto written = write_at(&output.file, offset, buffer, size);
    if (output.control.progress)
      output.control.progress(output.completed + offset + written, output.total);
    return written;
  } catch (...) {
    output.error = std::current_exception();
    return 0;
  }
}
} // namespace
void extract_package(const fs::path &source, const fs::path &destination,
                     const ExtractionControl &control) {
  const auto check_cancel = [&] {
    if (control.cancelled && control.cancelled())
      throw OperationCancelled();
  };
  check_cancel();
  if (!fs::is_regular_file(source) || fs::file_size(source) > 512ull * 1024 * 1024)
    throw std::runtime_error("Choose a module ZIP smaller than 512 MiB");
  if (fs::exists(destination) && !fs::is_empty(destination))
    throw std::runtime_error("Package staging folder must be empty");
  fs::create_directories(destination);
  std::ifstream input(source, std::ios::binary);
  mz_zip_archive zip{};
  zip.m_pRead = read_at;
  zip.m_pIO_opaque = &input;
  if (!mz_zip_reader_init(&zip, fs::file_size(source), 0))
    throw std::runtime_error("Invalid ZIP package");
  try {
    const auto count = mz_zip_reader_get_num_files(&zip);
    if (count > 20000)
      throw std::runtime_error("Package contains too many entries");
    std::set<std::string> names;
    mz_uint64 total = 0;
    // Validate the entire directory before writing any package member.
    for (mz_uint i = 0; i < count; ++i) {
      check_cancel();
      mz_zip_archive_file_stat stat{};
      if (!mz_zip_reader_file_stat(&zip, i, &stat))
        throw std::runtime_error("Unreadable ZIP directory");
      const auto length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
      if (length < 2 || length > 1024)
        throw std::runtime_error("Invalid package filename length");
      std::vector<char> bytes(length);
      mz_zip_reader_get_filename(&zip, i, bytes.data(), length);
      const std::string name(bytes.data());
      const auto path = fs::u8path(name);
      if (name.empty() || name.size() + 1 != length || path.is_absolute() || path.has_root_name() ||
          name.find('\\') != std::string::npos ||
          name.find_first_of(":<>|?*\"") != std::string::npos)
        throw std::runtime_error("Unsafe ZIP entry path");
      for (const auto &component : path) {
        const auto part = component.string();
        if (part == ".." || part == ".")
          throw std::runtime_error("Package path traversal rejected");
        if (!part.empty() && (part.back() == '.' || part.back() == ' '))
          throw std::runtime_error("Package contains non-portable path components");
        const auto base = folded(component.stem().string());
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
            (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
             base[3] >= '1' && base[3] <= '9'))
          throw std::runtime_error("Package contains a reserved device name");
      }
      const auto mode = (stat.m_external_attr >> 16) & 0170000;
      if (mode == 0120000 || stat.m_is_encrypted || !stat.m_is_supported)
        throw std::runtime_error("Package contains unsupported links or encrypted entries");
      if (!names.insert(folded(path.generic_string())).second)
        throw std::runtime_error("Package has duplicate or case-colliding entries");
      if (stat.m_uncomp_size > 2ull * 1024 * 1024 * 1024 - total)
        throw std::runtime_error("Unpacked package exceeds 2 GiB");
      total += stat.m_uncomp_size;
    }
    if (fs::space(destination).available < total + 64ull * 1024 * 1024)
      throw std::runtime_error("Not enough disk space to unpack the module package");
    std::uint64_t completed = 0;
    if (control.progress)
      control.progress(0, total);
    for (mz_uint i = 0; i < count; ++i) {
      check_cancel();
      mz_zip_archive_file_stat stat{};
      mz_zip_reader_file_stat(&zip, i, &stat);
      std::vector<char> name(mz_zip_reader_get_filename(&zip, i, nullptr, 0));
      mz_zip_reader_get_filename(&zip, i, name.data(), static_cast<mz_uint>(name.size()));
      const auto target = destination / fs::u8path(name.data());
      if (stat.m_is_directory) {
        fs::create_directories(target);
        continue;
      }
      fs::create_directories(target.parent_path());
      std::ofstream output(target, std::ios::binary);
      ExtractOutput destination_output{output, control, completed, total, {}};
      const auto extracted =
          output && mz_zip_reader_extract_to_callback(&zip, i, extract_at, &destination_output, 0);
      if (destination_output.error)
        std::rethrow_exception(destination_output.error);
      if (!extracted)
        throw std::runtime_error("Package extraction or CRC verification failed");
      output.flush();
      if (!output)
        throw std::runtime_error("Could not flush an extracted package file");
      completed += stat.m_uncomp_size;
    }
    check_cancel();
    mz_zip_reader_end(&zip);
  } catch (...) {
    mz_zip_reader_end(&zip);
    throw;
  }
}
void write_zip(const fs::path &destination, const std::map<std::string, std::string> &files) {
  fs::create_directories(destination.parent_path());
  std::ofstream output(destination, std::ios::binary);
  if (!output)
    throw std::runtime_error("Could not create ZIP file");
  mz_zip_archive zip{};
  zip.m_pWrite = write_at;
  zip.m_pIO_opaque = &output;
  if (!mz_zip_writer_init(&zip, 0))
    throw std::runtime_error("Could not initialize ZIP writer");
  try {
    for (const auto &[name, contents] : files)
      if (!mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(),
                                 MZ_BEST_SPEED))
        throw std::runtime_error("Could not write ZIP entry");
    if (!mz_zip_writer_finalize_archive(&zip))
      throw std::runtime_error("Could not finalize ZIP file");
    mz_zip_writer_end(&zip);
    output.flush();
    if (!output)
      throw std::runtime_error("Could not flush ZIP file");
  } catch (...) {
    mz_zip_writer_end(&zip);
    throw;
  }
}
} // namespace xenon::launcher_next
