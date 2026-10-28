#include "launcher_service.hpp"
#include <iostream>
int main(int argc, char **argv) {
  try {
    xenon::launcher_next::LauncherService service(
        argc > 1 ? std::filesystem::u8path(argv[1]) : xenon::launcher_next::default_storage());
    std::string line;
    while (std::getline(std::cin, line)) {
      try {
        std::cout << service.request(xenon::launcher_next::Json::parse(line)).dump() << std::endl;
      } catch (const std::exception &error) {
        std::cout << xenon::launcher_next::Json{{"ok", false}, {"error", error.what()}}.dump()
                  << std::endl;
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
