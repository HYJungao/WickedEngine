#include "NewPipelineTransport.h"
#include "NewPipelineWebRTCBridge.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>

namespace wicked_newpipeline
{
namespace
{
constexpr const char* kLatestRemoteVideoFile = "latest.npv";
constexpr const char* kLatestControlFile = "latest.control";
constexpr size_t kRetainedRemotePayloadCount = 3;
constexpr uint64_t kMaxControlPacketAgeUsec = 2'000'000;
// Individual render targets may exceed 2K (the Sponza validation path is
// 2560x1440). The packed 2x2 video frame is independently capped by the
// WebRTC bridge at 8192 pixels per dimension.
constexpr uint32_t kMaxRemoteBufferDimension = 4096;
constexpr uint32_t kMetadataBitCellSize = 4;
constexpr uint32_t kTilePadding = 16;
constexpr float kHDRTransportMaximum = 16.0f;

#pragma pack(push, 1)
struct RemoteVideoWireBuffer
{
    uint32_t semantic = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t available = 0;
    uint32_t encoding = 0;
};

struct RemoteVideoWireMetadata
{
    uint32_t magic = kRemoteVideoWireMagic;
    uint32_t version = kRemoteVideoWireVersion;
    uint32_t byte_size = sizeof(RemoteVideoWireMetadata);
    uint32_t checksum = 0;
    uint64_t frame_id = 0;
    uint64_t timestamp_usec = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_generation = 0;
    uint32_t continuity_mask = 0;
    uint32_t available_buffer_mask = 0;
    uint32_t dynamic_range = 0;
    uint32_t tile_padding = kTilePadding;
    uint32_t flags = 0;
    uint32_t ddgi_frame_index = 0;
    uint32_t ddgi_reset_reason = 0;
    float confidence = 0.0f;
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    float pre_exposure = 1.0f;
    float view_origin[3] = {};
    float view_forward[3] = {};
    float temporal_jitter_pixels[2] = {};
    float view[16] = {};
    float projection[16] = {};
    float view_projection[16] = {};
    float inverse_view[16] = {};
    float inverse_projection[16] = {};
    float inverse_view_projection[16] = {};
    char source_stream_id[64] = {};
    RemoteVideoWireBuffer buffers[static_cast<size_t>(RemoteBufferSemantic::Count)] = {};
};

struct DownstreamFrameMetadataPacket
{
    uint32_t magic = 0x314d504eu; // NPM1
    uint32_t version = 1;
    uint32_t byte_size = sizeof(DownstreamFrameMetadataPacket);
    uint32_t checksum = 0;
    uint32_t video_width = 0;
    uint32_t video_height = 0;
    uint32_t tile_origins[static_cast<size_t>(RemoteBufferSemantic::Count)][2] = {};
    RemoteVideoWireMetadata wire;
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<RemoteVideoWireMetadata>);
static_assert(std::is_trivially_copyable_v<DownstreamFrameMetadataPacket>);

bool EncodeDownstreamFrameMetadata(
    const RemoteVideoFrameLayout& layout, DownstreamFrameMetadataPacket& packet);
bool DecodeDownstreamFrameMetadata(
    DownstreamFrameMetadataPacket packet, RemoteVideoFrameLayout& layout);

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

void CopyMatrixToWire(float (&output)[16], const XMFLOAT4X4& matrix)
{
    std::memcpy(output, &matrix._11, sizeof(output));
}

void CopyMatrixFromWire(XMFLOAT4X4& output, const float (&matrix)[16])
{
    std::memcpy(&output._11, matrix, sizeof(matrix));
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

uint32_t MetadataRows(uint32_t video_width)
{
    const uint64_t cells = static_cast<uint64_t>(sizeof(RemoteVideoWireMetadata)) * 8ull;
    const uint32_t cells_per_row = video_width / kMetadataBitCellSize;
    if (cells_per_row == 0)
        return 0;
    const uint64_t cell_rows = (cells + cells_per_row - 1ull) / cells_per_row;
    if (cell_rows > std::numeric_limits<uint32_t>::max() / kMetadataBitCellSize)
        return 0;
    return AlignEven(static_cast<uint32_t>(cell_rows) * kMetadataBitCellSize);
}

bool ValidateBuffer(const RemoteRawBuffer& buffer)
{
    if (!buffer.available)
        return buffer.payload_rgba8.empty();
    if (buffer.width == 0 || buffer.height == 0 ||
        buffer.width > kMaxRemoteBufferDimension || buffer.height > kMaxRemoteBufferDimension)
    {
        return false;
    }
    size_t expected_size = 0;
    return CheckedImageByteSize(buffer.width, buffer.height, 4u, expected_size) &&
        buffer.payload_rgba8.size() == expected_size;
}

RemoteVideoWireMetadata MakeWireMetadata(const RemoteRawFrame& frame, uint32_t tile_width, uint32_t tile_height)
{
    RemoteVideoWireMetadata wire;
    wire.frame_id = frame.metadata.frame_id;
    wire.timestamp_usec = frame.metadata.timestamp_usec;
    wire.width = tile_width;
    wire.height = tile_height;
    wire.source_generation = frame.metadata.source_generation;
    wire.continuity_mask = frame.metadata.continuity_mask;
    wire.available_buffer_mask = 0;
    wire.dynamic_range = static_cast<uint32_t>(frame.metadata.dynamic_range);
    wire.flags = (frame.metadata.history_valid ? kRemoteVideoFlagHistoryValid : 0u) |
        (frame.metadata.reset_this_frame ? kRemoteVideoFlagResetThisFrame : 0u) |
        (frame.metadata.camera_cut ? kRemoteVideoFlagCameraCut : 0u) |
        (frame.metadata.valid ? kRemoteVideoFlagValid : 0u);
    wire.ddgi_frame_index = frame.metadata.ddgi_frame_index;
    wire.ddgi_reset_reason = static_cast<uint32_t>(frame.metadata.ddgi_reset_reason);
    wire.confidence = frame.metadata.confidence;
    wire.near_plane = frame.metadata.near_plane;
    wire.far_plane = frame.metadata.far_plane;
    wire.pre_exposure = frame.metadata.pre_exposure;
    std::memcpy(wire.view_origin, &frame.metadata.view_origin.x, sizeof(wire.view_origin));
    std::memcpy(wire.view_forward, &frame.metadata.view_forward.x, sizeof(wire.view_forward));
    std::memcpy(wire.temporal_jitter_pixels, &frame.metadata.temporal_jitter_pixels.x, sizeof(wire.temporal_jitter_pixels));
    CopyMatrixToWire(wire.view, frame.metadata.view);
    CopyMatrixToWire(wire.projection, frame.metadata.projection);
    CopyMatrixToWire(wire.view_projection, frame.metadata.view_projection);
    CopyMatrixToWire(wire.inverse_view, frame.metadata.inverse_view);
    CopyMatrixToWire(wire.inverse_projection, frame.metadata.inverse_projection);
    CopyMatrixToWire(wire.inverse_view_projection, frame.metadata.inverse_view_projection);
    const size_t stream_bytes = std::min(frame.metadata.source_stream_id.size(), sizeof(wire.source_stream_id) - 1u);
    std::memcpy(wire.source_stream_id, frame.metadata.source_stream_id.data(), stream_bytes);

    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteRawBuffer& source = frame.buffers[index];
        RemoteVideoWireBuffer& destination = wire.buffers[index];
        destination.semantic = static_cast<uint32_t>(source.semantic);
        destination.width = source.width;
        destination.height = source.height;
        destination.available = source.available ? 1u : 0u;
        destination.encoding = static_cast<uint32_t>(source.encoding);
        if (source.available)
            wire.available_buffer_mask |= RemoteBufferKindMask(source.semantic);
    }

    wire.checksum = 0;
    wire.checksum = Fnv1a32(&wire, sizeof(wire));
    return wire;
}

bool ValidateWireMetadata(RemoteVideoWireMetadata wire)
{
    const uint32_t expected_checksum = wire.checksum;
    wire.checksum = 0;
    if (wire.magic != kRemoteVideoWireMagic || wire.version != kRemoteVideoWireVersion ||
        wire.byte_size != sizeof(RemoteVideoWireMetadata) || expected_checksum != Fnv1a32(&wire, sizeof(wire)) ||
        wire.frame_id == 0 || wire.width == 0 || wire.height == 0 ||
        wire.width > kMaxRemoteBufferDimension || wire.height > kMaxRemoteBufferDimension ||
        wire.ddgi_reset_reason > static_cast<uint32_t>(DDGIResetReason::GridChanged))
    {
        return false;
    }

    for (size_t index = 0; index < std::size(wire.buffers); ++index)
    {
        const RemoteVideoWireBuffer& buffer = wire.buffers[index];
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if (buffer.semantic != index || buffer.available > 1u ||
            buffer.encoding != static_cast<uint32_t>(RemoteBufferTransportEncoding(semantic)) ||
            (buffer.available != 0u &&
             (buffer.width == 0 || buffer.height == 0 || buffer.width > wire.width || buffer.height > wire.height)))
        {
            return false;
        }
    }
    return true;
}

uint64_t NowUsec()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count());
}

