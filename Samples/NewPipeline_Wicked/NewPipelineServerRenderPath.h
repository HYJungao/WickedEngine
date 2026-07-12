#pragma once

#include "NewPipelineScene.h"
#include "NewPipelineTransport.h"

namespace wicked_newpipeline
{
struct NewPipelineServerSettings
{
    bool ddgi_enabled = true;
    bool ddgi_debug_formal = false;
    uint32_t ddgi_ray_count = 64;
    float remote_publish_fps = 1.0f;
};

class NewPipelineServerRenderPath final : public wi::RenderPath3D
{
public:
    void SetRuntimeConfig(const RuntimeConfig& config);
    void SetServerSettings(const NewPipelineServerSettings& settings);
    void SetDebugPreviewMode(DebugPreviewMode mode);
    DebugPreviewMode GetDebugPreviewMode() const { return debug_preview_mode; }
    std::string GetEffectiveAlgorithmSummary() const;

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
    void LogDDGIStatusIfNeeded();
    void PublishRemotePayload(float dt);
    void MaintainWebRTC(float dt);
    bool EnsureTransportTexture(RemoteBufferSemantic semantic, uint32_t width, uint32_t height);
    bool EnsureShadowSliceTexture(uint32_t width, uint32_t height);
    const wi::graphics::Texture* GetDebugPreviewTexture() const;

    RuntimeConfig config;
    NewPipelineServerSettings settings;
    wi::scene::Scene local_scene;
    wi::scene::CameraComponent local_camera;
    FileMockControlMailbox mock_control_mailbox;
    FileMockRemoteMailbox mock_remote_mailbox;
    WebRTCVideoTransport webrtc_transport;
    std::array<wi::graphics::Texture, static_cast<size_t>(RemoteBufferSemantic::Count)> transport_textures;
    mutable wi::graphics::Texture shadow_slice_texture;
    mutable wi::graphics::Texture local_ao_snapshot;
    DebugPreviewMode debug_preview_mode = DebugPreviewMode::Final;
    bool hardware_raytracing = false;
    float mock_publish_accumulator = 0.0f;
    float webrtc_retry_accumulator = 0.0f;
    uint64_t last_applied_frame_id = 0;
    uint64_t remote_frame_id = 0;
    uint32_t remote_generation = 1;
    bool scene_initialized = false;
    bool          status_logged = false;
    bool ddgi_formal_status_logged = false;
    bool mock_control_source_logged = false;
    bool mock_remote_publish_logged = false;
    bool mock_remote_disabled_logged = false;
    mutable bool debug_preview_invalid_logged = false;
};
} // namespace wicked_newpipeline
