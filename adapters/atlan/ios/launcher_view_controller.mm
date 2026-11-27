// SPDX-License-Identifier: GPL-2.0-or-later
#import "launcher_view_controller.h"
#import "launcher_kit.h"
#import "launcher_settings.h"
#import "launcher_style.h"
#import "launcher_text.h"
#import "launcher_verified.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "adapters/atlan/data_status.hpp"
#include "shell/coordinate.hpp"
#include <cmath>
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace text = launcher_text;
using atlan::shell::AtlanDataStatus;

namespace {
enum class Screen { start, settings };
enum class Tab { general, map, versions, developer };

NSString* ns(const std::string& value) {
    return value.empty() ? text::unknown : ([NSString stringWithUTF8String:value.c_str()] ?: text::unknown);
}

// Several data dates shown as one value: one date when they agree, else each distinct one.
NSString* dates(std::initializer_list<const std::string*> values) {
    std::vector<std::string> distinct;
    for (const auto* value : values)
        if (!value->empty() && std::find(distinct.begin(), distinct.end(), *value) == distinct.end()) distinct.push_back(*value);
    if (distinct.empty()) return text::unknown;
    std::string joined;
    for (const auto& value : distinct) joined += (joined.empty() ? "" : " / ") + value;
    return ns(joined);
}

// The developer setting's fixed position and heading, as the fields show them.
NSString* pinnedCoordinate() {
    LauncherSettings* settings = LauncherSettings.shared;
    return [NSString stringWithFormat:@"%.6f, %.6f", settings.pinnedLatitude, settings.pinnedLongitude];
}
NSString* pinnedCourse() {
    return [NSString stringWithFormat:@"%g°", LauncherSettings.shared.pinnedCourse];
}

NSString* launcherVersion() {
    return [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"";
}

// The headline of the folder check: ready, or what is missing.
NSString* statusTitle(const AtlanDataStatus& status) {
    if (!status.found) return text::folder_missing;
    NSMutableArray<NSString*>* missing = [NSMutableArray array];
    for (const auto& folder : status.folders)
        if (!folder.present) [missing addObject:ns(std::string(folder.name))];
    if (missing.count) return [NSString stringWithFormat:text::folders_missing, [missing componentsJoinedByString:@" · "]];
    if (!status.package) return text::package_missing;
    return text::ready;
}

std::string fingerprint(const AtlanDataStatus& s) {
    std::string out = std::to_string(s.found) + std::to_string(s.package);
    for (const auto& folder : s.folders) out += std::to_string(folder.present);
    for (const auto* value : {&s.app_name, &s.app_version, &s.map, &s.route, &s.search, &s.safety_main, &s.safety_sub})
        out += "|" + *value;
    return out;
}
}

@interface LauncherViewController () <UIDocumentPickerDelegate>
@end

@implementation LauncherViewController {
    __weak id<LauncherDelegate> _delegate;
    UIView* _frame;      // where screens go: the safe area on iPhone, a centred box on iPad
    UIView* _screen;     // the current screen's root view
    Screen _shown;
    Tab _tab;
    AtlanDataStatus _status;
    std::string _digest;  // SHA-256 of the chosen folder's native library; empty without a readable package
    std::vector<std::pair<std::string, AtlanDataStatus>> _choices;  // every ATLAN folder in Documents
    std::size_t _pending;  // the folder marked on the first-run choice screen
    std::string _fingerprint;
    BOOL _known;         // a check has finished
    BOOL _checking;
    NSTimer* _timer;
    dispatch_queue_t _queue;
    BOOL _shownPortrait;  // the orientation the current screen was built for
    UIView* _introButtons;  // upright: the intro's buttons, placed at the bottom by intro:content:
    NSLayoutConstraint* _top;  // iPhone: space above the content
    NSLayoutConstraint* _boxWidth;   // iPad: the centred box, sized per orientation
    NSLayoutConstraint* _boxHeight;
}

- (instancetype)initWithDelegate:(id<LauncherDelegate>)delegate {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _delegate = delegate;
        _queue = dispatch_queue_create("launcher.check", DISPATCH_QUEUE_SERIAL);
        self.modalPresentationStyle = UIModalPresentationFullScreen;
        self.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    }
    return self;
}

- (void)setLandscapeOnly:(BOOL)landscapeOnly {
    if (_landscapeOnly == landscapeOnly) return;
    _landscapeOnly = landscapeOnly;
    if (@available(iOS 16.0, *)) [self setNeedsUpdateOfSupportedInterfaceOrientations];
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    if (_landscapeOnly) return UIInterfaceOrientationMaskLandscape;
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? UIInterfaceOrientationMaskAll
                                                                                : UIInterfaceOrientationMaskAllButUpsideDown;
}

// Touches no control took end here: the next responder of a presented controller is the one that presented it,
// which would hand them to the navigation underneath.
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; (void)event; }
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; (void)event; }
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; (void)event; }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { (void)touches; (void)event; }

- (BOOL)portrait {
    const CGSize size = self.view.bounds.size;
    return size.height > size.width;
}

// A turn of the device rebuilds the screen for the new shape.
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    const BOOL portrait = self.portrait;
    if (_boxWidth) {
        const auto& m = LauncherMetrics::current();
        const CGSize box = portrait ? m.portraitContainer : m.container;
        if (_boxWidth.constant != box.width || _boxHeight.constant != box.height) {
            _boxWidth.constant = box.width;
            _boxHeight.constant = box.height;
        }
    }
    _top.constant = portrait ? 0 : 16;  // upright the status bar's safe area already leaves room
    if (portrait != _shownPortrait) [self render];
}

- (void)setStopped:(BOOL)stopped {
    if (_stopped == stopped) return;
    _stopped = stopped;
    if (self.isViewLoaded) [self render];
}

