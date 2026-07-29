#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NPWebRTCBridge NPWebRTCBridge;
typedef struct NPWebRTCVideoFrame NPWebRTCVideoFrame;
typedef void (*NPWebRTCReleaseCallback)(void* context);

typedef enum NPWebRTCBridgeState
{
    NP_WEBRTC_DISABLED = 0,
    NP_WEBRTC_STARTING = 1,
    NP_WEBRTC_SIGNALING = 2,
    NP_WEBRTC_CONNECTED = 3,
    NP_WEBRTC_FAILED = 4,
} NPWebRTCBridgeState;

typedef struct NPWebRTCBridgeStats
{
    uint32_t state;
    uint64_t sent_frames;
    uint64_t received_frames;
    uint64_t dropped_frames;
    uint32_t decoded_queue_depth;
    uint64_t sent_controls;
    uint64_t received_controls;
    uint64_t compressed_bytes_sent;
    uint64_t compressed_bytes_received;
    uint64_t total_encode_time_usec;
    uint64_t total_decode_time_usec;
    uint64_t frames_encoded;
    uint64_t frames_decoded;
    uint32_t power_efficient_codec;
    char codec_name[64];
    char codec_implementation[64];
    char status[256];
} NPWebRTCBridgeStats;

NPWebRTCBridge* np_webrtc_bridge_create(
    int is_server,
    const char* signaling_url,
    const char* room_id,
    int use_internet_ice);
void np_webrtc_bridge_destroy(NPWebRTCBridge* bridge);

int np_webrtc_bridge_send_i420(
    NPWebRTCBridge* bridge,
    uint32_t width,
    uint32_t height,
    const uint8_t* data,
    size_t data_size,
    int64_t timestamp_usec);
int np_webrtc_bridge_send_i420_planes(
    NPWebRTCBridge* bridge,
    uint32_t width,
    uint32_t height,
    const uint8_t* y_plane,
    uint32_t y_stride,
    const uint8_t* u_plane,
    uint32_t u_stride,
    const uint8_t* v_plane,
    uint32_t v_stride,
    int64_t timestamp_usec,
    NPWebRTCReleaseCallback release_callback,
    void* release_context);
int np_webrtc_bridge_receive_i420(
    NPWebRTCBridge* bridge,
    uint32_t* width,
    uint32_t* height,
    uint8_t* destination,
    size_t destination_capacity,
    size_t* required_size);

int np_webrtc_bridge_acquire_i420_frame(
    NPWebRTCBridge* bridge,
    NPWebRTCVideoFrame** frame);
int np_webrtc_video_frame_get_i420(
    NPWebRTCVideoFrame* frame,
    uint32_t* width,
    uint32_t* height,
    const uint8_t** y_plane,
    uint32_t* y_stride,
    const uint8_t** u_plane,
    uint32_t* u_stride,
    const uint8_t** v_plane,
    uint32_t* v_stride,
    int64_t* timestamp_usec);
void np_webrtc_video_frame_release(NPWebRTCVideoFrame* frame);

int np_webrtc_bridge_send_control(NPWebRTCBridge* bridge, const uint8_t* data, size_t size);
int np_webrtc_bridge_receive_control(
    NPWebRTCBridge* bridge,
    uint8_t* destination,
    size_t destination_capacity,
    size_t* required_size);
int np_webrtc_bridge_send_frame_metadata(NPWebRTCBridge* bridge, const uint8_t* data, size_t size);
int np_webrtc_bridge_request_keyframe(NPWebRTCBridge* bridge);
int np_webrtc_bridge_receive_frame_metadata(
    NPWebRTCBridge* bridge,
    uint8_t* destination,
    size_t destination_capacity,
    size_t* required_size);

void np_webrtc_bridge_get_stats(NPWebRTCBridge* bridge, NPWebRTCBridgeStats* stats);

#ifdef __cplusplus
}
#endif
