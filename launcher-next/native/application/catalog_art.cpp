#include "launcher_service.hpp"
#include "core/archive.hpp"
#include "picosha2/picosha2.h"
#include <fstream>
#include <regex>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
void LauncherService::cache_catalog_art(Json &entry,
                                        const std::shared_ptr<std::atomic_bool> &cancel) {
  const auto root = storage_ / "catalog-art";
  std::error_code directory_error;
  fs::create_directories(root, directory_error);
  if (directory_error)
    return;
  const auto valid =
      std::regex("^assets/[A-Za-z0-9._-]+/[A-Za-z0-9][A-Za-z0-9._/-]*\\.(?:png|jpg|jpeg|webp)$",
                 std::regex::icase);
  for (const auto *kind : {"tile", "hero"}) {
    const auto field = std::string(kind) + "Art", file_key = "_" + std::string(kind) + "File",
               version_key = "_" + std::string(kind) + "Version";
    const auto path = entry["launcher"].value(field, "");
    if (path.find("..") != std::string::npos || !std::regex_match(path, valid))
      continue;
    const auto url = "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/" + path;
    const auto filename = picosha2::hash256_hex_string(url.begin(), url.end()) +
                          fs::u8path(path).extension().string();
    const auto target = root / fs::u8path(filename);
    auto partial = root / (filename + "." + make_id() + ".part");
    try {
      std::uintmax_t used = 0;
      for (const auto &item : fs::directory_iterator(root))
        if (item.is_regular_file())
          used += item.file_size();
      if (used > 256ull * 1024 * 1024)
        throw std::runtime_error("Artwork cache is full");
      std::string etag;
      const auto meta = root / (filename + ".json");
      if (fs::is_regular_file(target) && fs::is_regular_file(meta))
        etag = read_document(meta, 4096).value("etag", "");
      HttpResponse response;
      {
        std::ofstream output(partial, std::ios::binary);
        if (!output)
          throw std::runtime_error("Could not create artwork cache file");
        response = http_->get(url, {20ull * 1024 * 1024,
                                    etag,
                                    [&] { return cancel->load(); },
                                    [&](std::string_view bytes) {
                                      output.write(bytes.data(), bytes.size());
                                      if (!output)
                                        throw std::runtime_error("Could not cache artwork");
                                    },
                                    {}});
        output.flush();
        if (!output)
          throw std::runtime_error("Could not flush artwork");
      }
      if (response.status == 304) {
        fs::remove(partial);
      } else if (response.status >= 200 && response.status < 300) {
        fs::rename(partial, target);
        write_document(meta, {{"etag", response.etag}});
      } else
        throw std::runtime_error("Artwork download failed");
    } catch (const OperationCancelled &) {
      std::error_code error;
      fs::remove(partial, error);
      throw;
    } catch (const std::exception &) {
      std::error_code error;
      fs::remove(partial, error);
    }
    if (fs::is_regular_file(target)) {
      entry["launcher"][file_key] = filename;
      entry["launcher"][version_key] =
          std::to_string(fs::last_write_time(target).time_since_epoch().count());
    }
  }
}
} // namespace xenon::launcher_next
