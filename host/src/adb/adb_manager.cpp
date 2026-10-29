#include "adb/adb_manager.h"
#include "util/log.h"
#include <sstream>
namespace pds {
namespace {
std::wstring Serial(const std::string& value) { return std::wstring(value.begin(),value.end()); }
std::wstring Endpoint(uint16_t port) { return L"tcp:"+std::to_wstring(port); }
}
std::vector<AdbDevice> ParseAdbDevices(const std::string& text) {
    std::vector<AdbDevice> result;
    std::istringstream lines(text); std::string line;
    while(std::getline(lines,line)) {
        if(line.empty() || line.starts_with("List of devices") || line.starts_with('*')) continue;
        std::istringstream words(line); AdbDevice device;
        if(!(words>>device.serial>>device.state)) continue;
        if(device.state!="device" && device.state!="unauthorized" && device.state!="offline" && device.state!="no") continue;
        std::string word;
        while(words>>word) if(word.starts_with("model:")) device.model=word.substr(6);
        result.push_back(std::move(device));
    }
    return result;
}
bool HasReverse(const std::string& text,uint16_t port) {
    std::istringstream lines(text); std::string line;
    const auto endpoint="tcp:"+std::to_string(port);
    while(std::getline(lines,line)) {
        std::istringstream words(line); std::string transport, remote, local;
        if(words>>transport>>remote>>local && remote==endpoint && local==endpoint) return true;
    }
    return false;
}
std::optional<std::filesystem::path> FindAdb(const std::filesystem::path& directory) {
    std::error_code error;
    const auto local=directory/L"platform-tools"/L"adb.exe";
    if(std::filesystem::is_regular_file(local,error)) return local;
    std::wstring search(32768,L'\0');
    const auto search_size=GetEnvironmentVariableW(L"PATH",search.data(),static_cast<DWORD>(search.size()));
    if(!search_size || search_size>=search.size())return std::nullopt;
    search.resize(search_size);
    std::wstring path(32768,L'\0');
    const auto count=SearchPathW(search.c_str(),L"adb.exe",nullptr,static_cast<DWORD>(path.size()),path.data(),nullptr);
    if(count && count<path.size()) {path.resize(count); return std::filesystem::path(path);}
    return std::nullopt;
}
AdbManager::AdbManager(std::filesystem::path executable,Runner runner):executable_(std::move(executable)),runner_(std::move(runner)) {}
AdbManager::~AdbManager(){Reset();}
ProcessResult AdbManager::Run(const std::vector<std::wstring>& args) {
    auto result=runner_?runner_(args):RunProcess(executable_,args);
    if(result.exit_code!=0 || result.timed_out) LOG_WARN("adb command failed: exit=",result.exit_code," timeout=",result.timed_out," ",result.output);
    return result;
}
void AdbManager::Reset() {
    if(owns_mapping_ && !serial_.empty()) Run({L"-s",Serial(serial_),L"reverse",L"--remove",Endpoint(port_)});
    owns_mapping_=false; launched_=false; serial_.clear(); port_=0;
}
AdbStatus AdbManager::Poll(uint16_t port,const std::string& preferred,bool auto_reverse,bool foreground) {
    AdbStatus status;
    if(!server_started_) {
        const auto start=Run({L"start-server"});
        if(start.exit_code!=0 || start.timed_out) {status.error=true;status.message="adb 启动失败，请查看日志";return status;}
        server_started_=true;
    }
    const auto listing=Run({L"devices",L"-l"});
    if(listing.exit_code!=0 || listing.timed_out) {status.error=true;status.message="无法读取 USB 设备，请检查 adb";server_started_=false;return status;}
    status.devices=ParseAdbDevices(listing.output);
    const AdbDevice* selected=nullptr;
    if(!preferred.empty()) {
        for(const auto& device:status.devices) if(device.serial==preferred) selected=&device;
    } else if(status.devices.size()==1) selected=&status.devices.front();
    if(!selected) {
        if(!serial_.empty()) {Reset(); status.changed=true;}
        status.message=status.devices.size()>1?"检测到多个设备，请在设置中选择":"请连接平板并开启 USB 调试";
        return status;
    }
    status.serial=selected->serial;
    if(selected->state!="device") {
        if(!serial_.empty()) {Reset();status.changed=true;}
        status.message=selected->state=="unauthorized"?"请在平板上点允许":"设备离线，请检查 USB 连接";return status;
    }
    if(serial_!=selected->serial || port_!=port) {Reset();serial_=selected->serial;port_=port;status.changed=true;}
    if(auto_reverse) {
        const auto mappings=Run({L"-s",Serial(serial_),L"reverse",L"--list"});
        if(mappings.exit_code!=0 || mappings.timed_out) {status.error=true;status.message="读取 USB 端口映射失败";return status;}
        if(!HasReverse(mappings.output,port)) {
            const auto reverse=Run({L"-s",Serial(serial_),L"reverse",Endpoint(port),Endpoint(port)});
            if(reverse.exit_code!=0 || reverse.timed_out) {status.error=true;status.message="配置 USB 连接失败，请查看日志";return status;}
            owns_mapping_=true;launched_=false;status.changed=true;
        }
    }
    if(!launched_) {
        // TODO(design): launch the installed Activity once per USB/mapping recovery.
        // Missing APK is reported rather than installing an unrequested package.
        const auto launch=Run({L"-s",Serial(serial_),L"shell",L"am",L"start",L"-n",L"com.secondaryscreen.client/.MainActivity",
            L"-f",L"0x10008000",
            L"--ei",L"port",std::to_wstring(port),L"--ez",L"foreground",foreground?L"true":L"false"});
        if(launch.exit_code!=0 || launch.timed_out || launch.output.find("Error:")!=std::string::npos || launch.output.find("Exception")!=std::string::npos) {
            status.error=true;status.message="平板 App 无法启动，请先安装 SecondaryScreen.apk";return status;
        }
        launched_=true;
    }
    status.ready=true;status.message="USB 已就绪，等待平板连接";return status;
}
}
