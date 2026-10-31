#include "display/monitor_enum.h"
#include "protocol/protocol.h"
#include <array>
#include <stdexcept>
#include "util/log.h"
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        for (const bool wgc : {false, true}) {
            for (uint32_t orientation = 0; orientation < 4; ++orientation)
                Require(pds::AckRotation(orientation, wgc) == 0, "unverified capture paths use composed desktop");
        }
        pds::DisplaySignature baseline{2944, 1840, 144, 0};
        Require(baseline == pds::DisplaySignature{2944, 1840, 144, 0}, "unchanged display");
        Require(baseline != pds::DisplaySignature{1840, 2944, 144, 0}, "size change");
        Require(baseline != pds::DisplaySignature{2944, 1840, 60, 0}, "refresh change");
        Require(baseline != pds::DisplaySignature{2944, 1840, 144, 1}, "orientation change");
        const auto portrait = pds::NegotiateResolution(1840, 2944, 1840, 2944, 4096, 4096);
        Require(portrait.width == 1840 && portrait.height == 2944, "portrait native size");
        const auto limited = pds::NegotiateResolution(1840, 2944, 1840, 2944, 1280, 2048);
        Require(limited.width == 1280 && limited.height == 2048, "portrait decoder limit keeps ratio");
        Require(sizeof(pds::HelloAck) == 32 && pds::kCapRotateOutput == 8, "wire layout");
        LOG_INFO("PASS stage 8 rotation policy");
        return 0;
    } catch (const std::exception& error) { LOG_ERROR("FAIL stage 8: ", error.what()); return 1; }
}