- (void)setRunning:(BOOL)running {
    if (_running == running) return;
    _running = running;
    if (self.isViewLoaded) [self render];
}

#pragma mark Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = LauncherColor.bg;
    _frame = [UIView new];
    _frame.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:_frame];
    const auto& m = LauncherMetrics::current();
    UIView* view = self.view;
    UILayoutGuide* safe = view.safeAreaLayoutGuide;
    if (m.container.width > 0) {
        NSLayoutConstraint* width = [_frame.widthAnchor constraintEqualToConstant:m.container.width];
        NSLayoutConstraint* height = [_frame.heightAnchor constraintEqualToConstant:m.container.height];
        width.priority = height.priority = UILayoutPriorityDefaultHigh;
        _boxWidth = width;
        _boxHeight = height;
        [NSLayoutConstraint activateConstraints:@[
            [_frame.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
            [_frame.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
            [_frame.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-48],
            [_frame.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor constant:-48],
            width, height,
        ]];
    } else {
        // 16 at the top and 24 at the bottom as designed; at the sides the notch's safe area plus a little (at least
        // 20 when the phone stands upright).
        NSLayoutConstraint* bottom = [_frame.bottomAnchor constraintEqualToAnchor:view.bottomAnchor constant:-24];
        bottom.priority = UILayoutPriorityDefaultHigh;
        NSLayoutConstraint* leading = [_frame.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12];
        NSLayoutConstraint* trailing = [_frame.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12];
        leading.priority = trailing.priority = UILayoutPriorityDefaultHigh;
        [NSLayoutConstraint activateConstraints:@[
            _top = [_frame.topAnchor constraintEqualToAnchor:safe.topAnchor constant:16],
            [_frame.bottomAnchor constraintLessThanOrEqualToAnchor:view.bottomAnchor constant:-24],
            [_frame.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor],
            [_frame.leadingAnchor constraintGreaterThanOrEqualToAnchor:view.leadingAnchor constant:20],
            [_frame.trailingAnchor constraintLessThanOrEqualToAnchor:view.trailingAnchor constant:-20],
            [_frame.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor],
            [_frame.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor],
            bottom, leading, trailing,
        ]];
    }
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(check)
                                               name:UIApplicationDidBecomeActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(check)
                                               name:LauncherSettingsDidChange object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(listChanged)
                                               name:VerifiedListDidChange object:nil];
    (void)VerifiedListStore.shared;  // fetches the list when first used
    [self render];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self check];
    __weak LauncherViewController* weakSelf = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:4 repeats:YES block:^(NSTimer*) { [weakSelf check]; }];
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate];
    _timer = nil;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_timer invalidate];
}

#pragma mark Folder check

// Reads the folder off the main thread (a picked folder may be on a slow provider); redraws only on a change.
- (void)check {
    if (_checking) return;
    _checking = YES;
    NSString* folder = LauncherSettings.shared.atlanFolder;
    NSArray<NSString*>* folders = LauncherSettings.shared.documentsFolders;
    __weak LauncherViewController* weakSelf = self;
    dispatch_async(_queue, ^{
        auto status = std::make_shared<AtlanDataStatus>(atlan::shell::inspect_atlan_data(folder.UTF8String ?: ""));
        auto digest = std::make_shared<std::string>(atlan::shell::package_library_sha256(folder.UTF8String ?: ""));
        auto choices = std::make_shared<std::vector<std::pair<std::string, AtlanDataStatus>>>();
        for (NSString* each in folders)
            choices->emplace_back(each.UTF8String ?: "", atlan::shell::inspect_atlan_data(each.UTF8String ?: ""));
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf checked:*status digest:*digest choices:*choices folder:folder]; });
    });
}

- (void)checked:(const AtlanDataStatus&)status digest:(const std::string&)digest
        choices:(const std::vector<std::pair<std::string, AtlanDataStatus>>&)choices folder:(NSString*)folder {
    _checking = NO;
    std::string print = fingerprint(status) + digest + (LauncherSettings.shared.atlanFolderIsPicked ? "p" : "d") + (folder.UTF8String ?: "");
    for (const auto& [path, each] : choices) print += "\n" + path + fingerprint(each);
    if (_known && print == _fingerprint) return;
    _known = YES;
    _status = status;
    _digest = digest;
    _choices = choices;
    _fingerprint = print;
    if (status.ready() && !LauncherSettings.shared.setupDone) LauncherSettings.shared.setupDone = YES;
    [self render];
}

- (void)listChanged {
    if (_known) [self render];
}

- (void)pickFolder {
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder]];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    if (urls.firstObject) [LauncherSettings.shared usePickedFolder:urls.firstObject];
}

#pragma mark Screens

- (void)render {
    if (!_frame) return;
    [_screen removeFromSuperview];
    _shownPortrait = self.portrait;
    _screen = _known ? [self buildScreen] : [UIView new];
    _screen.translatesAutoresizingMaskIntoConstraints = NO;
    [_frame addSubview:_screen];
    [NSLayoutConstraint activateConstraints:@[
        [_screen.topAnchor constraintEqualToAnchor:_frame.topAnchor],
        [_screen.leadingAnchor constraintEqualToAnchor:_frame.leadingAnchor],
        [_screen.trailingAnchor constraintEqualToAnchor:_frame.trailingAnchor],
        [_screen.bottomAnchor constraintEqualToAnchor:_frame.bottomAnchor],
    ]];
}

- (UIView*)buildScreen {
    LauncherSettings* settings = LauncherSettings.shared;
    if (_shown == Screen::settings) return [self settingsScreen];
    if (_stopped) return [self startScreen];
    if (!_running && settings.needsFolderChoice) return [self chooseScreen];
    if (!_running && !settings.setupDone && !_status.ready()) return [self welcomeScreen];
    if (!_running && _status.ready() && !settings.autoStartAsked) return [self autoStartScreen];
    return [self startScreen];
}