std::string ToMetadataString(const RemoteFrameMetadata& metadata, const std::string& payload_file)
{
    std::ostringstream stream;
    stream << "version=1\n";
    stream << "frame_id=" << metadata.frame_id << "\n";
    stream << "timestamp_usec=" << metadata.timestamp_usec << "\n";
    stream << "width=" << metadata.width << "\n";
    stream << "height=" << metadata.height << "\n";
    stream << "source_generation=" << metadata.source_generation << "\n";
    stream << "continuity_mask=" << metadata.continuity_mask << "\n";
    stream << "dynamic_range=" << ToString(metadata.dynamic_range) << "\n";
    stream << "source_stream_id=" << metadata.source_stream_id << "\n";
    stream << "history_valid=" << (metadata.history_valid ? 1 : 0) << "\n";
    stream << "reset_this_frame=" << (metadata.reset_this_frame ? 1 : 0) << "\n";
    stream << "confidence=" << metadata.confidence << "\n";
    stream << "valid=" << (metadata.valid ? 1 : 0) << "\n";
    stream << "payload_file=" << payload_file << "\n";
    stream << "payload_format=R8G8B8A8_UNORM\n";
    stream << "payload_bytes=" << metadata.width * metadata.height * 4ull << "\n";
    return stream.str();
}

bool WriteBytes(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;

    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return stream.good();
}

bool ReadBytes(const std::filesystem::path& path, std::vector<uint8_t>& data)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return false;

    const std::streamoff size = stream.tellg();
    if (size < 0)
        return false;

    data.resize(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!data.empty())
        stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));

    return stream.good() || (data.empty() && stream.eof());
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream)
        return {};

    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::string GetValue(const std::string& text, const std::string& key)
{
    const std::string prefix = key + "=";
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.rfind(prefix, 0) == 0)
            return line.substr(prefix.size());
    }
    return {};
}

bool ParseUInt64(const std::string& value, uint64_t& result)
{
    if (value.empty())
        return false;

    uint64_t parsed = 0;
    for (char c : value)
    {
        if (c < '0' || c > '9')
            return false;

        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10ull)
            return false;

        parsed = parsed * 10ull + digit;
    }

    result = parsed;
    return true;
}

bool ParseUInt32(const std::string& value, uint32_t& result)
{
    uint64_t parsed = 0;
    if (!ParseUInt64(value, parsed) || parsed > std::numeric_limits<uint32_t>::max())
        return false;

    result = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseFloat(const std::string& value, float& result)
{
    if (value.empty())
        return false;

    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || (end != nullptr && *end != '\0'))
        return false;

    result = parsed;
    return true;
}

bool ParseBool(const std::string& value)
{
    return value == "1" || value == "true";
}

std::string ToFloat3String(const XMFLOAT3& value)
{
    std::ostringstream stream;
    stream << value.x << "," << value.y << "," << value.z;
    return stream.str();
}

std::string ToFloat4x4String(const XMFLOAT4X4& value)
{
    std::ostringstream stream;
    const float* data = &value._11;
    for (int i = 0; i < 16; ++i)
    {
        if (i > 0)
            stream << ",";
        stream << data[i];
    }
    return stream.str();
}

bool ParseFloatList(const std::string& value, float* output, size_t count)
{
    std::istringstream stream(value);
    std::string token;
    for (size_t i = 0; i < count; ++i)
    {
        if (!std::getline(stream, token, ',') || !ParseFloat(token, output[i]))
            return false;
    }

    return !std::getline(stream, token, ',');
}

bool ParseFloat3(const std::string& value, XMFLOAT3& result)
{
    float parsed[3] = {};
    if (!ParseFloatList(value, parsed, 3))
        return false;

    result = XMFLOAT3(parsed[0], parsed[1], parsed[2]);
    return true;
}

bool ParseFloat4x4(const std::string& value, XMFLOAT4X4& result)
{
    float parsed[16] = {};
    if (!ParseFloatList(value, parsed, 16))
        return false;

    std::memcpy(&result._11, parsed, sizeof(parsed));
    return true;
}

RemoteDynamicRange ParseDynamicRange(const std::string& value)
{
    if (value == "LDR")
        return RemoteDynamicRange::LDR;
    if (value == "HDR")
        return RemoteDynamicRange::HDR;
    return RemoteDynamicRange::Unknown;
}

bool ParseMetadata(const std::string& text, RemoteFrameMetadata& metadata, std::string& payload_file)
{
    if (GetValue(text, "version") != "1")
        return false;

    if (!ParseUInt64(GetValue(text, "frame_id"), metadata.frame_id) ||
        !ParseUInt64(GetValue(text, "timestamp_usec"), metadata.timestamp_usec) ||
        !ParseUInt32(GetValue(text, "width"), metadata.width) ||
        !ParseUInt32(GetValue(text, "height"), metadata.height) ||
        !ParseUInt32(GetValue(text, "source_generation"), metadata.source_generation) ||
        !ParseUInt32(GetValue(text, "continuity_mask"), metadata.continuity_mask) ||
        !ParseFloat(GetValue(text, "confidence"), metadata.confidence))
    {
        return false;
    }

    metadata.dynamic_range = ParseDynamicRange(GetValue(text, "dynamic_range"));
    metadata.source_stream_id = GetValue(text, "source_stream_id");
    metadata.history_valid = ParseBool(GetValue(text, "history_valid"));
    metadata.reset_this_frame = ParseBool(GetValue(text, "reset_this_frame"));
    metadata.valid = ParseBool(GetValue(text, "valid"));
    payload_file = GetValue(text, "payload_file");
    return metadata.width > 0 && metadata.height > 0 && !payload_file.empty();
}

std::string ToControlString(const ClientControlPacket& packet)
{
    std::ostringstream stream;
    stream << "version=1\n";
    stream << "stream=" << kControlStreamName << "\n";
    stream << "frame_id=" << packet.frame_id << "\n";
    stream << "timestamp_usec=" << packet.timestamp_usec << "\n";
    stream << "viewport_width=" << packet.viewport_width << "\n";
    stream << "viewport_height=" << packet.viewport_height << "\n";
    stream << "scene_generation=" << packet.scene_generation << "\n";
    stream << "near_plane=" << packet.near_plane << "\n";
    stream << "far_plane=" << packet.far_plane << "\n";
    stream << "eye=" << ToFloat3String(packet.eye) << "\n";
    stream << "at=" << ToFloat3String(packet.at) << "\n";
    stream << "up=" << ToFloat3String(packet.up) << "\n";
    stream << "view=" << ToFloat4x4String(packet.view) << "\n";
    stream << "projection=" << ToFloat4x4String(packet.projection) << "\n";
    stream << "sun_enabled=" << (packet.sun_enabled ? 1 : 0) << "\n";
    stream << "sun_direction=" << ToFloat3String(packet.sun_direction) << "\n";
    stream << "sun_color=" << ToFloat3String(packet.sun_color) << "\n";
    stream << "sun_intensity=" << packet.sun_intensity << "\n";
    stream << "ambient=" << ToFloat3String(packet.ambient) << "\n";
    stream << "horizon=" << ToFloat3String(packet.horizon) << "\n";
    stream << "zenith=" << ToFloat3String(packet.zenith) << "\n";
    return stream.str();
}

