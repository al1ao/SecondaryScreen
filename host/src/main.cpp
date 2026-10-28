#include "net/pipeline_session.h"
#include <atomic>
#include <charconv>
#include <memory>
#include <string_view>
#include "util/log.h"
namespace pds {
namespace {
std::atomic<bool> exit_requested{false};
BOOL WINAPI ConsoleControl(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT) return FALSE;
    exit_requested.store(true); return TRUE;
}
bool ParseOptions(int argc, char** argv, SessionOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--verbose") { g_log_level.store(LogLevel::Debug); continue; }
        if (key == "--show-stats") { options.show_stats = true; continue; }
        if (key == "--disable-hardware") { options.disable_hardware = true; continue; }
        if (key == "--show-cursor") {
            options.show_cursor = true;
            if (i + 1 < argc && (std::string_view(argv[i + 1]) == "false" || std::string_view(argv[i + 1]) == "0")) {
                options.show_cursor = false; ++i;
            } else if (i + 1 < argc && (std::string_view(argv[i + 1]) == "true" || std::string_view(argv[i + 1]) == "1")) ++i;
            continue;
        }
        if (i + 1 >= argc) return false;
        const std::string_view text(argv[++i]);
        uint32_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) return false;
        // TODO(design): keep simple bounded CLI values; reject instead of silently defaulting.
        if (key == "--port" && value > 0 && value <= 65535) options.port = static_cast<uint16_t>(value);
        else if (key == "--width" && value >= 2 && value <= 16384) options.width = value & ~1u;
        else if (key == "--height" && value >= 2 && value <= 16384) options.height = value & ~1u;
        else if (key == "--fps" && value >= 1 && value <= 60) options.fps = value;
        else if (key == "--quality" && value >= 1 && value <= 100) options.quality = static_cast<int>(value);
        else if (key == "--bitrate" && value >= 100 && value <= 100000) options.bitrate_kbps = value;
        else return false;
    }
    return true;
}
}
int HostMain(int argc, char** argv) {
    SessionOptions options;
    if (!ParseOptions(argc, argv, options)) {
        LOG_ERROR("Usage: padscreen-host [--port 51900] [--width 1280] [--height 800] [--fps 60] [--bitrate 10000] [--quality 70] [--show-cursor true|false] [--disable-hardware] [--verbose] [--show-stats]");
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
    std::unique_ptr<PipelineSession> session;
    while (!exit_requested.load()) {
        if (session && !session->Running()) session.reset();
        if (!Readable(server.Socket(), 200)) continue;
        std::string peer;
        UniqueSocket client(server.Accept(peer));
        if (client.Get() == INVALID_SOCKET) continue;
        if (session) { LOG_INFO("New client replaces previous connection"); session.reset(); }
        LOG_INFO("client connected: ", peer);
        session = std::make_unique<PipelineSession>(std::move(client), options);
        session->Run();
    }
    return 0;
}
}
int main(int argc, char** argv) {
    try { return pds::HostMain(argc, argv); }
    catch (const std::exception& error) { LOG_ERROR("Fatal: ", error.what()); return 1; }
}
