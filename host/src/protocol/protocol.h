#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace pds {
constexpr uint32_t kMagic = 0x31534450u;
constexpr uint16_t kProtocolVersion = 1;
constexpr uint32_t kMaxPayload = 16u * 1024u * 1024u;
constexpr uint16_t kDefaultPort = 51900;
constexpr uint16_t kFlagKeyframe = 1;
constexpr uint16_t kFlagEndOfStream = 2;
constexpr uint32_t kCapCodecJpeg = 1;
constexpr uint32_t kCapCodecH264 = 2;
constexpr uint32_t kCapCodecHevc = 4;
enum class MessageType : uint16_t {
    Hello = 0x0001, HelloAck = 0x0002, VideoFrame = 0x0010,
    Stats = 0x0020, Ping = 0x0030, Pong = 0x0031, Bye = 0x00FF
};
enum class HelloResult : uint16_t { Ok = 0, Version = 1, Codec = 2, Resolution = 3 };
#pragma pack(push, 1)
struct MessageHeader { uint32_t magic; uint16_t type; uint16_t flags; uint32_t length; };
struct Hello {
    uint16_t protocol_version; uint16_t reserved;
    uint32_t preferred_width; uint32_t preferred_height; uint32_t preferred_fps;
    uint32_t screen_density_dpi; uint32_t decoder_max_width; uint32_t decoder_max_height;
    uint32_t capabilities;
};
struct HelloAck {
    uint16_t protocol_version; uint16_t result;
    uint8_t codec; uint8_t reserved0; uint16_t fps;
    uint32_t width; uint32_t height; uint32_t bitrate_kbps;
    uint32_t frame_interval_us; uint64_t server_time_us;
};
struct FrameInfo {
    uint32_t frame_id; uint8_t codec; uint8_t frame_flags; uint16_t reserved;
    uint32_t width; uint32_t height; uint64_t capture_ts_us; uint32_t payload_size;
};
#pragma pack(pop)
static_assert(sizeof(MessageHeader) == 12);
static_assert(sizeof(Hello) == 32);
static_assert(sizeof(HelloAck) == 32);
static_assert(sizeof(FrameInfo) == 28);
static_assert(offsetof(FrameInfo, capture_ts_us) == 16);
static_assert(offsetof(HelloAck, server_time_us) == 24);
static_assert(std::endian::native == std::endian::little);

template <typename T> std::array<uint8_t, sizeof(T)> Serialize(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<uint8_t, sizeof(T)> result{};
    std::memcpy(result.data(), &value, sizeof(T));
    return result;
}
template <typename T> bool Deserialize(std::span<const uint8_t> bytes, T& out) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (bytes.size() != sizeof(T)) return false;
    std::memcpy(&out, bytes.data(), sizeof(T));
    return true;
}
MessageHeader MakeHeader(MessageType type, uint32_t length, uint16_t flags = 0);
bool ValidHeader(const MessageHeader& header);
bool ValidFrame(const FrameInfo& info, uint32_t message_length);
}
