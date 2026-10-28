#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

namespace pds {
// Fixed memory. Quantiles are conservative 10us bucket upper bounds. The
// overflow bucket returns the observed maximum, never an optimistic clipped P95.
class TimingStats {
public:
    struct Sample { uint64_t count = 0, total_us = 0, p95_upper_us = 0; };
    struct Snapshot { Sample window, session; };
    void Record(uint64_t us) {
        std::lock_guard lock(mutex_);
        const auto index = static_cast<size_t>((std::min)(us / 10 + (us % 10 != 0), uint64_t{1024}));
        for (auto* accumulator : {&window_, &session_}) {
            ++accumulator->count; accumulator->total_us += us;
            accumulator->maximum_us = (std::max)(accumulator->maximum_us, us);
            ++accumulator->histogram[index];
        }
    }
    Snapshot TakeWindow() {
        std::lock_guard lock(mutex_);
        const Snapshot result{Summarize(window_), Summarize(session_)};
        window_ = {};
        return result;
    }
private:
    struct Accumulator {
        std::array<uint64_t, 1025> histogram{};
        uint64_t count = 0, total_us = 0, maximum_us = 0;
    };
    static Sample Summarize(const Accumulator& input) {
        Sample result{input.count, input.total_us, 0};
        if (!input.count) return result;
        const auto rank = input.count - input.count / 20; // ceil(0.95 * n)
        uint64_t seen = 0;
        for (size_t i = 0; i < input.histogram.size(); ++i) {
            seen += input.histogram[i];
            if (seen >= rank) {
                result.p95_upper_us = i == 1024 ? input.maximum_us : static_cast<uint64_t>(i) * 10;
                break;
            }
        }
        return result;
    }
    std::mutex mutex_;
    Accumulator window_, session_;
};
}
