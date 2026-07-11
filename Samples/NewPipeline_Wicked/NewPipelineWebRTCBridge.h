#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NPWebRTCBridge NPWebRTCBridge;

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
    uint64_t sent_controls;
    uint64_t received_controls;
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
int np_webrtc_bridge_receive_i420(
    NPWebRTCBridge* bridge,
    uint32_t* width,
    uint32_t* height,
    uint8_t* destination,
    size_t destination_capacity,
    size_t* required_size);

int np_webrtc_bridge_send_control(NPWebRTCBridge* bridge, const uint8_t* data, size_t size);
int np_webrtc_bridge_receive_control(
    NPWebRTCBridge* bridge,
    uint8_t* destination,
    size_t destination_capacity,
    size_t* required_size);

void np_webrtc_bridge_get_stats(NPWebRTCBridge* bridge, NPWebRTCBridgeStats* stats);

#ifdef __cplusplus
}
#endif
