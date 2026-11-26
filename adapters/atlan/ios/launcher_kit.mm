// SPDX-License-Identifier: GPL-2.0-or-later
#import "launcher_kit.h"
#import "launcher_style.h"

@implementation LKCard
- (instancetype)initWithContent:(UIView*)content padding:(UIEdgeInsets)padding radius:(CGFloat)radius {
    if ((self = [super initWithFrame:CGRectZero])) {
        self.backgroundColor = LauncherColor.card;
        self.layer.cornerRadius = radius;
        self.layer.borderWidth = 1;
        self.layer.shadowOffset = CGSizeMake(0, 1);
        self.layer.shadowRadius = 1;
        self.layer.shadowOpacity = 1;
        [self refreshColors];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        content.userInteractionEnabled = YES;
        [self addSubview:content];
        [NSLayoutConstraint activateConstraints:@[
            [content.topAnchor constraintEqualToAnchor:self.topAnchor constant:padding.top],
            [content.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:padding.left],
            [content.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-padding.right],
            [content.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-padding.bottom],
        ]];
    }
    return self;
}
- (void)refreshColors {
    UIColor* border = _selectedLook ? LauncherColor.lineStrong : LauncherColor.line;
    self.layer.borderColor = [border resolvedColorWithTraitCollection:self.traitCollection].CGColor;
    self.layer.shadowColor = [LauncherColor.shadow resolvedColorWithTraitCollection:self.traitCollection].CGColor;
}
- (void)setSelectedLook:(BOOL)selectedLook {
    _selectedLook = selectedLook;
    [self refreshColors];
}
- (void)traitCollectionDidChange:(UITraitCollection*)previous {
    [super traitCollectionDidChange:previous];
    [self refreshColors];
}
- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    self.alpha = highlighted ? 0.7 : 1;
}
// Rows inside a tappable card pass touches to it, except real controls such as buttons and switches.
- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
    UIView* hit = [super hitTest:point withEvent:event];
    if (!hit || [hit isKindOfClass:UIControl.class]) return hit;
    return self;
}
@end

@implementation LKStartButton {
    UIImageView* _icon;
    UILabel* _title;
}
- (instancetype)initWithTitle:(NSString*)title {
    if ((self = [super initWithFrame:CGRectZero])) {
        const auto& m = LauncherMetrics::current();
        self.layer.cornerRadius = m.cardRadius;
        _icon = LKIcon(@"location.north.fill", m.startIcon, LauncherColor.onAccent);
        _title = LKLabel(title, m.startTitle, UIFontWeightMedium, LauncherColor.onAccent);
        _title.numberOfLines = 2;
        self.accessibilityLabel = title;
        self.accessibilityTraits = UIAccessibilityTraitButton;
        self.isAccessibilityElement = YES;
        for (UIView* view in @[_icon, _title]) {
            view.translatesAutoresizingMaskIntoConstraints = NO;
            view.userInteractionEnabled = NO;
            [self addSubview:view];
        }
        [NSLayoutConstraint activateConstraints:@[
            [_icon.topAnchor constraintEqualToAnchor:self.topAnchor constant:m.startPadding],
            [_icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:m.startPadding],
            [_title.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:m.startPadding],
            [_title.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-m.startPadding],
            [_title.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-m.startPadding],
            [_title.topAnchor constraintGreaterThanOrEqualToAnchor:_icon.bottomAnchor constant:8],
        ]];
        [self refresh];
    }
    return self;
}
- (void)refresh {
    UIColor* ink = self.enabled ? LauncherColor.onAccent : LauncherColor.ink2;
    self.backgroundColor = self.enabled ? LauncherColor.accent : LauncherColor.raised;
    _icon.tintColor = ink;
    _title.textColor = ink;
    self.alpha = self.highlighted ? 0.8 : 1;
}
- (void)setEnabled:(BOOL)enabled { [super setEnabled:enabled]; [self refresh]; }
- (void)setHighlighted:(BOOL)highlighted { [super setHighlighted:highlighted]; [self refresh]; }
@end

UILabel* LKLabel(NSString* text, CGFloat size, UIFontWeight weight, UIColor* color) {
    UILabel* label = [UILabel new];
    label.text = text;
    label.font = LauncherFont(size, weight);
    label.textColor = color;
    label.numberOfLines = 0;
    label.lineBreakStrategy = NSLineBreakStrategyHangulWordPriority;
    return label;
}

