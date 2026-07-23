#pragma once

#include "NewPipelineClientStaticLighting.h"
#include "NewPipelineScene.h"
#include "NewPipelineTransport.h"

#include <string>
#include <deque>
#include <unordered_map>

namespace wicked_newpipeline
{
struct NewPipelineClientRenderSettings
{
    bool shadow_maps_enabled = true;
    bool ssao_enabled = true;
    bool environment_probe_enabled = true;
    bool baked_lightmaps_enabled = true;
    bool lightmap_bake_requested = false;
    bool remote_gi_enabled = true;
    bool remote_ao_enabled = true;
    float remote_gi_max_weight = 1.0f;
    float remote_ao_max_weight = 1.0f;
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
    std::string GetElasticLightingStatus() const;
    void SetInputActive(bool active);
    void SetRenderSettings(const NewPipelineClientRenderSettings& settings);
    const NewPipelineClientRenderSettings& GetRenderSettings() const { return render_settings; }
    void RequestLightmapBake();
    void CancelLightmapBake();
    bool IsLightmapBakeActive() const;
    std::string GetLightmapBakeStatus() const;
    void RequestStaticLightingBake();
    void CancelStaticLightingBake();
    bool IsStaticLightingBakeActive() const;
    std::string GetStaticLightingBakeStatus() const;
    void RequestReflectionProbeBake();
    void CancelReflectionProbeBake();
    bool IsReflectionProbeBakeActive() const;
    std::string GetReflectionProbeBakeStatus() const { return reflection_probe_status; }
    void SetReflectionProbeDebugMip(uint32_t mip);
    uint32_t GetReflectionProbeDebugMip() const { return reflection_probe_debug_mip; }
    uint32_t GetReflectionProbeDebugMipCount() const;
    const wi::graphics::Texture& GetLocalIndirectFinalInput() const { return local_indirect_final_input; }

    void Start() override;
    void Update(float dt) override;
    void Compose(wi::graphics::CommandList cmd) const override;
    void ResizeBuffers() override;
    void RenderAO(wi::graphics::CommandList cmd) const override;

private:
    void InitializeSceneIfNeeded();
    void UpdateLocalCamera(float dt);
    void PublishControlPacket(float dt);
    void AcquireRemoteVideoFrame(float dt);
    void PollRemoteFrameMetadata();
    bool TryMatchRemoteVideoFrame(RetainedI420Frame& frame, RemoteVideoFrameLayout& layout);
    void PrunePendingRemoteFrames(uint64_t now_usec);
    void ClearPendingRemoteFrames();
    void MaintainWebRTC(float dt);
    bool ValidateRemoteFrame(const RemoteRawFrame& frame, std::string& reason) const;
    bool ValidateRemoteVideoLayout(const RemoteVideoFrameLayout& layout, std::string& reason) const;
    bool IsControlPacketChanged(const ClientControlPacket& packet) const;
    void AcceptRemoteFrame(const RemoteRawFrame& frame);
    void AcceptRemoteVideoFrame(const RetainedI420Frame& frame, const RemoteVideoFrameLayout& layout);
    void CommitAcceptedRemoteMetadata(const RemoteFrameMetadata& metadata);
    void InvalidateRemote(const std::string& reason);
    bool UploadRemoteTextures(const RemoteRawFrame& frame);
    bool UploadRemoteVideoTextures(const RetainedI420Frame& frame, const RemoteVideoFrameLayout& layout);
    const wi::graphics::Texture* GetDebugPreviewTexture() const;
    void DrawUnavailablePreview(wi::graphics::CommandList cmd) const;
    void ApplyRenderSettings(bool log_changes);
    void ConfigureLowEndLocalRendering();
    void ApplyShadowSettings(bool log_changes);
    void ApplySSAOSettings(bool log_changes);
    void UpdateElasticLighting(float dt);
    void ApplyElasticLightingResources();
    void ApplyEnvironmentProbeSettings(bool log_changes);
    void LoadStaticLightingAssets();
    void LoadEnvironmentProbeAsset();
    void InitializeBlackEnvironmentProbe(wi::scene::EnvironmentProbeComponent& probe);
    void PlaceNewEnvironmentProbe(wi::scene::TransformComponent& transform) const;
    void CreateEnvironmentProbeMipViews();
    void UpdateReflectionProbeBake();
    void EnsureSpecularIndirectDebugTexture();
    void ApplyBakedLightmapSettings(bool previous_enabled, bool log_changes, bool force_log = false);
    void DisableBakedLightmaps();
    void RestoreBakedLightmaps();
    bool PrepareLightmapBake();
    void UpdateLightmapBake(float dt);
    bool FillLightmapBakeBatch();
    bool FinalizeOnePendingLightmap();
    void UpdateLightmapBakeWorkload(float dt);
    void UpdateLightmapBakeProgress();
    void ClearLightmapBakeRequests(bool clear_lightmaps);
    void ResetLightmapBakeScheduling();
    uint64_t GetActiveLightmapTexelCount() const;
    uint64_t GetLightmapBakeTotalSamples() const;
    void FinishLightmapBake();
    void FailLightmapBake(const std::string& reason);
    bool SavePreparedScene(const std::string& path, std::string& error);
    bool CommitLightmapBakeFiles(std::string& error);
    bool VerifySourceSceneUnchanged(std::string& error) const;
    void CleanupLightmapBakeTemps();
    void ReloadSceneAfterLightmapBakeAbort();
    void LogLightmapSceneParity(const char* phase);

