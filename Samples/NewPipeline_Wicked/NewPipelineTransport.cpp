#include "NewPipelineTransport.h"
#include "NewPipelineWebRTCBridge.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
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

#pragma pack(push, 1)
struct RemoteVideoWireBuffer
{
    uint32_t semantic = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t available = 0;
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
    uint32_t flags = 0;
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
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<RemoteVideoWireMetadata>);

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
        wire.width > kMaxRemoteBufferDimension || wire.height > kMaxRemoteBufferDimension)
    {
        return false;
    }

    for (size_t index = 0; index < std::size(wire.buffers); ++index)
    {
        const RemoteVideoWireBuffer& buffer = wire.buffers[index];
        if (buffer.semantic != index || buffer.available > 1u ||
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
        buffers[index].semantic = static_cast<RemoteBufferSemantic>(index);
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
        if (buffer.semantic != static_cast<RemoteBufferSemantic>(index) || !ValidateBuffer(buffer))
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
    const uint32_t video_width = tile_width * 2u;
    const uint32_t metadata_rows = MetadataRows(video_width);
    if (metadata_rows == 0 || tile_width > std::numeric_limits<uint32_t>::max() / 2u ||
        tile_height > (std::numeric_limits<uint32_t>::max() - metadata_rows) / 2u)
    {
        SetError(error, "remote video dimensions overflow");
        return false;
    }
    const uint32_t video_height = metadata_rows + tile_height * 2u;
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

        const uint32_t origin_x = static_cast<uint32_t>(buffer_index & 1u) * tile_width;
        const uint32_t origin_y = metadata_rows + static_cast<uint32_t>(buffer_index / 2u) * tile_height;
        for (uint32_t y = 0; y < buffer.height; ++y)
        {
            for (uint32_t x = 0; x < buffer.width; ++x)
            {
                const size_t source_index = (static_cast<size_t>(y) * buffer.width + x) * 4u;
                uint8_t y_value = 16;
                uint8_t u_value = 128;
                uint8_t v_value = 128;
                RGBToYUV(buffer.payload_rgba8[source_index + 0u],
                    buffer.payload_rgba8[source_index + 1u],
                    buffer.payload_rgba8[source_index + 2u],
                    y_value, u_value, v_value);
                y_plane[static_cast<size_t>(origin_y + y) * video_width + origin_x + x] = y_value;
            }
        }

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

bool DecodeRemoteVideoFrame(const PackedRemoteVideoFrame& video, RemoteRawFrame& frame, std::string* error)
{
    frame = {};
    if (video.width < kMetadataBitCellSize || video.height == 0 || (video.width & 1u) != 0 || (video.height & 1u) != 0)
    {
        SetError(error, "remote video dimensions are invalid");
        return false;
    }
    size_t y_size = 0;
    size_t uv_size = 0;
    if (!CheckedImageByteSize(video.width, video.height, 1u, y_size) ||
        !CheckedImageByteSize(video.width / 2u, video.height / 2u, 1u, uv_size) ||
        video.i420.size() != y_size + uv_size * 2u)
    {
        SetError(error, "remote I420 payload size is invalid");
        return false;
    }

    const uint32_t metadata_rows = MetadataRows(video.width);
    if (metadata_rows == 0 || metadata_rows >= video.height)
    {
        SetError(error, "remote video metadata band is invalid");
        return false;
    }

    const uint8_t* y_plane = video.i420.data();
    const uint8_t* u_plane = y_plane + y_size;
    const uint8_t* v_plane = u_plane + uv_size;
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
                sum += y_plane[static_cast<size_t>(cell_y + y) * video.width + cell_x + x];
        }
        if (sum >= 128u * kMetadataBitCellSize * kMetadataBitCellSize)
            wire_bytes[bit_index / 8ull] |= static_cast<uint8_t>(1u << (bit_index & 7ull));
    }

    if (!ValidateWireMetadata(wire) || wire.width * 2u != video.width ||
        metadata_rows + wire.height * 2u != video.height)
    {
        SetError(error, "remote video metadata checksum or layout is invalid");
        return false;
    }

    frame.metadata.frame_id = wire.frame_id;
    frame.metadata.timestamp_usec = wire.timestamp_usec;
    frame.metadata.width = wire.width;
    frame.metadata.height = wire.height;
    frame.metadata.source_generation = wire.source_generation;
    frame.metadata.continuity_mask = wire.continuity_mask;
    frame.metadata.available_buffer_mask = wire.available_buffer_mask;
    frame.metadata.dynamic_range = static_cast<RemoteDynamicRange>(wire.dynamic_range);
    frame.metadata.source_stream_id.assign(wire.source_stream_id,
        strnlen(wire.source_stream_id, sizeof(wire.source_stream_id)));
    std::memcpy(&frame.metadata.view_origin.x, wire.view_origin, sizeof(wire.view_origin));
    std::memcpy(&frame.metadata.view_forward.x, wire.view_forward, sizeof(wire.view_forward));
    std::memcpy(&frame.metadata.temporal_jitter_pixels.x, wire.temporal_jitter_pixels, sizeof(wire.temporal_jitter_pixels));
    CopyMatrixFromWire(frame.metadata.view, wire.view);
    CopyMatrixFromWire(frame.metadata.projection, wire.projection);
    CopyMatrixFromWire(frame.metadata.view_projection, wire.view_projection);
    CopyMatrixFromWire(frame.metadata.inverse_view, wire.inverse_view);
    CopyMatrixFromWire(frame.metadata.inverse_projection, wire.inverse_projection);
    CopyMatrixFromWire(frame.metadata.inverse_view_projection, wire.inverse_view_projection);
    frame.metadata.near_plane = wire.near_plane;
    frame.metadata.far_plane = wire.far_plane;
    frame.metadata.pre_exposure = wire.pre_exposure;
    frame.metadata.history_valid = (wire.flags & kRemoteVideoFlagHistoryValid) != 0;
    frame.metadata.reset_this_frame = (wire.flags & kRemoteVideoFlagResetThisFrame) != 0;
    frame.metadata.camera_cut = (wire.flags & kRemoteVideoFlagCameraCut) != 0;
    frame.metadata.valid = (wire.flags & kRemoteVideoFlagValid) != 0;
    frame.metadata.confidence = wire.confidence;
    frame.metadata.local_receive_timestamp_usec = NowUsec();

    const uint32_t chroma_width = video.width / 2u;
    for (size_t buffer_index = 0; buffer_index < frame.buffers.size(); ++buffer_index)
    {
        const RemoteVideoWireBuffer& source = wire.buffers[buffer_index];
        RemoteRawBuffer& buffer = frame.buffers[buffer_index];
        buffer.semantic = static_cast<RemoteBufferSemantic>(source.semantic);
        buffer.width = source.width;
        buffer.height = source.height;
        buffer.available = source.available != 0;
        if (!buffer.available)
            continue;

        size_t payload_size = 0;
        if (!CheckedImageByteSize(buffer.width, buffer.height, 4u, payload_size))
        {
            SetError(error, "decoded remote buffer size overflow");
            return false;
        }
        buffer.payload_rgba8.resize(payload_size);
        const uint32_t origin_x = static_cast<uint32_t>(buffer_index & 1u) * wire.width;
        const uint32_t origin_y = metadata_rows + static_cast<uint32_t>(buffer_index / 2u) * wire.height;
        for (uint32_t y = 0; y < buffer.height; ++y)
        {
            for (uint32_t x = 0; x < buffer.width; ++x)
            {
                const uint8_t y_value = y_plane[static_cast<size_t>(origin_y + y) * video.width + origin_x + x];
                const size_t chroma_index = static_cast<size_t>((origin_y + y) / 2u) * chroma_width +
                    (origin_x + x) / 2u;
                uint8_t red = 0;
                uint8_t green = 0;
                uint8_t blue = 0;
                YUVToRGB(y_value, u_plane[chroma_index], v_plane[chroma_index], red, green, blue);
                const size_t destination_index = (static_cast<size_t>(y) * buffer.width + x) * 4u;
                buffer.payload_rgba8[destination_index + 0u] = red;
                buffer.payload_rgba8[destination_index + 1u] = green;
                buffer.payload_rgba8[destination_index + 2u] = blue;
                buffer.payload_rgba8[destination_index + 3u] = 255u;
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

struct WebRTCVideoTransport::Impl
{
    NPWebRTCBridge* bridge = nullptr;
    bool server = false;
};

WebRTCVideoTransport::WebRTCVideoTransport() : impl(std::make_unique<Impl>())
{
}

WebRTCVideoTransport::~WebRTCVideoTransport()
{
    Stop();
}

bool WebRTCVideoTransport::Start(bool server, const RuntimeConfig& config, std::string* error)
{
    Stop();
    impl->server = server;
    impl->bridge = np_webrtc_bridge_create(
        server ? 1 : 0,
        config.signaling_url.c_str(),
        config.room_id.c_str(),
        config.use_internet_ice ? 1 : 0);
    if (impl->bridge == nullptr)
    {
        SetError(error, "failed to create native WebRTC video transport");
        return false;
    }
    return true;
}

void WebRTCVideoTransport::Stop()
{
    if (impl && impl->bridge != nullptr)
    {
        np_webrtc_bridge_destroy(impl->bridge);
        impl->bridge = nullptr;
    }
}

void WebRTCVideoTransport::Tick()
{
    // Native libwebrtc and the signaling WebSocket own their worker threads.
}

bool WebRTCVideoTransport::SendControl(const ClientControlPacket& packet)
{
    static_assert(std::is_trivially_copyable_v<ClientControlPacket>);
    return impl && impl->bridge && !impl->server &&
        np_webrtc_bridge_send_control(impl->bridge, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)) == 1;
}

bool WebRTCVideoTransport::TryReceiveControl(ClientControlPacket& packet)
{
    static_assert(std::is_trivially_copyable_v<ClientControlPacket>);
    if (!impl || !impl->bridge || !impl->server)
        return false;
    size_t required = 0;
    const int result = np_webrtc_bridge_receive_control(
        impl->bridge, reinterpret_cast<uint8_t*>(&packet), sizeof(packet), &required);
    return result == 1 && required == sizeof(packet);
}

bool WebRTCVideoTransport::SendFrame(const RemoteRawFrame& frame)
{
    if (!impl || !impl->bridge || !impl->server)
        return false;
    PackedRemoteVideoFrame video;
    if (!EncodeRemoteVideoFrame(frame, video, nullptr))
        return false;
    return np_webrtc_bridge_send_i420(
        impl->bridge,
        video.width,
        video.height,
        video.i420.data(),
        video.i420.size(),
        static_cast<int64_t>(frame.metadata.timestamp_usec)) == 1;
}

bool WebRTCVideoTransport::TryReceiveFrame(RemoteRawFrame& frame)
{
    if (!impl || !impl->bridge || impl->server)
        return false;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t required = 0;
    int result = np_webrtc_bridge_receive_i420(impl->bridge, &width, &height, nullptr, 0, &required);
    if (result == 0 || required == 0)
        return false;
    PackedRemoteVideoFrame video;
    video.width = width;
    video.height = height;
    video.i420.resize(required);
    result = np_webrtc_bridge_receive_i420(
        impl->bridge, &video.width, &video.height, video.i420.data(), video.i420.size(), &required);
    return result == 1 && required == video.i420.size() && DecodeRemoteVideoFrame(video, frame, nullptr);
}

WebRTCTransportStats WebRTCVideoTransport::GetStats() const
{
    WebRTCTransportStats result;
    if (!impl || !impl->bridge)
        return result;
    NPWebRTCBridgeStats native = {};
    np_webrtc_bridge_get_stats(impl->bridge, &native);
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
    result.sent_controls = native.sent_controls;
    result.received_controls = native.received_controls;
    result.status = native.status;
    return result;
}
} // namespace wicked_newpipeline
