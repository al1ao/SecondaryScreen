#include "net/frame_sender.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>
#include "capture/capture.h"
#include "util/bounded_queue.h"
#include "util/com_ptr.h"
#include "util/timer.h"

namespace pds {
namespace {
std::atomic<bool> exit_requested{false};
BOOL WINAPI ConsoleControl(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT) return FALSE;
    exit_requested.store(true);
    return TRUE;
}
struct Options {
    uint16_t port = kDefaultPort;
    uint32_t width = 1280, height = 800, fps = 15;
    int quality = 70;
    bool show_stats = false;
};
bool ParseOptions(int argc, char** argv, Options& options) {
    // TODO(design): reject invalid CLI values with usage text; cap stage-1 dimensions
    // at 16384 per axis and fps at 60 rather than trying to allocate unbounded images.
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--verbose") { g_log_level.store(LogLevel::Debug); continue; }
        // TODO(design): checklist mentions --show-stats but stage 1 defines no STATS
        // wire message. This only logs local frame counts; Android displays local FPS.
        if (key == "--show-stats") { options.show_stats = true; continue; }
        if (i + 1 >= argc) return false;
        const std::string_view text(argv[++i]);
        uint32_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) return false;
        if (key == "--port" && value > 0 && value <= 65535) options.port = static_cast<uint16_t>(value);
        else if (key == "--width" && value >= 2 && value <= 16384) options.width = value & ~1u;
        else if (key == "--height" && value >= 2 && value <= 16384) options.height = value & ~1u;
        else if (key == "--fps" && value >= 1 && value <= 60) options.fps = value;
        else if (key == "--quality" && value >= 1 && value <= 100) options.quality = static_cast<int>(value);
        else return false;
    }
    return true;
}
bool ReadMessage(SOCKET socket, MessageHeader& header, std::vector<uint8_t>& payload) {
    std::array<uint8_t, sizeof(MessageHeader)> bytes{};
    if (!TcpServer::RecvExact(socket, bytes.data(), bytes.size())) return false;
    if (!Deserialize(bytes, header) || !ValidHeader(header)) {
        LOG_ERROR("Protocol error: invalid magic/length"); return false;
    }
    return TcpServer::RecvExact(socket, payload.data(), header.length);
}
class Session {
public:
    Session(UniqueSocket socket, Options options) : socket_(std::move(socket)), options_(options) {}
    ~Session() {
        Stop();
        if (receiver_.joinable()) receiver_.join();
        if (producer_.joinable()) producer_.join();
        if (sender_.joinable()) sender_.join();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool Running() const { return running_.load(); }
    void Stop() {
        if (!running_.exchange(false)) return;
        source_.request_stop();
        queue_.Stop();
        wait_cv_.notify_all();
        // Shutdown unblocks recv/send; close only after workers have joined to avoid socket reuse races.
        if (shutdown(socket_.Get(), SD_BOTH) == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error != WSAENOTCONN) LOG_HR(HRESULT_FROM_WIN32(error), "shutdown session");
        }
    }
    void Run() {
        running_.store(true);
        receiver_ = std::jthread([this] {
            try { Receive(); }
            catch (const std::exception& error) { LOG_ERROR("Session: ", error.what()); Stop(); }
        });
    }
private:
    void Receive() {
        std::vector<uint8_t> payload(kMaxPayload); // One reusable receive buffer per session.
        MessageHeader header{};
        Hello hello{};
        for (;;) {
            if (!Running() || !ReadMessage(socket_.Get(), header, payload)) { Stop(); return; }
            if (header.type == static_cast<uint16_t>(MessageType::Hello)) break;
            LOG_WARN("Ignoring unknown/unexpected type before HELLO: ", header.type);
        }
        if (!Deserialize(std::span(payload.data(), header.length), hello)) {
            LOG_ERROR("Protocol error: HELLO must be 32 bytes"); Stop(); return;
        }
        LOG_INFO("HELLO received: preferred=", hello.preferred_width, 'x', hello.preferred_height,
            " fps=", hello.preferred_fps, " caps=", hello.capabilities, " dpi=", hello.screen_density_dpi);
        HelloAck ack{};
        ack.protocol_version = kProtocolVersion;
        ack.codec = static_cast<uint8_t>(Codec::JPEG);
        ack.fps = static_cast<uint16_t>(options_.fps);
        ack.frame_interval_us = 1000000u / options_.fps;
        if (hello.protocol_version != kProtocolVersion) ack.result = static_cast<uint16_t>(HelloResult::Version);
        else if ((hello.capabilities & kCapCodecJpeg) == 0) ack.result = static_cast<uint16_t>(HelloResult::Codec);
        if (ack.result == 0) {
            std::promise<std::pair<uint32_t, uint32_t>> ready;
            auto result = ready.get_future();
            producer_ = std::jthread([this, signal = std::move(ready)]() mutable { Produce(std::move(signal)); });
            const auto [width, height] = result.get();
            ack.width = width; ack.height = height;
            if (width == 0 || height == 0) ack.result = static_cast<uint16_t>(HelloResult::Resolution);
        }
        ack.server_time_us = NowUs();
        if (!Running() || !SendHelloAck(socket_.Get(), ack) || ack.result != 0) { Stop(); return; }
        LOG_INFO("HELLO_ACK ", ack.width, 'x', ack.height, " JPEG @", ack.fps, "fps");
        sender_ = std::jthread([this] {
            try { Send(); }
            catch (const std::exception& error) { LOG_ERROR("Sender: ", error.what()); Stop(); }
        });
        // No stage-4 heartbeat. select permits idle client input while detecting FIN and malformed traffic.
        while (Running()) {
            if (!Readable(socket_.Get(), 200)) continue;
            if (!ReadMessage(socket_.Get(), header, payload)) break;
            LOG_WARN("Ignoring unknown/unexpected client type: ", header.type);
        }
        LOG_INFO("client disconnected; waiting for connection");
        Stop();
    }
    void Produce(std::promise<std::pair<uint32_t, uint32_t>> ready) {
        bool signaled = false;
        try {
            // Stage-1 permitted simplification: capture and encode share a worker.
            // GDI DCs and COM objects are created, used and destroyed on this same thread.
            ComApartment apartment;
            auto capture = CreateGdiCapture();
            auto encoder = CreateJpegEncoder(options_.quality);
            if (!apartment.Ready() || !capture->Initialize(options_.width, options_.height) ||
                !encoder->Initialize(capture->Width(), capture->Height(), options_.fps)) {
                ready.set_value({0, 0}); return;
            }
            ready.set_value({capture->Width(), capture->Height()}); signaled = true;
            LOG_INFO("Capture: ", capture->Name(), "; encoder: ", encoder->Name());
            const auto interval = std::chrono::microseconds(1000000u / options_.fps);
            auto deadline = std::chrono::steady_clock::now();
            CapturedFrame captured;
            while (Running()) {
                EncodedFrame encoded;
                if (capture->Capture(captured) && encoder->Encode(captured, encoded)) {
                    dropped_.fetch_add(queue_.Push(std::move(encoded)));
                } else LOG_WARN("Capture/encode failed; skipping frame");
                deadline += interval;
                const auto now = std::chrono::steady_clock::now();
                if (deadline < now) deadline = now; // Never accumulate missed deadlines.
                std::unique_lock lock(wait_mutex_);
                wait_cv_.wait_until(lock, source_.get_token(), deadline, [this] { return !Running(); });
            }
        } catch (const std::exception& error) {
            LOG_ERROR("Capture/encoder: ", error.what());
            if (!signaled) ready.set_value({0, 0});
            else Stop();
        }
    }
    void Send() {
        EncodedFrame frame;
        uint32_t id = 0, interval_frames = 0;
        auto last_stats = NowUs();
        while (queue_.Pop(frame, source_.get_token())) {
            if (!SendFrame(socket_.Get(), frame, ++id)) break;
            ++interval_frames;
            const auto now = NowUs();
            if (options_.show_stats && now - last_stats >= 1000000) {
                LOG_INFO("sent fps=", static_cast<double>(interval_frames) * 1000000.0 / static_cast<double>(now - last_stats),
                    " total=", id, " dropped=", dropped_.load());
                last_stats = now; interval_frames = 0;
            }
        }
        Stop();
    }
    UniqueSocket socket_;
    Options options_;
    BoundedQueue<EncodedFrame> queue_{2};
    std::atomic<bool> running_{false};
    std::atomic<size_t> dropped_{0};
    std::stop_source source_;
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;
    std::jthread receiver_, producer_, sender_;
};
}
int HostMain(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        LOG_ERROR("Usage: padscreen-host [--port 51900] [--width 1280] [--height 800] [--fps 15] [--quality 70] [--verbose] [--show-stats]");
        return 2;
    }
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        LOG_HR(LastErrorHr(), "SetProcessDpiAwarenessContext"); return 1;
    }
    if (!SetConsoleCtrlHandler(ConsoleControl, TRUE)) { LOG_HR(LastErrorHr(), "SetConsoleCtrlHandler"); return 1; }
    WinsockRuntime winsock;
    if (!winsock.Ready()) return 1;
    TcpServer server;
    if (!server.Listen(options.port)) return 1;
    LOG_INFO("Listening on 127.0.0.1:", options.port, "; waiting for connection (Ctrl+C to stop)");
    std::unique_ptr<Session> session;
    while (!exit_requested.load()) {
        if (session && !session->Running()) session.reset();
        if (!Readable(server.Socket(), 200)) continue;
        std::string peer;
        UniqueSocket client(server.Accept(peer));
        if (client.Get() == INVALID_SOCKET) continue;
        // Keep accepting during streaming; the protocol requires a new connection to replace the old.
        if (session) { LOG_INFO("New client replaces previous connection"); session.reset(); }
        LOG_INFO("client connected: ", peer);
        session = std::make_unique<Session>(std::move(client), options);
        session->Run();
    }
    return 0;
}
}
int main(int argc, char** argv) {
    try { return pds::HostMain(argc, argv); }
    catch (const std::exception& error) { LOG_ERROR("Fatal: ", error.what()); return 1; }
}
