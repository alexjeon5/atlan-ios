// SPDX-License-Identifier: GPL-2.0-or-later
#import "launcher_settings.h"
#include "adapters/atlan/data_status.hpp"
#include <filesystem>

NSNotificationName const LauncherSettingsDidChange = @"LauncherSettingsDidChange";

namespace {
NSString* const keyAutoStart = @"launcher.autoStart";
NSString* const keyAutoStartAsked = @"launcher.autoStartAsked";
NSString* const keySetupDone = @"launcher.setupDone";
NSString* const keyAppearance = @"launcher.appearance";
NSString* const keyNotchInset = @"launcher.notchInset";
NSString* const keyNotchMap = @"launcher.notchMap";
NSString* const keyDuckOthers = @"launcher.duckOthers";
NSString* const keySessionLog = @"launcher.sessionLog";
NSString* const keyPerfLog = @"launcher.perfLog";
NSString* const keyNetworkLog = @"launcher.networkLog";
NSString* const keyProfiler = @"launcher.profiler";
NSString* const keyPinLocation = @"launcher.pinLocation";
NSString* const keyPinnedLatitude = @"launcher.pinnedLatitude";
NSString* const keyPinnedLongitude = @"launcher.pinnedLongitude";
NSString* const keyPinnedCourse = @"launcher.pinnedCourse";
NSString* const keyPickedFolder = @"launcher.pickedFolder";
NSString* const keyActiveFolder = @"launcher.activeFolder";
}

@implementation LauncherSettings {
    NSURL* _picked;  // resolved once, with its security scope open for the life of the app
    BOOL _pickedResolved;
}

+ (LauncherSettings*)shared {
    static LauncherSettings* settings;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        settings = [LauncherSettings new];
        [NSUserDefaults.standardUserDefaults registerDefaults:@{
            keyNotchInset: @YES, keyNotchMap: @YES, keyDuckOthers: @YES, keySessionLog: @YES, keyPerfLog: @YES, keyNetworkLog: @NO, keyProfiler: @NO,
            keyPinLocation: @NO, keyPinnedLatitude: @37.566535, keyPinnedLongitude: @126.977969, keyPinnedCourse: @0.0,
        }];
    });
    return settings;
}

- (BOOL)flag:(NSString*)key { return [NSUserDefaults.standardUserDefaults boolForKey:key]; }
- (void)setFlag:(BOOL)value forKey:(NSString*)key {
    [NSUserDefaults.standardUserDefaults setBool:value forKey:key];
    [self changed];
}
- (void)changed {
    [NSNotificationCenter.defaultCenter postNotificationName:LauncherSettingsDidChange object:self];
}

- (BOOL)autoStart { return [self flag:keyAutoStart]; }
- (void)setAutoStart:(BOOL)value { [self setFlag:value forKey:keyAutoStart]; }
- (BOOL)autoStartAsked { return [self flag:keyAutoStartAsked]; }
- (void)setAutoStartAsked:(BOOL)value { [self setFlag:value forKey:keyAutoStartAsked]; }
- (BOOL)setupDone { return [self flag:keySetupDone]; }
- (void)setSetupDone:(BOOL)value { [self setFlag:value forKey:keySetupDone]; }
- (BOOL)notchInset { return [self flag:keyNotchInset]; }
- (void)setNotchInset:(BOOL)value { [self setFlag:value forKey:keyNotchInset]; }
- (BOOL)notchMap { return [self flag:keyNotchMap]; }
- (void)setNotchMap:(BOOL)value { [self setFlag:value forKey:keyNotchMap]; }
- (BOOL)duckOthers { return [self flag:keyDuckOthers]; }
- (void)setDuckOthers:(BOOL)value { [self setFlag:value forKey:keyDuckOthers]; }
- (BOOL)sessionLog { return [self flag:keySessionLog]; }
- (void)setSessionLog:(BOOL)value { [self setFlag:value forKey:keySessionLog]; }
- (BOOL)perfLog { return [self flag:keyPerfLog]; }
- (void)setPerfLog:(BOOL)value { [self setFlag:value forKey:keyPerfLog]; }
- (BOOL)networkLog { return [self flag:keyNetworkLog]; }
- (void)setNetworkLog:(BOOL)value { [self setFlag:value forKey:keyNetworkLog]; }
- (BOOL)profiler { return [self flag:keyProfiler]; }
- (void)setProfiler:(BOOL)value { [self setFlag:value forKey:keyProfiler]; }
- (BOOL)pinLocation { return [self flag:keyPinLocation]; }
- (void)setPinLocation:(BOOL)value { [self setFlag:value forKey:keyPinLocation]; }
- (double)pinnedLatitude { return [NSUserDefaults.standardUserDefaults doubleForKey:keyPinnedLatitude]; }
- (double)pinnedLongitude { return [NSUserDefaults.standardUserDefaults doubleForKey:keyPinnedLongitude]; }
- (void)setPinnedLatitude:(double)latitude longitude:(double)longitude {
    [NSUserDefaults.standardUserDefaults setDouble:latitude forKey:keyPinnedLatitude];
    [NSUserDefaults.standardUserDefaults setDouble:longitude forKey:keyPinnedLongitude];
    [self changed];
}
- (double)pinnedCourse { return [NSUserDefaults.standardUserDefaults doubleForKey:keyPinnedCourse]; }
- (void)setPinnedCourse:(double)value {
    [NSUserDefaults.standardUserDefaults setDouble:value forKey:keyPinnedCourse];
    [self changed];
}