bool ParseControlPacket(const std::string& text, ClientControlPacket& packet)
{
    if (GetValue(text, "version") != "1" || GetValue(text, "stream") != kControlStreamName)
        return false;

    if (!ParseUInt64(GetValue(text, "frame_id"), packet.frame_id) ||
        !ParseUInt64(GetValue(text, "timestamp_usec"), packet.timestamp_usec) ||
        !ParseUInt32(GetValue(text, "viewport_width"), packet.viewport_width) ||
        !ParseUInt32(GetValue(text, "viewport_height"), packet.viewport_height) ||
        !ParseUInt32(GetValue(text, "scene_generation"), packet.scene_generation) ||
        !ParseFloat(GetValue(text, "near_plane"), packet.near_plane) ||
        !ParseFloat(GetValue(text, "far_plane"), packet.far_plane) ||
        !ParseFloat3(GetValue(text, "eye"), packet.eye) ||
        !ParseFloat3(GetValue(text, "at"), packet.at) ||
        !ParseFloat3(GetValue(text, "up"), packet.up) ||
        !ParseFloat4x4(GetValue(text, "view"), packet.view) ||
        !ParseFloat4x4(GetValue(text, "projection"), packet.projection) ||
        !ParseFloat3(GetValue(text, "sun_direction"), packet.sun_direction) ||
        !ParseFloat3(GetValue(text, "sun_color"), packet.sun_color) ||
        !ParseFloat(GetValue(text, "sun_intensity"), packet.sun_intensity) ||
        !ParseFloat3(GetValue(text, "ambient"), packet.ambient) ||
        !ParseFloat3(GetValue(text, "horizon"), packet.horizon) ||
        !ParseFloat3(GetValue(text, "zenith"), packet.zenith))
    {
        return false;
    }

    const std::string sun_enabled = GetValue(text, "sun_enabled");
    if (!sun_enabled.empty())
        packet.sun_enabled = ParseBool(sun_enabled);

    return packet.viewport_width > 0 && packet.viewport_height > 0;
}

void PruneOldRemotePayloads(const std::filesystem::path& root, const std::filesystem::path& keep_file)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    std::vector<fs::directory_entry> payloads;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, ec))
    {
        if (ec)
            return;
        if (!entry.is_regular_file(ec) || ec)
        {
            ec.clear();
            continue;
        }
        if (entry.path().extension() == ".rgba8")
            payloads.push_back(entry);
    }

    std::sort(payloads.begin(), payloads.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        std::error_code ec_a;
        std::error_code ec_b;
        return a.last_write_time(ec_a) > b.last_write_time(ec_b);
    });

    size_t retained = 0;
    for (const fs::directory_entry& entry : payloads)
    {
        if (entry.path() == keep_file || retained < kRetainedRemotePayloadCount)
        {
            ++retained;
            continue;
        }
        fs::remove(entry.path(), ec);
        ec.clear();
    }
}

void RemoveStaleTempFiles(const std::filesystem::path& root)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, ec))
    {
        if (ec)
            return;
        if (!entry.is_regular_file(ec) || ec)
        {
            ec.clear();
            continue;
        }
        if (entry.path().extension() == ".tmp")
        {
            fs::remove(entry.path(), ec);
            ec.clear();
        }
    }
}

void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
}
} // namespace

RemoteRawFrame::RemoteRawFrame()
{
    for (size_t index = 0; index < buffers.size(); ++index)
    {
        buffers[index].semantic = static_cast<RemoteBufferSemantic>(index);
        buffers[index].encoding = RemoteBufferTransportEncoding(buffers[index].semantic);
    }
}

RemoteRawBuffer* RemoteRawFrame::FindBuffer(RemoteBufferSemantic semantic)
{
    const size_t index = static_cast<size_t>(semantic);
    return index < buffers.size() ? &buffers[index] : nullptr;
}

const RemoteRawBuffer* RemoteRawFrame::FindBuffer(RemoteBufferSemantic semantic) const
{
    const size_t index = static_cast<size_t>(semantic);
    return index < buffers.size() ? &buffers[index] : nullptr;
}

bool BuildRemoteVideoFrameLayout(
    const RemoteRawFrame& frame,
    RemoteVideoFrameLayout& layout,
    std::vector<uint8_t>& metadata_luma,
    std::string* error)
{
    layout = {};
    metadata_luma.clear();
    if (!frame.metadata.valid || frame.metadata.frame_id == 0 || frame.metadata.source_generation == 0)
    {
        SetError(error, "remote frame metadata is incomplete");
        return false;
    }

    uint32_t tile_width = 0;
    uint32_t tile_height = 0;
    uint32_t available_mask = 0;
    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteRawBuffer& buffer = frame.buffers[index];
        if (buffer.semantic != static_cast<RemoteBufferSemantic>(index) ||
            buffer.encoding != RemoteBufferTransportEncoding(buffer.semantic) ||
            (buffer.available && (buffer.width == 0 || buffer.height == 0 ||
                buffer.width > kMaxRemoteBufferDimension || buffer.height > kMaxRemoteBufferDimension)))
        {
            SetError(error, "remote buffer layout contract is invalid");
            return false;
        }
        if (buffer.available)
        {
            tile_width = std::max(tile_width, buffer.width);
            tile_height = std::max(tile_height, buffer.height);
            available_mask |= RemoteBufferKindMask(buffer.semantic);
        }
    }
    if (available_mask == 0 || tile_width == 0 || tile_height == 0 ||
        (frame.metadata.continuity_mask & available_mask) != available_mask)
    {
        SetError(error, "remote frame contains no continuous video buffers");
        return false;
    }

    tile_width = AlignEven(tile_width);
    tile_height = AlignEven(tile_height);
    const uint32_t tile_stride_x = tile_width + kTilePadding * 2u;
    const uint32_t tile_stride_y = tile_height + kTilePadding * 2u;
    const uint32_t video_width = tile_stride_x * 2u;
    const uint32_t metadata_rows = MetadataRows(video_width);
    if (metadata_rows == 0 || tile_width > std::numeric_limits<uint32_t>::max() / 2u ||
        tile_stride_y > (std::numeric_limits<uint32_t>::max() - metadata_rows) / 2u)
    {
        SetError(error, "remote video dimensions overflow");
        return false;
    }
    const uint32_t video_height = metadata_rows + tile_stride_y * 2u;
    size_t metadata_size = 0;
    if (!CheckedImageByteSize(video_width, metadata_rows, 1u, metadata_size))
    {
        SetError(error, "remote video metadata band size overflow");
        return false;
    }

    layout.video_width = video_width;
    layout.video_height = video_height;
    layout.metadata = frame.metadata;
    layout.metadata.width = tile_width;
    layout.metadata.height = tile_height;
    layout.metadata.available_buffer_mask = available_mask;
    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteRawBuffer& buffer = frame.buffers[index];
        RemoteVideoTileLayout& tile = layout.tiles[index];
        tile.semantic = buffer.semantic;
        tile.width = buffer.width;
        tile.height = buffer.height;
        tile.available = buffer.available;
        tile.encoding = buffer.encoding;
        tile.origin_x = static_cast<uint32_t>(index & 1u) * tile_stride_x + kTilePadding;
        tile.origin_y = metadata_rows + static_cast<uint32_t>(index / 2u) * tile_stride_y + kTilePadding;
    }

    metadata_luma.assign(metadata_size, 16u);
    const RemoteVideoWireMetadata wire = MakeWireMetadata(frame, tile_width, tile_height);
    const auto* wire_bytes = reinterpret_cast<const uint8_t*>(&wire);
    const uint32_t cells_per_row = video_width / kMetadataBitCellSize;
    const uint64_t bit_count = static_cast<uint64_t>(sizeof(wire)) * 8ull;
    for (uint64_t bit_index = 0; bit_index < bit_count; ++bit_index)
    {
        const bool one = (wire_bytes[bit_index / 8ull] & (1u << (bit_index & 7ull))) != 0;
        const uint32_t cell_x = static_cast<uint32_t>(bit_index % cells_per_row) * kMetadataBitCellSize;
        const uint32_t cell_y = static_cast<uint32_t>(bit_index / cells_per_row) * kMetadataBitCellSize;
        const uint8_t value = one ? 224u : 32u;
        for (uint32_t y = 0; y < kMetadataBitCellSize; ++y)
            std::fill_n(metadata_luma.data() + static_cast<size_t>(cell_y + y) * video_width + cell_x,
                kMetadataBitCellSize, value);
    }
    return true;
}

