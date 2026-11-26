// SPDX-License-Identifier: GPL-2.0-or-later
#import "launcher_verified.h"
#import <UIKit/UIKit.h>
#include <optional>
#include <string>

NSNotificationName const VerifiedListDidChange = @"VerifiedListDidChange";

namespace {
constexpr NSTimeInterval fetch_timeout = 15, refresh_interval = 300;

NSURL* cacheURL() {
    NSURL* folder = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
    return [folder URLByAppendingPathComponent:@"verified_versions.json"];
}

std::optional<atlan::shell::VerifiedList> parse(NSData* data) {
    if (!data) return std::nullopt;
    std::string why;
    auto list = atlan::shell::parse_verified_list(std::string_view(static_cast<const char*>(data.bytes), data.length), why);
    if (!list) NSLog(@"verified list rejected: %s", why.c_str());
    return list;
}
}

@implementation VerifiedListStore {
    std::optional<atlan::shell::VerifiedList> _list;
    BOOL _fetching;
    NSDate* _lastAttempt;
}

+ (VerifiedListStore*)shared {
    static VerifiedListStore* store;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ store = [VerifiedListStore new]; });
    return store;
}

- (instancetype)init {
    if ((self = [super init])) {
        _list = parse([NSData dataWithContentsOfURL:cacheURL()]);
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(becameActive)
                                                   name:UIApplicationDidBecomeActiveNotification object:nil];
        [self refresh];
    }
    return self;
}

- (const atlan::shell::VerifiedList*)list { return _list ? &*_list : nullptr; }

- (void)becameActive {
    if (!_lastAttempt || -_lastAttempt.timeIntervalSinceNow >= refresh_interval) [self refresh];
}

- (void)refresh {
    if (_fetching) return;
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:std::string(atlan::shell::verified_list_url).c_str()]];
    if (!url) return;
    _fetching = YES;
    _lastAttempt = [NSDate date];
    NSURLSessionConfiguration* configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.timeoutIntervalForRequest = fetch_timeout;
    configuration.timeoutIntervalForResource = fetch_timeout;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration];
    __weak VerifiedListStore* weakSelf = self;
    [[session dataTaskWithURL:url completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        const NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse*)response).statusCode : 0;
        auto list = !error && status == 200 ? parse(data) : std::nullopt;
        if (list) [data writeToURL:cacheURL() atomically:YES];
        else NSLog(@"verified list not fetched: %@", error.localizedDescription ?: [NSString stringWithFormat:@"HTTP %ld", (long)status]);
        dispatch_async(dispatch_get_main_queue(), ^{
            VerifiedListStore* strongSelf = weakSelf;
            if (!strongSelf) return;
            strongSelf->_fetching = NO;
            if (!list) return;
            strongSelf->_list = std::move(list);
            strongSelf->_fresh = YES;
            [NSNotificationCenter.defaultCenter postNotificationName:VerifiedListDidChange object:strongSelf];
        });
    }] resume];
    [session finishTasksAndInvalidate];
}
@end
