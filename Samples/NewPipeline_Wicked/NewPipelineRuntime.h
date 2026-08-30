#pragma once

#include "WickedEngine.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wicked_newpipeline
{
enum class RemoteBufferKind : uint32_t
{
    None               = 0u,
    IndirectDiffuse    = 1u << 0u,
    AmbientOcclusion   = 1u << 1u,
    SpecularIndirect   = 1u << 2u,
    ShadowVisibility   = 1u << 3u,
    All                = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u),
};

enum class RemoteBufferSemantic : uint8_t
{
    RemoteIndirectDiffuse = 0,
    RemoteAO = 1,
    RemoteSpecularIndirect = 2,
    RemoteShadowVisibility = 3,
    Count = 4,
};

enum class RemoteDynamicRange : uint8_t
{
    Unknown,
    LDR,
    HDR
};

enum class RemoteBufferEncoding : uint8_t
{
    LinearRGBA8,
    LogHDR16F,
    ScalarLuma8,
};

// V3 names the physical representation independently from the UI buffer label.
// These values are protocol constants; do not reorder or reuse them.
enum class RemoteBufferRepresentationV3 : uint8_t
{
    DiffuseIrradiance = 0,
    AmbientVisibility = 1,
    SpecularIndirectPreAO = 2,
    PrimaryLightVisibility = 3,
};

enum class RemoteQualityTierV3 : uint8_t
{
    High = 0,
    Balanced = 1,
    Low = 2,
};

enum class RemoteEncoderPreference : uint8_t
{
    Hardware,
    Software,
};

struct FormalLightingBlendV3
{
    XMFLOAT3 diffuse_local = {};
    XMFLOAT3 diffuse_remote = {};
    XMFLOAT3 specular_pre_ao_local = {};
    XMFLOAT3 specular_pre_ao_remote = {};
    float ambient_visibility_local = 1.0f;
    float ambient_visibility_remote = 1.0f;
    float primary_visibility_local = 1.0f;
    float primary_visibility_remote = 1.0f;
    float diffuse_weight = 0.0f;
    float ao_weight = 0.0f;
    float specular_weight = 0.0f;
    float primary_visibility_weight = 0.0f;
};

struct FormalLightingBlendV3Result
{
    XMFLOAT3 diffuse = {};
    XMFLOAT3 specular_pre_ao = {};
    float ambient_visibility = 1.0f;
    float primary_visibility = 1.0f;
};

enum class DDGIResetReason : uint8_t
{
    None,
    InitialScene,
    SceneGeneration,
    LightingChanged,
    GridChanged,
};

enum class DebugPreviewMode : uint8_t
{
    Final,
    GBufferDepth,
    GBufferNormalRoughness,
    GBufferNormalXY,
    GBufferRoughness,
    LocalIndirectDiffuse,
    LocalAO,
    LocalSpecularIndirect,
    LocalSpecularIndirectPreAO,
    LocalShadowVisibility,
    RemoteIndirectDiffuse,
    RemoteAO,
    RemoteSpecularIndirect,
    RemoteShadowVisibility,
    ElasticIndirectDiffuse,
    ElasticAO,
    ElasticSpecularIndirectPreAO,
    ElasticPrimaryLightVisibility,
    TransportIndirectDiffuse,
    TransportAO,
    TransportSpecularIndirect,
    TransportShadowVisibility,
    LocalReflectionProbe,
    LocalIndirectFinalInput,
};

