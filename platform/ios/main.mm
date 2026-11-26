// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <CoreLocation/CoreLocation.h>
#include "apple_audio_sink.hpp"
#include "apple_network.hpp"
#include "metal_gl_backend.hpp"
#import "adapters/atlan/ios/launcher_settings.h"
#import "adapters/atlan/ios/launcher_view_controller.h"
#import <QuartzCore/CAMetalLayer.h>
#include "adapters/atlan/atlan_adapter.hpp"
#include "atlan/demo.hpp"
#include "shell/app_package.hpp"
#include "adapters/atlan/data_status.hpp"
#include <string>
#include <cstdio>
#include "shell/input.hpp"
#include "shell/log_file.hpp"
#include "shell/render_config.hpp"
#include "shell/session.hpp"
#include <atomic>
#include <exception>
#include <memory>
#include <vector>

@interface ShellViewController : UIViewController <CLLocationManagerDelegate, LauncherDelegate>
@property(nonatomic, readonly) BOOL running;  // a guest session is running
@end

@implementation ShellViewController {
    std::unique_ptr<atlan::shell::GuestSession> _session;
    atlan::AppleAudioSink _audio;
    atlan::shell::SurfaceState _surface;
    UIView* _metalView;
    std::unique_ptr<atlan::MetalGlBackend> _gl;
    CAMetalLayer* _layer;
    atlan::shell::InputQueue _input;
    std::atomic<std::uint64_t> _guestSurface;  // (width << 32) | height from the guest's setSurfaces; 0 until reported
    std::vector<UITouch*> _fingers;            // slot index = pointer id; nil when free
    CLLocationManager* _location;
    int _locationSource;                       // 0 not started, -1 starting, 1 the device (CoreLocation), 2 the pinned position
    NSTimer* _pinTimer;                        // repeats the pinned position like a GPS receiver while it is used
    NSString* _pinnedAt;                       // the pinned position last logged
    atlan::shell::SessionLogFile _logFile;
    NSString* _package;                        // the app package in the ATLAN folder
    NSString* _mapRoot;                        // the ATLAN folder when it is not Documents/ATLAN, else nil
    NSString* _stateRoot;                      // where that data set's NAND state is kept; nil: Documents/ATLAN/NAND
    BOOL _launched;                            // the first screen has been chosen
    LauncherViewController* _launcher;
    NSTimer* _watch;                           // brings the launcher back when a run ends by itself
    BOOL _landscape;                           // locked to landscape: the navigation runs or is about to
    BOOL _startPending;                        // start the navigation once the screen has turned to landscape
}

// Bindings from host input to the guest's native methods: Documents/input.cfg (format in shell/input.hpp).
- (NSString*)inputBindingsPath {
    return [self.documentsPath stringByAppendingPathComponent:@"input.cfg"];
}

- (atlan::shell::PixelSize)touchSurface {
    const auto guest = _guestSurface.load();
    if (guest != 0) return {static_cast<std::uint32_t>(guest >> 32), static_cast<std::uint32_t>(guest)};
    return _surface.get();
}

// On an iPhone with a notch or Dynamic Island, the strip of the display it sits in, as an inset on that one side.
// In landscape the safe area reports the same inset on both sides; the notch is at the device's top, which is on
// the left in landscape-right and on the right in landscape-left. Elsewhere (iPad, iPhones without a notch) zero.
- (UIEdgeInsets)notchInsets {
    if (!LauncherSettings.shared.notchInset) return UIEdgeInsetsZero;  // the user's choice in the launcher
    if (UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPhone) return UIEdgeInsetsZero;
    const UIEdgeInsets safe = self.view.safeAreaInsets;
    const CGFloat notch = MAX(safe.left, safe.right);
    if (!(notch > 0)) return UIEdgeInsetsZero;
    const UIInterfaceOrientation orientation = self.view.window.windowScene.interfaceOrientation;
    return orientation == UIInterfaceOrientationLandscapeLeft ? UIEdgeInsetsMake(0, 0, 0, notch)
                                                             : UIEdgeInsetsMake(0, notch, 0, 0);
}