- (void)openSettings:(Tab)tab {
    _shown = Screen::settings;
    _tab = tab;
    [self render];
}

- (UIView*)nameLine {
    const auto& m = LauncherMetrics::current();
    // The trailing spacer keeps the version beside the name when the line is given the full width.
    UIStackView* line = LKRow(@[LKLabel(text::app_name, m.head, UIFontWeightSemibold, LauncherColor.ink),
                                LKLabel(launcherVersion(), m.caption, UIFontWeightRegular, LauncherColor.muted), LKSpacer()], 8);
    line.alignment = UIStackViewAlignmentFirstBaseline;
    return line;
}

- (UIView*)titleBlock:(NSString*)title body:(NSString*)body {
    const auto& m = LauncherMetrics::current();
    return LKColumn(@[LKLabel(title, m.title, UIFontWeightMedium, LauncherColor.ink),
                      LKLabel(body, m.body, UIFontWeightRegular, LauncherColor.ink2)], 8);
}

// Left column of the first-run screens: name and settings at the top, the question and its buttons at the bottom.
- (UIView*)introColumn:(UIView*)block buttons:(NSArray<UIButton*>*)buttons {
    __weak LauncherViewController* weakSelf = self;
    UIButton* gear = LKIconButton(@"slider.horizontal.3", text::settings, ^{ [weakSelf openSettings:Tab::general]; });
    UIStackView* top = LKRow(@[[self nameLine], LKSpacer(), gear], 8);
    for (UIButton* button in buttons) [button setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [buttons.firstObject setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* row = LKRow(buttons, 8);
    row.alignment = UIStackViewAlignmentFill;
    if (self.portrait) {
        // Upright the buttons go to the bottom of the screen, under the screen's content (see intro:content:).
        _introButtons = row;
        return LKColumn(@[top, block], 20);
    }
    return LKColumn(@[top, LKSpacer(), LKColumn(@[block, row], 16)], 0);
}

// A first-run screen: the intro column beside its content, or upright the intro, the content and the buttons at
// the bottom.
- (UIView*)intro:(UIView*)left content:(UIView*)content {
    const auto& m = LauncherMetrics::current();
    if (!self.portrait) return [self pair:left width:m.introColumn with:content width:0 spacing:24 grow:content];
    UIStackView* column = LKColumn(@[left, content, LKSpacer(), _introButtons ?: [UIView new]], 20);
    [column setCustomSpacing:16 afterView:column.arrangedSubviews[2]];
    _introButtons = nil;
    return column;
}

- (UIView*)twoColumns:(UIView*)left right:(UIView*)right spacing:(CGFloat)spacing {
    UIStackView* row = LKRow(@[left, right], spacing);
    row.alignment = UIStackViewAlignmentFill;
    return row;
}

// Two parts side by side in landscape, a fixed width (> 0) on either, or one above the other in portrait, where
// `grow` takes the spare height.
- (UIView*)pair:(UIView*)first width:(CGFloat)firstWidth with:(UIView*)second width:(CGFloat)secondWidth
        spacing:(CGFloat)spacing grow:(UIView*)grow {
    if (self.portrait) {
        UIView* other = grow == first ? second : first;
        [grow setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisVertical];
        [other setContentHuggingPriority:UILayoutPriorityDefaultHigh + 1 forAxis:UILayoutConstraintAxisVertical];
        [other setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
        return LKColumn(@[first, second], spacing);
    }
    if (firstWidth > 0) LKSize(first, firstWidth, 0);
    if (secondWidth > 0) LKSize(second, secondWidth, 0);
    return [self twoColumns:first right:second spacing:spacing];
}

// Two cards side by side in landscape; in portrait one under the other in a scroll view.
- (UIView*)cards:(UIView*)first with:(UIView*)second secondWidth:(CGFloat)width {
    const auto& m = LauncherMetrics::current();
    if (self.portrait) return LKScroll(LKColumn(@[first, second], m.gap));
    LKSize(second, width, 0);
    return [self twoColumns:first right:second spacing:m.gap];
}

- (UIView*)welcomeScreen {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    NSString* device = UIDevice.currentDevice.model;
    UIView* left = [self introColumn:[self titleBlock:text::welcome_title body:text::welcome_body] buttons:@[
        LKButton(text::pick_folder, @"folder", LKButtonPrimary, m.button, ^{ [weakSelf pickFolder]; }),
        LKButton(text::recheck, nil, LKButtonSecondary, m.button, ^{ [weakSelf check]; }),
    ]];
    LKCard* ways = LKGroup(@[
        [self padded:LKCaptionBlock([NSString stringWithFormat:text::welcome_way1, device],
                                    [NSString stringWithFormat:text::welcome_way1_body, device])],
        [self padded:LKCaptionBlock(text::welcome_way2, text::welcome_way2_body)],
    ]);
    NSMutableArray<UIView*>* cells = [NSMutableArray array];
    for (const auto& folder : _status.folders)
        [cells addObject:LKColumn(@[LKLabel(ns(std::string(folder.name)), m.body, UIFontWeightMedium, LauncherColor.ink),
                                    LKLabel(folder.present ? text::present : text::absent, m.caption, UIFontWeightRegular, LauncherColor.muted)], 0)];
    UIStackView* grid = LKRow(cells, 12);
    grid.distribution = UIStackViewDistributionFillEqually;
    grid.alignment = UIStackViewAlignmentTop;
    UIView* layout = [[LKCard alloc] initWithContent:LKColumn(@[
        LKCaptionBlock(text::welcome_layout, text::welcome_layout_body), LKSpacer(), grid, LKSpacer(),
        LKLabel(text::welcome_package, m.caption, UIFontWeightRegular, LauncherColor.muted)], 8)
                                              padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, m.cardPadding, m.cardPadding)
                                               radius:m.cardRadius];
    UIView* right = LKColumn(@[ways, layout], m.gap);
    return [self intro:left content:right];
}

- (UIView*)autoStartScreen {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    UIView* left = [self introColumn:[self titleBlock:text::auto_title body:text::auto_body] buttons:@[
        LKButton(text::auto_yes, nil, LKButtonPrimary, m.button, ^{ [weakSelf answerAutoStart:YES]; }),
        LKButton(text::auto_no, nil, LKButtonSecondary, m.button, ^{ [weakSelf answerAutoStart:NO]; }),
    ]];
    LKCard* notes = LKGroup(@[
        [self padded:LKCaptionBlock(text::auto_return, text::auto_return_body)],
        [self padded:LKCaptionBlock(text::auto_later, text::auto_later_body)],
    ]);
    UIStackView* head = LKRow(@[LKLabel(text::map_data, m.caption, UIFontWeightRegular, LauncherColor.muted), LKSpacer(),
                                LKDot(LauncherColor.positive, 8),
                                LKLabel(text::ready, m.caption, UIFontWeightRegular, LauncherColor.ink2)], 8);
    NSMutableArray<UIView*>* cells = [NSMutableArray array];
    for (NSArray<NSString*>* pair in @[@[text::app_row, ns(_status.app_label())], @[text::map_row, ns(_status.map)],
                                       @[text::safety_row, ns(_status.safety_main)]])
        [cells addObject:LKColumn(@[LKLabel(pair[0], m.caption, UIFontWeightRegular, LauncherColor.muted),
                                    LKLabel(pair[1], m.body, UIFontWeightMedium, LauncherColor.ink)], 0)];
    UIStackView* grid = LKRow(cells, 12);
    grid.distribution = UIStackViewDistributionFillEqually;
    grid.alignment = UIStackViewAlignmentTop;
    UIView* data = [[LKCard alloc] initWithContent:LKColumn(@[head, LKSpacer(), grid], 8)
                                           padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, m.cardPadding, m.cardPadding)
                                            radius:m.cardRadius];
    UIView* right = LKColumn(@[notes, data], m.gap);
    return [self intro:left content:right];
}

- (void)answerAutoStart:(BOOL)on {
    LauncherSettings.shared.autoStart = on;
    LauncherSettings.shared.autoStartAsked = YES;
    [self render];
}

- (UIView*)padded:(UIView*)view {
    UIStackView* box = LKColumn(@[view], 0);
    box.layoutMarginsRelativeArrangement = YES;
    box.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(12, 0, 12, 0);
    return box;
}

// The three values the launcher shows for a data set, side by side under a caption each.
- (UIView*)valueGrid:(const AtlanDataStatus&)status {
    const auto& m = LauncherMetrics::current();
    NSMutableArray<UIView*>* cells = [NSMutableArray array];
    for (NSArray<NSString*>* pair in @[@[text::app_row, ns(status.app_label())], @[text::map_row, ns(status.map)],
                                       @[text::safety_row, ns(status.safety_main)]])
        [cells addObject:LKColumn(@[LKLabel(pair[0], m.caption, UIFontWeightRegular, LauncherColor.muted),
                                    LKLabel(pair[1], m.body, UIFontWeightMedium, LauncherColor.ink)], 0)];
    UIStackView* grid = LKRow(cells, 12);
    grid.distribution = UIStackViewDistributionFillEqually;
    grid.alignment = UIStackViewAlignmentTop;
    return grid;
}

// First run with several ATLAN folders in Documents: one card per folder, the first preselected.
- (UIView*)chooseScreen {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    if (_pending >= _choices.size()) _pending = 0;
    UIView* left = [self introColumn:[self titleBlock:text::choose_title
                                                 body:[NSString stringWithFormat:text::choose_body, (unsigned long)_choices.size()]]
                             buttons:@[LKButton(text::choose_use, nil, LKButtonPrimary, m.button, ^{ [weakSelf useChoice]; })]];
    NSMutableArray<UIView*>* cards = [NSMutableArray array];
    for (std::size_t index = 0; index < _choices.size(); ++index) {
        const auto& choice = _choices[index];
        NSString* name = ns(choice.first).lastPathComponent;
        UIView* title = LKRow(@[LKRadio(index == _pending),
                                LKColumn(@[LKLabel(name, m.value, UIFontWeightMedium, LauncherColor.ink),
                                           LKLabel([NSString stringWithFormat:text::location_in_documents, name], m.caption,
                                                   UIFontWeightRegular, LauncherColor.muted)], 0), LKSpacer()], 12);
        UIStackView* values = LKColumn(@[[self valueGrid:choice.second]], 0);
        values.layoutMarginsRelativeArrangement = YES;
        values.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(0, 32, 0, 0);
        LKCard* card = [[LKCard alloc] initWithContent:LKColumn(@[title, LKSpacer(), values], 8)
                                               padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, m.cardPadding, m.cardPadding)
                                                radius:m.cardRadius];
        card.selectedLook = index == _pending;
        card.accessibilityLabel = name;
        card.accessibilityTraits = UIAccessibilityTraitButton | (index == _pending ? UIAccessibilityTraitSelected : 0);
        [card addAction:[UIAction actionWithHandler:^(UIAction*) { [weakSelf setPending:index]; }]
       forControlEvents:UIControlEventTouchUpInside];
        [cards addObject:card];
    }
    UIStackView* list = LKColumn(cards, m.gap);
    list.distribution = UIStackViewDistributionFillEqually;
    // Two or three fill the column; more scroll.
    UIView* right = _choices.size() <= 3 ? list : LKScroll(list);
    return [self intro:left content:right];
}

