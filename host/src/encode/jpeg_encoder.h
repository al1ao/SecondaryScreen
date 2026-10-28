#pragma once
#include <wincodec.h>
#include "encode/encoder.h"
#include "util/com_ptr.h"
namespace pds {
class JpegEncoder final : public IVideoEncoder {
public:
    explicit JpegEncoder(int quality) : quality_(quality) {}
    bool Initialize(uint32_t width, uint32_t height, uint32_t fps) override;
    bool Encode(const CapturedFrame& in, EncodedFrame& out) override;
    Codec Type() const override { return Codec::JPEG; }
    const char* Name() const override { return "WIC JPEG"; }
private:
    int quality_;
    uint32_t width_ = 0, height_ = 0;
    ComPtr<IWICImagingFactory> factory_;
};
}
