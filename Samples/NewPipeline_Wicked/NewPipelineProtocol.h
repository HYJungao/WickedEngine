#pragma once

#include "NewPipelineRuntime.h"

#include <cstdint>

namespace wicked_newpipeline
{
constexpr const char* kControlStreamName             = "np.control";
constexpr const char* kRemoteVideoStreamName         = "np.remote.video";
constexpr const char* kRemoteFrameStreamId            = "RemoteFrame.CloudBuffers.V1";
constexpr uint32_t    kRemoteVideoWireMagic           = 0x3156504Eu; // NPV1
constexpr uint32_t    kRemoteVideoWireVersion         = 1u;

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
};
} // namespace wicked_newpipeline
