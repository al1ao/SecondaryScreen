#include "net/pipeline_session.h"
#include "capture/wgc_capture.h"
#include "util/timer.h"
#include "util/log.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace pds {
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::atomic<unsigned> preflight_calls{0};
HRESULT Denied(WgcCapture&, HMONITOR) { ++preflight_calls; return E_ACCESSDENIED; }
HRESULT Unsupported(WgcCapture&, HMONITOR) { ++preflight_calls; return E_NOTIMPL; }
HRESULT SlowPreflight(WgcCapture&, HMONITOR) {
    ++preflight_calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(4100));
    return S_OK; // Budget must be checked before creating any GPU resources.
}

void HandshakeCase(const char* name, WgcPreflight preflight, uint32_t capabilities, bool expect_jpeg, bool slow = false) {
    TcpServer server;
    Check(server.Listen(0), "handshake test loopback listener");
    sockaddr_in address{};
    int size = sizeof(address);
    Check(getsockname(server.Socket(), reinterpret_cast<sockaddr*>(&address), &size) == 0, "ephemeral address");
    UniqueSocket client(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    Check(client.Get() != INVALID_SOCKET, "handshake test client");
    const DWORD timeout = 5000;
    Check(setsockopt(client.Get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "bounded ACK receive");
    Check(connect(client.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "handshake test connect");
    std::string peer;
    UniqueSocket accepted(server.Accept(peer));
    Check(accepted.Get() != INVALID_SOCKET, "handshake test accept");
    SessionOptions options;
    options.capture_primary = true;
    options.width = 320; options.height = 200;
    preflight_calls.store(0);
    PipelineSession session(std::move(accepted), options, preflight);
    session.Run();
    const auto header = Serialize(MakeHeader(MessageType::Hello, sizeof(Hello)));
    const auto hello = Serialize(Hello{1, 0, 320, 200, 60, 320, 1920, 1200, capabilities});
    const auto began = NowUs();
    Check(TcpServer::SendAll(client.Get(), header.data(), header.size()) &&
        TcpServer::SendAll(client.Get(), hello.data(), hello.size()), "send caps HELLO");
    MessageHeader received{};
    HelloAck ack{};
    Check(TcpServer::RecvExact(client.Get(), &received, sizeof(received)) && ValidHeader(received) &&
        received.type == static_cast<uint16_t>(MessageType::HelloAck) && received.length == sizeof(HelloAck), "received HELLO_ACK");
    Check(TcpServer::RecvExact(client.Get(), &ack, sizeof(ack)), "ACK payload");
    const auto elapsed = NowUs() - began;
    Check(elapsed < 5000000, "failure path ACK must be below 5 seconds");
    Check(preflight_calls.load() == 1, "session/capture-item failures must not retry other adapters");
    if (slow) Check(elapsed >= 4000000, "delayed preflight exercised the retry budget");
    if (expect_jpeg) {
        Check(ack.result == 0 && ack.codec == 1 && ack.width == 320 && ack.height == 200,
            "caps=3 receives real JPEG fallback ACK");
        FrameInfo info{};
        Check(TcpServer::RecvExact(client.Get(), &received, sizeof(received)) && ValidHeader(received) &&
            received.type == static_cast<uint16_t>(MessageType::VideoFrame), "JPEG fallback first frame header");
        Check(TcpServer::RecvExact(client.Get(), &info, sizeof(info)) && ValidFrame(info, received.length) && info.codec == 1,
            "JPEG fallback first frame info");
        std::vector<uint8_t> bytes(info.payload_size);
        Check(TcpServer::RecvExact(client.Get(), bytes.data(), bytes.size()) && bytes.size() > 4 &&
            bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[bytes.size() - 2] == 0xFF && bytes.back() == 0xD9,
            "fallback emits a real GDI/WIC JPEG");
    } else {
        Check(ack.result == static_cast<uint16_t>(HelloResult::Codec), "H264-only client gets explicit codec rejection");
        uint8_t byte = 0;
        Check(!TcpServer::RecvExact(client.Get(), &byte, 1), "rejected H264-only session closes");
    }
    client.Reset();
    LOG_INFO("PASS stage 4 handshake: ", name, " caps=", capabilities, " result=", ack.result,
        " codec=", static_cast<unsigned>(ack.codec), " elapsed_ms=", static_cast<double>(elapsed) / 1000.0,
        " preflight_calls=", preflight_calls.load(), " (injected preflight outcome; real TCP and JPEG fallback)");
}
}
}
int main() {
    try {
        pds::WinsockRuntime winsock;
        pds::Check(winsock.Ready(), "handshake Winsock");
        pds::HandshakeCase("E_ACCESSDENIED JPEG fallback", pds::Denied, 3, true);
        pds::HandshakeCase("E_ACCESSDENIED H264-only rejection", pds::Denied, 2, false);
        pds::HandshakeCase("WGC unavailable JPEG fallback", pds::Unsupported, 3, true);
        pds::HandshakeCase("4s preflight exhausts retry budget", pds::SlowPreflight, 3, true, true);
        return 0;
    } catch (const std::exception& error) { LOG_ERROR("FAIL stage 4 handshake: ", error.what()); return 1; }
}