// Where the guest surface is shown inside the metal view; touches map through the same rectangle.
- (atlan::shell::ViewRect)shownRect {
    // The guest's screen is stretched over the whole display whatever its size, as the app's Android layout does
    // on a phone; its UI is laid out for exactly that size. Only the notch's strip is left out (black), since the
    // notch would cover the guest's text there; the rounded corners and the home indicator are left as they are.
    const CGSize view = _metalView.bounds.size;
    const UIEdgeInsets notch = self.notchInsets;
    const CGFloat width = view.width - notch.left - notch.right;
    if (!(width > 0)) return {0, 0, view.width, view.height};
    return {notch.left, 0, width, view.height};
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
    [self startIfLandscape];
}

// The launcher turns with the device; the navigation itself is laid out for landscape only, so the app locks to
// landscape from the moment it starts (and the launcher reached through FINE DRIVE stays landscape).
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    if (_landscape) return UIInterfaceOrientationMaskLandscape;
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? UIInterfaceOrientationMaskAll
                                                                                : UIInterfaceOrientationMaskAllButUpsideDown;
}

- (void)setLandscape:(BOOL)landscape {
    _landscape = landscape;
    _launcher.landscapeOnly = landscape;
    if (@available(iOS 16.0, *)) {
        [self setNeedsUpdateOfSupportedInterfaceOrientations];
        if (!landscape) return;
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            auto* preferences = [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
            [(UIWindowScene*)scene requestGeometryUpdateWithPreferences:preferences errorHandler:nil];
        }
    } else {
        [UIViewController attemptRotationToDeviceOrientation];
    }
}

// The guest's surface is sized from the view when it starts, so the run waits until the view is landscape.
- (void)startInLandscape {
    [self setLandscape:YES];
    _startPending = YES;
    [self startIfLandscape];
}

- (void)startIfLandscape {
    if (!_startPending || self.presentedViewController || !self.view.window) return;
    const CGSize size = self.view.bounds.size;
    if (!(size.width > size.height)) return;
    _startPending = NO;
    __weak ShellViewController* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        ShellViewController* strongSelf = weakSelf;
        if (!strongSelf) return;
        [strongSelf runGuest];
        if (!strongSelf.running) [strongSelf showLauncher];
    });
}

- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    [self layoutSurface];
}

// Turning the phone over (landscape-left to landscape-right) keeps the size and the insets but moves the notch to
// the other side; lay the surface out again once the new orientation is in place.
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    __weak ShellViewController* weakSelf = self;
    [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> context) {
        (void)context;
        [weakSelf layoutSurface];
    }];
}

