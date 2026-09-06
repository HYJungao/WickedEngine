#include "NewPipelineTransport.h"
#include "NewPipelinePacing.h"
#include "NewPipelineVideoFrameIdentity.h"
#if defined(__APPLE__)
extern "C" int np_validate_apple_video_codec_factories(
    char* error, size_t error_capacity);
#elif defined(_WIN32)
extern "C" int np_validate_windows_video_codec_factories(
    char* error, size_t error_capacity);
#endif
#include "NewPipelineWebRTCBridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <thread>
#include <shared_mutex>
#include <future>
#include <utility>

namespace wicked_newpipeline
{
namespace
{
constexpr uint32_t kMetadataBitCellSize = 4;
constexpr uint32_t kV3TilePadding = 4;
constexpr uint32_t kV3PixelBandMagic = 0x3342504Eu; // NPB3
constexpr uint32_t kV3PixelBandBytes = 40;
constexpr uint32_t kV3FrameMetadataMagic = 0x334d504eu; // NPM3
constexpr uint32_t kV3FrameMetadataHeaderBytes = 32;
constexpr float kHDRTransportMaximum = 16.0f;

bool EncodeDownstreamFrameMetadataBytes(
    const RemoteVideoFrameLayout& layout, std::vector<uint8_t>& bytes);
bool DecodeDownstreamFrameMetadataBytes(
    const uint8_t* bytes, size_t byte_count, RemoteVideoFrameLayout& layout);

constexpr uint32_t kRemoteVideoFlagHistoryValid = 1u << 0u;
constexpr uint32_t kRemoteVideoFlagResetThisFrame = 1u << 1u;
constexpr uint32_t kRemoteVideoFlagCameraCut = 1u << 2u;
constexpr uint32_t kRemoteVideoFlagValid = 1u << 3u;

uint32_t AlignEven(uint32_t value)
{
    return (value + 1u) & ~1u;
}

bool CheckedImageByteSize(uint32_t width, uint32_t height, size_t bytes_per_pixel, size_t& result)
{
    if (width == 0 || height == 0 || bytes_per_pixel == 0)
        return false;
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count / static_cast<size_t>(width) != static_cast<size_t>(height) ||
        pixel_count > std::numeric_limits<size_t>::max() / bytes_per_pixel)
    {
        return false;
    }
    result = pixel_count * bytes_per_pixel;
    return true;
}

uint32_t Fnv1a32(const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (uint32_t shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}
void AppendU64(std::vector<uint8_t>& bytes, uint64_t value)
{
    for (uint32_t shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}
void AppendFloat(std::vector<uint8_t>& bytes, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendU32(bytes, bits);
}
void AppendFloat3(std::vector<uint8_t>& bytes, const XMFLOAT3& value)
{
    AppendFloat(bytes, value.x);
    AppendFloat(bytes, value.y);
    AppendFloat(bytes, value.z);
}
void AppendMatrix(std::vector<uint8_t>& bytes, const XMFLOAT4X4& value)
{
    const float* data = &value._11;
    for (size_t index = 0; index < 16; ++index)
        AppendFloat(bytes, data[index]);
}

bool ComputeAtlasLayoutChecksum(
    const RemoteFrameContractV3& source,
    uint32_t& checksum)
{
    RemoteFrameContractV3 canonical = source;
    canonical.source_control_frame_id = 1;
    for (RemoteBufferDescriptorV3& descriptor : canonical.descriptors)
    {
        if ((descriptor.flags & kRemoteBufferDescriptorAvailableV3) == 0)
            continue;
        descriptor.content_frame_id = 1;
        descriptor.content_generation = 1;
        descriptor.confidence_unorm = 0;
        if (descriptor.semantic ==
            RemoteBufferSemantic::RemoteShadowVisibility)
        {
            descriptor.stable_subject_id = 1;
            descriptor.stable_subject_generation = 1;
        }
    }
    std::vector<uint8_t> bytes;
    if (!SerializeRemoteFrameContractV3(
            canonical, bytes, nullptr))
        return false;
    checksum = Fnv1a32(bytes.data(), bytes.size());
    return checksum != 0;
}

bool ConsumeU32(const uint8_t*& cursor, const uint8_t* end, uint32_t& value)
{
    if (static_cast<size_t>(end - cursor) < 4)
        return false;
    value = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8)
        value |= static_cast<uint32_t>(*cursor++) << shift;
    return true;
}
bool ConsumeU64(const uint8_t*& cursor, const uint8_t* end, uint64_t& value)
{
    if (static_cast<size_t>(end - cursor) < 8)
        return false;
    value = 0;
    for (uint32_t shift = 0; shift < 64; shift += 8)
        value |= static_cast<uint64_t>(*cursor++) << shift;
    return true;
}
bool ConsumeFloat(const uint8_t*& cursor, const uint8_t* end, float& value)
{
    uint32_t bits = 0;
    if (!ConsumeU32(cursor, end, bits))
        return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}
bool ConsumeFloat3(const uint8_t*& cursor, const uint8_t* end, XMFLOAT3& value)
{
    return ConsumeFloat(cursor, end, value.x) &&
        ConsumeFloat(cursor, end, value.y) &&
        ConsumeFloat(cursor, end, value.z);
}
bool ConsumeMatrix(const uint8_t*& cursor, const uint8_t* end, XMFLOAT4X4& value)
{
    float* data = &value._11;
    for (size_t index = 0; index < 16; ++index)
    {
        if (!ConsumeFloat(cursor, end, data[index]))
            return false;
    }
    return true;
}
void StoreU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    bytes[offset + 0] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8u);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16u);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24u);
}

uint8_t ClampByte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

