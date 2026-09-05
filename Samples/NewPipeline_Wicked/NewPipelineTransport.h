#pragma once

#include "NewPipelineProtocol.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wicked_newpipeline
{
struct RemoteSemanticSource
{
    RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteIndirectDiffuse;
    uint32_t width = 0;
    uint32_t height = 0;
    bool available = false;
    RemoteBufferEncoding encoding = RemoteBufferEncoding::LinearRGBA8;
};

struct RemoteFrameSource
{
    RemoteFrameMetadata metadata;
    std::array<RemoteSemanticSource,
        static_cast<size_t>(RemoteBufferSemantic::Count)> buffers = {};

    RemoteFrameSource();
};

// Non-owning I420 plane views whose lifetime is retained by frame_lifetime.
// Copying this object copies only the small ownership token, never the planes.
struct RetainedI420Frame
{
    uint32_t width = 0;
    uint32_t height = 0;
    const uint8_t* y_plane = nullptr;
    uint32_t y_stride = 0;
    const uint8_t* u_plane = nullptr;
    uint32_t u_stride = 0;
    const uint8_t* v_plane = nullptr;
    uint32_t v_stride = 0;
    int64_t timestamp_usec = 0;
    std::shared_ptr<void> frame_lifetime;

    bool IsValid() const;
};

// A retained Windows NV12 texture shared between DX12 and D3D11/Media
// Foundation. The handles remain valid for frame_lifetime. The producer and
// consumer values belong to one monotonic shared-fence timeline.
struct RetainedNV12Frame
{
    uint32_t width = 0;
    uint32_t height = 0;
    void* texture_shared_handle = nullptr;
    void* fence_shared_handle = nullptr;
    uint64_t producer_fence_value = 0;
    uint64_t consumer_fence_value = 0;
    uint64_t adapter_luid = 0;
    uint32_t rtp_timestamp = 0;
    int64_t timestamp_usec = 0;
    std::shared_ptr<void> frame_lifetime;
    void (*mark_completion_scheduled)(void*) = nullptr;
    void* completion_context = nullptr;

    bool IsValid() const;
    void MarkCompletionScheduled() const;
};

struct RetainedRemoteVideoFrame
{
    RetainedI420Frame i420;
    RetainedNV12Frame nv12;

    bool IsValid() const { return i420.IsValid() || nv12.IsValid(); }
    bool IsNativeNV12() const { return nv12.IsValid(); }
};

struct RemoteVideoTileLayout
{
    RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteIndirectDiffuse;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t origin_x = 0;
    uint32_t origin_y = 0;
    bool available = false;
    RemoteBufferEncoding encoding = RemoteBufferEncoding::LinearRGBA8;
};

struct RemoteVideoFrameLayout
{
    RemoteFrameMetadata metadata;
    uint32_t protocol_version = kRemoteVideoWireVersionV3;
    uint32_t encoding_profile_id = 0;
    RemoteQualityTierV3 quality_tier = RemoteQualityTierV3::High;
    uint64_t source_control_frame_id = 0;
    // Stable over content-frame/confidence/light-identity updates. This
    // identifies only the negotiated semantic atlas geometry and encoding.
    uint32_t layout_checksum = 0;
    // Per-frame checksum of the complete serialized V3 descriptor contract.
    uint32_t descriptor_checksum = 0;
    uint32_t metadata_rows = 0;
    uint32_t video_width = 0;
    uint32_t video_height = 0;
    std::array<RemoteVideoTileLayout, static_cast<size_t>(RemoteBufferSemantic::Count)> tiles = {};
    RemoteFrameContractV3 contract_v3;
};

struct RemoteBufferContentStateV3
{
    uint64_t frame_id = 0;
    uint32_t generation = 0;
    uint16_t confidence_unorm = 0;
};
bool BuildRemoteVideoFrameLayoutV3(
    const RemoteFrameSource& frame,
    const RemoteStreamSelection& selection,
    uint64_t source_control_frame_id,
    uint64_t stable_shadow_id,
    uint32_t stable_shadow_generation,
    RemoteVideoFrameLayout& layout,
    std::vector<uint8_t>& metadata_luma,
    std::string* error = nullptr,
    const std::array<RemoteBufferContentStateV3,
        static_cast<size_t>(RemoteBufferSemantic::Count)>*
        content_states = nullptr);
bool DecodeRemoteVideoFrameLayout(
    const RetainedI420Frame& video, RemoteVideoFrameLayout& layout, std::string* error = nullptr);
bool ValidateRemoteTransportSelfTest(std::string* error = nullptr);

enum class WebRTCTransportState : uint8_t
{
    Disabled,
    Starting,
    Signaling,
    Connected,
    Failed,
};

struct WebRTCTransportStats
{
    WebRTCTransportState state = WebRTCTransportState::Disabled;
    uint64_t sent_frames = 0;
    uint64_t received_frames = 0;
    uint64_t dropped_frames = 0;
    uint32_t decoded_queue_depth = 0;
    uint64_t sent_controls = 0;
    uint64_t received_controls = 0;
    uint64_t connection_attempts = 0;
    uint64_t disconnected_frame_drops = 0;
    uint64_t busy_frame_drops = 0;
    uint64_t disconnected_control_drops = 0;
    uint64_t queued_control_drops = 0;
    uint64_t lifecycle_last_duration_usec = 0;
    uint64_t cpu_full_frame_copy_bytes = 0;
    uint64_t cpu_conversion_usec = 0;
    uint64_t retained_frame_acquires = 0;
    uint64_t retained_i420_bytes = 0;
    uint64_t compressed_bytes_sent = 0;
    uint64_t compressed_bytes_received = 0;
    uint64_t total_encode_time_usec = 0;
    uint64_t total_decode_time_usec = 0;
    uint64_t frames_encoded = 0;
    uint64_t frames_decoded = 0;
    std::string codec_name = "unknown";
    std::string codec_implementation = "unknown";
    bool power_efficient_codec = false;
    bool native_codec = false;
    std::string requested_encoder_mode = "unknown";
    std::string active_encoder_mode = "unknown";
    std::string input_surface = "unknown";
    std::string codec_profile = "unknown";
    std::string fallback_reason = "none";
    std::string status;
};

const char* ToString(WebRTCTransportState state);

class WebRTCVideoTransport final
{
public:
    WebRTCVideoTransport();
    ~WebRTCVideoTransport();
    WebRTCVideoTransport(const WebRTCVideoTransport&) = delete;
    WebRTCVideoTransport& operator=(const WebRTCVideoTransport&) = delete;

    bool RequestStart(
        bool server,
        const RuntimeConfig& config,
        uint64_t adapter_luid = 0,
        std::string* error = nullptr);
    void RequestStop();
    void Stop();
    void Tick();
    bool SendControl(const ClientControlPacket& packet);
    bool TryReceiveControl(ClientControlPacket& packet);
    bool SendI420Frame(const RetainedI420Frame& frame);
    bool SendNV12Frame(const RetainedNV12Frame& frame);
    bool SendFrameMetadata(const RemoteVideoFrameLayout& layout);
    bool SendStreamStatus(const RemoteStreamStatus& status);
    bool RequestKeyframe();
    bool TryReceiveFrameMetadata(
        RemoteVideoFrameLayout& layout,
        RemoteStreamStatus* stream_status = nullptr);
    bool TryAcquireI420Frame(RetainedI420Frame& frame);
    bool TryAcquireVideoFrame(RetainedRemoteVideoFrame& frame);
    bool SupportsNativeNV12() const;
    void ReportNativeNV12Failure();
    WebRTCTransportStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend bool ValidateRemoteTransportLifecycleSelfTest(std::string* error);
};

bool ValidateRemoteTransportLifecycleSelfTest(std::string* error = nullptr);
} // namespace wicked_newpipeline