- (void)setPending:(std::size_t)index {
    _pending = index;
    [self render];
}

- (void)useChoice {
    if (_pending < _choices.size()) [LauncherSettings.shared chooseDocumentsFolder:ns(_choices[_pending].first)];
}

- (UIView*)startScreen {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    const bool ready = _status.ready();
    const bool blocked = !_status.found;

    UIImageView* chevron = LKIcon(@"chevron.right", m.caption + 2, LauncherColor.muted);
    UIView* caption = LKRow(@[LKLabel(text::map_data, m.caption, UIFontWeightRegular, LauncherColor.muted), LKSpacer(), chevron], 8);
    UIView* headline = LKRow(@[LKDot(ready ? LauncherColor.positive : LauncherColor.negative, m.caption > 12 ? 12 : 8),
                               LKLabel(statusTitle(_status), m.title, UIFontWeightMedium, LauncherColor.ink)], m.caption > 12 ? 12 : 8);
    NSMutableArray<UIView*>* top = [NSMutableArray arrayWithObjects:caption, headline, nil];
    if (blocked) [top addObject:LKLabel(text::blocked_body, m.body, UIFontWeightRegular, LauncherColor.ink2)];
    UIStackView* topColumn = LKColumn(top, 8);
    [topColumn setCustomSpacing:4 afterView:headline];

    NSMutableArray<UIView*>* bottom = [NSMutableArray array];
    if (ready) {
        for (NSArray<NSString*>* pair in @[@[text::app_row, ns(_status.app_label())], @[text::map_row, ns(_status.map)],
                                           @[text::safety_row, ns(_status.safety_main)]])
            [bottom addObject:LKListRow(pair[0], nil, LKValue(pair[1]), m.row, YES, LauncherColor.ink2)];
    } else if (blocked) {
        UIButton* pick = LKButton(text::pick_map_folder, @"folder", LKButtonPrimary, m.iconButton, ^{ [weakSelf pickFolder]; });
        UIButtonConfiguration* config = pick.configuration;
        config.background.cornerRadius = 8;
        pick.configuration = config;
        [bottom addObject:pick];
    } else {
        for (const auto& folder : _status.folders) {
            const bool appWithoutPackage = folder.present && folder.name == "APP" && !_status.package;
            const bool problem = !folder.present || appWithoutPackage;
            NSString* value = !folder.present ? text::absent : appWithoutPackage ? text::no_package : text::present;
            UILabel* state = LKLabel(value, problem ? m.body : m.caption, problem ? UIFontWeightMedium : UIFontWeightRegular,
                                     problem ? LauncherColor.ink : LauncherColor.muted);
            UIView* row = LKListRow(ns(std::string(folder.name)), nil, state, m.compactRow, YES, problem ? LauncherColor.ink : LauncherColor.muted);
            [bottom addObject:row];
        }
    }
    UIStackView* cardContent = LKColumn(@[topColumn, LKSpacer(), LKColumn(bottom, 0)], 0);
    [cardContent setCustomSpacing:16 afterView:topColumn];  // at least this much above the rows
    LKCard* card = [[LKCard alloc] initWithContent:cardContent
                                           padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, blocked ? m.cardPadding : 4, m.cardPadding)
                                            radius:m.cardRadius];
    [card addAction:[UIAction actionWithHandler:^(UIAction*) { [weakSelf openSettings:Tab::map]; }]
   forControlEvents:UIControlEventTouchUpInside];
    LKStartButton* start = [[LKStartButton alloc] initWithTitle:_stopped ? text::reopen : _running ? text::resume : text::start];
    start.enabled = ready || _running || _stopped;
    [start addAction:[UIAction actionWithHandler:^(UIAction*) { [weakSelf startNavigation]; }]
    forControlEvents:UIControlEventTouchUpInside];
    UIButton* gear = LKButton(text::settings, @"slider.horizontal.3", LKButtonSecondary, m.settingsButton,
                              ^{ [weakSelf openSettings:Tab::general]; });
    if (self.portrait) {
        // Upright: the name, the folder card, then the start button taking the rest of the height (at least the
        // designed size) so the screen has no empty band; settings at the bottom. On iPad the block sits centred.
        if (m.container.width > 0) {
            [start.heightAnchor constraintEqualToConstant:m.portraitStart].active = YES;
            return [self centeredVertically:LKColumn(@[[self nameLine], LKColumn(@[card, start, gear], m.gap)], m.gap + 8)];
        }
        [start.heightAnchor constraintGreaterThanOrEqualToConstant:m.portraitStart].active = YES;
        [start setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisVertical];
        [card setContentHuggingPriority:UILayoutPriorityDefaultHigh + 1 forAxis:UILayoutConstraintAxisVertical];
        [card setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
        return LKColumn(@[[self nameLine], card, start, gear], m.gap);
    }
    UIStackView* left = LKColumn(@[[self nameLine], card], m.gap);
    UIStackView* right = LKColumn(@[start, gear], m.gap);
    LKSize(right, m.startColumn, 0);
    return [self twoColumns:left right:right spacing:24];
}