void RGBToYUV(uint8_t red, uint8_t green, uint8_t blue, uint8_t& y, uint8_t& u, uint8_t& v)
{
    y = ClampByte(((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
    u = ClampByte(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
    v = ClampByte(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
}

void YUVToRGB(uint8_t y, uint8_t u, uint8_t v, uint8_t& red, uint8_t& green, uint8_t& blue)
{
    const int c = static_cast<int>(y) - 16;
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;
    red = ClampByte((298 * c + 409 * e + 128) >> 8);
    green = ClampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
    blue = ClampByte((298 * c + 516 * d + 128) >> 8);
}

uint32_t MetadataRowsForBytes(uint32_t video_width, size_t byte_count)
{
    const uint64_t cells = static_cast<uint64_t>(byte_count) * 8ull;
    const uint32_t cells_per_row = video_width / kMetadataBitCellSize;
    if (cells_per_row == 0)
        return 0;
    const uint64_t cell_rows = (cells + cells_per_row - 1ull) / cells_per_row;
    if (cell_rows > std::numeric_limits<uint32_t>::max() / kMetadataBitCellSize)
        return 0;
    return AlignEven(static_cast<uint32_t>(cell_rows) * kMetadataBitCellSize);
}

bool EncodeMetadataBits(
    const std::vector<uint8_t>& bytes,
    uint32_t video_width,
    uint32_t metadata_rows,
    std::vector<uint8_t>& metadata_luma)
{
    size_t metadata_size = 0;
    if (bytes.empty() || metadata_rows == 0 ||
        !CheckedImageByteSize(video_width, metadata_rows, 1u, metadata_size))
        return false;
    const uint32_t cells_per_row = video_width / kMetadataBitCellSize;
    if (cells_per_row == 0)
        return false;
    metadata_luma.assign(metadata_size, 16u);
    const uint64_t bit_count = static_cast<uint64_t>(bytes.size()) * 8ull;
    for (uint64_t bit_index = 0; bit_index < bit_count; ++bit_index)
    {
        const bool one = (bytes[bit_index / 8ull] & (1u << (bit_index & 7ull))) != 0;
        const uint32_t cell_x = static_cast<uint32_t>(bit_index % cells_per_row) * kMetadataBitCellSize;
        const uint32_t cell_y = static_cast<uint32_t>(bit_index / cells_per_row) * kMetadataBitCellSize;
        if (cell_y + kMetadataBitCellSize > metadata_rows)
            return false;
        const uint8_t value = one ? 224u : 32u;
        for (uint32_t y = 0; y < kMetadataBitCellSize; ++y)
            std::fill_n(metadata_luma.data() + static_cast<size_t>(cell_y + y) * video_width + cell_x,
                kMetadataBitCellSize, value);
    }
    return true;
}

bool DecodeMetadataBits(
    const RetainedI420Frame& video,
    size_t byte_count,
    uint32_t metadata_rows,
    std::vector<uint8_t>& bytes)
{
    if (!video.IsValid() || byte_count == 0 || metadata_rows == 0 || metadata_rows > video.height)
        return false;
    const uint32_t cells_per_row = video.width / kMetadataBitCellSize;
    if (cells_per_row == 0)
        return false;
    bytes.assign(byte_count, 0);
    const uint64_t bit_count = static_cast<uint64_t>(byte_count) * 8ull;
    for (uint64_t bit_index = 0; bit_index < bit_count; ++bit_index)
    {
        const uint32_t cell_x = static_cast<uint32_t>(bit_index % cells_per_row) * kMetadataBitCellSize;
        const uint32_t cell_y = static_cast<uint32_t>(bit_index / cells_per_row) * kMetadataBitCellSize;
        if (cell_y + kMetadataBitCellSize > metadata_rows)
            return false;
        uint32_t sum = 0;
        for (uint32_t y = 0; y < kMetadataBitCellSize; ++y)
        {
            for (uint32_t x = 0; x < kMetadataBitCellSize; ++x)
                sum += video.y_plane[static_cast<size_t>(cell_y + y) * video.y_stride + cell_x + x];
        }
        if (sum >= 128u * kMetadataBitCellSize * kMetadataBitCellSize)
            bytes[bit_index / 8ull] |= static_cast<uint8_t>(1u << (bit_index & 7ull));
    }
    return true;
}

uint64_t NowUsec()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count());
}

bool ValidateRemoteFrameMetadataV3(const RemoteFrameMetadata& metadata)
{
    const auto finite = [](const float* values, size_t count) {
        for (size_t index = 0; index < count; ++index)
        {
            if (!std::isfinite(values[index]))
                return false;
        }
        return true;
    };
    constexpr uint32_t all_semantics =
        static_cast<uint32_t>(RemoteBufferKind::All);
    return metadata.frame_id != 0 &&
        metadata.timestamp_usec != 0 &&
        metadata.valid &&
        metadata.width != 0 && metadata.height != 0 &&
        metadata.width <= kRemoteVideoV3MaxLogicalDimension &&
        metadata.height <= kRemoteVideoV3MaxLogicalDimension &&
        metadata.source_generation != 0 &&
        metadata.available_buffer_mask != 0 &&
        (metadata.available_buffer_mask & ~all_semantics) == 0 &&
        (metadata.continuity_mask & metadata.available_buffer_mask) ==
            metadata.available_buffer_mask &&
        metadata.dynamic_range == RemoteDynamicRange::HDR &&
        metadata.ddgi_reset_reason <= DDGIResetReason::GridChanged &&
        std::isfinite(metadata.confidence) &&
        metadata.confidence >= 0.0f && metadata.confidence <= 1.0f &&
        std::isfinite(metadata.near_plane) && metadata.near_plane > 0.0f &&
        std::isfinite(metadata.far_plane) &&
        metadata.far_plane > metadata.near_plane &&
        std::isfinite(metadata.pre_exposure) &&
        metadata.pre_exposure > 0.0f &&
        finite(&metadata.view_origin.x, 3) &&
        finite(&metadata.view_forward.x, 3) &&
        finite(&metadata.temporal_jitter_pixels.x, 2) &&
        finite(&metadata.view._11, 16) &&
        finite(&metadata.projection._11, 16) &&
        finite(&metadata.view_projection._11, 16) &&
        finite(&metadata.inverse_view._11, 16) &&
        finite(&metadata.inverse_projection._11, 16) &&
        finite(&metadata.inverse_view_projection._11, 16);
}

bool EncodeRemoteFrameMetadataV3(
    const RemoteFrameMetadata& metadata,
    std::vector<uint8_t>& bytes)
{
    if (!ValidateRemoteFrameMetadataV3(metadata))
        return false;
    const uint32_t flags =
        (metadata.history_valid ? kRemoteVideoFlagHistoryValid : 0u) |
        (metadata.reset_this_frame ? kRemoteVideoFlagResetThisFrame : 0u) |
        (metadata.camera_cut ? kRemoteVideoFlagCameraCut : 0u) |
        (metadata.valid ? kRemoteVideoFlagValid : 0u);
    bytes.clear();
    bytes.reserve(512);
    AppendU32(bytes, flags);
    AppendU64(bytes, metadata.frame_id);
    AppendU64(bytes, metadata.timestamp_usec);
    AppendU32(bytes, metadata.width);
    AppendU32(bytes, metadata.height);
    AppendU32(bytes, metadata.source_generation);
    AppendU32(bytes, metadata.continuity_mask);
    AppendU32(bytes, metadata.available_buffer_mask);
    AppendU32(bytes, static_cast<uint32_t>(metadata.dynamic_range));
    AppendU32(bytes, metadata.ddgi_frame_index);
    AppendU32(bytes, static_cast<uint32_t>(metadata.ddgi_reset_reason));
    AppendFloat(bytes, metadata.confidence);
    AppendFloat(bytes, metadata.near_plane);
    AppendFloat(bytes, metadata.far_plane);
    AppendFloat(bytes, metadata.pre_exposure);
    AppendFloat3(bytes, metadata.view_origin);
    AppendFloat3(bytes, metadata.view_forward);
    AppendFloat(bytes, metadata.temporal_jitter_pixels.x);
    AppendFloat(bytes, metadata.temporal_jitter_pixels.y);
    AppendMatrix(bytes, metadata.view);
    AppendMatrix(bytes, metadata.projection);
    AppendMatrix(bytes, metadata.view_projection);
    AppendMatrix(bytes, metadata.inverse_view);
    AppendMatrix(bytes, metadata.inverse_projection);
    AppendMatrix(bytes, metadata.inverse_view_projection);
    return true;
}

bool DecodeRemoteFrameMetadataV3(
    const uint8_t* bytes,
    size_t byte_count,
    RemoteFrameMetadata& metadata)
{
    if (bytes == nullptr || byte_count == 0)
        return false;
    const uint8_t* cursor = bytes;
    const uint8_t* end = bytes + byte_count;
    uint32_t flags = 0;
    uint32_t dynamic_range = 0;
    uint32_t reset_reason = 0;
    RemoteFrameMetadata decoded;
    if (!ConsumeU32(cursor, end, flags) ||
        !ConsumeU64(cursor, end, decoded.frame_id) ||
        !ConsumeU64(cursor, end, decoded.timestamp_usec) ||
        !ConsumeU32(cursor, end, decoded.width) ||
        !ConsumeU32(cursor, end, decoded.height) ||
        !ConsumeU32(cursor, end, decoded.source_generation) ||
        !ConsumeU32(cursor, end, decoded.continuity_mask) ||
        !ConsumeU32(cursor, end, decoded.available_buffer_mask) ||
        !ConsumeU32(cursor, end, dynamic_range) ||
        !ConsumeU32(cursor, end, decoded.ddgi_frame_index) ||
        !ConsumeU32(cursor, end, reset_reason) ||
        !ConsumeFloat(cursor, end, decoded.confidence) ||
        !ConsumeFloat(cursor, end, decoded.near_plane) ||
        !ConsumeFloat(cursor, end, decoded.far_plane) ||
        !ConsumeFloat(cursor, end, decoded.pre_exposure) ||
        !ConsumeFloat3(cursor, end, decoded.view_origin) ||
        !ConsumeFloat3(cursor, end, decoded.view_forward) ||
        !ConsumeFloat(cursor, end, decoded.temporal_jitter_pixels.x) ||
        !ConsumeFloat(cursor, end, decoded.temporal_jitter_pixels.y) ||
        !ConsumeMatrix(cursor, end, decoded.view) ||
        !ConsumeMatrix(cursor, end, decoded.projection) ||
        !ConsumeMatrix(cursor, end, decoded.view_projection) ||
        !ConsumeMatrix(cursor, end, decoded.inverse_view) ||
        !ConsumeMatrix(cursor, end, decoded.inverse_projection) ||
        !ConsumeMatrix(cursor, end, decoded.inverse_view_projection) ||
        cursor != end ||
        (flags & ~(kRemoteVideoFlagHistoryValid |
            kRemoteVideoFlagResetThisFrame |
            kRemoteVideoFlagCameraCut |
            kRemoteVideoFlagValid)) != 0)
    {
        return false;
    }
    decoded.dynamic_range =
        static_cast<RemoteDynamicRange>(dynamic_range);
    decoded.ddgi_reset_reason =
        static_cast<DDGIResetReason>(reset_reason);
    decoded.history_valid =
        (flags & kRemoteVideoFlagHistoryValid) != 0;
    decoded.reset_this_frame =
        (flags & kRemoteVideoFlagResetThisFrame) != 0;
    decoded.camera_cut =
        (flags & kRemoteVideoFlagCameraCut) != 0;
    decoded.valid = (flags & kRemoteVideoFlagValid) != 0;
    decoded.source_stream_id = kRemoteFrameStreamId;
    decoded.local_receive_timestamp_usec = NowUsec();
    if (!ValidateRemoteFrameMetadataV3(decoded))
        return false;
    metadata = std::move(decoded);
    return true;
}

void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
}
} // namespace

RemoteFrameSource::RemoteFrameSource()
{
    for (size_t index = 0; index < buffers.size(); ++index)
    {
        buffers[index].semantic = static_cast<RemoteBufferSemantic>(index);
        buffers[index].encoding = RemoteBufferTransportEncoding(buffers[index].semantic);
    }
}

bool BuildRemoteVideoFrameLayoutV3(
    const RemoteFrameSource& frame,
    const RemoteStreamSelection& selection,
    uint64_t source_control_frame_id,
    uint64_t stable_shadow_id,
    uint32_t stable_shadow_generation,
    RemoteVideoFrameLayout& layout,
    std::vector<uint8_t>& metadata_luma,
    std::string* error,
    const std::array<RemoteBufferContentStateV3,
        static_cast<size_t>(RemoteBufferSemantic::Count)>* content_states)
{
    layout = {};
    metadata_luma.clear();
    if (selection.protocol_version != kRemoteVideoWireVersionV3 ||
        selection.encoding_profile_id != kRemoteEncodingProfileI420V3 ||
        source_control_frame_id == 0 || !frame.metadata.valid ||
        frame.metadata.frame_id == 0 || frame.metadata.source_generation == 0)
    {
        SetError(error, "V3 layout requires a negotiated profile and complete frame/control identity");
        return false;
    }

    struct Region
    {
        size_t index = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::array<Region, static_cast<size_t>(RemoteBufferSemantic::Count)> regions = {};
    size_t region_count = 0;
    uint32_t available_mask = 0;
    auto scale_dimension = [&](RemoteBufferSemantic semantic, uint32_t value) {
        uint32_t divisor = 1;
        if (selection.quality_tier == RemoteQualityTierV3::Balanced)
        {
            divisor = semantic == RemoteBufferSemantic::RemoteAO ? 4u :
                (semantic == RemoteBufferSemantic::RemoteShadowVisibility ? 1u : 2u);
        }
        else if (selection.quality_tier == RemoteQualityTierV3::Low)
        {
            divisor = semantic == RemoteBufferSemantic::RemoteAO ? 8u :
                (semantic == RemoteBufferSemantic::RemoteShadowVisibility ? 1u : 4u);
        }
        return AlignEven(std::max(2u, (value + divisor - 1u) / divisor));
    };

    RemoteFrameContractV3 contract;
    contract.quality_tier = selection.quality_tier;
    contract.encoding_profile_id = selection.encoding_profile_id;
    contract.source_control_frame_id = source_control_frame_id;
    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteSemanticSource& buffer = frame.buffers[index];
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if (buffer.semantic != semantic ||
            buffer.encoding != RemoteBufferTransportEncoding(semantic) ||
            (buffer.available && (buffer.width == 0 || buffer.height == 0 ||
                buffer.width > kRemoteVideoV3MaxLogicalDimension ||
                buffer.height > kRemoteVideoV3MaxLogicalDimension)))
        {
            SetError(error, "V3 source buffer violates its semantic contract");
            return false;
        }

        RemoteBufferDescriptorV3& descriptor = contract.descriptors[index];
        descriptor.semantic = semantic;
        descriptor.representation = RemoteBufferRepresentationContractV3(semantic);
        descriptor.encoding = RemoteBufferTransportEncoding(semantic);
        const bool shadow_identity_valid = semantic != RemoteBufferSemantic::RemoteShadowVisibility ||
            (stable_shadow_id != 0 && stable_shadow_generation != 0);
        if (!buffer.available || !shadow_identity_valid)
            continue;

        descriptor.flags = kRemoteBufferDescriptorAvailableV3;
        descriptor.logical_width = static_cast<uint16_t>(buffer.width);
        descriptor.logical_height = static_cast<uint16_t>(buffer.height);
        descriptor.atlas_width = static_cast<uint16_t>(scale_dimension(semantic, buffer.width));
        descriptor.atlas_height = static_cast<uint16_t>(scale_dimension(semantic, buffer.height));
        const RemoteBufferContentStateV3* content_state =
            content_states != nullptr ? &(*content_states)[index] : nullptr;
        if (content_state != nullptr && content_state->frame_id != 0)
        {
            descriptor.content_frame_id = content_state->frame_id;
            descriptor.content_generation = content_state->generation;
            descriptor.confidence_unorm = content_state->confidence_unorm;
        }
        else
        {
            descriptor.content_frame_id = frame.metadata.frame_id;
            descriptor.content_generation = frame.metadata.source_generation;
            descriptor.confidence_unorm = static_cast<uint16_t>(
                std::clamp(frame.metadata.confidence, 0.0f, 1.0f) *
                    65535.0f + 0.5f);
        }
        if (semantic == RemoteBufferSemantic::RemoteShadowVisibility)
        {
            descriptor.stable_subject_id = stable_shadow_id;
            descriptor.stable_subject_generation = stable_shadow_generation;
        }
        regions[region_count++] = Region{
            index, descriptor.atlas_width, descriptor.atlas_height};
        available_mask |= RemoteBufferKindMask(semantic);
    }
    if (region_count == 0 || (frame.metadata.continuity_mask & available_mask) != available_mask)
    {
        SetError(error, "V3 frame has no continuous available semantics");
        return false;
    }

    std::array<size_t, static_cast<size_t>(RemoteBufferSemantic::Count)> order = {};
    for (size_t i = 0; i < region_count; ++i) order[i] = i;
    std::array<XMUINT2, static_cast<size_t>(RemoteBufferSemantic::Count)> best_origins = {};
    uint64_t best_i420_bytes = std::numeric_limits<uint64_t>::max();
    uint64_t best_aspect_error = std::numeric_limits<uint64_t>::max();
    uint32_t best_width = 0;
    uint32_t best_height = 0;
    do
    {
        const uint32_t break_count = region_count > 0 ? (1u << static_cast<uint32_t>(region_count - 1u)) : 0u;
        for (uint32_t break_mask = 0; break_mask < break_count; ++break_mask)
        {
            std::array<XMUINT2, static_cast<size_t>(RemoteBufferSemantic::Count)> origins = {};
            uint32_t x = 0, y = 0, row_height = 0, packed_width = 0;
            for (size_t position = 0; position < region_count; ++position)
            {
                if (position > 0 && (break_mask & (1u << static_cast<uint32_t>(position - 1u))) != 0)
                {
                    packed_width = std::max(packed_width, x);
                    y += row_height;
                    x = 0;
                    row_height = 0;
                }
                const Region& region = regions[order[position]];
                origins[region.index] = XMUINT2(x + kV3TilePadding, y + kV3TilePadding);
                x += region.width + kV3TilePadding * 2u;
                row_height = std::max(row_height, region.height + kV3TilePadding * 2u);
            }
            packed_width = AlignEven(std::max(packed_width, x));
            const uint32_t packed_height = AlignEven(y + row_height);
            const uint32_t metadata_rows = MetadataRowsForBytes(packed_width, kV3PixelBandBytes);
            const uint32_t video_height = AlignEven(metadata_rows + packed_height);
            if (packed_width == 0 || metadata_rows == 0 ||
                packed_width > kRemoteVideoV3MaxAtlasDimension ||
                video_height > kRemoteVideoV3MaxAtlasDimension)
                continue;
            const uint64_t bytes = static_cast<uint64_t>(packed_width) * video_height * 3ull / 2ull;
            const uint64_t aspect_error = static_cast<uint64_t>(
                std::llabs(static_cast<long long>(packed_width) * 9ll -
                    static_cast<long long>(video_height) * 16ll));
            if (bytes < best_i420_bytes || (bytes == best_i420_bytes && aspect_error < best_aspect_error))
            {
                best_i420_bytes = bytes;
                best_aspect_error = aspect_error;
                best_width = packed_width;
                best_height = video_height;
                best_origins = origins;
            }
        }
    } while (std::next_permutation(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(region_count)));

    if (best_width == 0 || best_height == 0)
    {
        SetError(error, "V3 deterministic atlas packing found no valid codec-aligned layout");
        return false;
    }
    const uint32_t metadata_rows = MetadataRowsForBytes(best_width, kV3PixelBandBytes);
    contract.atlas_width = static_cast<uint16_t>(best_width);
    contract.atlas_height = static_cast<uint16_t>(best_height);
    for (const Region& region : regions)
    {
        if (region.width == 0)
            continue;
        RemoteBufferDescriptorV3& descriptor = contract.descriptors[region.index];
        descriptor.atlas_x = static_cast<uint16_t>(best_origins[region.index].x);
        descriptor.atlas_y = static_cast<uint16_t>(metadata_rows + best_origins[region.index].y);
    }
    if (!ValidateRemoteFrameContractV3(contract, error))
        return false;

    std::vector<uint8_t> contract_bytes;
    if (!SerializeRemoteFrameContractV3(contract, contract_bytes, error))
        return false;
    uint32_t layout_checksum = 0;
    if (!ComputeAtlasLayoutChecksum(contract, layout_checksum))
    {
        SetError(error, "V3 atlas layout checksum could not be computed");
        return false;
    }
    const uint32_t descriptor_checksum = Fnv1a32(contract_bytes.data(), contract_bytes.size());
    std::vector<uint8_t> pixel_band;
    pixel_band.reserve(kV3PixelBandBytes);
    AppendU32(pixel_band, kV3PixelBandMagic);
    AppendU32(pixel_band, kRemoteVideoWireVersionV3);
    AppendU32(pixel_band, kV3PixelBandBytes);
    AppendU32(pixel_band, 0);
    AppendU64(pixel_band, frame.metadata.frame_id);
    AppendU32(pixel_band, frame.metadata.source_generation);
    AppendU32(pixel_band, descriptor_checksum);
    AppendU64(pixel_band, source_control_frame_id);
    StoreU32(pixel_band, 12, Fnv1a32(pixel_band.data() + 16, pixel_band.size() - 16));
    if (pixel_band.size() != kV3PixelBandBytes ||
        !EncodeMetadataBits(pixel_band, best_width, metadata_rows, metadata_luma))
    {
        SetError(error, "V3 pixel metadata band could not be encoded");
        return false;
    }

    layout.metadata = frame.metadata;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    for (const RemoteBufferDescriptorV3& descriptor : contract.descriptors)
    {
        logical_width = std::max(logical_width, static_cast<uint32_t>(descriptor.logical_width));
        logical_height = std::max(logical_height, static_cast<uint32_t>(descriptor.logical_height));
    }
    layout.metadata.width = logical_width;
    layout.metadata.height = logical_height;
    layout.metadata.available_buffer_mask = available_mask;
    layout.protocol_version = kRemoteVideoWireVersionV3;
    layout.encoding_profile_id = selection.encoding_profile_id;
    layout.quality_tier = selection.quality_tier;
    layout.source_control_frame_id = source_control_frame_id;
    layout.layout_checksum = layout_checksum;
    layout.descriptor_checksum = descriptor_checksum;
    layout.metadata_rows = metadata_rows;
    layout.video_width = best_width;
    layout.video_height = best_height;
    layout.contract_v3 = contract;
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteBufferDescriptorV3& descriptor = contract.descriptors[index];
        RemoteVideoTileLayout& tile = layout.tiles[index];
        tile.semantic = descriptor.semantic;
        tile.width = descriptor.atlas_width;
        tile.height = descriptor.atlas_height;
        tile.origin_x = descriptor.atlas_x;
        tile.origin_y = descriptor.atlas_y;
        tile.available = (descriptor.flags & kRemoteBufferDescriptorAvailableV3) != 0;
        tile.encoding = descriptor.encoding;
    }
    return true;
}

