#include "app/cli_options.h"
#include <charconv>
#include <string_view>
#include "util/log.h"
namespace pds {
bool ParseSessionOptions(int argc, char** argv, SessionOptions& options) {
    bool explicit_resize = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--verbose") { g_log_level.store(LogLevel::Debug); continue; }
        if (key == "--show-stats") { options.show_stats = true; continue; }
        if (key == "--disable-hardware") { options.disable_hardware = true; continue; }
        if (key == "--capture-primary") { options.capture_primary = true; continue; }
        if (key == "--auto-resize") { options.auto_resize = true; explicit_resize = true; continue; }
        if (key == "--no-auto-resize") { options.auto_resize = false; explicit_resize = false; continue; }
        if (key == "--force-extend") { options.force_extend = true; continue; }
        if (key == "--list-displays") { options.list_displays = true; continue; }
        if (key == "--show-cursor") {
            options.show_cursor = true;
            if (i + 1 < argc && (std::string_view(argv[i + 1]) == "false" || std::string_view(argv[i + 1]) == "0")) {
                options.show_cursor = false; ++i;
            } else if (i + 1 < argc && (std::string_view(argv[i + 1]) == "true" || std::string_view(argv[i + 1]) == "1")) ++i;
            continue;
        }
        if (i + 1 >= argc) return false;
        const std::string_view text(argv[++i]);
        if (key == "--wgc-throttle") {
            if (text != "on" && text != "off") return false;
            options.wgc_throttle = text == "on"; continue;
        }
        if (key == "--display") {
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (size <= 0) return false;
            options.display_match.resize(static_cast<size_t>(size));
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), options.display_match.data(), size)) return false;
            continue;
        }
        uint32_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) return false;
        // TODO(design): keep simple bounded CLI values; reject instead of silently defaulting.
        if (key == "--port" && value > 0 && value <= 65535) options.port = static_cast<uint16_t>(value);
        else if (key == "--width" && value >= 2 && value <= 16384) { options.width = value & ~1u; options.width_explicit = true; }
        else if (key == "--height" && value >= 2 && value <= 16384) { options.height = value & ~1u; options.height_explicit = true; }
        else if (key == "--fps" && value >= 1 && value <= 165) options.fps = value;
        else if (key == "--max-fps" && value <= 165) options.max_fps = value;
        else if (key == "--quality" && value >= 1 && value <= 100) options.quality = static_cast<int>(value);
        else if (key == "--bitrate" && value >= 100 && value <= 100000) options.bitrate_kbps = value;
        else if (key == "--queue-depth" && (value == 1 || value == 2)) options.queue_depth = value;
        else return false;
    }
    if (options.capture_primary && !explicit_resize) options.auto_resize = false;
    return !(options.capture_primary && (!options.display_match.empty() || options.auto_resize)) &&
        !(options.list_displays && options.force_extend);
}
}