- (void)forward:(NSSet<UITouch*>*)touches phase:(atlan::shell::TouchPhase)phase {
    using atlan::shell::TouchPhase;
    const auto shown = self.shownRect;
    for (UITouch* touch in touches) {
        // Touches the launcher (presented over the guest) leaves unhandled travel up the responder chain to this
        // controller; only those that began on the guest's own view are the guest's.
        if (![touch.view isDescendantOfView:self.view]) continue;
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

// Where the navigation's position comes from: the device, or the developer setting's fixed position (launcher
// settings), switched live whenever the setting changes once the navigation has started.
- (void)startLocation {
    if (_locationSource == 0) _locationSource = -1;  // wanted, source not chosen yet
    [self applyLocationSource];
}
- (void)applyLocationSource {
    if (_locationSource == 0) return;
    LauncherSettings* settings = LauncherSettings.shared;
    if (settings.pinLocation) {
        if (_locationSource != 2) {
            [_location stopUpdatingLocation];
            __weak ShellViewController* weakSelf = self;
            _pinTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer*) { [weakSelf pushPinnedLocation]; }];
            _locationSource = 2;
        }
        NSString* where = [NSString stringWithFormat:@"%.6f, %.6f, %g", settings.pinnedLatitude, settings.pinnedLongitude,
                                                     settings.pinnedCourse];
        if (![where isEqualToString:_pinnedAt]) {
            _pinnedAt = where;
            [self note:[@"location pinned " stringByAppendingString:where]];
        }
        [self pushPinnedLocation];
        return;
    }
    if (_locationSource == 1) return;
    if (_locationSource == 2) [self note:@"location from the device"];
    [_pinTimer invalidate];
    _pinTimer = nil;
    _pinnedAt = nil;
    _locationSource = 1;
    if (!_location) {
        _location = [CLLocationManager new];
        _location.delegate = self;
        _location.desiredAccuracy = kCLLocationAccuracyBestForNavigation;
        _location.activityType = CLActivityTypeAutomotiveNavigation;
        _location.pausesLocationUpdatesAutomatically = NO;
    }
    const CLAuthorizationStatus status = _location.authorizationStatus;
    if (status == kCLAuthorizationStatusNotDetermined) [_location requestWhenInUseAuthorization];
    else if (status == kCLAuthorizationStatusDenied || status == kCLAuthorizationStatusRestricted) [self note:@"location denied"];
    else [_location startUpdatingLocation];
}
// The pinned position as a stationary fix: speed 0, the set heading, a good accuracy, the current time.
- (void)pushPinnedLocation {
    LauncherSettings* settings = LauncherSettings.shared;
    const auto now = static_cast<std::uint64_t>(NSDate.date.timeIntervalSince1970 * 1000.0);
    _input.push(atlan::shell::LocationFix{settings.pinnedLatitude, settings.pinnedLongitude, 0, 0, settings.pinnedCourse, 5, now});
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)manager {
    if (_locationSource != 1) return;  // the pinned position is in use
    const CLAuthorizationStatus status = manager.authorizationStatus;
    if (status == kCLAuthorizationStatusAuthorizedWhenInUse || status == kCLAuthorizationStatusAuthorizedAlways)
        [manager startUpdatingLocation];
    else if (status != kCLAuthorizationStatusNotDetermined) [self note:@"location denied"];
}
- (void)locationManager:(CLLocationManager*)manager didUpdateLocations:(NSArray<CLLocation*>*)locations {
    (void)manager;
    CLLocation* fix = locations.lastObject;
    if (_locationSource != 1 || !fix || fix.horizontalAccuracy < 0) return;  // an invalid fix carries no position
    _input.push(atlan::shell::LocationFix{fix.coordinate.latitude, fix.coordinate.longitude, fix.altitude, fix.speed,
                                          fix.course, fix.horizontalAccuracy,
                                          static_cast<std::uint64_t>(fix.timestamp.timeIntervalSince1970 * 1000.0)});
}
- (void)locationManager:(CLLocationManager*)manager didFailWithError:(NSError*)error {
    (void)manager;
    if (error.code != kCLErrorLocationUnknown) [self note:[NSString stringWithFormat:@"location failed: %@", error.localizedDescription]];
}

- (NSString*)documentsPath {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject.path;
}

// The app as installed on Android: ATLAN/APP/atlan.dat or an APK in the ATLAN folder (shell/app_package.hpp), in
// the app's Documents or in a folder the user picked in the launcher.
- (void)findPackage {
    LauncherSettings* settings = LauncherSettings.shared;
    NSString* folder = settings.atlanFolder;
    // Documents/ATLAN is where the app looks by itself; another data set beside it (ATLAN_RTA, ...) or a folder
    // picked elsewhere is mounted in its place. A data set in Documents keeps its own NAND state inside it.
    const BOOL standard = !settings.atlanFolderIsPicked &&
                          [folder.lastPathComponent caseInsensitiveCompare:@"ATLAN"] == NSOrderedSame &&
                          [folder.stringByDeletingLastPathComponent isEqualToString:self.documentsPath];
    _mapRoot = folder.length && !standard ? folder : nil;
    _stateRoot = _mapRoot && !settings.atlanFolderIsPicked ? [folder stringByAppendingPathComponent:@"NAND"] : nil;
    const std::string path = _mapRoot ? atlan::shell::find_package_in(_mapRoot.UTF8String)
                                      : atlan::shell::find_package(self.documentsPath.UTF8String);
    _package = path.empty() ? nil : [NSString stringWithUTF8String:path.c_str()];
}

- (BOOL)running {
    return _session && _session->state() == atlan::shell::SessionState::running;
}

