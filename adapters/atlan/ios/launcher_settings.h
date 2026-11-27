// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

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
@property(nonatomic) BOOL notchMap;        // experimental: the map runs on under the notch's strip
@property(nonatomic) BOOL duckOthers;      // lower other apps' audio while a voice prompt plays
@property(nonatomic) BOOL sessionLog;
@property(nonatomic) BOOL perfLog;
@property(nonatomic) BOOL networkLog;
@property(nonatomic) BOOL profiler;
// Developer: the navigation gets this fixed position instead of the device's location (for debugging and recordings).
@property(nonatomic) BOOL pinLocation;
@property(nonatomic, readonly) double pinnedLatitude;
@property(nonatomic, readonly) double pinnedLongitude;
- (void)setPinnedLatitude:(double)latitude longitude:(double)longitude;
@property(nonatomic) double pinnedCourse;  // degrees clockwise from north

@property(nonatomic, readonly) NSString* documentsPath;
// A folder the user picked outside the app's Documents, kept with access across launches; nil when none.
@property(nonatomic, readonly, nullable) NSURL* pickedFolder;
- (void)usePickedFolder:(NSURL*)url;  // a URL from the folder picker
- (void)forgetPickedFolder;
// ATLAN folders side by side in Documents (ATLAN, ATLAN_RTA, ...), by name.
@property(nonatomic, readonly) NSArray<NSString*>* documentsFolders;
// The one of them chosen when there are several.
- (void)chooseDocumentsFolder:(NSString*)path;
// Several are there, none has been chosen and no folder elsewhere was picked.
@property(nonatomic, readonly) BOOL needsFolderChoice;
// The ATLAN folder in use: inside the picked folder if there is one, else the chosen (or only) one in Documents.
// Empty when there is none or a choice is still needed.
@property(nonatomic, readonly) NSString* atlanFolder;
@property(nonatomic, readonly) BOOL atlanFolderIsPicked;

- (void)applyAppearanceTo:(UIWindow*)window;
@end

NS_ASSUME_NONNULL_END
