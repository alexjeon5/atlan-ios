// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

@class LauncherViewController;

@protocol LauncherDelegate <NSObject>
// The big button: start the navigation, or go back to it when it is already running.
- (void)launcherWantsNavigation:(LauncherViewController*)launcher;
@end

// The app's own home screen: the first-run steps, the start screen and the settings. It checks the ATLAN folder in
// the background while it is shown. The navigation runs (or waits) underneath; FINE DRIVE in the navigation brings
// this screen back.
@interface LauncherViewController : UIViewController
- (instancetype)initWithDelegate:(id<LauncherDelegate>)delegate;
@property(nonatomic) BOOL running;  // the navigation is running underneath
@end
