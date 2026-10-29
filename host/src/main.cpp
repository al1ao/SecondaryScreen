#include "net/pipeline_session.h"
#include "app/cli_options.h"
#include <atomic>
#include <memory>
#include <vector>
#include "display/monitor_enum.h"
#include "util/log.h"
namespace pds {
namespace {
std::atomic<bool> exit_requested{false};
BOOL WINAPI ConsoleControl(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT) return FALSE;
    exit_requested.store(true); return TRUE;
}
}
int HostMain(int argc, char** argv) {
    SessionOptions options;
    if (!ParseSessionOptions(argc, argv, options)) {
        LOG_ERROR("Usage: padscreen-host [--display friendly-name | --capture-primary] [--list-displays] [--auto-resize | --no-auto-resize] [--force-extend] [--wgc-throttle on|off] [--queue-depth 1|2] [--port 51900] [--width 1280] [--height 800] [--fps 1..165] [--max-fps 0..165] [--bitrate 10000] [--quality 70] [--show-cursor true|false] [--disable-hardware] [--verbose] [--show-stats]");
        return 2;
    }
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        LOG_HR(LastErrorHr(), "SetProcessDpiAwarenessContext"); return 1;
    }
    CheckExtendMode(options.force_extend);
    const auto monitors = EnumerateMonitors();
    const auto selected = SelectTargetMonitor(monitors, options.display_match, options.capture_primary);
    LogMonitors(monitors, selected ? selected->hmonitor : nullptr);
    if (options.list_displays) return 0;
    if (!selected) { LogMissingVirtualMonitor(); return 1; }
    if (!options.capture_primary) options.monitor_id = selected->monitor_device_id;
    LOG_INFO("Selected display: ", Utf8(selected->device_name), " (", Utf8(selected->friendly_name), ") ", selected->width, 'x', selected->height);
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
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args; args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) args.push_back(pds::Utf8(argv[i]));
        std::vector<char*> pointers; pointers.reserve(args.size());
        for (auto& arg : args) pointers.push_back(arg.data());
        return pds::HostMain(argc, pointers.data());
    }
    catch (const std::exception& error) { LOG_ERROR("Fatal: ", error.what()); return 1; }
}
