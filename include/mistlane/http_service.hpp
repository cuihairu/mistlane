#pragma once
#include <functional>
#include <map>
#include <string>
namespace mistlane {
struct WebRequest {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;
};
struct WebResponse {
    unsigned status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
};
using HttpCompletion = std::function<void(WebResponse)>;
using HttpHandler = std::function<void(WebRequest, HttpCompletion)>;
}
