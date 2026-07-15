#pragma once

#include "WickedEngine.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wicked_newpipeline
{
enum class RemoteSourceMode : uint8_t
{
    Mock,
    WebRTC
};

enum class RemoteBufferKind : uint32_t
{
    None               = 0u,
    IndirectDiffuse    = 1u << 0u,
    AmbientOcclusion   = 1u << 1u,
    SpecularIndirect   = 1u << 2u,
    ShadowVisibility   = 1u << 3u,
    All                = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u),

    // Compatibility aliases retained for existing command-line/config code.
    GI                 = IndirectDiffuse,
    Reflection         = SpecularIndirect,
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

enum class DDGIResetReason : uint8_t
{
    None,
    InitialScene,
    SceneGeneration,
    LightingChanged,
    GridChanged,
};

enum class RemoteDebugMode : uint8_t
{
    Local,
    Raw,
    DebugComposite
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
    LocalShadowVisibility,
    RemoteIndirectDiffuse,
    RemoteAO,
    RemoteSpecularIndirect,
    RemoteShadowVisibility,
    RemoteOverview,
    TransportIndirectDiffuse,
    TransportAO,
    TransportSpecularIndirect,
    TransportShadowVisibility,
    LocalReflectionProbe,
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

struct RemoteStreamConfig
{
    std::string        stream_id       = "RemoteBuffer.RemoteIndirectDiffuse.V1";
    RemoteSourceMode   source_mode     = RemoteSourceMode::Mock;
    uint32_t           produced_kinds  = static_cast<uint32_t>(RemoteBufferKind::All);
    uint32_t           target_width    = 0;
    uint32_t           target_height   = 0;
    float              target_fps      = 30.0f;
    RemoteDynamicRange dynamic_range   = RemoteDynamicRange::LDR;
};

struct RuntimeConfig
{
    RemoteSourceMode        remote_source = RemoteSourceMode::WebRTC;
    RemoteDebugMode         remote_debug_mode = RemoteDebugMode::Local;
    std::string             signaling_url = "ws://127.0.0.1:39876";
    std::string             room_id = "NewPipeline.Wicked.V1";
    bool                    use_internet_ice = false;
    std::vector<std::string> parse_warnings;
};

constexpr uint32_t RemoteBufferKindMask(RemoteBufferSemantic semantic)
{
    return 1u << static_cast<uint32_t>(semantic);
}

const char* ToString(RemoteBufferSemantic semantic);
const char* ToString(RemoteSourceMode mode);
const char* ToString(RemoteDynamicRange range);
const char* ToString(RemoteDebugMode mode);
const char* ToString(DebugPreviewMode mode);
const char* ToString(RemoteBufferEncoding encoding);
const char* ToString(DDGIResetReason reason);

RemoteSourceMode ParseRemoteSourceMode(const std::string& value, RemoteSourceMode fallback);
RemoteDebugMode ParseRemoteDebugMode(const std::string& value, RemoteDebugMode fallback);
RuntimeConfig ParseRuntimeConfig(int argc, char* argv[], RuntimeConfig fallback = {});
} // namespace wicked_newpipeline
