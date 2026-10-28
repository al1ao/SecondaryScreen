#pragma once
#include "encode/encoder.h"
namespace pds {
bool EnsureMediaFoundation();
class MfH264Encoder final : public IGpuVideoEncoder {
public:
    MfH264Encoder();
    ~MfH264Encoder() override;
    bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height,
                    uint32_t fps, uint32_t bitrate_kbps) override;
    bool Encode(ID3D11Texture2D* input, uint64_t capture_ts_us, EncodedFrame& out) override;
    bool ForceKeyframe() override;
    Codec Type() const override { return Codec::H264; }
    const char* Name() const override;
    bool Healthy() const;
    bool Poll(EncodedFrame& out);
    uint64_t LastEncodeTimeUs() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
