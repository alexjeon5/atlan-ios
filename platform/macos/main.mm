// SPDX-License-Identifier: GPL-2.0-or-later
#import <AppKit/AppKit.h>
#include "atlan/demo.hpp"
#include <exception>

@interface RuntimeMacDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) NSWindow* window;
@end
@implementation RuntimeMacDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 360)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"ARM32 Runtime";
    auto* label = [NSTextField labelWithString:@""];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont monospacedDigitSystemFontOfSize:72 weight:NSFontWeightMedium];
    label.alignment = NSTextAlignmentCenter;
    try {
        const auto result = atlan::run_demo();
        label.stringValue = result.value == 42 && result.execution.event == atlan::Event::supervisor_call
            ? @"42" : @"실행 실패";
    } catch (const std::exception&) {
        label.stringValue = @"실행 실패";
    }
    [self.window.contentView addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerXAnchor constraintEqualToAnchor:self.window.contentView.centerXAnchor],
        [label.centerYAnchor constraintEqualToAnchor:self.window.contentView.centerYAnchor]
    ]];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}
@end
int main() {
    @autoreleasepool {
        auto* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        __attribute__((objc_precise_lifetime)) RuntimeMacDelegate* delegate = [RuntimeMacDelegate new];
        app.delegate = delegate;
        [app run];
    }
}
