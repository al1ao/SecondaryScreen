#include "util/gpu.h"
#include <dxgi.h>
namespace pds {
bool CreateCaptureDevice(ComPtr<ID3D11Device>& device) {
    ComPtr<ID3D11DeviceContext> context;
    if (!LOG_HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), nullptr,
        context.GetAddressOf()), "D3D11CreateDevice hardware")) return false;
    ComPtr<ID3D10Multithread> multithread;
    if (!LOG_HR(context.As(&multithread), "ID3D10Multithread")) return false;
    multithread->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (!LOG_HR(device.As(&dxgi), "DXGI device") || !LOG_HR(dxgi->GetAdapter(adapter.GetAddressOf()), "DXGI adapter") ||
        !LOG_HR(adapter->GetDesc(&desc), "DXGI adapter description")) return false;
    LOG_INFO("D3D11 hardware adapter vendor=0x", std::hex, desc.VendorId, " device=0x", desc.DeviceId);
    return true;
}
bool CreateCaptureDeviceForAdapter(unsigned index, ComPtr<ID3D11Device>& device) {
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (!LOG_HR(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "DXGI factory")) return false;
    const auto hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
    if (hr == DXGI_ERROR_NOT_FOUND) return false;
    if (!LOG_HR(hr, "DXGI EnumAdapters1")) return false;
    DXGI_ADAPTER_DESC1 desc{};
    if (!LOG_HR(adapter->GetDesc1(&desc), "DXGI adapter description")) return false;
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return false;
    ComPtr<ID3D11DeviceContext> context;
    if (!LOG_HR(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), nullptr,
        context.GetAddressOf()), "D3D11 hardware adapter device")) return false;
    ComPtr<ID3D10Multithread> multithread;
    if (!LOG_HR(context.As(&multithread), "D3D multithread protection")) return false;
    multithread->SetMultithreadProtected(TRUE);
    LOG_INFO("D3D11 adapter index=", index, " vendor=0x", std::hex, desc.VendorId, " device=0x", desc.DeviceId);
    return true;
}
}
