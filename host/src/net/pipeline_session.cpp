#include "net/pipeline_session.h"
#include "net/frame_sender.h"
#include <roapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <thread>
#include "capture/wgc_capture.h"
#include "capture/gdi_monitor_capture.h"
#include "display/monitor_enum.h"
#include "encode/encoder_factory.h"
#include "encode/mf_h264_encoder.h"
#include "util/bounded_queue.h"
#include "util/gpu.h"
#include "util/timer.h"
namespace pds {
namespace {
struct RawFrame { CapturedFrame cpu; GpuFrame gpu; };
struct Packet { EncodedFrame frame; uint32_t id = 0; };
bool ReadMessage(SOCKET socket, MessageHeader& header, std::vector<uint8_t>& payload) {
    std::array<uint8_t, sizeof(MessageHeader)> bytes{};
    if (!TcpServer::RecvExact(socket, bytes.data(), bytes.size())) return false;
    if (!Deserialize(bytes, header) || !ValidHeader(header)) {
        LOG_ERROR("Protocol error: invalid magic/length"); return false;
    }
    return TcpServer::RecvExact(socket, payload.data(), header.length);
}
class WinRtApartment {
public:
    WinRtApartment() : hr_(RoInitialize(RO_INIT_MULTITHREADED)) { LOG_HR(hr_, "RoInitialize session"); }
    ~WinRtApartment() { if (SUCCEEDED(hr_)) RoUninitialize(); }
    bool Ready() const { return SUCCEEDED(hr_); }
private:
    HRESULT hr_;
};
}
struct PipelineSession::Impl {
    Impl(UniqueSocket value, SessionOptions opts) : socket(std::move(value)), options(opts) {}
    ~Impl() { Stop(); if (receiver.joinable()) receiver.join(); }
    void Stop() {
        if (!running.exchange(false)) return;
        stop.request_stop(); raw.Stop(); packets.Stop(); wait_cv.notify_all();
        if (shutdown(socket.Get(), SD_BOTH) == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error != WSAENOTCONN) LOG_HR(HRESULT_FROM_WIN32(error), "shutdown session");
        }
    }
    void JoinWorkers() {
        if (capture_worker.joinable()) capture_worker.join();
        if (encode_worker.joinable()) encode_worker.join();
        if (send_worker.joinable()) send_worker.join();
    }
    void Receive() {
        WinRtApartment apartment;
        try { if (apartment.Ready()) ReceiveLoop(); }
        catch (const std::exception& error) { LOG_ERROR("Session: ", error.what()); }
        Stop(); JoinWorkers();
        // All workers stop before COM/WinRT objects and the apartment are released.
        gpu_encoder.reset(); gpu_capture.reset(); jpeg_encoder.reset(); device.Reset();
        LOG_INFO("client disconnected; waiting for connection");
    }
    void ReceiveLoop() {
        std::vector<uint8_t> payload(kMaxPayload);
        MessageHeader header{};
        Hello hello{};
        for (;;) {
            if (!running.load() || !ReadMessage(socket.Get(), header, payload)) return;
            if (header.type == static_cast<uint16_t>(MessageType::Hello)) break;
            LOG_WARN("Ignoring unknown/unexpected type before HELLO: ", header.type);
        }
        if (!Deserialize(std::span(payload.data(), header.length), hello)) { LOG_ERROR("HELLO must be 32 bytes"); return; }
        LOG_INFO("HELLO received: ", hello.preferred_width, 'x', hello.preferred_height,
            " fps=", hello.preferred_fps, " caps=", hello.capabilities);
        HelloAck ack{};
        ack.protocol_version = kProtocolVersion;
        if (hello.protocol_version != kProtocolVersion) ack.result = static_cast<uint16_t>(HelloResult::Version);
        else if ((hello.capabilities & (kCapCodecJpeg | kCapCodecH264)) == 0) ack.result = static_cast<uint16_t>(HelloResult::Codec);
        if (ack.result == 0) InitializePipeline(hello, ack);
        ack.server_time_us = NowUs();
        if (!running.load() || !SendHelloAck(socket.Get(), ack) || ack.result != 0) return;
        LOG_INFO("HELLO_ACK ", width, 'x', height, " codec=", codec == Codec::H264 ? "H264" : "JPEG", " fps=", fps);
        if (codec == Codec::H264) capture_worker = std::jthread([this] { CaptureGpu(); });
        encode_worker = std::jthread([this] {
            try { Encode(); } catch (const std::exception& error) { LOG_ERROR("Encode worker: ", error.what()); Stop(); }
        });
        send_worker = std::jthread([this] {
            try { Send(); } catch (const std::exception& error) { LOG_ERROR("Send worker: ", error.what()); Stop(); }
        });
        while (running.load()) {
            if (!Readable(socket.Get(), 200)) continue;
            if (!ReadMessage(socket.Get(), header, payload)) break;
            LOG_WARN("Ignoring unknown/unexpected client type: ", header.type);
        }
    }
    void InitializePipeline(const Hello& hello, HelloAck& ack) {
        const auto monitors = EnumerateMonitors();
        const auto selected = SelectTargetMonitor(monitors, options.display_match, options.capture_primary, options.monitor_id);
        if (!selected) { LogMissingVirtualMonitor(); ack.result = static_cast<uint16_t>(HelloResult::Resolution); return; }
        monitor = *selected;
        if (options.auto_resize && SetDisplayResolution(monitor, hello.preferred_width, hello.preferred_height, options.fps ? options.fps : 60)) {
            const auto updated = EnumerateMonitors();
            const auto resized = SelectTargetMonitor(updated, options.display_match, false, monitor.monitor_device_id);
            if (!resized) { LogMissingVirtualMonitor(); ack.result = static_cast<uint16_t>(HelloResult::Resolution); return; }
            monitor = *resized;
        }
        LOG_INFO("Session selected display: ", Utf8(monitor.device_name), " (", Utf8(monitor.friendly_name), ") ", monitor.width, 'x', monitor.height);
        width = options.width; height = options.height;
        if (!options.capture_primary) {
            const auto size = NegotiateResolution(monitor.width, monitor.height, hello.preferred_width, hello.preferred_height,
                hello.decoder_max_width, hello.decoder_max_height,
                options.width_explicit ? options.width : 0, options.height_explicit ? options.height : 0);
            width = size.width; height = size.height;
        } else {
            // Retain the stage-2 primary-screen sizing path for regression/debugging.
            uint32_t max_width = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
            uint32_t max_height = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
            if (hello.decoder_max_width > 0) max_width = (std::min)(max_width, hello.decoder_max_width);
            if (hello.decoder_max_height > 0) max_height = (std::min)(max_height, hello.decoder_max_height);
            const double scale = (std::min)({1.0, static_cast<double>(max_width) / width, static_cast<double>(max_height) / height});
            width = static_cast<uint32_t>(width * scale) & ~1u; height = static_cast<uint32_t>(height * scale) & ~1u;
        }
        if (width < 2 || height < 2) { ack.result = static_cast<uint16_t>(HelloResult::Resolution); return; }
        if ((hello.capabilities & kCapCodecH264) != 0 && !options.disable_hardware) {
            fps = options.fps ? options.fps : 60;
            // TODO(design): probe installed adapters in order because a hardware
            // MFT on another GPU cannot consume this adapter's DXGI device manager.
            const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            for (unsigned index = 0; index < 8 && running.load() && std::chrono::steady_clock::now() < handshake_deadline; ++index) {
                if (!CreateCaptureDeviceForAdapter(index, device)) continue;
                gpu_encoder = CreateBestEncoder(device.Get(), width, height, fps, options.bitrate_kbps);
                if (!gpu_encoder) continue;
                auto capture = std::make_unique<WgcCapture>(options.show_cursor);
                const bool initialized = capture->InitializeForMonitor(device.Get(), monitor.hmonitor, width, height);
                gpu_capture = std::move(capture);
                if (initialized) {
                    GpuFrame first;
                    const auto deadline = (std::min)(handshake_deadline,
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500));
                    while (running.load() && std::chrono::steady_clock::now() < deadline && !gpu_capture->Capture(first))
                        WaitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
                    if (first.texture) {
                        codec = Codec::H264;
                        RawFrame initial; initial.gpu = std::move(first); raw.Push(std::move(initial));
                        break;
                    }
                    LOG_WARN("WGC produced no initial frame on this adapter; trying next adapter/JPEG");
                }
                gpu_capture.reset(); gpu_encoder.reset();
            }
        } else if (options.disable_hardware) LOG_INFO("Hardware encoding disabled by --disable-hardware");
        if (!gpu_encoder || !gpu_capture) {
            gpu_encoder.reset(); gpu_capture.reset(); device.Reset();
            if ((hello.capabilities & kCapCodecJpeg) == 0) { ack.result = static_cast<uint16_t>(HelloResult::Codec); return; }
            codec = Codec::JPEG; fps = options.fps ? options.fps : 15;
            LOG_WARN("Selected stage-1 fallback: GDI + WIC JPEG @", fps, "fps; monitor=", Utf8(monitor.device_name));
            std::promise<std::pair<uint32_t, uint32_t>> ready;
            auto initialized = ready.get_future();
            capture_worker = std::jthread([this, signal = std::move(ready)]() mutable { CaptureCpu(std::move(signal)); });
            const auto dimensions = initialized.get();
            width = dimensions.first; height = dimensions.second;
            jpeg_encoder = CreateJpegEncoder(options.quality);
            if (width == 0 || height == 0 || !jpeg_encoder->Initialize(width, height, fps)) {
                ack.result = static_cast<uint16_t>(HelloResult::Resolution); return;
            }
        }
        ack.codec = static_cast<uint8_t>(codec); ack.width = width; ack.height = height;
        ack.fps = static_cast<uint16_t>(fps); ack.frame_interval_us = 1000000u / fps;
        ack.bitrate_kbps = codec == Codec::H264 ? options.bitrate_kbps : 0;
    }
    void WaitUntil(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(wait_mutex);
        wait_cv.wait_until(lock, stop.get_token(), deadline, [this] { return !running.load(); });
    }
    void CaptureCpu(std::promise<std::pair<uint32_t, uint32_t>> ready) {
        bool signaled = false;
        try {
            auto capture = options.capture_primary ? CreateGdiCapture() : CreateGdiMonitorCapture(monitor, options.show_cursor);
            if (!capture->Initialize(width, height)) { ready.set_value({0, 0}); return; }
            ready.set_value({capture->Width(), capture->Height()}); signaled = true;
            const auto interval = std::chrono::microseconds(1000000u / fps);
            auto deadline = std::chrono::steady_clock::now();
            while (running.load()) {
                RawFrame frame;
                if (capture->Capture(frame.cpu)) raw_dropped.fetch_add(raw.Push(std::move(frame)));
                else if (!options.capture_primary) { LOG_WARN("Selected GDI display unavailable; closing session for renegotiation"); Stop(); break; }
                else LOG_WARN("GDI capture failed; skipping frame");
                deadline = (std::max)(deadline + interval, std::chrono::steady_clock::now());
                WaitUntil(deadline);
            }
        } catch (const std::exception& error) {
            LOG_ERROR("GDI capture worker: ", error.what());
            if (!signaled) ready.set_value({0, 0}); else Stop();
        }
    }
    void CaptureGpu() {
        try {
            const auto interval = std::chrono::microseconds(1000000u / fps);
            auto deadline = std::chrono::steady_clock::now();
            bool first = true;
            while (running.load()) {
                RawFrame frame;
                if (gpu_capture->Capture(frame.gpu)) {
                    if (first) { LOG_INFO("WGC first captured frame"); first = false; }
                    raw_dropped.fetch_add(raw.Push(std::move(frame)));
                    deadline = (std::max)(deadline + interval, std::chrono::steady_clock::now());
                    WaitUntil(deadline);
                } else {
                    if (static_cast<WgcCapture*>(gpu_capture.get())->Failed()) { LOG_WARN("WGC stopped; reconnect required"); Stop(); break; }
                    WaitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(2));
                }
            }
        } catch (const std::exception& error) { LOG_ERROR("WGC capture worker: ", error.what()); Stop(); }
    }
    void Encode() {
        ComApartment apartment;
        if (!apartment.Ready()) { Stop(); return; }
        RawFrame input;
        uint32_t id = 0;
        bool first_input = true;
        while (raw.Pop(input, stop.get_token())) {
            if (first_input) { LOG_INFO("Encoding first captured frame"); first_input = false; }
            Packet packet;
            const auto began = NowUs();
            bool produced = false;
            if (codec == Codec::H264) {
                if (force_keyframe.exchange(false) && !gpu_encoder->ForceKeyframe()) { Stop(); break; }
                produced = gpu_encoder->Encode(input.gpu.texture.Get(), input.gpu.capture_ts_us, packet.frame);
                if (!static_cast<MfH264Encoder*>(gpu_encoder.get())->Healthy()) { LOG_ERROR("MF encoder failed; closing session for renegotiation"); Stop(); break; }
            } else produced = jpeg_encoder->Encode(input.cpu, packet.frame);
            if (produced) {
                encode_us.fetch_add(codec == Codec::H264 ? static_cast<MfH264Encoder*>(gpu_encoder.get())->LastEncodeTimeUs() : NowUs() - began);
                encoded.fetch_add(1);
                packet.id = ++id;
                const auto dropped = packets.Push(std::move(packet));
                encoded_dropped.fetch_add(dropped);
                if (dropped != 0 && codec == Codec::H264) force_keyframe.store(true);
            } else if (codec == Codec::JPEG) LOG_WARN("JPEG encode failed; skipping frame");
        }
    }
    void Send() {
        Packet packet;
        uint32_t last_id = 0;
        uint64_t sent = 0, send_us = 0;
        auto last_stats = NowUs();
        while (packets.Pop(packet, stop.get_token())) {
            // Never render a P-frame after an encoded-queue gap. Request an IDR
            // and resume there so dropping old frames cannot leave broken references.
            if (codec == Codec::H264 && packet.id != last_id + 1 && !packet.frame.keyframe) {
                force_keyframe.store(true); encoded_dropped.fetch_add(1); continue;
            }
            const auto began = NowUs();
            if (!SendFrame(socket.Get(), packet.frame, packet.id, codec)) break;
            const auto now = NowUs();
            send_us += now - began; ++sent; last_id = packet.id;
            if (now - last_stats >= 5000000) {
                const auto count = encoded.exchange(0), total_encode = encode_us.exchange(0);
                const auto capture_drops = codec == Codec::H264 ? static_cast<WgcCapture*>(gpu_capture.get())->Dropped() : 0;
                LOG_INFO("fps=", static_cast<double>(sent) * 1000000.0 / static_cast<double>(now - last_stats),
                    " encode=", count ? static_cast<double>(total_encode) / static_cast<double>(count) / 1000.0 : 0.0,
                    "ms send=", sent ? static_cast<double>(send_us) / static_cast<double>(sent) / 1000.0 : 0.0,
                    "ms queue=", packets.Size(), " drop=", raw_dropped.load() + encoded_dropped.load() + capture_drops,
                    " raw_drop=", raw_dropped.load(), " encoded_drop=", encoded_dropped.load(), " wgc_drop=", capture_drops);
                if (options.show_stats) LOG_INFO("capture_to_send=", static_cast<double>(now - packet.frame.capture_ts_us) / 1000.0, "ms (host clock only)");
                sent = 0; send_us = 0; last_stats = now;
            }
        }
        Stop();
    }
    UniqueSocket socket;
    SessionOptions options;
    MonitorInfo monitor;
    BoundedQueue<RawFrame> raw{2};
    BoundedQueue<Packet> packets{2};
    std::atomic<bool> running{false}, force_keyframe{false};
    std::atomic<uint64_t> raw_dropped{0}, encoded_dropped{0}, encoded{0}, encode_us{0};
    std::stop_source stop;
    std::condition_variable_any wait_cv;
    std::mutex wait_mutex;
    std::jthread receiver, capture_worker, encode_worker, send_worker;
    Codec codec = Codec::JPEG;
    uint32_t width = 0, height = 0, fps = 0;
    ComPtr<ID3D11Device> device;
    std::unique_ptr<IGpuCapture> gpu_capture;
    std::unique_ptr<IGpuVideoEncoder> gpu_encoder;
    std::unique_ptr<IVideoEncoder> jpeg_encoder;
};
PipelineSession::PipelineSession(UniqueSocket socket, SessionOptions options) : impl_(std::make_unique<Impl>(std::move(socket), options)) {}
PipelineSession::~PipelineSession() = default;
void PipelineSession::Run() { impl_->running.store(true); impl_->receiver = std::jthread([this] { impl_->Receive(); }); }
bool PipelineSession::Running() const { return impl_->running.load(); }
}
