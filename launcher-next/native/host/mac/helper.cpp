#include "include/cef_sandbox_mac.h"
#include "include/wrapper/cef_library_loader.h"
int xenon_launcher_main(int argc, char **argv);
int main(int argc, char **argv) {
  CefScopedSandboxContext sandbox;
  if (!sandbox.Initialize(argc, argv)) return 1;
  CefScopedLibraryLoader loader;
  if (!loader.LoadInHelper()) return 1;
  // Use the same renderer application as the main executable: custom schemes
  // and the message-router context hooks must also exist in renderer helpers.
  return xenon_launcher_main(argc, argv);
}
