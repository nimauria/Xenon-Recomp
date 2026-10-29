#include "platform/http.hpp"
#include <iostream>
int main() {
  try {
    std::string body;
    const auto response = xenon::launcher_next::platform_http()->get(
        "https://raw.githubusercontent.com/nimauria/Xenon-Modules/main/catalog.json",
        {1024 * 1024, "", {}, [&](std::string_view bytes) { body.append(bytes); }, {}});
    std::cout << "HTTPS status " << response.status << ", " << response.bytes << " bytes\n";
    return response.status == 200 ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
