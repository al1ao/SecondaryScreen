#include "encode/h264_annex_b.h"
#include <algorithm>
namespace pds {
namespace {
constexpr uint8_t start_code[]{0, 0, 0, 1};
size_t StartAt(std::span<const uint8_t> bytes, size_t at) {
    if (at + 3 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0) {
        if (bytes[at + 2] == 1) return 3;
        if (at + 4 <= bytes.size() && bytes[at + 2] == 0 && bytes[at + 3] == 1) return 4;
    }
    return 0;
}
bool SplitAnnexB(std::span<const uint8_t> bytes, std::vector<std::span<const uint8_t>>& units) {
    size_t at = 0;
    while (at + 1 < bytes.size() && bytes[at] == 0 && !StartAt(bytes, at)) ++at;
    while (at < bytes.size()) {
        const auto prefix = StartAt(bytes, at);
        if (prefix == 0) return false;
        const size_t begin = at + prefix;
        size_t end = begin;
        while (end < bytes.size() && !StartAt(bytes, end)) ++end;
        size_t data_end = end;
        while (data_end > begin && bytes[data_end - 1] == 0) --data_end;
        if (data_end == begin || (bytes[begin] & 0x80u) != 0) return false;
        units.push_back(bytes.subspan(begin, data_end - begin));
        at = end;
    }
    return !units.empty();
}
void Append(std::vector<uint8_t>& out, std::span<const uint8_t> nal) {
    out.insert(out.end(), std::begin(start_code), std::end(start_code));
    out.insert(out.end(), nal.begin(), nal.end());
}
bool ParametersFromUnits(const std::vector<std::span<const uint8_t>>& units, std::vector<uint8_t>& out) {
    out.clear();
    bool sps = false, pps = false;
    for (const auto nal : units) {
        const auto type = nal[0] & 31;
        if (type == 7 || type == 8) { Append(out, nal); sps |= type == 7; pps |= type == 8; }
    }
    return sps && pps;
}
}
bool NormalizeH264(std::span<const uint8_t> bytes, unsigned length_size, std::vector<uint8_t>& out) {
    out.clear();
    if (bytes.empty() || length_size < 1 || length_size > 4) return false;
    // Try a complete length-prefixed parse first: a 357-byte AVCC NAL begins
    // 00 00 01 65 and must not be mistaken for an Annex-B IDR start code.
    size_t at = 0;
    bool valid_lengths = true;
    while (at < bytes.size()) {
        if (bytes.size() - at < length_size) { valid_lengths = false; break; }
        uint32_t length = 0;
        for (unsigned i = 0; i < length_size; ++i) length = (length << 8) | bytes[at++];
        if (length == 0 || length > bytes.size() - at || (bytes[at] & 0x80u) != 0) { valid_lengths = false; break; }
        Append(out, bytes.subspan(at, length));
        at += length;
    }
    if (valid_lengths && !out.empty()) return true;
    out.clear();
    std::vector<std::span<const uint8_t>> units;
    if (!SplitAnnexB(bytes, units)) return false;
    for (const auto unit : units) Append(out, unit);
    return true;
}
bool ReadH264Parameters(std::span<const uint8_t> bytes, H264Parameters& parameters) {
    if (bytes.empty()) return false;
    H264Parameters parsed;
    if (bytes[0] == 1 && bytes.size() >= 7) {
        parsed.length_size = (bytes[4] & 3u) + 1u;
        size_t at = 6;
        const unsigned sps_count = bytes[5] & 31u;
        bool sps = false, pps = false;
        for (unsigned group = 0; group < 2; ++group) {
            unsigned count = sps_count;
            if (group == 1) { if (at >= bytes.size()) return false; count = bytes[at++]; }
            for (unsigned i = 0; i < count; ++i) {
                if (bytes.size() - at < 2) return false;
                const size_t length = (static_cast<size_t>(bytes[at]) << 8) | bytes[at + 1]; at += 2;
                if (length == 0 || length > bytes.size() - at) return false;
                const auto type = bytes[at] & 31;
                if ((group == 0 && type != 7) || (group == 1 && type != 8)) return false;
                Append(parsed.sps_pps, bytes.subspan(at, length));
                sps |= type == 7; pps |= type == 8; at += length;
            }
        }
        if (!sps || !pps) return false;
    } else {
        std::vector<uint8_t> normalized;
        if (!NormalizeH264(bytes, parameters.length_size, normalized)) return false;
        std::vector<std::span<const uint8_t>> units;
        if (!SplitAnnexB(normalized, units) || !ParametersFromUnits(units, parsed.sps_pps)) return false;
        parsed.length_size = parameters.length_size;
    }
    parameters = std::move(parsed);
    return true;
}
bool HasH264Nal(std::span<const uint8_t> bytes, uint8_t type) {
    std::vector<std::span<const uint8_t>> units;
    if (!SplitAnnexB(bytes, units)) return false;
    return std::any_of(units.begin(), units.end(), [type](auto unit) { return (unit[0] & 31) == type; });
}
bool CountH264Slices(std::span<const uint8_t> bytes, std::array<uint64_t, 10>& counts) {
    std::vector<std::span<const uint8_t>> units;
    if (!SplitAnnexB(bytes, units)) return false;
    bool valid = true;
    for (const auto nal : units) {
        const auto type = nal[0] & 31;
        if (type != 1 && type != 5) continue;
        std::vector<uint8_t> rbsp;
        unsigned zeros = 0;
        for (size_t i = 1; i < nal.size(); ++i) {
            if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
            rbsp.push_back(nal[i]); zeros = nal[i] == 0 ? zeros + 1 : 0;
            if (rbsp.size() >= 16) break; // Only first_mb_in_slice and slice_type are needed.
        }
        size_t bit = 0;
        auto read_bit = [&]() -> int {
            if (bit >= rbsp.size() * 8) return -1;
            const auto value = (rbsp[bit / 8] >> (7 - bit % 8)) & 1; ++bit; return value;
        };
        auto ue = [&]() -> int64_t {
            unsigned leading = 0;
            int value = read_bit();
            while (value == 0 && leading < 31) { ++leading; value = read_bit(); }
            if (value < 0 || leading == 31) return -1;
            uint32_t suffix = 0;
            for (unsigned i = 0; i < leading; ++i) {
                value = read_bit(); if (value < 0) return -1;
                suffix = (suffix << 1) | static_cast<unsigned>(value);
            }
            return ((uint32_t{1} << leading) - 1u) + suffix;
        };
        if (ue() < 0) { valid = false; continue; } // first_mb_in_slice
        const auto slice_type = ue();
        if (slice_type < 0 || slice_type > 9) { valid = false; continue; }
        ++counts[static_cast<size_t>(slice_type)];
    }
    return valid;
}
bool HasH264BSlice(std::span<const uint8_t> bytes) {
    std::array<uint64_t, 10> counts{};
    CountH264Slices(bytes, counts);
    return counts[1] != 0 || counts[6] != 0;
}
bool MakeH264AccessUnit(std::span<const uint8_t> sample, H264Parameters& parameters,
    std::vector<uint8_t>& output, bool& keyframe) {
    std::vector<uint8_t> normalized;
    if (!NormalizeH264(sample, parameters.length_size, normalized)) return false;
    std::vector<std::span<const uint8_t>> units;
    if (!SplitAnnexB(normalized, units)) return false;
    std::vector<uint8_t> in_band;
    if (ParametersFromUnits(units, in_band)) parameters.sps_pps = std::move(in_band);
    keyframe = HasH264Nal(normalized, 5);
    if (!HasH264Nal(normalized, 1) && !keyframe) return false; // Parameter-only output is not a video frame.
    if (!keyframe) { output = std::move(normalized); return true; }
    if (!HasH264Nal(parameters.sps_pps, 7) || !HasH264Nal(parameters.sps_pps, 8)) return false;
    output = parameters.sps_pps;
    for (const auto unit : units) if ((unit[0] & 31) != 7 && (unit[0] & 31) != 8) Append(output, unit);
    return true;
}
}
