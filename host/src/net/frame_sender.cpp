#include "net/frame_sender.h"
#include <array>
#include "util/log.h"
namespace pds {
namespace {
bool SendBuffers(SOCKET socket, WSABUF* buffers, DWORD count) {
    DWORD index = 0;
    while (index < count) {
        DWORD sent = 0;
        if (WSASend(socket, buffers + index, count - index, &sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
            LOG_HR(HRESULT_FROM_WIN32(WSAGetLastError()), "WSASend"); return false;
        }
        if (sent == 0) return false;
        while (index < count && sent >= buffers[index].len) {
            sent -= buffers[index].len;
            ++index;
        }
        if (index < count && sent > 0) {
            buffers[index].buf += sent;
            buffers[index].len -= sent;
        }
    }
    return true;
}
}
bool SendHelloAck(SOCKET socket, const HelloAck& ack) {
    const auto header = Serialize(MakeHeader(MessageType::HelloAck, sizeof(HelloAck)));
    const auto payload = Serialize(ack);
    return TcpServer::SendAll(socket, header.data(), header.size()) &&
        TcpServer::SendAll(socket, payload.data(), payload.size());
}
bool SendFrame(SOCKET socket, const EncodedFrame& frame, uint32_t frame_id) {
    return SendFrame(socket, frame, frame_id, Codec::JPEG);
}
bool SendFrame(SOCKET socket, const EncodedFrame& frame, uint32_t frame_id, Codec codec) {
    if (frame.data.empty() || frame.data.size() > kMaxPayload - sizeof(FrameInfo)) return false;
    const auto payload_size = static_cast<uint32_t>(frame.data.size());
    const auto length = static_cast<uint32_t>(sizeof(FrameInfo)) + payload_size;
    const FrameInfo info{frame_id, static_cast<uint8_t>(codec), 0, 0,
        frame.width, frame.height, frame.capture_ts_us, payload_size};
    auto header = Serialize(MakeHeader(MessageType::VideoFrame, length, frame.keyframe ? kFlagKeyframe : 0));
    auto metadata = Serialize(info);
    // Scatter/gather keeps the JPEG vector intact; account for partial WSASend across all buffers.
    std::array<WSABUF, 3> buffers{{
        {static_cast<ULONG>(header.size()), reinterpret_cast<char*>(header.data())},
        {static_cast<ULONG>(metadata.size()), reinterpret_cast<char*>(metadata.data())},
        {static_cast<ULONG>(payload_size), reinterpret_cast<char*>(const_cast<uint8_t*>(frame.data.data()))}
    }};
    if (!SendBuffers(socket, buffers.data(), static_cast<DWORD>(buffers.size()))) return false;
    LOG_DEBUG("sent frame ", frame_id, " payload_size=", payload_size,
        " header.length=", length, " dimensions=", frame.width, 'x', frame.height);
    return true;
}
}
