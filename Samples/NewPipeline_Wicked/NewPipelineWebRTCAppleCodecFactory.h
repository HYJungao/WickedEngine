#pragma once

#if defined(__APPLE__)

#include <atomic>
#include <cstddef>
#include <memory>

#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

struct NPHardwareEncoderFailureSignal
{
    std::atomic_bool failed{false};
};

struct NPAppleVideoCodecFactories
{
    std::unique_ptr<webrtc::VideoEncoderFactory> encoder;
    std::unique_ptr<webrtc::VideoDecoderFactory> decoder;
    std::shared_ptr<NPHardwareEncoderFailureSignal> hardware_failure;
    bool hardware_encoder_available = false;
};

NPAppleVideoCodecFactories np_create_apple_video_codec_factories(
    bool request_hardware_encoder);

extern "C" int np_validate_apple_video_codec_factories(
    char* error, size_t error_capacity);

#endif