// The first screen: the navigation itself when the user turned auto start on and the ATLAN folder is complete,
// otherwise the launcher (which also explains what is missing).
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (_launched) return;
    _launched = YES;
    if (!LauncherSettings.shared.autoStart) { [self showLauncher]; return; }
    NSString* folder = LauncherSettings.shared.atlanFolder;
    __weak ShellViewController* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const bool ready = atlan::shell::inspect_atlan_data(folder.UTF8String ?: "").ready();
        dispatch_async(dispatch_get_main_queue(), ^{
            ShellViewController* strongSelf = weakSelf;
            if (!strongSelf) return;
            if (ready) [strongSelf startInLandscape];
            else [strongSelf showLauncher];
        });
    });
}

- (void)launcherWantsNavigation:(LauncherViewController*)launcher {
    (void)launcher;
    if (self.running) { [self dismissViewControllerAnimated:YES completion:nil]; return; }
    [self startInLandscape];
    __weak ShellViewController* weakSelf = self;
    [self dismissViewControllerAnimated:YES completion:^{ [weakSelf startIfLandscape]; }];
}

- (void)settingsChanged {
    [self layoutSurface];  // the notch inset may have been switched
    atlan::AppleAudioSink::set_duck_others(LauncherSettings.shared.duckOthers);
    [self applyLocationSource];  // the developer setting's fixed position, switched on or off or moved
}

// A line of the shell itself, kept in the session log file.
- (void)note:(NSString*)line {
    _logFile.write(line.UTF8String ?: "?");
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    _metalView = [UIView new];
    _metalView.layer.opaque = YES;
    _metalView.multipleTouchEnabled = YES;
    _metalView.backgroundColor = UIColor.blackColor;
    self.view.multipleTouchEnabled = YES;
    [self.view insertSubview:_metalView atIndex:0];
    _metalView.frame = self.view.bounds;
    _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    // Every log line of a run, unfiltered, goes to Documents/logs/session-<time>.log (the newest five are kept).
    if (LauncherSettings.shared.sessionLog) _logFile.open([self.documentsPath stringByAppendingPathComponent:@"logs"].UTF8String);
    try {
        const auto demo = atlan::run_demo();
        [self note:[NSString stringWithFormat:@"selftest %u", demo.value]];
    } catch (const std::exception&) {
        [self note:@"selftest failed"];
    }
    atlan::AppleAudioSink::set_duck_others(LauncherSettings.shared.duckOthers);
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(settingsChanged)
        name:LauncherSettingsDidChange object:nil];
}

- (CAMetalLayer*)metalLayer { return _layer; }

// The launcher over the guest, which keeps running underneath (the guest's home button, a finished run, or the
// first screen of the app).
- (void)showLauncher {
    if (!_launcher) _launcher = [[LauncherViewController alloc] initWithDelegate:self];
    _launcher.running = self.running;
    [self setLandscape:self.running];
    if (self.presentedViewController) return;
    [self presentViewController:_launcher animated:_launched && self.running completion:nil];
}

- (void)watchSession {
    if (self.running || !_session) return;
    [_watch invalidate];
    _watch = nil;
    [self showLauncher];
}

