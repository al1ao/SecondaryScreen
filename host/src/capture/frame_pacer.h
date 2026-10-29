#pragma once
#include <cstdint>
#include <stdexcept>
namespace pds {
// Called under the capture-state lock. Time is steady_clock microseconds.
class FramePacer {
public:
    FramePacer(bool enabled = true, uint32_t fps = 60) : enabled_(enabled), fps_(fps) {
        if (fps == 0 || fps > 165) throw std::invalid_argument("capture fps must be 1..165");
    }
    bool Accept(uint64_t now_us) {
        if (!enabled_) return true;
        const uint64_t now = now_us * fps_;
        // TODO(design): accumulate rational frame slots rather than restarting
        // from each accepted callback (which would quantize 144Hz -> 48fps).
        // 1ms tolerance absorbs scheduling jitter on sources already near 60Hz.
        if (next_ != 0 && now + 1000u * fps_ < next_) return false;
        if (next_ == 0 || now >= next_ + 1000000u) next_ = now;
        next_ += 1000000u;
        return true;
    }
private:
    bool enabled_;
    uint32_t fps_;
    uint64_t next_ = 0;
};
}