    RuntimeConfig config;
    enum class LightmapBakeState : uint8_t
    {
        Idle,
        Preparing,
        Baking,
        Saving,
        Completed,
        Cancelled,
        Failed,
    };

    enum class ReflectionProbeBakeState : uint8_t
    {
        Idle,
        Capturing,
        Saving,
        Completed,
        Failed,
    };

    NewPipelineClientRenderSettings render_settings;
    NewPipelineSunState sun_state = MakeSunStateFromAngles(true, -35.0f, 50.0f);
    NewPipelineSunState baked_sun_state = MakeSunStateFromAngles(true, -35.0f, 50.0f);
    DebugPreviewMode debug_preview_mode = DebugPreviewMode::Final;
    wi::scene::Scene local_scene;
    wi::scene::CameraComponent local_camera;
    ClientStaticLighting client_static_lighting;
    ClientLightmapBakeSettings lightmap_bake_settings;
    bool lightmap_bake_denoiser_available = false;
    bool lightmap_bake_denoiser_required = false;
    std::string scene_asset_path;
    wi::ecs::Entity scene_source_root_entity = wi::ecs::INVALID_ENTITY;
    std::string prepared_scene_temp_path;
    std::string prepared_package_temp_path;
    std::string lightmap_bake_status = "Lightmap: idle";
    wi::vector<wi::ecs::Entity> lightmap_bake_queue;
    wi::vector<wi::ecs::Entity> lightmap_bake_completed;
    wi::vector<wi::ecs::Entity> lightmap_bake_active;
    wi::vector<wi::ecs::Entity> lightmap_bake_pending_save;
    std::unordered_map<wi::ecs::Entity, XMUINT2> lightmap_bake_dimensions;
    LightmapBakeState lightmap_bake_state = LightmapBakeState::Idle;
    size_t lightmap_bake_next_index = 0;
    uint32_t lightmap_bake_skipped = 0;
    uint32_t lightmap_bake_iterations_per_frame = 1;
    uint32_t lightmap_bake_effective_iterations_per_frame = 1;
    uint32_t lightmap_bake_adaptation_frames = 0;
    uint32_t previous_raytrace_bounce_count = 0;
    uint64_t source_scene_hash_before_bake = 0;
    uint64_t prepared_derived_scene_hash = 0;
    uint64_t lightmap_bake_scheduled_active_texels = 0;
    uint64_t lightmap_bake_last_progress_time_usec = 0;
    uint64_t lightmap_bake_last_progress_sample_total = 0;
    uint64_t lightmap_bake_rate_time_usec = 0;
    uint64_t lightmap_bake_rate_sample_total = 0;
    float lightmap_bake_frame_time_ema = 0.0f;
    float lightmap_bake_samples_per_second = 0.0f;
    XMFLOAT3 lightmap_irradiance_min = {};
    XMFLOAT3 lightmap_irradiance_sum = {};
    XMFLOAT3 lightmap_irradiance_max = {};
    uint64_t lightmap_valid_texel_count = 0;
    uint64_t lightmap_missing_texel_count = 0;
    uint64_t lightmap_invalid_texel_count = 0;
    SceneParityFingerprint lightmap_bake_scene_fingerprint;
    bool lightmap_bake_scene_fingerprint_valid = false;
    bool lightmap_cancel_requested = false;
    FileMockControlMailbox mock_control_mailbox;
    FileMockRemoteMailbox mock_remote_mailbox;
    WebRTCVideoTransport webrtc_transport;
    struct RemoteVideoUploadSlot
    {
        wi::graphics::Texture upload_y;
        wi::graphics::Texture upload_uv;
        wi::graphics::Texture gpu_y;
        wi::graphics::Texture gpu_uv;
        std::array<wi::graphics::Texture,
            static_cast<size_t>(RemoteBufferSemantic::Count)> semantic_outputs;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::array<RemoteVideoUploadSlot, wi::graphics::GraphicsDevice::GetBufferCount()> remote_video_upload_ring;
    struct PendingRemoteVideoFrame
    {
        RetainedI420Frame frame;
        RemoteVideoFrameLayout pixel_layout;
        uint64_t local_receive_timestamp_usec = 0;
    };
    std::deque<PendingRemoteVideoFrame> pending_remote_video_frames;
    std::array<wi::graphics::Texture, static_cast<size_t>(RemoteBufferSemantic::Count)> accepted_remote_textures;
    uint32_t accepted_remote_buffer_mask = 0;
    uint64_t remote_texture_creation_count = 0;
    uint64_t remote_gpu_upload_bytes = 0;
    WebRTCTransportState previous_webrtc_state = WebRTCTransportState::Disabled;
    std::deque<RemoteVideoFrameLayout> downstream_metadata_cache;
    bool downstream_metadata_active = false;
    uint64_t downstream_metadata_matches = 0;
    uint64_t downstream_metadata_misses = 0;
    uint64_t downstream_metadata_mismatches = 0;
    uint64_t downstream_metadata_first_matches = 0;
    uint64_t downstream_video_first_matches = 0;
    uint64_t downstream_pair_expirations = 0;
    RemoteFrameMetadata accepted_remote_metadata;
    wi::ecs::Entity environment_probe_entity = wi::ecs::INVALID_ENTITY;
    bool environment_probe_created_by_client = false;
    bool environment_probe_load_attempted = false;
    ReflectionProbeBakeState reflection_probe_bake_state = ReflectionProbeBakeState::Idle;
    std::string reflection_probe_status = "Reflection Probe: idle";
    std::string reflection_probe_asset_path;
    wi::vector<int> reflection_probe_mip_subresources;
    uint32_t reflection_probe_debug_mip = 0;
    RemoteConsumeState remote_consume;
    XMFLOAT3 camera_position = XMFLOAT3(0, 2.5f, -8);
    XMFLOAT3 camera_rotation = XMFLOAT3(wi::math::DegreesToRadians(5), 0, 0);
    XMFLOAT4 camera_control_origin = XMFLOAT4(0, 0, 0, 0);
    uint64_t frame_id = 0;
    uint32_t scene_generation = 1;
    ClientControlPacket last_published_control_packet;
    float control_publish_accumulator = 0.0f;
    bool has_published_control_packet = false;
    bool scene_initialized = false;
    bool baked_sun_reference_valid = false;
    bool static_lighting_bake_requested = false;
    bool camera_control_start = true;
    bool input_active = true;
    mutable wi::graphics::Texture local_ao_snapshot;
    wi::graphics::Texture local_indirect_final_input;
    wi::graphics::Texture local_specular_indirect;
    wi::graphics::Texture local_specular_indirect_pre_ao;
    wi::graphics::Texture local_primary_light_visibility;
    wi::graphics::Texture elastic_indirect_diffuse;
    wi::graphics::Texture elastic_ao;
    float elastic_remote_gi_weight = 0.0f;
    float elastic_remote_ao_weight = 0.0f;
    float elastic_remote_quality = 0.0f;
    bool          status_logged = false;
    uint32_t remote_ddgi_frame_index = 0;
    DDGIResetReason remote_ddgi_reset_reason = DDGIResetReason::None;
    bool mock_control_publish_logged = false;
    bool remote_acquire_logged = false;
    bool remote_unchanged_skip_logged = false;
    bool remote_payload_read_logged = false;
    mutable bool debug_preview_invalid_logged = false;
};
} // namespace wicked_newpipeline