- (void)runGuest {
    if (_session) { _session->stop(); _session->join(); _session.reset(); }
    [self findPackage];
    NSString* library = _package;
    if (!library) { [self note:@"library missing"]; return; }
    [self note:library.lastPathComponent];
    __weak ShellViewController* weakSelf = self;
    atlan::shell::SessionConfig config;
    atlan::shell::AtlanAdapter::Config app;  // the app's Java side: network and the guest's surface size
    config.library_path = library.UTF8String;
    config.audio_sink = &_audio;
    config.data_root = self.documentsPath.UTF8String;
    const auto display = self.shownRect;  // the display less the notch's strip
    _surface.set(atlan::shell::render_size(display.width, display.height,
                                           self.view.window.screen.nativeScale ?: UIScreen.mainScreen.nativeScale));
    config.surface = &_surface;
    // The guest's own quit (its exit dialog): leave like the Android app does. The session thread has finished its
    // work by then; a short delay lets the last log line and sound out.
    config.on_guest_exit = [] {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ exit(0); });
    };
    config.frame_interval_ms = 50;  // 20 frames per second at most, for heat
    _guestSurface.store(0);
    (void)_input.drain();
    config.input = &_input;
    config.interactive = true;
    config.render_on_demand = true;
    LauncherSettings* settings = LauncherSettings.shared;
    const bool perfLog = settings.perfLog, networkLog = settings.networkLog;
    config.perf_log = perfLog;  // the 10-second frame report also lists block cache coverage and the busiest imports
    if (_mapRoot) app.map_root = _mapRoot.UTF8String;
    if (_stateRoot) app.state_root = _stateRoot.UTF8String;
    app.http = atlan::apple_http_fetch();
    app.network_available = atlan::apple_network_available();
    // Optional byte signatures for run-time library functions the guest image does not export.
    config.native_signatures = [self.documentsPath stringByAppendingPathComponent:@"native.sig"].UTF8String;
    if (settings.profiler) {
        // The per-function profiler (launcher settings) writes its files to Documents/profile.
        NSString* profileFolder = [self.documentsPath stringByAppendingPathComponent:@"profile"];
        [NSFileManager.defaultManager createDirectoryAtPath:profileFolder withIntermediateDirectories:YES attributes:nil error:nil];
        config.profile_path = [profileFolder stringByAppendingPathComponent:@"p"].UTF8String;
    }
    {
        std::string why;
        if (auto bindings = atlan::shell::load_input_bindings(self.inputBindingsPath.UTF8String, why)) {
            config.input_bindings = std::move(*bindings);
            [self note:config.input_bindings.empty() ? @"input bindings missing" : @"input bindings ok"];
        } else {
            [self note:[NSString stringWithFormat:@"input bindings rejected: %s", why.c_str()]];
        }
    }
    [self startLocation];
    const std::string perfPath = [self.documentsPath stringByAppendingPathComponent:@"perf.log"].UTF8String;
    std::remove(perfPath.c_str());  // one run per file
    const std::string netPath = [self.documentsPath stringByAppendingPathComponent:@"net.log"].UTF8String;
    std::remove(netPath.c_str());
    // A CAMetalLayer sublayer sized in native pixels; GL output is presented there.
    {
        const auto size = _surface.get();
        [_layer removeFromSuperlayer];  // the previous run's layer
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.drawableSize = CGSizeMake(size.width, size.height);
        layer.contentsGravity = kCAGravityResize;
        [_metalView.layer addSublayer:layer];
        _layer = layer;
        [self layoutSurface];
        _gl = std::make_unique<atlan::MetalGlBackend>((__bridge void*)layer);
        atlan::shell::SessionLogFile* logFile = &_logFile;  // outlives the backend and the session
        _gl->set_log([logFile](const std::string& line) { logFile->write("gl " + line); });
        config.gl_backend = _gl.get();
        // The guest sizes its GL surface itself (MainActivity.setSurfaces); render at exactly that size and show
        // it in the aspect-fitting rectangle of the view.
        std::atomic<std::uint64_t>* guestSurface = &_guestSurface;  // outlives the session (joined in dealloc)
        app.on_set_surfaces = [weakSelf, guestSurface](std::int32_t width, std::int32_t height) {
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
    app.on_home = [weakSelf] {
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf showLauncher]; });
    };
    config.adapter = std::make_shared<atlan::shell::AtlanAdapter>(std::move(app));
    _session = std::make_unique<atlan::shell::GuestSession>(
        config,
        [perfPath, netPath, perfLog, networkLog, logFile = &_logFile](const std::string& line) {
            logFile->write(line);
            // Performance lines are also kept in Documents/perf.log and network lines in Documents/net.log, which the
            // Files app can open and share, when the launcher's developer settings ask for them.
            const bool network = line.starts_with("net ") || line.starts_with("http ") || line.starts_with("java online") ||
                                 line.starts_with("java MainActivity.IsConnected");
            if ((perfLog && line.starts_with("perf ")) || (networkLog && network)) {
                if (std::FILE* file = std::fopen((network ? netPath : perfPath).c_str(), "a")) {
                    std::fputs(line.c_str(), file);
                    std::fputc('\n', file);
                    std::fclose(file);
                }
            }
        });
    if (!_session->start()) { [self note:@"thread start failed"]; return; }
    UIApplication.sharedApplication.idleTimerDisabled = YES;  // a navigation screen stays on without touches
    [_watch invalidate];
    _watch = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer*) { [weakSelf watchSession]; }];
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
    [LauncherSettings.shared applyAppearanceTo:self.window];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(RuntimeAppDelegate.class));
    }
}
