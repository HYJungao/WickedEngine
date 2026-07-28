#pragma once

#include "NewPipelineRuntime.h"

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace wicked_newpipeline
{
constexpr const char* kControlStreamName             = "np.control";
constexpr const char* kRemoteVideoStreamName         = "np.remote.video";
constexpr const char* kRemoteFrameStreamId            = "RemoteFrame.CloudBuffers.V2";
constexpr uint32_t    kRemoteVideoWireMagic           = 0x3156504Eu; // NPV1
constexpr uint32_t    kRemoteVideoWireVersion         = 2u;
constexpr uint32_t    kRemoteVideoWireVersionV3       = 3u;
constexpr uint32_t    kRemoteVideoWireMagicV3         = 0x3356504Eu; // NPV3
constexpr uint8_t     kRemoteBufferDescriptorAvailableV3 = 1u << 0u;
constexpr uint32_t    kRemoteEncodingProfileI420V3    = 1u;
constexpr uint16_t    kRemoteVideoV3CodecAlignment    = 2u;
constexpr uint16_t    kRemoteVideoV3MaxAtlasDimension = 8192u;
constexpr uint16_t    kRemoteVideoV3MaxLogicalDimension = 4096u;
constexpr uint32_t    kControlWireMagicV2               = 0x3243504Eu; // NPC2
constexpr uint32_t    kControlWireVersionV2             = 2u;
constexpr uint32_t    kStreamStatusWireMagicV3          = 0x3353504Eu; // NPS3
constexpr uint32_t    kStreamStatusWireVersionV3        = 1u;
constexpr uint32_t    kRemoteProtocolCapabilityV2       = 1u << kRemoteVideoWireVersion;
constexpr uint32_t    kRemoteProtocolCapabilityV3       = 1u << kRemoteVideoWireVersionV3;
constexpr uint32_t    kRemoteQualityCapabilityHigh      = 1u << static_cast<uint32_t>(RemoteQualityTierV3::High);
constexpr uint32_t    kRemoteQualityCapabilityBalanced  = 1u << static_cast<uint32_t>(RemoteQualityTierV3::Balanced);
constexpr uint32_t    kRemoteQualityCapabilityLow       = 1u << static_cast<uint32_t>(RemoteQualityTierV3::Low);

struct RemoteBufferDescriptorV3
{
    RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteIndirectDiffuse;
    RemoteBufferRepresentationV3 representation = RemoteBufferRepresentationV3::DiffuseIrradiance;
    RemoteBufferEncoding encoding = RemoteBufferEncoding::LogHDR16F;
    uint8_t flags = 0;
    uint16_t logical_width = 0;
    uint16_t logical_height = 0;
    uint16_t atlas_x = 0;
    uint16_t atlas_y = 0;
    uint16_t atlas_width = 0;
    uint16_t atlas_height = 0;
    uint64_t content_frame_id = 0;
    uint32_t content_generation = 0;
    uint16_t confidence_unorm = 0;
    uint16_t reserved = 0;
    uint64_t stable_subject_id = 0;
    uint32_t stable_subject_generation = 0;
};

struct RemoteFrameContractV3
{
    uint32_t protocol_version = kRemoteVideoWireVersionV3;
    uint32_t encoding_profile_id = kRemoteEncodingProfileI420V3;
    RemoteQualityTierV3 quality_tier = RemoteQualityTierV3::High;
    uint16_t atlas_width = 0;
    uint16_t atlas_height = 0;
    uint64_t source_control_frame_id = 0;
    std::array<RemoteBufferDescriptorV3,
        static_cast<size_t>(RemoteBufferSemantic::Count)> descriptors = {};
};

bool ValidateRemoteFrameContractV3(const RemoteFrameContractV3& contract, std::string* error = nullptr);
bool SerializeRemoteFrameContractV3(
    const RemoteFrameContractV3& contract, std::vector<uint8_t>& bytes, std::string* error = nullptr);
bool DeserializeRemoteFrameContractV3(
    const uint8_t* bytes, size_t byte_count, RemoteFrameContractV3& contract, std::string* error = nullptr);
bool ValidateRemoteFrameContractV3SelfTest(std::string* error = nullptr);

struct NewPipelineSunState
{
    bool     enabled       = true;
    float    yaw_degrees   = -35.0f;
    float    pitch_degrees = 50.0f;
    XMFLOAT3 direction     = XMFLOAT3(0.0f, 1.0f, 0.0f);
    XMFLOAT3 color         = XMFLOAT3(1.0f, 0.95f, 0.85f);
    float    intensity     = 3.0f;
};

struct ClientControlPacket
{
    // control_frame_id is the identity consumed by V3 reprojection/history.
    // It is intentionally separate from transport sequence numbers.
    uint64_t control_frame_id = 0;
    uint64_t frame_id         = 0;
    uint64_t timestamp_usec   = 0;
    uint32_t viewport_width   = 0;
    uint32_t viewport_height  = 0;
    uint32_t scene_generation = 0;
    float    near_plane       = 0.1f;
    float    far_plane        = 1000.0f;
    XMFLOAT3 eye              = XMFLOAT3(0, 2, -8);
    XMFLOAT3 at               = XMFLOAT3(0, 0, 1);
    XMFLOAT3 up               = XMFLOAT3(0, 1, 0);
    XMFLOAT4X4 view           = wi::math::IDENTITY_MATRIX;
    XMFLOAT4X4 projection     = wi::math::IDENTITY_MATRIX;
    bool     sun_enabled      = true;
    XMFLOAT3 sun_direction    = XMFLOAT3(-0.3f, -0.8f, 0.2f);
    XMFLOAT3 sun_color        = XMFLOAT3(1, 0.95f, 0.85f);
    float    sun_intensity    = 3.0f;
    XMFLOAT3 ambient          = XMFLOAT3(0.2f, 0.2f, 0.2f);
    XMFLOAT3 horizon          = XMFLOAT3(0.38f, 0.38f, 0.38f);
    XMFLOAT3 zenith           = XMFLOAT3(0.42f, 0.42f, 0.42f);
    uint32_t supported_protocol_versions =
        kRemoteProtocolCapabilityV2 | kRemoteProtocolCapabilityV3;
    uint32_t supported_quality_tiers =
        kRemoteQualityCapabilityHigh | kRemoteQualityCapabilityBalanced | kRemoteQualityCapabilityLow;
    uint32_t supported_encoding_profiles = 1u << kRemoteEncodingProfileI420V3;
    uint32_t preferred_protocol_version = kRemoteVideoWireVersionV3;
    // High remains the production default. Balanced and Low are negotiated
    // opt-in tiers; the implementation never changes quality tier implicitly.
    RemoteQualityTierV3 preferred_quality_tier = RemoteQualityTierV3::High;
};

struct RemoteStreamSelection
{
    uint32_t protocol_version = kRemoteVideoWireVersion;
    uint32_t encoding_profile_id = 0;
    RemoteQualityTierV3 quality_tier = RemoteQualityTierV3::High;

    bool operator==(const RemoteStreamSelection& other) const
    {
        return protocol_version == other.protocol_version &&
            encoding_profile_id == other.encoding_profile_id &&
            quality_tier == other.quality_tier;
    }
    bool operator!=(const RemoteStreamSelection& other) const { return !(*this == other); }
};

enum class RemoteStreamStatusCode : uint8_t
{
    Selected = 0,
    NoCommonProtocol = 1,
    NoCommonEncodingProfile = 2,
    NoCommonQualityTier = 3,
};

struct RemoteStreamStatus
{
    uint64_t control_frame_id = 0;
    RemoteStreamStatusCode code = RemoteStreamStatusCode::NoCommonProtocol;
    RemoteStreamSelection selection = {};
};

bool SerializeClientControlPacket(
    const ClientControlPacket& packet, std::vector<uint8_t>& bytes, std::string* error = nullptr);
bool DeserializeClientControlPacket(
    const uint8_t* bytes, size_t byte_count, ClientControlPacket& packet, std::string* error = nullptr);
RemoteStreamSelection NegotiateRemoteStream(const ClientControlPacket& packet);
RemoteStreamStatus BuildRemoteStreamStatus(const ClientControlPacket& packet);
bool SerializeRemoteStreamStatus(
    const RemoteStreamStatus& status, std::vector<uint8_t>& bytes, std::string* error = nullptr);
bool DeserializeRemoteStreamStatus(
    const uint8_t* bytes, size_t byte_count, RemoteStreamStatus& status, std::string* error = nullptr);
bool ValidateRemoteProtocolNegotiationSelfTest(std::string* error = nullptr);
} // namespace wicked_newpipeline
