#pragma once

#if defined(_WIN32)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

struct NPWindowsNativeNV12Surface
{
    uint32_t width = 0;
    uint32_t height = 0;
    void* texture_shared_handle = nullptr;
    void* fence_shared_handle = nullptr;
    uint64_t producer_fence_value = 0;
    uint64_t consumer_fence_value = 0;
    uint64_t adapter_luid = 0;
    // Original producer timestamp. WebRTC may rewrite the VideoFrame capture
    // time before Encode(), so native frames retain their stable identity in
    // the buffer itself.
    int64_t source_timestamp_usec = 0;
};

webrtc::scoped_refptr<webrtc::VideoFrameBuffer>
np_create_windows_native_nv12_buffer(
    const NPWindowsNativeNV12Surface& surface,
    std::function<void()> release,
    std::function<void()> completion_scheduled);
bool np_get_windows_native_nv12_surface(
    webrtc::VideoFrameBuffer* buffer,
    NPWindowsNativeNV12Surface& surface);
void np_mark_windows_native_nv12_completion_scheduled(
    webrtc::VideoFrameBuffer* buffer);

struct NPWindowsHardwareDecoderFailureSignal
{
    std::atomic_bool failed{false};
    std::atomic_bool native_surface_failed{false};
    std::atomic_uint64_t decode_calls{0};
    std::atomic_uint64_t keyframes_received{0};
    std::atomic_uint64_t dimension_requests{0};
    std::atomic_uint64_t queued_frames{0};
    std::atomic_uint64_t submitted_frames{0};
    std::atomic_uint64_t decoded_frames{0};
    std::atomic_uint64_t native_surface_attempts{0};
    std::atomic_uint64_t native_surface_outputs{0};
    std::atomic_uint64_t native_allocation_failures{0};
    std::atomic_uint64_t native_signal_failures{0};
    std::atomic_uint64_t native_ring_drops{0};
    std::atomic_uint32_t native_failure_stage{0};
    std::atomic_uint32_t native_failure_hresult{0};
};

struct NPWindowsHardwareEncoderFailureSignal
{
    std::atomic_bool failed{false};
    // Native-surface interop can fall back to I420 while the same hardware
    // H264 MFT remains active. This is intentionally separate from failed.
    std::atomic_bool native_surface_failed{false};
    std::atomic_uint64_t encode_calls{0};
    std::atomic_uint64_t native_frames{0};
    std::atomic_uint64_t submitted_frames{0};
    std::atomic_uint64_t output_frames{0};
    std::atomic_uint32_t failure_stage{0};
    std::atomic_uint32_t failure_hresult{0};
};

struct NPWindowsVideoCodecFactories
{
    std::unique_ptr<webrtc::VideoEncoderFactory> encoder;
    std::unique_ptr<webrtc::VideoDecoderFactory> decoder;
    std::shared_ptr<NPWindowsHardwareEncoderFailureSignal> hardware_encoder_failure;
    std::shared_ptr<NPWindowsHardwareDecoderFailureSignal> hardware_decoder_failure;
    bool hardware_encoder_available = false;
    bool hardware_decoder_available = false;
};

NPWindowsVideoCodecFactories np_create_windows_video_codec_factories(
    bool request_hardware_encoder,
    uint64_t adapter_luid);

extern "C" uint64_t np_get_windows_default_video_adapter_luid();

extern "C" int np_validate_windows_video_codec_factories(
    char* error, size_t error_capacity);

#endif
