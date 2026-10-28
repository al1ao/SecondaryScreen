#include "protocol/protocol.h"
namespace pds {
MessageHeader MakeHeader(MessageType type, uint32_t length, uint16_t flags) {
    return {kMagic, static_cast<uint16_t>(type), flags, length};
}
bool ValidHeader(const MessageHeader& header) {
    return header.magic == kMagic && header.length <= kMaxPayload;
}
bool ValidFrame(const FrameInfo& info, uint32_t message_length) {
    return message_length >= sizeof(FrameInfo) && message_length <= kMaxPayload &&
        info.payload_size == message_length - sizeof(FrameInfo) && info.payload_size > 0 &&
        info.width > 0 && info.height > 0;
}
}
