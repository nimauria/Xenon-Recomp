#pragma once

#include <memory>
#include <string_view>

#include "xenon/network/client.hpp"

namespace xenon::network {

// Stable boundary for future XAM/XNet exports. Guest structures are decoded
// before this adapter and network JSON/URLs remain below it.
class XboxServicesNetworkAdapter final {
 public:
  explicit XboxServicesNetworkAdapter(std::shared_ptr<XenonNetworkClient> client)
      : client_(std::move(client)) {}

  [[nodiscard]] NetworkError availability(std::string_view capability,
                                          bool authentication_required) const;
  void perform(std::string_view capability, bool authentication_required,
               NetworkOperation operation, XenonNetworkClient::Completion completion) const;

 private:
  std::shared_ptr<XenonNetworkClient> client_;
};

}  // namespace xenon::network