- (UIView*)centeredVertically:(UIView*)content {
    UIView* box = [UIView new];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [box addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
        [content.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
        [content.centerYAnchor constraintEqualToAnchor:box.centerYAnchor],
        [content.topAnchor constraintGreaterThanOrEqualToAnchor:box.topAnchor],
    ]];
    return box;
}

- (void)startNavigation {
    [_delegate launcherWantsNavigation:self];
}

#pragma mark Settings

- (UIView*)settingsScreen {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    UIButton* back = LKIconButton(@"chevron.left", text::back, ^{ [weakSelf closeSettings]; });
    UIStackView* header = LKRow(@[back, LKLabel(text::settings, m.head, UIFontWeightSemibold, LauncherColor.ink), LKSpacer()], 12);

    NSMutableArray<UIView*>* tabs = [NSMutableArray array];
    const std::pair<Tab, NSString*> names[] = {{Tab::general, text::tab_general}, {Tab::map, text::tab_map},
                                               {Tab::versions, text::tab_versions}, {Tab::developer, text::tab_developer}};
    for (const auto& [tab, name] : names) [tabs addObject:[self tabButton:name tab:tab]];
    UIView* pane = _tab == Tab::general ? [self generalPane]
                 : _tab == Tab::map     ? [self mapPane]
                 : _tab == Tab::versions ? [self versionsPane]
                                          : [self developerPane];
    if (self.portrait) {
        // Upright: the tabs in one row above the pane.
        UIStackView* tabRow = LKRow(tabs, 4);
        tabRow.distribution = UIStackViewDistributionFillEqually;
        tabRow.alignment = UIStackViewAlignmentFill;
        [pane setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisVertical];
        return LKColumn(@[header, tabRow, pane], m.gap);
    }
    [tabs addObject:LKSpacer()];
    UIStackView* tabColumn = LKColumn(tabs, 4);
    LKSize(tabColumn, m.tabColumn, 0);
    return LKColumn(@[header, [self twoColumns:tabColumn right:pane spacing:16]], m.gap);
}

