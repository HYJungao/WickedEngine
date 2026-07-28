#include "NewPipelineProtocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace wicked_newpipeline
{
namespace
{
constexpr size_t kDescriptorBytes = 44;
constexpr size_t kHeaderBytes = 36;
constexpr size_t kContractBytes = kHeaderBytes +
    kDescriptorBytes * static_cast<size_t>(RemoteBufferSemantic::Count);

void SetError(std::string* error, const std::string& value)
{
    if (error != nullptr) *error = value;
}

void WriteU8(std::vector<uint8_t>& bytes, uint8_t value) { bytes.push_back(value); }
void WriteU16(std::vector<uint8_t>& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8u));
}
void WriteU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (uint32_t shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(value >> shift));
}
void WriteU64(std::vector<uint8_t>& bytes, uint64_t value)
{
    for (uint32_t shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<uint8_t>(value >> shift));
}
void WriteFloat(std::vector<uint8_t>& bytes, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(bytes, bits);
}

bool ReadU8(const uint8_t*& cursor, const uint8_t* end, uint8_t& value)
{
    if (cursor >= end) return false;
    value = *cursor++;
    return true;
}
bool ReadU16(const uint8_t*& cursor, const uint8_t* end, uint16_t& value)
{
    if (static_cast<size_t>(end - cursor) < 2) return false;
    value = static_cast<uint16_t>(cursor[0]) | static_cast<uint16_t>(cursor[1] << 8u);
    cursor += 2;
    return true;
}
bool ReadU32(const uint8_t*& cursor, const uint8_t* end, uint32_t& value)
{
    if (static_cast<size_t>(end - cursor) < 4) return false;
    value = 0;
    for (uint32_t shift = 0; shift < 32; shift += 8) value |= static_cast<uint32_t>(*cursor++) << shift;
    return true;
}
bool ReadU64(const uint8_t*& cursor, const uint8_t* end, uint64_t& value)
{
    if (static_cast<size_t>(end - cursor) < 8) return false;
    value = 0;
    for (uint32_t shift = 0; shift < 64; shift += 8) value |= static_cast<uint64_t>(*cursor++) << shift;
    return true;
}
bool ReadFloat(const uint8_t*& cursor, const uint8_t* end, float& value)
{
    uint32_t bits = 0;
    if (!ReadU32(cursor, end, bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
}

void WriteFloat3(std::vector<uint8_t>& bytes, const XMFLOAT3& value)
{
    WriteFloat(bytes, value.x);
    WriteFloat(bytes, value.y);
    WriteFloat(bytes, value.z);
}
bool ReadFloat3(const uint8_t*& cursor, const uint8_t* end, XMFLOAT3& value)
{
    return ReadFloat(cursor, end, value.x) && ReadFloat(cursor, end, value.y) &&
        ReadFloat(cursor, end, value.z);
}
void WriteMatrix(std::vector<uint8_t>& bytes, const XMFLOAT4X4& value)
{
    const float* elements = &value._11;
    for (size_t i = 0; i < 16; ++i) WriteFloat(bytes, elements[i]);
}
bool ReadMatrix(const uint8_t*& cursor, const uint8_t* end, XMFLOAT4X4& value)
{
    float* elements = &value._11;
    for (size_t i = 0; i < 16; ++i)
    {
        if (!ReadFloat(cursor, end, elements[i])) return false;
    }
    return true;
}

uint32_t FNV1a32(const uint8_t* bytes, size_t count)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

bool IsAvailable(const RemoteBufferDescriptorV3& descriptor)
{
    return (descriptor.flags & kRemoteBufferDescriptorAvailableV3) != 0;
}

bool RectsOverlap(const RemoteBufferDescriptorV3& a, const RemoteBufferDescriptorV3& b)
{
    return a.atlas_x < static_cast<uint32_t>(b.atlas_x) + b.atlas_width &&
        b.atlas_x < static_cast<uint32_t>(a.atlas_x) + a.atlas_width &&
        a.atlas_y < static_cast<uint32_t>(b.atlas_y) + b.atlas_height &&
        b.atlas_y < static_cast<uint32_t>(a.atlas_y) + a.atlas_height;
}

void WriteDescriptor(std::vector<uint8_t>& bytes, const RemoteBufferDescriptorV3& descriptor)
{
    WriteU8(bytes, static_cast<uint8_t>(descriptor.semantic));
    WriteU8(bytes, static_cast<uint8_t>(descriptor.representation));
    WriteU8(bytes, static_cast<uint8_t>(descriptor.encoding));
    WriteU8(bytes, descriptor.flags);
    WriteU16(bytes, descriptor.logical_width);
    WriteU16(bytes, descriptor.logical_height);
    WriteU16(bytes, descriptor.atlas_x);
    WriteU16(bytes, descriptor.atlas_y);
    WriteU16(bytes, descriptor.atlas_width);
    WriteU16(bytes, descriptor.atlas_height);
    WriteU64(bytes, descriptor.content_frame_id);
    WriteU32(bytes, descriptor.content_generation);
    WriteU16(bytes, descriptor.confidence_unorm);
    WriteU16(bytes, descriptor.reserved);
    WriteU64(bytes, descriptor.stable_subject_id);
    WriteU32(bytes, descriptor.stable_subject_generation);
}

bool ReadDescriptor(const uint8_t*& cursor, const uint8_t* end, RemoteBufferDescriptorV3& descriptor)
{
    uint8_t semantic = 0, representation = 0, encoding = 0;
    if (!ReadU8(cursor, end, semantic) || !ReadU8(cursor, end, representation) ||
        !ReadU8(cursor, end, encoding) || !ReadU8(cursor, end, descriptor.flags) ||
        !ReadU16(cursor, end, descriptor.logical_width) || !ReadU16(cursor, end, descriptor.logical_height) ||
        !ReadU16(cursor, end, descriptor.atlas_x) || !ReadU16(cursor, end, descriptor.atlas_y) ||
        !ReadU16(cursor, end, descriptor.atlas_width) || !ReadU16(cursor, end, descriptor.atlas_height) ||
        !ReadU64(cursor, end, descriptor.content_frame_id) ||
        !ReadU32(cursor, end, descriptor.content_generation) ||
        !ReadU16(cursor, end, descriptor.confidence_unorm) || !ReadU16(cursor, end, descriptor.reserved) ||
        !ReadU64(cursor, end, descriptor.stable_subject_id) ||
        !ReadU32(cursor, end, descriptor.stable_subject_generation))
        return false;
    descriptor.semantic = static_cast<RemoteBufferSemantic>(semantic);
    descriptor.representation = static_cast<RemoteBufferRepresentationV3>(representation);
    descriptor.encoding = static_cast<RemoteBufferEncoding>(encoding);
    return true;
}
} // namespace

bool SerializeClientControlPacket(
    const ClientControlPacket& packet, std::vector<uint8_t>& bytes, std::string* error)
{
    if (packet.control_frame_id == 0 || packet.frame_id == 0 ||
        packet.viewport_width == 0 || packet.viewport_height == 0 ||
        packet.near_plane <= 0 || packet.far_plane <= packet.near_plane ||
        packet.supported_protocol_versions == 0)
    {
        SetError(error, "invalid client control identity, viewport, clip planes, or capabilities");
        return false;
    }

    bytes.clear();
    bytes.reserve(320);
    WriteU32(bytes, kControlWireMagicV2);
    WriteU32(bytes, kControlWireVersionV2);
    WriteU32(bytes, 0); // byte size
    WriteU32(bytes, 0); // checksum
    WriteU64(bytes, packet.control_frame_id);
    WriteU64(bytes, packet.frame_id);
    WriteU64(bytes, packet.timestamp_usec);
    WriteU32(bytes, packet.viewport_width);
    WriteU32(bytes, packet.viewport_height);
    WriteU32(bytes, packet.scene_generation);
    WriteFloat(bytes, packet.near_plane);
    WriteFloat(bytes, packet.far_plane);
    WriteFloat3(bytes, packet.eye);
    WriteFloat3(bytes, packet.at);
    WriteFloat3(bytes, packet.up);
    WriteMatrix(bytes, packet.view);
    WriteMatrix(bytes, packet.projection);
    WriteU8(bytes, packet.sun_enabled ? 1u : 0u);
    WriteU8(bytes, 0);
    WriteU16(bytes, 0);
    WriteFloat3(bytes, packet.sun_direction);
    WriteFloat3(bytes, packet.sun_color);
    WriteFloat(bytes, packet.sun_intensity);
    WriteFloat3(bytes, packet.ambient);
    WriteFloat3(bytes, packet.horizon);
    WriteFloat3(bytes, packet.zenith);
    WriteU32(bytes, packet.supported_protocol_versions);
    WriteU32(bytes, packet.supported_quality_tiers);
    WriteU32(bytes, packet.supported_encoding_profiles);
    WriteU32(bytes, packet.preferred_protocol_version);
    WriteU8(bytes, static_cast<uint8_t>(packet.preferred_quality_tier));
    WriteU8(bytes, 0);
    WriteU16(bytes, 0);

    const uint32_t encoded_size = static_cast<uint32_t>(bytes.size());
    bytes[8] = static_cast<uint8_t>(encoded_size);
    bytes[9] = static_cast<uint8_t>(encoded_size >> 8u);
    bytes[10] = static_cast<uint8_t>(encoded_size >> 16u);
    bytes[11] = static_cast<uint8_t>(encoded_size >> 24u);
    const uint32_t checksum = FNV1a32(bytes.data() + 16, bytes.size() - 16);
    bytes[12] = static_cast<uint8_t>(checksum);
    bytes[13] = static_cast<uint8_t>(checksum >> 8u);
    bytes[14] = static_cast<uint8_t>(checksum >> 16u);
    bytes[15] = static_cast<uint8_t>(checksum >> 24u);
    return true;
}

bool DeserializeClientControlPacket(
    const uint8_t* bytes, size_t byte_count, ClientControlPacket& packet, std::string* error)
{
    if (bytes == nullptr || byte_count < 16)
    {
        SetError(error, "client control packet is truncated");
        return false;
    }
    const uint8_t* cursor = bytes;
    const uint8_t* end = bytes + byte_count;
    uint32_t magic = 0, version = 0, encoded_size = 0, checksum = 0;
    uint8_t sun_enabled = 0, quality = 0, reserved8 = 0;
    uint16_t reserved16 = 0;
    ClientControlPacket decoded;
    if (!ReadU32(cursor, end, magic) || !ReadU32(cursor, end, version) ||
        !ReadU32(cursor, end, encoded_size) || !ReadU32(cursor, end, checksum) ||
        magic != kControlWireMagicV2 || version != kControlWireVersionV2 ||
        encoded_size != byte_count || checksum != FNV1a32(bytes + 16, byte_count - 16) ||
        !ReadU64(cursor, end, decoded.control_frame_id) ||
        !ReadU64(cursor, end, decoded.frame_id) ||
        !ReadU64(cursor, end, decoded.timestamp_usec) ||
        !ReadU32(cursor, end, decoded.viewport_width) ||
        !ReadU32(cursor, end, decoded.viewport_height) ||
        !ReadU32(cursor, end, decoded.scene_generation) ||
        !ReadFloat(cursor, end, decoded.near_plane) ||
        !ReadFloat(cursor, end, decoded.far_plane) ||
        !ReadFloat3(cursor, end, decoded.eye) ||
        !ReadFloat3(cursor, end, decoded.at) ||
        !ReadFloat3(cursor, end, decoded.up) ||
        !ReadMatrix(cursor, end, decoded.view) ||
        !ReadMatrix(cursor, end, decoded.projection) ||
        !ReadU8(cursor, end, sun_enabled) || !ReadU8(cursor, end, reserved8) ||
        !ReadU16(cursor, end, reserved16) || reserved8 != 0 || reserved16 != 0 ||
        !ReadFloat3(cursor, end, decoded.sun_direction) ||
        !ReadFloat3(cursor, end, decoded.sun_color) ||
        !ReadFloat(cursor, end, decoded.sun_intensity) ||
        !ReadFloat3(cursor, end, decoded.ambient) ||
        !ReadFloat3(cursor, end, decoded.horizon) ||
        !ReadFloat3(cursor, end, decoded.zenith) ||
        !ReadU32(cursor, end, decoded.supported_protocol_versions) ||
        !ReadU32(cursor, end, decoded.supported_quality_tiers) ||
        !ReadU32(cursor, end, decoded.supported_encoding_profiles) ||
        !ReadU32(cursor, end, decoded.preferred_protocol_version) ||
        !ReadU8(cursor, end, quality) || !ReadU8(cursor, end, reserved8) ||
        !ReadU16(cursor, end, reserved16) || reserved8 != 0 || reserved16 != 0 ||
        cursor != end || sun_enabled > 1 ||
        quality > static_cast<uint8_t>(RemoteQualityTierV3::Low))
    {
        SetError(error, "invalid client control wire packet");
        return false;
    }
    decoded.sun_enabled = sun_enabled != 0;
    decoded.preferred_quality_tier = static_cast<RemoteQualityTierV3>(quality);
    if (decoded.control_frame_id == 0 || decoded.frame_id == 0 ||
        decoded.viewport_width == 0 || decoded.viewport_height == 0 ||
        decoded.near_plane <= 0 || decoded.far_plane <= decoded.near_plane ||
        decoded.supported_protocol_versions == 0)
    {
        SetError(error, "client control payload violates the protocol contract");
        return false;
    }
    packet = decoded;
    return true;
}

RemoteStreamSelection NegotiateRemoteStream(const ClientControlPacket& packet)
{
    RemoteStreamSelection selection;
    const bool v2 = (packet.supported_protocol_versions & kRemoteProtocolCapabilityV2) != 0;
    const bool v3 = (packet.supported_protocol_versions & kRemoteProtocolCapabilityV3) != 0 &&
        (packet.supported_encoding_profiles & (1u << kRemoteEncodingProfileI420V3)) != 0;
    if (packet.preferred_protocol_version == kRemoteVideoWireVersion && v2)
        return selection;
    if (!v3)
    {
        if (!v2)
            selection.protocol_version = 0;
        return selection;
    }

    selection.protocol_version = kRemoteVideoWireVersionV3;
    selection.encoding_profile_id = kRemoteEncodingProfileI420V3;
    const uint32_t preferred = static_cast<uint32_t>(packet.preferred_quality_tier);
    if (preferred <= static_cast<uint32_t>(RemoteQualityTierV3::Low) &&
        (packet.supported_quality_tiers & (1u << preferred)) != 0)
    {
        selection.quality_tier = packet.preferred_quality_tier;
    }
    else if ((packet.supported_quality_tiers & kRemoteQualityCapabilityBalanced) != 0)
    {
        selection.quality_tier = RemoteQualityTierV3::Balanced;
    }
    else if ((packet.supported_quality_tiers & kRemoteQualityCapabilityHigh) != 0)
    {
        selection.quality_tier = RemoteQualityTierV3::High;
    }
    else if ((packet.supported_quality_tiers & kRemoteQualityCapabilityLow) != 0)
    {
        selection.quality_tier = RemoteQualityTierV3::Low;
    }
    else
    {
        selection.protocol_version = v2 ? kRemoteVideoWireVersion : 0;
        selection.encoding_profile_id = 0;
        selection.quality_tier = RemoteQualityTierV3::High;
    }
    return selection;
}

RemoteStreamStatus BuildRemoteStreamStatus(const ClientControlPacket& packet)
{
    RemoteStreamStatus status;
    status.control_frame_id = packet.control_frame_id;
    status.selection = NegotiateRemoteStream(packet);
    if (status.selection.protocol_version != 0)
    {
        status.code = RemoteStreamStatusCode::Selected;
        return status;
    }

    const bool supports_v2 =
        (packet.supported_protocol_versions & kRemoteProtocolCapabilityV2) != 0;
    const bool supports_v3 =
        (packet.supported_protocol_versions & kRemoteProtocolCapabilityV3) != 0;
    if (!supports_v2 && !supports_v3)
    {
        status.code = RemoteStreamStatusCode::NoCommonProtocol;
    }
    else if (supports_v3 &&
        (packet.supported_encoding_profiles &
            (1u << kRemoteEncodingProfileI420V3)) == 0)
    {
        status.code = RemoteStreamStatusCode::NoCommonEncodingProfile;
    }
    else
    {
        status.code = RemoteStreamStatusCode::NoCommonQualityTier;
    }
    return status;
}

bool SerializeRemoteStreamStatus(
    const RemoteStreamStatus& status,
    std::vector<uint8_t>& bytes,
    std::string* error)
{
    if (status.control_frame_id == 0 ||
        static_cast<uint8_t>(status.code) >
            static_cast<uint8_t>(RemoteStreamStatusCode::NoCommonQualityTier) ||
        static_cast<uint8_t>(status.selection.quality_tier) >
            static_cast<uint8_t>(RemoteQualityTierV3::Low) ||
        (status.code == RemoteStreamStatusCode::Selected &&
            status.selection.protocol_version == 0) ||
        (status.code != RemoteStreamStatusCode::Selected &&
            status.selection.protocol_version != 0))
    {
        SetError(error, "invalid remote stream status");
        return false;
    }

    bytes.clear();
    bytes.reserve(40);
    WriteU32(bytes, kStreamStatusWireMagicV3);
    WriteU32(bytes, kStreamStatusWireVersionV3);
    WriteU32(bytes, 0);
    WriteU32(bytes, 0);
    WriteU64(bytes, status.control_frame_id);
    WriteU8(bytes, static_cast<uint8_t>(status.code));
    WriteU8(bytes, static_cast<uint8_t>(status.selection.quality_tier));
    WriteU16(bytes, 0);
    WriteU32(bytes, status.selection.protocol_version);
    WriteU32(bytes, status.selection.encoding_profile_id);
    const uint32_t encoded_size = static_cast<uint32_t>(bytes.size());
    bytes[8] = static_cast<uint8_t>(encoded_size);
    bytes[9] = static_cast<uint8_t>(encoded_size >> 8u);
    bytes[10] = static_cast<uint8_t>(encoded_size >> 16u);
    bytes[11] = static_cast<uint8_t>(encoded_size >> 24u);
    const uint32_t checksum = FNV1a32(bytes.data() + 16, bytes.size() - 16);
    bytes[12] = static_cast<uint8_t>(checksum);
    bytes[13] = static_cast<uint8_t>(checksum >> 8u);
    bytes[14] = static_cast<uint8_t>(checksum >> 16u);
    bytes[15] = static_cast<uint8_t>(checksum >> 24u);
    return true;
}

bool DeserializeRemoteStreamStatus(
    const uint8_t* bytes,
    size_t byte_count,
    RemoteStreamStatus& status,
    std::string* error)
{
    if (bytes == nullptr || byte_count < 16)
    {
        SetError(error, "remote stream status is truncated");
        return false;
    }
    const uint8_t* cursor = bytes;
    const uint8_t* end = bytes + byte_count;
    uint32_t magic = 0, version = 0, encoded_size = 0, checksum = 0;
    uint8_t code = 0, quality = 0;
    uint16_t reserved = 0;
    RemoteStreamStatus decoded;
    if (!ReadU32(cursor, end, magic) || !ReadU32(cursor, end, version) ||
        !ReadU32(cursor, end, encoded_size) || !ReadU32(cursor, end, checksum) ||
        magic != kStreamStatusWireMagicV3 ||
        version != kStreamStatusWireVersionV3 ||
        encoded_size != byte_count ||
        checksum != FNV1a32(bytes + 16, byte_count - 16) ||
        !ReadU64(cursor, end, decoded.control_frame_id) ||
        !ReadU8(cursor, end, code) || !ReadU8(cursor, end, quality) ||
        !ReadU16(cursor, end, reserved) ||
        !ReadU32(cursor, end, decoded.selection.protocol_version) ||
        !ReadU32(cursor, end, decoded.selection.encoding_profile_id) ||
        cursor != end || reserved != 0 ||
        code > static_cast<uint8_t>(RemoteStreamStatusCode::NoCommonQualityTier) ||
        quality > static_cast<uint8_t>(RemoteQualityTierV3::Low))
    {
        SetError(error, "invalid remote stream status wire packet");
        return false;
    }
    decoded.code = static_cast<RemoteStreamStatusCode>(code);
    decoded.selection.quality_tier =
        static_cast<RemoteQualityTierV3>(quality);
    if (decoded.control_frame_id == 0 ||
        (decoded.code == RemoteStreamStatusCode::Selected &&
            decoded.selection.protocol_version == 0) ||
        (decoded.code != RemoteStreamStatusCode::Selected &&
            decoded.selection.protocol_version != 0))
    {
        SetError(error, "remote stream status violates the protocol contract");
        return false;
    }
    status = decoded;
    return true;
}

bool ValidateRemoteProtocolNegotiationSelfTest(std::string* error)
{
    ClientControlPacket source;
    source.control_frame_id = 11;
    source.frame_id = 12;
    source.timestamp_usec = 13;
    source.viewport_width = 1280;
    source.viewport_height = 720;
    std::vector<uint8_t> bytes;
    ClientControlPacket decoded;
    if (!SerializeClientControlPacket(source, bytes, error) ||
        !DeserializeClientControlPacket(bytes.data(), bytes.size(), decoded, error) ||
        decoded.control_frame_id != source.control_frame_id ||
        decoded.preferred_quality_tier != RemoteQualityTierV3::High ||
        NegotiateRemoteStream(decoded).protocol_version != kRemoteVideoWireVersionV3)
    {
        SetError(error, "V3 control negotiation round trip failed");
        return false;
    }
    bytes.back() ^= 1u;
    if (DeserializeClientControlPacket(bytes.data(), bytes.size(), decoded, nullptr))
    {
        SetError(error, "corrupt control checksum was accepted");
        return false;
    }
    source.supported_protocol_versions = kRemoteProtocolCapabilityV2;
    if (NegotiateRemoteStream(source).protocol_version != kRemoteVideoWireVersion)
    {
        SetError(error, "V2-only control did not negotiate V2");
        return false;
    }
    source.supported_protocol_versions = kRemoteProtocolCapabilityV3;
    source.supported_encoding_profiles = 0;
    const RemoteStreamStatus mismatch = BuildRemoteStreamStatus(source);
    std::vector<uint8_t> status_bytes;
    RemoteStreamStatus decoded_status;
    if (mismatch.code != RemoteStreamStatusCode::NoCommonEncodingProfile ||
        mismatch.selection.protocol_version != 0 ||
        !SerializeRemoteStreamStatus(mismatch, status_bytes, error) ||
        !DeserializeRemoteStreamStatus(
            status_bytes.data(), status_bytes.size(), decoded_status, error) ||
        decoded_status.code != mismatch.code ||
        decoded_status.control_frame_id != mismatch.control_frame_id)
    {
        SetError(error, "V3 mismatch status round trip failed");
        return false;
    }
    status_bytes.back() ^= 1u;
    if (DeserializeRemoteStreamStatus(
        status_bytes.data(), status_bytes.size(), decoded_status, nullptr))
    {
        SetError(error, "corrupt stream status checksum was accepted");
        return false;
    }
    return true;
}

bool ValidateRemoteFrameContractV3(const RemoteFrameContractV3& contract, std::string* error)
{
    if (contract.protocol_version != kRemoteVideoWireVersionV3)
    {
        SetError(error, "unsupported V3 protocol version");
        return false;
    }
    if (contract.encoding_profile_id != kRemoteEncodingProfileI420V3 ||
        contract.source_control_frame_id == 0 ||
        contract.atlas_width == 0 || contract.atlas_height == 0 ||
        contract.atlas_width > kRemoteVideoV3MaxAtlasDimension ||
        contract.atlas_height > kRemoteVideoV3MaxAtlasDimension ||
        (contract.atlas_width % kRemoteVideoV3CodecAlignment) != 0 ||
        (contract.atlas_height % kRemoteVideoV3CodecAlignment) != 0)
    {
        SetError(error, "invalid V3 profile, source control frame, or atlas extent");
        return false;
    }
    if (static_cast<uint8_t>(contract.quality_tier) > static_cast<uint8_t>(RemoteQualityTierV3::Low))
    {
        SetError(error, "unknown V3 quality tier");
        return false;
    }

    std::array<bool, static_cast<size_t>(RemoteBufferSemantic::Count)> seen = {};
    const auto expected_atlas_dimension =
        [&contract](RemoteBufferSemantic semantic,
            uint16_t logical_dimension) {
            uint32_t divisor = 1;
            if (contract.quality_tier ==
                RemoteQualityTierV3::Balanced)
            {
                divisor = semantic ==
                        RemoteBufferSemantic::RemoteAO
                    ? 4u
                    : (semantic ==
                            RemoteBufferSemantic::RemoteShadowVisibility
                        ? 1u
                        : 2u);
            }
            else if (contract.quality_tier ==
                RemoteQualityTierV3::Low)
            {
                divisor = semantic ==
                        RemoteBufferSemantic::RemoteAO
                    ? 8u
                    : (semantic ==
                            RemoteBufferSemantic::RemoteShadowVisibility
                        ? 1u
                        : 4u);
            }
            const uint32_t scaled = std::max(
                2u,
                (static_cast<uint32_t>(logical_dimension) +
                    divisor - 1u) /
                    divisor);
            return static_cast<uint16_t>(
                (scaled +
                    kRemoteVideoV3CodecAlignment - 1u) /
                    kRemoteVideoV3CodecAlignment *
                kRemoteVideoV3CodecAlignment);
        };
    for (const RemoteBufferDescriptorV3& descriptor : contract.descriptors)
    {
        const size_t semantic_index = static_cast<size_t>(descriptor.semantic);
        if (semantic_index >= seen.size() || seen[semantic_index])
        {
            SetError(error, "unknown or duplicate V3 semantic");
            return false;
        }
        seen[semantic_index] = true;
        if (descriptor.representation != RemoteBufferRepresentationContractV3(descriptor.semantic) ||
            descriptor.encoding != RemoteBufferTransportEncoding(descriptor.semantic))
        {
            SetError(error, "V3 semantic representation/encoding mismatch");
            return false;
        }
        if ((descriptor.flags & ~kRemoteBufferDescriptorAvailableV3) != 0 || descriptor.reserved != 0)
        {
            SetError(error, "V3 descriptor has unknown flags or nonzero reserved fields");
            return false;
        }
        const bool shadow = descriptor.semantic == RemoteBufferSemantic::RemoteShadowVisibility;
        if ((shadow && IsAvailable(descriptor) &&
                (descriptor.stable_subject_id == 0 || descriptor.stable_subject_generation == 0)) ||
            (!shadow &&
                (descriptor.stable_subject_id != 0 || descriptor.stable_subject_generation != 0)))
        {
            SetError(error, "V3 stable subject identity violates semantic contract");
            return false;
        }
        if (!IsAvailable(descriptor))
        {
            if (descriptor.logical_width != 0 || descriptor.logical_height != 0 ||
                descriptor.atlas_x != 0 || descriptor.atlas_y != 0 ||
                descriptor.atlas_width != 0 || descriptor.atlas_height != 0 ||
                descriptor.content_frame_id != 0 || descriptor.content_generation != 0 ||
                descriptor.confidence_unorm != 0 || descriptor.stable_subject_id != 0 ||
                descriptor.stable_subject_generation != 0)
            {
                SetError(error, "unavailable V3 descriptor carries active image state");
                return false;
            }
            continue;
        }
        if (descriptor.logical_width == 0 || descriptor.logical_height == 0 ||
            descriptor.logical_width > kRemoteVideoV3MaxLogicalDimension ||
            descriptor.logical_height > kRemoteVideoV3MaxLogicalDimension ||
            descriptor.atlas_width == 0 || descriptor.atlas_height == 0 ||
            descriptor.content_frame_id == 0 || descriptor.content_generation == 0 ||
            (descriptor.atlas_x % kRemoteVideoV3CodecAlignment) != 0 ||
            (descriptor.atlas_y % kRemoteVideoV3CodecAlignment) != 0 ||
            (descriptor.atlas_width % kRemoteVideoV3CodecAlignment) != 0 ||
            (descriptor.atlas_height % kRemoteVideoV3CodecAlignment) != 0 ||
            static_cast<uint32_t>(descriptor.atlas_x) + descriptor.atlas_width > contract.atlas_width ||
            static_cast<uint32_t>(descriptor.atlas_y) + descriptor.atlas_height > contract.atlas_height)
        {
            SetError(error, "V3 descriptor has invalid dimensions, alignment, generation, or atlas bounds");
            return false;
        }
        if (descriptor.atlas_width !=
                expected_atlas_dimension(
                    descriptor.semantic,
                    descriptor.logical_width) ||
            descriptor.atlas_height !=
                expected_atlas_dimension(
                    descriptor.semantic,
                    descriptor.logical_height))
        {
            SetError(
                error,
                "V3 descriptor resolution does not match negotiated quality tier");
            return false;
        }
    }
    for (size_t i = 0; i < contract.descriptors.size(); ++i)
    {
        if (!IsAvailable(contract.descriptors[i])) continue;
        for (size_t j = i + 1; j < contract.descriptors.size(); ++j)
        {
            if (IsAvailable(contract.descriptors[j]) && RectsOverlap(contract.descriptors[i], contract.descriptors[j]))
            {
                SetError(error, "V3 atlas rectangles overlap");
                return false;
            }
        }
    }
    return true;
}

bool SerializeRemoteFrameContractV3(
    const RemoteFrameContractV3& contract, std::vector<uint8_t>& bytes, std::string* error)
{
    if (!ValidateRemoteFrameContractV3(contract, error)) return false;
    bytes.clear();
    bytes.reserve(kContractBytes);
    WriteU32(bytes, kRemoteVideoWireMagicV3);
    WriteU32(bytes, contract.protocol_version);
    WriteU32(bytes, static_cast<uint32_t>(kContractBytes));
    WriteU32(bytes, 0); // checksum, filled after all explicit little-endian fields are written
    WriteU32(bytes, contract.encoding_profile_id);
    WriteU8(bytes, static_cast<uint8_t>(contract.quality_tier));
    WriteU8(bytes, static_cast<uint8_t>(contract.descriptors.size()));
    WriteU16(bytes, contract.atlas_width);
    WriteU16(bytes, contract.atlas_height);
    WriteU16(bytes, 0);
    WriteU64(bytes, contract.source_control_frame_id);
    for (const RemoteBufferDescriptorV3& descriptor : contract.descriptors) WriteDescriptor(bytes, descriptor);
    if (bytes.size() != kContractBytes)
    {
        SetError(error, "internal V3 serialized-size mismatch");
        bytes.clear();
        return false;
    }
    const uint32_t checksum = FNV1a32(bytes.data() + 16, bytes.size() - 16);
    bytes[12] = static_cast<uint8_t>(checksum);
    bytes[13] = static_cast<uint8_t>(checksum >> 8u);
    bytes[14] = static_cast<uint8_t>(checksum >> 16u);
    bytes[15] = static_cast<uint8_t>(checksum >> 24u);
    return true;
}

bool DeserializeRemoteFrameContractV3(
    const uint8_t* bytes, size_t byte_count, RemoteFrameContractV3& contract, std::string* error)
{
    if (bytes == nullptr || byte_count != kContractBytes)
    {
        SetError(error, "invalid V3 contract byte size");
        return false;
    }
    const uint8_t* cursor = bytes;
    const uint8_t* end = bytes + byte_count;
    uint32_t magic = 0, encoded_size = 0, checksum = 0;
    uint8_t quality = 0, descriptor_count = 0;
    uint16_t reserved = 0;
    if (!ReadU32(cursor, end, magic) || !ReadU32(cursor, end, contract.protocol_version) ||
        !ReadU32(cursor, end, encoded_size) || !ReadU32(cursor, end, checksum) ||
        !ReadU32(cursor, end, contract.encoding_profile_id) || !ReadU8(cursor, end, quality) ||
        !ReadU8(cursor, end, descriptor_count) || !ReadU16(cursor, end, contract.atlas_width) ||
        !ReadU16(cursor, end, contract.atlas_height) || !ReadU16(cursor, end, reserved) ||
        !ReadU64(cursor, end, contract.source_control_frame_id))
    {
        SetError(error, "truncated V3 contract header");
        return false;
    }
    if (magic != kRemoteVideoWireMagicV3 || encoded_size != byte_count || reserved != 0 ||
        descriptor_count != contract.descriptors.size() ||
        checksum != FNV1a32(bytes + 16, byte_count - 16))
    {
        SetError(error, "V3 magic, size, count, reserved field, or checksum mismatch");
        return false;
    }
    contract.quality_tier = static_cast<RemoteQualityTierV3>(quality);
    for (RemoteBufferDescriptorV3& descriptor : contract.descriptors)
    {
        if (!ReadDescriptor(cursor, end, descriptor))
        {
            SetError(error, "truncated V3 descriptor");
            return false;
        }
    }
    if (cursor != end)
    {
        SetError(error, "V3 contract has trailing bytes");
        return false;
    }
    return ValidateRemoteFrameContractV3(contract, error);
}

bool ValidateRemoteFrameContractV3SelfTest(std::string* error)
{
    RemoteFrameContractV3 source;
    source.atlas_width = 16;
    source.atlas_height = 16;
    source.source_control_frame_id = 41;
    for (size_t i = 0; i < source.descriptors.size(); ++i)
    {
        RemoteBufferDescriptorV3& descriptor = source.descriptors[i];
        descriptor.semantic = static_cast<RemoteBufferSemantic>(i);
        descriptor.representation = RemoteBufferRepresentationContractV3(descriptor.semantic);
        descriptor.encoding = RemoteBufferTransportEncoding(descriptor.semantic);
        descriptor.flags = kRemoteBufferDescriptorAvailableV3;
        descriptor.logical_width = 8;
        descriptor.logical_height = 8;
        descriptor.atlas_x = static_cast<uint16_t>((i & 1u) * 8u);
        descriptor.atlas_y = static_cast<uint16_t>((i >> 1u) * 8u);
        descriptor.atlas_width = 8;
        descriptor.atlas_height = 8;
        descriptor.content_frame_id = 39 + i;
        descriptor.content_generation = 2;
        descriptor.confidence_unorm = 65535;
        descriptor.stable_subject_id = descriptor.semantic == RemoteBufferSemantic::RemoteShadowVisibility
            ? 0x4e505f53554eULL : 0;
        descriptor.stable_subject_generation =
            descriptor.semantic == RemoteBufferSemantic::RemoteShadowVisibility ? 7u : 0u;
    }
    std::vector<uint8_t> bytes;
    if (!SerializeRemoteFrameContractV3(source, bytes, error)) return false;
    RemoteFrameContractV3 decoded;
    if (!DeserializeRemoteFrameContractV3(bytes.data(), bytes.size(), decoded, error) ||
        decoded.source_control_frame_id != source.source_control_frame_id ||
        decoded.descriptors[2].representation != RemoteBufferRepresentationV3::SpecularIndirectPreAO ||
        decoded.descriptors[3].stable_subject_generation !=
            source.descriptors[3].stable_subject_generation)
    {
        SetError(error, "V3 descriptor explicit serialization round-trip mismatch");
        return false;
    }

    std::vector<uint8_t> corrupt = bytes;
    corrupt.back() ^= 1u;
    if (DeserializeRemoteFrameContractV3(corrupt.data(), corrupt.size(), decoded, nullptr))
    {
        SetError(error, "V3 checksum corruption was accepted");
        return false;
    }
    RemoteFrameContractV3 invalid = source;
    invalid.protocol_version = kRemoteVideoWireVersionV3 + 1;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "unknown V3 protocol version was accepted");
        return false;
    }
    invalid = source;
    invalid.encoding_profile_id = kRemoteEncodingProfileI420V3 + 1;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "unknown V3 encoding profile was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[1].atlas_x = invalid.descriptors[0].atlas_x;
    invalid.descriptors[1].atlas_y = invalid.descriptors[0].atlas_y;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 overlapping atlas rectangles were accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[0].stable_subject_id = 1;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 identity on a non-shadow semantic was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[3].stable_subject_id = 0;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 available shadow without stable identity was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[3].stable_subject_generation = 0;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 available shadow without subject generation was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[2].atlas_x = 14;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 out-of-bounds atlas rectangle was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[2].encoding = RemoteBufferEncoding::ScalarLuma8;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 incompatible semantic encoding was accepted");
        return false;
    }
    invalid = source;
    invalid.atlas_width = static_cast<uint16_t>(kRemoteVideoV3MaxAtlasDimension + 2u);
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 oversized atlas was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[0].logical_width =
        static_cast<uint16_t>(kRemoteVideoV3MaxLogicalDimension + 1u);
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 oversized logical buffer was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[0].atlas_x = 1;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 codec-misaligned rectangle was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[0].atlas_width = 6;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(
            error,
            "V3 quality-tier resolution mismatch was accepted");
        return false;
    }
    invalid = source;
    invalid.descriptors[0].flags = 0;
    invalid.descriptors[0].logical_width = 0;
    invalid.descriptors[0].logical_height = 0;
    invalid.descriptors[0].atlas_width = 0;
    invalid.descriptors[0].atlas_height = 0;
    invalid.descriptors[0].content_frame_id = 0;
    invalid.descriptors[0].content_generation = 0;
    invalid.descriptors[0].confidence_unorm = 0;
    invalid.descriptors[0].atlas_x = 2;
    if (ValidateRemoteFrameContractV3(invalid, nullptr))
    {
        SetError(error, "V3 unavailable descriptor with stale image state was accepted");
        return false;
    }
    return true;
}
} // namespace wicked_newpipeline