bool EncodeRemoteVideoFrame(const RemoteRawFrame& frame, PackedRemoteVideoFrame& video, std::string* error)
{
    video = {};
    if (!frame.metadata.valid || frame.metadata.frame_id == 0 || frame.metadata.source_generation == 0)
    {
        SetError(error, "remote frame metadata is incomplete");
        return false;
    }

    uint32_t tile_width = 0;
    uint32_t tile_height = 0;
    uint32_t available_mask = 0;
    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteRawBuffer& buffer = frame.buffers[index];
        if (buffer.semantic != static_cast<RemoteBufferSemantic>(index) ||
            buffer.encoding != RemoteBufferTransportEncoding(buffer.semantic) || !ValidateBuffer(buffer))
        {
            SetError(error, "remote buffer contract is invalid");
            return false;
        }
        if (buffer.available)
        {
            tile_width = std::max(tile_width, buffer.width);
            tile_height = std::max(tile_height, buffer.height);
            available_mask |= RemoteBufferKindMask(buffer.semantic);
        }
    }

    if (available_mask == 0 || tile_width == 0 || tile_height == 0)
    {
        SetError(error, "remote frame contains no video buffers");
        return false;
    }
    if ((frame.metadata.continuity_mask & available_mask) != available_mask)
    {
        SetError(error, "remote frame continuity mask omits an available buffer");
        return false;
    }

    tile_width = AlignEven(tile_width);
    tile_height = AlignEven(tile_height);
    const uint32_t tile_stride_x = tile_width + kTilePadding * 2u;
    const uint32_t tile_stride_y = tile_height + kTilePadding * 2u;
    const uint32_t video_width = tile_stride_x * 2u;
    const uint32_t metadata_rows = MetadataRows(video_width);
    if (metadata_rows == 0 || tile_width > std::numeric_limits<uint32_t>::max() / 2u ||
        tile_stride_y > (std::numeric_limits<uint32_t>::max() - metadata_rows) / 2u)
    {
        SetError(error, "remote video dimensions overflow");
        return false;
    }
    const uint32_t video_height = metadata_rows + tile_stride_y * 2u;
    const uint32_t chroma_width = video_width / 2u;
    const uint32_t chroma_height = video_height / 2u;
    size_t y_size = 0;
    size_t uv_size = 0;
    if (!CheckedImageByteSize(video_width, video_height, 1u, y_size) ||
        !CheckedImageByteSize(chroma_width, chroma_height, 1u, uv_size) ||
        y_size > std::numeric_limits<size_t>::max() - uv_size * 2u)
    {
        SetError(error, "remote video allocation size overflow");
        return false;
    }

    video.width = video_width;
    video.height = video_height;
    video.i420.assign(y_size + uv_size * 2u, 128u);
    uint8_t* y_plane = video.i420.data();
    uint8_t* u_plane = y_plane + y_size;
    uint8_t* v_plane = u_plane + uv_size;
    std::fill(y_plane, y_plane + y_size, 16u);

    const RemoteVideoWireMetadata wire = MakeWireMetadata(frame, tile_width, tile_height);
    const auto* wire_bytes = reinterpret_cast<const uint8_t*>(&wire);
    const uint32_t cells_per_row = video_width / kMetadataBitCellSize;
    const uint64_t bit_count = static_cast<uint64_t>(sizeof(wire)) * 8ull;
    for (uint64_t bit_index = 0; bit_index < bit_count; ++bit_index)
    {
        const bool one = (wire_bytes[bit_index / 8ull] & (1u << (bit_index & 7ull))) != 0;
        const uint32_t cell_x = static_cast<uint32_t>(bit_index % cells_per_row) * kMetadataBitCellSize;
        const uint32_t cell_y = static_cast<uint32_t>(bit_index / cells_per_row) * kMetadataBitCellSize;
        const uint8_t value = one ? 224u : 32u;
        for (uint32_t y = 0; y < kMetadataBitCellSize; ++y)
        {
            std::fill_n(y_plane + static_cast<size_t>(cell_y + y) * video_width + cell_x,
                kMetadataBitCellSize, value);
        }
    }

    for (size_t buffer_index = 0; buffer_index < frame.buffers.size(); ++buffer_index)
    {
        const RemoteRawBuffer& buffer = frame.buffers[buffer_index];
        if (!buffer.available)
            continue;

        const uint32_t origin_x = static_cast<uint32_t>(buffer_index & 1u) * tile_stride_x + kTilePadding;
        const uint32_t origin_y = metadata_rows + static_cast<uint32_t>(buffer_index / 2u) * tile_stride_y + kTilePadding;
        for (uint32_t y = 0; y < buffer.height; ++y)
        {
            for (uint32_t x = 0; x < buffer.width; ++x)
            {
                const size_t source_index = (static_cast<size_t>(y) * buffer.width + x) * 4u;
                uint8_t y_value = 16;
                uint8_t u_value = 128;
                uint8_t v_value = 128;
                if (buffer.encoding == RemoteBufferEncoding::ScalarLuma8)
                {
                    const uint8_t scalar = buffer.payload_rgba8[source_index];
                    RGBToYUV(scalar, scalar, scalar, y_value, u_value, v_value);
                }
                else
                {
                    RGBToYUV(buffer.payload_rgba8[source_index + 0u],
                        buffer.payload_rgba8[source_index + 1u],
                        buffer.payload_rgba8[source_index + 2u],
                        y_value, u_value, v_value);
                }
                y_plane[static_cast<size_t>(origin_y + y) * video_width + origin_x + x] = y_value;
            }
        }

        if (buffer.encoding == RemoteBufferEncoding::ScalarLuma8)
            continue; // U/V stay neutral; the scalar uses full-resolution Y only.

        for (uint32_t y = 0; y < buffer.height; y += 2u)
        {
            for (uint32_t x = 0; x < buffer.width; x += 2u)
            {
                uint32_t red = 0;
                uint32_t green = 0;
                uint32_t blue = 0;
                uint32_t count = 0;
                for (uint32_t dy = 0; dy < 2u; ++dy)
                {
                    for (uint32_t dx = 0; dx < 2u; ++dx)
                    {
                        const uint32_t source_x = x + dx;
                        const uint32_t source_y = y + dy;
                        if (source_x >= buffer.width || source_y >= buffer.height)
                            continue;
                        const size_t source_index = (static_cast<size_t>(source_y) * buffer.width + source_x) * 4u;
                        red += buffer.payload_rgba8[source_index + 0u];
                        green += buffer.payload_rgba8[source_index + 1u];
                        blue += buffer.payload_rgba8[source_index + 2u];
                        ++count;
                    }
                }
                uint8_t y_value = 16;
                uint8_t u_value = 128;
                uint8_t v_value = 128;
                RGBToYUV(static_cast<uint8_t>(red / count), static_cast<uint8_t>(green / count),
                    static_cast<uint8_t>(blue / count), y_value, u_value, v_value);
                const size_t chroma_index = static_cast<size_t>((origin_y + y) / 2u) * chroma_width +
                    (origin_x + x) / 2u;
                u_plane[chroma_index] = u_value;
                v_plane[chroma_index] = v_value;
            }
        }
    }
    return true;
}

bool RetainedI420Frame::IsValid() const
{
    return width >= kMetadataBitCellSize && height > 0 && (width & 1u) == 0 && (height & 1u) == 0 &&
        y_plane != nullptr && u_plane != nullptr && v_plane != nullptr &&
        y_stride >= width && u_stride >= width / 2u && v_stride >= width / 2u;
}

