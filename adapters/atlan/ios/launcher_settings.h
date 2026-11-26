// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, LauncherAppearance) { LauncherAppearanceSystem, LauncherAppearanceLight, LauncherAppearanceDark };

// Posted on the main thread whenever a setting changes.
extern NSNotificationName const LauncherSettingsDidChange;

// The launcher's own settings, kept in the app's user defaults, and where the ATLAN folder is.
@interface LauncherSettings : NSObject
@property(class, readonly) LauncherSettings* shared;
@property(nonatomic) BOOL autoStart;
@property(nonatomic) BOOL autoStartAsked;   // the first-run question was answered
@property(nonatomic) BOOL setupDone;        // a complete ATLAN folder has been seen once
@property(nonatomic) LauncherAppearance appearance;
@property(nonatomic) BOOL notchInset;
@property(nonatomic) BOOL sessionLog;
@property(nonatomic) BOOL perfLog;
@property(nonatomic) BOOL networkLog;
@property(nonatomic) BOOL profiler;

@property(nonatomic, readonly) NSString* documentsPath;
// A folder the user picked outside the app's Documents, kept with access across launches; nil when none.
@property(nonatomic, readonly, nullable) NSURL* pickedFolder;
- (void)usePickedFolder:(NSURL*)url;  // a URL from the folder picker
- (void)forgetPickedFolder;
// The ATLAN folder in use: inside the picked folder if there is one, else Documents/ATLAN. Empty when neither exists.
@property(nonatomic, readonly) NSString* atlanFolder;
@property(nonatomic, readonly) BOOL atlanFolderIsPicked;

- (void)applyAppearanceTo:(UIWindow*)window;
@end
