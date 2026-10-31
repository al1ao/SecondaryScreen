#include "util/frame_dump.h"
#include <wincodec.h>
#include <cstring>
#include <string>
#include <vector>
#include "util/com_ptr.h"
#include "util/gpu.h"
#include "util/log.h"
#include "util/timer.h"
namespace pds {
namespace {
bool WritePng(const CapturedFrame& frame, const std::filesystem::path& directory,
              const char* name, uint32_t orientation) {
    if (frame.width == 0 || frame.height == 0 ||
        frame.bgra.size() != static_cast<size_t>(frame.width) * frame.height * 4) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { LOG_ERROR("Frame dump directory: ", error.message()); return false; }
    const auto file = directory / (std::string(name) + "-dmdo" + std::to_string(orientation) +
        "-" + std::to_string(NowUs()) + ".png");
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> image;
    if (!LOG_HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf())), "dump WIC factory") ||
        !LOG_HR(factory->CreateStream(stream.GetAddressOf()), "dump WIC stream") ||
        !LOG_HR(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE), "dump PNG file") ||
        !LOG_HR(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()), "dump PNG encoder") ||
        !LOG_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "dump PNG initialize") ||
        !LOG_HR(encoder->CreateNewFrame(image.GetAddressOf(), nullptr), "dump PNG frame") ||
        !LOG_HR(image->Initialize(nullptr), "dump PNG frame initialize") ||
        !LOG_HR(image->SetSize(frame.width, frame.height), "dump PNG size")) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (!LOG_HR(image->SetPixelFormat(&format), "dump PNG pixel format") ||
        !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) ||
        !LOG_HR(image->WritePixels(frame.height, frame.width * 4,
            static_cast<UINT>(frame.bgra.size()), const_cast<BYTE*>(frame.bgra.data())), "dump PNG pixels") ||
        !LOG_HR(image->Commit(), "dump PNG frame commit") ||
        !LOG_HR(encoder->Commit(), "dump PNG commit")) return false;
    LOG_INFO("First frame dumped: ", file.string(), " ", frame.width, 'x', frame.height);
    return true;
}
}
bool DumpFirstFrame(const CapturedFrame& frame, const std::filesystem::path& directory,
                    const char* path_name, uint32_t orientation) {
    return WritePng(frame, directory, path_name, orientation);
}
bool DumpFirstFrame(const GpuFrame& frame, const std::filesystem::path& directory,
                    const char* path_name, uint32_t orientation) {
    if (!frame.texture) return false;
    D3D11_TEXTURE2D_DESC description{}; frame.texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return false;
    ComPtr<ID3D11Device> device; frame.texture->GetDevice(device.GetAddressOf());
    if (!device) return false;
    ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(context.GetAddressOf());
    if (!context) return false;
    auto staging_description = description;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (!LOG_HR(device->CreateTexture2D(&staging_description, nullptr, staging.GetAddressOf()), "dump staging texture")) return false;
    CapturedFrame cpu; cpu.width = description.Width; cpu.height = description.Height;
    cpu.bgra.resize(static_cast<size_t>(cpu.width) * cpu.height * 4);
    {
        GpuContextLock lock(context.Get());
        context->CopyResource(staging.Get(), frame.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!LOG_HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "dump staging Map")) return false;
        for (uint32_t row = 0; row < cpu.height; ++row)
            std::memcpy(cpu.bgra.data() + static_cast<size_t>(row) * cpu.width * 4,
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
                static_cast<size_t>(cpu.width) * 4);
        context->Unmap(staging.Get(), 0);
    }
    return WritePng(cpu, directory, path_name, orientation);
}
}
