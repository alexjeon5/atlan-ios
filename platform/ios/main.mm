// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <CoreLocation/CoreLocation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "apple_audio_sink.hpp"
#include "metal_gl_backend.hpp"
#import <QuartzCore/CAMetalLayer.h>
#include "atlan/demo.hpp"
#include "shell/data_file.hpp"
#include "shell/input.hpp"
#include "shell/render_config.hpp"
#include "shell/session.hpp"
#include <atomic>
#include <exception>
#include <memory>
#include <vector>

@interface ShellViewController : UIViewController <UIDocumentPickerDelegate, CLLocationManagerDelegate>
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
    CAMetalLayer* _layer;
    UIButton* _logToggle;
    atlan::shell::InputQueue _input;
    std::atomic<std::uint64_t> _guestSurface;  // (width << 32) | height from the guest's setSurfaces; 0 until reported
    std::vector<UITouch*> _fingers;            // slot index = pointer id; nil when free
    CLLocationManager* _location;
}

// Bindings from host input to the guest's native methods: Documents/input.cfg (format in shell/input.hpp).
- (NSString*)inputBindingsPath {
    return [self.documentsPath stringByAppendingPathComponent:@"input.cfg"];
}

- (void)toggleLog {
    _log.hidden = !_log.hidden;  // the log covers the surface and takes its touches while shown
}

- (atlan::shell::PixelSize)touchSurface {
    const auto guest = _guestSurface.load();
    if (guest != 0) return {static_cast<std::uint32_t>(guest >> 32), static_cast<std::uint32_t>(guest)};
    return _surface.get();
}

// Where the guest surface is shown inside the metal view; touches map through the same rectangle.
- (atlan::shell::ViewRect)shownRect {
    const CGSize view = _metalView.bounds.size;
    return atlan::shell::fit_rect(view.width, view.height, self.touchSurface);
}

- (void)layoutSurface {
    if (!_layer) return;
    const auto shown = self.shownRect;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _layer.frame = CGRectMake(shown.x, shown.y, shown.width, shown.height);
    [CATransaction commit];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self layoutSurface];
}

