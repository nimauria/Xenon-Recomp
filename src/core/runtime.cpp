#include "xenon/core/runtime.hpp"

#include <iostream>

namespace xenon
{

Runtime::Runtime() = default;

Runtime::~Runtime()
{
    shutdown();
}

bool Runtime::initialize(const RuntimeConfig& config)
{
    if (initialized_)
    {
        return true;
    }

    config_ = config;

    if (config_.enable_logging)
    {
        std::cout << "[Xenon] Initializing runtime...\n";
    }

    initialized_ = true;

    if (config_.enable_logging)
    {
        std::cout << "[Xenon] Runtime initialized.\n";
    }

    return true;
}

void Runtime::shutdown()
{
    if (!initialized_)
    {
        return;
    }

    if (config_.enable_logging)
    {
        std::cout << "[Xenon] Shutting down runtime...\n";
    }

    initialized_ = false;
}

bool Runtime::is_initialized() const noexcept
{
    return initialized_;
}

} // namespace xenon
