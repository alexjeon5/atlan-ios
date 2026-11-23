// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#include "atlan/demo.hpp"
#include <exception>

@interface RuntimeAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation RuntimeAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application;
    (void)options;
    auto* controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.systemBackgroundColor;
    auto* resultLabel = [UILabel new];
    resultLabel.translatesAutoresizingMaskIntoConstraints = NO;
    resultLabel.font = [UIFont monospacedDigitSystemFontOfSize:72 weight:UIFontWeightMedium];
    resultLabel.textAlignment = NSTextAlignmentCenter;
    resultLabel.numberOfLines = 0;
    @autoreleasepool {
        try {
            const auto result = atlan::run_demo();
            if (result.execution.event == atlan::Event::supervisor_call && result.value == 42) {
                resultLabel.text = [NSString stringWithFormat:@"%u", result.value];
                resultLabel.accessibilityLabel = @"ARM32 실행 결과 42";
            } else {
                resultLabel.text = @"실행 실패";
            }
        } catch (const std::exception&) {
            resultLabel.text = @"실행 실패";
        }
    }
    [controller.view addSubview:resultLabel];
    [NSLayoutConstraint activateConstraints:@[
        [resultLabel.centerXAnchor constraintEqualToAnchor:controller.view.centerXAnchor],
        [resultLabel.centerYAnchor constraintEqualToAnchor:controller.view.centerYAnchor],
        [resultLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:controller.view.safeAreaLayoutGuide.leadingAnchor constant:24],
        [resultLabel.trailingAnchor constraintLessThanOrEqualToAnchor:controller.view.safeAreaLayoutGuide.trailingAnchor constant:-24]
    ]];
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(RuntimeAppDelegate.class));
    }
}