static UIStackView* stack(NSArray<UIView*>* views, UILayoutConstraintAxis axis, CGFloat spacing) {
    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:views];
    stack.axis = axis;
    stack.spacing = spacing;
    return stack;
}
UIStackView* LKColumn(NSArray<UIView*>* views, CGFloat spacing) { return stack(views, UILayoutConstraintAxisVertical, spacing); }
UIStackView* LKRow(NSArray<UIView*>* views, CGFloat spacing) {
    UIStackView* row = stack(views, UILayoutConstraintAxisHorizontal, spacing);
    row.alignment = UIStackViewAlignmentCenter;
    return row;
}

UIView* LKSpacer(void) {
    UIView* view = [UIView new];
    [view setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisHorizontal];
    [view setContentHuggingPriority:1 forAxis:UILayoutConstraintAxisVertical];
    return view;
}

UIView* LKLine(void) {
    UIView* line = [UIView new];
    line.backgroundColor = LauncherColor.line;
    LKSize(line, 0, 1);
    return line;
}

UIView* LKDot(UIColor* color, CGFloat size) {
    UIView* dot = [UIView new];
    dot.backgroundColor = color;
    dot.layer.cornerRadius = size / 2;
    LKSize(dot, size, size);
    return dot;
}

UIView* LKRadio(BOOL on) {
    UIView* ring = [UIView new];
    ring.layer.cornerRadius = 10;
    ring.layer.borderWidth = 1.5;
    ring.backgroundColor = on ? LauncherColor.accent : UIColor.clearColor;
    ring.layer.borderColor = (on ? LauncherColor.accent : LauncherColor.muted).CGColor;  // fixed accent; muted is close in both modes
    LKSize(ring, 20, 20);
    if (on) {
        UIView* dot = LKDot(LauncherColor.onAccent, 8);
        [ring addSubview:dot];
        [NSLayoutConstraint activateConstraints:@[[dot.centerXAnchor constraintEqualToAnchor:ring.centerXAnchor],
                                                  [dot.centerYAnchor constraintEqualToAnchor:ring.centerYAnchor]]];
    }
    return ring;
}

UIImageView* LKIcon(NSString* symbol, CGFloat size, UIColor* color) {
    UIImageSymbolConfiguration* config = [UIImageSymbolConfiguration configurationWithPointSize:size weight:UIImageSymbolWeightRegular];
    UIImageView* view = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol withConfiguration:config]];
    view.tintColor = color;
    view.contentMode = UIViewContentModeCenter;
    [view setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return view;
}

void LKSize(UIView* view, CGFloat width, CGFloat height) {
    view.translatesAutoresizingMaskIntoConstraints = NO;
    if (width > 0) [view.widthAnchor constraintEqualToConstant:width].active = YES;
    if (height > 0) [view.heightAnchor constraintEqualToConstant:height].active = YES;
}

UIButton* LKButton(NSString* title, NSString* symbol, LKButtonKind kind, CGFloat height, void (^action)(void)) {
    const auto& m = LauncherMetrics::current();
    UIButtonConfiguration* config = UIButtonConfiguration.plainButtonConfiguration;
    UIColor* ink = kind == LKButtonPrimary ? LauncherColor.onAccent : LauncherColor.ink;
    UIFont* font = LauncherFont(m.value, UIFontWeightMedium);
    config.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:@{NSFontAttributeName: font}];
    config.baseForegroundColor = ink;
    if (symbol) {
        config.image = [UIImage systemImageNamed:symbol withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:m.value]];
        config.imagePadding = 8;
    }
    config.contentInsets = NSDirectionalEdgeInsetsMake(0, 16, 0, 16);
    config.background.cornerRadius = kind == LKButtonRaised ? 8 : m.buttonRadius;
    config.background.backgroundColor = kind == LKButtonPrimary ? LauncherColor.accent
                                      : kind == LKButtonRaised  ? LauncherColor.raised
                                                                : LauncherColor.card;
    if (kind == LKButtonSecondary) {
        config.background.strokeColor = LauncherColor.line;
        config.background.strokeWidth = 1;
    }
    UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction*) { action(); }]];
    LKSize(button, 0, height);
    return button;
}

UIButton* LKIconButton(NSString* symbol, NSString* label, void (^action)(void)) {
    const auto& m = LauncherMetrics::current();
    UIButton* button = LKButton(@"", nil, LKButtonSecondary, m.iconButton, action);
    UIButtonConfiguration* config = button.configuration;
    config.attributedTitle = nil;
    config.title = nil;
    config.image = [UIImage systemImageNamed:symbol withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:16]];
    config.contentInsets = NSDirectionalEdgeInsetsZero;
    button.configuration = config;
    button.accessibilityLabel = label;
    [button.widthAnchor constraintEqualToConstant:m.iconButton].active = YES;
    return button;
}

