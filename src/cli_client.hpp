// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include <string>
#include <string_view>
namespace mistlane::cli {
struct ClientOptions {
    std::string url, ca_file;
    unsigned timeout_ms = 5000;
    std::string method = "POST";
};
class Client {
public:
    virtual ~Client() = default;
    virtual std::string request(std::string_view payload, unsigned timeout_ms = 0) = 0;
};
std::unique_ptr<Client> make_client(const ClientOptions&);
}
