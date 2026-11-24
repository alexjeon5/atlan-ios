// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "apple_audio_sink.hpp"
#include "metal_gl_backend.hpp"
#import <QuartzCore/CAMetalLayer.h>
#include "atlan/demo.hpp"
#include "shell/data_file.hpp"
#include "shell/render_config.hpp"
#include "shell/session.hpp"
#include <exception>
#include <memory>

@interface ShellViewController : UIViewController <UIDocumentPickerDelegate>
@end

@implementation ShellViewController {
    UITextView* _log;
    UIButton* _library;
    UIButton* _run;
    UILabel* _map;
    std::unique_ptr<atlan::shell::GuestSession> _session;
    atlan::AppleAudioSink _audio;
    atlan::shell::SurfaceState _surface;
    UIView* _metalView;
    std::unique_ptr<atlan::MetalGlBackend> _gl;
}

- (NSURL*)libraryURL {
    NSURL* base = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                       inDomains:NSUserDomainMask].firstObject;
    [NSFileManager.defaultManager createDirectoryAtURL:base withIntermediateDirectories:YES
                                            attributes:nil error:nil];
    return [base URLByAppendingPathComponent:@"guest.so"];
}

- (NSString*)documentsPath {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject.path;
}

// The map stays where the user put it; the largest regular file in Documents is used in place.
- (NSString*)mapPath {
    NSURL* docs = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                       inDomains:NSUserDomainMask].firstObject;
    NSArray<NSURL*>* items = [NSFileManager.defaultManager
        contentsOfDirectoryAtURL:docs includingPropertiesForKeys:@[NSURLFileSizeKey, NSURLIsRegularFileKey]
                         options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
    NSURL* best = nil;
    NSInteger bestSize = -1;
    for (NSURL* item in items) {
        NSNumber* regular = nil; NSNumber* size = nil;
        [item getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil];
        [item getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        if (regular.boolValue && size.longLongValue > bestSize) { best = item; bestSize = size.longLongValue; }
    }
    return best.path;
}

- (void)append:(NSString*)line {
    _log.text = [_log.text stringByAppendingFormat:@"%@\n", line];
    [_log scrollRangeToVisible:NSMakeRange(_log.text.length, 0)];
}

- (void)refresh {
    BOOL hasLibrary = [NSFileManager.defaultManager fileExistsAtPath:self.libraryURL.path];
    NSString* path = self.mapPath;
    if (path) {
        const auto status = atlan::shell::inspect_data_file(path.UTF8String);
        _map.text = status.ok
            ? [NSString stringWithFormat:@"%@  %.2f GB", path.lastPathComponent, status.size / 1073741824.0]
            : [NSString stringWithFormat:@"%@  %s", path.lastPathComponent, status.message.c_str()];
        _map.textColor = status.ok ? UIColor.labelColor : UIColor.systemRedColor;
    } else {
        _map.text = @"—";
        _map.textColor = UIColor.secondaryLabelColor;
    }
    _run.enabled = hasLibrary && !(_session && _session->state() == atlan::shell::SessionState::running);
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    _library = [UIButton buttonWithType:UIButtonTypeSystem];
    [_library setImage:[UIImage systemImageNamed:@"square.and.arrow.down"] forState:UIControlStateNormal];
    _library.accessibilityLabel = @"라이브러리 가져오기";
    [_library addTarget:self action:@selector(importLibrary) forControlEvents:UIControlEventTouchUpInside];
    _run = [UIButton buttonWithType:UIButtonTypeSystem];
    [_run setImage:[UIImage systemImageNamed:@"play.fill"] forState:UIControlStateNormal];
    _run.accessibilityLabel = @"실행";
    [_run addTarget:self action:@selector(runGuest) forControlEvents:UIControlEventTouchUpInside];
    _map = [UILabel new];
    _map.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    _map.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _log = [UITextView new];
    _log.editable = NO;
    _log.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    _log.backgroundColor = [UIColor.secondarySystemBackgroundColor colorWithAlphaComponent:0.55];
    _metalView = [UIView new];
    _metalView.layer.opaque = YES;
    auto* bar = [[UIStackView alloc] initWithArrangedSubviews:@[_library, _run, _map]];
    bar.spacing = 20;
    bar.alignment = UIStackViewAlignmentCenter;
    [self.view insertSubview:_metalView atIndex:0];
    _metalView.frame = self.view.bounds;
    _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    for (UIView* view in @[bar, _log]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [self.view addSubview:view];
    }
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [bar.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],
        [bar.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [bar.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [_log.topAnchor constraintEqualToAnchor:bar.bottomAnchor constant:12],
        [_log.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_log.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_log.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor]
    ]];
    try {
        const auto demo = atlan::run_demo();
        [self append:[NSString stringWithFormat:@"selftest %u", demo.value]];
    } catch (const std::exception&) {
        [self append:@"selftest failed"];
    }
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refresh)
        name:UIApplicationDidBecomeActiveNotification object:nil];
    [self refresh];
}

