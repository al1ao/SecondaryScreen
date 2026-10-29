#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
namespace pds {
struct ProcessResult { unsigned long exit_code = 1; bool timed_out = false; std::string output; };
std::wstring QuoteArgument(const std::wstring& value);
ProcessResult RunProcess(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));
std::filesystem::path ExecutableDirectory();
}