- (void)closeSettings {
    _shown = Screen::start;
    [self render];
}

- (UIButton*)tabButton:(NSString*)name tab:(Tab)tab {
    const auto& m = LauncherMetrics::current();
    const bool on = tab == _tab;
    UIButtonConfiguration* config = UIButtonConfiguration.plainButtonConfiguration;
    const BOOL portrait = self.portrait;  // tabs share one row then, so they are smaller and centred
    config.attributedTitle = [[NSAttributedString alloc] initWithString:name attributes:@{
        NSFontAttributeName: LauncherFont(portrait ? m.body : m.value, on ? UIFontWeightMedium : UIFontWeightRegular)}];
    config.baseForegroundColor = on ? LauncherColor.ink : LauncherColor.ink2;
    config.contentInsets = portrait ? NSDirectionalEdgeInsetsMake(0, 4, 0, 4) : NSDirectionalEdgeInsetsMake(0, 12, 0, 12);
    config.titleLineBreakMode = NSLineBreakByTruncatingTail;
    config.background.cornerRadius = 8;
    config.background.backgroundColor = on ? LauncherColor.tabSelected : UIColor.clearColor;
    config.background.strokeColor = on ? LauncherColor.tabSelectedLine : UIColor.clearColor;
    config.background.strokeWidth = 1;
    __weak LauncherViewController* weakSelf = self;
    UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction*) {
        [weakSelf openSettings:tab];
    }]];
    button.contentHorizontalAlignment = portrait ? UIControlContentHorizontalAlignmentCenter : UIControlContentHorizontalAlignmentLeading;
    if (on) button.accessibilityTraits |= UIAccessibilityTraitSelected;
    LKSize(button, 0, m.iconButton);
    return button;
}

- (UIView*)sectionCaption:(NSString*)caption top:(CGFloat)top {
    const auto& m = LauncherMetrics::current();
    UIStackView* box = LKColumn(@[LKLabel(caption, m.caption, UIFontWeightRegular, LauncherColor.muted)], 0);
    box.layoutMarginsRelativeArrangement = YES;
    box.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(top, m.cardPadding, 4, 0);
    return box;
}

- (UIView*)generalPane {
    LauncherSettings* settings = LauncherSettings.shared;
    NSArray<NSString*>* modes = @[text::appearance_system, text::appearance_light, text::appearance_dark];
    void (^pickMode)(NSInteger) = ^(NSInteger index) { LauncherSettings.shared.appearance = (LauncherAppearance)index; };
    UIView* appearance = LKSegmentRow(text::appearance, text::appearance_detail, modes, settings.appearance, 60, pickMode);
    if (self.portrait) {
        // Upright the row is too narrow for text and choices side by side: the choices go under the text.
        UIStackView* stacked = LKColumn(@[LKListRow(text::appearance, text::appearance_detail, nil, 52, NO, nil),
                                          LKSegments(modes, settings.appearance, pickMode)], 4);
        stacked.layoutMarginsRelativeArrangement = YES;
        stacked.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(0, 0, 12, 0);
        appearance = stacked;
    }
    NSMutableArray<UIView*>* screenRows = [NSMutableArray arrayWithObject:appearance];
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone)
        [screenRows addObject:LKSwitchRow(text::notch, text::notch_detail, settings.notchInset, 60,
                                          ^(BOOL on) { LauncherSettings.shared.notchInset = on; })];
    return LKScroll(LKColumn(@[
        [self sectionCaption:text::section_start top:0],
        LKGroup(@[LKSwitchRow(text::auto_start, text::auto_start_detail, settings.autoStart, 72,
                              ^(BOOL on) { LauncherSettings.shared.autoStart = on; })]),
        [self sectionCaption:text::section_screen top:12],
        LKGroup(screenRows),
        [self sectionCaption:text::section_sound top:12],
        LKGroup(@[LKSwitchRow(text::duck, text::duck_detail, settings.duckOthers, 60,
                              ^(BOOL on) { LauncherSettings.shared.duckOthers = on; })]),
    ], 0));
}

