#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdexcept>
#include "encode/encoder.h"
#include "encode/h264_annex_b.h"
#include "util/com_ptr.h"
namespace pds::test {
// Independent system software decoder for validating hardware output. Not a host backend.
class MfDecoder {
public:
    ~MfDecoder() {
        if (decoder_) LOG_HR(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "test decoder flush");
        if (activation_) LOG_HR(activation_->ShutdownObject(), "test decoder shutdown");
    }
    bool Feed(const EncodedFrame& frame, std::vector<uint8_t>* decoded = nullptr) {
        if (!decoder_ && !Initialize(frame)) return false;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if (!LOG_HR(MFCreateSample(sample.GetAddressOf()), "test decoder input sample") ||
            !LOG_HR(MFCreateMemoryBuffer(static_cast<DWORD>(frame.data.size()), buffer.GetAddressOf()), "test decoder input buffer")) return false;
        BYTE* bytes = nullptr;
        if (!LOG_HR(buffer->Lock(&bytes, nullptr, nullptr), "test decoder Lock")) return false;
        std::memcpy(bytes, frame.data.data(), frame.data.size());
        if (!LOG_HR(buffer->Unlock(), "test decoder Unlock") ||
            !LOG_HR(buffer->SetCurrentLength(static_cast<DWORD>(frame.data.size())), "test decoder input length") ||
            !LOG_HR(sample->AddBuffer(buffer.Get()), "test decoder AddBuffer") ||
            !LOG_HR(sample->SetSampleTime(static_cast<LONGLONG>(frame.capture_ts_us * 10)), "test decoder input PTS") ||
            !LOG_HR(sample->SetSampleDuration(10000000 / 60), "test decoder duration") ||
            !LOG_HR(decoder_->ProcessInput(0, sample.Get(), 0), "test decoder ProcessInput")) return false;
        return DrainAvailable(decoded);
    }
    bool Finish(std::vector<uint8_t>* decoded = nullptr) {
        if (!LOG_HR(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "test decoder end of stream") ||
            !LOG_HR(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "test decoder drain command")) return false;
        for (unsigned i = 0; i < 16; ++i) {
            const auto before = frames_;
            if (!DrainAvailable(decoded)) return false;
            if (before == frames_) break;
        }
        return true;
    }
    bool DrainAvailable(std::vector<uint8_t>* decoded = nullptr) {
        BYTE* bytes = nullptr;
        for (unsigned tries = 0; tries < 16; ++tries) {
            MFT_OUTPUT_STREAM_INFO info{};
            if (!LOG_HR(decoder_->GetOutputStreamInfo(0, &info), "test decoder output info")) return false;
            ComPtr<IMFSample> supplied;
            if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
                ComPtr<IMFMediaBuffer> memory;
                if (!LOG_HR(MFCreateSample(supplied.GetAddressOf()), "test decoded sample") ||
                    !LOG_HR(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, memory.GetAddressOf()), "test decoder aligned buffer") ||
                    !LOG_HR(supplied->AddBuffer(memory.Get()), "test decoder output AddBuffer")) return false;
            }
            MFT_OUTPUT_DATA_BUFFER output{}; output.pSample = supplied.Get();
            DWORD flags = 0;
            const auto hr = decoder_->ProcessOutput(0, 1, &output, &flags);
            ComPtr<IMFCollection> events; events.Attach(output.pEvents);
            ComPtr<IMFSample> owned;
            if (output.pSample && output.pSample != supplied.Get()) owned.Attach(output.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (!SetOutput()) return false; continue; }
            if (!LOG_HR(hr, "test H264 decode ProcessOutput") || !output.pSample) return false;
            ++frames_;
            if (decoded) {
                ComPtr<IMFMediaBuffer> contiguous;
                if (!LOG_HR(output.pSample->ConvertToContiguousBuffer(contiguous.GetAddressOf()), "test decoded contiguous")) return false;
                DWORD length = 0;
                if (!LOG_HR(contiguous->Lock(&bytes, nullptr, &length), "test decoded pixels Lock")) return false;
                try { decoded->assign(bytes, bytes + length); }
                catch (...) { LOG_HR(contiguous->Unlock(), "test decoded pixels Unlock"); throw; }
                if (!LOG_HR(contiguous->Unlock(), "test decoded pixels Unlock")) return false;
            }
        }
        return true;
    }
    uint64_t Frames() const { return frames_; }
    uint32_t Stride() const { return stride_; }
private:
    bool Initialize(const EncodedFrame& frame) {
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_H264};
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_NV12};
        IMFActivate** candidates = nullptr;
        UINT32 count = 0;
        if (!LOG_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &input, &output, &candidates, &count), "test enumerate software H264 decoder")) return false;
        if (count) activation_ = candidates[0];
        for (UINT32 i = 0; i < count; ++i) candidates[i]->Release();
        CoTaskMemFree(candidates);
        if (!activation_ || !LOG_HR(activation_->ActivateObject(IID_PPV_ARGS(decoder_.GetAddressOf())), "test activate H264 decoder")) return false;
        ComPtr<IMFMediaType> type;
        H264Parameters parameters;
        if (!ReadH264Parameters(frame.data, parameters) ||
            !LOG_HR(MFCreateMediaType(type.GetAddressOf()), "test H264 media type") ||
            !LOG_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "test decoder major type") ||
            !LOG_HR(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "test decoder subtype") ||
            !LOG_HR(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, frame.width, frame.height), "test decoder dimensions") ||
            !LOG_HR(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 60, 1), "test decoder frame rate") ||
            !LOG_HR(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "test decoder progressive") ||
            !LOG_HR(type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, parameters.sps_pps.data(), static_cast<UINT32>(parameters.sps_pps.size())), "test decoder SPS/PPS") ||
            !LOG_HR(decoder_->SetInputType(0, type.Get(), 0), "test decoder SetInputType") || !SetOutput() ||
            !LOG_HR(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "test decoder begin") ||
            !LOG_HR(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "test decoder start")) return false;
        return true;
    }
    bool SetOutput() {
        for (DWORD index = 0; index < 64; ++index) {
            ComPtr<IMFMediaType> type;
            const auto hr = decoder_->GetOutputAvailableType(0, index, type.GetAddressOf());
            if (hr == MF_E_NO_MORE_TYPES) return false;
            if (!LOG_HR(hr, "test decoder output type")) return false;
            GUID subtype{};
            if (!LOG_HR(type->GetGUID(MF_MT_SUBTYPE, &subtype), "test decoder output subtype")) return false;
            if (subtype != MFVideoFormat_NV12) continue;
            if (!LOG_HR(decoder_->SetOutputType(0, type.Get(), 0), "test decoder SetOutputType NV12")) return false;
            stride_ = MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, 0);
            if (!stride_) {
                UINT32 width = 0, height = 0;
                if (!LOG_HR(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "test decoder output size")) return false;
                stride_ = width;
            }
            return true;
        }
        return false;
    }
    ComPtr<IMFActivate> activation_;
    ComPtr<IMFTransform> decoder_;
    uint64_t frames_ = 0;
    uint32_t stride_ = 0;
};
}
