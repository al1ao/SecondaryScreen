#pragma once
#include "net/tcp_server.h"
#include "protocol/protocol.h"
#include <memory>
#include <string>
namespace pds {
class WgcCapture;
using WgcPreflight = HRESULT (*)(WgcCapture&, HMONITOR);
struct SessionOptions {
    uint16_t port = kDefaultPort;
    uint32_t width = 1280, height = 800, fps = 0, bitrate_kbps = 10000;
    uint32_t max_fps = 0;
    uint32_t queue_depth = 2;
    bool wgc_throttle = true;
    int quality = 70;
    bool show_stats = false, show_cursor = true, disable_hardware = false;
    bool capture_primary = false, auto_resize = true, force_extend = false, list_displays = false;
    bool width_explicit = false, height_explicit = false;
    std::wstring display_match, monitor_id;
};
class PipelineSession {
public:
    PipelineSession(UniqueSocket socket, SessionOptions options);
    // Allows deterministic failure-path tests without changing the user's desktop.
    PipelineSession(UniqueSocket socket, SessionOptions options, WgcPreflight preflight);
    ~PipelineSession();
    void Run();
    bool Running() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
