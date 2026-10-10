#include "xenon/logging/probe_log.hpp"

#include <cstdarg>
#include <cstdio>

namespace xenon::logging {

void append_probe_log(const char* file_name, const char* format, ...) {
  if (FILE* file = std::fopen(file_name, "a")) {
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);
    std::fclose(file);
  }
}

}  // namespace xenon::logging