bool DecodeRemoteVideoFrame(const PackedRemoteVideoFrame& video, RemoteRawFrame& frame, std::string* error)
{
    size_t y_size = 0;
    size_t uv_size = 0;
    if (!CheckedImageByteSize(video.width, video.height, 1u, y_size) ||
        !CheckedImageByteSize(video.width / 2u, video.height / 2u, 1u, uv_size) ||
        video.i420.size() != y_size + uv_size * 2u)
    {
        frame = {};
        SetError(error, "remote I420 payload size is invalid");
        return false;
    }

    RetainedI420Frame retained;
    retained.width = video.width;
    retained.height = video.height;
    retained.y_plane = video.i420.data();
    retained.y_stride = video.width;
    retained.u_plane = retained.y_plane + y_size;
    retained.u_stride = video.width / 2u;
    retained.v_plane = retained.u_plane + uv_size;
    retained.v_stride = video.width / 2u;
    return DecodeRemoteVideoFrame(retained, frame, error);
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

    const uint32_t metadata_rows = MetadataRows(video.width);
    if (metadata_rows == 0 || metadata_rows >= video.height)
    {
        SetError(error, "remote video metadata band is invalid");
        return false;
    }

    RemoteVideoWireMetadata wire = {};
    auto* wire_bytes = reinterpret_cast<uint8_t*>(&wire);
    const uint32_t cells_per_row = video.width / kMetadataBitCellSize;
    const uint64_t bit_count = static_cast<uint64_t>(sizeof(wire)) * 8ull;
    for (uint64_t bit_index = 0; bit_index < bit_count; ++bit_index)
    {
        const uint32_t cell_x = static_cast<uint32_t>(bit_index % cells_per_row) * kMetadataBitCellSize;
        const uint32_t cell_y = static_cast<uint32_t>(bit_index / cells_per_row) * kMetadataBitCellSize;
        uint32_t sum = 0;
        for (uint32_t y = 0; y < kMetadataBitCellSize; ++y)
        {
            for (uint32_t x = 0; x < kMetadataBitCellSize; ++x)
                sum += video.y_plane[static_cast<size_t>(cell_y + y) * video.y_stride + cell_x + x];
        }
        if (sum >= 128u * kMetadataBitCellSize * kMetadataBitCellSize)
            wire_bytes[bit_index / 8ull] |= static_cast<uint8_t>(1u << (bit_index & 7ull));
    }

    const uint32_t tile_stride_x = wire.width + wire.tile_padding * 2u;
    const uint32_t tile_stride_y = wire.height + wire.tile_padding * 2u;
    if (!ValidateWireMetadata(wire) || tile_stride_x * 2u != video.width ||
        metadata_rows + tile_stride_y * 2u != video.height || wire.tile_padding != kTilePadding)
    {
        SetError(error, "remote video metadata checksum or layout is invalid");
        return false;
    }

    layout.video_width = video.width;
    layout.video_height = video.height;
    layout.metadata.frame_id = wire.frame_id;
    layout.metadata.timestamp_usec = wire.timestamp_usec;
    layout.metadata.width = wire.width;
    layout.metadata.height = wire.height;
    layout.metadata.source_generation = wire.source_generation;
    layout.metadata.continuity_mask = wire.continuity_mask;
    layout.metadata.available_buffer_mask = wire.available_buffer_mask;
    layout.metadata.dynamic_range = static_cast<RemoteDynamicRange>(wire.dynamic_range);
    layout.metadata.source_stream_id.assign(wire.source_stream_id,
        strnlen(wire.source_stream_id, sizeof(wire.source_stream_id)));
    std::memcpy(&layout.metadata.view_origin.x, wire.view_origin, sizeof(wire.view_origin));
    std::memcpy(&layout.metadata.view_forward.x, wire.view_forward, sizeof(wire.view_forward));
    std::memcpy(&layout.metadata.temporal_jitter_pixels.x, wire.temporal_jitter_pixels, sizeof(wire.temporal_jitter_pixels));
    CopyMatrixFromWire(layout.metadata.view, wire.view);
    CopyMatrixFromWire(layout.metadata.projection, wire.projection);
    CopyMatrixFromWire(layout.metadata.view_projection, wire.view_projection);
    CopyMatrixFromWire(layout.metadata.inverse_view, wire.inverse_view);
    CopyMatrixFromWire(layout.metadata.inverse_projection, wire.inverse_projection);
    CopyMatrixFromWire(layout.metadata.inverse_view_projection, wire.inverse_view_projection);
    layout.metadata.near_plane = wire.near_plane;
    layout.metadata.far_plane = wire.far_plane;
    layout.metadata.pre_exposure = wire.pre_exposure;
    layout.metadata.history_valid = (wire.flags & kRemoteVideoFlagHistoryValid) != 0;
    layout.metadata.reset_this_frame = (wire.flags & kRemoteVideoFlagResetThisFrame) != 0;
    layout.metadata.camera_cut = (wire.flags & kRemoteVideoFlagCameraCut) != 0;
    layout.metadata.valid = (wire.flags & kRemoteVideoFlagValid) != 0;
    layout.metadata.ddgi_frame_index = wire.ddgi_frame_index;
    layout.metadata.ddgi_reset_reason = static_cast<DDGIResetReason>(wire.ddgi_reset_reason);
    layout.metadata.confidence = wire.confidence;
    layout.metadata.local_receive_timestamp_usec = NowUsec();

    for (size_t buffer_index = 0; buffer_index < layout.tiles.size(); ++buffer_index)
    {
        const RemoteVideoWireBuffer& source = wire.buffers[buffer_index];
        RemoteVideoTileLayout& tile = layout.tiles[buffer_index];
        tile.semantic = static_cast<RemoteBufferSemantic>(source.semantic);
        tile.width = source.width;
        tile.height = source.height;
        tile.available = source.available != 0;
        tile.encoding = static_cast<RemoteBufferEncoding>(source.encoding);
        tile.origin_x = static_cast<uint32_t>(buffer_index & 1u) * tile_stride_x + wire.tile_padding;
        tile.origin_y = metadata_rows + static_cast<uint32_t>(buffer_index / 2u) * tile_stride_y + wire.tile_padding;
    }
    return true;
}

bool DecodeRemoteVideoFrame(const RetainedI420Frame& video, RemoteRawFrame& frame, std::string* error)
{
    frame = {};
    RemoteVideoFrameLayout layout;
    if (!DecodeRemoteVideoFrameLayout(video, layout, error))
        return false;
    frame.metadata = layout.metadata;

    for (size_t buffer_index = 0; buffer_index < frame.buffers.size(); ++buffer_index)
    {
        const RemoteVideoTileLayout& source = layout.tiles[buffer_index];
        RemoteRawBuffer& buffer = frame.buffers[buffer_index];
        buffer.semantic = source.semantic;
        buffer.width = source.width;
        buffer.height = source.height;
        buffer.available = source.available;
        buffer.encoding = source.encoding;
        if (!buffer.available)
            continue;

        size_t payload_size = 0;
        if (!CheckedImageByteSize(buffer.width, buffer.height, 4u, payload_size))
        {
            SetError(error, "decoded remote buffer size overflow");
            return false;
        }
        buffer.payload_rgba8.resize(payload_size);
        const uint32_t origin_x = source.origin_x;
        const uint32_t origin_y = source.origin_y;
        if (buffer.encoding == RemoteBufferEncoding::LogHDR16F)
            buffer.payload_rgba16f.resize(static_cast<size_t>(buffer.width) * buffer.height * 4u);
        for (uint32_t y = 0; y < buffer.height; ++y)
        {
            for (uint32_t x = 0; x < buffer.width; ++x)
            {
                const uint8_t y_value = video.y_plane[
                    static_cast<size_t>(origin_y + y) * video.y_stride + origin_x + x];
                const size_t chroma_x = (origin_x + x) / 2u;
                const size_t chroma_y = (origin_y + y) / 2u;
                uint8_t red = 0;
                uint8_t green = 0;
                uint8_t blue = 0;
                YUVToRGB(y_value,
                    video.u_plane[chroma_y * video.u_stride + chroma_x],
                    video.v_plane[chroma_y * video.v_stride + chroma_x], red, green, blue);
                if (buffer.encoding == RemoteBufferEncoding::ScalarLuma8)
                {
                    green = red;
                    blue = red;
                }
                const size_t destination_index = (static_cast<size_t>(y) * buffer.width + x) * 4u;
                buffer.payload_rgba8[destination_index + 0u] = red;
                buffer.payload_rgba8[destination_index + 1u] = green;
                buffer.payload_rgba8[destination_index + 2u] = blue;
                buffer.payload_rgba8[destination_index + 3u] = 255u;
                if (buffer.encoding == RemoteBufferEncoding::LogHDR16F)
                {
                    const float scale = std::log2(1.0f + kHDRTransportMaximum);
                    const float decoded_r = std::exp2((red / 255.0f) * scale) - 1.0f;
                    const float decoded_g = std::exp2((green / 255.0f) * scale) - 1.0f;
                    const float decoded_b = std::exp2((blue / 255.0f) * scale) - 1.0f;
                    buffer.payload_rgba16f[destination_index + 0u] = static_cast<uint16_t>(wi::math::f32tof16(decoded_r));
                    buffer.payload_rgba16f[destination_index + 1u] = static_cast<uint16_t>(wi::math::f32tof16(decoded_g));
                    buffer.payload_rgba16f[destination_index + 2u] = static_cast<uint16_t>(wi::math::f32tof16(decoded_b));
                    buffer.payload_rgba16f[destination_index + 3u] = static_cast<uint16_t>(wi::math::f32tof16(1.0f));
                }
            }
        }
    }

    if (!frame.metadata.valid || frame.metadata.available_buffer_mask == 0)
    {
        SetError(error, "decoded remote frame is not usable");
        return false;
    }
    return true;
}

