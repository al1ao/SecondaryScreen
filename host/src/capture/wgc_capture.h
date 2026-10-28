#pragma once
#include "capture/capture.h"
namespace pds {
class WgcCapture final : public IGpuCapture {
public:
    explicit WgcCapture(bool show_cursor);
    ~WgcCapture() override;
    bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height) override;
    bool Capture(GpuFrame& out) override;
    uint32_t Width() const override;
    uint32_t Height() const override;
    const char* Name() const override { return "WGC primary monitor"; }
    uint64_t Dropped() const;
    bool Failed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
