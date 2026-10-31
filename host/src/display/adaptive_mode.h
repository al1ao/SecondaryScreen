#pragma once
#include "net/pipeline_session.h"
#include <functional>
#include <optional>
#include <span>
#include <vector>
namespace pds {
struct VideoMode { uint32_t width = 0, height = 0, fps = 60; };
VideoMode ResolveVideoRequest(const SessionOptions& options, const Hello& hello);
// Candidate modes with the request's aspect ratio, best first: the exact size,
// then the largest same-ratio mode not exceeding the requested size, then the
// smallest larger one. Sizes unable to reach min(60, requested) Hz rank after
// those that can. Within a size, prefer the requested rate, else the fastest
// rate not above it, else the slowest.
std::vector<VideoMode> RankSameRatioModes(std::span<const VideoMode> available, const VideoMode& requested);
// The first ranked candidate, or null when no ratio match exists.
std::optional<VideoMode> ChooseSameRatioMode(std::span<const VideoMode> available, const VideoMode& requested);
// The setter returns the mode read back from the same monitor after applying it.
using ModeSetter = std::function<std::optional<VideoMode>(const VideoMode&)>;
// Enumerates the modes the driver currently advertises for the target display.
using ModeLister = std::function<std::vector<VideoMode>()>;
VideoMode AdaptDisplayMode(VideoMode current, VideoMode requested, bool enabled, const ModeSetter& setter,
    const ModeLister& list_modes = {});
}
