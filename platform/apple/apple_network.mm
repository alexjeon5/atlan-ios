// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple_network.hpp"
#import <Foundation/Foundation.h>
#import <Network/Network.h>
#include <algorithm>
#include <atomic>
#include <memory>

namespace atlan {
shell::HttpFetch apple_http_fetch() {
    NSURLSessionConfiguration* configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.HTTPCookieStorage = nil;
    configuration.URLCache = nil;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration];
    const auto plain = shell::plain_http_fetch();
    return [session, plain](shell::HttpRequest request, std::function<void(shell::HttpResponse)> done) {
        if (request.url.rfind("https://", 0) != 0 && request.url.rfind("HTTPS://", 0) != 0) {
            plain(std::move(request), std::move(done));
            return;
        }
        NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request.url.c_str()] ?: @""];
        if (url == nil) {
            shell::HttpResponse response;
            response.outcome = shell::HttpResponse::Outcome::malformed_url;
            response.error = "malformed URL";
            done(std::move(response));
            return;
        }
        NSMutableURLRequest* call = [NSMutableURLRequest requestWithURL:url];
        call.HTTPMethod = [NSString stringWithUTF8String:request.method.c_str()];
        call.timeoutInterval = std::max(request.timeout_ms, 1) / 1000.0;
        [call setValue:@"Dalvik/2.1.0 (Linux; U; Android)" forHTTPHeaderField:@"User-Agent"];
        for (const auto& [name, value] : request.headers)
            [call setValue:[NSString stringWithUTF8String:value.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
        if (!request.body.empty()) call.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];
        auto finish = std::make_shared<std::function<void(shell::HttpResponse)>>(std::move(done));
        NSURLSessionDataTask* task = [session dataTaskWithRequest:call
                                                completionHandler:^(NSData* data, NSURLResponse* reply, NSError* error) {
            shell::HttpResponse response;
            if (error != nil || ![reply isKindOfClass:NSHTTPURLResponse.class]) {
                response.error = error ? error.localizedDescription.UTF8String : "no HTTP response";
            } else {
                response.outcome = shell::HttpResponse::Outcome::ok;
                response.status = static_cast<int>(((NSHTTPURLResponse*)reply).statusCode);
                const auto* bytes = static_cast<const std::uint8_t*>(data.bytes);
                if (bytes != nullptr) response.body.assign(bytes, bytes + data.length);
            }
            (*finish)(std::move(response));
        }];
        [task resume];
    };
}

std::function<bool()> apple_network_available() {
    static const auto available = [] {
        auto state = std::make_shared<std::atomic<bool>>(true);
        nw_path_monitor_t monitor = nw_path_monitor_create();
        nw_path_monitor_set_queue(monitor, dispatch_queue_create("atlan.network", DISPATCH_QUEUE_SERIAL));
        nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
            state->store(nw_path_get_status(path) == nw_path_status_satisfied);
        });
        nw_path_monitor_start(monitor);
        static nw_path_monitor_t keep = monitor;  // runs for the life of the process
        (void)keep;
        return state;
    }();
    return [state = available] { return state->load(); };
}
}
