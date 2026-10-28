#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace pds {
struct CapturedFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t capture_ts_us = 0;
    // BGRA8, tightly packed, stride == width * 4.
    std::vector<uint8_t> bgra;
};
class IDesktopCapture {
public:
    virtual ~IDesktopCapture() = default;
    virtual bool Initialize(uint32_t width, uint32_t height) = 0;
    virtual bool Capture(CapturedFrame& out) = 0;
    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;
    virtual const char* Name() const = 0;
};
std::unique_ptr<IDesktopCapture> CreateGdiCapture();
}
