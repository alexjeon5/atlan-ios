// SPDX-License-Identifier: GPL-2.0-or-later
#import "launcher_view_controller.h"
#import "launcher_kit.h"
#import "launcher_settings.h"
#import "launcher_style.h"
#import "launcher_text.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "adapters/atlan/data_status.hpp"
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
    std::vector<std::pair<std::string, AtlanDataStatus>> _choices;  // every ATLAN folder in Documents
    std::string _fingerprint;
    BOOL _known;         // a check has finished
    BOOL _checking;
    NSTimer* _timer;
    dispatch_queue_t _queue;
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
        [NSLayoutConstraint activateConstraints:@[
            [_frame.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
            [_frame.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
            [_frame.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-48],
            [_frame.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor constant:-48],
            width, height,
        ]];
    } else {
        // 16 at the top and 24 at the bottom as designed; at the sides the notch's safe area plus a little.
        NSLayoutConstraint* bottom = [_frame.bottomAnchor constraintEqualToAnchor:view.bottomAnchor constant:-24];
        bottom.priority = UILayoutPriorityDefaultHigh;
        NSLayoutConstraint* leading = [_frame.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:12];
        NSLayoutConstraint* trailing = [_frame.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-12];
        leading.priority = trailing.priority = UILayoutPriorityDefaultHigh;
        [NSLayoutConstraint activateConstraints:@[
            [_frame.topAnchor constraintEqualToAnchor:safe.topAnchor constant:16],
            [_frame.bottomAnchor constraintLessThanOrEqualToAnchor:view.bottomAnchor constant:-24],
            [_frame.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor],
            [_frame.leadingAnchor constraintGreaterThanOrEqualToAnchor:view.leadingAnchor constant:24],
            [_frame.trailingAnchor constraintLessThanOrEqualToAnchor:view.trailingAnchor constant:-24],
            [_frame.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor],
            [_frame.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor],
            bottom, leading, trailing,
        ]];
    }
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(check)
                                               name:UIApplicationDidBecomeActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(check)
                                               name:LauncherSettingsDidChange object:nil];
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
        auto choices = std::make_shared<std::vector<std::pair<std::string, AtlanDataStatus>>>();
        for (NSString* each in folders)
            choices->emplace_back(each.UTF8String ?: "", atlan::shell::inspect_atlan_data(each.UTF8String ?: ""));
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf checked:*status choices:*choices folder:folder]; });
    });
}

