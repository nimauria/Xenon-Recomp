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

void Runtime::set_filesystem(std::shared_ptr<filesystem::VirtualFileSystem> vfs)
{
    vfs_ = vfs;
    
    if (config_.enable_logging && vfs)
    {
        std::cout << "[Xenon] VFS connected to runtime.\n";
    }
}

std::shared_ptr<filesystem::VirtualFileSystem> Runtime::filesystem() const
{
    return vfs_;
}

} // namespace xenon