struct RemoteFrameMetadata
{
    uint64_t           frame_id          = 0;
    uint64_t           timestamp_usec    = 0;
    // Assigned by the receiver after the complete video frame has been decoded.
    // This is intentionally not serialized into the video metadata band.
    uint64_t           local_receive_timestamp_usec = 0;
    uint32_t           width             = 0;
    uint32_t           height            = 0;
    uint32_t           source_generation = 0;
    uint32_t           continuity_mask   = 0;
    uint32_t           available_buffer_mask = 0;
    RemoteDynamicRange dynamic_range     = RemoteDynamicRange::Unknown;
    std::string        source_stream_id;
    XMFLOAT3           view_origin       = XMFLOAT3(0, 0, 0);
    XMFLOAT3           view_forward      = XMFLOAT3(0, 0, 1);
    XMFLOAT2           temporal_jitter_pixels = XMFLOAT2(0, 0);
    XMFLOAT4X4         view              = wi::math::IDENTITY_MATRIX;
    XMFLOAT4X4         projection        = wi::math::IDENTITY_MATRIX;
    XMFLOAT4X4         view_projection   = wi::math::IDENTITY_MATRIX;
    XMFLOAT4X4         inverse_view      = wi::math::IDENTITY_MATRIX;
    XMFLOAT4X4         inverse_projection = wi::math::IDENTITY_MATRIX;
    XMFLOAT4X4         inverse_view_projection = wi::math::IDENTITY_MATRIX;
    float              near_plane        = 0.1f;
    float              far_plane         = 1000.0f;
    float              pre_exposure      = 1.0f;
    bool               history_valid     = false;
    bool               reset_this_frame  = false;
    bool               camera_cut        = false;
    float              confidence        = 1.0f;
    bool               valid             = false;
    uint32_t           ddgi_frame_index  = 0;
    DDGIResetReason    ddgi_reset_reason = DDGIResetReason::None;
};

struct RuntimeConfig
{
    std::string             signaling_url = "ws://127.0.0.1:39876";
    std::string             room_id = "NewPipeline.Wicked.V1";
    RemoteQualityTierV3     remote_quality_tier = RemoteQualityTierV3::High;
    RemoteEncoderPreference remote_encoder = RemoteEncoderPreference::Hardware;
    bool                    use_internet_ice = false;
    std::vector<std::string> parse_warnings;
};

// WebRTC video RTP uses a 90 kHz clock. Keep the transport identity in that
// clock domain instead of reusing the application frame counter.
constexpr uint32_t RemoteVideoRtpTimestamp(uint64_t timestamp_usec)
{
    const uint64_t seconds = timestamp_usec / 1'000'000ull;
    const uint64_t remainder = timestamp_usec % 1'000'000ull;
    return static_cast<uint32_t>(
        seconds * 90'000ull + remainder * 9ull / 100ull);
}

constexpr uint32_t RemoteBufferKindMask(RemoteBufferSemantic semantic)
{
    return 1u << static_cast<uint32_t>(semantic);
}

// The wire encoding is part of each semantic's protocol contract.  Keep this
// centralized so producers, metadata validation, GPU unpack and debug views do
// not infer the representation from an array position.
constexpr RemoteBufferEncoding RemoteBufferTransportEncoding(RemoteBufferSemantic semantic)
{
    return semantic == RemoteBufferSemantic::RemoteIndirectDiffuse ||
        semantic == RemoteBufferSemantic::RemoteSpecularIndirect
        ? RemoteBufferEncoding::LogHDR16F
        : RemoteBufferEncoding::ScalarLuma8;
}

constexpr RemoteBufferRepresentationV3 RemoteBufferRepresentationContractV3(RemoteBufferSemantic semantic)
{
    switch (semantic)
    {
    case RemoteBufferSemantic::RemoteIndirectDiffuse:
        return RemoteBufferRepresentationV3::DiffuseIrradiance;
    case RemoteBufferSemantic::RemoteAO:
        return RemoteBufferRepresentationV3::AmbientVisibility;
    case RemoteBufferSemantic::RemoteSpecularIndirect:
        return RemoteBufferRepresentationV3::SpecularIndirectPreAO;
    case RemoteBufferSemantic::RemoteShadowVisibility:
    default:
        return RemoteBufferRepresentationV3::PrimaryLightVisibility;
    }
}

FormalLightingBlendV3Result EvaluateFormalLightingBlendV3(const FormalLightingBlendV3& input);
bool ValidateFormalLightingBlendV3Reference(std::string* error = nullptr);

const char* ToString(RemoteBufferSemantic semantic);
const char* ToString(RemoteDynamicRange range);
const char* ToString(DebugPreviewMode mode);
const char* ToString(RemoteBufferEncoding encoding);
const char* ToString(RemoteQualityTierV3 tier);
const char* ToString(RemoteEncoderPreference preference);
const char* ToString(DDGIResetReason reason);

RuntimeConfig ParseRuntimeConfig(int argc, char* argv[], RuntimeConfig fallback = {});
} // namespace wicked_newpipeline
