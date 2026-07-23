#include "NewPipelineProtocol.h"

#include <algorithm>
#include <array>
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
