#include "encode/jpeg_encoder.h"
#include <cstring>
#include <limits>
#include "protocol/protocol.h"

namespace pds {
bool JpegEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps) {
    if (width == 0 || height == 0 || fps == 0 || quality_ < 1 || quality_ > 100 ||
        static_cast<uint64_t>(width) * height * 4 > (std::numeric_limits<UINT>::max)()) return false;
    width_ = width; height_ = height;
    factory_.Reset();
    return LOG_HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory_.GetAddressOf())), "create WIC factory");
}
bool JpegEncoder::Encode(const CapturedFrame& in, EncodedFrame& out) {
    if (!factory_ || in.width != width_ || in.height != height_ ||
        in.bgra.size() != static_cast<size_t>(width_) * height_ * 4) return false;
    ComPtr<IStream> memory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (!LOG_HR(CreateStreamOnHGlobal(nullptr, TRUE, memory.GetAddressOf()), "CreateStreamOnHGlobal") ||
        !LOG_HR(factory_->CreateStream(stream.GetAddressOf()), "WIC CreateStream") ||
        !LOG_HR(stream->InitializeFromIStream(memory.Get()), "WIC InitializeFromIStream") ||
        !LOG_HR(factory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.GetAddressOf()), "WIC JPEG encoder") ||
        !LOG_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "WIC encoder Initialize") ||
        !LOG_HR(encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf()), "WIC CreateNewFrame")) return false;
    PROPBAG2 option{};
    wchar_t quality_name[] = L"ImageQuality";
    option.pstrName = quality_name;
    VARIANT value{};
    value.vt = VT_R4;
    value.fltVal = static_cast<float>(quality_) / 100.0f;
    if (!LOG_HR(properties->Write(1, &option, &value), "WIC ImageQuality") ||
        !LOG_HR(frame->Initialize(properties.Get()), "WIC frame Initialize") ||
        !LOG_HR(frame->SetSize(width_, height_), "WIC SetSize")) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (!LOG_HR(frame->SetPixelFormat(&format), "WIC SetPixelFormat")) return false;
    if (!IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
        LOG_ERROR("Unexpected WIC JPEG pixel format"); return false;
    }
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    // WIC handles BGRA -> BGR; GDI's unused alpha byte must not darken the image.
    if (!LOG_HR(factory_->CreateBitmapFromMemory(width_, height_, GUID_WICPixelFormat32bppBGRA,
        width_ * 4, static_cast<UINT>(in.bgra.size()), const_cast<BYTE*>(in.bgra.data()), bitmap.GetAddressOf()), "WIC BGRA bitmap") ||
        !LOG_HR(factory_->CreateFormatConverter(converter.GetAddressOf()), "WIC format converter") ||
        !LOG_HR(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom), "WIC BGRA to BGR") ||
        !LOG_HR(frame->WriteSource(converter.Get(), nullptr), "WIC WriteSource") ||
        !LOG_HR(frame->Commit(), "WIC frame Commit") ||
        !LOG_HR(encoder->Commit(), "WIC encoder Commit")) return false;
    STATSTG stat{};
    HGLOBAL global = nullptr;
    if (!LOG_HR(memory->Stat(&stat, STATFLAG_NONAME), "JPEG stream Stat") ||
        !LOG_HR(GetHGlobalFromStream(memory.Get(), &global), "GetHGlobalFromStream")) return false;
    const auto size = stat.cbSize.QuadPart;
    if (size < 4 || size > kMaxPayload - sizeof(FrameInfo)) {
        LOG_WARN("JPEG exceeds protocol frame size limit: ", size); return false;
    }
    const auto* data = static_cast<const uint8_t*>(GlobalLock(global));
    if (!data) { LOG_HR(LastErrorHr(), "GlobalLock JPEG"); return false; }
    // Copy out before unlocking; the IStream owns and releases HGLOBAL on every path.
    try { out.data.assign(data, data + static_cast<size_t>(size)); }
    catch (...) { GlobalUnlock(global); throw; }
    SetLastError(ERROR_SUCCESS);
    if (!GlobalUnlock(global) && GetLastError() != ERROR_SUCCESS) {
        LOG_HR(LastErrorHr(), "GlobalUnlock JPEG"); return false;
    }
    if (out.data[0] != 0xFF || out.data[1] != 0xD8 ||
        out.data[out.data.size() - 2] != 0xFF || out.data.back() != 0xD9) return false;
    out.width = width_; out.height = height_; out.capture_ts_us = in.capture_ts_us;
    out.keyframe = true;
    return true;
}
std::unique_ptr<IVideoEncoder> CreateJpegEncoder(int quality) { return std::make_unique<JpegEncoder>(quality); }
}
