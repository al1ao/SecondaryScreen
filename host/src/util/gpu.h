#pragma once
#include <d3d11.h>
#include <d3d10.h>
#include "util/com_ptr.h"
namespace pds {
bool CreateCaptureDevice(ComPtr<ID3D11Device>& device);
bool CreateCaptureDeviceForAdapter(unsigned index, ComPtr<ID3D11Device>& device);
// WGC callbacks and MF use the shared immediate context from different threads.
class GpuContextLock {
public:
    explicit GpuContextLock(ID3D11DeviceContext* context) {
        if (LOG_HR(context->QueryInterface(IID_PPV_ARGS(lock_.GetAddressOf())), "D3D multithread lock")) lock_->Enter();
    }
    ~GpuContextLock() { if (lock_) lock_->Leave(); }
    GpuContextLock(const GpuContextLock&) = delete;
    GpuContextLock& operator=(const GpuContextLock&) = delete;
private:
    ComPtr<ID3D10Multithread> lock_;
};
}
