#include "app/config.h"
#include "app/process.h"
#include "util/log.h"
#include <shlobj.h>
#include <fstream>
#include <nlohmann/json.hpp>
namespace pds {
namespace {
uint32_t Number(const nlohmann::json& value,uint32_t minimum,uint32_t maximum) {
    if(!value.is_number_integer() || value<minimum || value>maximum)throw std::runtime_error("invalid config integer");
    return value.get<uint32_t>();
}
}
std::filesystem::path ConfigPath() {
    PWSTR folder = nullptr;
    if (!LOG_HR(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &folder), "Roaming AppData")) return {};
    const auto path = std::filesystem::path(folder) / L"PadScreen" / L"config.json";
    CoTaskMemFree(folder); return path;
}
AppConfig LoadConfig(const std::filesystem::path& path) {
    AppConfig value;
    try {
        if (!std::filesystem::exists(path)) return value;
        if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("config exceeds 64KiB");
        const auto json = nlohmann::json::parse(std::ifstream(path));
        if (json.at("version") != 1) throw std::runtime_error("unknown config version");
        const auto& display = json.at("display"); const auto& connection = json.at("connection");
        value.monitor_id = display.value("monitorId", std::string{});
        value.primary = display.value("primary", false); value.auto_resize = display.value("autoResize", false);
        value.width = Number(display.at("width"),2,16384); value.height = Number(display.at("height"),2,16384); value.fps = Number(display.at("fps"),1,60);
        value.bitrate_kbps = Number(json.at("encoder").at("bitrateKbps"),100,100000);
        const auto port = Number(connection.at("port"),1,65535);
        if (port == 0 || port > 65535 || value.width < 2 || value.width > 16384 || value.width % 2 ||
            value.height < 2 || value.height > 16384 || value.height % 2 || value.fps < 1 || value.fps > 60 ||
            value.bitrate_kbps < 100 || value.bitrate_kbps > 100000 || (value.primary && value.auto_resize)) throw std::runtime_error("invalid config range");
        value.port = static_cast<uint16_t>(port);
        value.device_serial = connection.value("deviceSerial", std::string{}); value.auto_reverse = connection.value("autoReverse", true);
        value.foreground = connection.value("foreground", false); value.auto_start = json.at("app").value("autoStart", false);
        if (value.monitor_id.size() > 4096 || value.device_serial.size() > 256) throw std::runtime_error("invalid config string");
        return value;
    } catch (const std::exception& error) { LOG_WARN("Config invalid; using defaults: ", error.what()); return {}; }
}
bool SaveConfig(const std::filesystem::path& path, const AppConfig& config) {
    try {
        const nlohmann::json json{{"version",1}, {"display",{{"monitorId",config.monitor_id},{"primary",config.primary},{"width",config.width},{"height",config.height},{"fps",config.fps},{"autoResize",config.auto_resize}}},
            {"encoder",{{"backend","auto"},{"bitrateKbps",config.bitrate_kbps}}},
            {"connection",{{"port",config.port},{"deviceSerial",config.device_serial},{"autoReverse",config.auto_reverse},{"foreground",config.foreground}}}, {"app",{{"autoStart",config.auto_start}}}};
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path; temporary += L".tmp";
        const auto text = json.dump(2) + "\n";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { LOG_HR(LastErrorHr(), "Create config tmp"); return false; }
        DWORD written = 0;
        const bool saved = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() && FlushFileBuffers(file);
        if (!saved) LOG_HR(LastErrorHr(), "Write/flush config");
        CloseHandle(file);
        if (saved && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        if (saved) LOG_HR(LastErrorHr(), "Atomic config replace");
        DeleteFileW(temporary.c_str()); return false;
    } catch (const std::exception& error) { LOG_ERROR("Save config: ", error.what()); return false; }
}
bool AutoStartEnabled() {
    std::wstring command(32768,L'\0');DWORD bytes=static_cast<DWORD>(command.size()*sizeof(wchar_t));
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"PadScreen",RRF_RT_REG_SZ,nullptr,command.data(),&bytes)!=ERROR_SUCCESS)return false;
    command.resize(bytes/sizeof(wchar_t));while(!command.empty()&&command.back()==L'\0')command.pop_back();
    return command==QuoteArgument((ExecutableDirectory()/L"PadScreen.exe").wstring());
}
bool SetAutoStart(bool enabled) {
    HKEY key = nullptr;
    const auto opened = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) return LOG_HR(HRESULT_FROM_WIN32(opened), "Open user startup key");
    const auto command = QuoteArgument((ExecutableDirectory() / L"PadScreen.exe").wstring());
    const auto status = enabled ? RegSetValueExW(key, L"PadScreen", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size()+1)*sizeof(wchar_t))) : RegDeleteValueW(key,L"PadScreen");
    RegCloseKey(key);
    return status == ERROR_SUCCESS || (!enabled && status == ERROR_FILE_NOT_FOUND) || LOG_HR(HRESULT_FROM_WIN32(status), "Update user startup");
}
SessionOptions SessionFromConfig(const AppConfig& config) {
    SessionOptions options;
    options.width=config.width; options.height=config.height; options.width_explicit=true; options.height_explicit=true;
    options.fps=config.fps; options.bitrate_kbps=config.bitrate_kbps; options.port=config.port;
    options.capture_primary=config.primary; options.auto_resize=config.auto_resize; options.show_stats=true;
    if (!config.primary && !config.monitor_id.empty()) {
        const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,config.monitor_id.data(),static_cast<int>(config.monitor_id.size()),nullptr,0);
        options.monitor_id.resize(static_cast<size_t>((std::max)(count,0)));
        if(count) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,config.monitor_id.data(),static_cast<int>(config.monitor_id.size()),options.monitor_id.data(),count);
    }
    return options;
}
}
