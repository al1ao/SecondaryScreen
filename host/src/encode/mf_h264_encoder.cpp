#include "encode/mf_h264_encoder.h"
#include <codecapi.h>
#include <strmif.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <dxgi.h>
#include <chrono>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include "encode/gpu_nv12_converter.h"
#include "encode/h264_annex_b.h"
#include "protocol/protocol.h"
#include "util/timer.h"

namespace pds {
namespace {
class MfRuntime {
public:
    MfRuntime() : hr_(MFStartup(MF_VERSION, MFSTARTUP_LITE)) { LOG_HR(hr_, "MFStartup"); }
    ~MfRuntime() { if (SUCCEEDED(hr_)) LOG_HR(MFShutdown(), "MFShutdown"); }
    bool Ready() const { return SUCCEEDED(hr_); }
private:
    HRESULT hr_;
};
struct Activations {
    IMFActivate** items = nullptr;
    UINT32 count = 0;
    ~Activations() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};
bool SetCodecValue(ICodecAPI* api, const GUID& key, ULONG value, bool boolean, const char* name, bool required = true) {
    VARIANT option{};
    option.vt = static_cast<VARTYPE>(boolean ? VT_BOOL : VT_UI4);
    if (boolean) option.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    else option.ulVal = value;
    const auto hr = api->SetValue(&key, &option);
    if (FAILED(hr)) {
        LOG_HR(hr, name);
        if (!required) LOG_WARN("Optional encoder setting unavailable: ", name);
    }
    return SUCCEEDED(hr) || !required;
}
bool ConfigureVideoType(IMFMediaType* type, REFGUID subtype, uint32_t width, uint32_t height, uint32_t fps) {
    return LOG_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF major type") &&
        LOG_HR(type->SetGUID(MF_MT_SUBTYPE, subtype), "MF subtype") &&
        LOG_HR(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "MF progressive") &&
        LOG_HR(MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height), "MF frame size") &&
        LOG_HR(MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1), "MF frame rate") &&
        LOG_HR(MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "MF pixel aspect") &&
        LOG_HR(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "MF BT709 matrix") &&
        LOG_HR(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709), "MF BT709 primaries") &&
        LOG_HR(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709), "MF transfer") &&
        LOG_HR(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "MF limited range");
}
std::string EncoderName(IMFActivate* activate) {
    wchar_t* text = nullptr;
    UINT32 length = 0;
    if (!LOG_HR(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &text, &length), "MF encoder friendly name")) return "MF H.264 hardware";
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(text, &CoTaskMemFree);
    std::string name;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        name.resize(static_cast<size_t>(bytes));
        if (WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), name.data(), bytes, nullptr, nullptr) == 0)
            LOG_HR(LastErrorHr(), "MF encoder name UTF8");
    } else LOG_HR(LastErrorHr(), "MF encoder name length");
    return name;
}
}
bool EnsureMediaFoundation() { static MfRuntime runtime; return runtime.Ready(); }
struct MfH264Encoder::Impl {
    ~Impl() { Reset(); }
    void Reset() {
        if (transform) {
            LOG_HR(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "MF end of stream");
            LOG_HR(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "MF flush");
        }
        events.Reset(); api.Reset();
        if (activation) LOG_HR(activation->ShutdownObject(), "MF hardware ShutdownObject");
        transform.Reset(); activation.Reset(); manager.Reset();
        pending.clear(); submitted.clear(); input_tokens = 0; output_tokens = 0;
        parameters = {}; need_keyframe = false; last_encode_us = 0;
        healthy = false;
    }
    bool TryInitialize(IMFActivate* candidate, ID3D11Device* device) {
        activation = candidate;
        name = EncoderName(candidate);
        LOG_INFO("Probing MF hardware encoder: ", name);
        if (!LOG_HR(candidate->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf())), "MF ActivateObject")) return false;
        ComPtr<IMFAttributes> attributes;
        if (!LOG_HR(transform->GetAttributes(attributes.GetAddressOf()), "MF transform attributes")) return false;
        asynchronous = MFGetAttributeUINT32(attributes.Get(), MF_TRANSFORM_ASYNC, FALSE) != FALSE;
        if (asynchronous && (!LOG_HR(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF async unlock") ||
            !LOG_HR(transform.As(&events), "MF event generator"))) return false;
        if (MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE) == FALSE) {
            LOG_WARN(name, " does not support D3D11 surfaces"); return false;
        }
        if (!LOG_HR(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "MF low latency attribute") ||
            !LOG_HR(transform.As(&api), "MF ICodecAPI")) return false;
        UINT reset_token = 0;
        if (!LOG_HR(MFCreateDXGIDeviceManager(&reset_token, manager.GetAddressOf()), "MF DXGI manager") ||
            !LOG_HR(manager->ResetDevice(device, reset_token), "MF DXGI ResetDevice") ||
            !LOG_HR(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())), "MF D3D manager")) return false;
        // These are actual ICodecAPI properties, not IMFTransform::SetUINT32 calls.
        if (!SetCodecValue(api.Get(), CODECAPI_AVLowLatencyMode, TRUE, true, "CODECAPI_AVLowLatencyMode") ||
            !SetCodecValue(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, false, "MF CBR") ||
            !SetCodecValue(api.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate * 1000u, false, "MF mean bitrate") ||
            !SetCodecValue(api.Get(), CODECAPI_AVEncMPVGOPSize, fps, false, "MF GOP")) return false;
        // Some hardware MFTs do not implement this property. Actual output slice_type
        // is authoritative: ReadOutput rejects B slices even when SetValue succeeds.
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, false,
            "CODECAPI_AVEncMPVDefaultBPictureCount", /*required=*/false);
        SetCodecValue(api.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 0, false, "MF quality vs speed", false);
        SetCodecValue(api.Get(), CODECAPI_AVEncMPVFrameFieldMode, eAVEncMPVFrameFieldMode_FrameMode, false, "MF progressive field mode", false);
        ComPtr<IMFMediaType> output, input;
        // TODO(design): the document's MF_MT_MPEG2_SPS_IN_MPEG_SEQUENCE_HEADER
        // symbol does not exist in the Windows SDK. Read MF_MT_MPEG_SEQUENCE_HEADER
        // from the current type, then explicitly normalize and prepend Annex-B SPS/PPS.
        if (!LOG_HR(MFCreateMediaType(output.GetAddressOf()), "MF output type") ||
            !ConfigureVideoType(output.Get(), MFVideoFormat_H264, width, height, fps) ||
            !LOG_HR(output->SetUINT32(MF_MT_AVG_BITRATE, bitrate * 1000u), "MF output bitrate") ||
            !LOG_HR(output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base), "MF baseline profile") ||
            !LOG_HR(transform->SetOutputType(0, output.Get(), 0), "MF SetOutputType H264") ||
            !LOG_HR(MFCreateMediaType(input.GetAddressOf()), "MF input type") ||
            !ConfigureVideoType(input.Get(), MFVideoFormat_NV12, width, height, fps) ||
            !LOG_HR(transform->SetInputType(0, input.Get(), 0), "MF SetInputType NV12")) return false;
        if (!LOG_HR(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MF begin streaming") ||
            !LOG_HR(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MF start stream")) return false;
        healthy = true;
        RefreshParameters();
        LOG_INFO("Selected MF H.264 hardware: ", name, ", NV12 GPU shader, B=0 best-effort; actual B slices rejected, low-latency=true, ",
            width, 'x', height, " @", fps, "fps bitrate=", bitrate, "kbps");
        return true;
    }
    void RefreshParameters() {
        ComPtr<IMFMediaType> current;
        if (!LOG_HR(transform->GetOutputCurrentType(0, current.GetAddressOf()), "MF current output type")) { healthy = false; return; }
        UINT32 size = 0;
        const auto hr = current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size);
        // Some drivers only expose SPS/PPS after producing the first encoded sample.
        if (hr == MF_E_ATTRIBUTENOTFOUND || size == 0) return;
        if (!LOG_HR(hr, "MF sequence header size") || size > kMaxPayload) { healthy = false; return; }
        std::vector<uint8_t> blob(size);
        if (!LOG_HR(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), size, nullptr), "MF sequence header")) { healthy = false; return; }
        if (!ReadH264Parameters(blob, parameters)) LOG_DEBUG("MF sequence header not ready; checking in-band SPS/PPS");
    }
    bool ReadOutput(unsigned format_retries = 2) {
        MFT_OUTPUT_STREAM_INFO info{};
        if (!LOG_HR(transform->GetOutputStreamInfo(0, &info), "MF output stream info")) { healthy = false; return false; }
        ComPtr<IMFSample> provided;
        if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            ComPtr<IMFMediaBuffer> buffer;
            if (!LOG_HR(MFCreateSample(provided.GetAddressOf()), "MF output sample") ||
                !LOG_HR(MFCreateAlignedMemoryBuffer((std::max)(info.cbSize, DWORD{1024u * 1024u}),
                    info.cbAlignment ? info.cbAlignment - 1 : 0, buffer.GetAddressOf()), "MF aligned output buffer") ||
                !LOG_HR(provided->AddBuffer(buffer.Get()), "MF output AddBuffer")) { healthy = false; return false; }
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = provided.Get();
        DWORD status = 0;
        const auto hr = transform->ProcessOutput(0, 1, &output, &status);
        ComPtr<IMFCollection> output_events;
        output_events.Attach(output.pEvents);
        ComPtr<IMFSample> owned;
        if (output.pSample && output.pSample != provided.Get()) owned.Attach(output.pSample);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type;
            if (!LOG_HR(transform->GetOutputAvailableType(0, 0, type.GetAddressOf()), "MF changed output type") ||
                !LOG_HR(transform->SetOutputType(0, type.Get(), 0), "MF accept changed output type")) healthy = false;
            else {
                RefreshParameters();
                if (healthy && format_retries > 0) return ReadOutput(format_retries - 1);
            }
            if (healthy) { LOG_ERROR("MF repeatedly changed output type"); healthy = false; }
            return false;
        }
        if (!LOG_HR(hr, "MF ProcessOutput") || !output.pSample) { healthy = false; return false; }
        ComPtr<IMFMediaBuffer> contiguous;
        if (!LOG_HR(output.pSample->ConvertToContiguousBuffer(contiguous.GetAddressOf()), "MF contiguous output")) { healthy = false; return false; }
        LONGLONG sample_time = 0;
        if (!LOG_HR(output.pSample->GetSampleTime(&sample_time), "MF output PTS")) { healthy = false; return false; }
        BYTE* bytes = nullptr;
        DWORD length = 0;
        if (!LOG_HR(contiguous->Lock(&bytes, nullptr, &length), "MF output Lock")) { healthy = false; return false; }
        std::vector<uint8_t> copy;
        try { copy.assign(bytes, bytes + length); }
        catch (...) { LOG_HR(contiguous->Unlock(), "MF output Unlock after exception"); throw; }
        if (!LOG_HR(contiguous->Unlock(), "MF output Unlock")) { healthy = false; return false; }
        RefreshParameters();
        EncodedFrame frame;
        frame.width = width; frame.height = height;
        // MF sample times are 100 ns: microseconds * 10 on input, / 10 on output.
        frame.capture_ts_us = static_cast<uint64_t>(sample_time / 10);
        const auto found = submitted.find(sample_time);
        if (found != submitted.end()) { last_encode_us = NowUs() - found->second; submitted.erase(found); }
        if (!MakeH264AccessUnit(copy, parameters, frame.data, frame.keyframe)) {
            LOG_WARN("Dropped H264 output without complete SPS/PPS+IDR or valid Annex-B access unit");
            need_keyframe = true;
            return false;
        }
        if (frame.data.size() > kMaxPayload - sizeof(FrameInfo)) { healthy = false; LOG_ERROR("H264 output exceeds protocol limit"); return false; }
        if (HasH264BSlice(frame.data)) { healthy = false; LOG_ERROR("Actual H264 bitstream contains B slices; rejecting hardware backend: ", name); return false; }
        pending.push_back(std::move(frame));
        return true;
    }
    void PumpEvents() {
        if (!healthy) return;
        if (!asynchronous) {
            for (unsigned n = 0; n < 8 && healthy; ++n) if (!ReadOutput()) break;
            return;
        }
        for (unsigned n = 0; n < 64 && healthy; ++n) {
            ComPtr<IMFMediaEvent> event;
            const auto hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.GetAddressOf());
            if (hr == MF_E_NO_EVENTS_AVAILABLE) break;
            if (!LOG_HR(hr, "MF GetEvent")) { healthy = false; break; }
            HRESULT event_status = S_OK;
            MediaEventType type = MEUnknown;
            if (!LOG_HR(event->GetStatus(&event_status), "MF event status") || !LOG_HR(event_status, "MF hardware event") ||
                !LOG_HR(event->GetType(&type), "MF event type")) { healthy = false; break; }
            if (type == METransformNeedInput) ++input_tokens;
            else if (type == METransformHaveOutput) ++output_tokens;
            else if (type == MEError) { LOG_ERROR("MF hardware encoder signaled MEError"); healthy = false; }
        }
        while (healthy && output_tokens > 0) { --output_tokens; ReadOutput(); }
    }
    bool TakeOutput(EncodedFrame& out) {
        if (pending.empty()) return false;
        out = std::move(pending.front()); pending.pop_front(); return true;
    }
    ComPtr<IMFActivate> activation;
    ComPtr<IMFTransform> transform;
    ComPtr<ICodecAPI> api;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFDXGIDeviceManager> manager;
    GpuNv12Converter converter;
    H264Parameters parameters;
    std::deque<EncodedFrame> pending;
    std::unordered_map<LONGLONG, uint64_t> submitted;
    std::string name = "MF H.264 hardware";
    uint32_t width = 0, height = 0, fps = 0, bitrate = 0;
    unsigned input_tokens = 0, output_tokens = 0;
    bool asynchronous = false, healthy = false, need_keyframe = false;
    uint64_t last_encode_us = 0;
};
MfH264Encoder::MfH264Encoder() : impl_(std::make_unique<Impl>()) {}
MfH264Encoder::~MfH264Encoder() = default;
bool MfH264Encoder::Initialize(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps) {
    if (!device || width < 2 || height < 2 || width % 2 || height % 2 || fps == 0 ||
        bitrate_kbps == 0 || bitrate_kbps > 100000 || !EnsureMediaFoundation()) return false;
    impl_->Reset();
    impl_->width = width; impl_->height = height; impl_->fps = fps; impl_->bitrate = bitrate_kbps;
    if (!impl_->converter.Initialize(device, width, height)) return false;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    ComPtr<IMFAttributes> enumeration;
    if (!LOG_HR(device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf())), "MF device IDXGIDevice") ||
        !LOG_HR(dxgi_device->GetAdapter(adapter.GetAddressOf()), "MF device adapter") ||
        !LOG_HR(adapter->GetDesc(&adapter_desc), "MF device adapter description") ||
        !LOG_HR(MFCreateAttributes(enumeration.GetAddressOf(), 1), "MF enumeration attributes") ||
        !LOG_HR(enumeration->SetBlob(MFT_ENUM_ADAPTER_LUID,
            reinterpret_cast<const UINT8*>(&adapter_desc.AdapterLuid), sizeof(LUID)), "MF enumeration adapter LUID")) return false;
    // MF_SA_D3D11_ADAPTER_LUID is not a Windows SDK attribute. MFTEnum2 filters
    // hardware candidates by the device's LUID before any ActivateObject call.
    Activations candidates;
    if (!LOG_HR(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, enumeration.Get(), &candidates.items, &candidates.count), "MFTEnum2 H264 hardware for adapter")) return false;
    LOG_INFO("MF hardware H264 candidates=", candidates.count, " adapter LUID=", adapter_desc.AdapterLuid.HighPart,
        ':', adapter_desc.AdapterLuid.LowPart, " vendor=", adapter_desc.VendorId);
    for (UINT32 i = 0; i < candidates.count; ++i) {
        if (impl_->TryInitialize(candidates.items[i], device)) return true;
        impl_->Reset();
    }
    return false;
}
bool MfH264Encoder::ForceKeyframe() {
    if (!impl_->healthy) return false;
    return SetCodecValue(impl_->api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1, false, "MF force keyframe");
}
bool MfH264Encoder::Encode(ID3D11Texture2D* input, uint64_t capture_ts_us, EncodedFrame& out) {
    auto& p = *impl_;
    if (!p.healthy || !input) return false;
    p.PumpEvents();
    // A previous asynchronous output must not consume this call's raw input.
    // Submit the new input first, then return the oldest completed output.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    while (p.healthy && p.asynchronous && p.input_tokens == 0 && std::chrono::steady_clock::now() < deadline) {
        p.PumpEvents();
        if (p.input_tokens == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!p.healthy) return false;
    if (p.asynchronous && p.input_tokens == 0) return p.TakeOutput(out);
    if (p.need_keyframe) { if (!ForceKeyframe()) { p.healthy = false; return false; } p.need_keyframe = false; }
    const auto started = NowUs();
    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    if (!p.converter.Convert(input, nv12) ||
        !LOG_HR(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12.Get(), 0, FALSE, buffer.GetAddressOf()), "MF DXGI surface buffer") ||
        !LOG_HR(MFCreateSample(sample.GetAddressOf()), "MF input sample") ||
        !LOG_HR(sample->AddBuffer(buffer.Get()), "MF input AddBuffer")) { p.healthy = false; return false; }
    const auto pts = static_cast<LONGLONG>(capture_ts_us * 10u);
    if (!LOG_HR(sample->SetSampleTime(pts), "MF input PTS") ||
        !LOG_HR(sample->SetSampleDuration(10000000LL / p.fps), "MF input duration")) { p.healthy = false; return false; }
    const auto hr = p.transform->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) { p.PumpEvents(); return p.TakeOutput(out); }
    if (!LOG_HR(hr, "MF ProcessInput")) { p.healthy = false; return false; }
    if (p.asynchronous) --p.input_tokens;
    p.submitted.emplace(pts, started);
    if (p.submitted.size() > 16) { LOG_ERROR("MF encoder stalled with too many in-flight samples"); p.healthy = false; return false; }
    if (p.TakeOutput(out)) return true;
    const auto output_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    do {
        p.PumpEvents();
        if (p.TakeOutput(out)) return true;
        if (!p.healthy) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < output_deadline);
    return false;
}
bool MfH264Encoder::Poll(EncodedFrame& out) { impl_->PumpEvents(); return impl_->TakeOutput(out); }
bool MfH264Encoder::Healthy() const { return impl_->healthy; }
uint64_t MfH264Encoder::LastEncodeTimeUs() const { return impl_->last_encode_us; }
const char* MfH264Encoder::Name() const { return impl_->name.c_str(); }
std::unique_ptr<IGpuVideoEncoder> CreateMfH264Encoder() { return std::make_unique<MfH264Encoder>(); }
}
