// SPDX-License-Identifier: Apache-2.0
#include "mistlane/http_client.hpp"
#include <curl/curl.h>
#include <memory>
#include <stdexcept>
#include <limits>
namespace mistlane {
namespace {
struct Runtime {
    Runtime() { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl init failed"); }
    ~Runtime() { curl_global_cleanup(); }
};
struct Sink { std::string body; std::size_t limit; };
std::size_t receive(char* ptr, std::size_t size, std::size_t count, void* context) noexcept {
    auto& sink = *static_cast<Sink*>(context);
    if (size && count > std::numeric_limits<std::size_t>::max()/size) return CURL_WRITEFUNC_ERROR;
    auto bytes = size*count;
    if (bytes > sink.limit-sink.body.size()) return CURL_WRITEFUNC_ERROR;
    try { sink.body.append(ptr, bytes); } catch (...) { return CURL_WRITEFUNC_ERROR; }
    return bytes;
}
}
HttpResponse http_request(const HttpRequest& request) {
    if (request.connect_timeout_ms <= 0 || request.timeout_ms <= 0 || request.max_response_bytes == 0 ||
        (request.method != "GET" && request.method != "POST") || request.url.find('\0') != std::string::npos ||
        request.ca_file.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid HTTP request options");
    static Runtime runtime;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("curl handle allocation failed");
    auto set = [&](CURLoption key, auto value) {
        auto error = curl_easy_setopt(handle.get(), key, value);
        if (error != CURLE_OK) throw std::runtime_error(curl_easy_strerror(error));
    };
    Sink sink{{}, request.max_response_bytes};
    set(CURLOPT_URL, request.url.c_str());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT_MS, request.connect_timeout_ms);
    set(CURLOPT_TIMEOUT_MS, request.timeout_ms);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    if (!request.ca_file.empty()) set(CURLOPT_CAINFO, request.ca_file.c_str());
    set(CURLOPT_WRITEFUNCTION, &receive);
    set(CURLOPT_WRITEDATA, &sink);
    if (request.method == "POST") {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, request.body.data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    auto error = curl_easy_perform(handle.get());
    if (error != CURLE_OK) throw std::runtime_error(curl_easy_strerror(error));
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
        throw std::runtime_error("cannot read HTTP status");
    return {status, std::move(sink.body)};
}
}
