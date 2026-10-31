#pragma once
#include <filesystem>
#include "capture/capture.h"
namespace pds {
bool DumpFirstFrame(const CapturedFrame& frame, const std::filesystem::path& directory,
                    const char* path_name, uint32_t orientation);
bool DumpFirstFrame(const GpuFrame& frame, const std::filesystem::path& directory,
                    const char* path_name, uint32_t orientation);
}