bool ValidateRemoteVideoV2RoundTrip(std::string* error)
{
    RemoteRawFrame source;
    source.metadata.frame_id = 1;
    source.metadata.timestamp_usec = 1;
    source.metadata.source_generation = 1;
    source.metadata.continuity_mask = static_cast<uint32_t>(RemoteBufferKind::All);
    source.metadata.available_buffer_mask = static_cast<uint32_t>(RemoteBufferKind::All);
    source.metadata.dynamic_range = RemoteDynamicRange::HDR;
    source.metadata.source_stream_id = kRemoteFrameStreamId;
    source.metadata.valid = true;
    source.metadata.ddgi_frame_index = 64;
    source.metadata.history_valid = true;
    for (size_t index = 0; index < source.buffers.size(); ++index)
    {
        RemoteRawBuffer& buffer = source.buffers[index];
        buffer.width = 4;
        buffer.height = 4;
        buffer.available = true;
        buffer.encoding = index == 0 || index == 2
            ? RemoteBufferEncoding::LogHDR16F
            : RemoteBufferEncoding::ScalarLuma8;
        buffer.payload_rgba8.resize(4u * 4u * 4u);
        for (size_t pixel = 0; pixel < 16; ++pixel)
        {
            const uint8_t value = static_cast<uint8_t>(32 + pixel * 12);
            buffer.payload_rgba8[pixel * 4 + 0] = value;
            buffer.payload_rgba8[pixel * 4 + 1] = buffer.encoding == RemoteBufferEncoding::ScalarLuma8 ? value : static_cast<uint8_t>(255 - value);
            buffer.payload_rgba8[pixel * 4 + 2] = value / 2;
            buffer.payload_rgba8[pixel * 4 + 3] = 255;
        }
    }
    PackedRemoteVideoFrame packed;
    if (!EncodeRemoteVideoFrame(source, packed, error))
        return false;
    RemoteRawFrame decoded;
    if (!DecodeRemoteVideoFrame(packed, decoded, error))
        return false;
    if (decoded.metadata.ddgi_frame_index != 64 || !decoded.metadata.history_valid ||
        decoded.metadata.available_buffer_mask != static_cast<uint32_t>(RemoteBufferKind::All) ||
        decoded.buffers[0].payload_rgba16f.size() != 64 || decoded.buffers[2].payload_rgba16f.size() != 64)
    {
        SetError(error, "V2 HDR/scalar round-trip contract mismatch");
        return false;
    }

    RemoteVideoFrameLayout source_layout;
    std::vector<uint8_t> metadata_luma;
    if (!BuildRemoteVideoFrameLayout(source, source_layout, metadata_luma, error))
        return false;
    DownstreamFrameMetadataPacket metadata_packet;
    if (!EncodeDownstreamFrameMetadata(source_layout, metadata_packet))
    {
        SetError(error, "downstream metadata packet encode failed");
        return false;
    }
    RemoteVideoFrameLayout metadata_roundtrip;
    if (!DecodeDownstreamFrameMetadata(metadata_packet, metadata_roundtrip) ||
        metadata_roundtrip.metadata.frame_id != source_layout.metadata.frame_id ||
        metadata_roundtrip.metadata.timestamp_usec != source_layout.metadata.timestamp_usec ||
        metadata_roundtrip.metadata.source_generation != source_layout.metadata.source_generation ||
        metadata_roundtrip.video_width != source_layout.video_width ||
        metadata_roundtrip.video_height != source_layout.video_height)
    {
        SetError(error, "downstream metadata packet round-trip contract mismatch");
        return false;
    }
    for (size_t index = 0; index < source_layout.tiles.size(); ++index)
    {
        const RemoteVideoTileLayout& expected = source_layout.tiles[index];
        const RemoteVideoTileLayout& actual = metadata_roundtrip.tiles[index];
        if (actual.semantic != expected.semantic || actual.width != expected.width ||
            actual.height != expected.height || actual.origin_x != expected.origin_x ||
            actual.origin_y != expected.origin_y || actual.available != expected.available ||
            actual.encoding != expected.encoding)
        {
            SetError(error, "downstream metadata tile round-trip contract mismatch");
            return false;
        }
    }
    metadata_packet.checksum ^= 1u;
    if (DecodeDownstreamFrameMetadata(metadata_packet, metadata_roundtrip))
    {
        SetError(error, "downstream metadata checksum corruption was accepted");
        return false;
    }
    return true;
}

void InProcessControlMailbox::Publish(const ClientControlPacket& packet)
{
    std::lock_guard<std::mutex> lock(mutex);
    latest_packet = packet;
    ++latest_sequence;
}

bool InProcessControlMailbox::TryConsumeLatest(ClientControlPacket& packet)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (latest_sequence == 0 || consumed_sequence == latest_sequence)
        return false;

    packet = latest_packet;
    consumed_sequence = latest_sequence;
    return true;
}

bool InProcessControlMailbox::PeekLatest(ClientControlPacket& packet) const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (latest_sequence == 0)
        return false;

    packet = latest_packet;
    return true;
}

void InProcessControlMailbox::Reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    latest_packet = {};
    latest_sequence = 0;
    consumed_sequence = 0;
}

InProcessControlMailbox& GetInProcessControlMailbox()
{
    static InProcessControlMailbox mailbox;
    return mailbox;
}

std::string GetDefaultMockMailboxDirectory()
{
    return (std::filesystem::temp_directory_path() / "wicked_newpipeline_mock").string();
}

std::string GetDefaultMockRemoteMailboxDirectory()
{
    return (std::filesystem::path(GetDefaultMockMailboxDirectory()) / "remote").string();
}

std::string GetDefaultMockControlMailboxDirectory()
{
    return (std::filesystem::path(GetDefaultMockMailboxDirectory()) / "control").string();
}

FileMockControlMailbox::FileMockControlMailbox(std::string root_directory) :
    root_directory(std::move(root_directory))
{
}

bool FileMockControlMailbox::PublishLatest(const ClientControlPacket& packet, std::string* error) const
{
    namespace fs = std::filesystem;

    if (packet.frame_id == 0 || packet.viewport_width == 0 || packet.viewport_height == 0)
    {
        SetError(error, "control packet is incomplete");
        return false;
    }

    const fs::path root(root_directory);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
    {
        SetError(error, "failed to create mock control directory: " + ec.message());
        return false;
    }
    RemoveStaleTempFiles(root);

    const fs::path control_tmp = root / "latest.control.tmp";
    const fs::path control_final = root / kLatestControlFile;
    const std::string text = ToControlString(packet);
    if (!WriteBytes(control_tmp, text.data(), text.size()))
    {
        SetError(error, "failed to write mock control packet");
        return false;
    }

    fs::rename(control_tmp, control_final, ec);
    if (ec)
    {
        fs::remove(control_final, ec);
        ec.clear();
        fs::rename(control_tmp, control_final, ec);
        if (ec)
        {
            SetError(error, "failed to publish mock control packet: " + ec.message());
            return false;
        }
    }

    return true;
}

bool FileMockControlMailbox::TryConsumeLatest(ClientControlPacket& packet, std::string* error)
{
    namespace fs = std::filesystem;

    const fs::path root(root_directory);
    const fs::path control_path = root / kLatestControlFile;
    std::error_code ec;
    if (!fs::exists(control_path, ec) || ec)
        return false;

    ClientControlPacket latest;
    if (!ParseControlPacket(ReadTextFile(control_path), latest))
    {
        SetError(error, "failed to parse mock control packet");
        return false;
    }

    if (latest.frame_id == consumed_frame_id)
        return false;

    const uint64_t now = NowUsec();
    if (latest.timestamp_usec == 0 || latest.timestamp_usec + kMaxControlPacketAgeUsec < now)
    {
        consumed_frame_id = latest.frame_id;
        return false;
    }

    consumed_frame_id = latest.frame_id;
    packet = latest;
    return true;
}

FileMockRemoteMailbox::FileMockRemoteMailbox(std::string root_directory) :
    root_directory(std::move(root_directory))
{
}

bool FileMockRemoteMailbox::PublishLatest(const RemoteRawFrame& frame, std::string* error) const
{
    namespace fs = std::filesystem;

    PackedRemoteVideoFrame video;
    if (!EncodeRemoteVideoFrame(frame, video, error))
        return false;

    const fs::path root(root_directory);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
    {
        SetError(error, "failed to create mock mailbox directory: " + ec.message());
        return false;
    }
    RemoveStaleTempFiles(root);

    struct PackedFileHeader
    {
        uint32_t magic = kRemoteVideoWireMagic;
        uint32_t version = kRemoteVideoWireVersion;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t byte_size = 0;
    };
    PackedFileHeader file_header;
    file_header.width = video.width;
    file_header.height = video.height;
    file_header.byte_size = video.i420.size();
    std::vector<uint8_t> file_bytes(sizeof(file_header) + video.i420.size());
    std::memcpy(file_bytes.data(), &file_header, sizeof(file_header));
    std::memcpy(file_bytes.data() + sizeof(file_header), video.i420.data(), video.i420.size());

    const fs::path video_tmp = root / "latest.npv.tmp";
    const fs::path video_final = root / kLatestRemoteVideoFile;
    if (!WriteBytes(video_tmp, file_bytes.data(), file_bytes.size()))
    {
        SetError(error, "failed to write mock remote video frame");
        return false;
    }

    fs::rename(video_tmp, video_final, ec);
    if (ec)
    {
        fs::remove(video_final, ec);
        ec.clear();
        fs::rename(video_tmp, video_final, ec);
        if (ec)
        {
            SetError(error, "failed to publish mock remote video frame: " + ec.message());
            return false;
        }
    }
    return true;
}

