#include "display/monitor_enum.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include "util/log.h"
namespace pds {
namespace {
std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return text;
}
bool ActivePaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        UINT32 path_count = 0, mode_count = 0;
        auto result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (result != ERROR_SUCCESS) return LOG_HR(HRESULT_FROM_WIN32(result), "GetDisplayConfigBufferSizes");
        paths.resize(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        // QDC_ONLY_ACTIVE_PATHS requires a null topology-id pointer. Derive the
        // live topology from source IDs instead of passing the invalid example.
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) continue;
        if (result != ERROR_SUCCESS) return LOG_HR(HRESULT_FROM_WIN32(result), "QueryDisplayConfig active paths");
        paths.resize(path_count); return true;
    }
    LOG_WARN("Display configuration changed repeatedly; retry after the desktop settles");
    return false;
}
struct Enumeration {
    std::vector<MonitorInfo> monitors;
    const std::vector<DISPLAYCONFIG_PATH_INFO>* paths;
    bool failed = false;
};
BOOL CALLBACK EnumMonitor(HMONITOR handle, HDC, LPRECT, LPARAM parameter) {
    auto& state = *reinterpret_cast<Enumeration*>(parameter);
    try {
        MONITORINFOEXW native{}; native.cbSize = sizeof(native);
        if (!GetMonitorInfoW(handle, &native)) { LOG_HR(LastErrorHr(), "GetMonitorInfo enumeration"); state.failed = true; return FALSE; }
        MonitorInfo monitor;
        monitor.hmonitor = handle; monitor.device_name = native.szDevice;
        monitor.rect = native.rcMonitor; monitor.is_primary = (native.dwFlags & MONITORINFOF_PRIMARY) != 0;
        monitor.width = static_cast<uint32_t>(native.rcMonitor.right - native.rcMonitor.left);
        monitor.height = static_cast<uint32_t>(native.rcMonitor.bottom - native.rcMonitor.top);
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(native.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            if (mode.dmDisplayFrequency > 1) monitor.refresh_rate = mode.dmDisplayFrequency;
            if (mode.dmFields & DM_DISPLAYORIENTATION) monitor.orientation = mode.dmDisplayOrientation;
            LOG_DEBUG("Display mode ", Utf8(monitor.device_name), " DEVMODE=", mode.dmPelsWidth, 'x',
                mode.dmPelsHeight, " orientation=", monitor.orientation, " fields=", mode.dmFields,
                " rect=", monitor.rect.left, ',', monitor.rect.top, ',', monitor.rect.right, ',', monitor.rect.bottom);
        }
        DISPLAY_DEVICEW device{}; device.cb = sizeof(device);
        if (EnumDisplayDevicesW(native.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
            monitor.friendly_name = device.DeviceString; monitor.monitor_device_id = device.DeviceID;
        }
        for (const auto& path : *state.paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId, path.sourceInfo.id};
            const auto source_result = DisplayConfigGetDeviceInfo(&source.header);
            if (source_result != ERROR_SUCCESS) { LOG_HR(HRESULT_FROM_WIN32(source_result), "DisplayConfig source name"); continue; }
            if (monitor.device_name != source.viewGdiDeviceName) continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), path.targetInfo.adapterId, path.targetInfo.id};
            const auto result = DisplayConfigGetDeviceInfo(&target.header);
            if (result != ERROR_SUCCESS) { LOG_HR(HRESULT_FROM_WIN32(result), "DisplayConfig target name"); continue; }
            if (target.monitorFriendlyDeviceName[0]) monitor.friendly_name = target.monitorFriendlyDeviceName;
            if (target.monitorDevicePath[0]) monitor.monitor_device_id = target.monitorDevicePath;
        }
        // A driver may publish a generic EDID monitor name. Include the matching
        // adapter's name, so VDD identification does not depend on DISPLAY numbers.
        for (DWORD index = 0;; ++index) {
            DISPLAY_DEVICEW adapter{}; adapter.cb = sizeof(adapter);
            if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) break;
            if (monitor.device_name == adapter.DeviceName && IsVirtualDisplayName(adapter.DeviceString)) {
                monitor.friendly_name = std::wstring(adapter.DeviceString) + L" / " + monitor.friendly_name;
                break;
            }
        }
        monitor.is_virtual = IsVirtualDisplayName(monitor.friendly_name);
        state.monitors.push_back(std::move(monitor));
        return TRUE;
    } catch (const std::exception& error) {
        LOG_ERROR("Monitor enumeration: ", error.what()); state.failed = true; return FALSE;
    }
}
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const auto length = static_cast<int>(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (!bytes) { LOG_HR(LastErrorHr(), "monitor name UTF8 size"); return {}; }
    std::string result(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), bytes, nullptr, nullptr)) {
        LOG_HR(LastErrorHr(), "monitor name UTF8"); return {};
    }
    return result;
}
bool IsVirtualDisplayName(const std::wstring& name) {
    const auto lower = Lower(name);
    for (const auto* keyword : {L"virtual display", L"vdd", L"indirect display", L"virtual monitor"})
        if (lower.find(keyword) != std::wstring::npos) return true;
    return false;
}
std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    if (!ActivePaths(paths)) LOG_WARN("DisplayConfig unavailable; falling back to EnumDisplayDevices monitor names");
    Enumeration state{{}, &paths};
    if (!EnumDisplayMonitors(nullptr, nullptr, EnumMonitor, reinterpret_cast<LPARAM>(&state))) {
        if (!state.failed) LOG_HR(LastErrorHr(), "EnumDisplayMonitors");
        return {};
    }
    return state.monitors;
}
DisplaySignature SignatureOf(const MonitorInfo& monitor) {
    return {monitor.width, monitor.height, monitor.refresh_rate, monitor.orientation};
}
std::optional<DisplaySignature> ReadDisplaySignature(const MonitorInfo& monitor) {
    MONITORINFOEXW native{}; native.cbSize = sizeof(native);
    if (!GetMonitorInfoW(monitor.hmonitor, &native) || monitor.device_name != native.szDevice) return std::nullopt;
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(native.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return std::nullopt;
    return DisplaySignature{
        static_cast<uint32_t>(native.rcMonitor.right - native.rcMonitor.left),
        static_cast<uint32_t>(native.rcMonitor.bottom - native.rcMonitor.top),
        mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0,
        (mode.dmFields & DM_DISPLAYORIENTATION) ? mode.dmDisplayOrientation : 0
    };
}
uint32_t AckRotation(uint32_t dmdo, bool wgc_path) {
    // Both capture paths are assumed to deliver the composed desktop until a
    // four-direction diagnostic on the target VDD proves otherwise. Keep the
    // path-specific table here so measured corrections are isolated.
    constexpr uint32_t wgc[] = {0, 0, 0, 0};
    constexpr uint32_t gdi[] = {0, 0, 0, 0};
    return (wgc_path ? wgc : gdi)[dmdo & 3u];
}
std::vector<DisplayMode> EnumerateDisplayModes(const std::wstring& device_name) {
    std::vector<DisplayMode> modes;
    if (device_name.empty()) return modes;
    for (DWORD index = 0;; ++index) {
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(device_name.c_str(), index, &mode, 0)) break;
        if (mode.dmPelsWidth < 2 || mode.dmPelsHeight < 2 || mode.dmPelsWidth > 16384 || mode.dmPelsHeight > 16384) continue;
        if (mode.dmDisplayFrequency < 2 || mode.dmDisplayFrequency > 1000) continue;
        const bool duplicate = std::any_of(modes.begin(), modes.end(), [&](const DisplayMode& known) {
            return known.width == mode.dmPelsWidth && known.height == mode.dmPelsHeight && known.fps == mode.dmDisplayFrequency;
        });
        if (!duplicate) modes.push_back({mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency});
    }
    return modes;
}
std::optional<MonitorInfo> SelectTargetMonitor(std::span<const MonitorInfo> monitors,
    const std::wstring& preferred_name, bool capture_primary, const std::wstring& stable_id) {
    const auto preferred = Lower(preferred_name);
    std::optional<MonitorInfo> selected;
    for (const auto& monitor : monitors) {
        // TODO(design): a remote/virtual adapter may back the existing primary desktop. It is
        // not an extension screen. Primary capture requires --capture-primary.
        if (!capture_primary && monitor.is_primary) continue;
        const bool matches = capture_primary ? monitor.is_primary : !stable_id.empty()
            ? Lower(monitor.monitor_device_id) == Lower(stable_id) : preferred.empty()
            ? monitor.is_virtual : Lower(monitor.friendly_name).find(preferred) != std::wstring::npos;
        if (!matches) continue;
        if (!selected) selected = monitor;
        else LOG_WARN("Multiple displays match; choosing the first. Use --display with a more specific friendly name");
    }
    return selected;
}
HMONITOR FindTargetMonitor(const std::wstring& preferred_name, std::wstring* out_name) {
    const auto monitors = EnumerateMonitors();
    const auto selected = SelectTargetMonitor(monitors, preferred_name, false);
    if (!selected) return nullptr;
    if (out_name) *out_name = selected->friendly_name;
    return selected->hmonitor;
}
void LogMonitors(std::span<const MonitorInfo> monitors, HMONITOR selected) {
    LOG_INFO("Enumerated ", monitors.size(), " active monitors:");
    for (size_t index = 0; index < monitors.size(); ++index) {
        const auto& monitor = monitors[index];
        LOG_INFO("  [", index, "] ", Utf8(monitor.device_name), " \"", Utf8(monitor.friendly_name), "\" ",
            monitor.width, 'x', monitor.height, " @", monitor.refresh_rate, "Hz (", monitor.rect.left, ',', monitor.rect.top, ')',
            " orientation=", monitor.orientation,
            monitor.is_primary ? " PRIMARY" : "", monitor.hmonitor == selected ? " <-- selected" : "");
    }
}
void LogMissingVirtualMonitor() {
    LOG_ERROR("未找到匹配的活动虚拟显示器。可使用 --capture-primary 对比调试。");
    size_t inactive = 0;
    std::wstring inactive_name;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW adapter{}; adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) break;
        if (IsVirtualDisplayName(adapter.DeviceString) && (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
            ++inactive;
            if (inactive_name.empty()) inactive_name = adapter.DeviceString;
        }
    }
    if (inactive) LOG_WARN("发现 ", inactive, " 个未连接到桌面的虚拟适配器条目（示例：", Utf8(inactive_name), "）；请检查显示设置与设备管理器，启用目标虚拟屏。");
    LOG_ERROR("1. VDD 未安装：以管理员身份运行 host\\scripts\\install-vdd.ps1。");
    LOG_ERROR("2. 显示器未启用：打开 设置 > 系统 > 显示，启用 Virtual Display Driver 并选择扩展这些显示器。");
    LOG_ERROR("3. 驱动未正常启动：在设备管理器 > 显示适配器 检查 Virtual Display Driver 状态；查看其驱动日志并按提示重启。");
}
bool CheckExtendMode(bool force_extend) {
    if (force_extend) {
        const auto result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
        if (result != ERROR_SUCCESS) LOG_HR(HRESULT_FROM_WIN32(result), "SetDisplayConfig --force-extend");
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    if (!ActivePaths(paths)) { LOG_WARN("Cannot verify display topology in this session"); return false; }
    bool extended = paths.size() >= 2;
    for (size_t i = 0; i < paths.size(); ++i) for (size_t j = 0; j < i; ++j) {
        const auto& a = paths[i].sourceInfo; const auto& b = paths[j].sourceInfo;
        if (a.id == b.id && a.adapterId.HighPart == b.adapterId.HighPart && a.adapterId.LowPart == b.adapterId.LowPart) extended = false;
    }
    if (extended) LOG_INFO("Display topology: extended desktop");
    else LOG_WARN("当前显示模式不是独立扩展桌面。请打开 设置 > 系统 > 显示，选择扩展这些显示器，并启用虚拟显示器。只有显式 --force-extend 才会尝试自动修改拓扑。");
    return extended;
}
bool SetDisplayResolution(const MonitorInfo& monitor, uint32_t width, uint32_t height, uint32_t refresh) {
    if (!monitor.is_virtual || monitor.is_primary || width < 2 || height < 2 || width > 16384 || height > 16384 || refresh == 0) {
        LOG_WARN("--auto-resize only changes a non-primary virtual display, with valid dimensions"); return false;
    }
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = width; mode.dmPelsHeight = height; mode.dmDisplayFrequency = refresh;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    // Only during HELLO, never per frame; do not persistently rewrite the registry.
    const auto result = ChangeDisplaySettingsExW(monitor.device_name.c_str(), &mode, nullptr, 0, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) { LOG_WARN("Virtual display resize failed, DISP_CHANGE=", result, "; retaining current resolution. Add the requested mode to VDD settings first."); return false; }
    return true;
}
OutputResolution NegotiateResolution(uint32_t width, uint32_t height, uint32_t preferred_width, uint32_t preferred_height,
    uint32_t decoder_max_width, uint32_t decoder_max_height, uint32_t width_limit, uint32_t height_limit) {
    if (width < 2 || height < 2) return {};
    double scale = 1.0;
    auto limit = [&](uint32_t bound, uint32_t size) { if (bound) scale = (std::min)(scale, static_cast<double>(bound) / size); };
    limit(16384, width); limit(16384, height);
    limit(decoder_max_width, width); limit(decoder_max_height, height);
    limit(width_limit, width); limit(height_limit, height);
    if (preferred_width && preferred_height) {
        const double ratio_error = std::abs((static_cast<double>(width) / height) / (static_cast<double>(preferred_width) / preferred_height) - 1.0);
        if (ratio_error >= 0.05) { limit(preferred_width, width); limit(preferred_height, height); }
    }
    return {static_cast<uint32_t>(width * scale) & ~1u, static_cast<uint32_t>(height * scale) & ~1u};
}
}
