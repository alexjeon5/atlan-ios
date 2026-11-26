// SPDX-License-Identifier: GPL-2.0-or-later
#import "launcher_style.h"
#import <CoreText/CoreText.h>
#include <cstdint>

namespace {
UIColor* rgb(std::uint32_t value, CGFloat alpha = 1) {
    return [UIColor colorWithRed:((value >> 16) & 255) / 255.0 green:((value >> 8) & 255) / 255.0
                            blue:(value & 255) / 255.0 alpha:alpha];
}
UIColor* pair(UIColor* dark, UIColor* light) {
    return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
        return traits.userInterfaceStyle == UIUserInterfaceStyleDark ? dark : light;
    }];
}
}

@implementation LauncherColor
+ (UIColor*)bg { return pair(rgb(0x0B0D14), rgb(0xF5F7FA)); }
+ (UIColor*)card { return pair(rgb(0x11141C), rgb(0xFFFFFF)); }
+ (UIColor*)raised { return pair(rgb(0x1A1E28), rgb(0xECEFF4)); }
+ (UIColor*)line { return pair(rgb(0xEEF0F5, 0.08), rgb(0x111827, 0.08)); }
+ (UIColor*)lineStrong { return pair(rgb(0xEEF0F5, 0.4), rgb(0x111827, 0.4)); }
+ (UIColor*)ink { return pair(rgb(0xEEF0F5), rgb(0x111827)); }
+ (UIColor*)ink2 { return pair(rgb(0xA3A9B8), rgb(0x4B5565)); }
+ (UIColor*)muted { return pair(rgb(0x7A8296), rgb(0x646D80)); }
+ (UIColor*)accent { return rgb(0x00AFFF); }
+ (UIColor*)onAccent { return rgb(0x0B0D14); }
+ (UIColor*)positive { return pair(rgb(0x4CC38A), rgb(0x16A34A)); }
+ (UIColor*)negative { return pair(rgb(0xE5484D), rgb(0xDC2626)); }
+ (UIColor*)tabSelected { return pair(rgb(0x1A1E28), rgb(0xFFFFFF)); }
+ (UIColor*)tabSelectedLine { return pair(UIColor.clearColor, rgb(0x111827, 0.08)); }
+ (UIColor*)segmentBg { return pair(rgb(0x0B0D14), rgb(0xECEFF4)); }
+ (UIColor*)segmentSelected { return pair(rgb(0x1A1E28), rgb(0xFFFFFF)); }
+ (UIColor*)shadow { return pair(UIColor.clearColor, rgb(0x000000, 0.05)); }
@end

const LauncherMetrics& LauncherMetrics::current() {
    static const LauncherMetrics phone{
        .caption = 12, .body = 14, .value = 16, .head = 16, .title = 28,
        .row = 44, .compactRow = 36, .settingsRow = 64,
        .gap = 12, .cardPadding = 16, .cardRadius = 16, .buttonRadius = 12,
        .button = 48, .iconButton = 44, .startColumn = 304, .startPadding = 20, .startIcon = 40, .startTitle = 28,
        .settingsButton = 48,
        .introColumn = 288, .tabColumn = 168, .sideColumn = 208,
        .container = CGSizeZero,
        .portraitContainer = CGSizeZero,
        .portraitStart = 200,
    };
    static const LauncherMetrics pad{
        .caption = 14, .body = 16, .value = 20, .head = 20, .title = 40,
        .row = 64, .compactRow = 48, .settingsRow = 72,
        .gap = 16, .cardPadding = 24, .cardRadius = 20, .buttonRadius = 12,
        .button = 56, .iconButton = 52, .startColumn = 400, .startPadding = 32, .startIcon = 64, .startTitle = 40,
        .settingsButton = 56,
        .introColumn = 360, .tabColumn = 200, .sideColumn = 260,
        .container = CGSizeMake(1000, 560),
        .portraitContainer = CGSizeMake(560, 1000),
        .portraitStart = 320,
    };
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? pad : phone;
}

// The design's type: Barlow for Latin text and figures (tabular), Noto Sans KR for Korean, at weights 400/500/600.
// Both ship with the app (fonts/, UIAppFonts); Noto Sans KR is one variable font, so its weight is a variation.
// Without them (a build without the resources) the system font stands in.
UIFont* LauncherFont(CGFloat size, UIFontWeight weight) {
    static NSMutableDictionary<NSString*, UIFont*>* cache = [NSMutableDictionary dictionary];
    const int wght = weight >= UIFontWeightSemibold ? 600 : weight >= UIFontWeightMedium ? 500 : 400;
    NSString* key = [NSString stringWithFormat:@"%g/%d", size, wght];
    if (UIFont* font = cache[key]) return font;
    NSString* latin = wght == 600 ? @"Barlow-SemiBold" : wght == 500 ? @"Barlow-Medium" : @"Barlow-Regular";
    UIFontDescriptor* korean = [UIFontDescriptor fontDescriptorWithFontAttributes:@{
        UIFontDescriptorNameAttribute: @"NotoSansKR-Thin",  // the variable font's default instance
        (__bridge NSString*)kCTFontVariationAttribute: @{@(0x77676874 /* 'wght' */): @(wght)},
    }];
    UIFontDescriptor* descriptor = [UIFontDescriptor fontDescriptorWithFontAttributes:@{
        UIFontDescriptorNameAttribute: latin,
        UIFontDescriptorCascadeListAttribute: @[korean],
        UIFontDescriptorFeatureSettingsAttribute: @[@{
            (__bridge NSString*)kCTFontOpenTypeFeatureTag: @"tnum",
            (__bridge NSString*)kCTFontOpenTypeFeatureValue: @1,
        }],
    }];
    UIFont* font = [UIFont fontWithDescriptor:descriptor size:size];
    if (![font.fontName hasPrefix:@"Barlow"]) font = [UIFont monospacedDigitSystemFontOfSize:size weight:weight];
    cache[key] = font;
    return font;
}
