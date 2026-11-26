// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

// Small building blocks the launcher screens are composed from. Screens are rebuilt from these whenever what they
// show changes, so a layout change is an edit to one builder call.

typedef NS_ENUM(NSInteger, LKButtonKind) { LKButtonPrimary, LKButtonSecondary, LKButtonRaised };

// A card: rounded surface with a hairline border (and a faint shadow in light mode). Tappable when given an action.
@interface LKCard : UIControl
- (instancetype)initWithContent:(UIView*)content padding:(UIEdgeInsets)padding radius:(CGFloat)radius;
@property(nonatomic) BOOL selectedLook;  // the stronger border of a chosen option
@end

// The large start button: an icon at the top left and the title at the bottom left.
@interface LKStartButton : UIControl
- (instancetype)initWithTitle:(NSString*)title;
@end

UILabel* LKLabel(NSString* text, CGFloat size, UIFontWeight weight, UIColor* color);
UIStackView* LKColumn(NSArray<UIView*>* views, CGFloat spacing);
UIStackView* LKRow(NSArray<UIView*>* views, CGFloat spacing);
UIView* LKSpacer(void);
UIView* LKLine(void);
UIView* LKDot(UIColor* color, CGFloat size);
UIView* LKRadio(BOOL on);  // the round mark of one option among several
UIImageView* LKIcon(NSString* symbol, CGFloat size, UIColor* color);
void LKSize(UIView* view, CGFloat width, CGFloat height);  // 0 leaves that side free

UIButton* LKButton(NSString* title, NSString* symbol, LKButtonKind kind, CGFloat height, void (^action)(void));
UIButton* LKIconButton(NSString* symbol, NSString* label, void (^action)(void));

// Title (with an optional caption below it) on the left, `trailing` on the right.
UIView* LKListRow(NSString* title, NSString* detail, UIView* trailing, CGFloat height, BOOL lineAbove, UIColor* titleColor);
// Value text in the list's value style.
UILabel* LKValue(NSString* text);
// A dot and a word, as used for "ready" and verification states.
UIView* LKStatus(UIColor* color, NSString* text);
UIView* LKSwitchRow(NSString* title, NSString* detail, BOOL on, CGFloat height, void (^changed)(BOOL));
UISegmentedControl* LKSegments(NSArray<NSString*>* options, NSInteger selected, void (^changed)(NSInteger));
UIView* LKSegmentRow(NSString* title, NSString* detail, NSArray<NSString*>* options, NSInteger selected, CGFloat height,
                     void (^changed)(NSInteger));
// A one-line text field in the list's value style. `committed` gets the text when editing ends and returns what the
// field shows afterwards (the value as stored, or the previous one when the text was not accepted).
UITextField* LKField(NSString* text, CGFloat width, UIKeyboardType keyboard, NSString* (^committed)(NSString*));
// Small grey heading above a block, e.g. "방법 1".
UIView* LKCaptionBlock(NSString* caption, NSString* body);
// Rows inside one card, separated by hairlines.
LKCard* LKGroup(NSArray<UIView*>* rows);
// Content in a vertical scroll view, for panes that may be taller than the screen. It keeps a text field being
// edited inside it clear of the keyboard; a tap outside the field ends the editing.
UIScrollView* LKScroll(UIView* content);