UILabel* LKValue(NSString* text) {
    UILabel* label = LKLabel(text, LauncherMetrics::current().value, UIFontWeightMedium, LauncherColor.ink);
    label.numberOfLines = 1;
    label.textAlignment = NSTextAlignmentRight;
    [label setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return label;
}

UIView* LKStatus(UIColor* color, NSString* text) {
    UILabel* label = LKValue(text);
    return LKRow(@[LKDot(color, 8), label], 8);
}

UIView* LKListRow(NSString* title, NSString* detail, UIView* trailing, CGFloat height, BOOL lineAbove, UIColor* titleColor) {
    const auto& m = LauncherMetrics::current();
    UILabel* name = LKLabel(title, detail ? m.value : m.body, UIFontWeightRegular, titleColor ?: LauncherColor.ink);
    UIView* left = name;
    if (detail) left = LKColumn(@[name, LKLabel(detail, m.caption, UIFontWeightRegular, LauncherColor.muted)], 0);
    [left setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* row = LKRow(trailing ? @[left, LKSpacer(), trailing] : @[left, LKSpacer()], 12);
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [row.heightAnchor constraintGreaterThanOrEqualToConstant:height].active = YES;
    if (!lineAbove) return row;
    return LKColumn(@[LKLine(), row], 0);
}

UIView* LKSwitchRow(NSString* title, NSString* detail, BOOL on, CGFloat height, void (^changed)(BOOL)) {
    UISwitch* toggle = [UISwitch new];
    toggle.on = on;
    toggle.onTintColor = LauncherColor.accent;
    toggle.accessibilityLabel = title;
    [toggle addAction:[UIAction actionWithHandler:^(UIAction* action) { changed(((UISwitch*)action.sender).on); }]
     forControlEvents:UIControlEventValueChanged];
    return LKListRow(title, detail, toggle, height, NO, nil);
}

UIView* LKSegmentRow(NSString* title, NSString* detail, NSArray<NSString*>* options, NSInteger selected, CGFloat height,
                     void (^changed)(NSInteger)) {
    const auto& m = LauncherMetrics::current();
    UISegmentedControl* control = [[UISegmentedControl alloc] initWithItems:options];
    control.selectedSegmentIndex = selected;
    control.backgroundColor = LauncherColor.segmentBg;
    control.selectedSegmentTintColor = LauncherColor.segmentSelected;
    [control setTitleTextAttributes:@{NSFontAttributeName: LauncherFont(m.body, UIFontWeightRegular),
                                      NSForegroundColorAttributeName: LauncherColor.ink2} forState:UIControlStateNormal];
    [control setTitleTextAttributes:@{NSFontAttributeName: LauncherFont(m.body, UIFontWeightMedium),
                                      NSForegroundColorAttributeName: LauncherColor.ink} forState:UIControlStateSelected];
    [control addAction:[UIAction actionWithHandler:^(UIAction* action) {
        changed(((UISegmentedControl*)action.sender).selectedSegmentIndex);
    }] forControlEvents:UIControlEventValueChanged];
    LKSize(control, 64.0 * options.count + 8, 40);
    return LKListRow(title, detail, control, height, NO, nil);
}

UIView* LKCaptionBlock(NSString* caption, NSString* body) {
    const auto& m = LauncherMetrics::current();
    return LKColumn(@[LKLabel(caption, m.caption, UIFontWeightRegular, LauncherColor.muted),
                      LKLabel(body, m.body, UIFontWeightRegular, LauncherColor.ink)], 4);
}

LKCard* LKGroup(NSArray<UIView*>* rows) {
    const auto& m = LauncherMetrics::current();
    NSMutableArray<UIView*>* items = [NSMutableArray array];
    for (UIView* row in rows) {
        if (items.count) [items addObject:LKLine()];
        [items addObject:row];
    }
    return [[LKCard alloc] initWithContent:LKColumn(items, 0) padding:UIEdgeInsetsMake(4, m.cardPadding, 4, m.cardPadding)
                                    radius:m.cardRadius];
}

UIScrollView* LKScroll(UIView* content) {
    UIScrollView* scroll = [UIScrollView new];
    scroll.alwaysBounceVertical = NO;
    scroll.showsVerticalScrollIndicator = NO;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [content.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [content.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [content.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [content.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
    ]];
    return scroll;
}
