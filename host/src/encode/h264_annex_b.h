#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace pds {
struct H264Parameters {
    std::vector<uint8_t> sps_pps;
    unsigned length_size = 4;
};
bool NormalizeH264(std::span<const uint8_t> bytes, unsigned length_size, std::vector<uint8_t>& annex_b);
bool ReadH264Parameters(std::span<const uint8_t> sequence_header, H264Parameters& parameters);
bool HasH264Nal(std::span<const uint8_t> annex_b, uint8_t type);
bool HasH264BSlice(std::span<const uint8_t> annex_b);
bool MakeH264AccessUnit(std::span<const uint8_t> sample, H264Parameters& parameters,
                        std::vector<uint8_t>& output, bool& keyframe);
}
