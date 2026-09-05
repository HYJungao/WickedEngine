#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>
#include <utility>

// Shared by both H264 encoders and the native decoder. This is the source
// clock carried in the compressed picture, independent of receiver clocks.
namespace wicked_newpipeline::video_identity
{
inline constexpr uint8_t kAnnexBStartCode[] = {0, 0, 0, 1};
struct NalUnit
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

inline std::vector<NalUnit> SplitAnnexB(const uint8_t* data, size_t size)
{
    std::vector<NalUnit> units;
    if (data == nullptr || size < 4)
        return units;
    const auto start_code_size = [&](size_t offset) -> size_t {
        if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 1)
            return 3;
        if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 0 && data[offset + 3] == 1)
            return 4;
        return 0;
    };
    size_t cursor = 0;
    while (cursor < size)
    {
        size_t prefix = 0;
        while (cursor < size && (prefix = start_code_size(cursor)) == 0)
            ++cursor;
        if (cursor >= size)
            break;
        const size_t nal_begin = cursor + prefix;
        size_t next = nal_begin;
        while (next < size && start_code_size(next) == 0)
            ++next;
        if (next > nal_begin)
            units.push_back({data + nal_begin, next - nal_begin});
        cursor = next;
    }
    return units;
}

// user_data_unregistered UUID for the stable source timestamp carried beside
// the compressed picture. capture_time_ms_ is local WebRTC timing metadata and
// is reconstructed on the receiver, so it cannot identify the matching
// DataChannel packet across a real RTP session.
constexpr uint8_t kFrameIdentityUuid[16] = {
    0x57, 0x49, 0x43, 0x4B, 0x45, 0x44, 0x2D, 0x4E,
    0x50, 0x2D, 0x46, 0x52, 0x41, 0x4D, 0x45, 0x31,
};

inline void PrependFrameIdentitySEI(
    std::vector<uint8_t>& annex_b, int64_t source_timestamp_usec)
{
    if (source_timestamp_usec <= 0)
        return;

    std::vector<uint8_t> rbsp;
    rbsp.reserve(28);
    rbsp.push_back(5); // user_data_unregistered
    rbsp.push_back(25); // UUID + version + uint64 timestamp
    rbsp.insert(rbsp.end(), std::begin(kFrameIdentityUuid),
        std::end(kFrameIdentityUuid));
    rbsp.push_back(1); // payload version
    const uint64_t timestamp = static_cast<uint64_t>(source_timestamp_usec);
    for (uint32_t shift = 0; shift < 64; shift += 8)
        rbsp.push_back(static_cast<uint8_t>(timestamp >> shift));
    rbsp.push_back(0x80); // rbsp_trailing_bits

    std::vector<uint8_t> nal;
    nal.reserve(rbsp.size() + 4);
    nal.push_back(0x06); // SEI NAL
    uint32_t consecutive_zeroes = 0;
    for (const uint8_t byte : rbsp)
    {
        if (consecutive_zeroes >= 2 && byte <= 0x03)
        {
            nal.push_back(0x03);
            consecutive_zeroes = 0;
        }
        nal.push_back(byte);
        consecutive_zeroes = byte == 0 ? consecutive_zeroes + 1 : 0;
    }

    std::vector<uint8_t> complete;
    complete.reserve(sizeof(kAnnexBStartCode) + nal.size() + annex_b.size());
    complete.insert(complete.end(), std::begin(kAnnexBStartCode), std::end(kAnnexBStartCode));
    complete.insert(complete.end(), nal.begin(), nal.end());
    complete.insert(complete.end(), annex_b.begin(), annex_b.end());
    annex_b = std::move(complete);
}

inline std::optional<int64_t> ParseFrameIdentitySEI(
    const uint8_t* data, size_t size)
{
    for (const NalUnit& unit : SplitAnnexB(data, size))
    {
        if (unit.size <= 1 || (unit.data[0] & 0x1fu) != 6)
            continue;

        std::vector<uint8_t> rbsp;
        rbsp.reserve(unit.size - 1);
        uint32_t consecutive_zeroes = 0;
        for (size_t index = 1; index < unit.size; ++index)
        {
            const uint8_t byte = unit.data[index];
            if (consecutive_zeroes >= 2 && byte == 0x03)
            {
                consecutive_zeroes = 0;
                continue;
            }
            rbsp.push_back(byte);
            consecutive_zeroes = byte == 0 ? consecutive_zeroes + 1 : 0;
        }

        size_t cursor = 0;
        while (cursor < rbsp.size())
        {
            uint32_t payload_type = 0;
            while (cursor < rbsp.size() && rbsp[cursor] == 0xff)
            {
                payload_type += 0xff;
                ++cursor;
            }
            if (cursor >= rbsp.size())
                break;
            payload_type += rbsp[cursor++];

            size_t payload_size = 0;
            while (cursor < rbsp.size() && rbsp[cursor] == 0xff)
            {
                payload_size += 0xff;
                ++cursor;
            }
            if (cursor >= rbsp.size())
                break;
            payload_size += rbsp[cursor++];
            if (payload_size > rbsp.size() - cursor)
                break;

            if (payload_type == 5 && payload_size >= 25 &&
                std::equal(std::begin(kFrameIdentityUuid),
                    std::end(kFrameIdentityUuid), rbsp.begin() + cursor) &&
                rbsp[cursor + 16] == 1)
            {
                uint64_t timestamp = 0;
                for (uint32_t byte = 0; byte < 8; ++byte)
                {
                    timestamp |= static_cast<uint64_t>(
                        rbsp[cursor + 17 + byte]) << (byte * 8);
                }
                if (timestamp > 0 &&
                    timestamp <= static_cast<uint64_t>(INT64_MAX))
                    return static_cast<int64_t>(timestamp);
            }
            cursor += payload_size;
        }
    }
    return std::nullopt;
}

} // namespace wicked_newpipeline::video_identity