- (void)importLibrary {
    auto* picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    NSURL* destination = self.libraryURL;
    NSURL* staged = [destination URLByAppendingPathExtension:@"part"];
    NSError* error = nil;
    [NSFileManager.defaultManager removeItemAtURL:staged error:nil];
    if ([NSFileManager.defaultManager copyItemAtURL:urls.firstObject toURL:staged error:&error] &&
        [NSFileManager.defaultManager replaceItemAtURL:destination withItemAtURL:staged backupItemName:nil
                                               options:0 resultingItemURL:nil error:&error]) {
        [self append:[NSString stringWithFormat:@"library %@", urls.firstObject.lastPathComponent]];
    } else if ([NSFileManager.defaultManager moveItemAtURL:staged toURL:destination error:nil]) {
        [self append:[NSString stringWithFormat:@"library %@", urls.firstObject.lastPathComponent]];
    } else {
        [self append:[NSString stringWithFormat:@"import failed: %@", error.localizedDescription]];
    }
    [self refresh];
}

- (void)runGuest {
    if (_session) { _session->join(); _session.reset(); }
    NSString* map = self.mapPath;
    if (map) {
        const auto status = atlan::shell::inspect_data_file(map.UTF8String);
        [self append:status.ok ? @"map ok" : [NSString stringWithFormat:@"map rejected: %s", status.message.c_str()]];
    } else {
        [self append:@"map missing"];
    }
    __weak ShellViewController* weakSelf = self;
    atlan::shell::SessionConfig config;
    config.library_path = self.libraryURL.path.UTF8String;
    config.audio_sink = &_audio;
    config.data_root = self.documentsPath.UTF8String;
    _surface.set(atlan::shell::render_size(self.view.bounds.size.width, self.view.bounds.size.height,
                                           self.view.window.screen.nativeScale ?: UIScreen.mainScreen.nativeScale));
    config.surface = &_surface;
    // A CAMetalLayer sublayer sized in native pixels; GL output is presented there.
    {
        const auto size = _surface.get();
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.frame = _metalView.bounds;
        layer.drawableSize = CGSizeMake(size.width, size.height);
        [_metalView.layer addSublayer:layer];
        _gl = std::make_unique<atlan::MetalGlBackend>((__bridge void*)layer);
        config.gl_backend = _gl.get();
    }
    _session = std::make_unique<atlan::shell::GuestSession>(
        config,
        [weakSelf](const std::string& line) {
            NSString* text = [NSString stringWithUTF8String:line.c_str()] ?: @"?";
            dispatch_async(dispatch_get_main_queue(), ^{
                ShellViewController* strongSelf = weakSelf;
                [strongSelf append:text];
                [strongSelf refresh];
            });
        });
    if (!_session->start()) [self append:@"thread start failed"];
    [self refresh];
}

- (void)dealloc {
    if (_session) { _session->stop(); _session->join(); }
}
@end

@interface RuntimeAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation RuntimeAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application;
    (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [ShellViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(RuntimeAppDelegate.class));
    }
}