bool RetainedI420Frame::IsValid() const
{
    return width >= kMetadataBitCellSize && height > 0 && (width & 1u) == 0 && (height & 1u) == 0 &&
        y_plane != nullptr && u_plane != nullptr && v_plane != nullptr &&
        y_stride >= width && u_stride >= width / 2u && v_stride >= width / 2u;
}

bool RetainedNV12Frame::IsValid() const
{
    return width > 0 && height > 0 && (width & 1u) == 0 &&
        (height & 1u) == 0 && texture_shared_handle != nullptr &&
        fence_shared_handle != nullptr && producer_fence_value != 0 &&
        consumer_fence_value > producer_fence_value && adapter_luid != 0 &&
        frame_lifetime != nullptr;
}

void RetainedNV12Frame::MarkCompletionScheduled() const
{
    if (mark_completion_scheduled != nullptr && completion_context != nullptr)
        mark_completion_scheduled(completion_context);
}

bool DecodeRemoteVideoFrameLayout(
    const RetainedI420Frame& video, RemoteVideoFrameLayout& layout, std::string* error)
{
    layout = {};
    if (!video.IsValid())
    {
        SetError(error, "remote I420 plane layout is invalid");
        return false;
    }

    const uint32_t v3_metadata_rows = MetadataRowsForBytes(video.width, kV3PixelBandBytes);
    std::vector<uint8_t> v3_pixel_band;
    if (v3_metadata_rows == 0 || v3_metadata_rows >= video.height ||
        !DecodeMetadataBits(video, kV3PixelBandBytes, v3_metadata_rows, v3_pixel_band))
    {
        SetError(error, "V3 pixel metadata band is invalid");
        return false;
    }
    const uint8_t* cursor = v3_pixel_band.data();
    const uint8_t* end = cursor + v3_pixel_band.size();
    uint32_t magic = 0, version = 0, byte_size = 0, checksum = 0;
    uint64_t frame_id = 0, source_control_frame_id = 0;
    uint32_t generation = 0, descriptor_checksum = 0;
    if (!ConsumeU32(cursor, end, magic) ||
        !ConsumeU32(cursor, end, version) ||
        !ConsumeU32(cursor, end, byte_size) ||
        !ConsumeU32(cursor, end, checksum) ||
        !ConsumeU64(cursor, end, frame_id) ||
        !ConsumeU32(cursor, end, generation) ||
        !ConsumeU32(cursor, end, descriptor_checksum) ||
        !ConsumeU64(cursor, end, source_control_frame_id) ||
        cursor != end || magic != kV3PixelBandMagic ||
        version != kRemoteVideoWireVersionV3 ||
        byte_size != kV3PixelBandBytes ||
        checksum != Fnv1a32(
            v3_pixel_band.data() + 16,
            v3_pixel_band.size() - 16) ||
        frame_id == 0 || generation == 0 ||
        descriptor_checksum == 0 ||
        source_control_frame_id == 0)
    {
        SetError(error, "V3 pixel metadata checksum or identity is invalid");
        return false;
    }
    layout.protocol_version = version;
    layout.video_width = video.width;
    layout.video_height = video.height;
    layout.metadata_rows = v3_metadata_rows;
    layout.metadata.frame_id = frame_id;
    layout.metadata.source_generation = generation;
    layout.metadata.valid = true;
    layout.source_control_frame_id = source_control_frame_id;
    layout.descriptor_checksum = descriptor_checksum;
    return true;
}

