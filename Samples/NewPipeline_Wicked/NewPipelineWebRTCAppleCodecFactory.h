#pragma once

#if defined(__APPLE__)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

struct NPHardwareEncoderFailureSignal
{
    std::atomic_bool failed{false};
};

struct NPHardwareDecoderFailureSignal
{
    std::atomic_bool failed{false};
    std::atomic<uint32_t> failure_stage{0};
    std::atomic<int32_t> failure_status{0};
};

struct NPAppleVideoCodecFactories
{
    std::unique_ptr<webrtc::VideoEncoderFactory> encoder;
    std::unique_ptr<webrtc::VideoDecoderFactory> decoder;
    std::shared_ptr<NPHardwareEncoderFailureSignal> hardware_failure;
    std::shared_ptr<NPHardwareDecoderFailureSignal> hardware_decoder_failure;
    bool hardware_encoder_available = false;
};

NPAppleVideoCodecFactories np_create_apple_video_codec_factories(
    bool request_hardware_encoder);

extern "C" int np_validate_apple_video_codec_factories(
    char* error, size_t error_capacity);

#endif
