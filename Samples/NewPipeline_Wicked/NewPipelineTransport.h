#pragma once

#include "NewPipelineProtocol.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wicked_newpipeline
{
struct RemoteRawBuffer
{
    RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteIndirectDiffuse;
    uint32_t width = 0;
    uint32_t height = 0;
    bool available = false;
    RemoteBufferEncoding encoding = RemoteBufferEncoding::LinearRGBA8;
    std::vector<uint8_t> payload_rgba8;
    std::vector<uint16_t> payload_rgba16f;
};

struct RemoteRawFrame
{
    RemoteFrameMetadata metadata;
    std::array<RemoteRawBuffer, static_cast<size_t>(RemoteBufferSemantic::Count)> buffers = {};

    RemoteRawFrame();
    RemoteRawBuffer* FindBuffer(RemoteBufferSemantic semantic);
    const RemoteRawBuffer* FindBuffer(RemoteBufferSemantic semantic) const;
};

struct PackedRemoteVideoFrame
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> i420;
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
    uint32_t protocol_version = kRemoteVideoWireVersion;
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

// Backend-neutral ownership token for a decoded or encoder-ready GPU surface.
// The native backend owns the concrete resource/fence types. RenderPath code
// only moves this token and retains lifetime until GPU consumers finish.
enum class RemoteSurfaceBackend : uint8_t
{
    None,
    D3D12,
    Metal,
    Vulkan,
};

enum class RemoteSurfaceFormat : uint8_t
{
    Unknown,
    NV12,
};

struct RemoteSurfaceToken
{
    uint64_t frame_id = 0;
    uint64_t timestamp_usec = 0;
    uint32_t generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    RemoteSurfaceBackend backend = RemoteSurfaceBackend::None;
    RemoteSurfaceFormat format = RemoteSurfaceFormat::Unknown;
    void* native_resource = nullptr;
    void* ready_fence = nullptr;
    uint64_t ready_fence_value = 0;
    std::shared_ptr<void> lifetime;

    bool IsValid() const
    {
        return backend != RemoteSurfaceBackend::None && format != RemoteSurfaceFormat::Unknown &&
            width > 0 && height > 0 && native_resource != nullptr && lifetime != nullptr;
    }
};

struct RemoteVideoCodecCapabilities
{
    bool software_i420 = true;
    bool native_surface_encode = false;
    bool native_surface_decode = false;
    bool h264 = false;
    bool nv12 = false;
    std::string backend_name = "software-i420";
    std::string unavailable_reason;
};

// Platform codec implementations live behind this contract. Implementations
// must use bounded latest-frame queues and never wait from RenderPath calls.
class IRemoteVideoCodecBackend
{
public:
    virtual ~IRemoteVideoCodecBackend() = default;
    virtual RemoteVideoCodecCapabilities GetCapabilities() const = 0;
    virtual bool SubmitEncodeSurface(RemoteSurfaceToken surface) = 0;
    virtual bool TryAcquireDecodedSurface(RemoteSurfaceToken& surface) = 0;
    virtual void RequestKeyframe() = 0;
    virtual void Flush() = 0;
};

// Transitional software-video format: semantic pixels are packed into I420 and
// the legacy luma metadata band is retained for dual-write validation.  The
// authoritative frame metadata is also sent through np.frame_meta.
bool EncodeRemoteVideoFrame(const RemoteRawFrame& frame, PackedRemoteVideoFrame& video, std::string* error = nullptr);
bool BuildRemoteVideoFrameLayout(
    const RemoteRawFrame& frame,
    RemoteVideoFrameLayout& layout,
    std::vector<uint8_t>& metadata_luma,
    std::string* error = nullptr);
struct RemoteBufferContentStateV3
{
    uint64_t frame_id = 0;
    uint32_t generation = 0;
    uint16_t confidence_unorm = 0;
};
bool BuildRemoteVideoFrameLayoutV3(
    const RemoteRawFrame& frame,
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
bool DecodeRemoteVideoFrame(const PackedRemoteVideoFrame& video, RemoteRawFrame& frame, std::string* error = nullptr);
bool DecodeRemoteVideoFrame(const RetainedI420Frame& video, RemoteRawFrame& frame, std::string* error = nullptr);
bool DecodeRemoteVideoFrameLayout(
    const RetainedI420Frame& video, RemoteVideoFrameLayout& layout, std::string* error = nullptr);
bool ValidateRemoteVideoV2RoundTrip(std::string* error = nullptr);
bool ValidateRemoteTransportSelfTest(std::string* error = nullptr);

class IRemoteTransport
{
public:
    virtual ~IRemoteTransport() = default;

    virtual RemoteSourceMode GetSourceMode() const = 0;
};

class NullRemoteTransport final : public IRemoteTransport
{
public:
    explicit NullRemoteTransport(RemoteSourceMode mode) : source_mode(mode) {}

    RemoteSourceMode GetSourceMode() const override { return source_mode; }

private:
    RemoteSourceMode source_mode = RemoteSourceMode::Mock;
};

class InProcessControlMailbox final
{
public:
    void Publish(const ClientControlPacket& packet);
    bool TryConsumeLatest(ClientControlPacket& packet);
    bool PeekLatest(ClientControlPacket& packet) const;
    void Reset();

private:
    mutable std::mutex     mutex;
    ClientControlPacket    latest_packet;
    uint64_t               latest_sequence  = 0;
    uint64_t               consumed_sequence = 0;
};

InProcessControlMailbox& GetInProcessControlMailbox();

std::string GetDefaultMockMailboxDirectory();
std::string GetDefaultMockRemoteMailboxDirectory();
std::string GetDefaultMockControlMailboxDirectory();

class FileMockControlMailbox final
{
public:
    explicit FileMockControlMailbox(std::string root_directory = GetDefaultMockControlMailboxDirectory());

    const std::string& GetRootDirectory() const { return root_directory; }

    bool PublishLatest(const ClientControlPacket& packet, std::string* error = nullptr) const;
    bool TryConsumeLatest(ClientControlPacket& packet, std::string* error = nullptr);

private:
    std::string root_directory;
    uint64_t consumed_frame_id = 0;
};

class FileMockRemoteMailbox final
{
public:
    explicit FileMockRemoteMailbox(std::string root_directory = GetDefaultMockRemoteMailboxDirectory());

    const std::string& GetRootDirectory() const { return root_directory; }

    bool PublishLatest(const RemoteRawFrame& frame, std::string* error = nullptr) const;
    bool TryReadLatest(RemoteRawFrame& frame, std::string* error = nullptr) const;

private:
    std::string root_directory;
};

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
    std::string codec_fallback_reason;
    bool power_efficient_codec = false;
    bool native_codec = false;
    std::string status;
};

const char* ToString(WebRTCTransportState state);

class WebRTCVideoTransport final : public IRemoteTransport
{
public:
    WebRTCVideoTransport();
    ~WebRTCVideoTransport() override;
    WebRTCVideoTransport(const WebRTCVideoTransport&) = delete;
    WebRTCVideoTransport& operator=(const WebRTCVideoTransport&) = delete;

    RemoteSourceMode GetSourceMode() const override { return RemoteSourceMode::WebRTC; }
    bool RequestStart(bool server, const RuntimeConfig& config, std::string* error = nullptr);
    void RequestStop();
    void Stop();
    void Tick();
    bool SendControl(const ClientControlPacket& packet);
    bool TryReceiveControl(ClientControlPacket& packet);
    bool SendFrame(const RemoteRawFrame& frame);
    bool SendI420Frame(const RetainedI420Frame& frame);
    bool SendFrameMetadata(const RemoteVideoFrameLayout& layout);
    bool SendStreamStatus(const RemoteStreamStatus& status);
    bool RequestKeyframe();
    bool TryReceiveFrameMetadata(
        RemoteVideoFrameLayout& layout,
        RemoteStreamStatus* stream_status = nullptr);
    bool TryAcquireI420Frame(RetainedI420Frame& frame);
    bool TryReceiveFrame(RemoteRawFrame& frame);
    WebRTCTransportStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace wicked_newpipeline