- (void)checked:(const AtlanDataStatus&)status choices:(const std::vector<std::pair<std::string, AtlanDataStatus>>&)choices
         folder:(NSString*)folder {
    _checking = NO;
    std::string print = fingerprint(status) + (LauncherSettings.shared.atlanFolderIsPicked ? "p" : "d") + (folder.UTF8String ?: "");
    for (const auto& [path, each] : choices) print += "\n" + path + fingerprint(each);
    if (_known && print == _fingerprint) return;
    _known = YES;
    _status = status;
    _choices = choices;
    _fingerprint = print;
    if (status.ready() && !LauncherSettings.shared.setupDone) LauncherSettings.shared.setupDone = YES;
    [self render];
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
    UIStackView* line = LKRow(@[LKLabel(text::app_name, m.head, UIFontWeightSemibold, LauncherColor.ink),
                                LKLabel(launcherVersion(), m.caption, UIFontWeightRegular, LauncherColor.muted)], 8);
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
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    UIButton* gear = LKIconButton(@"slider.horizontal.3", text::settings, ^{ [weakSelf openSettings:Tab::general]; });
    UIStackView* top = LKRow(@[[self nameLine], LKSpacer(), gear], 8);
    for (UIButton* button in buttons) [button setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [buttons.firstObject setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* row = LKRow(buttons, 8);
    row.alignment = UIStackViewAlignmentFill;
    UIStackView* column = LKColumn(@[top, LKSpacer(), LKColumn(@[block, row], 16)], 0);
    LKSize(column, m.introColumn, 0);
    return column;
}

- (UIView*)twoColumns:(UIView*)left right:(UIView*)right spacing:(CGFloat)spacing {
    UIStackView* row = LKRow(@[left, right], spacing);
    row.alignment = UIStackViewAlignmentFill;
    return row;
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
    return [self twoColumns:left right:LKColumn(@[ways, layout], m.gap) spacing:24];
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
    return [self twoColumns:left right:LKColumn(@[notes, data], m.gap) spacing:24];
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

// One card per ATLAN folder in Documents: its name, the app inside and whether it is complete.
- (UIView*)folderCard:(const std::string&)path status:(const AtlanDataStatus&)status chosen:(BOOL)chosen {
    const auto& m = LauncherMetrics::current();
    NSString* folder = ns(path);
    NSString* detail = status.ready() ? ns(status.app_label()) : statusTitle(status);
    UIView* names = LKColumn(@[LKLabel(folder.lastPathComponent, m.value, UIFontWeightMedium, LauncherColor.ink),
                               LKLabel(detail, m.caption, UIFontWeightRegular, LauncherColor.muted)], 0);
    UIView* mark = chosen ? LKIcon(@"checkmark", m.value, LauncherColor.accent) : LKIcon(@"chevron.right", m.caption + 2, LauncherColor.muted);
    UIStackView* row = LKRow(@[LKDot(status.ready() ? LauncherColor.positive : LauncherColor.negative, 8), names, LKSpacer(), mark], 12);
    [row.heightAnchor constraintGreaterThanOrEqualToConstant:m.row].active = YES;
    LKCard* card = [[LKCard alloc] initWithContent:row padding:UIEdgeInsetsMake(8, m.cardPadding, 8, m.cardPadding) radius:m.cardRadius];
    card.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", folder.lastPathComponent, detail];
    [card addAction:[UIAction actionWithHandler:^(UIAction*) { [LauncherSettings.shared chooseDocumentsFolder:folder]; }]
   forControlEvents:UIControlEventTouchUpInside];
    return card;
}

- (UIView*)chooseScreen {
    const auto& m = LauncherMetrics::current();
    __weak LauncherViewController* weakSelf = self;
    UIView* left = [self introColumn:[self titleBlock:text::choose_title body:text::choose_body] buttons:@[
        LKButton(text::recheck, nil, LKButtonSecondary, m.button, ^{ [weakSelf check]; }),
    ]];
    NSMutableArray<UIView*>* cards = [NSMutableArray array];
    for (const auto& [path, status] : _choices) [cards addObject:[self folderCard:path status:status chosen:NO]];
    return [self twoColumns:left right:LKScroll(LKColumn(cards, m.gap)) spacing:24];
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
    LKCard* card = [[LKCard alloc] initWithContent:cardContent
                                           padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, blocked ? m.cardPadding : 4, m.cardPadding)
                                            radius:m.cardRadius];
    [card addAction:[UIAction actionWithHandler:^(UIAction*) { [weakSelf openSettings:Tab::map]; }]
   forControlEvents:UIControlEventTouchUpInside];
    UIStackView* left = LKColumn(@[[self nameLine], card], m.gap);

    LKStartButton* start = [[LKStartButton alloc] initWithTitle:_running ? text::resume : text::start];
    start.enabled = ready || _running;
    [start addAction:[UIAction actionWithHandler:^(UIAction*) { [weakSelf startNavigation]; }]
    forControlEvents:UIControlEventTouchUpInside];
    UIButton* gear = LKButton(text::settings, @"slider.horizontal.3", LKButtonSecondary, m.settingsButton,
                              ^{ [weakSelf openSettings:Tab::general]; });
    UIStackView* right = LKColumn(@[start, gear], m.gap);
    LKSize(right, m.startColumn, 0);
    return [self twoColumns:left right:right spacing:24];
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
    [tabs addObject:LKSpacer()];
    UIStackView* tabColumn = LKColumn(tabs, 4);
    LKSize(tabColumn, m.tabColumn, 0);

    UIView* pane = _tab == Tab::general ? [self generalPane]
                 : _tab == Tab::map     ? [self mapPane]
                 : _tab == Tab::versions ? [self versionsPane]
                                          : [self developerPane];
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
    config.attributedTitle = [[NSAttributedString alloc] initWithString:name attributes:@{
        NSFontAttributeName: LauncherFont(m.value, on ? UIFontWeightMedium : UIFontWeightRegular)}];
    config.baseForegroundColor = on ? LauncherColor.ink : LauncherColor.ink2;
    config.contentInsets = NSDirectionalEdgeInsetsMake(0, 12, 0, 12);
    config.background.cornerRadius = 8;
    config.background.backgroundColor = on ? LauncherColor.tabSelected : UIColor.clearColor;
    config.background.strokeColor = on ? LauncherColor.tabSelectedLine : UIColor.clearColor;
    config.background.strokeWidth = 1;
    __weak LauncherViewController* weakSelf = self;
    UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction*) {
        [weakSelf openSettings:tab];
    }]];
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
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
    NSMutableArray<UIView*>* screenRows = [NSMutableArray arrayWithObject:
        LKSegmentRow(text::appearance, text::appearance_detail,
                     @[text::appearance_system, text::appearance_light, text::appearance_dark], settings.appearance, 60,
                     ^(NSInteger index) { LauncherSettings.shared.appearance = (LauncherAppearance)index; })];
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone)
        [screenRows addObject:LKSwitchRow(text::notch, text::notch_detail, settings.notchInset, 60,
                                          ^(BOOL on) { LauncherSettings.shared.notchInset = on; })];
    return LKScroll(LKColumn(@[
        [self sectionCaption:text::section_start top:0],
        LKGroup(@[LKSwitchRow(text::auto_start, text::auto_start_detail, settings.autoStart, 72,
                              ^(BOOL on) { LauncherSettings.shared.autoStart = on; })]),
        [self sectionCaption:text::section_screen top:12],
        LKGroup(screenRows),
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
    // Several data sets in Documents: switch between them here.
    NSMutableArray<UIView*>* items = [NSMutableArray arrayWithObjects:card, [self sectionCaption:text::documents_folders top:4], nil];
    for (const auto& [path, status] : _choices) {
        const BOOL chosen = !settings.atlanFolderIsPicked && [ns(path) isEqualToString:active];
        [items addObject:[self folderCard:path status:status chosen:chosen]];
    }
    UIStackView* column = LKColumn(items, 8);
    [column setCustomSpacing:m.gap afterView:card];
    return LKScroll(column);
}

- (UIView*)versionsPane {
    const auto& m = LauncherMetrics::current();
    const bool known = !_status.app_name.empty();
    const bool verified = atlan::shell::is_verified(_status);
    UIView* state = known ? LKStatus(verified ? LauncherColor.positive : LauncherColor.muted, verified ? text::verified : text::unverified)
                          : LKValue(text::unknown);
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

    NSMutableArray<UIView*>* list = [NSMutableArray arrayWithObject:LKLabel(text::verified, m.caption, UIFontWeightRegular, LauncherColor.muted)];
    BOOL first = YES;
    for (const auto& app : atlan::shell::verified_apps()) {
        const bool used = app.name == _status.app_name && app.version == _status.app_version;
        NSString* label = ns(std::string(app.name) + " " + std::string(app.version));
        UILabel* name = LKLabel(label, m.body, UIFontWeightMedium, LauncherColor.ink);
        UIView* trailing = used ? LKLabel(text::in_use, m.caption, UIFontWeightRegular, LauncherColor.ink2) : nil;
        UIStackView* row = LKRow(trailing ? @[name, LKSpacer(), trailing] : @[name, LKSpacer()], 8);
        [row.heightAnchor constraintEqualToConstant:m.row].active = YES;
        [list addObject:first ? row : LKColumn(@[LKLine(), row], 0)];
        first = NO;
    }
    LKCard* right = [[LKCard alloc] initWithContent:LKColumn(@[LKColumn(list, 0), LKSpacer(),
        LKLabel([NSString stringWithFormat:text::verified_footnote, text::app_name, launcherVersion()], m.caption, UIFontWeightRegular, LauncherColor.muted)], 12)
                                            padding:UIEdgeInsetsMake(m.cardPadding, m.cardPadding, m.cardPadding, m.cardPadding) radius:m.cardRadius];
    LKSize(right, m.sideColumn, 0);
    return [self twoColumns:left right:right spacing:m.gap];
}

- (UIView*)developerPane {
    const auto& m = LauncherMetrics::current();
    LauncherSettings* settings = LauncherSettings.shared;
    return LKScroll(LKGroup(@[
        LKSwitchRow(text::session_log, text::session_log_detail, settings.sessionLog, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.sessionLog = on; }),
        LKSwitchRow(text::perf_log, text::perf_log_detail, settings.perfLog, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.perfLog = on; }),
        LKSwitchRow(text::net_log, text::net_log_detail, settings.networkLog, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.networkLog = on; }),
        LKSwitchRow(text::profiler, text::profiler_detail, settings.profiler, m.settingsRow, ^(BOOL on) { LauncherSettings.shared.profiler = on; }),
    ]));
}
@end
