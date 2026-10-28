#include "encode/encoder_factory.h"
#include "util/log.h"
namespace pds {
std::unique_ptr<IGpuVideoEncoder> CreateBestEncoder(ID3D11Device* device, uint32_t width,
    uint32_t height, uint32_t fps, uint32_t bitrate_kbps, bool disable_hardware) {
    // Stage 2 deliberately starts at MF. NVENC is a stage-4 backend.
    if (!disable_hardware) {
        auto mf = CreateMfH264Encoder();
        if (mf->Initialize(device, width, height, fps, bitrate_kbps)) return mf;
    } else LOG_INFO("Hardware encoding disabled by --disable-hardware");
    LOG_WARN("MF H.264 unavailable on this adapter; try another adapter or stage-1 JPEG fallback");
    return nullptr;
}
}
