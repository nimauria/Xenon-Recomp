// xenon-prepare command-line options.

#include <charconv>
#include <system_error>

#include "prepare_internal.hpp"

namespace xenon::prepare_tool {

void print_usage() {
  std::cout <<
      "usage: xenon-prepare --content <path> --cache-root <dir>\n"
      "                     [--module <hint-package-dir>] [--module-id <id>]\n"
      "                     [--title-update <path>] [--config Release|Debug]\n"
      "                     [--status-file <path>] [--stop-signal <path>]\n"
      "                     [--observations <adaptive-observations.jsonl>]\n"
      "                     [--knowledge <knowledge.jsonl>]\n"
      "                     [--recomp-root <path>] [--jobs auto|N] [--force] [--query]\n"
      "  --jobs sets how many compiler processes build the native module;\n"
      "  'auto' (default) is min(8, logical CPUs - 1).\n";
}

bool parse_args(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (arg == "--content") options.content = next();
    else if (arg == "--title-update") options.title_update = next();
    else if (arg == "--module") options.module_dir = next();
    else if (arg == "--module-id") options.module_id = next();
    else if (arg == "--cache-root") options.cache_root = next();
    else if (arg == "--status-file") options.status_file = next();
    else if (arg == "--stop-signal") options.stop_signal = next();
    else if (arg == "--recomp-root") options.recomp_root = next();
    else if (arg == "--observations") options.observations = next();
    else if (arg == "--knowledge") options.knowledge = next();
    else if (arg == "--config") options.config = next();
    else if (arg == "--jobs") {
      const auto value = next();
      if (value == "auto") {
        options.jobs.reset();
      } else {
        std::size_t parsed = 0;
        const auto* const end = value.data() + value.size();
        const auto [stop, ec] = std::from_chars(value.data(), end, parsed);
        if (value.empty() || ec != std::errc{} || stop != end || parsed == 0) {
          error = "--jobs must be 'auto' or a positive integer, got '" + value + "'";
          return false;
        }
        options.jobs = parsed;
      }
    }
    else if (arg == "--force") options.force = true;
    else if (arg == "--query") options.query = true;
    else if (arg == "--help" || arg == "-h") { print_usage(); std::exit(0); }
    else { error = "unknown argument: " + arg; return false; }
  }
  if (options.content.empty() || options.cache_root.empty()) {
    error = "--content and --cache-root are required";
    return false;
  }
#if defined(XENON_SOURCE_ROOT)
  if (options.recomp_root.empty()) options.recomp_root = XENON_SOURCE_ROOT;
#endif
  return true;
}

}  // namespace xenon::prepare_tool
