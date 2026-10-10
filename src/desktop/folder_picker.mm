#import <AppKit/AppKit.h>
#include <string>

std::string choose_project_folder(const std::string& initial) {
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Select project folder";
    panel.prompt = @"Select folder";
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    if (!initial.empty()) {
      panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:initial.c_str()]];
    }
    if ([panel runModal] != NSModalResponseOK) return {};
    return std::string{panel.URL.fileSystemRepresentation};
  }
}
