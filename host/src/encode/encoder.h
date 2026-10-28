#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "capture/capture.h"

namespace pds {
enum class Codec : uint8_t { JPEG = 1, H264 = 2, HEVC = 3 };
struct EncodedFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t capture_ts_us = 0;
    bool keyframe = false;
    std::vector<uint8_t> data;
};
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual bool Initialize(uint32_t width, uint32_t height, uint32_t fps) = 0;
    virtual bool Encode(const CapturedFrame& in, EncodedFrame& out) = 0;
    virtual Codec Type() const = 0;
    virtual const char* Name() const = 0;
};
std::unique_ptr<IVideoEncoder> CreateJpegEncoder(int quality /*1-100*/);
}

#ifdef _WIN32
namespace pds {
class IGpuVideoEncoder {
public:
    virtual ~IGpuVideoEncoder() = default;
    virtual bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height,
                            uint32_t fps, uint32_t bitrate_kbps) = 0;
    virtual bool Encode(ID3D11Texture2D* input, uint64_t capture_ts_us, EncodedFrame& out) = 0;
    virtual bool ForceKeyframe() = 0;
    virtual Codec Type() const = 0;
    virtual const char* Name() const = 0;
};
std::unique_ptr<IGpuVideoEncoder> CreateMfH264Encoder();
}
#endif
