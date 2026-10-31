#include "net/pipeline_session.h"
#include "net/frame_sender.h"
#include "input/input_injector.h"
#include <roapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <thread>
#include <mutex>
#include "capture/wgc_capture.h"
#include "capture/gdi_monitor_capture.h"
#include "display/monitor_enum.h"
#include "display/adaptive_mode.h"
#include "encode/encoder_factory.h"
#include "encode/mf_h264_encoder.h"
#include "util/bounded_queue.h"
#include "util/gpu.h"
#include "util/timer.h"
#include "util/timing_stats.h"
#include "util/frame_dump.h"
namespace pds {
namespace {
struct RawFrame { CapturedFrame cpu; GpuFrame gpu; };
struct Packet { EncodedFrame frame; uint32_t id = 0; };
HRESULT PrepareWgc(WgcCapture& capture, HMONITOR monitor) { return capture.PrepareForMonitor(monitor); }
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
    Impl(UniqueSocket value, SessionOptions opts, WgcPreflight check) : socket(std::move(value)), options(opts), raw(opts.queue_depth), packets(opts.queue_depth), preflight(check ? check : PrepareWgc) {}
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
        Stop(); JoinWorkers(); injector.Release();
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
        const auto handshake_started = NowUs();
        ack.protocol_version = kProtocolVersion;
        if (hello.protocol_version != kProtocolVersion) ack.result = static_cast<uint16_t>(HelloResult::Version);
        else if ((hello.capabilities & (kCapCodecJpeg | kCapCodecH264)) == 0) ack.result = static_cast<uint16_t>(HelloResult::Codec);
        if (ack.result == 0) InitializePipeline(hello, ack);
        ack.server_time_us = NowUs();
        const auto rotation = AckRotation(monitor.orientation, codec == Codec::H264);
        ack.reserved0 = kAckCapTouchInput;
        if (hello.capabilities & kCapRotateOutput)
            ack.reserved0 |= static_cast<uint8_t>((rotation / 90u) << kAckRotationShift);
        else if (monitor.orientation != 0 || rotation != 0)
            LOG_WARN("Client does not advertise rotated output support; display orientation=", monitor.orientation);
        LOG_INFO("HELLO_ACK ready: handshake_ms=", static_cast<double>(ack.server_time_us - handshake_started) / 1000.0,
            " result=", ack.result, " codec=", static_cast<unsigned>(ack.codec));
        if (!running.load() || !SendHelloAck(socket.Get(), ack) || ack.result != 0) return;
        LOG_INFO("HELLO_ACK ", width, 'x', height, " codec=", codec == Codec::H264 ? "H264" : "JPEG",
            " fps=", fps, " orientation=", monitor.orientation, " rotation=", rotation);
        if (codec == Codec::H264) capture_worker = std::jthread([this] { CaptureGpu(); });
        encode_worker = std::jthread([this] {
            try { Encode(); } catch (const std::exception& error) { LOG_ERROR("Encode worker: ", error.what()); Stop(); }
        });
        send_worker = std::jthread([this] {
            try { Send(); } catch (const std::exception& error) { LOG_ERROR("Send worker: ", error.what()); Stop(); }
        });
        const auto initial_display = SignatureOf(monitor);
        auto last_display_check = NowUs();
        while (running.load()) {
            const auto now = NowUs();
            if (now - last_display_check >= 1000000) {
                last_display_check = now;
                const auto current = ReadDisplaySignature(monitor);
                if (!current || *current != initial_display) {
                    if (current) LOG_INFO("Display changed: ", initial_display.width, 'x', initial_display.height,
                        '@', initial_display.refresh, " orientation=", initial_display.orientation, " -> ",
                        current->width, 'x', current->height, '@', current->refresh,
                        " orientation=", current->orientation, "; reconnecting");
                    else LOG_INFO("Selected display disappeared or changed identity; reconnecting");
                    Stop(); break;
                }
            }
            if (!Readable(socket.Get(), 200)) continue;
            if (!ReadMessage(socket.Get(), header, payload)) break;
            const auto type = static_cast<MessageType>(header.type);
            if (type == MessageType::Ping) {
                Ping ping{};
                const auto received = NowUs();
                if (!Deserialize(std::span(payload.data(), header.length), ping)) continue;
                if (received - last_ping_us < 100000) continue;
                last_ping_us = received;
                Pong pong{ping.client_send_us, received, 0};
                const auto pong_header = Serialize(MakeHeader(MessageType::Pong, sizeof(Pong)));
                std::lock_guard lock(write_mutex);
                pong.host_send_us = NowUs();
                const auto pong_bytes = Serialize(pong);
                if (!TcpServer::SendAll(socket.Get(), pong_header.data(), pong_header.size()) ||
                    !TcpServer::SendAll(socket.Get(), pong_bytes.data(), pong_bytes.size())) break;
            } else if (type == MessageType::Input) {
                TouchInput input{};
                if (Deserialize(std::span(payload.data(), header.length), input)) injector.OnTouch(input);
            } else LOG_WARN("Ignoring unknown/unexpected client type: ", header.type);
        }
    }
    void InitializePipeline(const Hello& hello, HelloAck& ack) {
        // TODO(design): this bounds retries between synchronous Windows/driver
        // calls, not the duration of an individual uninterruptible driver call.
        // Preflight denial avoids entering any such D3D/MF call in this session.
        const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        const auto within_budget = [&](const char* step) {
            if (!running.load()) return false;
            if (std::chrono::steady_clock::now() < handshake_deadline) return true;
            LOG_WARN("Hardware handshake retry budget exhausted at ", step, "; leaving time for JPEG fallback");
            return false;
        };
        const auto monitors = EnumerateMonitors();
        const auto selected = SelectTargetMonitor(monitors, options.display_match, options.capture_primary, options.monitor_id);
        if (!selected) { LogMissingVirtualMonitor(); ack.result = static_cast<uint16_t>(HelloResult::Resolution); return; }
        monitor = *selected;
        const auto requested = ResolveVideoRequest(options, hello);
        if (requested.width < 2 || requested.height < 2) { ack.result = static_cast<uint16_t>(HelloResult::Resolution); return; }
        bool display_lost = false;
        const auto mode = AdaptDisplayMode({monitor.width, monitor.height, monitor.refresh_rate}, requested,
            options.auto_resize && !options.capture_primary && monitor.is_virtual && !monitor.is_primary,
            [&](const VideoMode& target) -> std::optional<VideoMode> {
                if (display_lost || !SetDisplayResolution(monitor, target.width, target.height, target.fps)) return std::nullopt;
                const auto updated = EnumerateMonitors();
                const auto resized = SelectTargetMonitor(updated, options.display_match, false, monitor.monitor_device_id);
                if (!resized) { display_lost = true; return std::nullopt; }
                monitor = *resized;
                return VideoMode{monitor.width, monitor.height, monitor.refresh_rate ? monitor.refresh_rate : target.fps};
            },
            [&] {
                std::vector<VideoMode> modes;
                for (const auto& available : EnumerateDisplayModes(monitor.device_name))
                    modes.push_back({available.width, available.height, available.fps});
                return modes;
            });
        if (display_lost) { LogMissingVirtualMonitor(); ack.result = static_cast<uint16_t>(HelloResult::Resolution); return; }
        injector.UpdateTarget(monitor.rect);
        LOG_INFO("Session selected display: ", Utf8(monitor.device_name), " (", Utf8(monitor.friendly_name), ") ", monitor.width, 'x', monitor.height);
        LOG_INFO("Pipeline options: wgc_throttle=", options.wgc_throttle ? "on" : "off", " queue_depth=", options.queue_depth,
            " target_fps=", mode.fps, " max_fps=", options.max_fps, " auto_resize=", options.auto_resize ? "on" : "off");
        width = requested.width; height = requested.height;
        if (!options.capture_primary) {
            const auto size = NegotiateResolution(mode.width, mode.height, requested.width, requested.height,
                hello.decoder_max_width, hello.decoder_max_height,
                requested.width, requested.height);
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
            fps = mode.fps;
            // Check desktop access before any D3D/MF driver initialization. A
            // denied capture item cannot be repaired by switching adapters.
            auto capture = std::make_unique<WgcCapture>(options.show_cursor);
            HRESULT prepared = E_ABORT;
            if (within_budget("WGC preflight")) prepared = preflight(*capture, monitor.hmonitor);
            if (prepared == E_ACCESSDENIED)
                LOG_ERROR("WGC requires an interactive desktop; not retrying other adapters (E_ACCESSDENIED).");
            else if (FAILED(prepared)) LOG_HR(prepared, "WGC preflight; skipping adapter probes");
            for (unsigned index = 0; SUCCEEDED(prepared) && index < 8; ++index) {
                if (!within_budget("adapter entry")) break;
                if (!CreateCaptureDeviceForAdapter(index, device)) continue;
                if (!within_budget("D3D device creation")) break;
                if (!capture) {
                    capture = std::make_unique<WgcCapture>(options.show_cursor);
                    prepared = preflight(*capture, monitor.hmonitor);
                    if (FAILED(prepared)) {
                        LOG_HR(prepared, "WGC capture-item preflight on retry; stopping adapter probes"); break;
                    }
                    if (!within_budget("WGC retry preflight")) break;
                }
                if (!capture->ConfigureThrottle(options.wgc_throttle, fps)) {
                    LOG_ERROR("Invalid WGC throttle configuration");
                    ack.result = static_cast<uint16_t>(HelloResult::Resolution); return;
                }
                const bool initialized = capture->InitializeForMonitor(device.Get(), monitor.hmonitor, width, height);
                const auto capture_result = capture->InitializationResult();
                if (capture_result == E_ACCESSDENIED) {
                    LOG_ERROR("WGC session access denied; not retrying other adapters."); break;
                }
                if (!within_budget("WGC initialization")) break;
                if (initialized) {
                    LOG_INFO("WGC item.Size=", capture->Width(), 'x', capture->Height(),
                        " rcMonitor=", monitor.width, 'x', monitor.height,
                        " orientation=", monitor.orientation);
                    const auto output = NegotiateResolution(capture->Width(), capture->Height(),
                        requested.width, requested.height, hello.decoder_max_width, hello.decoder_max_height,
                        requested.width, requested.height);
                    if (output.width < 2 || output.height < 2) { capture.reset(); continue; }
                    width = output.width; height = output.height;
                    gpu_encoder = CreateBestEncoder(device.Get(), width, height, fps, options.bitrate_kbps);
                    if (!within_budget("MF encoder creation")) { gpu_encoder.reset(); break; }
                    if (!gpu_encoder) { capture.reset(); continue; }
                    GpuFrame first;
                    const auto deadline = (std::min)(handshake_deadline,
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500));
                    while (running.load() && std::chrono::steady_clock::now() < deadline && !capture->Capture(first))
                        WaitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(5));
                    if (!within_budget("first WGC frame")) { gpu_encoder.reset(); break; }
                    if (first.texture) {
                        LOG_INFO("First WGC frame texture=", first.width, 'x', first.height,
                            " encoder=", width, 'x', height, " orientation=", monitor.orientation);
                        // MF initialization may span several WGC callbacks. Start
                        // with the newest owned texture, not an old startup frame.
                        GpuFrame newer;
                        while (capture->Capture(newer)) first = std::move(newer);
                        if (!options.dump_first_frame.empty())
                            DumpFirstFrame(first, options.dump_first_frame, "wgc", monitor.orientation);
                        gpu_capture = std::move(capture);
                        codec = Codec::H264;
                        RawFrame initial; initial.gpu = std::move(first); raw.Push(std::move(initial));
                        break;
                    }
                    LOG_WARN("WGC produced no initial frame on this adapter; trying next adapter/JPEG");
                }
                capture.reset(); gpu_encoder.reset();
            }
            if (!gpu_capture) {
                gpu_encoder.reset();
                LOG_WARN("WGC/H.264 hardware startup unavailable or retry budget exhausted; falling back to JPEG when supported");
            }
        } else if (options.disable_hardware) LOG_INFO("Hardware encoding disabled by --disable-hardware");
        if (!gpu_encoder || !gpu_capture) {
            gpu_encoder.reset(); gpu_capture.reset(); device.Reset();
            if ((hello.capabilities & kCapCodecJpeg) == 0) { ack.result = static_cast<uint16_t>(HelloResult::Codec); return; }
            codec = Codec::JPEG; fps = options.fps ? mode.fps : (std::min)(mode.fps, 15u);
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
    void RecordCaptureGap(uint64_t& previous_us) {
        const auto now = NowUs();
        if (previous_us != 0) {
            const auto gap = now - previous_us;
            auto maximum = capture_gap_max_us.load();
            while (gap > maximum && !capture_gap_max_us.compare_exchange_weak(maximum, gap)) {}
        }
        previous_us = now;
    }
    void CaptureCpu(std::promise<std::pair<uint32_t, uint32_t>> ready) {
        bool signaled = false;
        uint64_t last_capture_us = 0;
        try {
            ComApartment apartment;
            auto capture = options.capture_primary ? CreateGdiCapture() : CreateGdiMonitorCapture(monitor, options.show_cursor);
            if (!capture->Initialize(width, height)) { ready.set_value({0, 0}); return; }
            ready.set_value({capture->Width(), capture->Height()}); signaled = true;
            const auto interval = std::chrono::microseconds(1000000u / fps);
            auto deadline = std::chrono::steady_clock::now();
            bool dump_attempted = false;
            while (running.load()) {
                RawFrame frame;
                if (capture->Capture(frame.cpu)) {
                    RecordCaptureGap(last_capture_us);
                    if (!dump_attempted && !options.dump_first_frame.empty()) {
                        dump_attempted = true;
                        DumpFirstFrame(frame.cpu, options.dump_first_frame, "gdi", monitor.orientation);
                    }
                    raw_dropped.fetch_add(raw.Push(std::move(frame)));
                }
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
        uint64_t last_capture_us = 0;
        try {
            const auto interval = std::chrono::microseconds(1000000u / fps);
            auto deadline = std::chrono::steady_clock::now();
            bool first = true;
            while (running.load()) {
                RawFrame frame;
                if (gpu_capture->Capture(frame.gpu)) {
                    RecordCaptureGap(last_capture_us);
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
        const auto enqueue = [&](Packet& packet, uint64_t began) {
            encode_timings.Record(codec == Codec::H264 ? static_cast<MfH264Encoder*>(gpu_encoder.get())->LastEncodeTimeUs() : NowUs() - began);
            packet.id = ++id;
            const auto dropped = packets.Push(std::move(packet));
            encoded_dropped.fetch_add(dropped);
            if (dropped != 0 && codec == Codec::H264) force_keyframe.store(true);
        };
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
            if (produced) enqueue(packet, began);
            else if (codec == Codec::JPEG) LOG_WARN("JPEG encode failed; skipping frame");
            if (codec == Codec::H264) {
                // A deadline miss can leave both the previous and current
                // sample ready. Drain both now instead of retaining one forever.
                auto* hardware = static_cast<MfH264Encoder*>(gpu_encoder.get());
                while (hardware->Poll(packet.frame)) enqueue(packet, began);
                if (!hardware->Healthy()) { LOG_ERROR("MF output drain failed; reconnect required"); Stop(); break; }
            }
        }
    }
    void Send() {
        Packet packet;
        uint32_t last_id = 0;
        uint64_t sent = 0, send_us = 0, send_block_max_us = 0, idr = 0;
        auto last_stats = NowUs();
        while (packets.Pop(packet, stop.get_token())) {
            // Never render a P-frame after an encoded-queue gap. Request an IDR
            // and resume there so dropping old frames cannot leave broken references.
            if (codec == Codec::H264 && packet.id != last_id + 1 && !packet.frame.keyframe) {
                force_keyframe.store(true); encoded_dropped.fetch_add(1); continue;
            }
            const auto began = NowUs();
            { std::lock_guard lock(write_mutex);
              if (!SendFrame(socket.Get(), packet.frame, packet.id, codec)) break; }
            const auto now = NowUs();
            send_us += now - began; send_block_max_us = (std::max)(send_block_max_us, now - began);
            if (packet.frame.keyframe) ++idr;
            ++sent; last_id = packet.id;
            if (now - last_stats >= 5000000) {
                const auto timings = encode_timings.TakeWindow();
                const auto count = timings.window.count, total_encode = timings.window.total_us;
                const auto capture_drops = codec == Codec::H264 ? static_cast<WgcCapture*>(gpu_capture.get())->Dropped() : 0;
                LOG_INFO("fps=", static_cast<double>(sent) * 1000000.0 / static_cast<double>(now - last_stats),
                    " encode=", count ? static_cast<double>(total_encode) / static_cast<double>(count) / 1000.0 : 0.0,
                    "ms encode_p95_upper=", static_cast<double>(timings.window.p95_upper_us) / 1000.0,
                    "ms encode_session_avg=", timings.session.count ? static_cast<double>(timings.session.total_us) / static_cast<double>(timings.session.count) / 1000.0 : 0.0,
                    "ms encode_session_p95_upper=", static_cast<double>(timings.session.p95_upper_us) / 1000.0,
                    "ms encode_samples=", timings.session.count,
                    " send=", sent ? static_cast<double>(send_us) / static_cast<double>(sent) / 1000.0 : 0.0,
                    "ms send_block_max_ms=", static_cast<double>(send_block_max_us) / 1000.0,
                    " idr=", idr, " capture_gap_ms=", static_cast<double>(capture_gap_max_us.exchange(0)) / 1000.0,
                    "ms queue=", packets.Size(), " drop=", raw_dropped.load() + encoded_dropped.load() + capture_drops,
                    " raw_drop=", raw_dropped.load(), " encoded_drop=", encoded_dropped.load(), " wgc_drop=", capture_drops,
                    " wgc_throttled=", codec == Codec::H264 ? static_cast<WgcCapture*>(gpu_capture.get())->Throttled() : 0,
                    " wgc_received=", codec == Codec::H264 ? static_cast<WgcCapture*>(gpu_capture.get())->Received() : 0,
                    " wgc_copied=", codec == Codec::H264 ? static_cast<WgcCapture*>(gpu_capture.get())->Copied() : 0);
                if (options.show_stats) LOG_INFO("capture_to_send=", static_cast<double>(now - packet.frame.capture_ts_us) / 1000.0, "ms (host clock only)");
                sent = 0; send_us = 0; send_block_max_us = 0; idr = 0; last_stats = now;
            }
        }
        Stop();
    }
    UniqueSocket socket;
    SessionOptions options;
    MonitorInfo monitor;
    InputInjector injector;
    std::mutex write_mutex;
    uint64_t last_ping_us = 0;
    BoundedQueue<RawFrame> raw;
    BoundedQueue<Packet> packets;
    std::atomic<bool> running{false}, force_keyframe{false};
    std::atomic<uint64_t> raw_dropped{0}, encoded_dropped{0};
    std::atomic<uint64_t> capture_gap_max_us{0};
    TimingStats encode_timings;
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
    WgcPreflight preflight;
};
PipelineSession::PipelineSession(UniqueSocket socket, SessionOptions options)
    : PipelineSession(std::move(socket), std::move(options), PrepareWgc) {}
PipelineSession::PipelineSession(UniqueSocket socket, SessionOptions options, WgcPreflight preflight)
    : impl_(std::make_unique<Impl>(std::move(socket), std::move(options), preflight)) {}
PipelineSession::~PipelineSession() = default;
void PipelineSession::Run() { impl_->running.store(true); impl_->receiver = std::jthread([this] { impl_->Receive(); }); }
bool PipelineSession::Running() const { return impl_->running.load(); }
}
