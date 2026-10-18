// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
namespace mistlane::cli {
int run_lua_tests(const std::vector<std::string>& scripts, const std::string& selected,
                  bool list, unsigned timeout_ms);
}