- (UIView*)mapPane {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    LauncherSettings* settings = LauncherSettings.shared;
    NSString* active = settings.atlanFolder;
    NSString* where = settings.atlanFolderIsPicked ? (settings.pickedFolder.lastPathComponent ?: text::unknown)
                    : active.length ? [NSString stringWithFormat:text::location_in_documents, active.lastPathComponent]
                                    : text::unknown;
    NSMutableArray<UIView*>* buttons = [NSMutableArray array];
    if (settings.atlanFolderIsPicked)
        [buttons addObject:LKButton(text::use_documents, nil, LKButtonRaised, m.iconButton, ^{ [LauncherSettings.shared forgetPickedFolder]; })];
    [buttons addObject:LKButton(text::pick_other, @"folder", LKButtonRaised, m.iconButton, ^{ [weakSelf pickFolder]; })];
    UIView* place = LKColumn(@[LKLabel(text::location, m.caption, UIFontWeightRegular, LauncherColor.muted),
                               LKLabel(where, m.value, UIFontWeightMedium, LauncherColor.ink)], 0);
    [place setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* location = LKRow([@[place, LKSpacer()] arrayByAddingObjectsFromArray:buttons], 8);
    if (self.portrait) {
        UIStackView* buttonRow = LKRow(buttons, 8);
        buttonRow.distribution = UIStackViewDistributionFillEqually;
        location = LKColumn(@[place, buttonRow], 8);
        location.layoutMarginsRelativeArrangement = YES;
        location.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(12, 0, 12, 0);
    }
    [location.heightAnchor constraintGreaterThanOrEqualToConstant:60].active = YES;
    const bool ready = _status.ready();
    UIView* rows = LKColumn(@[
        location,
        LKListRow(text::status, nil, LKStatus(ready ? LauncherColor.positive : LauncherColor.negative, statusTitle(_status)), m.row, YES, nil),
        LKListRow(text::map_row, nil, LKValue(ns(_status.map)), m.row, YES, nil),
        LKListRow(text::safety_row, nil, LKValue(ns(_status.safety_main)), m.row, YES, nil),
    ], 0);
    UIView* content = LKColumn(@[rows, LKSpacer(), LKLabel(text::map_footnote, m.caption, UIFontWeightRegular, LauncherColor.muted)], 12);
    LKCard* card = [[LKCard alloc] initWithContent:content padding:UIEdgeInsetsMake(4, m.cardPadding, m.cardPadding, m.cardPadding) radius:m.cardRadius];
    if (_choices.size() < 2) return card;
    return [self multiFolderPane];
}

// Several ATLAN folders in Documents: pick one (used from the next start) and see the chosen one's state.
- (UIView*)multiFolderPane {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    LauncherSettings* settings = LauncherSettings.shared;
    NSString* active = settings.atlanFolder;
    NSMutableArray<UIView*>* options = [NSMutableArray arrayWithObject:LKLabel(text::atlan_folders, m.caption, UIFontWeightRegular, LauncherColor.muted)];
    for (const auto& choice : _choices) {
        NSString* path = ns(choice.first);
        const BOOL on = !settings.atlanFolderIsPicked && [path isEqualToString:active];
        NSString* detail = choice.second.ready() ? ns(choice.second.app_label()) : statusTitle(choice.second);
        UIButtonConfiguration* config = UIButtonConfiguration.plainButtonConfiguration;
        config.contentInsets = NSDirectionalEdgeInsetsZero;
        UIButton* option = [UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction*) {
            [LauncherSettings.shared chooseDocumentsFolder:path];
        }]];
        UIStackView* row = LKRow(@[LKRadio(on), LKColumn(@[LKLabel(path.lastPathComponent, m.value, UIFontWeightMedium, LauncherColor.ink),
                                                          LKLabel(detail, m.caption, UIFontWeightRegular, LauncherColor.muted)], 0),
                                   LKSpacer()], 12);
        row.userInteractionEnabled = NO;
        row.translatesAutoresizingMaskIntoConstraints = NO;
        [option addSubview:row];
        [NSLayoutConstraint activateConstraints:@[
            [row.leadingAnchor constraintEqualToAnchor:option.leadingAnchor], [row.trailingAnchor constraintEqualToAnchor:option.trailingAnchor],
            [row.centerYAnchor constraintEqualToAnchor:option.centerYAnchor], [option.heightAnchor constraintEqualToConstant:56],
        ]];
        option.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", path.lastPathComponent, detail];
        if (on) option.accessibilityTraits |= UIAccessibilityTraitSelected;
        [options addObject:option];
    }
    UIView* footer = LKColumn(@[LKLabel(text::applies_next_start, m.caption, UIFontWeightRegular, LauncherColor.muted),
                                LKButton(text::pick_other, @"folder", LKButtonRaised, m.iconButton, ^{ [weakSelf pickFolder]; })], 12);
    UIView* optionList = self.portrait ? LKColumn(options, 0) : LKScroll(LKColumn(options, 0));
    LKCard* chooser = [[LKCard alloc] initWithContent:LKColumn(@[optionList, footer], 12)
                                              padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, m.cardPadding, m.cardPadding)
                                               radius:m.cardRadius];

    const bool ready = _status.ready();
    UIView* rows = LKColumn(@[
        LKLabel(text::chosen_folder, m.caption, UIFontWeightRegular, LauncherColor.muted),
        LKListRow(text::status, nil, LKStatus(ready ? LauncherColor.positive : LauncherColor.negative, statusTitle(_status)), m.row, NO, nil),
        LKListRow(text::app_row, nil, LKValue(ns(_status.app_label())), m.row, YES, nil),
        LKListRow(text::map_row, nil, LKValue(ns(_status.map)), m.row, YES, nil),
        LKListRow(text::safety_row, nil, LKValue(ns(_status.safety_main)), m.row, YES, nil),
        LKSpacer(),
    ], 0);
    LKCard* chosen = [[LKCard alloc] initWithContent:rows padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, 4, m.cardPadding) radius:m.cardRadius];
    return [self cards:chooser with:chosen secondWidth:m.sideColumn];
}

