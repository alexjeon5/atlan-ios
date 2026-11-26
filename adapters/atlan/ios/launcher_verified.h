// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>
#include "adapters/atlan/verified_versions.hpp"

// The verified-builds list (adapters/atlan/verified_versions.hpp) as the launcher has it: fetched from
// atlan::shell::verified_list_url when first used and again when the app becomes active (at most every few minutes),
// and kept in Application Support so the last copy is there offline. Nothing waits for it. Main thread only.
extern NSNotificationName const VerifiedListDidChange;

@interface VerifiedListStore : NSObject
@property(class, readonly) VerifiedListStore* shared;
// The list, or nullptr when none was ever fetched (the launcher then cannot tell whether a build is verified).
@property(nonatomic, readonly) const atlan::shell::VerifiedList* list;
// The list came from the server during this run (not only from the saved copy).
@property(nonatomic, readonly) BOOL fresh;
- (void)refresh;
@end
