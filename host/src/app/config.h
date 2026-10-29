#pragma once
#include "net/pipeline_session.h"
#include <filesystem>
namespace pds {
struct AppConfig {
    std::string monitor_id, device_serial;
    uint32_t width = 1280, height = 800, fps = 0, max_fps = 0, bitrate_kbps = 10000;
    uint16_t port = kDefaultPort;
    bool primary = false, automatic_size = true, auto_resize = true, auto_reverse = true, auto_start = false, foreground = false;
    bool operator==(const AppConfig&) const = default;
};
std::filesystem::path ConfigPath();
AppConfig LoadConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const AppConfig& config);
bool SetAutoStart(bool enabled);
bool AutoStartEnabled();
SessionOptions SessionFromConfig(const AppConfig& config);
}
