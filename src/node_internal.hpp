// SPDX-License-Identifier: Apache-2.0
// Shared helpers for the node implementation files. The node was one
// translation unit; as its components move into their own files they still
// need the same address parsing and asio aliases, so they live here instead
// of an anonymous namespace per file.
#pragma once
#include "mistlane/node.hpp"
#include <boost/asio.hpp>
#include <sstream>
#include <string>
#include <string_view>

namespace mistlane {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;

// IPv6 literals travel bracketed ("[::1]") so that a port can follow them.
inline asio::ip::address parse_address(const std::string& text) {
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
        return asio::ip::make_address(text.substr(1, text.size()-2));
    return asio::ip::make_address(text);
}

// The service identity frame: "MLS1 <role> <instance> <token>", four single
// words with no quoting. role and instance only label the peer's logs; the
// token is the whole of the authority.
inline bool service_fields(std::string_view payload, std::string& role,
                           std::string& instance, std::string& token) {
    std::istringstream in{std::string(payload)};
    std::string magic, extra;
    return (in >> magic >> role >> instance >> token) && magic == "MLS1" && !(in >> extra);
}
}
