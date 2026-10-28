#include "net/tcp_server.h"
#include "protocol/protocol.h"
#include "encode/h264_annex_b.h"
#include "util/timer.h"
#include "util/log.h"
#include <psapi.h>
#include <algorithm>
#include <fstream>
#include <string_view>
#include <vector>
#include <stdexcept>
namespace {
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
class ProcessMetrics {
public:
    explicit ProcessMetrics(DWORD pid) {
        if (pid) { handle_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid); Check(handle_ != nullptr, "OpenProcess metrics"); }
    }
    ~ProcessMetrics() { if (handle_) CloseHandle(handle_); }
    uint64_t Memory() const {
        PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
        if (!handle_) return 0;
        Check(GetProcessMemoryInfo(handle_, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE, "process memory");
        return counters.PrivateUsage;
    }
    uint64_t CpuUs() const {
        if (!handle_) return 0;
        FILETIME created{}, exited{}, kernel{}, user{};
        Check(GetProcessTimes(handle_, &created, &exited, &kernel, &user) != FALSE, "process CPU time");
        auto ticks = [](const FILETIME& value) { return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        return (ticks(kernel) + ticks(user)) / 10;
    }
private:
    HANDLE handle_ = nullptr;
};
}
int main(int argc, char** argv) {
    try {
        uint16_t port = pds::kDefaultPort;
        unsigned seconds = 30, pid = 0;
        bool jpeg_only = false, expect_jpeg = false;
        std::string output_file;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key(argv[i]);
            if (key == "--jpeg-only") { jpeg_only = true; continue; }
            if (key == "--expect-jpeg") { expect_jpeg = true; continue; }
            if (i + 1 >= argc) throw std::runtime_error("probe argument missing");
            const std::string value(argv[++i]);
            if (key == "--port") port = static_cast<uint16_t>(std::stoul(value));
            else if (key == "--seconds") seconds = static_cast<unsigned>(std::stoul(value));
            else if (key == "--host-pid") pid = static_cast<unsigned>(std::stoul(value));
            else if (key == "--save") output_file = value;
            else throw std::runtime_error("unknown probe argument");
        }
        pds::WinsockRuntime winsock;
        Check(winsock.Ready(), "Winsock");
        pds::UniqueSocket socket_handle(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        Check(socket_handle.Get() != INVALID_SOCKET, "socket");
        const DWORD timeout = 15000;
        Check(setsockopt(socket_handle.Get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0, "probe receive timeout");
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        Check(connect(socket_handle.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "connect host");
        const auto header = pds::Serialize(pds::MakeHeader(pds::MessageType::Hello, sizeof(pds::Hello)));
        const auto hello = pds::Serialize(pds::Hello{1, 0, 1280, 800, 60, 320, 3840, 2160, jpeg_only ? 1u : 3u});
        Check(pds::TcpServer::SendAll(socket_handle.Get(), header.data(), header.size()) &&
            pds::TcpServer::SendAll(socket_handle.Get(), hello.data(), hello.size()), "send HELLO");
        std::vector<uint8_t> buffer(pds::kMaxPayload);
        auto read = [&](pds::MessageHeader& h) {
            return pds::TcpServer::RecvExact(socket_handle.Get(), &h, sizeof(h)) && pds::ValidHeader(h) &&
                pds::TcpServer::RecvExact(socket_handle.Get(), buffer.data(), h.length);
        };
        pds::MessageHeader wire{};
        Check(read(wire) && wire.type == static_cast<uint16_t>(pds::MessageType::HelloAck), "HELLO_ACK");
        pds::HelloAck ack{};
        Check(pds::Deserialize(std::span(buffer.data(), wire.length), ack) && ack.result == 0, "ACK result");
        LOG_INFO("PROBE ACK codec=", static_cast<unsigned>(ack.codec), " ", ack.width, 'x', ack.height, " fps=", ack.fps);
        Check(ack.codec == (expect_jpeg || jpeg_only ? 1 : 2), "negotiated expected codec");
        std::ofstream saved;
        if (!output_file.empty()) { saved.open(output_file, std::ios::binary); Check(saved.is_open(), "open H264 output file"); }
        ProcessMetrics metrics(pid);
        const auto connection_memory = metrics.Memory();
        const auto cpu_start_us = metrics.CpuUs();
        const auto began = pds::NowUs();
        auto log_time = began;
        uint64_t frames = 0, bytes = 0, keys = 0, latency_total = 0, latency_max = 0, gaps = 0;
        uint64_t missing_frames = 0, interval_start_frames = 0, peak_memory = connection_memory;
        uint64_t initial_memory = 0, final_memory = 0;
        uint32_t previous_id = 0;
        std::vector<uint64_t> latencies;
        std::array<uint64_t, 10> slice_counts{};
        while (pds::NowUs() - began < static_cast<uint64_t>(seconds) * 1000000) {
            Check(read(wire) && wire.type == static_cast<uint16_t>(pds::MessageType::VideoFrame), "continuous VIDEO_FRAME");
            pds::FrameInfo frame{};
            Check(pds::Deserialize(std::span(buffer.data(), sizeof(frame)), frame) && pds::ValidFrame(frame, wire.length) &&
                frame.codec == ack.codec && frame.width == ack.width && frame.height == ack.height && frame.frame_id > previous_id, "frame contract");
            if (previous_id && frame.frame_id != previous_id + 1) {
                ++gaps;
                missing_frames += static_cast<uint64_t>(frame.frame_id) - previous_id - 1;
            }
            previous_id = frame.frame_id;
            const auto data = std::span(buffer.data() + sizeof(frame), frame.payload_size);
            if (ack.codec == 2) {
                Check(pds::CountH264Slices(data, slice_counts), "valid H264 slice headers");
                Check(slice_counts[1] == 0 && slice_counts[6] == 0, "actual bitstream contains B slices");
                const bool idr = pds::HasH264Nal(data, 5);
                Check(idr == ((wire.flags & pds::kFlagKeyframe) != 0), "KEYFRAME flag agrees with IDR");
                if (idr) {
                    ++keys;
                    Check(data.size() > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1 &&
                        pds::HasH264Nal(data, 7) && pds::HasH264Nal(data, 8), "every keyframe includes Annex-B SPS/PPS");
                }
                if (saved.is_open()) saved.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            } else Check(data.size() > 4 && data[0] == 0xFF && data[1] == 0xD8 && data[data.size() - 1] == 0xD9, "JPEG fallback bytes");
            const auto now = pds::NowUs();
            const auto elapsed = now - frame.capture_ts_us;
            latency_total += elapsed; latency_max = (std::max)(latency_max, elapsed); latencies.push_back(elapsed);
            ++frames; bytes += wire.length + sizeof(pds::MessageHeader);
            if (!initial_memory && now - began >= 5000000) {
                initial_memory = metrics.Memory();
                peak_memory = (std::max)(peak_memory, initial_memory);
            }
            if (now - log_time >= 60000000) {
                const auto memory = metrics.Memory();
                peak_memory = (std::max)(peak_memory, memory);
                LOG_INFO("PROBE progress seconds=", (now - began) / 1000000, " frames=", frames, " keys=", keys,
                    " interval_fps=", static_cast<double>(frames - interval_start_frames) * 1000000.0 / static_cast<double>(now - log_time),
                    " missing_frames=", missing_frames, " private_bytes=", memory);
                interval_start_frames = frames;
                log_time = now;
            }
        }
        const double elapsed = static_cast<double>(pds::NowUs() - began) / 1000000.0;
        final_memory = metrics.Memory();
        peak_memory = (std::max)(peak_memory, final_memory);
        std::sort(latencies.begin(), latencies.end());
        Check(!latencies.empty(), "received frames");
        LOG_INFO("PROBE RESULT seconds=", elapsed, " frames=", frames, " keys=", keys,
            " fps=", static_cast<double>(frames) / elapsed, " Mbps=", static_cast<double>(bytes) * 8 / elapsed / 1000000.0,
            " host_capture_to_receive_avg_ms=", static_cast<double>(latency_total) / static_cast<double>(frames) / 1000.0,
            " p50_ms=", static_cast<double>(latencies[latencies.size() / 2]) / 1000.0,
            " p95_ms=", static_cast<double>(latencies[(latencies.size() - 1) * 95 / 100]) / 1000.0,
            " max_ms=", static_cast<double>(latency_max) / 1000.0, " encoded_gaps=", gaps, " missing_frames=", missing_frames,
            " private_start=", initial_memory, " private_end=", final_memory,
            " private_growth=", static_cast<int64_t>(final_memory) - static_cast<int64_t>(initial_memory),
            " private_connected=", connection_memory, " private_peak_sampled=", peak_memory,
            " private_growth_from_connect=", static_cast<int64_t>(final_memory) - static_cast<int64_t>(connection_memory),
            " host_cpu_single_core_pct=", static_cast<double>(metrics.CpuUs() - cpu_start_us) / (elapsed * 1000000.0) * 100.0);
        if (ack.codec == 2) {
            for (size_t type = 0; type < slice_counts.size(); ++type)
                LOG_INFO("PROBE slice_type[", type, "]=", slice_counts[type]);
            LOG_INFO("PROBE slices P=", slice_counts[0] + slice_counts[5], " B=", slice_counts[1] + slice_counts[6],
                " I=", slice_counts[2] + slice_counts[7], " SP=", slice_counts[3] + slice_counts[8], " SI=", slice_counts[4] + slice_counts[9]);
            Check(keys > 0, "received independently decodable keyframe");
        }
        return 0;
    } catch (const std::exception& error) { LOG_ERROR("PROBE FAIL: ", error.what()); return 1; }
}
