// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace mistlane {
// Modernized from d_common/net/TokenBucket.hpp and src/TokenBucket.cpp.
// Single event-loop ownership. A monotonic clock avoids wall-clock jumps.
class TokenBucket {
public:
    TokenBucket(double capacity = 200, double rate = 100)
        : capacity_(capacity), rate_(rate), tokens_(capacity), last_(Clock::now()) {
        if (capacity <= 0 || rate <= 0) throw std::invalid_argument("invalid token bucket");
    }
    bool consume(double count = 1) {
        auto now = Clock::now();
        tokens_ = std::min(capacity_, tokens_ + std::chrono::duration<double>(now-last_).count()*rate_);
        last_ = now;
        if (count < 0 || tokens_ < count) return false;
        tokens_ -= count;
        return true;
    }
private:
    using Clock = std::chrono::steady_clock;
    double capacity_, rate_, tokens_;
    Clock::time_point last_;
};
}
