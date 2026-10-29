#include "display/adaptive_mode.h"
#include "util/log.h"
#include <algorithm>
namespace pds {
namespace {
uint32_t Dimension(uint32_t value, uint32_t fallback) {
    return (value >= 2 && value <= 16384 ? value : fallback) & ~1u;
}
}
VideoMode ResolveVideoRequest(const SessionOptions& options, const Hello& hello) {
    VideoMode result{
        Dimension(options.width_explicit ? options.width : hello.preferred_width, Dimension(options.width, 1280)),
        Dimension(options.height_explicit ? options.height : hello.preferred_height, Dimension(options.height, 800)),
        options.fps ? options.fps : hello.preferred_fps};
    if (result.fps == 0 || result.fps > 165) result.fps = 60;
    if (options.max_fps) result.fps = (std::min)(result.fps, options.max_fps);
    // Decoder limits are safety bounds, including for explicit user dimensions.
    double scale = 1.0;
    if (hello.decoder_max_width) scale = (std::min)(scale, static_cast<double>(hello.decoder_max_width) / result.width);
    if (hello.decoder_max_height) scale = (std::min)(scale, static_cast<double>(hello.decoder_max_height) / result.height);
    result.width = static_cast<uint32_t>(result.width * scale) & ~1u;
    result.height = static_cast<uint32_t>(result.height * scale) & ~1u;
    return result;
}
VideoMode AdaptDisplayMode(VideoMode current, VideoMode requested, bool enabled, const ModeSetter& setter) {
    if (!enabled) { current.fps = requested.fps; return current; }
    auto apply = [&](VideoMode mode) -> std::optional<VideoMode> {
        LOG_INFO("尝试调整虚拟屏分辨率: ", mode.width, 'x', mode.height, " @", mode.fps, "Hz");
        auto actual = setter(mode);
        if (!actual || actual->width < 2 || actual->height < 2) return std::nullopt;
        // TODO(design): ACK describes the configured encoding rate. A display can
        // run faster than this when a low user cap has no matching driver mode.
        actual->fps = (std::min)(requested.fps, actual->fps ? actual->fps : mode.fps);
        return actual;
    };
    if (auto mode = apply(requested)) return *mode;
    if (requested.fps != 60) {
        LOG_WARN("刷新率 ", requested.fps, "Hz 不可用，尝试 60Hz");
        auto fallback = requested; fallback.fps = 60;
        if (auto mode = apply(fallback)) return *mode;
    }
    LOG_WARN("分辨率 ", requested.width, 'x', requested.height, " 不可用，保持当前 ", current.width, 'x', current.height);
    current.fps = (std::min)(requested.fps, current.fps ? current.fps : 60u);
    return current;
}
}
