// SPDX-License-Identifier: Apache-2.0
#include "mistlane/http_client.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 6) throw std::runtime_error("usage: mistlane-http-probe URL [BODY [CA_FILE [MAX_BYTES [TIMEOUT_MS]]]]");
        mistlane::HttpRequest request;
        request.url = argv[1];
        if (argc > 2) { request.method = "POST"; request.body = argv[2]; }
        if (argc > 3) request.ca_file = argv[3];
        if (argc > 4) request.max_response_bytes = std::stoul(argv[4]);
        if (argc > 5) request.timeout_ms = std::stol(argv[5]);
        auto response = mistlane::http_request(request);
        std::cout << response.status << '\n' << response.body;
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