bool ValidateRemoteTransportSelfTest(std::string* error)
{
    // Exercise the production cadence at fixed and fluctuating render rates.
    // The former reset-to-zero implementation sends only ~406 controls at
    // 61 FPS over 20 seconds, instead of the expected ~600.
    for (const int fps : {24, 59, 60, 61, 75, 90, 120, 144})
    {
        float accumulator = 0;
        int sent = 0;
        for (int tick = 0; tick < fps * 20; ++tick)
        {
            accumulator += 1.0f / fps;
            sent += ConsumeControlPublishInterval(accumulator, 1.0f / 30.0f) ? 1 : 0;
        }
        if (std::abs(sent - std::min(fps, 30) * 20) > 1)
        {
            SetError(error, "control cadence drifted at " + std::to_string(fps) + " FPS");
            return false;
        }
    }
    float accumulator = 0;
    double elapsed = 0;
    int sent = 0;
    for (int tick = 0; tick < 2400; ++tick)
    {
        const float dt = 1.0f / (tick % 2 ? 61.0f : 59.0f);
        elapsed += dt;
        accumulator += dt;
        sent += ConsumeControlPublishInterval(accumulator, 1.0f / 30.0f) ? 1 : 0;
    }
    if (std::abs(sent - elapsed * 30.0) > 1.0)
    {
        SetError(error, "control cadence drifted under fluctuating render FPS");
        return false;
    }
    accumulator = 5.0f;
    if (!ConsumeControlPublishInterval(accumulator, 1.0f / 30.0f) ||
        ConsumeControlPublishInterval(accumulator, 1.0f / 30.0f))
    {
        SetError(error, "control stall replayed obsolete snapshots in a burst");
        return false;
    }
    accumulator = 0.19f;
    if (ConsumeControlPublishInterval(accumulator, 1.0f / 5.0f) ||
        !ConsumeControlPublishInterval(accumulator, 1.0f / 30.0f))
    {
        SetError(error, "heartbeat to dirty-input transition delayed the snapshot");
        return false;
    }

    RemoteStreamStatus selected;
    selected.control_frame_id = 1;
    selected.code = RemoteStreamStatusCode::Selected;
    selected.selection = {kRemoteVideoWireVersionV3, kRemoteEncodingProfileI420V3, RemoteQualityTierV3::High};
    StreamStatusPublication status;
    if (status.CanPublish(selected, selected.selection) || !status.ShouldSend(selected, 0))
    {
        SetError(error, "first video bypassed negotiation status");
        return false;
    }
    status.RecordAttempt(selected, false, 0);
    if (status.CanPublish(selected, selected.selection) || status.ShouldSend(selected, 99'999) ||
        !status.ShouldSend(selected, 100'000))
    {
        SetError(error, "failed negotiation status did not apply bounded retry");
        return false;
    }
    status.RecordAttempt(selected, true, 100'000);
    // An unchanged heartbeat must still repair status loss on the wire.
    if (!status.CanPublish(selected, selected.selection) || status.ShouldSend(selected, 299'999) ||
        !status.ShouldSend(selected, 300'000))
    {
        SetError(error, "active selection did not permit video / periodic status recovery");
        return false;
    }
    ++selected.control_frame_id;
    status.RecordAttempt(selected, false, 300'000);
    if (!status.CanPublish(selected, selected.selection))
    {
        SetError(error, "ordinary input status failure blocked active video");
        return false;
    }
    auto changed = selected;
    changed.selection.quality_tier = RemoteQualityTierV3::Balanced;
    if (!status.ShouldSend(changed, 300'001) || status.CanPublish(changed, changed.selection))
    {
        SetError(error, "changed selection reused an old announcement");
        return false;
    }
    status.RecordAttempt(changed, true, 300'001);
    if (!status.CanPublish(changed, changed.selection) || status.CanPublish(changed, selected.selection))
    {
        SetError(error, "selection transition published a stale queued layout");
        return false;
    }
    auto rejected = changed;
    rejected.code = RemoteStreamStatusCode::NoCommonProtocol;
    if (status.CanPublish(rejected, changed.selection))
    {
        SetError(error, "rejected selection published video");
        return false;
    }
    status = {};
    if (status.CanPublish(changed, changed.selection))
    {
        SetError(error, "reconnect reused previous session announcement");
        return false;
    }
    if (!ValidateRemoteTransportLifecycleSelfTest(error))
        return false;
    const std::vector<uint8_t> picture = {0, 0, 0, 1, 0x65, 0x88, 0x84};
    // Include start-code-like byte patterns to exercise SEI emulation prevention.
    for (const int64_t timestamp : std::array<int64_t, 4>{1, 0x000003000001ll, 123456789, INT64_MAX})
    {
        auto encoded = picture;
        video_identity::PrependFrameIdentitySEI(encoded, timestamp);
        if (video_identity::ParseFrameIdentitySEI(encoded.data(), encoded.size()) != timestamp ||
            !std::equal(picture.rbegin(), picture.rend(), encoded.rbegin()))
        {
            SetError(error, "shared H264 source identity roundtrip failed");
            return false;
        }
        const size_t sei_size = encoded.size() - picture.size();
        if (video_identity::ParseFrameIdentitySEI(encoded.data(), sei_size - 2).has_value())
        {
            SetError(error, "truncated H264 identity was accepted");
            return false;
        }
    }
    if (video_identity::ParseFrameIdentitySEI(picture.data(), picture.size()).has_value())
    {
        SetError(error, "missing H264 source identity was fabricated");
        return false;
    }
    char program[] = "newpipeline";
    char software_argument[] = "--remote_encoder=software";
    char invalid_argument[] = "--remote_encoder=invalid";
    char* default_arguments[] = {program};
    char* software_arguments[] = {program, software_argument};
    char* invalid_arguments[] = {program, invalid_argument};
    const RuntimeConfig default_config = ParseRuntimeConfig(1, default_arguments);
    const RuntimeConfig software_config = ParseRuntimeConfig(2, software_arguments);
    const RuntimeConfig invalid_config = ParseRuntimeConfig(2, invalid_arguments);
    if (default_config.remote_encoder != RemoteEncoderPreference::Hardware ||
        software_config.remote_encoder != RemoteEncoderPreference::Software ||
        invalid_config.remote_encoder != RemoteEncoderPreference::Hardware ||
        invalid_config.parse_warnings.empty())
    {
        SetError(error, "remote encoder command-line selection self-test failed");
        return false;
    }
    if (RemoteVideoRtpTimestamp(1'000'000ull) != 90'000u ||
        RemoteVideoRtpTimestamp(1'033'333ull) != 92'999u ||
        RemoteVideoRtpTimestamp(1'000'000ull) ==
            RemoteVideoRtpTimestamp(1'033'333ull))
    {
        SetError(error, "native video RTP identity self-test failed");
        return false;
    }
#if defined(__APPLE__)
    char apple_codec_error[256] = {};
    if (np_validate_apple_video_codec_factories(
            apple_codec_error, sizeof(apple_codec_error)) != 1)
    {
        SetError(error, apple_codec_error[0] != '\0'
            ? apple_codec_error : "Apple codec factory self-test failed");
        return false;
    }
#elif defined(_WIN32)
    char windows_codec_error[256] = {};
    if (np_validate_windows_video_codec_factories(
            windows_codec_error, sizeof(windows_codec_error)) != 1)
    {
        SetError(error, windows_codec_error[0] != '\0'
            ? windows_codec_error : "Windows codec factory self-test failed");
        return false;
    }
#endif

    if (!ValidateFormalLightingBlendV3Reference(error) ||
        !ValidateRemoteFrameContractV3SelfTest(error) ||
        !ValidateRemoteProtocolNegotiationSelfTest(error))
        return false;

    RemoteFrameSource source;
    source.metadata.frame_id = 101;
    source.metadata.timestamp_usec = 202;
    source.metadata.source_generation = 7;
    source.metadata.continuity_mask = static_cast<uint32_t>(RemoteBufferKind::All);
    source.metadata.available_buffer_mask = static_cast<uint32_t>(RemoteBufferKind::All);
    source.metadata.dynamic_range = RemoteDynamicRange::HDR;
    source.metadata.source_stream_id = kRemoteFrameStreamId;
    source.metadata.temporal_jitter_pixels = XMFLOAT2(0.25f, -0.125f);
    source.metadata.pre_exposure = 1.5f;
    source.metadata.history_valid = true;
    source.metadata.camera_cut = true;
    source.metadata.ddgi_frame_index = 42;
    source.metadata.ddgi_reset_reason = DDGIResetReason::LightingChanged;
    source.metadata.valid = true;
    for (size_t index = 0; index < source.buffers.size(); ++index)
    {
        RemoteSemanticSource& buffer = source.buffers[index];
        buffer.semantic = static_cast<RemoteBufferSemantic>(index);
        buffer.width = 1280;
        buffer.height = 720;
        buffer.available = true;
        buffer.encoding = RemoteBufferTransportEncoding(buffer.semantic);
    }
    RemoteStreamSelection selection;
    selection.protocol_version = kRemoteVideoWireVersionV3;
    selection.encoding_profile_id = kRemoteEncodingProfileI420V3;
    selection.quality_tier = RemoteQualityTierV3::Balanced;
    RemoteVideoFrameLayout layout;
    std::vector<uint8_t> metadata_luma;
    if (!BuildRemoteVideoFrameLayoutV3(
            source, selection, 99, 0x1234, 3, layout, metadata_luma, error))
        return false;

    uint64_t content_area = 0;
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        content_area += static_cast<uint64_t>(tile.width) * tile.height;
        if (!tile.available)
            continue;
        if (tile.origin_x < kV3TilePadding ||
            tile.origin_y < layout.metadata_rows + kV3TilePadding ||
            tile.origin_x + tile.width + kV3TilePadding >
                layout.video_width ||
            tile.origin_y + tile.height + kV3TilePadding >
                layout.video_height)
        {
            SetError(error, "V3 tile padding exceeds atlas bounds");
            return false;
        }
        for (size_t other_index = index + 1;
            other_index < layout.tiles.size(); ++other_index)
        {
            const RemoteVideoTileLayout& other =
                layout.tiles[other_index];
            if (!other.available)
                continue;
            const bool padded_overlap =
                tile.origin_x - kV3TilePadding <
                    other.origin_x + other.width + kV3TilePadding &&
                tile.origin_x + tile.width + kV3TilePadding >
                    other.origin_x - kV3TilePadding &&
                tile.origin_y - kV3TilePadding <
                    other.origin_y + other.height + kV3TilePadding &&
                tile.origin_y + tile.height + kV3TilePadding >
                    other.origin_y - kV3TilePadding;
            if (padded_overlap)
            {
                SetError(error, "V3 padded atlas regions overlap");
                return false;
            }
        }
    }
    const uint64_t four_full_resolution_cells = 4ull * 1280ull * 720ull;
    if (content_area * 2ull > four_full_resolution_cells)
    {
        SetError(error, "Balanced V3 content area did not achieve the required 50 percent reduction");
        return false;
    }

    RemoteStreamSelection high_selection = selection;
    high_selection.quality_tier = RemoteQualityTierV3::High;
    RemoteVideoFrameLayout high_layout;
    if (!BuildRemoteVideoFrameLayoutV3(
            source, high_selection, 99, 0x1234, 3,
            high_layout, metadata_luma, error))
        return false;
    uint64_t high_content_area = 0;
    for (const RemoteVideoTileLayout& tile : high_layout.tiles)
    {
        high_content_area +=
            static_cast<uint64_t>(tile.width) * tile.height;
        if (tile.available &&
            (tile.width != 1280 || tile.height != 720))
        {
            SetError(error,
                "High V3 changed a frozen full-resolution semantic");
            return false;
        }
    }
    if (high_content_area != four_full_resolution_cells)
    {
        SetError(error, "High V3 full-resolution area mismatch");
        return false;
    }

    // Equal-sized fixtures cannot reveal semantic tile swaps. Give every
    // semantic a distinct extent and verify the complete source ->
    // descriptor -> atlas-tile identity chain.
    RemoteFrameSource identity_source = source;
    constexpr std::array<XMUINT2,
        static_cast<size_t>(RemoteBufferSemantic::Count)>
        identity_extents = {
            XMUINT2(1280, 720),
            XMUINT2(640, 360),
            XMUINT2(960, 540),
            XMUINT2(1024, 576),
        };
    for (size_t index = 0; index < identity_source.buffers.size(); ++index)
    {
        identity_source.buffers[index].width = identity_extents[index].x;
        identity_source.buffers[index].height = identity_extents[index].y;
    }
    RemoteVideoFrameLayout identity_layout;
    if (!BuildRemoteVideoFrameLayoutV3(
            identity_source, high_selection, 99, 0x1234, 3,
            identity_layout, metadata_luma, error))
        return false;
    for (size_t index = 0; index < identity_source.buffers.size(); ++index)
    {
        const RemoteBufferSemantic expected_semantic =
            static_cast<RemoteBufferSemantic>(index);
        const RemoteSemanticSource& source_buffer =
            identity_source.buffers[index];
        const RemoteBufferDescriptorV3& descriptor =
            identity_layout.contract_v3.descriptors[index];
        const RemoteVideoTileLayout& tile =
            identity_layout.tiles[index];
        if (descriptor.semantic != expected_semantic ||
            descriptor.representation !=
                RemoteBufferRepresentationContractV3(expected_semantic) ||
            descriptor.encoding !=
                RemoteBufferTransportEncoding(expected_semantic) ||
            descriptor.logical_width != source_buffer.width ||
            descriptor.logical_height != source_buffer.height ||
            descriptor.atlas_width != source_buffer.width ||
            descriptor.atlas_height != source_buffer.height ||
            tile.semantic != expected_semantic ||
            tile.encoding != descriptor.encoding ||
            tile.width != descriptor.atlas_width ||
            tile.height != descriptor.atlas_height ||
            tile.origin_x != descriptor.atlas_x ||
            tile.origin_y != descriptor.atlas_y)
        {
            SetError(error,
                "V3 semantic source/descriptor/atlas identity mismatch");
            return false;
        }
        for (size_t other = index + 1;
            other < identity_layout.tiles.size(); ++other)
        {
            if (tile.origin_x ==
                    identity_layout.tiles[other].origin_x &&
                tile.origin_y ==
                    identity_layout.tiles[other].origin_y)
            {
                SetError(error,
                    "V3 semantic atlas origins are not unique");
                return false;
            }
        }
    }

    RemoteStreamSelection low_selection = selection;
    low_selection.quality_tier = RemoteQualityTierV3::Low;
    RemoteVideoFrameLayout low_layout;
    if (!BuildRemoteVideoFrameLayoutV3(
            source, low_selection, 99, 0x1234, 3,
            low_layout, metadata_luma, error))
        return false;
    const RemoteVideoTileLayout& low_shadow =
        low_layout.tiles[static_cast<size_t>(
            RemoteBufferSemantic::RemoteShadowVisibility)];
    if (!low_shadow.available ||
        low_shadow.width != source.buffers[static_cast<size_t>(
            RemoteBufferSemantic::RemoteShadowVisibility)].width ||
        low_shadow.height != source.buffers[static_cast<size_t>(
            RemoteBufferSemantic::RemoteShadowVisibility)].height)
    {
        SetError(error,
            "Low V3 reduced the authoritative primary shadow");
        return false;
    }

    std::array<RemoteBufferContentStateV3,
        static_cast<size_t>(RemoteBufferSemantic::Count)> retained_content = {};
    for (size_t index = 0; index < retained_content.size(); ++index)
    {
        retained_content[index].frame_id = 90 + index;
        retained_content[index].generation = source.metadata.source_generation;
        retained_content[index].confidence_unorm =
            static_cast<uint16_t>(60000 - index);
    }
    RemoteVideoFrameLayout retained_layout;
    if (!BuildRemoteVideoFrameLayoutV3(
            source, selection, 99, 0x1234, 3,
            retained_layout, metadata_luma, error, &retained_content))
        return false;
    if (retained_layout.layout_checksum != layout.layout_checksum ||
        retained_layout.descriptor_checksum == layout.descriptor_checksum)
    {
        SetError(error,
            "V3 layout checksum was not stable across content updates");
        return false;
    }
    for (size_t index = 0; index < retained_content.size(); ++index)
    {
        const RemoteBufferDescriptorV3& descriptor =
            retained_layout.contract_v3.descriptors[index];
        if (descriptor.content_frame_id != retained_content[index].frame_id ||
            descriptor.content_generation != retained_content[index].generation ||
            descriptor.confidence_unorm !=
                retained_content[index].confidence_unorm)
        {
            SetError(error,
                "V3 per-semantic retained content state was lost");
            return false;
        }
    }

    constexpr std::array<uint8_t, 7> scalar_ramp =
        {0, 1, 16, 64, 128, 224, 255};
    for (const uint8_t input : scalar_ramp)
    {
        uint8_t y_value = 0, u_value = 0, v_value = 0;
        RGBToYUV(input, input, input, y_value, u_value, v_value);
        uint8_t red = 0, green = 0, blue = 0;
        YUVToRGB(y_value, u_value, v_value, red, green, blue);
        const int max_error = std::max({
            std::abs(static_cast<int>(red) - input),
            std::abs(static_cast<int>(green) - input),
            std::abs(static_cast<int>(blue) - input)});
        if (max_error > 3 ||
            std::abs(static_cast<int>(u_value) - 128) > 1 ||
            std::abs(static_cast<int>(v_value) - 128) > 1)
        {
            SetError(error,
                "I420 scalar ramp exceeded the numerical tolerance");
            return false;
        }
    }

    std::vector<uint8_t> channel_bytes;
    RemoteVideoFrameLayout channel_roundtrip;
    if (!EncodeDownstreamFrameMetadataBytes(layout, channel_bytes) ||
        !DecodeDownstreamFrameMetadataBytes(
            channel_bytes.data(), channel_bytes.size(), channel_roundtrip) ||
        channel_roundtrip.protocol_version != kRemoteVideoWireVersionV3 ||
        channel_roundtrip.layout_checksum != layout.layout_checksum ||
        channel_roundtrip.descriptor_checksum != layout.descriptor_checksum ||
        channel_roundtrip.source_control_frame_id != layout.source_control_frame_id ||
        channel_roundtrip.metadata.frame_id != layout.metadata.frame_id ||
        channel_roundtrip.metadata.timestamp_usec !=
            layout.metadata.timestamp_usec ||
        channel_roundtrip.metadata.source_generation !=
            layout.metadata.source_generation ||
        channel_roundtrip.metadata.history_valid !=
            layout.metadata.history_valid ||
        channel_roundtrip.metadata.camera_cut !=
            layout.metadata.camera_cut ||
        channel_roundtrip.metadata.pre_exposure !=
            layout.metadata.pre_exposure ||
        channel_roundtrip.metadata.temporal_jitter_pixels.x !=
            layout.metadata.temporal_jitter_pixels.x ||
        channel_roundtrip.metadata.temporal_jitter_pixels.y !=
            layout.metadata.temporal_jitter_pixels.y ||
        channel_roundtrip.metadata.ddgi_frame_index !=
            layout.metadata.ddgi_frame_index ||
        channel_roundtrip.metadata.ddgi_reset_reason !=
            layout.metadata.ddgi_reset_reason ||
        std::memcmp(
            &channel_roundtrip.metadata.view_projection,
            &layout.metadata.view_projection,
            sizeof(XMFLOAT4X4)) != 0)
    {
        SetError(error, "V3 DataChannel metadata round trip failed");
        return false;
    }

    std::vector<uint8_t> y(
        static_cast<size_t>(retained_layout.video_width) *
            retained_layout.video_height,
        16u);
    for (uint32_t row = 0; row < retained_layout.metadata_rows; ++row)
    {
        std::memcpy(
            y.data() +
                static_cast<size_t>(row) * retained_layout.video_width,
            metadata_luma.data() +
                static_cast<size_t>(row) * retained_layout.video_width,
            retained_layout.video_width);
    }
    std::vector<uint8_t> u(
        static_cast<size_t>(retained_layout.video_width / 2u) *
            (retained_layout.video_height / 2u),
        128u);
    std::vector<uint8_t> v = u;
    RetainedI420Frame retained;
    retained.width = retained_layout.video_width;
    retained.height = retained_layout.video_height;
    retained.y_plane = y.data();
    retained.y_stride = retained_layout.video_width;
    retained.u_plane = u.data();
    retained.u_stride = retained_layout.video_width / 2u;
    retained.v_plane = v.data();
    retained.v_stride = retained_layout.video_width / 2u;
    retained.frame_lifetime = std::make_shared<int>(1);
    RemoteVideoFrameLayout pixel_roundtrip;
    if (!DecodeRemoteVideoFrameLayout(retained, pixel_roundtrip, error) ||
        pixel_roundtrip.protocol_version != kRemoteVideoWireVersionV3 ||
        pixel_roundtrip.metadata.frame_id != retained_layout.metadata.frame_id ||
        pixel_roundtrip.metadata.source_generation !=
            retained_layout.metadata.source_generation ||
        pixel_roundtrip.descriptor_checksum !=
            retained_layout.descriptor_checksum ||
        pixel_roundtrip.source_control_frame_id !=
            retained_layout.source_control_frame_id)
    {
        SetError(error, "V3 pixel-band identity round trip failed");
        return false;
    }
    channel_bytes.back() ^= 1u;
    if (DecodeDownstreamFrameMetadataBytes(
            channel_bytes.data(), channel_bytes.size(), channel_roundtrip))
    {
        SetError(error, "corrupt V3 frame metadata was accepted");
        return false;
    }
    return true;
}

const char* ToString(WebRTCTransportState state)
{
    switch (state)
    {
    case WebRTCTransportState::Disabled: return "Disabled";
    case WebRTCTransportState::Starting: return "Starting";
    case WebRTCTransportState::Signaling: return "Signaling";
    case WebRTCTransportState::Connected: return "Connected";
    case WebRTCTransportState::Failed: return "Failed";
    default: return "Unknown";
    }
}

namespace
{
bool EncodeDownstreamFrameMetadataBytes(
    const RemoteVideoFrameLayout& layout, std::vector<uint8_t>& bytes)
{
    if (layout.protocol_version != kRemoteVideoWireVersionV3 ||
        layout.layout_checksum == 0 || layout.descriptor_checksum == 0 ||
        !ValidateRemoteFrameContractV3(layout.contract_v3, nullptr) ||
        layout.encoding_profile_id !=
            layout.contract_v3.encoding_profile_id ||
        layout.quality_tier != layout.contract_v3.quality_tier ||
        layout.source_control_frame_id !=
            layout.contract_v3.source_control_frame_id ||
        layout.video_width != layout.contract_v3.atlas_width ||
        layout.video_height != layout.contract_v3.atlas_height)
        return false;

    std::vector<uint8_t> metadata_bytes;
    std::vector<uint8_t> contract_bytes;
    uint32_t computed_layout_checksum = 0;
    if (!EncodeRemoteFrameMetadataV3(
            layout.metadata, metadata_bytes) ||
        !SerializeRemoteFrameContractV3(
            layout.contract_v3, contract_bytes, nullptr) ||
        !ComputeAtlasLayoutChecksum(
            layout.contract_v3, computed_layout_checksum) ||
        layout.layout_checksum != computed_layout_checksum ||
        layout.descriptor_checksum != Fnv1a32(contract_bytes.data(), contract_bytes.size()))
        return false;

    bytes.clear();
    bytes.reserve(
        kV3FrameMetadataHeaderBytes +
        metadata_bytes.size() +
        contract_bytes.size());
    AppendU32(bytes, kV3FrameMetadataMagic);
    AppendU32(bytes, kRemoteVideoWireVersionV3);
    AppendU32(bytes, 0);
    AppendU32(bytes, 0);
    AppendU32(bytes, layout.descriptor_checksum);
    AppendU32(bytes, static_cast<uint32_t>(metadata_bytes.size()));
    AppendU32(bytes, static_cast<uint32_t>(contract_bytes.size()));
    AppendU32(bytes, layout.layout_checksum);
    bytes.insert(bytes.end(), metadata_bytes.begin(), metadata_bytes.end());
    bytes.insert(bytes.end(), contract_bytes.begin(), contract_bytes.end());
    StoreU32(bytes, 8, static_cast<uint32_t>(bytes.size()));
    StoreU32(bytes, 12, Fnv1a32(bytes.data() + 16, bytes.size() - 16));
    return true;
}

bool DecodeDownstreamFrameMetadataBytes(
    const uint8_t* bytes, size_t byte_count, RemoteVideoFrameLayout& layout)
{
    layout = {};
    if (bytes == nullptr ||
        byte_count < kV3FrameMetadataHeaderBytes)
        return false;

    const uint8_t* cursor = bytes;
    const uint8_t* end = bytes + byte_count;
    uint32_t magic = 0, version = 0, encoded_size = 0, checksum = 0;
    uint32_t descriptor_checksum = 0, metadata_size = 0, contract_size = 0;
    uint32_t layout_checksum = 0;
    if (!ConsumeU32(cursor, end, magic) || !ConsumeU32(cursor, end, version) ||
        !ConsumeU32(cursor, end, encoded_size) || !ConsumeU32(cursor, end, checksum) ||
        !ConsumeU32(cursor, end, descriptor_checksum) || !ConsumeU32(cursor, end, metadata_size) ||
        !ConsumeU32(cursor, end, contract_size) || !ConsumeU32(cursor, end, layout_checksum) ||
        magic != kV3FrameMetadataMagic ||
        version != kRemoteVideoWireVersionV3 ||
        encoded_size != byte_count || layout_checksum == 0 || descriptor_checksum == 0 ||
        checksum != Fnv1a32(bytes + 16, byte_count - 16) ||
        metadata_size == 0 || contract_size == 0 ||
        static_cast<size_t>(end - cursor) !=
            static_cast<size_t>(metadata_size) + contract_size)
        return false;

    RemoteFrameMetadata metadata;
    if (!DecodeRemoteFrameMetadataV3(
            cursor, metadata_size, metadata))
        return false;
    cursor += metadata_size;
    RemoteVideoFrameLayout decoded;
    RemoteFrameContractV3 contract;
    uint32_t computed_layout_checksum = 0;
    if (!DeserializeRemoteFrameContractV3(
            cursor, contract_size, contract, nullptr) ||
        !ComputeAtlasLayoutChecksum(contract, computed_layout_checksum) ||
        layout_checksum != computed_layout_checksum ||
        descriptor_checksum != Fnv1a32(cursor, contract_size) ||
        contract.source_control_frame_id == 0)
        return false;

    decoded.metadata = std::move(metadata);
    decoded.protocol_version = kRemoteVideoWireVersionV3;
    decoded.encoding_profile_id = contract.encoding_profile_id;
    decoded.quality_tier = contract.quality_tier;
    decoded.source_control_frame_id = contract.source_control_frame_id;
    decoded.layout_checksum = layout_checksum;
    decoded.descriptor_checksum = descriptor_checksum;
    decoded.video_width = contract.atlas_width;
    decoded.video_height = contract.atlas_height;
    decoded.metadata_rows = MetadataRowsForBytes(decoded.video_width, kV3PixelBandBytes);
    if (decoded.metadata_rows == 0 ||
        decoded.metadata_rows >= decoded.video_height)
        return false;
    decoded.contract_v3 = contract;
    uint32_t available_mask = 0;
    for (const RemoteBufferDescriptorV3& descriptor : contract.descriptors)
    {
        const size_t index = static_cast<size_t>(descriptor.semantic);
        if (index >= decoded.tiles.size())
            return false;
        RemoteVideoTileLayout& tile = decoded.tiles[index];
        tile.semantic = descriptor.semantic;
        tile.width = descriptor.atlas_width;
        tile.height = descriptor.atlas_height;
        tile.origin_x = descriptor.atlas_x;
        tile.origin_y = descriptor.atlas_y;
        tile.available = (descriptor.flags & kRemoteBufferDescriptorAvailableV3) != 0;
        tile.encoding = descriptor.encoding;
        if (tile.available)
            available_mask |= RemoteBufferKindMask(tile.semantic);
    }
    if (available_mask != decoded.metadata.available_buffer_mask ||
        (decoded.metadata.continuity_mask & available_mask) !=
            available_mask)
        return false;
    layout = std::move(decoded);
    return true;
}
} // namespace

struct WebRTCVideoTransport::Impl
{
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    RuntimeConfig requested_config;
    uint64_t requested_adapter_luid = 0;
    uint64_t request_revision = 0;
    bool requested_server = false;
    bool desired_running = false;
    bool shutdown = false;

    // Shared operations pin the bridge while its independently synchronized
    // channels run concurrently. Only attach/detach requires exclusive access;
    // a signaling BlockingCall must never exclude render-thread receives.
    std::shared_mutex bridge_mutex;
    // The C bridge's two-call receive API has per-stream staging storage.
    std::mutex control_receive_mutex;
    std::mutex metadata_receive_mutex;
    std::mutex video_receive_mutex;
    NPWebRTCBridge* bridge = nullptr;
    bool server = false;
    std::atomic_bool bridge_attached{false};

    std::thread lifecycle_thread;
    std::shared_ptr<const WebRTCTransportStats> stats_snapshot =
        std::make_shared<const WebRTCTransportStats>();
    std::atomic<uint64_t> connection_attempts{0};
    std::atomic<uint64_t> disconnected_frame_drops{0};
    std::atomic<uint64_t> busy_frame_drops{0};
    std::atomic<uint64_t> disconnected_control_drops{0};
    std::atomic<uint64_t> queued_control_drops{0};
    std::atomic<uint64_t> lifecycle_last_duration_usec{0};
    std::atomic<uint64_t> cpu_full_frame_copy_bytes{0};
    std::atomic<uint64_t> cpu_conversion_usec{0};
    std::atomic<uint64_t> retained_frame_acquires{0};
    std::atomic<uint64_t> retained_i420_bytes{0};
    std::atomic<uint64_t> control_receive_busy{0};
    std::atomic<uint64_t> metadata_receive_busy{0};
    std::atomic<uint64_t> video_receive_busy{0};
    std::atomic<uint64_t> control_send_last_usec{0};
    std::atomic<uint64_t> control_send_max_usec{0};
    std::mutex control_queue_mutex;
    ClientControlPacket pending_control = {};
    std::atomic_bool control_pending{false};

    static WebRTCTransportStats ReadNativeStats(NPWebRTCBridge* bridge)
    {
        WebRTCTransportStats result;
        if (bridge == nullptr)
            return result;
        NPWebRTCBridgeStats native = {};
        np_webrtc_bridge_get_stats(bridge, &native);
        switch (native.state)
        {
        case NP_WEBRTC_STARTING: result.state = WebRTCTransportState::Starting; break;
        case NP_WEBRTC_SIGNALING: result.state = WebRTCTransportState::Signaling; break;
        case NP_WEBRTC_CONNECTED: result.state = WebRTCTransportState::Connected; break;
        case NP_WEBRTC_FAILED: result.state = WebRTCTransportState::Failed; break;
        case NP_WEBRTC_DISABLED:
        default: result.state = WebRTCTransportState::Disabled; break;
        }
        result.sent_frames = native.sent_frames;
        result.received_frames = native.received_frames;
        result.dropped_frames = native.dropped_frames;
        result.decoded_queue_depth = native.decoded_queue_depth;
        result.sent_controls = native.sent_controls;
        result.received_controls = native.received_controls;
        result.compressed_bytes_sent = native.compressed_bytes_sent;
        result.compressed_bytes_received = native.compressed_bytes_received;
        result.total_encode_time_usec = native.total_encode_time_usec;
        result.total_decode_time_usec = native.total_decode_time_usec;
        result.frames_encoded = native.frames_encoded;
        result.frames_decoded = native.frames_decoded;
        result.codec_name = native.codec_name;
        result.codec_implementation = native.codec_implementation;
        result.power_efficient_codec = native.power_efficient_codec != 0;
        result.requested_encoder_mode = native.requested_encoder_mode;
        result.active_encoder_mode = native.active_encoder_mode;
        result.input_surface = native.input_surface;
        result.native_codec = result.input_surface.find("retained") !=
            std::string::npos;
        result.codec_profile = native.codec_profile;
        result.fallback_reason = native.fallback_reason;
        result.status = native.status;
        return result;
    }

    void PublishStats(WebRTCTransportStats stats)
    {
        if (stats.state != WebRTCTransportState::Connected)
        {
            std::lock_guard lock(control_queue_mutex);
            pending_control = {};
            control_pending.store(false, std::memory_order_release);
        }
        stats.connection_attempts = connection_attempts.load(std::memory_order_relaxed);
        stats.disconnected_frame_drops = disconnected_frame_drops.load(std::memory_order_relaxed);
        stats.busy_frame_drops = busy_frame_drops.load(std::memory_order_relaxed);
        stats.disconnected_control_drops = disconnected_control_drops.load(std::memory_order_relaxed);
        stats.queued_control_drops = queued_control_drops.load(std::memory_order_relaxed);
        stats.lifecycle_last_duration_usec = lifecycle_last_duration_usec.load(std::memory_order_relaxed);
        stats.cpu_full_frame_copy_bytes = cpu_full_frame_copy_bytes.load(std::memory_order_relaxed);
        stats.cpu_conversion_usec = cpu_conversion_usec.load(std::memory_order_relaxed);
        stats.retained_frame_acquires = retained_frame_acquires.load(std::memory_order_relaxed);
        stats.retained_i420_bytes = retained_i420_bytes.load(std::memory_order_relaxed);
        stats.control_receive_busy = control_receive_busy.load(std::memory_order_relaxed);
        stats.metadata_receive_busy = metadata_receive_busy.load(std::memory_order_relaxed);
        stats.video_receive_busy = video_receive_busy.load(std::memory_order_relaxed);
        stats.control_send_last_usec = control_send_last_usec.load(std::memory_order_relaxed);
        stats.control_send_max_usec = control_send_max_usec.load(std::memory_order_relaxed);
        std::atomic_store(&stats_snapshot,
            std::make_shared<const WebRTCTransportStats>(std::move(stats)));
    }

    NPWebRTCBridge* DetachBridge()
    {
        std::unique_lock lock(bridge_mutex);
        NPWebRTCBridge* detached = bridge;
        bridge = nullptr;
        bridge_attached.store(false, std::memory_order_release);
        {
            std::lock_guard control_lock(control_queue_mutex);
            pending_control = {};
            control_pending.store(false, std::memory_order_release);
        }
        return detached;
    }

    bool HasSendableControl() const
    {
        return control_pending.load(std::memory_order_acquire) &&
            bridge_attached.load(std::memory_order_acquire) &&
            std::atomic_load(&stats_snapshot)->state == WebRTCTransportState::Connected;
    }

    void DestroyBridge()
    {
        NPWebRTCBridge* detached = DetachBridge();
        if (detached == nullptr)
            return;
        const uint64_t begin = NowUsec();
        np_webrtc_bridge_destroy(detached);
        lifecycle_last_duration_usec.store(NowUsec() - begin, std::memory_order_relaxed);
    }

    static std::chrono::milliseconds RetryDelay(uint32_t retry_attempt, uint64_t revision)
    {
        const uint32_t exponent = std::min(retry_attempt, 4u);
        const uint32_t base_ms = std::min(2000u << exponent, 30000u);
        // Deterministic 80-120% jitter avoids Client and Server retrying in lockstep.
        const uint32_t hash = static_cast<uint32_t>((revision * 1103515245ull + retry_attempt * 12345ull) >> 16u);
        const uint32_t percent = 80u + hash % 41u;
        return std::chrono::milliseconds(base_ms * percent / 100u);
    }

    void LifecycleLoop()
    {
        uint64_t applied_revision = 0;
        uint32_t retry_attempt = 0;
        bool force_software_fallback = false;
        auto next_retry = std::chrono::steady_clock::now();

        const auto apply_encoder_request = [&](WebRTCTransportStats& stats,
            bool role_server, const RuntimeConfig& config) {
            if (!role_server)
                return;
            stats.requested_encoder_mode = ToString(config.remote_encoder);
            if (config.remote_encoder == RemoteEncoderPreference::Hardware &&
                force_software_fallback)
            {
                stats.active_encoder_mode = "fallback-software";
                stats.fallback_reason = "hardware-runtime-failed";
            }
        };

        for (;;)
        {
            RuntimeConfig config;
            uint64_t revision = 0;
            bool run = false;
            bool role_server = false;
            uint64_t adapter_luid = 0;
            {
                std::unique_lock lock(lifecycle_mutex);
                const auto wake_time = bridge_attached.load(std::memory_order_acquire)
                    ? std::chrono::steady_clock::now() + std::chrono::milliseconds(100)
                    : next_retry;
                lifecycle_cv.wait_until(lock, wake_time, [&] {
                    return shutdown || request_revision != applied_revision ||
                        HasSendableControl();
                });
                if (shutdown)
                    break;
                revision = request_revision;
                run = desired_running;
                role_server = requested_server;
                adapter_luid = requested_adapter_luid;
                config = requested_config;
            }

            if (revision != applied_revision)
            {
                DestroyBridge();
                applied_revision = revision;
                retry_attempt = 0;
                force_software_fallback = false;
                next_retry = std::chrono::steady_clock::now();
            }

            if (!run)
            {
                DestroyBridge();
                WebRTCTransportStats disabled;
                disabled.status = "WebRTC disabled";
                PublishStats(std::move(disabled));
                std::unique_lock lock(lifecycle_mutex);
                lifecycle_cv.wait(lock, [&] {
                    return shutdown || request_revision != applied_revision;
                });
                continue;
            }

            NPWebRTCBridge* current = nullptr;
            {
                std::shared_lock lock(bridge_mutex);
                current = bridge;
            }
            if (current != nullptr)
            {
                WebRTCTransportStats stats;
                {
                    std::shared_lock lock(bridge_mutex);
                    stats = ReadNativeStats(bridge);
                }
                apply_encoder_request(stats, role_server, config);
                if (!role_server && stats.state == WebRTCTransportState::Connected &&
                    control_pending.load(std::memory_order_acquire))
                {
                    ClientControlPacket packet;
                    {
                        std::lock_guard lock(control_queue_mutex);
                        packet = pending_control;
                        control_pending.store(false, std::memory_order_release);
                    }
                    std::shared_lock lock(bridge_mutex);
                    if (bridge != nullptr && !server)
                    {
                        std::vector<uint8_t> bytes;
                        if (SerializeClientControlPacket(packet, bytes, nullptr))
                        {
                            const uint64_t begin = NowUsec();
                            np_webrtc_bridge_send_control(bridge, bytes.data(), bytes.size());
                            const uint64_t elapsed = NowUsec() - begin;
                            control_send_last_usec.store(elapsed, std::memory_order_relaxed);
                            control_send_max_usec.store(std::max(elapsed,
                                control_send_max_usec.load(std::memory_order_relaxed)),
                                std::memory_order_relaxed);
                        }
                    }
                }
                PublishStats(stats);
                if (stats.state != WebRTCTransportState::Failed)
                    continue;

                const bool switch_to_software = role_server &&
                    config.remote_encoder == RemoteEncoderPreference::Hardware &&
                    !force_software_fallback &&
                    stats.fallback_reason == "hardware-runtime-failed";
                DestroyBridge();
                if (switch_to_software)
                {
                    force_software_fallback = true;
                    retry_attempt = 0;
                }
                const auto delay = switch_to_software
                    ? std::chrono::milliseconds(0)
                    : RetryDelay(retry_attempt++, applied_revision);
                next_retry = std::chrono::steady_clock::now() + delay;
                stats.status += switch_to_software
                    ? "; restarting with software encoder"
                    : "; retry in " + std::to_string(delay.count()) + " ms";
                PublishStats(std::move(stats));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < next_retry)
            {
                std::unique_lock lock(lifecycle_mutex);
                lifecycle_cv.wait_until(lock, next_retry, [&] {
                    return shutdown || request_revision != applied_revision;
                });
                continue;
            }

            WebRTCTransportStats starting;
            starting.state = WebRTCTransportState::Starting;
            starting.status = "Starting WebRTC on RTC service thread";
            PublishStats(std::move(starting));
            connection_attempts.fetch_add(1, std::memory_order_relaxed);
            const uint64_t begin = NowUsec();
            NPWebRTCBridge* candidate = np_webrtc_bridge_create(
                role_server ? 1 : 0,
                config.signaling_url.c_str(),
                config.room_id.c_str(),
                config.use_internet_ice ? 1 : 0,
                config.remote_encoder == RemoteEncoderPreference::Software ||
                    force_software_fallback
                    ? NP_WEBRTC_ENCODER_SOFTWARE
                    : NP_WEBRTC_ENCODER_HARDWARE,
                adapter_luid);
            lifecycle_last_duration_usec.store(NowUsec() - begin, std::memory_order_relaxed);

            bool discard_candidate = false;
            bool stop_after_discard = false;
            {
                std::lock_guard lock(lifecycle_mutex);
                if (shutdown || request_revision != applied_revision || !desired_running)
                {
                    discard_candidate = true;
                    stop_after_discard = shutdown;
                }
            }
            if (discard_candidate)
            {
                if (candidate != nullptr)
                    np_webrtc_bridge_destroy(candidate);
                if (stop_after_discard)
                    break;
                continue;
            }

            WebRTCTransportStats stats = ReadNativeStats(candidate);
            apply_encoder_request(stats, role_server, config);
            if (candidate == nullptr || stats.state == WebRTCTransportState::Failed)
            {
                if (candidate != nullptr)
                    np_webrtc_bridge_destroy(candidate);
                const auto delay = RetryDelay(retry_attempt++, applied_revision);
                next_retry = std::chrono::steady_clock::now() + delay;
                stats.state = WebRTCTransportState::Failed;
                if (stats.status.empty())
                    stats.status = "Could not create native WebRTC transport";
                stats.status += "; retry in " + std::to_string(delay.count()) + " ms";
                PublishStats(std::move(stats));
                continue;
            }

            {
                std::unique_lock lock(bridge_mutex);
                bridge = candidate;
                server = role_server;
                bridge_attached.store(true, std::memory_order_release);
            }
            retry_attempt = 0;
            PublishStats(std::move(stats));
        }

        DestroyBridge();
        WebRTCTransportStats disabled;
        disabled.status = "WebRTC stopped";
        PublishStats(std::move(disabled));
    }
};

bool ValidateRemoteTransportLifecycleSelfTest(std::string* error)
{
    WebRTCVideoTransport::Impl state;
    // Hold the production send-side lifetime lock while simulating a stalled
    // signaling call. Receivers must still acquire it, and detach must wait.
    std::promise<void> send_entered;
    std::promise<void> release_send;
    auto release = release_send.get_future();
    std::thread sender([&] {
        std::shared_lock lock(state.bridge_mutex);
        send_entered.set_value();
        release.wait();
    });
    send_entered.get_future().wait();
    bool receive_ready = false;
    bool detach_excluded = false;
    {
        std::shared_lock receive(state.bridge_mutex, std::try_to_lock);
        receive_ready = receive.owns_lock();
    }
    {
        std::unique_lock detach(state.bridge_mutex, std::try_to_lock);
        detach_excluded = !detach.owns_lock();
    }
    auto detached = std::async(std::launch::async, [&] { return state.DetachBridge(); });
    const bool detach_waited = detached.wait_for(std::chrono::milliseconds(10)) ==
        std::future_status::timeout;
    release_send.set_value();
    sender.join();
    detached.get();
    if (!receive_ready || !detach_excluded || !detach_waited)
    {
        SetError(error, "bridge lifetime lock blocked receive or failed to pin an active send");
        return false;
    }
    WebRTCTransportStats connected;
    connected.state = WebRTCTransportState::Connected;
    state.PublishStats(connected);
    state.bridge_attached.store(true);
    state.control_pending.store(true);
    if (!state.HasSendableControl())
    {
        SetError(error, "connected control did not wake the service");
        return false;
    }
    WebRTCTransportStats disconnected;
    disconnected.state = WebRTCTransportState::Signaling;
    state.PublishStats(disconnected);
    if (state.control_pending.load())
    {
        SetError(error, "disconnect retained a previous-session control");
        return false;
    }
    // Model SendControl racing a stale Connected stats snapshot. Even if it
    // queues after disconnect, the waiting thread must respect its deadline.
    state.control_pending.store(true);
    std::unique_lock lock(state.lifecycle_mutex);
    if (state.lifecycle_cv.wait_until(lock,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(10),
            [&] { return state.HasSendableControl(); }))
    {
        SetError(error, "disconnected pending control bypassed service wait");
        return false;
    }
    state.DetachBridge();
    if (state.control_pending.load() || state.HasSendableControl())
    {
        SetError(error, "detached session retained a control wakeup");
        return false;
    }
    return true;
}

WebRTCVideoTransport::WebRTCVideoTransport() : impl(std::make_unique<Impl>())
{
    impl->lifecycle_thread = std::thread([this] { impl->LifecycleLoop(); });
}

WebRTCVideoTransport::~WebRTCVideoTransport()
{
    Stop();
}

bool WebRTCVideoTransport::RequestStart(
    bool server,
    const RuntimeConfig& config,
    uint64_t adapter_luid,
    std::string* error)
{
    if (!impl || config.signaling_url.empty() || config.room_id.empty())
    {
        SetError(error, "signaling URL and room ID must not be empty");
        return false;
    }
    {
        std::lock_guard lock(impl->lifecycle_mutex);
        impl->requested_server = server;
        impl->requested_config = config;
        impl->requested_adapter_luid = adapter_luid;
        impl->desired_running = true;
        ++impl->request_revision;
    }
    WebRTCTransportStats starting;
    starting.state = WebRTCTransportState::Starting;
    starting.status = "WebRTC start requested";
    impl->PublishStats(std::move(starting));
    impl->lifecycle_cv.notify_one();
    return true;
}

void WebRTCVideoTransport::RequestStop()
{
    if (!impl)
        return;
    {
        std::lock_guard lock(impl->lifecycle_mutex);
        impl->desired_running = false;
        ++impl->request_revision;
    }
    WebRTCTransportStats disabled;
    disabled.status = "WebRTC stop requested";
    impl->PublishStats(std::move(disabled));
    impl->lifecycle_cv.notify_one();
}

void WebRTCVideoTransport::Stop()
{
    if (!impl)
        return;
    {
        std::lock_guard lock(impl->lifecycle_mutex);
        impl->desired_running = false;
        impl->shutdown = true;
        ++impl->request_revision;
    }
    impl->lifecycle_cv.notify_one();
    if (impl->lifecycle_thread.joinable())
        impl->lifecycle_thread.join();
}

void WebRTCVideoTransport::Tick()
{
    // Native libwebrtc and the signaling WebSocket own their worker threads.
}

bool WebRTCVideoTransport::SendControl(const ClientControlPacket& packet)
{
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
    {
        if (impl)
            impl->disconnected_control_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    {
        std::lock_guard lock(impl->control_queue_mutex);
        if (impl->control_pending.load(std::memory_order_relaxed))
            impl->queued_control_drops.fetch_add(1, std::memory_order_relaxed);
        impl->pending_control = packet;
        impl->control_pending.store(true, std::memory_order_release);
    }
    impl->lifecycle_cv.notify_one();
    return true;
}

bool WebRTCVideoTransport::TryReceiveControl(ClientControlPacket& packet)
{
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::unique_lock receive_lock(impl->control_receive_mutex, std::try_to_lock);
    if (!receive_lock.owns_lock())
    {
        impl->control_receive_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::shared_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        impl->control_receive_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (impl->bridge == nullptr || !impl->server)
        return false;
    size_t required = 0;
    int result = np_webrtc_bridge_receive_control(impl->bridge, nullptr, 0, &required);
    if (result == 0 || required == 0 || required > 4096)
        return false;
    std::vector<uint8_t> bytes(required);
    result = np_webrtc_bridge_receive_control(
        impl->bridge, bytes.data(), bytes.size(), &required);
    if (result != 1 || required != bytes.size())
        return false;
    return DeserializeClientControlPacket(
        bytes.data(), bytes.size(), packet, nullptr);
}

bool WebRTCVideoTransport::SendI420Frame(const RetainedI420Frame& frame)
{
    if (!impl || !frame.IsValid() || !frame.frame_lifetime ||
        GetStats().state != WebRTCTransportState::Connected)
    {
        if (impl)
            impl->disconnected_frame_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // This API is called by the dedicated latest-only publication worker, not
    // the render thread. A short stats/lifecycle lock collision is therefore
    // backpressure, not a failed frame, and can safely wait for the bridge.
    std::shared_lock lock(impl->bridge_mutex);
    if (impl->bridge == nullptr || !impl->server)
    {
        impl->disconnected_frame_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto* lifetime = new std::shared_ptr<void>(frame.frame_lifetime);
    return np_webrtc_bridge_send_i420_planes(
        impl->bridge,
        frame.width,
        frame.height,
        frame.y_plane,
        frame.y_stride,
        frame.u_plane,
        frame.u_stride,
        frame.v_plane,
        frame.v_stride,
        frame.timestamp_usec,
        [](void* context) {
            delete static_cast<std::shared_ptr<void>*>(context);
        },
        lifetime) == 1;
}

bool WebRTCVideoTransport::SendNV12Frame(const RetainedNV12Frame& frame)
{
    if (!impl || !frame.IsValid() ||
        GetStats().state != WebRTCTransportState::Connected)
    {
        if (impl)
            impl->disconnected_frame_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Native publication uses the same worker as I420 publication, so it can
    // wait out the brief bridge stats lock without touching frame cadence on
    // the render thread.
    std::shared_lock lock(impl->bridge_mutex);
    if (impl->bridge == nullptr || !impl->server ||
        np_webrtc_bridge_supports_native_nv12(impl->bridge) != 1)
    {
        impl->disconnected_frame_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    struct Lifetime
    {
        std::shared_ptr<void> owner;
        void (*completion_callback)(void*) = nullptr;
        void* completion_context = nullptr;
    };
    auto* lifetime = new Lifetime{
        frame.frame_lifetime,
        frame.mark_completion_scheduled,
        frame.completion_context};
    return np_webrtc_bridge_send_nv12_surface(
        impl->bridge,
        frame.width,
        frame.height,
        frame.texture_shared_handle,
        frame.fence_shared_handle,
        frame.producer_fence_value,
        frame.consumer_fence_value,
        frame.adapter_luid,
        frame.rtp_timestamp,
        frame.timestamp_usec,
        [](void* context) { delete static_cast<Lifetime*>(context); },
        lifetime,
        [](void* context) {
            auto* value = static_cast<Lifetime*>(context);
            if (value != nullptr && value->completion_callback != nullptr)
                value->completion_callback(value->completion_context);
        },
        lifetime) == 1;
}

bool WebRTCVideoTransport::SendFrameMetadata(const RemoteVideoFrameLayout& layout)
{
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::vector<uint8_t> bytes;
    if (!EncodeDownstreamFrameMetadataBytes(layout, bytes))
        return false;
    // Keep video and its exact identity metadata on the same serialized
    // publication path. The latest-only queue already bounds upstream delay.
    std::shared_lock lock(impl->bridge_mutex);
    if (impl->bridge == nullptr || !impl->server)
        return false;
    return np_webrtc_bridge_send_frame_metadata(
        impl->bridge, bytes.data(), bytes.size()) == 1;
}

bool WebRTCVideoTransport::SendStreamStatus(const RemoteStreamStatus& status)
{
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::vector<uint8_t> bytes;
    if (!SerializeRemoteStreamStatus(status, bytes, nullptr))
        return false;
    std::shared_lock lock(impl->bridge_mutex);
    if (impl->bridge == nullptr || !impl->server)
        return false;
    return np_webrtc_bridge_send_frame_metadata(
        impl->bridge, bytes.data(), bytes.size()) == 1;
}

bool WebRTCVideoTransport::RequestKeyframe()
{
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    // This runs on the Server publication worker, never the RenderPath thread.
    // Execute synchronously so the encoder observes the keyframe request before
    // the first frame carrying a new generation/layout is submitted.
    std::shared_lock lock(impl->bridge_mutex);
    return impl->bridge != nullptr && impl->server &&
        np_webrtc_bridge_request_keyframe(impl->bridge) == 1;
}

bool WebRTCVideoTransport::TryReceiveFrameMetadata(
    RemoteVideoFrameLayout& layout,
    RemoteStreamStatus* stream_status)
{
    layout = {};
    if (stream_status != nullptr)
        *stream_status = {};
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::unique_lock receive_lock(impl->metadata_receive_mutex, std::try_to_lock);
    if (!receive_lock.owns_lock())
    {
        impl->metadata_receive_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::shared_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        impl->metadata_receive_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (impl->bridge == nullptr || impl->server)
        return false;
    size_t required = 0;
    int result = np_webrtc_bridge_receive_frame_metadata(impl->bridge, nullptr, 0, &required);
    if (result == 0 || required == 0 || required > 64u * 1024u)
        return false;
    std::vector<uint8_t> bytes(required);
    result = np_webrtc_bridge_receive_frame_metadata(
        impl->bridge, bytes.data(), bytes.size(), &required);
    if (result != 1 || required != bytes.size())
        return false;
    if (DecodeDownstreamFrameMetadataBytes(bytes.data(), bytes.size(), layout))
        return true;
    RemoteStreamStatus decoded_status;
    if (stream_status != nullptr &&
        DeserializeRemoteStreamStatus(
            bytes.data(), bytes.size(), decoded_status, nullptr))
    {
        *stream_status = decoded_status;
    }
    return false;
}

bool WebRTCVideoTransport::TryAcquireI420Frame(RetainedI420Frame& frame)
{
    RetainedRemoteVideoFrame remote;
    if (!TryAcquireVideoFrame(remote) || !remote.i420.IsValid())
    {
        frame = {};
        return false;
    }
    frame = std::move(remote.i420);
    return true;
}

bool WebRTCVideoTransport::TryAcquireVideoFrame(RetainedRemoteVideoFrame& frame)
{
    frame = {};
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::unique_lock receive_lock(impl->video_receive_mutex, std::try_to_lock);
    if (!receive_lock.owns_lock())
    {
        impl->video_receive_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::shared_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        impl->video_receive_busy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (impl->bridge == nullptr || impl->server)
        return false;
    NPWebRTCVideoFrame* retained = nullptr;
    if (np_webrtc_bridge_acquire_video_frame(impl->bridge, &retained) != 1 || retained == nullptr)
        return false;
    std::shared_ptr<void> lifetime(retained, [](void* value) {
        np_webrtc_video_frame_release(static_cast<NPWebRTCVideoFrame*>(value));
    });
    RetainedNV12Frame nv12;
    nv12.frame_lifetime = lifetime;
    if (np_webrtc_video_frame_get_nv12_surface(
            retained,
            &nv12.width,
            &nv12.height,
            &nv12.texture_shared_handle,
            &nv12.fence_shared_handle,
            &nv12.producer_fence_value,
            &nv12.consumer_fence_value,
            &nv12.adapter_luid,
            &nv12.rtp_timestamp,
            &nv12.timestamp_usec) == 1 &&
        nv12.IsValid())
    {
        nv12.mark_completion_scheduled = [](void* context) {
            np_webrtc_video_frame_mark_native_completion_scheduled(
                static_cast<NPWebRTCVideoFrame*>(context));
        };
        nv12.completion_context = retained;
        frame.nv12 = std::move(nv12);
        impl->retained_frame_acquires.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    RetainedI420Frame i420;
    i420.frame_lifetime = std::move(lifetime);
    if (np_webrtc_video_frame_get_i420(
            retained,
            &i420.width,
            &i420.height,
            &i420.y_plane,
            &i420.y_stride,
            &i420.u_plane,
            &i420.u_stride,
            &i420.v_plane,
            &i420.v_stride,
            &i420.timestamp_usec) != 1 ||
        !i420.IsValid())
    {
        frame = {};
        return false;
    }
    const uint64_t y_bytes = static_cast<uint64_t>(i420.width) * i420.height;
    const uint64_t uv_bytes = static_cast<uint64_t>(i420.width / 2u) * (i420.height / 2u);
    impl->retained_frame_acquires.fetch_add(1, std::memory_order_relaxed);
    impl->retained_i420_bytes.fetch_add(y_bytes + uv_bytes * 2u, std::memory_order_relaxed);
    frame.i420 = std::move(i420);
    return true;
}

bool WebRTCVideoTransport::SupportsNativeNV12() const
{
    if (!impl)
        return false;
    // The render thread must not interpret a transient bridge-mutex collision
    // as a codec capability change. The lifecycle thread already publishes an
    // atomic stats snapshot after querying the bridge; native_codec is true
    // only for the confirmed retained NV12 surface path.
    const WebRTCTransportStats stats = GetStats();
    return stats.state == WebRTCTransportState::Connected &&
        stats.native_codec;
}

void WebRTCVideoTransport::ReportNativeNV12Failure()
{
    if (!impl)
        return;
    // This is a one-shot error path. Use a blocking lock so the notification
    // cannot be silently lost to an ordinary signaling/stats lock collision.
    std::shared_lock lock(impl->bridge_mutex);
    if (impl->bridge != nullptr && !impl->server)
        np_webrtc_bridge_report_native_nv12_failure(impl->bridge);
}

WebRTCTransportStats WebRTCVideoTransport::GetStats() const
{
    if (!impl)
        return {};
    const std::shared_ptr<const WebRTCTransportStats> snapshot = std::atomic_load(&impl->stats_snapshot);
    return snapshot ? *snapshot : WebRTCTransportStats{};
}
} // namespace wicked_newpipeline
