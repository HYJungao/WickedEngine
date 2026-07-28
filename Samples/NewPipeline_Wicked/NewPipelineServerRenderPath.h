#pragma once

#include "NewPipelineScene.h"
#include "NewPipelineTransport.h"

#include <condition_variable>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace wicked_newpipeline
{
struct NewPipelineServerSettings
{
    bool ddgi_enabled = true;
    bool ddgi_debug_formal = false;
    uint32_t ddgi_ray_count = 256;
    float remote_publish_fps = 1.0f;
};

class NewPipelineServerRenderPath final : public wi::RenderPath3D
{
public:
    ~NewPipelineServerRenderPath() override;
    void SetRuntimeConfig(const RuntimeConfig& config);
    void SetServerSettings(const NewPipelineServerSettings& settings);
    void SetDebugPreviewMode(DebugPreviewMode mode);
    DebugPreviewMode GetDebugPreviewMode() const { return debug_preview_mode; }
    std::string GetEffectiveAlgorithmSummary() const;
    std::string GetDebugStatusSummary() const;

    void Start() override;
    void Update(float dt) override;
    void Render() const override;
    void Compose(wi::graphics::CommandList cmd) const override;
    void ResizeBuffers() override;
    void RenderAO(wi::graphics::CommandList cmd) const override;

private:
    void InitializeSceneIfNeeded();
    void ConfigureDDGI();
    void ApplyLatestControlPacket();
    void RefreshAuthoritativeShadowIdentity() const;
    void ResetDDGI(DDGIResetReason reason);
    void LogDDGIStatusIfNeeded();
    void PublishRemotePayload(float dt);
    void CaptureRequestedRemotePayload();
    void MaintainWebRTC(float dt);
    bool EnsureTransportTexture(RemoteBufferSemantic semantic, uint32_t width, uint32_t height);
    bool EnsureTransportAtlasTexture(uint32_t width, uint32_t height);
    bool EncodeTransportTexture(RemoteBufferSemantic semantic, const wi::graphics::Texture& source,
        wi::graphics::Texture& destination, wi::graphics::CommandList cmd) const;
    const wi::graphics::Texture* GetDebugPreviewTexture() const;
    void ConsumeCompletedReadback();
    void CapturePackedRemoteFrame(const std::array<const wi::graphics::Texture*,
        static_cast<size_t>(RemoteBufferSemantic::Count)>& sources);
    void ConsumeCompletedPackedReadback();
    void StartPublishWorker();
    void StopPublishWorker();
    void QueueFrameForPublish(RemoteRawFrame&& frame);
    void QueueI420FrameForPublish(RetainedI420Frame&& frame, const RemoteVideoFrameLayout& layout);
    void DrawUnavailablePreview(wi::graphics::CommandList cmd) const;

    static constexpr size_t kReadbackRingSize = 3;
    struct ReadbackSlot
    {
        std::array<wi::graphics::Texture, static_cast<size_t>(RemoteBufferSemantic::Count)> textures;
        RemoteFrameMetadata metadata;
        uint32_t available_mask = 0;
        bool pending = false;
    };

    RuntimeConfig config;
    NewPipelineServerSettings settings;
    wi::scene::Scene local_scene;
    wi::scene::CameraComponent local_camera;
    FileMockControlMailbox mock_control_mailbox;
    FileMockRemoteMailbox mock_remote_mailbox;
    WebRTCVideoTransport webrtc_transport;
    std::array<wi::graphics::Texture, static_cast<size_t>(RemoteBufferSemantic::Count)> transport_textures;
    wi::graphics::Texture transport_atlas_texture;
    std::array<ReadbackSlot, kReadbackRingSize> readback_ring;
    size_t readback_write_index = 0;
    struct PackedReadbackStorage
    {
        wi::graphics::GPUBuffer buffer;
    };
    struct PackedReadbackSlot
    {
        wi::graphics::GPUBuffer metadata_upload;
        wi::graphics::GPUBuffer packed_gpu;
        std::shared_ptr<PackedReadbackStorage> readback;
        RemoteVideoFrameLayout layout;
        uint32_t y_stride = 0;
        uint32_t uv_stride = 0;
        uint32_t u_offset = 0;
        uint32_t v_offset = 0;
        uint64_t gpu_ready_frame = 0;
        bool pending = false;
    };
    std::array<PackedReadbackSlot, kReadbackRingSize> packed_readback_ring;
    size_t packed_readback_write_index = 0;
    uint32_t packed_layout_width = 0;
    uint32_t packed_layout_height = 0;
    RemoteVideoFrameLayout packed_layout_contract;
    bool packed_layout_contract_valid = false;
    uint32_t transport_preview_available_mask = 0;
    mutable wi::graphics::Texture local_ao_snapshot;
    wi::graphics::Texture local_specular_indirect_pre_ao;
    wi::graphics::Texture local_primary_light_visibility;
    DebugPreviewMode debug_preview_mode = DebugPreviewMode::Final;
    bool hardware_raytracing = false;
    float mock_publish_accumulator = 0.0f;
    uint64_t last_applied_frame_id = 0;
    ClientControlPacket last_applied_control = {};
    ClientControlPacket ddgi_reset_reference_control = {};
    bool has_last_applied_control = false;
    bool has_ddgi_reset_reference_control = false;
    bool camera_cut_pending = false;
    DDGIResetReason ddgi_reset_reason = DDGIResetReason::InitialScene;
    uint64_t ddgi_reset_serial = 0;
    uint64_t ddgi_announced_reset_serial = 0;
    uint64_t remote_frame_id = 0;
    uint32_t remote_generation = 1;
    RemoteStreamSelection remote_stream_selection = {
        kRemoteVideoWireVersionV3,
        kRemoteEncodingProfileI420V3,
        RemoteQualityTierV3::High};
    bool remote_stream_selection_initialized = false;
    std::optional<RemoteStreamStatus> pending_stream_status;
    std::array<RemoteBufferContentStateV3,
        static_cast<size_t>(RemoteBufferSemantic::Count)>
        remote_content_states = {};
    uint64_t remote_content_control_frame_id = 0;
    std::array<uint64_t,
        static_cast<size_t>(RemoteBufferSemantic::Count)>
        remote_content_updates = {};
    bool remote_protocol_mismatch_logged = false;
    bool scene_initialized = false;
    bool          status_logged = false;
    bool ddgi_formal_status_logged = false;
    bool mock_control_source_logged = false;
    bool mock_remote_publish_logged = false;
    bool mock_remote_disabled_logged = false;
    bool remote_capture_requested = false;
    mutable bool debug_preview_invalid_logged = false;
    mutable uint32_t authoritative_shadow_index = std::numeric_limits<uint32_t>::max();
    mutable uint64_t authoritative_shadow_light_id = 0;
    mutable uint32_t authoritative_shadow_light_generation = 0;
    mutable wi::ecs::Entity authoritative_shadow_light_entity = wi::ecs::INVALID_ENTITY;
    std::thread publish_worker;
    std::mutex publish_mutex;
    std::condition_variable publish_cv;
    std::optional<RemoteRawFrame> pending_publish_frame;
    struct PendingI420Publish
    {
        RetainedI420Frame frame;
        RemoteVideoFrameLayout layout;
    };
    std::optional<PendingI420Publish> pending_i420_frame;
    std::atomic<uint64_t> publish_queue_drops{0};
    uint64_t remote_capture_count = 0;
    uint64_t remote_capture_drops = 0;
    uint64_t gpu_readback_bytes = 0;
    uint64_t cpu_readback_copy_bytes = 0;
    float transport_telemetry_window_seconds = 0.0f;
    uint64_t transport_telemetry_previous_bytes = 0;
    uint64_t transport_bitrate_bps = 0;
    WebRTCTransportState previous_webrtc_state = WebRTCTransportState::Disabled;
    bool publish_worker_stop = false;
};
} // namespace wicked_newpipeline
