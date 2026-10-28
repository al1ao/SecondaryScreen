#pragma once
#include "net/tcp_server.h"
#include "protocol/protocol.h"
#include <memory>
namespace pds {
struct SessionOptions {
    uint16_t port = kDefaultPort;
    uint32_t width = 1280, height = 800, fps = 0, bitrate_kbps = 10000;
    int quality = 70;
    bool show_stats = false, show_cursor = true, disable_hardware = false;
};
class PipelineSession {
public:
    PipelineSession(UniqueSocket socket, SessionOptions options);
    ~PipelineSession();
    void Run();
    bool Running() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
