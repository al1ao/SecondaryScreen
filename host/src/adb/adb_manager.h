#pragma once
#include "app/process.h"
#include <functional>
#include <optional>
namespace pds {
struct AdbDevice { std::string serial, state, model; bool operator==(const AdbDevice&) const = default; };
std::vector<AdbDevice> ParseAdbDevices(const std::string& text);
bool HasReverse(const std::string& text, uint16_t port);
std::optional<std::filesystem::path> FindAdb(const std::filesystem::path& program_directory);
struct AdbStatus { bool ready=false, error=false, changed=false; std::string message, serial; std::vector<AdbDevice> devices; };
class AdbManager {
public:
    using Runner=std::function<ProcessResult(const std::vector<std::wstring>&)>;
    explicit AdbManager(std::filesystem::path executable, Runner runner = {});
    ~AdbManager();
    AdbStatus Poll(uint16_t port, const std::string& preferred_serial, bool auto_reverse, bool foreground);
    void Reset();
private:
    ProcessResult Run(const std::vector<std::wstring>& args);
    std::filesystem::path executable_;
    Runner runner_;
    std::string serial_;
    uint16_t port_=0;
    bool server_started_=false, owns_mapping_=false, launched_=false;
};
}
