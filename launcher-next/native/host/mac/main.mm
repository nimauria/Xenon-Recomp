#import <Cocoa/Cocoa.h>
#include "include/cef_application_mac.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_library_loader.h"
#include <exception>
int xenon_launcher_main(int argc, char **argv);
void xenon_launcher_quit();
void xenon_launcher_reopen();
// CEF requires this protocol on NSApp to scope Chromium's nested event dispatch.
@interface XenonApplication : NSApplication <CefAppProtocol>
@property(nonatomic) BOOL handlingSendEvent;
@end
@implementation XenonApplication
- (BOOL)isHandlingSendEvent { return self.handlingSendEvent; }
- (void)sendEvent:(NSEvent *)event {
  CefScopedSendingEvent scope;
  [super sendEvent:event];
}
- (void)terminate:(id)sender { xenon_launcher_quit(); }
@end
@interface XenonDelegate : NSObject <NSApplicationDelegate>
@end
@implementation XenonDelegate
- (BOOL)applicationShouldHandleReopen:(NSApplication *)application hasVisibleWindows:(BOOL)visible {
  xenon_launcher_reopen(); return NO;
}
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)application { return YES; }
@end
int main(int argc, char **argv) {
  CefScopedLibraryLoader loader;
  if (!loader.LoadInMain()) return 1;
  @autoreleasepool {
    [XenonApplication sharedApplication];
    if (![NSApp isKindOfClass:[XenonApplication class]]) return 1;
    __attribute__((objc_precise_lifetime)) XenonDelegate *delegate = [XenonDelegate new];
    NSApp.delegate = delegate;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu *menu = [NSMenu new];
    NSMenuItem *applicationItem = [NSMenuItem new];
    [menu addItem:applicationItem];
    NSMenu *applicationMenu = [NSMenu new];
    [applicationMenu addItemWithTitle:@"Quit Xenon Launcher" action:@selector(terminate:) keyEquivalent:@"q"];
    applicationItem.submenu = applicationMenu;
    NSApp.mainMenu = menu;
    int result = 1;
    try { result = xenon_launcher_main(argc, argv); }
    catch (const std::exception &error) {
      NSAlert *alert = [NSAlert new];
      alert.messageText = @"Xenon Launcher startup problem";
      alert.informativeText = [NSString stringWithUTF8String:error.what()] ?: @"Startup failed";
      [alert runModal];
    }
    NSApp.delegate = nil;
    return result;
  }
}
