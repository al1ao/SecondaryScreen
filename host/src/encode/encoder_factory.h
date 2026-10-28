#pragma once
#include "encode/encoder.h"
namespace pds {
std::unique_ptr<IGpuVideoEncoder> CreateBestEncoder(ID3D11Device* device, uint32_t width,
    uint32_t height, uint32_t fps, uint32_t bitrate_kbps, bool disable_hardware = false);
}
