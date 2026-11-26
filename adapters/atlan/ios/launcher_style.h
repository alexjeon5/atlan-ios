// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

// The launcher's colours (each follows light and dark mode) and sizes. Visual tweaks belong here.
@interface LauncherColor : NSObject
@property(class, readonly) UIColor* bg;
@property(class, readonly) UIColor* card;
@property(class, readonly) UIColor* raised;
@property(class, readonly) UIColor* line;
@property(class, readonly) UIColor* ink;
@property(class, readonly) UIColor* ink2;
@property(class, readonly) UIColor* muted;
@property(class, readonly) UIColor* accent;
@property(class, readonly) UIColor* onAccent;
@property(class, readonly) UIColor* positive;
@property(class, readonly) UIColor* negative;
@property(class, readonly) UIColor* tabSelected;
@property(class, readonly) UIColor* tabSelectedLine;
@property(class, readonly) UIColor* segmentBg;
@property(class, readonly) UIColor* segmentSelected;
@property(class, readonly) UIColor* shadow;  // clear in dark mode
@end

// Sizes in points. The iPhone values follow the 844 × 390 design; the iPad ones its 1180 × 820 start screen, laid
// out in a centred `container` box.
struct LauncherMetrics {
    CGFloat caption, body, value, head, title;  // font sizes
    CGFloat row, compactRow, settingsRow;        // list row heights
    CGFloat gap, cardPadding, cardRadius, buttonRadius;
    CGFloat button, iconButton, startColumn, startPadding, startIcon, startTitle, settingsButton;
    CGFloat introColumn, tabColumn, sideColumn;
    CGSize container;  // zero: the whole safe area
    static const LauncherMetrics& current();
};

UIFont* LauncherFont(CGFloat size, UIFontWeight weight);
