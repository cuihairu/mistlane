// SPDX-License-Identifier: Apache-2.0
// Shared helpers for the node implementation files. The node was one
// translation unit; as its components move into their own files they still
// need the same address parsing and asio aliases, so they live here instead
// of an anonymous namespace per file.
#pragma once
#include "mistlane/node.hpp"
#include <boost/asio.hpp>
#include <array>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <string_view>

namespace mistlane {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;

// IPv6 literals travel bracketed ("[::1]") so that a port can follow them.
// A name that is not an address literal — a container network's service name,
// localhost, an FQDN — resolves here, so every server-side endpoint accepts
// hostnames exactly like the client already does. Resolution picks the first
// IPv4 answer and only falls back to IPv6, matching what most clients connect
// to (localhost resolves to ::1 first on many systems, and a v6-only bind
// would strand a v4 probe); an explicit "[::1]" literal is still honored.
inline asio::ip::address parse_address(const std::string& text) {
    std::string host = text;
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size()-2);
    try {
        return asio::ip::make_address(host);
    } catch (...) {
        // Not a literal: fall through to name resolution.
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
        throw std::invalid_argument("cannot resolve address: " + text);
    asio::ip::address resolved;
    bool found = false;
    for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET) continue; // prefer IPv4, fall back below
        const auto* in = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
        asio::ip::address_v4::bytes_type bytes{};
        std::memcpy(bytes.data(), &in->sin_addr, bytes.size());
        resolved = asio::ip::address_v4(bytes);
        found = true;
        break;
    }
    if (!found) {
        for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
            if (entry->ai_family != AF_INET6) continue;
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(entry->ai_addr);
            asio::ip::address_v6::bytes_type bytes{};
            std::memcpy(bytes.data(), &in6->sin6_addr, bytes.size());
            resolved = asio::ip::address_v6(bytes);
            found = true;
            break;
        }
    }
    freeaddrinfo(result);
    if (found) return resolved;
    throw std::invalid_argument("cannot resolve address: " + text);
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
