#pragma once
#include "net/tcp_server.h"
#include "encode/encoder.h"
#include "protocol/protocol.h"
namespace pds {
bool SendHelloAck(SOCKET socket, const HelloAck& ack);
bool SendFrame(SOCKET socket, const EncodedFrame& frame, uint32_t frame_id);
bool SendFrame(SOCKET socket, const EncodedFrame& frame, uint32_t frame_id, Codec codec);
}
