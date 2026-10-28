#include "net/frame_sender.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>
#include <wincodec.h>
#include <psapi.h>
#include "capture/capture.h"
#include "util/bounded_queue.h"
#include "util/com_ptr.h"

namespace pds {
namespace {
void Require(bool ok, const char* description) { if (!ok) throw std::runtime_error(description); }
constexpr std::array<uint8_t, 44> kHelloVector{
    0x50,0x44,0x53,0x31,0x01,0x00,0x00,0x00,0x20,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x05,0x00,0x00,0x20,0x03,0x00,0x00,
    0x3C,0x00,0x00,0x00,0x40,0x01,0x00,0x00,0x00,0x05,0x00,0x00,
    0x20,0x03,0x00,0x00,0x01,0x00,0x00,0x00
};
void ProtocolTest() {
    MessageHeader header{};
    Hello hello{};
    Require(Deserialize(std::span(kHelloVector).first<12>(), header) && ValidHeader(header), "HELLO header vector");
    Require(Deserialize(std::span(kHelloVector).subspan<12>(), hello), "HELLO payload vector");
    Require(hello.preferred_width == 1280 && hello.preferred_height == 800 && hello.preferred_fps == 60 &&
        hello.screen_density_dpi == 320 && hello.capabilities == 1 && hello.protocol_version == 1, "HELLO decoded values");
    const auto serialized = Serialize(hello);
    Require(std::equal(serialized.begin(), serialized.end(), kHelloVector.begin() + 12), "HELLO serialization vector");
    Require(!ValidHeader({0, 1, 0, 32}) && !ValidHeader({kMagic, 1, 0, kMaxPayload + 1}), "header validation");
    Require(ValidHeader({kMagic, 0xABCD, 0, 0}), "unknown/empty message accepted");
    const FrameInfo frame{1, 1, 0, 0, 1280, 800, 1234567890123ULL, 4};
    Require(ValidFrame(frame, 32) && !ValidFrame(frame, 31) && !ValidFrame(frame, 27), "FrameInfo length validation");
    LOG_INFO("PASS: protocol vector, packed sizes, invalid headers/frame lengths");
}
void QueueTest() {
    BoundedQueue<int> queue(2);
    Require(queue.Push(1) == 0 && queue.Push(2) == 0 && queue.Push(3) == 1, "queue drops oldest");
    std::stop_source stop;
    int out = 0;
    Require(queue.Pop(out, stop.get_token()) && out == 2, "queue retains second item");
    Require(queue.Pop(out, stop.get_token()) && out == 3, "queue retains newest item");
    std::jthread waiter([&](std::stop_token token) { Require(!queue.Pop(out, token), "stop token wakes Pop"); });
    waiter.request_stop(); waiter.join();
    queue.Stop();
    Require(!queue.Pop(out, stop.get_token()), "Stop wakes queue");
    LOG_INFO("PASS: queue capacity, drop-oldest, stop-token cancellation");
}
uint16_t PortOf(const TcpServer& server) = delete;
uint16_t PortOf(TcpServer& server) {
    sockaddr_in address{};
    int length = sizeof(address);
    Require(getsockname(server.Socket(), reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname");
    return ntohs(address.sin_port);
}
SOCKET Connect(uint16_t port) {
    UniqueSocket socket_handle(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Require(socket_handle.Get() != INVALID_SOCKET, "client socket");
    const DWORD timeout = 5000;
    Require(setsockopt(socket_handle.Get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "recv timeout");
    const BOOL no_delay = TRUE;
    Require(setsockopt(socket_handle.Get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)) == 0, "no delay");
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    if (connect(socket_handle.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return INVALID_SOCKET;
    return socket_handle.Release();
}
void NetworkTest() {
    TcpServer server;
    Require(server.Listen(0), "listen on ephemeral loopback port");
    UniqueSocket client(Connect(PortOf(server)));
    Require(client.Get() != INVALID_SOCKET, "connect");
    std::string peer;
    UniqueSocket accepted(server.Accept(peer));
    Require(accepted.Get() != INVALID_SOCKET, "accept");
    BOOL no_delay = FALSE;
    int opt_size = sizeof(no_delay);
    Require(getsockopt(accepted.Get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&no_delay), &opt_size) == 0 && no_delay, "TCP_NODELAY");
    std::jthread fragmented([&] {
        for (const auto byte : kHelloVector) {
            Require(TcpServer::SendAll(client.Get(), &byte, 1), "fragmented send");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::array<uint8_t, 44> received{};
    Require(TcpServer::RecvExact(accepted.Get(), received.data(), received.size()), "RecvExact fragmented message");
    fragmented.join();
    Require(received == kHelloVector, "fragmented message content");
    EncodedFrame frame{1280, 800, 987654321ULL, true, std::vector<uint8_t>(1024u * 1024u, 0x5A)};
    std::jthread sender([&] { Require(SendFrame(accepted.Get(), frame, 7), "scatter/gather frame send"); });
    MessageHeader header{};
    FrameInfo info{};
    Require(TcpServer::RecvExact(client.Get(), &header, sizeof(header)), "frame header");
    Require(TcpServer::RecvExact(client.Get(), &info, sizeof(info)), "frame metadata");
    std::vector<uint8_t> body(info.payload_size);
    Require(TcpServer::RecvExact(client.Get(), body.data(), body.size()), "large frame body");
    sender.join();
    Require(ValidFrame(info, header.length) && info.frame_id == 7 && body == frame.data &&
        header.flags == kFlagKeyframe && info.capture_ts_us == frame.capture_ts_us, "frame wire consistency");
    client.Reset();
    uint8_t byte = 0;
    Require(!TcpServer::RecvExact(accepted.Get(), &byte, 1), "recv EOF");
    LOG_INFO("PASS: loopback options, fragmented RecvExact, 1 MiB scatter/gather send, EOF");
}
void JpegTest() {
    ComApartment apartment;
    Require(apartment.Ready(), "COM apartment");
    auto encoder = CreateJpegEncoder(95);
    Require(encoder->Initialize(64, 32, 15), "JPEG Initialize");
    CapturedFrame input{64, 32, 12345, std::vector<uint8_t>(64 * 32 * 4)};
    for (size_t pixel = 0; pixel < 64 * 32; ++pixel) {
        input.bgra[pixel * 4] = pixel / 64 < 16 ? 0 : 255;
        input.bgra[pixel * 4 + 1] = 0;
        input.bgra[pixel * 4 + 2] = pixel / 64 < 16 ? 255 : 0;
    }
    EncodedFrame encoded;
    Require(encoder->Encode(input, encoded) && encoded.keyframe && encoded.capture_ts_us == 12345, "WIC encode JPEG");
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Require(LOG_HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())), "test WIC factory"), "factory");
    Require(LOG_HR(factory->CreateStream(stream.GetAddressOf()), "test stream"), "stream");
    Require(LOG_HR(stream->InitializeFromMemory(encoded.data.data(), static_cast<DWORD>(encoded.data.size())), "test stream memory"), "stream memory");
    Require(LOG_HR(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()), "test JPEG decoder"), "decoder");
    Require(LOG_HR(decoder->GetFrame(0, frame.GetAddressOf()), "test JPEG frame"), "frame");
    Require(LOG_HR(factory->CreateFormatConverter(converter.GetAddressOf()), "test converter"), "converter");
    Require(LOG_HR(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
        WICBitmapPaletteTypeCustom), "test decode BGR"), "decode convert");
    std::vector<uint8_t> pixels(input.bgra.size());
    Require(LOG_HR(converter->CopyPixels(nullptr, 64 * 4, static_cast<UINT>(pixels.size()), pixels.data()), "test pixels"), "pixels");
    Require(pixels[(4 * 64 + 4) * 4 + 2] > 230 && pixels[(4 * 64 + 4) * 4] < 20 &&
        pixels[(24 * 64 + 4) * 4] > 230 && pixels[(24 * 64 + 4) * 4 + 2] < 20, "JPEG colors and top-down orientation");
    LOG_INFO("PASS: WIC JPEG decode roundtrip, BGRA colors, top-down orientation, metadata");
}
void CaptureTest() {
    Require(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE, "DPI awareness");
    auto capture = CreateGdiCapture();
    Require(capture->Initialize(1280, 800), "GDI initialize");
    CapturedFrame frame;
    Require(capture->Capture(frame), "GDI capture current primary display");
    Require(frame.bgra.size() == static_cast<size_t>(frame.width) * frame.height * 4 && frame.capture_ts_us > 0 &&
        frame.width % 2 == 0 && frame.height % 2 == 0, "GDI packed even frame");
    LOG_INFO("PASS: live GDI capture ", frame.width, 'x', frame.height);
}
class ChildHost {
public:
    ChildHost(const std::filesystem::path& executable, uint16_t port) {
        std::wstring command = L"\"" + executable.wstring() + L"\" --port " + std::to_wstring(port) + L" --fps 15";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        Require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            nullptr, &startup, &process_) != FALSE, "start isolated test host");
    }
    ~ChildHost() {
        if (process_.hProcess) {
            if (!TerminateProcess(process_.hProcess, 0)) LOG_HR(LastErrorHr(), "stop test host");
            WaitForSingleObject(process_.hProcess, 5000);
            CloseHandle(process_.hProcess); CloseHandle(process_.hThread);
        }
    }
    HANDLE Handle() const { return process_.hProcess; }
private:
    PROCESS_INFORMATION process_{};
};
bool ReadWire(SOCKET socket, MessageHeader& header, std::vector<uint8_t>& body) {
    return TcpServer::RecvExact(socket, &header, sizeof(header)) && ValidHeader(header) &&
        TcpServer::RecvExact(socket, body.data(), header.length);
}
HelloAck Handshake(SOCKET socket, uint16_t expected = 0, uint16_t version = 1, uint32_t caps = 1) {
    auto bytes = kHelloVector;
    Hello hello{};
    Require(Deserialize(std::span(bytes).subspan<12>(), hello), "test hello parse");
    hello.protocol_version = version; hello.capabilities = caps;
    const auto payload = Serialize(hello);
    std::copy(payload.begin(), payload.end(), bytes.begin() + 12);
    Require(TcpServer::SendAll(socket, bytes.data(), bytes.size()), "HELLO send to real host");
    std::vector<uint8_t> body(kMaxPayload);
    MessageHeader header{};
    Require(ReadWire(socket, header, body) && header.type == static_cast<uint16_t>(MessageType::HelloAck), "real host HELLO_ACK");
    HelloAck ack{};
    Require(Deserialize(std::span(body.data(), header.length), ack) && ack.result == expected, "real ACK result");
    if (expected == 0) Require(ack.width > 0 && ack.height > 0 && ack.codec == 1 && ack.fps == 15, "real ACK fields");
    return ack;
}
void IntegrationTest(const std::filesystem::path& host) {
    uint16_t port = 0;
    { TcpServer temporary; Require(temporary.Listen(0), "find test port"); port = PortOf(temporary); }
    ChildHost process(host, port);
    UniqueSocket client;
    for (int retry = 0; retry < 50; ++retry) {
        client.Reset(Connect(port));
        if (client.Get() != INVALID_SOCKET) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Require(client.Get() != INVALID_SOCKET, "host startup");
    const auto unknown = Serialize(MessageHeader{kMagic, 0x4444, 0, 3});
    const std::array<uint8_t, 3> junk{1,2,3};
    Require(TcpServer::SendAll(client.Get(), unknown.data(), unknown.size()) &&
        TcpServer::SendAll(client.Get(), junk.data(), junk.size()), "unknown before HELLO");
    const auto ack = Handshake(client.Get());
    Require(TcpServer::SendAll(client.Get(), unknown.data(), unknown.size()) &&
        TcpServer::SendAll(client.Get(), junk.data(), junk.size()), "unknown during stream");
    std::vector<uint8_t> body(kMaxPayload);
    MessageHeader header{};
    for (uint32_t id = 1; id <= 3; ++id) {
        Require(ReadWire(client.Get(), header, body), "stream frame");
        FrameInfo info{};
        Require(Deserialize(std::span(body.data(), sizeof(info)), info) && ValidFrame(info, header.length) &&
            info.frame_id == id && info.width == ack.width && info.height == ack.height &&
            body[28] == 0xFF && body[29] == 0xD8 && body[header.length - 2] == 0xFF && body[header.length - 1] == 0xD9,
            "real GDI/JPEG frame wire fields");
    }
    UniqueSocket replacement(Connect(port));
    Require(replacement.Get() != INVALID_SOCKET, "new client during streaming");
    Handshake(replacement.Get());
    bool old_closed = false;
    for (int i = 0; i < 100 && !old_closed; ++i) old_closed = !ReadWire(client.Get(), header, body);
    Require(old_closed, "new client replaces old");
    replacement.Reset();
    for (const auto invalid : {MessageHeader{0, 1, 0, 32}, MessageHeader{kMagic, 1, 0, kMaxPayload + 1}}) {
        UniqueSocket bad(Connect(port));
        Require(bad.Get() != INVALID_SOCKET, "malformed client connect");
        Require(TcpServer::SendAll(bad.Get(), &invalid, sizeof(invalid)), "malformed header send");
        Require(!ReadWire(bad.Get(), header, body), "host rejects malformed header");
    }
    { UniqueSocket rejected(Connect(port)); Handshake(rejected.Get(), 1, 2); }
    { UniqueSocket rejected(Connect(port)); Handshake(rejected.Get(), 2, 1, 0); }
    { UniqueSocket reconnected(Connect(port)); Handshake(reconnected.Get()); Require(ReadWire(reconnected.Get(), header, body), "reconnect after errors"); }
    LOG_INFO("PASS: live host HELLO/JPEG frames, unknown types, replacement, bad magic/length, version/codec rejection, reconnect");
}
void SoakTest(const std::filesystem::path& host, int seconds) {
    uint16_t port = 0;
    { TcpServer temporary; Require(temporary.Listen(0), "soak port"); port = PortOf(temporary); }
    ChildHost process(host, port);
    UniqueSocket client;
    for (int retry = 0; retry < 50; ++retry) {
        client.Reset(Connect(port));
        if (client.Get() != INVALID_SOCKET) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Require(client.Get() != INVALID_SOCKET, "soak connect");
    Handshake(client.Get());
    PROCESS_MEMORY_COUNTERS_EX initial{}, final{};
    initial.cb = sizeof(initial); final.cb = sizeof(final);
    Require(GetProcessMemoryInfo(process.Handle(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&initial), sizeof(initial)) != FALSE, "soak initial memory");
    const auto started = std::chrono::steady_clock::now();
    auto next_log = started + std::chrono::seconds(60);
    std::vector<uint8_t> body(kMaxPayload);
    uint64_t frames = 0, bytes = 0;
    MessageHeader header{};
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(seconds)) {
        Require(ReadWire(client.Get(), header, body), "soak uninterrupted frame stream");
        FrameInfo info{};
        Require(header.type == static_cast<uint16_t>(MessageType::VideoFrame) &&
            Deserialize(std::span(body.data(), sizeof(info)), info) && ValidFrame(info, header.length), "soak frame validation");
        ++frames; bytes += header.length + sizeof(MessageHeader);
        if (std::chrono::steady_clock::now() >= next_log) {
            LOG_INFO("Soak progress: frames=", frames);
            next_log += std::chrono::seconds(60);
        }
    }
    Require(GetProcessMemoryInfo(process.Handle(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&final), sizeof(final)) != FALSE, "soak final memory");
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto growth = static_cast<int64_t>(final.PrivateUsage) - static_cast<int64_t>(initial.PrivateUsage);
    LOG_INFO("SOAK seconds=", elapsed, " frames=", frames, " fps=", static_cast<double>(frames) / elapsed,
        " Mbps=", static_cast<double>(bytes) * 8 / elapsed / 1000000.0,
        " private_start=", initial.PrivateUsage, " private_end=", final.PrivateUsage,
        " private_growth_bytes=", growth);
    Require(growth < 30LL * 1024 * 1024, "soak private memory growth < 30 MiB");
}
}
}
int main(int argc, char** argv) {
    try {
        pds::WinsockRuntime winsock;
        pds::Require(winsock.Ready(), "Winsock runtime");
        if (argc == 3 && std::string_view(argv[1]) == "--soak-seconds") {
            pds::SoakTest(std::filesystem::absolute(argv[0]).parent_path() / "padscreen-host.exe", std::stoi(argv[2]));
            return 0;
        }
        pds::ProtocolTest(); pds::QueueTest(); pds::NetworkTest(); pds::JpegTest(); pds::CaptureTest();
        if (argc > 0) pds::IntegrationTest(std::filesystem::absolute(argv[0]).parent_path() / "padscreen-host.exe");
        LOG_INFO("All stage-1 host tests passed");
        return 0;
    } catch (const std::exception& error) { LOG_ERROR("FAIL: ", error.what()); return 1; }
}
