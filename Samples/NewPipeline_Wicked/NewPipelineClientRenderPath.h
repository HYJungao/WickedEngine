#pragma once

#include "NewPipelineScene.h"
#include "NewPipelineTransport.h"

#include <limits>
#include <string>

namespace wicked_newpipeline
{
struct NewPipelineClientRenderSettings
{
    bool shadow_maps_enabled = true;
    bool ssao_enabled = true;
    bool environment_probe_enabled = true;
    bool baked_lightmaps_enabled = true;
    bool lightmap_bake_requested = false;
};

struct RemoteConsumeState
{
    uint64_t latest_frame_id = 0;
    uint64_t accepted_frame_id = 0;
    uint64_t history_frame_id = 0;
    uint32_t latest_generation = 0;
    uint32_t accepted_generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float confidence = 0.0f;
    float stale_timer = 0.0f;
    std::string fallback_reason = "no remote frame";
    bool accepted_valid = false;
    bool history_valid = false;
    bool placeholder = false;
    bool stale_logged = false;
    bool placeholder_logged = false;
    bool no_remote_logged = false;
};

class NewPipelineClientRenderPath final : public wi::RenderPath3D
{
public:
    void SetRuntimeConfig(const RuntimeConfig& config);
    void SetSunState(const NewPipelineSunState& state);
    const NewPipelineSunState& GetSunState() const { return sun_state; }
    void SetDebugPreviewMode(DebugPreviewMode mode);
    DebugPreviewMode GetDebugPreviewMode() const { return debug_preview_mode; }
    std::string GetEffectiveAlgorithmSummary() const;
    std::string GetDebugStatusSummary() const;
    void SetInputActive(bool active);
    void SetRenderSettings(const NewPipelineClientRenderSettings& settings);
    const NewPipelineClientRenderSettings& GetRenderSettings() const { return render_settings; }

    void Start() override;
    void Update(float dt) override;
    void Compose(wi::graphics::CommandList cmd) const override;
    void ResizeBuffers() override;
    void RenderAO(wi::graphics::CommandList cmd) const override;
    void RenderPostprocessChain(wi::graphics::CommandList cmd) const override;

private:
    void InitializeSceneIfNeeded();
    void UpdateLocalCamera(float dt);
    void PublishControlPacket(float dt);
    void AcquireRemoteVideoFrame(float dt);
    void MaintainWebRTC(float dt);
    bool ValidateRemoteFrame(const RemoteRawFrame& frame, std::string& reason) const;
    bool IsControlPacketChanged(const ClientControlPacket& packet) const;
    void AcceptRemoteFrame(const RemoteRawFrame& frame);
    void InvalidateRemote(const std::string& reason);
    bool UploadRemoteTextures(const RemoteRawFrame& frame);
    const wi::graphics::Texture* GetDebugPreviewTexture() const;
    bool EnsureLocalShadowSnapshot(uint32_t width, uint32_t height) const;
    void DrawUnavailablePreview(wi::graphics::CommandList cmd) const;
    void ApplyRenderSettings(bool log_changes);
    void ConfigureComparableLocalBuffers();
    void ResetLocalDDGI(DDGIResetReason reason);
    void ApplyShadowSettings(bool log_changes);
    void ApplySSAOSettings(bool log_changes);
    void ApplyEnvironmentProbeSettings(bool log_changes);
    void ApplyBakedLightmapSettings(bool previous_enabled, bool log_changes, bool force_log = false);
    void ApplyLightmapBakeRequest(bool previous_requested, bool log_changes, bool force_log = false);
    void DisableBakedLightmaps();
    void RestoreBakedLightmaps();
    bool ObjectSupportsLightmapBake(const wi::scene::ObjectComponent& object) const;

    RuntimeConfig config;
    struct SavedLightmap
    {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        uint32_t width = 0;
        uint32_t height = 0;
        wi::vector<uint8_t> texture_data;
    };

    NewPipelineClientRenderSettings render_settings;
    NewPipelineSunState sun_state = MakeSunStateFromAngles(true, -35.0f, 50.0f);
    DebugPreviewMode debug_preview_mode = DebugPreviewMode::Final;
    wi::scene::Scene local_scene;
    wi::scene::CameraComponent local_camera;
    wi::vector<SavedLightmap> saved_lightmaps;
    FileMockControlMailbox mock_control_mailbox;
    FileMockRemoteMailbox mock_remote_mailbox;
    WebRTCVideoTransport webrtc_transport;
    std::array<wi::graphics::Texture, static_cast<size_t>(RemoteBufferSemantic::Count)> accepted_remote_textures;
    uint32_t accepted_remote_buffer_mask = 0;
    wi::ecs::Entity environment_probe_entity = wi::ecs::INVALID_ENTITY;
    RemoteConsumeState remote_consume;
    XMFLOAT3 camera_position = XMFLOAT3(0, 2.5f, -8);
    XMFLOAT3 camera_rotation = XMFLOAT3(wi::math::DegreesToRadians(5), 0, 0);
    XMFLOAT4 camera_control_origin = XMFLOAT4(0, 0, 0, 0);
    uint64_t frame_id = 0;
    uint32_t scene_generation = 1;
    ClientControlPacket last_published_control_packet;
    float control_publish_accumulator = 0.0f;
    float webrtc_retry_accumulator = 0.0f;
    bool has_published_control_packet = false;
    bool scene_initialized = false;
    bool camera_control_start = true;
    bool input_active = true;
    bool hardware_raytracing = false;
    mutable wi::graphics::Texture local_ao_snapshot;
    mutable wi::graphics::Texture local_shadow_snapshot;
    mutable uint32_t local_shadow_index = std::numeric_limits<uint32_t>::max();
    mutable bool local_shadow_snapshot_valid = false;
    bool          status_logged = false;
    uint32_t remote_ddgi_frame_index = 0;
    DDGIResetReason remote_ddgi_reset_reason = DDGIResetReason::None;
    DDGIResetReason local_ddgi_reset_reason = DDGIResetReason::InitialScene;
    NewPipelineSunState local_ddgi_reset_reference_sun;
    bool mock_control_publish_logged = false;
    bool remote_acquire_logged = false;
    bool remote_unchanged_skip_logged = false;
    bool remote_payload_read_logged = false;
    mutable bool debug_preview_invalid_logged = false;
};
} // namespace wicked_newpipeline
