// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

// The host's own home screen, shown when the guest asks for the device's home (the role a car unit's launcher plays
// for the app). For now it only leads back to the guest; the app's real landing page and its options replace it here.
@interface LandingViewController : UIViewController
@end
