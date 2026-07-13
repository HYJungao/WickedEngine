#pragma once

#include "NewPipelineScene.h"
#include "NewPipelineTransport.h"

#include <condition_variable>
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
    void Compose(wi::graphics::CommandList cmd) const override;
    void ResizeBuffers() override;
    void RenderAO(wi::graphics::CommandList cmd) const override;
    void RenderPostprocessChain(wi::graphics::CommandList cmd) const override;

private:
    void InitializeSceneIfNeeded();
    void ConfigureDDGI();
    void ApplyLatestControlPacket();
    void ResetDDGI(DDGIResetReason reason);
    void LogDDGIStatusIfNeeded();
    void PublishRemotePayload(float dt);
    void MaintainWebRTC(float dt);
    bool EnsureTransportTexture(RemoteBufferSemantic semantic, uint32_t width, uint32_t height);
    void EncodeTransportTexture(RemoteBufferSemantic semantic, const wi::graphics::Texture& source,
        wi::graphics::Texture& destination, wi::graphics::CommandList cmd) const;
    bool EnsureShadowSliceTexture(uint32_t width, uint32_t height) const;
    const wi::graphics::Texture* GetDebugPreviewTexture() const;
    void ConsumeCompletedReadback();
    void StartPublishWorker();
    void StopPublishWorker();
    void QueueFrameForPublish(RemoteRawFrame&& frame);
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
    std::array<ReadbackSlot, kReadbackRingSize> readback_ring;
    size_t readback_write_index = 0;
    mutable wi::graphics::Texture shadow_slice_texture;
    mutable wi::graphics::Texture local_ao_snapshot;
    DebugPreviewMode debug_preview_mode = DebugPreviewMode::Final;
    bool hardware_raytracing = false;
    float mock_publish_accumulator = 0.0f;
    float webrtc_retry_accumulator = 0.0f;
    uint64_t last_applied_frame_id = 0;
    ClientControlPacket last_applied_control = {};
    ClientControlPacket ddgi_reset_reference_control = {};
    bool has_last_applied_control = false;
    bool has_ddgi_reset_reference_control = false;
    DDGIResetReason ddgi_reset_reason = DDGIResetReason::InitialScene;
    uint64_t ddgi_reset_serial = 0;
    uint64_t ddgi_announced_reset_serial = 0;
    uint64_t remote_frame_id = 0;
    uint32_t remote_generation = 1;
    bool scene_initialized = false;
    bool          status_logged = false;
    bool ddgi_formal_status_logged = false;
    bool mock_control_source_logged = false;
    bool mock_remote_publish_logged = false;
    bool mock_remote_disabled_logged = false;
    mutable bool debug_preview_invalid_logged = false;
    mutable uint32_t authoritative_shadow_index = std::numeric_limits<uint32_t>::max();
    mutable bool shadow_snapshot_valid = false;
    std::thread publish_worker;
    std::mutex publish_mutex;
    std::mutex webrtc_mutex;
    std::condition_variable publish_cv;
    std::optional<RemoteRawFrame> pending_publish_frame;
    bool publish_worker_stop = false;
};
} // namespace wicked_newpipeline
