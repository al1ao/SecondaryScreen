#include "app/process.h"
#include "util/log.h"
#include <array>
#include <memory>
namespace pds {
namespace {
struct Close { void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } };
using Handle = std::unique_ptr<void, Close>;
}
std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { quoted.append(slashes * 2 + 1, L'\\'); quoted += c; }
        else { quoted.append(slashes, L'\\'); quoted += c; }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'"';
}
std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count); return std::filesystem::path(path).parent_path();
}
ProcessResult RunProcess(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments, std::chrono::milliseconds timeout) {
    ProcessResult result;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!LOG_HR(CreatePipe(&read, &write, &security, 0) ? S_OK : LastErrorHr(), "CreatePipe child output")) return result;
    Handle reader(read), writer(write);
    if (!LOG_HR(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) ? S_OK : LastErrorHr(), "Non-inheritable child reader")) return result;
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) { LOG_HR(LastErrorHr(), "Open child stdin NUL"); return result; }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write; startup.StartupInfo.hStdError = write; startup.StartupInfo.hStdInput = input.get();
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    auto attributes = std::make_unique<unsigned char[]>(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.get());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) { LOG_HR(LastErrorHr(), "Child attribute list"); return result; }
    struct DeleteAttributes { LPPROC_THREAD_ATTRIBUTE_LIST list; ~DeleteAttributes() { DeleteProcThreadAttributeList(list); } } cleanup{startup.lpAttributeList};
    HANDLE inherited[]{write, input.get()};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        LOG_HR(LastErrorHr(), "Child handle allow-list"); return result;
    }
    std::wstring command = QuoteArgument(executable.wstring());
    for (const auto& argument : arguments) command += L" " + QuoteArgument(argument);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info)) {
        LOG_HR(LastErrorHr(), "CreateProcess hidden child"); return result;
    }
    Handle process(info.hProcess), thread(info.hThread);
    writer.reset();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto drain = [&] {
        DWORD available = 0;
        std::array<char, 4096> buffer{};
        // Bound each drain pass so a continuously writing child cannot starve
        // the process deadline. The pipe itself bounds remaining buffered data.
        for (unsigned chunks = 0; chunks < 64 && PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available; ++chunks) {
            DWORD count = 0;
            if (!ReadFile(read, buffer.data(), (std::min)(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) break;
            // Bound stored diagnostics; continue draining to prevent a full-pipe deadlock.
            if (result.output.size() < 1024 * 1024) result.output.append(buffer.data(), (std::min)(static_cast<size_t>(count), 1024 * 1024 - result.output.size()));
        }
    };
    while (WaitForSingleObject(process.get(), 10) == WAIT_TIMEOUT) {
        drain();
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            if (!TerminateProcess(process.get(), ERROR_TIMEOUT)) LOG_HR(LastErrorHr(), "Terminate timed out child");
            WaitForSingleObject(process.get(), 1000); break;
        }
    }
    drain();
    if (!GetExitCodeProcess(process.get(), &result.exit_code)) LOG_HR(LastErrorHr(), "Child exit code");
    return result;
}
}
