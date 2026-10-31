#include "display/adaptive_mode.h"
#include "util/log.h"
#include <algorithm>
#include <cmath>
namespace pds {
namespace {
constexpr uint32_t kMaxFps = 165;
// 16:10 (1.60) and 16:9 (1.78) differ by ~11%; 2% keeps real aspect
// variants together without ever treating a 16:9 mode as a 16:10 fit.
constexpr double kRatioTolerance = 0.02;
uint32_t Dimension(uint32_t value, uint32_t fallback) {
    return (value >= 2 && value <= 16384 ? value : fallback) & ~1u;
}
uint32_t PickRefresh(std::span<const VideoMode> available, uint32_t width, uint32_t height, uint32_t requested, uint32_t floor) {
    uint32_t exact = 0, below = 0, above = 0;
    for (const auto& mode : available) {
        if (mode.width != width || mode.height != height || mode.fps < 2 || mode.fps > kMaxFps || mode.fps < floor) continue;
        if (mode.fps == requested) exact = mode.fps;
        else if (mode.fps < requested && mode.fps > below) below = mode.fps;
        else if (mode.fps > requested && (!above || mode.fps < above)) above = mode.fps;
    }
    return exact ? exact : below ? below : above;
}
}
std::vector<VideoMode> RankSameRatioModes(std::span<const VideoMode> available, const VideoMode& requested) {
    std::vector<VideoMode> ranked;
    if (requested.width < 2 || requested.height < 2 || available.empty()) return ranked;
    const double wanted = static_cast<double>(requested.width) / requested.height;
    const uint64_t requested_area = static_cast<uint64_t>(requested.width) * requested.height;
    // A native size stuck at a slideshow rate is worse than one step smaller at
    // 60Hz+, so sizes that cannot reach this floor rank after those that can.
    const uint32_t smooth_floor = (std::min)(60u, requested.fps);
    struct Size { uint32_t width, height; uint64_t area; bool fits; };
    std::vector<Size> sizes;
    for (const auto& mode : available) {
        if (mode.width < 2 || mode.height < 2 || mode.fps < 2 || mode.fps > kMaxFps) continue;
        const double ratio = static_cast<double>(mode.width) / mode.height;
        if (std::abs(ratio / wanted - 1.0) > kRatioTolerance) continue;
        const bool known = std::any_of(sizes.begin(), sizes.end(), [&](const Size& size) {
            return size.width == mode.width && size.height == mode.height;
        });
        if (known) continue;
        const uint64_t area = static_cast<uint64_t>(mode.width) * mode.height;
        sizes.push_back({mode.width, mode.height, area, area <= requested_area});
    }
    struct Ranked { VideoMode mode; uint64_t area; bool fits, smooth; };
    std::vector<Ranked> candidates;
    for (const auto& size : sizes) {
        const bool smooth = std::any_of(available.begin(), available.end(), [&](const VideoMode& mode) {
            return mode.width == size.width && mode.height == size.height && mode.fps >= smooth_floor && mode.fps <= kMaxFps;
        });
        const uint32_t fps = PickRefresh(available, size.width, size.height, requested.fps, smooth ? smooth_floor : 0);
        if (fps) candidates.push_back({{size.width, size.height, fps}, size.area, size.fits, smooth});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Ranked& first, const Ranked& second) {
        if (first.smooth != second.smooth) return first.smooth && !second.smooth;
        if (first.fits != second.fits) return first.fits && !second.fits;
        // The tablet's native size is the largest fit and therefore first.
        // Below it, prefer the largest mode (least upscaling); above it, the
        // smallest (least downscale).
        return first.fits ? first.area > second.area : first.area < second.area;
    });
    for (const auto& candidate : candidates) ranked.push_back(candidate.mode);
    return ranked;
}
std::optional<VideoMode> ChooseSameRatioMode(std::span<const VideoMode> available, const VideoMode& requested) {
    const auto ranked = RankSameRatioModes(available, requested);
    if (ranked.empty()) return std::nullopt;
    return ranked.front();
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
VideoMode AdaptDisplayMode(VideoMode current, VideoMode requested, bool enabled, const ModeSetter& setter, const ModeLister& list_modes) {
    if (!enabled) { current.fps = requested.fps; return current; }
    std::vector<VideoMode> attempted;
    auto try_apply = [&](const VideoMode& mode) -> std::optional<VideoMode> {
        const bool duplicate = std::any_of(attempted.begin(), attempted.end(), [&](const VideoMode& seen) {
            return seen.width == mode.width && seen.height == mode.height && seen.fps == mode.fps;
        });
        if (duplicate) return std::nullopt;
        attempted.push_back(mode);
        LOG_INFO("尝试调整虚拟屏分辨率: ", mode.width, 'x', mode.height, " @", mode.fps, "Hz");
        auto actual = setter(mode);
        if (!actual || actual->width < 2 || actual->height < 2) return std::nullopt;
        // TODO(design): ACK describes the configured encoding rate. A display can
        // run faster than this when a low user cap has no matching driver mode.
        actual->fps = (std::min)(requested.fps, actual->fps ? actual->fps : mode.fps);
        return actual;
    };
    if (auto mode = try_apply(requested)) return *mode;
    // The exact size at the requested rate is unavailable. The tablet letterboxes
    // any stream whose aspect differs from its own, so prefer another driver mode
    // with the same aspect — including a valid rate at the native size — over
    // silently keeping a 16:9 mode on a 16:10 panel.
    bool enumerated = false;
    if (list_modes) {
        const auto available = list_modes();
        enumerated = !available.empty();
        const auto candidates = RankSameRatioModes(available, requested);
        if (enumerated && candidates.empty()) {
            LOG_WARN("虚拟屏没有与平板同比例（", requested.width, 'x', requested.height,
                "）的可用模式，画面可能带黑边。请以管理员身份运行 scripts/update-vdd-modes.ps1 更新 VDD 配置后重试。");
        }
        unsigned attempts = 0;
        for (const auto& candidate : candidates) {
            if (attempts >= 3) break;
            if (auto mode = try_apply(candidate)) return *mode;
            ++attempts;
        }
    }
    if (requested.fps != 60) {
        LOG_WARN("刷新率 ", requested.fps, "Hz 不可用，尝试 60Hz");
        auto fallback = requested; fallback.fps = 60;
        if (auto mode = try_apply(fallback)) return *mode;
    }
    LOG_WARN("分辨率 ", requested.width, 'x', requested.height, " 不可用，保持当前 ", current.width, 'x', current.height);
    current.fps = (std::min)(requested.fps, current.fps ? current.fps : 60u);
    return current;
}
}
