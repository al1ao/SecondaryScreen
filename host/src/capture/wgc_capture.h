#pragma once
#include "capture/capture.h"
namespace pds {
class WgcCapture final : public IGpuCapture {
public:
    explicit WgcCapture(bool show_cursor);
    ~WgcCapture() override;
    // Capture-item access is independent of adapters and MF. Check it first.
    HRESULT PrepareForMonitor(HMONITOR monitor);
    HRESULT InitializationResult() const;
    bool Initialize(ID3D11Device* device, uint32_t width, uint32_t height) override;
    bool InitializeForMonitor(HMONITOR monitor, uint32_t width, uint32_t height);
    bool InitializeForMonitor(ID3D11Device* device, HMONITOR monitor, uint32_t width, uint32_t height);
    bool ConfigureThrottle(bool enabled, uint32_t target_fps);
    bool Capture(GpuFrame& out) override;
    uint32_t Width() const override;
    uint32_t Height() const override;
    const char* Name() const override { return "WGC monitor capture"; }
    uint64_t Dropped() const;
    uint64_t Throttled() const;
    uint64_t Received() const;
    uint64_t Copied() const;
    bool Failed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
