// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>
namespace mistlane {
struct HttpRequest {
    std::string url;
    std::string method = "GET"; // GET or POST
    std::string body;
    std::string ca_file; // empty: libcurl's default trust store
    long connect_timeout_ms = 2000;
    long timeout_ms = 10000;
    std::size_t max_response_bytes = 1024*1024;
};
struct HttpResponse { long status; std::string body; };
// Blocking. Use a bounded application executor, never the node/Lua event loop.
// Transport failures throw; HTTP 4xx/5xx are returned as ordinary responses.
HttpResponse http_request(const HttpRequest& request);
}