bool FileMockRemoteMailbox::TryReadLatest(RemoteRawFrame& frame, std::string* error) const
{
    namespace fs = std::filesystem;
    const fs::path root(root_directory);
    const fs::path video_path = root / kLatestRemoteVideoFile;
    std::error_code ec;
    if (!fs::exists(video_path, ec) || ec)
        return false;

    std::vector<uint8_t> file_bytes;
    if (!ReadBytes(video_path, file_bytes))
    {
        SetError(error, "failed to read mock remote video frame");
        return false;
    }

    struct PackedFileHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t width;
        uint32_t height;
        uint64_t byte_size;
    };
    if (file_bytes.size() < sizeof(PackedFileHeader))
    {
        SetError(error, "mock remote video file header is truncated");
        return false;
    }

    PackedFileHeader file_header = {};
    std::memcpy(&file_header, file_bytes.data(), sizeof(file_header));
    if (file_header.magic != kRemoteVideoWireMagic || file_header.version != kRemoteVideoWireVersion ||
        file_header.width == 0 || file_header.height == 0 ||
        file_header.byte_size != file_bytes.size() - sizeof(PackedFileHeader))
    {
        SetError(error, "mock remote video file header is invalid");
        return false;
    }

    PackedRemoteVideoFrame video;
    video.width = file_header.width;
    video.height = file_header.height;
    video.i420.assign(file_bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(PackedFileHeader)), file_bytes.end());
    return DecodeRemoteVideoFrame(video, frame, error);
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
bool EncodeDownstreamFrameMetadata(
    const RemoteVideoFrameLayout& layout, DownstreamFrameMetadataPacket& packet)
{
    if (!layout.metadata.valid || layout.video_width == 0 || layout.video_height == 0)
        return false;
    RemoteRawFrame descriptor;
    descriptor.metadata = layout.metadata;
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        RemoteRawBuffer& buffer = descriptor.buffers[index];
        buffer.semantic = tile.semantic;
        buffer.width = tile.width;
        buffer.height = tile.height;
        buffer.available = tile.available;
        buffer.encoding = tile.encoding;
    }
    packet = {};
    packet.video_width = layout.video_width;
    packet.video_height = layout.video_height;
    packet.wire = MakeWireMetadata(descriptor, layout.metadata.width, layout.metadata.height);
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        packet.tile_origins[index][0] = layout.tiles[index].origin_x;
        packet.tile_origins[index][1] = layout.tiles[index].origin_y;
    }
    packet.checksum = 0;
    packet.checksum = Fnv1a32(&packet, sizeof(packet));
    return true;
}

bool DecodeDownstreamFrameMetadata(
    DownstreamFrameMetadataPacket packet, RemoteVideoFrameLayout& layout)
{
    const uint32_t checksum = packet.checksum;
    packet.checksum = 0;
    if (packet.magic != 0x314d504eu || packet.version != 1 ||
        packet.byte_size != sizeof(packet) || checksum != Fnv1a32(&packet, sizeof(packet)) ||
        !ValidateWireMetadata(packet.wire) || packet.video_width == 0 || packet.video_height == 0 ||
        (packet.video_width & 1u) != 0 || (packet.video_height & 1u) != 0)
        return false;

    const RemoteVideoWireMetadata& wire = packet.wire;
    layout = {};
    layout.video_width = packet.video_width;
    layout.video_height = packet.video_height;
    layout.metadata.frame_id = wire.frame_id;
    layout.metadata.timestamp_usec = wire.timestamp_usec;
    layout.metadata.width = wire.width;
    layout.metadata.height = wire.height;
    layout.metadata.source_generation = wire.source_generation;
    layout.metadata.continuity_mask = wire.continuity_mask;
    layout.metadata.available_buffer_mask = wire.available_buffer_mask;
    layout.metadata.dynamic_range = static_cast<RemoteDynamicRange>(wire.dynamic_range);
    layout.metadata.source_stream_id.assign(wire.source_stream_id,
        strnlen(wire.source_stream_id, sizeof(wire.source_stream_id)));
    std::memcpy(&layout.metadata.view_origin.x, wire.view_origin, sizeof(wire.view_origin));
    std::memcpy(&layout.metadata.view_forward.x, wire.view_forward, sizeof(wire.view_forward));
    std::memcpy(&layout.metadata.temporal_jitter_pixels.x, wire.temporal_jitter_pixels, sizeof(wire.temporal_jitter_pixels));
    CopyMatrixFromWire(layout.metadata.view, wire.view);
    CopyMatrixFromWire(layout.metadata.projection, wire.projection);
    CopyMatrixFromWire(layout.metadata.view_projection, wire.view_projection);
    CopyMatrixFromWire(layout.metadata.inverse_view, wire.inverse_view);
    CopyMatrixFromWire(layout.metadata.inverse_projection, wire.inverse_projection);
    CopyMatrixFromWire(layout.metadata.inverse_view_projection, wire.inverse_view_projection);
    layout.metadata.near_plane = wire.near_plane;
    layout.metadata.far_plane = wire.far_plane;
    layout.metadata.pre_exposure = wire.pre_exposure;
    layout.metadata.history_valid = (wire.flags & kRemoteVideoFlagHistoryValid) != 0;
    layout.metadata.reset_this_frame = (wire.flags & kRemoteVideoFlagResetThisFrame) != 0;
    layout.metadata.camera_cut = (wire.flags & kRemoteVideoFlagCameraCut) != 0;
    layout.metadata.valid = (wire.flags & kRemoteVideoFlagValid) != 0;
    layout.metadata.ddgi_frame_index = wire.ddgi_frame_index;
    layout.metadata.ddgi_reset_reason = static_cast<DDGIResetReason>(wire.ddgi_reset_reason);
    layout.metadata.confidence = wire.confidence;
    layout.metadata.local_receive_timestamp_usec = NowUsec();
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteVideoWireBuffer& source = wire.buffers[index];
        RemoteVideoTileLayout& tile = layout.tiles[index];
        tile.semantic = static_cast<RemoteBufferSemantic>(source.semantic);
        tile.width = source.width;
        tile.height = source.height;
        tile.available = source.available != 0;
        tile.encoding = static_cast<RemoteBufferEncoding>(source.encoding);
        tile.origin_x = packet.tile_origins[index][0];
        tile.origin_y = packet.tile_origins[index][1];
        if (tile.available && (tile.origin_x + tile.width > layout.video_width ||
            tile.origin_y + tile.height > layout.video_height))
            return false;
    }
    return true;
}
} // namespace

struct WebRTCVideoTransport::Impl
{
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    RuntimeConfig requested_config;
    uint64_t request_revision = 0;
    bool requested_server = false;
    bool desired_running = false;
    bool shutdown = false;

