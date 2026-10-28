#include "net/frame_sender.h"
#include <roapi.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <stdexcept>
#include "capture/wgc_capture.h"
#include "encode/mf_h264_encoder.h"
#include "encode/gpu_nv12_converter.h"
#include "encode/h264_annex_b.h"
#include "util/gpu.h"
#include "util/timer.h"
#include "mf_test_decoder.h"
namespace pds {
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void AnnexBTest() {
    const std::vector<uint8_t> sequence{0,0,0,1,0x67,0x42,0x00,0x1E,0x80,0,0,1,0x68,0xCE,0x38,0x80};
    H264Parameters parameters;
    Require(ReadH264Parameters(sequence, parameters), "Annex-B SPS/PPS");
    const std::vector<uint8_t> avcc{1,0x42,0,0x1E,0xFF,0xE1,0,5,0x67,0x42,0,0x1E,0x80,1,0,4,0x68,0xCE,0x38,0x80};
    Require(ReadH264Parameters(avcc, parameters) && parameters.length_size == 4, "AVCDecoderConfigurationRecord");
    const std::vector<uint8_t> idr{0,0,0,3,0x65,0x88,0x84};
    std::vector<uint8_t> output;
    bool keyframe = false;
    for (int i = 0; i < 3; ++i) {
        Require(MakeH264AccessUnit(idr, parameters, output, keyframe) && keyframe, "AVCC IDR to Annex-B");
        Require(output.size() > 4 && output[0] == 0 && output[1] == 0 && output[2] == 0 && output[3] == 1 &&
            HasH264Nal(output, 7) && HasH264Nal(output, 8) && HasH264Nal(output, 5), "every IDR contains SPS PPS IDR");
    }
    const std::vector<uint8_t> delta{0,0,1,0x41,0x9A,0x80};
    Require(MakeH264AccessUnit(delta, parameters, output, keyframe) && !keyframe && HasH264Nal(output, 1), "non-keyframe");
    H264Parameters empty;
    Require(!MakeH264AccessUnit(idr, empty, output, keyframe), "IDR without SPS/PPS rejected");
    const std::vector<uint8_t> truncated{0,0,0,10,0x65};
    Require(!NormalizeH264(truncated, 4, output), "truncated AVCC rejected");
    Require(!ReadH264Parameters(std::span(avcc).first(10), parameters), "truncated avcC rejected");
    Require(HasH264BSlice(std::vector<uint8_t>{0,0,0,1,0x41,0xA8}), "B slice recognized");
    Require(!HasH264BSlice(std::vector<uint8_t>{0,0,0,1,0x41,0xE0}), "P slice is not B slice");
    for (unsigned type = 0; type < 10; ++type) {
        // first_mb_in_slice=0, followed by ue(slice_type), then rbsp_stop_one_bit.
        unsigned bits = 0;
        for (unsigned value = type + 1; value > 1; value >>= 1) ++bits;
        const unsigned length = 1 + bits * 2 + 1 + 1;
        const unsigned packed = (1u << (bits * 2 + 2)) | ((type + 1) << 1) | 1u;
        std::vector<uint8_t> slice{0,0,0,1,0x41};
        for (unsigned offset = 0; offset < length; offset += 8) {
            const unsigned remaining = length - offset;
            slice.push_back(static_cast<uint8_t>(remaining >= 8 ? packed >> (remaining - 8) : packed << (8 - remaining)));
        }
        std::array<uint64_t, 10> counts{};
        Require(CountH264Slices(slice, counts) && counts[type] == 1, "raw slice_type distribution 0..9");
        Require(HasH264BSlice(slice) == (type % 5 == 1), "both B slice_type values detected");
    }
    const std::vector<uint8_t> invalid_then_b{0,0,0,1,0x41,0,0,0,1,0x41,0xA8};
    std::array<uint64_t, 10> invalid_counts{};
    Require(!CountH264Slices(invalid_then_b, invalid_counts) && HasH264BSlice(invalid_then_b),
        "malformed earlier slice cannot hide a later B slice");
    std::vector<uint8_t> ambiguous_avcc(361, 0x80);
    ambiguous_avcc[0] = 0; ambiguous_avcc[1] = 0; ambiguous_avcc[2] = 1; ambiguous_avcc[3] = 0x65;
    ambiguous_avcc[4] = 0x41;
    Require(NormalizeH264(ambiguous_avcc, 4, output) && HasH264Nal(output, 1) && !HasH264Nal(output, 5),
        "AVCC length resembling a three-byte Annex-B start code");
    LOG_INFO("PASS: Annex-B/AVCC, SPS/PPS on every keyframe, invalid/truncated samples");
}
ComPtr<ID3D11Texture2D> ColorTexture(ID3D11Device* device, uint32_t width = 128, uint32_t height = 64) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel) {
        pixels[pixel * 4] = pixel / width < height / 2 ? 0 : 255;
        pixels[pixel * 4 + 2] = pixel / width < height / 2 ? 255 : 0;
        pixels[pixel * 4 + 3] = 255;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    Require(LOG_HR(device->CreateTexture2D(&desc, &initial, texture.GetAddressOf()), "test BGRA texture"), "color texture");
    return texture;
}
void Nv12Test(ID3D11Device* device) {
    GpuNv12Converter converter;
    Require(converter.Initialize(device, 128, 64), "GPU NV12 shader initialize");
    const auto source = ColorTexture(device);
    ComPtr<ID3D11Texture2D> converted, staging;
    Require(converter.Convert(source.Get(), converted), "GPU NV12 convert");
    D3D11_TEXTURE2D_DESC desc{};
    converted->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Require(LOG_HR(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "test staging"), "staging texture");
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.GetAddressOf());
    GpuContextLock lock(context.Get());
    context->CopyResource(staging.Get(), converted.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Require(LOG_HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "test NV12 Map"), "Map NV12");
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    const int top = bytes[mapped.RowPitch * 8 + 8];
    const int bottom = bytes[mapped.RowPitch * 48 + 8];
    const int u = bytes[mapped.RowPitch * 64 + mapped.RowPitch * 4 + 8];
    const int v = bytes[mapped.RowPitch * 64 + mapped.RowPitch * 4 + 9];
    context->Unmap(staging.Get(), 0);
    LOG_INFO("NV12 color probes: redY=", top, " blueY=", bottom, " redU=", u, " redV=", v);
    Require(abs(top - 63) <= 2 && abs(bottom - 32) <= 2 && abs(u - 102) <= 2 && abs(v - 240) <= 2,
        "BT709 limited-range GPU conversion colors and orientation");
    LOG_INFO("PASS: GPU NV12 shader color/plane/orientation readback");
}
void H264WireTest(const EncodedFrame& frame) {
    TcpServer server;
    Require(server.Listen(0), "H264 wire listen");
    sockaddr_in address{};
    int address_size = sizeof(address);
    Require(getsockname(server.Socket(), reinterpret_cast<sockaddr*>(&address), &address_size) == 0, "H264 wire address");
    UniqueSocket client(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Require(client.Get() != INVALID_SOCKET && connect(client.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "H264 wire connect");
    std::string peer;
    UniqueSocket accepted(server.Accept(peer));
    Require(accepted.Get() != INVALID_SOCKET, "H264 wire accept");
    bool sent = false;
    std::jthread sender([&] { sent = SendFrame(accepted.Get(), frame, 1, Codec::H264); });
    MessageHeader header{};
    FrameInfo info{};
    Require(TcpServer::RecvExact(client.Get(), &header, sizeof(header)) && ValidHeader(header), "H264 wire header");
    Require(TcpServer::RecvExact(client.Get(), &info, sizeof(info)) && ValidFrame(info, header.length), "H264 wire frame info");
    std::vector<uint8_t> received(info.payload_size);
    Require(TcpServer::RecvExact(client.Get(), received.data(), received.size()), "H264 wire payload");
    sender.join();
    Require(sent && info.codec == 2 && header.flags == kFlagKeyframe && received == frame.data &&
        info.width == frame.width && info.height == frame.height && info.capture_ts_us == frame.capture_ts_us,
        "H264 codec and frozen frame layout across TCP");
    LOG_INFO("PASS: real H264 keyframe transported with codec=2 and unchanged 12/28-byte wire structures");
}
void HardwareTest(ID3D11Device* device) {
    auto encoder = CreateMfH264Encoder();
    ComPtr<ID3D11Device> selected = device;
    bool ready = encoder->Initialize(selected.Get(), 1280, 800, 60, 10000);
    for (unsigned index = 1; index < 4 && !ready; ++index) {
        if (CreateCaptureDeviceForAdapter(index, selected)) {
            encoder = CreateMfH264Encoder();
            ready = encoder->Initialize(selected.Get(), 1280, 800, 60, 10000);
        }
    }
    Require(ready, "MF H264 hardware initialization");
    const auto source = ColorTexture(selected.Get());
    uint32_t encoded_count = 0, keys = 0;
    bool forced_first = false, forced_second = false;
    test::MfDecoder decoder;
    std::vector<uint8_t> decoded;
    std::vector<uint64_t> encode_times;
    std::array<uint64_t, 10> slice_counts{};
    const auto start = NowUs();
    auto consume = [&](const EncodedFrame& frame, uint32_t submitted_index) {
            ++encoded_count;
            encode_times.push_back(static_cast<MfH264Encoder*>(encoder.get())->LastEncodeTimeUs());
            if (encoded_count == 1) H264WireTest(frame);
            Require(frame.width == 1280 && frame.height == 800 && frame.capture_ts_us >= start &&
                frame.capture_ts_us <= start + static_cast<uint64_t>(submitted_index) * 16667, "output dimensions and 100ns PTS roundtrip");
            if (frame.keyframe) {
                ++keys;
                const auto input_index = (frame.capture_ts_us - start) / 16667;
                forced_first |= input_index >= 45 && input_index <= 50;
                forced_second |= input_index >= 105 && input_index <= 110;
                Require(frame.data.size() >= 4 && frame.data[0] == 0 && frame.data[1] == 0 && frame.data[2] == 0 && frame.data[3] == 1 &&
                    HasH264Nal(frame.data, 7) && HasH264Nal(frame.data, 8) && HasH264Nal(frame.data, 5), "hardware keyframe SPS PPS IDR");
            } else Require(!HasH264Nal(frame.data, 5), "flag agrees with NAL contents");
            Require(!HasH264BSlice(frame.data), "bitstream has no B slices");
            Require(CountH264Slices(frame.data, slice_counts), "hardware slice headers parse");
            Require(decoder.Feed(frame, &decoded), "independent MF H264 decode");
    };
    for (uint32_t i = 0; i < 180; ++i) {
        if (i == 45 || i == 105) Require(encoder->ForceKeyframe(), "ForceKeyframe");
        EncodedFrame frame;
        if (encoder->Encode(source.Get(), start + static_cast<uint64_t>(i) * 16667, frame)) consume(frame, i);
        Require(static_cast<MfH264Encoder*>(encoder.get())->Healthy(), "encoder remains healthy");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Hardware completion is asynchronous: collect the final submitted frame too.
    const auto drain_deadline = NowUs() + 2000000;
    while (encoded_count < 180 && NowUs() < drain_deadline) {
        EncodedFrame frame;
        if (static_cast<MfH264Encoder*>(encoder.get())->Poll(frame)) consume(frame, 179);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(static_cast<MfH264Encoder*>(encoder.get())->Healthy(), "encoder healthy during final output polling");
    }
    LOG_INFO("HARDWARE output frames=", encoded_count, " keys=", keys, " forced45=", forced_first,
        " forced105=", forced_second, " P=", slice_counts[0] + slice_counts[5],
        " B=", slice_counts[1] + slice_counts[6], " I=", slice_counts[2] + slice_counts[7]);
    Require(encoded_count == 180 && keys >= 3 && forced_first && forced_second, "all 180 inputs output and forced IDRs away from natural GOP boundaries");
    Require(decoder.Finish(&decoded), "independent decoder final drain");
    Require(decoder.Frames() == encoded_count && decoded.size() >= static_cast<size_t>(decoder.Stride()) * 800 * 3 / 2,
        "hardware bitstream decodes continuously to NV12");
    const auto pitch = decoder.Stride();
    const int red_y = decoded[static_cast<size_t>(pitch) * 160 + 128];
    const int blue_y = decoded[static_cast<size_t>(pitch) * 640 + 128];
    const int black_y = decoded[static_cast<size_t>(pitch) * 16 + 128];
    const auto chroma_base = static_cast<size_t>(pitch) * 800;
    const int red_u = decoded[chroma_base + static_cast<size_t>(pitch) * 80 + 128];
    const int red_v = decoded[chroma_base + static_cast<size_t>(pitch) * 80 + 129];
    const int blue_u = decoded[chroma_base + static_cast<size_t>(pitch) * 320 + 128];
    const int blue_v = decoded[chroma_base + static_cast<size_t>(pitch) * 320 + 129];
    Require(abs(red_y - 63) < 12 && abs(blue_y - 32) < 12 && abs(black_y - 16) < 12,
        "decoded H264 colors, letterboxing and top-down orientation");
    Require(abs(red_u - 102) < 12 && abs(red_v - 240) < 12 && abs(blue_u - 240) < 12 && abs(blue_v - 118) < 12,
        "decoded H264 chroma channels and limited range");
    LOG_INFO("PASS: independent MF decode frames=", decoder.Frames(), " Y probes=", red_y, '/', blue_y, '/', black_y);
    LOG_INFO("Decoded color UV probes: red=", red_u, '/', red_v, " blue=", blue_u, '/', blue_v);
    uint64_t encode_total = 0;
    for (const auto value : encode_times) encode_total += value;
    std::sort(encode_times.begin(), encode_times.end());
    LOG_INFO("HARDWARE BENCH encode_avg_ms=", static_cast<double>(encode_total) / static_cast<double>(encode_times.size()) / 1000.0,
        " encode_p95_ms=", static_cast<double>(encode_times[(encode_times.size() - 1) * 95 / 100]) / 1000.0,
        " encode_max_ms=", static_cast<double>(encode_times.back()) / 1000.0,
        " (GPU shader + MFT completion observed by event polling; synthetic color pattern)");
    LOG_INFO("PASS: real MF hardware H264 frames=", encoded_count, " keyframes=", keys, " name=", encoder->Name());
    LOG_INFO("HARDWARE slices P=", slice_counts[0] + slice_counts[5], " B=", slice_counts[1] + slice_counts[6],
        " I=", slice_counts[2] + slice_counts[7]);
}
void WgcTest(ID3D11Device* device) {
    auto capture = CreateWgcCapture();
    Require(capture->Initialize(device, 1280, 800), "WGC initialize primary monitor");
    GpuFrame first, second;
    for (int i = 0; i < 300 && !first.texture; ++i) {
        capture->Capture(first); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < 300 && !second.texture; ++i) {
        capture->Capture(second); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Require(first.texture && second.texture && first.texture.Get() != second.texture.Get() &&
        first.capture_ts_us > 0 && second.capture_ts_us >= first.capture_ts_us, "independent copied WGC textures");
    LOG_INFO("PASS: WGC owns distinct frame textures, native ", first.width, 'x', first.height);
}
}
}
int main() {
    const auto initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (!LOG_HR(initialized, "test RoInitialize")) return 1;
    int result = 0;
    try {
        pds::WinsockRuntime winsock;
        pds::Require(winsock.Ready(), "stage2 Winsock");
        pds::AnnexBTest();
        pds::ComPtr<ID3D11Device> device;
        pds::Require(pds::CreateCaptureDevice(device), "hardware D3D11");
        pds::Nv12Test(device.Get());
        // Run WGC even if an encoder assertion fails; neither failure may be hidden.
        try { pds::HardwareTest(device.Get()); }
        catch (const std::exception& error) { LOG_ERROR("FAIL stage 2: ", error.what()); result = 1; }
        pds::WgcTest(device.Get());
    } catch (const std::exception& error) { LOG_ERROR("FAIL stage 2: ", error.what()); result = 1; }
    RoUninitialize();
    return result;
}
