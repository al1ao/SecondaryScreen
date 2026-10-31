#pragma once
#include <windows.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace pds {
struct MonitorInfo {
    std::wstring device_name, friendly_name, monitor_device_id;
    HMONITOR hmonitor = nullptr;
    RECT rect{};
    uint32_t width = 0, height = 0, refresh_rate = 0;
    uint32_t orientation = 0;
    bool is_primary = false, is_virtual = false;
};
struct DisplayMode { uint32_t width = 0, height = 0, fps = 0; };
struct DisplaySignature {
    uint32_t width = 0, height = 0, refresh = 0, orientation = 0;
    bool operator==(const DisplaySignature&) const = default;
};
DisplaySignature SignatureOf(const MonitorInfo& monitor);
std::optional<DisplaySignature> ReadDisplaySignature(const MonitorInfo& monitor);
uint32_t AckRotation(uint32_t dmdo, bool wgc_path);
bool IsVirtualDisplayName(const std::wstring& name);
std::vector<MonitorInfo> EnumerateMonitors();
std::vector<DisplayMode> EnumerateDisplayModes(const std::wstring& device_name);
std::optional<MonitorInfo> SelectTargetMonitor(std::span<const MonitorInfo> monitors,
    const std::wstring& preferred_name, bool capture_primary, const std::wstring& stable_id = {});
HMONITOR FindTargetMonitor(const std::wstring& preferred_name, std::wstring* out_name);
void LogMonitors(std::span<const MonitorInfo> monitors, HMONITOR selected);
void LogMissingVirtualMonitor();
bool CheckExtendMode(bool force_extend = false);
bool SetDisplayResolution(const MonitorInfo& monitor, uint32_t width, uint32_t height, uint32_t refresh);
struct OutputResolution { uint32_t width = 0, height = 0; };
OutputResolution NegotiateResolution(uint32_t monitor_width, uint32_t monitor_height,
    uint32_t preferred_width, uint32_t preferred_height, uint32_t decoder_max_width,
    uint32_t decoder_max_height, uint32_t width_limit = 0, uint32_t height_limit = 0);
std::string Utf8(const std::wstring& text);
}