- (UIView*)versionsPane {
    const auto& m = LauncherMetrics::current();
    VerifiedListStore* store = VerifiedListStore.shared;
    const atlan::shell::VerifiedList* list = store.list;
    const atlan::shell::VerifiedBuild* build = list && !_digest.empty() ? list->find(_digest) : nullptr;
    UIView* state = _digest.empty() ? LKValue(text::unknown)
                  : !list           ? LKValue(text::cannot_check)
                  : LKStatus(build ? LauncherColor.positive : LauncherColor.muted, build ? text::verified : text::unverified);
    UIView* rows = LKColumn(@[
        LKListRow(text::app_name, nil, LKValue(launcherVersion()), m.row, NO, nil),
        LKListRow(text::nav_software, nil, LKValue(ns(_status.app_label())), m.row, YES, nil),
        LKListRow(text::nav_db, text::nav_db_detail, LKValue(dates({&_status.map, &_status.route, &_status.search})), m.row, YES, nil),
        LKListRow(text::safety_db, text::safety_db_detail, LKValue(dates({&_status.safety_main, &_status.safety_sub})), m.row, YES, nil),
        LKListRow(text::status, nil, state, m.row, YES, nil),
    ], 0);
    LKCard* left = [[LKCard alloc] initWithContent:LKColumn(@[rows, LKSpacer(),
        LKLabel(text::versions_footnote, m.caption, UIFontWeightRegular, LauncherColor.muted)], 12)
                                           padding:UIEdgeInsetsMake(4, m.cardPadding, m.cardPadding, m.cardPadding) radius:m.cardRadius];

    NSMutableArray<UIView*>* entries = [NSMutableArray array];
    if (list) {
        for (const auto& each : list->builds) {
            const bool used = &each == build;
            UILabel* name = LKLabel(ns(each.name + " " + each.version), m.body, UIFontWeightMedium, LauncherColor.ink);
            UIView* trailing = used ? LKLabel(text::in_use, m.caption, UIFontWeightRegular, LauncherColor.ink2) : nil;
            UIStackView* row = LKRow(trailing ? @[name, LKSpacer(), trailing] : @[name, LKSpacer()], 8);
            [row.heightAnchor constraintEqualToConstant:m.row].active = YES;
            [entries addObject:entries.count ? LKColumn(@[LKLine(), row], 0) : row];
        }
    } else {
        [entries addObject:LKLabel(text::list_unavailable, m.body, UIFontWeightRegular, LauncherColor.muted)];
    }
    NSMutableArray<UIView*>* column = [NSMutableArray arrayWithObject:LKLabel(text::verified, m.caption, UIFontWeightRegular, LauncherColor.muted)];
    [column addObject:LKColumn(entries, 0)];
    [column addObject:LKSpacer()];
    if (list)
        [column addObject:LKLabel([NSString stringWithFormat:store.fresh ? text::list_date : text::list_date_saved, ns(list->updated)],
                                  m.caption, UIFontWeightRegular, LauncherColor.muted)];
    LKCard* right = [[LKCard alloc] initWithContent:LKColumn(column, 12)
                                            padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, m.cardPadding, m.cardPadding) radius:m.cardRadius];
    return [self cards:left with:right secondWidth:m.sideColumn];
}

- (UIView*)developerPane {
    const auto& m = LauncherMetrics::current();
    LauncherSettings* settings = LauncherSettings.shared;
    NSMutableArray<UIView*>* sections = [NSMutableArray array];
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
        [sections addObject:[self sectionCaption:text::section_experimental top:0]];
        [sections addObject:LKGroup(@[LKSwitchRow(text::notch_map, text::notch_map_detail, settings.notchMap, m.settingsRow,
                                                  ^(BOOL on) { LauncherSettings.shared.notchMap = on; })])];
    }
    [sections addObjectsFromArray:@[
        [self sectionCaption:text::section_logs top:sections.count ? 12 : 0],
        LKGroup(@[
            LKSwitchRow(text::session_log, text::session_log_detail, settings.sessionLog, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.sessionLog = on; }),
            LKSwitchRow(text::perf_log, text::perf_log_detail, settings.perfLog, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.perfLog = on; }),
            LKSwitchRow(text::net_log, text::net_log_detail, settings.networkLog, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.networkLog = on; }),
            LKSwitchRow(text::profiler, text::profiler_detail, settings.profiler, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.profiler = on; }),
        ]),
        [self sectionCaption:text::section_location top:12],
        LKGroup(@[
            LKSwitchRow(text::pin_location, text::pin_location_detail, settings.pinLocation, m.settingsRow,
                        ^(BOOL on) { LauncherSettings.shared.pinLocation = on; }),
            LKListRow(text::coordinate, text::coordinate_detail, LKField(pinnedCoordinate(), m.value * 14, UIKeyboardTypeNumbersAndPunctuation,
                                                                        ^NSString*(NSString* typed) {
                if (const auto point = atlan::shell::parse_coordinate(typed.UTF8String ?: ""))
                    [LauncherSettings.shared setPinnedLatitude:point->latitude longitude:point->longitude];
                return pinnedCoordinate();
            }), m.settingsRow, NO, nil),
            LKListRow(text::course, text::course_detail, LKField(pinnedCourse(), m.value * 5, UIKeyboardTypeNumbersAndPunctuation,
                                                                ^NSString*(NSString* typed) {
                NSScanner* scanner = [NSScanner scannerWithString:typed];
                double degrees = 0;
                if ([scanner scanDouble:&degrees] && std::isfinite(degrees) && degrees >= 0 && degrees <= 360)
                    LauncherSettings.shared.pinnedCourse = degrees == 360 ? 0 : degrees;
                return pinnedCourse();
            }), m.settingsRow, NO, nil),
        ]),
    ]];
    return LKScroll(LKColumn(sections, 0));
}
@end
