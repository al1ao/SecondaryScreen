#pragma once
#include "net/pipeline_session.h"
#include <functional>
#include <optional>
namespace pds {
struct VideoMode { uint32_t width = 0, height = 0, fps = 60; };
VideoMode ResolveVideoRequest(const SessionOptions& options, const Hello& hello);
// The setter returns the mode read back from the same monitor after applying it.
using ModeSetter = std::function<std::optional<VideoMode>(const VideoMode&)>;
VideoMode AdaptDisplayMode(VideoMode current, VideoMode requested, bool enabled, const ModeSetter& setter);
}
