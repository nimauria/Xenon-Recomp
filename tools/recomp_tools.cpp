#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>

#include "xenon/recomp/driver.hpp"
#include "xenon/cpu/decoder.hpp"

int main(int argc, char** argv) {
  const std::string tool = XENON_TOOL_NAME;
  if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
    std::cout << tool << " - Xenon recompilation utility\n";
    if (tool == "recomp-driver") std::cout << "usage: recomp-driver <game.xex> [output-directory] [--hints file] [--json]\n";
    else if (tool == "ppc-disasm") std::cout << "usage: ppc-disasm <game.xex> [--json]\n";
    else if (tool == "ir-dump") std::cout << "usage: ir-dump <game.xex> [--json]\n";
    else if (tool == "import-scanner") std::cout << "usage: import-scanner <game.xex> [--json]\n";
    else std::cout << "usage: module-inspector <game.xex> [--json]\n";
    return argc < 2 ? 1 : 0;
  }
  xenon::recomp::DriverOptions options;
  options.input = argv[1];
  for (int index = 2; index < argc; ++index) {
    if (std::string(argv[index]) == "--json") {
      continue;
    } else if (std::string(argv[index]) == "--hints" && index + 1 < argc) {
      options.hints_file = argv[++index];
    } else if (tool == "recomp-driver" && index == 2) {
      options.output = argv[index];
    } else {
      std::cerr << tool << ": unknown argument: " << argv[index] << "\n";
      return 1;
    }
  }
  xenon::recomp::AnalysisReport report;
  std::string error;
  if (!xenon::recomp::load_and_analyze(options, report, error)) {
    std::cerr << tool << ": " << error << "\n";
    return 2;
  }
  bool json = false;
  for (int index = 2; index < argc; ++index)
    if (std::string(argv[index]) == "--json") json = true;
  if (tool == "recomp-driver") {
    if (!xenon::recomp::generate_project(options, report, error)) {
      std::cerr << tool << ": " << error << "\n";
      return 3;
    }
    std::cout << (json ? xenon::recomp::format_report_json(report)
                       : xenon::recomp::format_report(report));
  } else if (tool == "ir-dump") {
    for (const auto& function : report.functions) std::cout << xenon::recomp::format_ir(function);
  } else if (tool == "ppc-disasm") {
    xenon::cpu::Decoder decoder;
    for (const auto& section : report.image.sections) {
      if (!section.executable) continue;
      const auto count = section.bytes.size() / 4u;
      for (std::size_t index = 0; index < count; ++index) {
        const auto word = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4])) << 24u) |
                          (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4 + 1])) << 16u) |
                          (static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4 + 2])) << 8u) |
                          static_cast<std::uint32_t>(std::to_integer<unsigned char>(section.bytes[index * 4 + 3]));
        const auto instruction = decoder.decode(section.virtual_address + static_cast<std::uint32_t>(index * 4u), word);
        std::cout << "0x" << std::hex << instruction.address << ": 0x" << word << " "
                  << (instruction.valid() ? instruction.mnemonic() : "invalid") << "\n";
      }
    }
  } else if (tool == "import-scanner") {
    for (const auto& item : report.image.imports)
      std::cout << item.module << "!" << item.symbol << " ordinal=" << item.ordinal << " thunk=0x" << std::hex << item.guest_thunk << "\n";
  } else if (tool == "module-inspector") {
    std::cout << "format=" << static_cast<unsigned>(report.image.format) << " entry=0x" << std::hex << report.image.entry_point
              << " sections=" << std::dec << report.image.sections.size() << " imports=" << report.image.imports.size()
              << " exports=" << report.image.exports.size()
              << " function_metadata=" << report.image.function_metadata.size() << "\n";
  } else {
    std::cout << xenon::recomp::format_report(report);
  }
  return report.unresolved.empty() ? 0 : 4;
}
