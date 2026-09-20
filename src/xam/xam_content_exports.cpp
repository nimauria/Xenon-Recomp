#include "xenon/core/export_registry.hpp"
#include "xenon/xam/content_manager.hpp"
#include "xenon/xam/content_graph.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::xam {
namespace {

void write_u32_be(cpu::MemoryPort& memory, cpu::GuestAddress addr, std::uint32_t value) {
  memory.write32_be(addr, value);
}

}  // namespace

bool register_content_exports(core::ExportRegistry& registry, ContentManager& content_manager) {
  bool ok = true;

  // XamShowDeviceSelectorUI (0x0250) - Stubbed
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamShowDeviceSelectorUI";
    desc.ordinal = ordinal::XamShowDeviceSelectorUI;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      const auto user_index = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_device_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);

      // Return default HDD device
      if (out_device_ptr != 0) {
        write_u32_be(ctx.memory, out_device_ptr, content_manager.default_device());
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentCreateEnumerator (0x0234) - Stubbed
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentCreateEnumerator";
    desc.ordinal = ordinal::XamContentCreateEnumerator;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto out_handle_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[6]);

      // Return stub handle
      if (out_handle_ptr != 0) {
        write_u32_be(ctx.memory, out_handle_ptr, 0xDEADBEEF);
      }

      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentClose (0x0237) - Stubbed
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentClose";
    desc.ordinal = ordinal::XamContentClose;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentGetDeviceData (0x0238) - Enhanced with content services
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentGetDeviceData";
    desc.ordinal = ordinal::XamContentGetDeviceData;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      const auto device_id = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_data_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      
      if (out_data_ptr == 0) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      
      // Get device info
      auto device_opt = content_manager.get_device(device_id);
      if (!device_opt.has_value()) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      
      const auto& device = *device_opt;
      
      // Write device data structure (simplified)
      write_u32_be(ctx.memory, out_data_ptr + 0, device.device_id);
      write_u32_be(ctx.memory, out_data_ptr + 4, static_cast<std::uint32_t>(device.total_bytes >> 32));
      write_u32_be(ctx.memory, out_data_ptr + 8, static_cast<std::uint32_t>(device.total_bytes & 0xFFFFFFFF));
      write_u32_be(ctx.memory, out_data_ptr + 12, static_cast<std::uint32_t>(device.free_bytes >> 32));
      write_u32_be(ctx.memory, out_data_ptr + 16, static_cast<std::uint32_t>(device.free_bytes & 0xFFFFFFFF));
      
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  // XamContentGetDeviceName (0x0239) - Enhanced
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamContentGetDeviceName";
    desc.ordinal = ordinal::XamContentGetDeviceName;
    desc.requirement = core::ExportRequirement::Stubbed;
    desc.handler = [&content_manager](core::ExportCallContext& ctx) -> bool {
      const auto device_id = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
      const auto out_name_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
      const auto name_length = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);
      
      if (out_name_ptr == 0 || name_length == 0) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      
      auto device_opt = content_manager.get_device(device_id);
      if (!device_opt.has_value()) {
        ctx.cpu.gpr[3] = result::InvalidParameter;
        return true;
      }
      
      // Write device name (wide string)
      const auto& name = device_opt->name;
      for (std::size_t i = 0; i < name.size() && i < name_length - 1; ++i) {
        ctx.memory.write16_be(out_name_ptr + i * 2, static_cast<std::uint16_t>(name[i]));
      }
      ctx.memory.write16_be(out_name_ptr + name.size() * 2, 0);  // Null terminator
      
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