- (void)forward:(NSSet<UITouch*>*)touches phase:(atlan::shell::TouchPhase)phase {
    using atlan::shell::TouchPhase;
    const auto shown = self.shownRect;
    for (UITouch* touch in touches) {
        std::size_t slot = 0;
        while (slot < _fingers.size() && _fingers[slot] != touch) ++slot;
        if (slot == _fingers.size()) {
            if (phase != TouchPhase::down) continue;  // began before the surface took touches
            slot = 0;
            while (slot < _fingers.size() && _fingers[slot] != nil) ++slot;
            if (slot == _fingers.size()) _fingers.push_back(nil);
            _fingers[slot] = touch;
        }
        const CGPoint at = [touch locationInView:_metalView];
        const auto point = atlan::shell::to_surface(at.x, at.y, shown, self.touchSurface);
        _input.push(atlan::shell::TouchEvent{phase, static_cast<std::uint32_t>(slot), point.x, point.y,
                                             static_cast<std::uint64_t>(touch.timestamp * 1000.0)});
        if (phase == TouchPhase::up || phase == TouchPhase::cancel) _fingers[slot] = nil;
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event; [self forward:touches phase:atlan::shell::TouchPhase::down];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event; [self forward:touches phase:atlan::shell::TouchPhase::move];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event; [self forward:touches phase:atlan::shell::TouchPhase::up];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event; [self forward:touches phase:atlan::shell::TouchPhase::cancel];
}

- (void)startLocation {
    if (!_location) {
        _location = [CLLocationManager new];
        _location.delegate = self;
        _location.desiredAccuracy = kCLLocationAccuracyBestForNavigation;
        _location.activityType = CLActivityTypeAutomotiveNavigation;
        _location.pausesLocationUpdatesAutomatically = NO;
    }
    const CLAuthorizationStatus status = _location.authorizationStatus;
    if (status == kCLAuthorizationStatusNotDetermined) [_location requestWhenInUseAuthorization];
    else if (status == kCLAuthorizationStatusDenied || status == kCLAuthorizationStatusRestricted) [self append:@"location denied"];
    else [_location startUpdatingLocation];
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager {
    const CLAuthorizationStatus status = manager.authorizationStatus;
    if (status == kCLAuthorizationStatusAuthorizedWhenInUse || status == kCLAuthorizationStatusAuthorizedAlways)
        [manager startUpdatingLocation];
    else if (status != kCLAuthorizationStatusNotDetermined) [self append:@"location denied"];
}
- (void)locationManager:(CLLocationManager*)manager didUpdateLocations:(NSArray<CLLocation*>*)locations {
    (void)manager;
    CLLocation* fix = locations.lastObject;
    if (!fix || fix.horizontalAccuracy < 0) return;  // an invalid fix carries no position
    _input.push(atlan::shell::LocationFix{fix.coordinate.latitude, fix.coordinate.longitude, fix.altitude, fix.speed,
                                          fix.course, fix.horizontalAccuracy,
                                          static_cast<std::uint64_t>(fix.timestamp.timeIntervalSince1970 * 1000.0)});
}
- (void)locationManager:(CLLocationManager*)manager didFailWithError:(NSError*)error {
    (void)manager;
    if (error.code != kCLErrorLocationUnknown) [self append:[NSString stringWithFormat:@"location failed: %@", error.localizedDescription]];
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
    // Appending in place keeps a long log cheap; the oldest half goes once it is large.
    NSMutableString* text = _log.textStorage.mutableString;
    if (text.length > 200000) [text deleteCharactersInRange:NSMakeRange(0, text.length / 2)];
    [text appendFormat:@"%@\n", line];
    if (!_log.hidden) [_log scrollRangeToVisible:NSMakeRange(text.length, 0)];
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
    _logToggle = [UIButton buttonWithType:UIButtonTypeSystem];
    [_logToggle setImage:[UIImage systemImageNamed:@"text.alignleft"] forState:UIControlStateNormal];
    _logToggle.accessibilityLabel = @"로그";
    [_logToggle addTarget:self action:@selector(toggleLog) forControlEvents:UIControlEventTouchUpInside];
    _map = [UILabel new];
    _map.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    _map.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _log = [UITextView new];
    _log.editable = NO;
    _log.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    _log.backgroundColor = [UIColor.secondarySystemBackgroundColor colorWithAlphaComponent:0.55];
    _metalView = [UIView new];
    _metalView.layer.opaque = YES;
    _metalView.multipleTouchEnabled = YES;
    self.view.multipleTouchEnabled = YES;
    auto* bar = [[UIStackView alloc] initWithArrangedSubviews:@[_library, _run, _logToggle, _map]];
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

- (CAMetalLayer*)metalLayer { return _layer; }

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
    if (_session) { _session->stop(); _session->join(); _session.reset(); }
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
    config.fill_surface = true;
    _guestSurface.store(0);
    (void)_input.drain();
    config.input = &_input;
    config.interactive = true;
    config.render_on_demand = true;
    config.perf_log = true;  // the 10-second frame report also lists block cache coverage and the busiest imports
    {
        std::string why;
        if (auto bindings = atlan::shell::load_input_bindings(self.inputBindingsPath.UTF8String, why)) {
            config.input_bindings = std::move(*bindings);
            [self append:config.input_bindings.empty() ? @"input bindings missing" : @"input bindings ok"];
        } else {
            [self append:[NSString stringWithFormat:@"input bindings rejected: %s", why.c_str()]];
        }
    }
    [self startLocation];
    // A CAMetalLayer sublayer sized in native pixels; GL output is presented there.
    {
        const auto size = _surface.get();
        [_layer removeFromSuperlayer];  // the previous run's layer
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.drawableSize = CGSizeMake(size.width, size.height);
        layer.contentsGravity = kCAGravityResize;
        _metalView.backgroundColor = UIColor.blackColor;
        [_metalView.layer addSublayer:layer];
        _layer = layer;
        [self layoutSurface];
        _gl = std::make_unique<atlan::MetalGlBackend>((__bridge void*)layer);
        _gl->set_log([weakSelf](const std::string& line) {
            NSString* text = [NSString stringWithFormat:@"gl %s", line.c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf append:text]; });
        });
        config.gl_backend = _gl.get();
        // The guest sizes its GL surface itself (MainActivity.setSurfaces); render at exactly that size and show
        // it in the aspect-fitting rectangle of the view.
        std::atomic<std::uint64_t>* guestSurface = &_guestSurface;  // outlives the session (joined in dealloc)
        config.on_set_surfaces = [weakSelf, guestSurface](std::int32_t width, std::int32_t height) {
            if (width > 0 && height > 0)
                guestSurface->store((std::uint64_t{static_cast<std::uint32_t>(width)} << 32) | static_cast<std::uint32_t>(height));
            dispatch_async(dispatch_get_main_queue(), ^{
                if (width > 0 && height > 0) {
                    [weakSelf metalLayer].drawableSize = CGSizeMake(width, height);
                    [weakSelf layoutSurface];
                }
            });
        };
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