- (LauncherAppearance)appearance {
    const NSInteger value = [NSUserDefaults.standardUserDefaults integerForKey:keyAppearance];
    return value == LauncherAppearanceLight || value == LauncherAppearanceDark ? (LauncherAppearance)value : LauncherAppearanceSystem;
}
- (void)setAppearance:(LauncherAppearance)value {
    [NSUserDefaults.standardUserDefaults setInteger:value forKey:keyAppearance];
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
        if ([scene isKindOfClass:UIWindowScene.class])
            for (UIWindow* window in ((UIWindowScene*)scene).windows) [self applyAppearanceTo:window];
    [self changed];
}
- (void)applyAppearanceTo:(UIWindow*)window {
    switch (self.appearance) {
    case LauncherAppearanceLight: window.overrideUserInterfaceStyle = UIUserInterfaceStyleLight; break;
    case LauncherAppearanceDark: window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark; break;
    default: window.overrideUserInterfaceStyle = UIUserInterfaceStyleUnspecified; break;
    }
}

- (NSString*)documentsPath {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject.path;
}

- (NSURL*)pickedFolder {
    if (_pickedResolved) return _picked;
    _pickedResolved = YES;
    NSData* bookmark = [NSUserDefaults.standardUserDefaults dataForKey:keyPickedFolder];
    if (!bookmark) return nil;
    BOOL stale = NO;
    NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
    if (!url) return nil;
    [url startAccessingSecurityScopedResource];
    if (stale) {
        NSData* fresh = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if (fresh) [NSUserDefaults.standardUserDefaults setObject:fresh forKey:keyPickedFolder];
    }
    _picked = url;
    return _picked;
}

- (void)usePickedFolder:(NSURL*)url {
    [url startAccessingSecurityScopedResource];
    // A folder inside the app's own Documents needs no bookmark: that is the default place.
    NSString* documents = self.documentsPath.stringByStandardizingPath;
    NSString* path = url.path.stringByStandardizingPath;
    if ([path isEqualToString:documents] || [path hasPrefix:[documents stringByAppendingString:@"/"]]) {
        [url stopAccessingSecurityScopedResource];
        [self forgetPickedFolder];
        return;
    }
    if (_picked) [_picked stopAccessingSecurityScopedResource];
    NSData* bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (bookmark) [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:keyPickedFolder];
    _picked = url;
    _pickedResolved = YES;
    [self changed];
}

- (void)forgetPickedFolder {
    if (_picked) [_picked stopAccessingSecurityScopedResource];
    _picked = nil;
    _pickedResolved = YES;
    [NSUserDefaults.standardUserDefaults removeObjectForKey:keyPickedFolder];
    [self changed];
}

- (BOOL)atlanFolderIsPicked { return self.pickedFolder != nil; }

- (NSArray<NSString*>*)documentsFolders {
    NSMutableArray<NSString*>* folders = [NSMutableArray array];
    for (const auto& path : atlan::shell::find_atlan_folders(self.documentsPath.UTF8String ?: ""))
        if (NSString* folder = [NSString stringWithUTF8String:path.c_str()]) [folders addObject:folder];
    return folders;
}

- (void)chooseDocumentsFolder:(NSString*)path {
    [NSUserDefaults.standardUserDefaults setObject:path.lastPathComponent forKey:keyActiveFolder];
    if (self.pickedFolder) [self forgetPickedFolder];
    else [self changed];
}

// The chosen Documents folder if it is still there, else the only one; nil when that leaves no single answer.
- (NSString*)documentsChoiceIn:(NSArray<NSString*>*)folders {
    NSString* chosen = [NSUserDefaults.standardUserDefaults stringForKey:keyActiveFolder];
    for (NSString* folder in folders)
        if (chosen && [folder.lastPathComponent isEqualToString:chosen]) return folder;
    return folders.count == 1 ? folders.firstObject : nil;
}

- (BOOL)needsFolderChoice {
    if (self.pickedFolder) return NO;
    NSArray<NSString*>* folders = self.documentsFolders;
    return folders.count > 1 && ![self documentsChoiceIn:folders];
}

- (NSString*)atlanFolder {
    if (NSString* picked = self.pickedFolder.path) {
        const std::string resolved = atlan::shell::resolve_atlan_folder(picked.UTF8String);
        return [NSString stringWithUTF8String:resolved.c_str()] ?: @"";
    }
    return [self documentsChoiceIn:self.documentsFolders] ?: @"";
}
@end