    std::mutex bridge_mutex;
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
    std::mutex control_queue_mutex;
    ClientControlPacket pending_control = {};
    std::atomic_bool control_pending{false};
    std::atomic_bool keyframe_pending{false};

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
        result.codec_fallback_reason = native.codec_fallback_reason;
        result.power_efficient_codec = native.power_efficient_codec != 0;
        result.status = native.status;
        return result;
    }

    void PublishStats(WebRTCTransportStats stats)
    {
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
        std::atomic_store(&stats_snapshot,
            std::make_shared<const WebRTCTransportStats>(std::move(stats)));
    }

    NPWebRTCBridge* DetachBridge()
    {
        std::lock_guard lock(bridge_mutex);
        NPWebRTCBridge* detached = bridge;
        bridge = nullptr;
        bridge_attached.store(false, std::memory_order_release);
        return detached;
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
        auto next_retry = std::chrono::steady_clock::now();

        for (;;)
        {
            RuntimeConfig config;
            uint64_t revision = 0;
            bool run = false;
            bool role_server = false;
            {
                std::unique_lock lock(lifecycle_mutex);
                const auto wake_time = bridge_attached.load(std::memory_order_acquire)
                    ? std::chrono::steady_clock::now() + std::chrono::milliseconds(100)
                    : next_retry;
                lifecycle_cv.wait_until(lock, wake_time, [&] {
                    return shutdown || request_revision != applied_revision ||
                        control_pending.load(std::memory_order_acquire) ||
                        (bridge_attached.load(std::memory_order_acquire) &&
                            keyframe_pending.load(std::memory_order_acquire));
                });
                if (shutdown)
                    break;
                revision = request_revision;
                run = desired_running;
                role_server = requested_server;
                config = requested_config;
            }

            if (revision != applied_revision)
            {
                DestroyBridge();
                applied_revision = revision;
                retry_attempt = 0;
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
                std::lock_guard lock(bridge_mutex);
                current = bridge;
            }
            if (current != nullptr)
            {
                WebRTCTransportStats stats;
                {
                    std::lock_guard lock(bridge_mutex);
                    stats = ReadNativeStats(bridge);
                }
                if (!role_server && stats.state == WebRTCTransportState::Connected &&
                    control_pending.load(std::memory_order_acquire))
                {
                    ClientControlPacket packet;
                    {
                        std::lock_guard lock(control_queue_mutex);
                        packet = pending_control;
                        control_pending.store(false, std::memory_order_release);
                    }
                    std::lock_guard lock(bridge_mutex);
                    if (bridge != nullptr && !server)
                    {
                        np_webrtc_bridge_send_control(
                            bridge, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
                    }
                }
                if (role_server && stats.state == WebRTCTransportState::Connected &&
                    keyframe_pending.exchange(false, std::memory_order_acq_rel))
                {
                    std::lock_guard lock(bridge_mutex);
                    if (bridge != nullptr)
                        np_webrtc_bridge_request_keyframe(bridge);
                }
                PublishStats(stats);
                if (stats.state != WebRTCTransportState::Failed)
                    continue;

                DestroyBridge();
                const auto delay = RetryDelay(retry_attempt++, applied_revision);
                next_retry = std::chrono::steady_clock::now() + delay;
                stats.status += "; retry in " + std::to_string(delay.count()) + " ms";
                PublishStats(std::move(stats));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < next_retry)
            {
                std::unique_lock lock(lifecycle_mutex);
                lifecycle_cv.wait_until(lock, next_retry, [&] {
                    return shutdown || request_revision != applied_revision ||
                        control_pending.load(std::memory_order_acquire) ||
                        (bridge_attached.load(std::memory_order_acquire) &&
                            keyframe_pending.load(std::memory_order_acquire));
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
                config.use_internet_ice ? 1 : 0);
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
                std::lock_guard lock(bridge_mutex);
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

WebRTCVideoTransport::WebRTCVideoTransport() : impl(std::make_unique<Impl>())
{
    impl->lifecycle_thread = std::thread([this] { impl->LifecycleLoop(); });
}

WebRTCVideoTransport::~WebRTCVideoTransport()
{
    Stop();
}

bool WebRTCVideoTransport::RequestStart(bool server, const RuntimeConfig& config, std::string* error)
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
    static_assert(std::is_trivially_copyable_v<ClientControlPacket>);
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
    static_assert(std::is_trivially_copyable_v<ClientControlPacket>);
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::unique_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl->bridge == nullptr || !impl->server)
        return false;
    size_t required = 0;
    const int result = np_webrtc_bridge_receive_control(
        impl->bridge, reinterpret_cast<uint8_t*>(&packet), sizeof(packet), &required);
    return result == 1 && required == sizeof(packet);
}

bool WebRTCVideoTransport::SendFrame(const RemoteRawFrame& frame)
{
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
    {
        if (impl)
            impl->disconnected_frame_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    PackedRemoteVideoFrame video;
    const uint64_t conversion_begin = NowUsec();
    if (!EncodeRemoteVideoFrame(frame, video, nullptr))
        return false;
    impl->cpu_conversion_usec.fetch_add(NowUsec() - conversion_begin, std::memory_order_relaxed);
    std::unique_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl->bridge == nullptr || !impl->server)
    {
        impl->busy_frame_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool sent = np_webrtc_bridge_send_i420(
        impl->bridge,
        video.width,
        video.height,
        video.i420.data(),
        video.i420.size(),
        static_cast<int64_t>(frame.metadata.timestamp_usec)) == 1;
    if (sent)
        impl->cpu_full_frame_copy_bytes.fetch_add(video.i420.size(), std::memory_order_relaxed);
    return sent;
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
    std::unique_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl->bridge == nullptr || !impl->server)
    {
        impl->busy_frame_drops.fetch_add(1, std::memory_order_relaxed);
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

bool WebRTCVideoTransport::SendFrameMetadata(const RemoteVideoFrameLayout& layout)
{
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    DownstreamFrameMetadataPacket packet;
    if (!EncodeDownstreamFrameMetadata(layout, packet))
        return false;
    std::unique_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl->bridge == nullptr || !impl->server)
        return false;
    return np_webrtc_bridge_send_frame_metadata(
        impl->bridge, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)) == 1;
}

void WebRTCVideoTransport::RequestKeyframe()
{
    if (!impl)
        return;
    impl->keyframe_pending.store(true, std::memory_order_release);
    impl->lifecycle_cv.notify_one();
}

bool WebRTCVideoTransport::TryReceiveFrameMetadata(RemoteVideoFrameLayout& layout)
{
    layout = {};
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::unique_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl->bridge == nullptr || impl->server)
        return false;
    size_t required = 0;
    int result = np_webrtc_bridge_receive_frame_metadata(impl->bridge, nullptr, 0, &required);
    if (result == 0 || required != sizeof(DownstreamFrameMetadataPacket))
        return false;
    DownstreamFrameMetadataPacket packet;
    result = np_webrtc_bridge_receive_frame_metadata(
        impl->bridge, reinterpret_cast<uint8_t*>(&packet), sizeof(packet), &required);
    return result == 1 && required == sizeof(packet) && DecodeDownstreamFrameMetadata(packet, layout);
}

bool WebRTCVideoTransport::TryAcquireI420Frame(RetainedI420Frame& frame)
{
    frame = {};
    if (!impl || GetStats().state != WebRTCTransportState::Connected)
        return false;
    std::unique_lock lock(impl->bridge_mutex, std::try_to_lock);
    if (!lock.owns_lock() || impl->bridge == nullptr || impl->server)
        return false;
    NPWebRTCVideoFrame* retained = nullptr;
    if (np_webrtc_bridge_acquire_i420_frame(impl->bridge, &retained) != 1 || retained == nullptr)
        return false;
    frame.frame_lifetime = std::shared_ptr<void>(retained, [](void* value) {
        np_webrtc_video_frame_release(static_cast<NPWebRTCVideoFrame*>(value));
    });
    if (np_webrtc_video_frame_get_i420(
            retained,
            &frame.width,
            &frame.height,
            &frame.y_plane,
            &frame.y_stride,
            &frame.u_plane,
            &frame.u_stride,
            &frame.v_plane,
            &frame.v_stride,
            &frame.timestamp_usec) != 1 ||
        !frame.IsValid())
    {
        frame = {};
        return false;
    }
    const uint64_t y_bytes = static_cast<uint64_t>(frame.width) * frame.height;
    const uint64_t uv_bytes = static_cast<uint64_t>(frame.width / 2u) * (frame.height / 2u);
    impl->retained_frame_acquires.fetch_add(1, std::memory_order_relaxed);
    impl->retained_i420_bytes.fetch_add(y_bytes + uv_bytes * 2u, std::memory_order_relaxed);
    return true;
}

bool WebRTCVideoTransport::TryReceiveFrame(RemoteRawFrame& frame)
{
    RetainedI420Frame retained;
    if (!TryAcquireI420Frame(retained))
        return false;
    const uint64_t conversion_begin = NowUsec();
    const bool decoded = DecodeRemoteVideoFrame(retained, frame, nullptr);
    impl->cpu_conversion_usec.fetch_add(NowUsec() - conversion_begin, std::memory_order_relaxed);
    return decoded;
}

WebRTCTransportStats WebRTCVideoTransport::GetStats() const
{
    if (!impl)
        return {};
    const std::shared_ptr<const WebRTCTransportStats> snapshot = std::atomic_load(&impl->stats_snapshot);
    return snapshot ? *snapshot : WebRTCTransportStats{};
}
} // namespace wicked_newpipeline
