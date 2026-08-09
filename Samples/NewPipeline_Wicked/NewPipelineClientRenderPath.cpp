#include "NewPipelineClientRenderPath.h"

#include "wiArchive.h"
#include "wiHelper.h"
#include "wiImage.h"
#include "wiFont.h"
#include "wiTextureHelper.h"
#include "shaders/ShaderInterop_DDGI.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace wicked_newpipeline
{
namespace
{
// The validation preset intentionally supports a 1 FPS sender. Leave room for
// encode/decode and scheduling jitter before falling back to local rendering.
constexpr float kRemoteStaleTimeoutSeconds = 5.0f;
constexpr uint64_t kMaxRemoteFrameAgeUsec = 5'000'000;
constexpr uint64_t kRemotePairTimeoutUsec = 1'000'000;
constexpr size_t kMaxPendingRemotePairs = 8;
constexpr float kRemoteFullQualitySeconds = 1.5f;
constexpr float kElasticBlendAttackSpeed = 5.0f;
constexpr float kElasticBlendReleaseSpeed = 10.0f;
constexpr float kControlDirtyPublishIntervalSeconds = 1.0f / 30.0f;
constexpr float kControlHeartbeatIntervalSeconds = 1.0f / 5.0f;
constexpr const char* kClientEnvironmentProbeName = "NewPipelineEnvironmentProbe";
constexpr uint32_t kMobileShadow2DResolution = 1024;
constexpr uint32_t kMobileShadowCubeResolution = 512;
constexpr uint32_t kMobileShadow2DMinResolution = 96;
constexpr uint32_t kMobileShadowCubeMinResolution = 64;
constexpr uint32_t kMobileDirectionalCascadeCount = 4;
constexpr float kMobileDirectionalCascadeLambda = 0.95f;
constexpr uint32_t kMobileLightmapResolution = 256;
constexpr uint32_t kClientReflectionProbeResolution = 128;
constexpr const char* kLightmapBakeModeMetadataKey = "newpipeline.lightmap_bake_mode";
constexpr size_t kMaxLightmapsInFlight = 8;
constexpr uint32_t kMaxClientLightmapIterationsPerFrame = 32;
constexpr uint64_t kLightmapTransientBytesPerTexel = 57;
constexpr uint64_t kLightmapMinimumVRAMReserve = 512ull * 1024ull * 1024ull;
constexpr float kLightmapFrameTimeEMAWeight = 0.2f;
constexpr float kLightmapFrameTimeRampUp = 0.075f;
constexpr float kLightmapFrameTimeRampDown = 0.110f;
constexpr float kLightmapFrameTimeEmergency = 0.200f;
constexpr uint32_t kLightmapAdaptationIntervalFrames = 4;
constexpr uint64_t kLightmapNoProgressTimeoutUsec = 30'000'000;
constexpr uint32_t kClientVLMGridLongestAxis = 24;
constexpr uint32_t kClientVLMGridMinimumAxis = 4;
constexpr uint32_t kClientVLMMaxProbeCount = 4096;
constexpr uint32_t kClientVLMConvergenceFrames = 64;
constexpr uint32_t kClientVLMConvergenceMinRays = 128;
constexpr uint32_t kClientVLMFinalProjectionRays = DDGI_MAX_RAYCOUNT;
constexpr uint32_t kClientVLMFloat4sPerInstance = 7;

uint64_t EstimateLightmapTransientBytes(const XMUINT2& dimensions)
{
    const uint64_t texels = uint64_t(dimensions.x) * uint64_t(dimensions.y);
    // R32G32B32A32 accumulation, independent batch and adaptive statistics
    // (3 * 16 B) + R16G16B16A16 resolved result (8 B) + strict R8 coverage
    // (1 B), plus a 25% allocator/deferred-destruction safety margin.
    return texels * kLightmapTransientBytesPerTexel * 5ull / 4ull;
}

enum class LightmapBakeEligibility : uint8_t
{
    Eligible,
    ForcedEligible,
    AuthorExcluded,
    NonRenderableOrMissingMesh,
    Dynamic,
    SkinnedSoftBodyOrParticle,
    Transparent,
};

struct LightmapBakeCoverage
{
    uint32_t total = 0;
    uint32_t eligible = 0;
    uint32_t forced_eligible = 0;
    uint32_t author_excluded = 0;
    uint32_t unsupported = 0;
    uint32_t dynamic = 0;
    uint32_t deformed = 0;
    uint32_t transparent = 0;
    uint32_t atlas_failures = 0;
};

std::string GetLightmapBakeMode(const wi::scene::Scene& scene, wi::ecs::Entity entity)
{
    const wi::scene::MetadataComponent* metadata = scene.metadatas.GetComponent(entity);
    if (metadata == nullptr)
        return "auto";

    std::string mode = metadata->string_values.get(kLightmapBakeModeMetadataKey);
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return mode.empty() ? "auto" : mode;
}

LightmapBakeEligibility ClassifyLightmapBakeEligibility(
    const wi::scene::Scene& scene,
    wi::ecs::Entity entity,
    const wi::scene::ObjectComponent& object)
{
    const std::string mode = GetLightmapBakeMode(scene, entity);
    if (mode == "exclude")
        return LightmapBakeEligibility::AuthorExcluded;

    const wi::scene::MeshComponent* mesh = scene.meshes.GetComponent(object.meshID);
    if (!object.IsRenderable() || mesh == nullptr || !mesh->IsRenderable())
        return LightmapBakeEligibility::NonRenderableOrMissingMesh;

    // An explicit include is the only way to opt dynamic/deforming/transparent
    // content into a static bake. Missing or non-renderable geometry remains a
    // hard safety failure because it cannot produce a valid atlas.
    if (mode == "include")
        return LightmapBakeEligibility::ForcedEligible;

    if (object.IsDynamic() || mesh->IsDynamic())
        return LightmapBakeEligibility::Dynamic;
    if (mesh->IsSkinned() || scene.softbodies.Contains(object.meshID) || scene.emitters.Contains(entity))
        return LightmapBakeEligibility::SkinnedSoftBodyOrParticle;
    if ((object.GetFilterMask() & wi::enums::FILTER_TRANSPARENT) != 0)
        return LightmapBakeEligibility::Transparent;
    for (const wi::scene::MeshComponent::MeshSubset& subset : mesh->subsets)
    {
        const wi::scene::MaterialComponent* material = scene.materials.GetComponent(subset.materialID);
        if (material != nullptr && (material->GetFilterMask() & wi::enums::FILTER_TRANSPARENT) != 0)
            return LightmapBakeEligibility::Transparent;
    }
    return LightmapBakeEligibility::Eligible;
}

void AccumulateCoverage(LightmapBakeCoverage& coverage, LightmapBakeEligibility eligibility)
{
    switch (eligibility)
    {
    case LightmapBakeEligibility::Eligible: ++coverage.eligible; break;
    case LightmapBakeEligibility::ForcedEligible: ++coverage.forced_eligible; break;
    case LightmapBakeEligibility::AuthorExcluded: ++coverage.author_excluded; break;
    case LightmapBakeEligibility::NonRenderableOrMissingMesh: ++coverage.unsupported; break;
    case LightmapBakeEligibility::Dynamic: ++coverage.dynamic; break;
    case LightmapBakeEligibility::SkinnedSoftBodyOrParticle: ++coverage.deformed; break;
    case LightmapBakeEligibility::Transparent: ++coverage.transparent; break;
    }
}

std::string FormatLightmapBakeCoverage(const LightmapBakeCoverage& coverage, size_t queued)
{
    return "total=" + std::to_string(coverage.total) +
        " queued=" + std::to_string(queued) +
        " eligible_auto=" + std::to_string(coverage.eligible) +
        " eligible_forced=" + std::to_string(coverage.forced_eligible) +
        " skipped_author=" + std::to_string(coverage.author_excluded) +
        " skipped_dynamic=" + std::to_string(coverage.dynamic) +
        " skipped_skinned_softbody_particle=" + std::to_string(coverage.deformed) +
        " skipped_transparent=" + std::to_string(coverage.transparent) +
        " skipped_unsupported=" + std::to_string(coverage.unsupported) +
        " atlas_failures=" + std::to_string(coverage.atlas_failures);
}

uint64_t NowUsec()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count());
}

bool IsDown(wi::input::BUTTON button)
{
    return wi::input::Down(button);
}

bool IsCharacterDown(char c)
{
    return wi::input::Down((wi::input::BUTTON)c);
}

XMFLOAT3 NormalizeDirectionOrDefault(const XMFLOAT3& value)
{
    const XMVECTOR input = XMLoadFloat3(&value);
    if (XMVectorGetX(XMVector3LengthSq(input)) <= 0.000001f)
        return XMFLOAT3(0.0f, 1.0f, 0.0f);

    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3Normalize(input));
    return result;
}

bool NearlyEqual(float a, float b, float epsilon = 0.0001f)
{
    return std::abs(a - b) <= epsilon;
}

bool NearlyEqual(const XMFLOAT3& a, const XMFLOAT3& b, float epsilon = 0.0001f)
{
    return NearlyEqual(a.x, b.x, epsilon) &&
        NearlyEqual(a.y, b.y, epsilon) &&
        NearlyEqual(a.z, b.z, epsilon);
}

bool NearlyEqual(
    const XMFLOAT4X4& a,
    const XMFLOAT4X4& b,
    float epsilon = 0.0001f)
{
    const float* left = &a._11;
    const float* right = &b._11;
    for (size_t index = 0; index < 16; ++index)
    {
        if (!NearlyEqual(left[index], right[index], epsilon))
            return false;
    }
    return true;
}

bool SunMatches(const NewPipelineSunState& a, const NewPipelineSunState& b)
{
    return a.enabled == b.enabled && NearlyEqual(a.direction, b.direction) &&
        NearlyEqual(a.color, b.color) && NearlyEqual(a.intensity, b.intensity);
}

std::string EnabledString(bool value)
{
    return value ? "enabled" : "disabled";
}

float SmoothWeight(float current, float target, float dt)
{
    const float speed = target > current ? kElasticBlendAttackSpeed : kElasticBlendReleaseSpeed;
    const float blend = 1.0f - std::exp(-std::max(0.0f, dt) * speed);
    return std::lerp(current, target, blend);
}

uint64_t ComputeControlLightingFingerprint(
    const ClientControlPacket& packet)
{
    uint64_t hash = 14695981039346656037ull;
    const auto append_byte = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    const auto append_u32 = [&append_byte](uint32_t value) {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            append_byte(static_cast<uint8_t>(value >> shift));
    };
    const auto append_float = [&append_u32](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        append_u32(bits);
    };
    constexpr char domain[] = "newpipeline.control-lighting.v1";
    for (const char character : domain)
        append_byte(static_cast<uint8_t>(character));
    append_u32(packet.sun_enabled ? 1u : 0u);
    append_float(packet.sun_direction.x);
    append_float(packet.sun_direction.y);
    append_float(packet.sun_direction.z);
    append_float(packet.sun_color.x);
    append_float(packet.sun_color.y);
    append_float(packet.sun_color.z);
    append_float(packet.sun_intensity);
    append_float(packet.ambient.x);
    append_float(packet.ambient.y);
    append_float(packet.ambient.z);
    append_float(packet.horizon.x);
    append_float(packet.horizon.y);
    append_float(packet.horizon.z);
    append_float(packet.zenith.x);
    append_float(packet.zenith.y);
    append_float(packet.zenith.z);
    return hash == 0 ? 1 : hash;
}
} // namespace

void NewPipelineClientRenderPath::SetRuntimeConfig(const RuntimeConfig& value)
{
    config        = value;
    debug_preview_mode = DebugPreviewMode::Final;
    status_logged = false;
    remote_acquire_logged = false;
}

void NewPipelineClientRenderPath::SetSunState(const NewPipelineSunState& value)
{
    if (IsStaticLightingBakeActive())
    {
        wi::backlog::post("Client sun change ignored while static lighting bake is active");
        return;
    }
    sun_state = value;
    sun_state.direction = NormalizeDirectionOrDefault(sun_state.direction);
    if (scene_initialized)
    {
        ApplySunStateToScene(local_scene, sun_state);
        if (baked_sun_reference_valid && !SunMatches(sun_state, baked_sun_state))
        {
            client_static_lighting.MarkStale("runtime sun differs from baked sun");
            lightmap_bake_status = client_static_lighting.GetLightmapStatus();
            reflection_probe_status = client_static_lighting.GetProbeStatus();
            client_static_lighting.DisableLightmaps(local_scene);
            if (wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity))
            {
                InitializeBlackEnvironmentProbe(*probe);
                CreateEnvironmentProbeMipViews();
            }
        }
        else if (baked_sun_reference_valid && client_static_lighting.IsStale())
        {
            client_static_lighting.ClearStale();
            LoadStaticLightingAssets();
        }
    }
}

void NewPipelineClientRenderPath::SetDebugPreviewMode(DebugPreviewMode mode)
{
    debug_preview_mode = mode;
    EnsureSpecularIndirectDebugTexture();
    setDDGIOutputDebugPreview(wi::RenderPath3D::DDGIOutputDebugPreview::Disabled);
    debug_preview_invalid_logged = false;
    wi::backlog::post(std::string{"Client debug preview mode: "} + ToString(debug_preview_mode));
}

std::string NewPipelineClientRenderPath::GetEffectiveAlgorithmSummary() const
{
    return std::string{render_settings.shadow_maps_enabled ? "Shadow Map 1024/512" : "Shadow Map off"} +
        " | " + (render_settings.ssao_enabled ? "SSAO" : "SSAO off") +
        " | " + (render_settings.baked_lightmaps_enabled ? "Baked Lightmap" : "Baked Lightmap off") +
        " | " + (render_settings.dynamic_object_vlm_enabled ? "Dynamic VLM" : "Dynamic VLM off") +
        " | " + (render_settings.environment_probe_enabled ? "Baked Probe 128" : "Baked Probe off") +
        " | Elastic DDGI/RTAO" +
        " | local DDGI/RT/SSR off";
}

std::string NewPipelineClientRenderPath::GetDebugStatusSummary() const
{
    const WebRTCTransportStats transport = webrtc_transport.GetStats();
    const std::string transport_status =
        "\nWebRTC: " + std::string{ToString(transport.state)} +
            "\nDecoder: codec=" + transport.codec_name +
            " profile=" + transport.codec_profile +
            " impl=" + transport.codec_implementation +
            " power-efficient=" + (transport.power_efficient_codec ? "yes" : "no") +
            " surface=" + transport.input_surface +
            " retained=" + std::to_string(transport.retained_frame_acquires) +
            " decode-q=" + std::to_string(transport.decoded_queue_depth) +
            " decode-avg=" + std::to_string(
                transport.frames_decoded > 0
                    ? transport.total_decode_time_usec /
                        transport.frames_decoded / 1000u
                    : 0u) + " ms" +
            " net=" + std::to_string(transport.compressed_bytes_received / 1024u) + " KiB" +
            " bitrate=" + std::to_string(transport_bitrate_bps / 1000u) + " kbps" +
            " I420=" + std::to_string(transport.retained_i420_bytes / 1024u) + " KiB" +
            " cpu-copy=" + std::to_string(transport.cpu_full_frame_copy_bytes / 1024u) + " KiB" +
            " convert=" + std::to_string(transport.cpu_conversion_usec / 1000u) + " ms" +
            " upload=" + std::to_string(remote_gpu_upload_bytes / 1024u) + " KiB" +
            " tex-create=" + std::to_string(remote_texture_creation_count) +
            " meta=" + std::to_string(downstream_metadata_matches) + "/" +
                std::to_string(downstream_metadata_misses) + "/" +
                std::to_string(downstream_metadata_mismatches) +
            " pair[m/v/x]=" + std::to_string(downstream_metadata_first_matches) + "/" +
                std::to_string(downstream_video_first_matches) + "/" +
                std::to_string(downstream_pair_expirations) +
            " drop[old/status]=" +
                std::to_string(downstream_out_of_order_drops) + "/" +
                std::to_string(downstream_stale_status_drops) +
            " pending=" + std::to_string(downstream_metadata_cache.size()) + "/" +
                std::to_string(pending_remote_video_frames.size());
    return GetEffectiveAlgorithmSummary() + "\n" + client_static_lighting.GetStatusSummary() + transport_status +
        "\nPrimary light: stable-id=" + std::to_string(GetNewPipelineSunStableId(local_scene)) +
        " shadow-index=" +
            (GetNewPipelineSunShadowIndex(local_scene) < 16
                ? std::to_string(GetNewPipelineSunShadowIndex(local_scene)) : std::string{"unavailable"}) +
        "\nRemote decoded: " +
        (remote_consume.accepted_valid ? std::string{"available"} :
            std::string{"unavailable ("} + remote_consume.invalid_reason + ")") +
        "\nRemote DDGI: frame " + std::to_string(remote_ddgi_frame_index) +
        (remote_consume.history_valid ? " converged" : " warming") +
        " reset=" + ToString(remote_ddgi_reset_reason) +
        "\nNegotiation: " +
        (negotiated_stream_selection_valid
            ? "selected protocol=" +
                std::to_string(
                    negotiated_stream_selection.protocol_version) +
                " quality=" +
                ToString(negotiated_stream_selection.quality_tier)
            : "pending/rejected") +
        "\nRemote GBuffer history: " +
        std::to_string(remote_gbuffer_history_active_capacity) + "/" +
        std::to_string(kRemoteGBufferHistoryCapacity) +
        " slots (384 MiB budget)" +
        "\n" + GetElasticLightingStatus();
}

std::string NewPipelineClientRenderPath::GetElasticLightingStatus() const
{
    const int quality_percent = static_cast<int>(std::round(std::clamp(elastic_remote_quality, 0.0f, 1.0f) * 100.0f));
    const int gi_percent = static_cast<int>(std::round(std::clamp(elastic_remote_gi_weight, 0.0f, 1.0f) * 100.0f));
    const int ao_percent = static_cast<int>(std::round(std::clamp(elastic_remote_ao_weight, 0.0f, 1.0f) * 100.0f));
    const int specular_percent = static_cast<int>(std::round(
        std::clamp(elastic_remote_specular_weight, 0.0f, 1.0f) * 100.0f));
    const int shadow_percent = static_cast<int>(std::round(
        std::clamp(elastic_remote_shadow_weight, 0.0f, 1.0f) * 100.0f));
    std::string semantic_status;
    if (accepted_remote_contract_v3_valid &&
        remote_consume.accepted_valid)
    {
        semantic_status = "\nSemantic age/confidence:";
        for (const RemoteBufferDescriptorV3& descriptor :
            accepted_remote_contract_v3.descriptors)
        {
            if ((descriptor.flags &
                    kRemoteBufferDescriptorAvailableV3) == 0)
                continue;
            const uint64_t age =
                remote_consume.accepted_frame_id >=
                    descriptor.content_frame_id
                ? remote_consume.accepted_frame_id -
                    descriptor.content_frame_id
                : 0;
            semantic_status += " " +
                std::string{ToString(descriptor.semantic)} +
                "=" + std::to_string(age) + "/" +
                std::to_string(descriptor.confidence_unorm);
        }
    }
    return "Elastic quality " + std::to_string(quality_percent) + "% | GI remote " +
        std::to_string(gi_percent) + "% | AO remote " + std::to_string(ao_percent) +
        "% | Spec remote " + std::to_string(specular_percent) +
        "% | Shadow remote " + std::to_string(shadow_percent) +
        "%\nAlignment: control-frame GBuffer depth/normal/roughness reprojection" +
        semantic_status;
}

void NewPipelineClientRenderPath::SetInputActive(bool active)
{
    if (input_active == active)
        return;
    input_active = active;
    camera_control_start = true;
    wi::input::HidePointer(false);
}

void NewPipelineClientRenderPath::SetRenderSettings(const NewPipelineClientRenderSettings& settings)
{
    const NewPipelineClientRenderSettings previous = render_settings;
    render_settings = settings;
    const bool shadows_changed = previous.shadow_maps_enabled != render_settings.shadow_maps_enabled;
    const bool ssao_changed = previous.ssao_enabled != render_settings.ssao_enabled;
    const bool probe_changed = previous.environment_probe_enabled != render_settings.environment_probe_enabled;
    const bool dynamic_vlm_changed =
        previous.dynamic_object_vlm_enabled !=
        render_settings.dynamic_object_vlm_enabled;
    render_settings.remote_gi_max_weight = std::clamp(render_settings.remote_gi_max_weight, 0.0f, 1.0f);
    render_settings.remote_ao_max_weight = std::clamp(render_settings.remote_ao_max_weight, 0.0f, 1.0f);
    render_settings.remote_specular_max_weight =
        std::clamp(render_settings.remote_specular_max_weight, 0.0f, 1.0f);
    render_settings.remote_shadow_max_weight =
        std::clamp(render_settings.remote_shadow_max_weight, 0.0f, 1.0f);

    if (scene_initialized)
    {
        ApplyBakedLightmapSettings(previous.baked_lightmaps_enabled, true);
        if (!previous.lightmap_bake_requested && render_settings.lightmap_bake_requested)
            RequestLightmapBake();
        else if (previous.lightmap_bake_requested && !render_settings.lightmap_bake_requested)
            CancelLightmapBake();
    }

    ApplyShadowSettings(shadows_changed);
    ApplySSAOSettings(ssao_changed);
    if (scene_initialized)
        ApplyEnvironmentProbeSettings(probe_changed);
    if (dynamic_vlm_changed)
    {
        wi::backlog::post(
            "Client dynamic-object volumetric lightmap: " +
            EnabledString(render_settings.dynamic_object_vlm_enabled));
    }
    ApplyElasticLightingResources();
}

void NewPipelineClientRenderPath::Start()
{
    InitializeSceneIfNeeded();
    ConfigureLowEndLocalRendering();
    // Formal local outputs and elastic Final fusion are implemented in
    // Visibility_Shade. Make that renderer-supported path explicit instead of
    // allocating outputs that are never dispatched.
    setVisibilityComputeShadingEnabled(true);
    setVisibilitySurfaceResourcesForced(true);
    ApplyRenderSettings(true);
    wi::RenderPath3D::Start();
    std::string error;
    if (!webrtc_transport.RequestStart(false, config, &error))
        wi::backlog::post("Client WebRTC start failed: " + error);

    wi::backlog::post("NewPipeline_Wicked Client render path started.");
    wi::backlog::post("Client remote source: WebRTC V3");
    wi::backlog::post(std::string{"Client debug preview mode: "} + ToString(debug_preview_mode));
    status_logged = true;
}

void NewPipelineClientRenderPath::ResizeBuffers()
{
    wi::RenderPath3D::ResizeBuffers();
    local_ao_snapshot = {};
    local_indirect_final_input = {};
    local_specular_indirect = {};
    local_specular_indirect_pre_ao = {};
    local_primary_light_visibility = {};
    elastic_indirect_diffuse = {};
    elastic_ao = {};
    elastic_specular_indirect_pre_ao = {};
    elastic_primary_light_visibility = {};
    visibilityResources.texture_local_indirect_diffuse = nullptr;
    visibilityResources.buffer_client_vlm_instances = nullptr;
    visibilityResources.buffer_client_vlm_instances_upload = nullptr;
    visibilityResources.texture_specular_indirect = nullptr;
    visibilityResources.texture_specular_indirect_pre_ao = nullptr;
    visibilityResources.texture_primary_light_visibility = nullptr;
    visibilityResources.primary_light_shadow_index = -1;
    visibilityResources.texture_elastic_indirect_diffuse = nullptr;
    visibilityResources.texture_elastic_ao = nullptr;
    visibilityResources.texture_elastic_specular_indirect_pre_ao = nullptr;
    visibilityResources.texture_elastic_primary_light_visibility = nullptr;
    visibilityResources.texture_remote_history_depth = nullptr;
    visibilityResources.texture_remote_history_normal_roughness = nullptr;
    if (rtAO.IsValid())
    {
        wi::graphics::TextureDesc desc = rtAO.GetDesc();
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_ao_snapshot);
        wi::graphics::GetDevice()->SetName(&local_ao_snapshot, "newpipeline.client.local_ao_snapshot");
    }
    const XMUINT2 internal_resolution = GetInternalResolution();
    if (internal_resolution.x > 0 && internal_resolution.y > 0)
    {
        wi::graphics::TextureDesc desc;
        desc.width = internal_resolution.x;
        desc.height = internal_resolution.y;
        desc.format = wi::graphics::Format::R16G16B16A16_FLOAT;
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        if (wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_indirect_final_input))
        {
            wi::graphics::GetDevice()->SetName(&local_indirect_final_input, "newpipeline.client.local_indirect_final_input");
            visibilityResources.texture_local_indirect_diffuse = &local_indirect_final_input;
        }
        else
        {
            wi::backlog::post("Client Local Indirect Final Input texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" + std::to_string(internal_resolution.y));
        }

        if (wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_specular_indirect_pre_ao))
        {
            wi::graphics::GetDevice()->SetName(
                &local_specular_indirect_pre_ao, "newpipeline.client.local_specular_indirect_pre_ao");
            visibilityResources.texture_specular_indirect_pre_ao = &local_specular_indirect_pre_ao;
        }
        else
        {
            wi::backlog::post("Client Local Specular Indirect Pre-AO texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" + std::to_string(internal_resolution.y));
        }

        wi::graphics::TextureDesc primary_visibility_desc = desc;
        primary_visibility_desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
        if (wi::graphics::GetDevice()->CreateTexture(
            &primary_visibility_desc, nullptr, &local_primary_light_visibility))
        {
            wi::graphics::GetDevice()->SetName(
                &local_primary_light_visibility, "newpipeline.client.local_primary_light_visibility");
            visibilityResources.texture_primary_light_visibility = &local_primary_light_visibility;
        }
        else
        {
            wi::backlog::post("Client Local Primary Light Visibility texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" + std::to_string(internal_resolution.y));
        }

        wi::graphics::TextureDesc elastic_gi_desc = desc;
        if (wi::graphics::GetDevice()->CreateTexture(&elastic_gi_desc, nullptr, &elastic_indirect_diffuse))
        {
            wi::graphics::GetDevice()->SetName(&elastic_indirect_diffuse, "newpipeline.client.elastic_indirect_diffuse");
            visibilityResources.texture_elastic_indirect_diffuse = &elastic_indirect_diffuse;
        }
        else
        {
            wi::backlog::post("Client Elastic Indirect Diffuse texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" + std::to_string(internal_resolution.y));
        }

        wi::graphics::TextureDesc elastic_ao_desc = desc;
        elastic_ao_desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
        if (wi::graphics::GetDevice()->CreateTexture(&elastic_ao_desc, nullptr, &elastic_ao))
        {
            wi::graphics::GetDevice()->SetName(&elastic_ao, "newpipeline.client.elastic_ao");
            visibilityResources.texture_elastic_ao = &elastic_ao;
        }
        else
        {
            wi::backlog::post("Client Elastic AO texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" + std::to_string(internal_resolution.y));
        }

        if (wi::graphics::GetDevice()->CreateTexture(
            &desc, nullptr, &elastic_specular_indirect_pre_ao))
        {
            wi::graphics::GetDevice()->SetName(
                &elastic_specular_indirect_pre_ao,
                "newpipeline.client.elastic_specular_indirect_pre_ao");
            visibilityResources.texture_elastic_specular_indirect_pre_ao =
                &elastic_specular_indirect_pre_ao;
        }
        else
        {
            wi::backlog::post("Client Elastic Specular Pre-AO texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" +
                std::to_string(internal_resolution.y));
        }

        if (wi::graphics::GetDevice()->CreateTexture(
            &primary_visibility_desc, nullptr, &elastic_primary_light_visibility))
        {
            wi::graphics::GetDevice()->SetName(
                &elastic_primary_light_visibility,
                "newpipeline.client.elastic_primary_light_visibility");
            visibilityResources.texture_elastic_primary_light_visibility =
                &elastic_primary_light_visibility;
        }
        else
        {
            wi::backlog::post("Client Elastic Primary Visibility texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" +
                std::to_string(internal_resolution.y));
        }
    }
    ResetRemoteGBufferHistory();
    ClearPendingRemoteFrames();
    accepted_remote_contract_v3_valid = false;
    accepted_remote_source_control_frame_id = 0;
    accepted_remote_buffer_mask = 0;
    InvalidateRemote("render buffers resized");
    EnsureSpecularIndirectDebugTexture();
    ApplyElasticLightingResources();
}

void NewPipelineClientRenderPath::ResetRemoteGBufferHistory()
{
    remote_gbuffer_history_active_capacity = 0;
    remote_gbuffer_history_write_index = 0;
    remote_gbuffer_history_last_capture = 0;
    for (RemoteGBufferHistoryEntry& entry : remote_gbuffer_history)
    {
        entry.control_frame_id = 0;
        entry.lighting_fingerprint = 0;
        entry.scene_generation = 0;
        entry.view_origin = {};
        entry.valid = false;
    }

    if (!depthBuffer_Copy.IsValid() ||
        !visibilityResources.texture_normal_roughness.IsValid())
    {
        remote_gbuffer_history_rebuild_pending = true;
        return;
    }

    wi::graphics::TextureDesc depth_desc = depthBuffer_Copy.GetDesc();
    // Reprojection samples only the base level. Do not clone Wicked's depth
    // pyramid into every history entry; at high resolutions that would add a
    // large, unused 5-mip allocation per control frame.
    depth_desc.mip_levels = 1;
    depth_desc.bind_flags =
        wi::graphics::BindFlag::SHADER_RESOURCE |
        wi::graphics::BindFlag::UNORDERED_ACCESS;
    depth_desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
    wi::graphics::TextureDesc normal_desc =
        visibilityResources.texture_normal_roughness.GetDesc();
    normal_desc.bind_flags =
        wi::graphics::BindFlag::SHADER_RESOURCE |
        wi::graphics::BindFlag::UNORDERED_ACCESS;
    normal_desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;

    constexpr uint64_t history_budget_bytes = 384ull * 1024ull * 1024ull;
    const uint64_t texel_count =
        static_cast<uint64_t>(depth_desc.width) * depth_desc.height;
    const uint64_t bytes_per_entry = texel_count *
        (wi::graphics::GetFormatStride(depth_desc.format) +
            wi::graphics::GetFormatStride(normal_desc.format));
    if (bytes_per_entry == 0 ||
        bytes_per_entry > history_budget_bytes)
        return;
    remote_gbuffer_history_active_capacity = static_cast<size_t>(
        std::clamp<uint64_t>(
            history_budget_bytes / bytes_per_entry,
            1u,
            remote_gbuffer_history.size()));

    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    for (size_t index = 0; index < remote_gbuffer_history.size(); ++index)
    {
        RemoteGBufferHistoryEntry& entry = remote_gbuffer_history[index];
        if (index >= remote_gbuffer_history_active_capacity)
        {
            entry.depth = {};
            entry.normal_roughness = {};
            continue;
        }
        const bool depth_matches =
            entry.depth.IsValid() &&
            entry.depth.GetDesc().width == depth_desc.width &&
            entry.depth.GetDesc().height == depth_desc.height &&
            entry.depth.GetDesc().format == depth_desc.format;
        if (!depth_matches)
        {
            entry.depth = {};
            if (device->CreateTexture(&depth_desc, nullptr, &entry.depth))
            {
                device->SetName(&entry.depth,
                    ("newpipeline.client.remote_history_depth[" +
                        std::to_string(index) + "]").c_str());
            }
        }
        const bool normal_matches =
            entry.normal_roughness.IsValid() &&
            entry.normal_roughness.GetDesc().width == normal_desc.width &&
            entry.normal_roughness.GetDesc().height == normal_desc.height &&
            entry.normal_roughness.GetDesc().format == normal_desc.format;
        if (!normal_matches)
        {
            entry.normal_roughness = {};
            if (device->CreateTexture(
                &normal_desc, nullptr, &entry.normal_roughness))
            {
                device->SetName(&entry.normal_roughness,
                    ("newpipeline.client.remote_history_normal_roughness[" +
                        std::to_string(index) + "]").c_str());
            }
        }
        if (!entry.depth.IsValid() ||
            !entry.normal_roughness.IsValid())
        {
            // Keep the usable ring contiguous so the capture writer can
            // never become stuck on a failed allocation.
            entry.depth = {};
            entry.normal_roughness = {};
            remote_gbuffer_history_active_capacity = index;
        }
    }
    remote_gbuffer_history_rebuild_pending = false;
}

void NewPipelineClientRenderPath::AdvanceSceneGeneration(
    const char* reason)
{
    ++scene_generation;
    if (scene_generation == 0)
        scene_generation = 1;
    ResetRemoteGBufferHistory();
    ClearPendingRemoteFrames();
    accepted_remote_contract_v3_valid = false;
    accepted_remote_source_control_frame_id = 0;
    InvalidateRemote(
        std::string{"scene generation changed: "} +
        (reason != nullptr ? reason : "unspecified"));
    wi::backlog::post(
        "Client scene generation advanced to " +
        std::to_string(scene_generation));
}

void NewPipelineClientRenderPath::EnsureSpecularIndirectDebugTexture()
{
    visibilityResources.texture_specular_indirect = nullptr;
    if (debug_preview_mode != DebugPreviewMode::LocalSpecularIndirect)
    {
        local_specular_indirect = {};
        return;
    }

    const XMUINT2 resolution = GetInternalResolution();
    if (resolution.x == 0 || resolution.y == 0)
        return;
    if (!local_specular_indirect.IsValid() ||
        local_specular_indirect.GetDesc().width != resolution.x ||
        local_specular_indirect.GetDesc().height != resolution.y)
    {
        wi::graphics::TextureDesc desc;
        desc.width = resolution.x;
        desc.height = resolution.y;
        desc.format = wi::graphics::Format::R16G16B16A16_FLOAT;
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        local_specular_indirect = {};
        if (!wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_specular_indirect))
        {
            wi::backlog::post("Client Local Specular Indirect debug texture creation failed: " +
                std::to_string(resolution.x) + "x" + std::to_string(resolution.y));
            return;
        }
        wi::graphics::GetDevice()->SetName(&local_specular_indirect, "newpipeline.client.local_specular_indirect");
    }
    visibilityResources.texture_specular_indirect = &local_specular_indirect;
}

void NewPipelineClientRenderPath::RenderAO(wi::graphics::CommandList cmd) const
{
    wi::RenderPath3D::RenderAO(cmd);
    if (rtAO.IsValid() && local_ao_snapshot.IsValid())
        wi::renderer::CopyTexture2D(local_ao_snapshot, rtAO, cmd);
    CaptureRemoteGBufferHistory(cmd);
}

void NewPipelineClientRenderPath::PreRender()
{
    // RenderPath3D creates the visibility normal/roughness surface in
    // PreRender(), after ResizeBuffers() has completed. Build the control-frame
    // GBuffer history only after that source surface exists.
    wi::RenderPath3D::PreRender();
    if (remote_gbuffer_history_rebuild_pending)
        ResetRemoteGBufferHistory();
}

void NewPipelineClientRenderPath::Render() const
{
    wi::RenderPath3D::Render();
    ScheduleVolumetricLightmapReadback();
}

void NewPipelineClientRenderPath::ScheduleVolumetricLightmapReadback() const
{
    if (!volumetric_readback_requested ||
        volumetric_readback_scheduled)
        return;

    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    if (!local_scene.ddgi.ray_buffer.IsValid() ||
        !local_scene.ddgi.raycount_buffer.IsValid())
    {
        volumetric_readback_requested = false;
        volumetric_readback_scheduled = true;
        volumetric_readback_error =
            "volumetric lightmap ray resources became unavailable";
        return;
    }
    wi::graphics::GPUBufferDesc ray_desc;
    ray_desc.size = local_scene.ddgi.ray_buffer.GetDesc().size;
    ray_desc.usage = wi::graphics::Usage::READBACK;
    wi::graphics::GPUBufferDesc ray_count_desc;
    ray_count_desc.size =
        local_scene.ddgi.raycount_buffer.GetDesc().size;
    ray_count_desc.usage = wi::graphics::Usage::READBACK;

    volumetric_ray_readback = {};
    volumetric_ray_count_readback = {};
    if (!device->CreateBuffer(
            &ray_desc, nullptr, &volumetric_ray_readback) ||
        !device->CreateBuffer(
            &ray_count_desc, nullptr, &volumetric_ray_count_readback))
    {
        volumetric_readback_requested = false;
        volumetric_readback_scheduled = true;
        volumetric_readback_error =
            "volumetric lightmap readback allocation failed";
        return;
    }
    device->SetName(
        &volumetric_ray_readback,
        "newpipeline.client.vlm_ray_readback");
    device->SetName(
        &volumetric_ray_count_readback,
        "newpipeline.client.vlm_ray_count_readback");

    // This is recorded after RenderPath3D's DDGI work on the same compute
    // queue. The host remains the sole command-list submitter, so Wicked's
    // frame and buffered-resource indices cannot be advanced from RenderPath.
    const wi::graphics::CommandList cmd =
        device->BeginCommandList(wi::graphics::QUEUE_COMPUTE);
    const wi::graphics::GPUBarrier begin_barriers[] = {
        wi::graphics::GPUBarrier::Buffer(
            &local_scene.ddgi.ray_buffer,
            wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE,
            wi::graphics::ResourceState::COPY_SRC),
        wi::graphics::GPUBarrier::Buffer(
            &local_scene.ddgi.raycount_buffer,
            wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE,
            wi::graphics::ResourceState::COPY_SRC),
    };
    device->Barrier(
        begin_barriers,
        static_cast<uint32_t>(std::size(begin_barriers)),
        cmd);
    device->CopyResource(
        &volumetric_ray_readback,
        &local_scene.ddgi.ray_buffer,
        cmd);
    device->CopyResource(
        &volumetric_ray_count_readback,
        &local_scene.ddgi.raycount_buffer,
        cmd);
    const wi::graphics::GPUBarrier end_barriers[] = {
        wi::graphics::GPUBarrier::Buffer(
            &local_scene.ddgi.ray_buffer,
            wi::graphics::ResourceState::COPY_SRC,
            wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE),
        wi::graphics::GPUBarrier::Buffer(
            &local_scene.ddgi.raycount_buffer,
            wi::graphics::ResourceState::COPY_SRC,
            wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE),
    };
    device->Barrier(
        end_barriers,
        static_cast<uint32_t>(std::size(end_barriers)),
        cmd);
    volumetric_readback_requested = false;
    volumetric_readback_scheduled = true;
    volumetric_readback_submit_frame = device->GetFrameCount();
    volumetric_readback_error.clear();
}

void NewPipelineClientRenderPath::CaptureRemoteGBufferHistory(
    wi::graphics::CommandList cmd) const
{
    if (!has_published_control_packet ||
        last_published_control_packet.control_frame_id == 0 ||
        last_published_control_packet.control_frame_id ==
            remote_gbuffer_history_last_capture ||
        !depthBuffer_Copy.IsValid() ||
        !visibilityResources.texture_normal_roughness.IsValid() ||
        remote_gbuffer_history_active_capacity == 0)
        return;

    RemoteGBufferHistoryEntry& entry =
        remote_gbuffer_history[remote_gbuffer_history_write_index];
    if (!entry.depth.IsValid() || !entry.normal_roughness.IsValid())
        return;
    wi::renderer::CopyTexture2D(entry.depth, depthBuffer_Copy, cmd);
    wi::renderer::CopyTexture2D(
        entry.normal_roughness,
        visibilityResources.texture_normal_roughness,
        cmd);
    entry.control_frame_id = last_published_control_packet.control_frame_id;
    entry.lighting_fingerprint =
        ComputeControlLightingFingerprint(
            last_published_control_packet);
    entry.scene_generation = last_published_control_packet.scene_generation;
    XMStoreFloat4x4(
        &entry.view_projection, local_camera.GetViewProjection());
    entry.view_origin = local_camera.Eye;
    entry.near_plane = local_camera.zNearP;
    entry.far_plane = local_camera.zFarP;
    entry.valid = true;
    remote_gbuffer_history_last_capture = entry.control_frame_id;
    remote_gbuffer_history_write_index =
        (remote_gbuffer_history_write_index + 1u) %
        remote_gbuffer_history_active_capacity;
}

void NewPipelineClientRenderPath::Update(float dt)
{
    InitializeSceneIfNeeded();
    UpdateReflectionProbeBake();
    UpdateLightmapBake(dt);
    UpdateLocalCamera(dt);
    ConfigureClientDirectionalShadowCascades();
    MaintainWebRTC(dt);
    PublishControlPacket(dt);

    wi::RenderPath3D::Update(dt);
    UpdateClientVolumetricLightmapInstances();
    PollRemoteFrameMetadata();
    AcquireRemoteVideoFrame(dt);
    UpdateElasticLighting(dt);

    if (!status_logged)
    {
        wi::backlog::post("Client remote source: WebRTC V3");
        status_logged = true;
    }
}

void NewPipelineClientRenderPath::MaintainWebRTC(float dt)
{
    webrtc_transport.Tick();
    const WebRTCTransportStats stats = webrtc_transport.GetStats();
    transport_telemetry_window_seconds += std::max(0.0f, dt);
    if (transport_telemetry_window_seconds >= 1.0f)
    {
        const uint64_t delta = stats.compressed_bytes_received >=
                transport_telemetry_previous_bytes
            ? stats.compressed_bytes_received -
                transport_telemetry_previous_bytes
            : 0;
        transport_bitrate_bps = static_cast<uint64_t>(
            static_cast<double>(delta) * 8.0 /
            transport_telemetry_window_seconds);
        transport_telemetry_previous_bytes =
            stats.compressed_bytes_received;
        transport_telemetry_window_seconds = 0.0f;
    }
    if (stats.state == previous_webrtc_state)
        return;
    wi::backlog::post("Client WebRTC " + std::string{ToString(previous_webrtc_state)} + " -> " +
        ToString(stats.state) + (stats.status.empty() ? std::string{} : ": " + stats.status));
    if (previous_webrtc_state == WebRTCTransportState::Connected &&
        stats.state != WebRTCTransportState::Connected)
    {
        ClearPendingRemoteFrames();
        InvalidateRemote("transport disconnected");
    }
    previous_webrtc_state = stats.state;
}

void NewPipelineClientRenderPath::PollRemoteFrameMetadata()
{
    for (size_t receive_count = 0; receive_count < kMaxPendingRemotePairs * 2u;
        ++receive_count)
    {
        RemoteVideoFrameLayout layout;
        RemoteStreamStatus stream_status;
        if (!webrtc_transport.TryReceiveFrameMetadata(layout, &stream_status))
        {
            if (stream_status.control_frame_id == 0)
                break;
            if (stream_status.control_frame_id <=
                negotiated_stream_control_frame_id)
            {
                ++downstream_stale_status_drops;
                continue;
            }
            negotiated_stream_control_frame_id = stream_status.control_frame_id;
            negotiated_stream_selection = stream_status.selection;
            negotiated_stream_selection_valid =
                stream_status.code == RemoteStreamStatusCode::Selected;
            if (!negotiated_stream_selection_valid)
            {
                const char* reason = "no common protocol";
                if (stream_status.code ==
                    RemoteStreamStatusCode::NoCommonEncodingProfile)
                    reason = "no common encoding profile";
                else if (stream_status.code ==
                    RemoteStreamStatusCode::NoCommonQualityTier)
                    reason = "no common quality tier";
                InvalidateRemote(std::string{"remote negotiation rejected: "} + reason);
            }
            continue;
        }
        for (auto iterator = downstream_metadata_cache.begin();
            iterator != downstream_metadata_cache.end();)
        {
            if (iterator->metadata.frame_id == layout.metadata.frame_id &&
                iterator->metadata.source_generation ==
                    layout.metadata.source_generation)
                iterator = downstream_metadata_cache.erase(iterator);
            else
                ++iterator;
        }
        downstream_metadata_cache.push_back(std::move(layout));
        while (downstream_metadata_cache.size() > kMaxPendingRemotePairs)
        {
            downstream_metadata_cache.pop_front();
            ++downstream_pair_expirations;
        }
    }
}

void NewPipelineClientRenderPath::ClearPendingRemoteFrames()
{
    downstream_metadata_cache.clear();
    pending_remote_video_frames.clear();
}

void NewPipelineClientRenderPath::PrunePendingRemoteFrames(uint64_t now_usec)
{
    while (!downstream_metadata_cache.empty())
    {
        const uint64_t received = downstream_metadata_cache.front().metadata.local_receive_timestamp_usec;
        if (received != 0 && received + kRemotePairTimeoutUsec >= now_usec)
            break;
        downstream_metadata_cache.pop_front();
        ++downstream_pair_expirations;
    }
    while (!pending_remote_video_frames.empty())
    {
        const uint64_t received = pending_remote_video_frames.front().local_receive_timestamp_usec;
        if (received + kRemotePairTimeoutUsec >= now_usec)
            break;
        pending_remote_video_frames.pop_front();
        ++downstream_pair_expirations;
    }
}

bool NewPipelineClientRenderPath::TryMatchRemoteVideoFrame(
    RetainedI420Frame& frame,
    RemoteVideoFrameLayout& layout)
{
    struct Match
    {
        size_t video_index = 0;
        size_t metadata_index = 0;
        uint64_t local_receive_timestamp_usec = 0;
        bool valid = false;
    } match;

    for (size_t video_index = 0; video_index < pending_remote_video_frames.size(); ++video_index)
    {
        const PendingRemoteVideoFrame& pending_video = pending_remote_video_frames[video_index];
        const RemoteFrameMetadata& pixel_metadata = pending_video.pixel_layout.metadata;
        for (size_t metadata_index = 0; metadata_index < downstream_metadata_cache.size(); ++metadata_index)
        {
            const RemoteFrameMetadata& channel_metadata =
                downstream_metadata_cache[metadata_index].metadata;
            // WebRTC transports timestamps through RTP's clock domain, so the
            // decoded frame timestamp is not an identity-preserving copy of
            // the producer's microsecond timestamp. The metadata band is part
            // of the encoded frame and therefore provides the stable key.
            if (channel_metadata.frame_id != pixel_metadata.frame_id ||
                channel_metadata.source_generation != pixel_metadata.source_generation)
                continue;
            if (!match.valid || pending_video.local_receive_timestamp_usec >
                    match.local_receive_timestamp_usec)
            {
                match.video_index = video_index;
                match.metadata_index = metadata_index;
                match.local_receive_timestamp_usec =
                    pending_video.local_receive_timestamp_usec;
                match.valid = true;
            }
        }
    }
    if (!match.valid)
        return false;

    const PendingRemoteVideoFrame& pending_video = pending_remote_video_frames[match.video_index];
    const RemoteVideoFrameLayout& pending_metadata = downstream_metadata_cache[match.metadata_index];
    if (pending_metadata.metadata.local_receive_timestamp_usec <= pending_video.local_receive_timestamp_usec)
        ++downstream_metadata_first_matches;
    else
        ++downstream_video_first_matches;

    frame = pending_video.frame;
    layout = pending_metadata;
    layout.metadata.local_receive_timestamp_usec = NowUsec();

    pending_remote_video_frames.erase(
        pending_remote_video_frames.begin(),
        std::next(pending_remote_video_frames.begin(), match.video_index + 1));
    downstream_metadata_cache.erase(
        downstream_metadata_cache.begin(),
        std::next(downstream_metadata_cache.begin(), match.metadata_index + 1));
    ++downstream_metadata_matches;
    return true;
}

void NewPipelineClientRenderPath::InitializeSceneIfNeeded()
{
    if (scene_initialized)
        return;

    scene = &local_scene;
    camera = &local_camera;

    const SceneInitializationResult result = InitializeDefaultScene(local_scene);
    sun_state = ExtractSunStateFromScene(local_scene);
    InitializeDefaultCamera(
        local_camera,
        (uint32_t)GetLogicalWidth(),
        (uint32_t)GetLogicalHeight(),
        result.kind,
        &local_scene);
    camera_position = local_camera.Eye;
    const XMFLOAT3 forward = NormalizeDirectionOrDefault(local_camera.At);
    camera_rotation.x = -std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    camera_rotation.y = std::atan2(forward.x, forward.z);
    camera_rotation.z = 0.0f;

    std::string scene_message = std::string{"Client scene initialized: "} + ToString(result.kind);
    if (!result.loaded_asset_path.empty())
        scene_message += " (" + result.loaded_asset_path + ")";
    if (result.object_count > 0 || result.mesh_count > 0 || result.material_count > 0)
    {
        scene_message += " objects=" + std::to_string(result.object_count) +
            " meshes=" + std::to_string(result.mesh_count) +
            " materials=" + std::to_string(result.material_count);
    }
    wi::backlog::post(scene_message);
    if (!result.diagnostic.empty())
        wi::backlog::post("Client scene diagnostic: " + result.diagnostic);
    wi::backlog::post("Client scene parity: " +
        FormatSceneParityFingerprint(ComputeSceneParityFingerprint(local_scene)));

    scene_asset_path = result.loaded_asset_path;
    scene_source_root_entity = result.loaded_root_entity;
    if (!scene_asset_path.empty())
    {
        const ClientLightmapPackageResult package_result = client_static_lighting.LoadLightmaps(scene_asset_path, local_scene);
        client_volumetric_lightmap =
            package_result.success
                ? package_result.volumetric_lightmap
                : ClientVolumetricLightmapData{};
        if (package_result.scene_replaced)
            scene_source_root_entity = wi::ecs::INVALID_ENTITY;
        wi::backlog::post(package_result.diagnostic);
        lightmap_bake_status = client_static_lighting.GetLightmapStatus();
    }
    else
    {
        ClientLightmapPackage::ClearSceneLightmaps(local_scene);
        wi::backlog::post("Client Lightmap package unavailable: procedural scene has no persistent source asset");
    }

    scene_initialized = true;
    ApplyEnvironmentProbeSettings(false);
    baked_sun_state = sun_state;
    baked_sun_reference_valid =
        client_static_lighting.GetLightmapState() == ClientLightingAssetState::Valid ||
        client_static_lighting.GetProbeState() == ClientLightingAssetState::Valid;
}

void NewPipelineClientRenderPath::ApplyRenderSettings(bool log_changes)
{
    ApplyShadowSettings(log_changes);
    ApplySSAOSettings(log_changes);
    if (scene_initialized)
    {
        ApplyEnvironmentProbeSettings(log_changes);
        ApplyBakedLightmapSettings(render_settings.baked_lightmaps_enabled, log_changes, true);
    }
}

void NewPipelineClientRenderPath::ConfigureLowEndLocalRendering()
{
    wi::renderer::SetDDGIEnabled(false);
    wi::renderer::SetDDGIRayCount(0);
    wi::renderer::SetDDGIDebugEnabled(false);
    setRaytracedDiffuseEnabled(false);
    setSSGIEnabled(false);
    setRaytracedReflectionsEnabled(false);
    setSSREnabled(false);
    setReflectionsEnabled(false);
    wi::backlog::post("Client local rendering profile: " + GetEffectiveAlgorithmSummary());
}

void NewPipelineClientRenderPath::ApplyShadowSettings(bool log_changes)
{
    setShadowsEnabled(render_settings.shadow_maps_enabled);
    wi::renderer::SetShadowsEnabled(render_settings.shadow_maps_enabled);
    wi::renderer::SetShadowProps2D(render_settings.shadow_maps_enabled ? kMobileShadow2DResolution : 0);
    wi::renderer::SetShadowPropsCube(render_settings.shadow_maps_enabled ? kMobileShadowCubeResolution : 0);
    wi::renderer::SetShadowMinResolution2D(
        render_settings.shadow_maps_enabled ? kMobileShadow2DMinResolution : 0);
    wi::renderer::SetShadowMinResolutionCube(
        render_settings.shadow_maps_enabled ? kMobileShadowCubeMinResolution : 0);
    ConfigureClientDirectionalShadowCascades();
    wi::renderer::SetRaytracedShadowsEnabled(false);
    wi::renderer::SetScreenSpaceShadowsEnabled(false);
    if (log_changes)
    {
        wi::backlog::post(std::string{"Client local shadows (4-cascade CSM / raster Shadow Map 2D 1024 / Cube 512): "} +
            EnabledString(render_settings.shadow_maps_enabled));
    }
}

void NewPipelineClientRenderPath::ConfigureClientDirectionalShadowCascades()
{
    if (!render_settings.shadow_maps_enabled)
        return;

    const float near_plane = std::max(0.01f, local_camera.zNearP);
    const float far_plane = std::max(near_plane + 1.0f, local_camera.zFarP);
    for (size_t light_index = 0; light_index < local_scene.lights.GetCount(); ++light_index)
    {
        wi::scene::LightComponent& light = local_scene.lights[light_index];
        if (light.GetType() != wi::scene::LightComponent::DIRECTIONAL ||
            !light.IsCastingShadow())
            continue;

        light.cascade_distances.resize(kMobileDirectionalCascadeCount);
        for (uint32_t cascade = 1; cascade <= kMobileDirectionalCascadeCount; ++cascade)
        {
            const float fraction =
                static_cast<float>(cascade) /
                static_cast<float>(kMobileDirectionalCascadeCount);
            const float logarithmic =
                near_plane * std::pow(far_plane / near_plane, fraction);
            const float uniform =
                near_plane + (far_plane - near_plane) * fraction;
            light.cascade_distances[cascade - 1] =
                cascade == kMobileDirectionalCascadeCount
                    ? far_plane
                    : uniform * (1.0f - kMobileDirectionalCascadeLambda) +
                        logarithmic * kMobileDirectionalCascadeLambda;
        }
    }
}

void NewPipelineClientRenderPath::ApplySSAOSettings(bool log_changes)
{
    setAORange(1.0f);
    setAOPower(1.0f);
    setAO(render_settings.ssao_enabled ? wi::RenderPath3D::AO_SSAO : wi::RenderPath3D::AO_DISABLED);
    if (log_changes)
    {
        wi::backlog::post(std::string{"Client local AO (SSAO): "} + EnabledString(render_settings.ssao_enabled));
    }
}

void NewPipelineClientRenderPath::UpdateElasticLighting(float dt)
{
    float quality = 0.0f;
    if (remote_consume.accepted_valid)
    {
        const float freshness = remote_consume.stale_timer <= kRemoteFullQualitySeconds
            ? 1.0f
            : 1.0f - (remote_consume.stale_timer - kRemoteFullQualitySeconds) /
                (kRemoteStaleTimeoutSeconds - kRemoteFullQualitySeconds);
        quality = std::clamp(freshness, 0.0f, 1.0f) *
            std::clamp(remote_consume.confidence, 0.0f, 1.0f);
    }
    elastic_remote_quality = quality;

    bool matching_history = false;
    const uint64_t current_lighting_fingerprint =
        has_published_control_packet
        ? ComputeControlLightingFingerprint(
            last_published_control_packet)
        : 0;
    if (accepted_remote_contract_v3_valid &&
        accepted_remote_source_control_frame_id != 0 &&
        !accepted_remote_metadata.camera_cut)
    {
        for (const RemoteGBufferHistoryEntry& entry :
            remote_gbuffer_history)
        {
            if (entry.valid &&
                entry.control_frame_id ==
                    accepted_remote_source_control_frame_id &&
                entry.scene_generation == scene_generation &&
                entry.lighting_fingerprint ==
                    current_lighting_fingerprint &&
                entry.depth.IsValid() &&
                entry.normal_roughness.IsValid())
            {
                matching_history = true;
                break;
            }
        }
    }
    if (!matching_history)
    {
        elastic_remote_quality = 0.0f;
        elastic_remote_gi_weight = 0.0f;
        elastic_remote_ao_weight = 0.0f;
        elastic_remote_specular_weight = 0.0f;
        elastic_remote_shadow_weight = 0.0f;
        ApplyElasticLightingResources();
        return;
    }

    const auto semantic_quality = [&](RemoteBufferSemantic semantic) {
        const size_t index = static_cast<size_t>(semantic);
        if (!accepted_remote_contract_v3_valid ||
            index >= accepted_remote_contract_v3.descriptors.size() ||
            (accepted_remote_buffer_mask & RemoteBufferKindMask(semantic)) == 0 ||
            !accepted_remote_textures[index].IsValid())
            return 0.0f;
        const RemoteBufferDescriptorV3& descriptor =
            accepted_remote_contract_v3.descriptors[index];
        if ((descriptor.flags & kRemoteBufferDescriptorAvailableV3) == 0 ||
            descriptor.content_generation != remote_consume.accepted_generation ||
            descriptor.content_frame_id > remote_consume.accepted_frame_id)
            return 0.0f;
        const uint64_t age =
            remote_consume.accepted_frame_id - descriptor.content_frame_id;
        const bool view_sensitive =
            semantic ==
                RemoteBufferSemantic::RemoteSpecularIndirect ||
            semantic ==
                RemoteBufferSemantic::RemoteShadowVisibility;
        // Specular and the authoritative shadow are view-sensitive and update
        // every published frame, so never retain an older atlas region. The
        // lower-frequency diffuse/AO regions keep their negotiated cadence and
        // fade independently after one retained frame.
        const float age_quality = view_sensitive
            ? (age == 0 ? 1.0f : 0.0f)
            : (age <= 1
                ? 1.0f
                : std::clamp(
                    1.0f -
                        static_cast<float>(age - 1u) / 3.0f,
                    0.0f,
                    1.0f));
        return (static_cast<float>(descriptor.confidence_unorm) / 65535.0f) *
            age_quality;
    };
    const float gi_quality =
        semantic_quality(RemoteBufferSemantic::RemoteIndirectDiffuse);
    const float ao_quality =
        semantic_quality(RemoteBufferSemantic::RemoteAO);
    const float specular_quality =
        semantic_quality(RemoteBufferSemantic::RemoteSpecularIndirect);
    float shadow_quality =
        semantic_quality(RemoteBufferSemantic::RemoteShadowVisibility);

    const uint64_t local_primary_id = GetNewPipelineSunStableId(local_scene);
    primary_light_entity =
        ResolveStableLightId(local_scene, local_primary_id);
    primary_light_generation =
        ComputeStableLightGeneration(local_scene, local_primary_id);
    if (accepted_remote_contract_v3_valid)
    {
        const RemoteBufferDescriptorV3& shadow_descriptor =
            accepted_remote_contract_v3.descriptors[
                static_cast<size_t>(
                    RemoteBufferSemantic::RemoteShadowVisibility)];
        if (shadow_descriptor.stable_subject_id != local_primary_id ||
            shadow_descriptor.stable_subject_generation !=
                primary_light_generation)
            shadow_quality = 0.0f;
    }

    const float ddgi_readiness = remote_consume.history_valid
        ? 1.0f
        : std::clamp(static_cast<float>(remote_ddgi_frame_index) / 64.0f, 0.0f, 1.0f);
    const float gi_target = render_settings.remote_gi_enabled && gi_quality > 0
        ? render_settings.remote_gi_max_weight * quality * gi_quality * ddgi_readiness
        : 0.0f;
    const float ao_target = render_settings.remote_ao_enabled && ao_quality > 0
        ? render_settings.remote_ao_max_weight * quality * ao_quality
        : 0.0f;
    const float specular_target =
        render_settings.remote_specular_enabled && specular_quality > 0
        ? render_settings.remote_specular_max_weight * quality * specular_quality
        : 0.0f;
    const float shadow_target =
        render_settings.remote_shadow_enabled && shadow_quality > 0
        ? render_settings.remote_shadow_max_weight * quality * shadow_quality
        : 0.0f;
    elastic_remote_gi_weight = gi_target > 0
        ? SmoothWeight(elastic_remote_gi_weight, gi_target, dt) : 0.0f;
    elastic_remote_ao_weight = ao_target > 0
        ? SmoothWeight(elastic_remote_ao_weight, ao_target, dt) : 0.0f;
    elastic_remote_specular_weight =
        specular_target > 0
        ? SmoothWeight(elastic_remote_specular_weight, specular_target, dt)
        : 0.0f;
    elastic_remote_shadow_weight =
        shadow_target > 0
        ? SmoothWeight(elastic_remote_shadow_weight, shadow_target, dt)
        : 0.0f;
    if (elastic_remote_gi_weight < 0.0001f)
        elastic_remote_gi_weight = 0.0f;
    if (elastic_remote_ao_weight < 0.0001f)
        elastic_remote_ao_weight = 0.0f;
    if (elastic_remote_specular_weight < 0.0001f)
        elastic_remote_specular_weight = 0.0f;
    if (elastic_remote_shadow_weight < 0.0001f)
        elastic_remote_shadow_weight = 0.0f;

    ApplyElasticLightingResources();
}

void NewPipelineClientRenderPath::ApplyElasticLightingResources()
{
    const size_t gi_index = static_cast<size_t>(RemoteBufferSemantic::RemoteIndirectDiffuse);
    const size_t ao_index = static_cast<size_t>(RemoteBufferSemantic::RemoteAO);
    const size_t specular_index =
        static_cast<size_t>(RemoteBufferSemantic::RemoteSpecularIndirect);
    const size_t shadow_index =
        static_cast<size_t>(RemoteBufferSemantic::RemoteShadowVisibility);
    visibilityResources.texture_remote_indirect_diffuse =
        elastic_remote_gi_weight > 0.0f && accepted_remote_textures[gi_index].IsValid()
            ? &accepted_remote_textures[gi_index]
            : nullptr;
    visibilityResources.texture_remote_ao =
        elastic_remote_ao_weight > 0.0f && accepted_remote_textures[ao_index].IsValid()
            ? &accepted_remote_textures[ao_index]
            : nullptr;
    visibilityResources.texture_remote_specular_indirect_pre_ao =
        elastic_remote_specular_weight > 0.0f &&
            accepted_remote_textures[specular_index].IsValid()
        ? &accepted_remote_textures[specular_index] : nullptr;
    visibilityResources.texture_remote_primary_light_visibility =
        elastic_remote_shadow_weight > 0.0f &&
            accepted_remote_textures[shadow_index].IsValid()
        ? &accepted_remote_textures[shadow_index] : nullptr;
    visibilityResources.remote_indirect_diffuse_weight = elastic_remote_gi_weight;
    visibilityResources.remote_ao_weight = elastic_remote_ao_weight;
    visibilityResources.remote_specular_indirect_weight =
        elastic_remote_specular_weight;
    visibilityResources.remote_primary_light_visibility_weight =
        elastic_remote_shadow_weight;
    visibilityResources.texture_remote_history_depth = nullptr;
    visibilityResources.texture_remote_history_normal_roughness = nullptr;
    for (const RemoteGBufferHistoryEntry& entry : remote_gbuffer_history)
    {
        if (!entry.valid ||
            entry.control_frame_id != accepted_remote_source_control_frame_id ||
            entry.scene_generation != scene_generation ||
            entry.lighting_fingerprint !=
                ComputeControlLightingFingerprint(
                    last_published_control_packet) ||
            !entry.depth.IsValid() || !entry.normal_roughness.IsValid())
            continue;
        visibilityResources.texture_remote_history_depth = &entry.depth;
        visibilityResources.texture_remote_history_normal_roughness =
            &entry.normal_roughness;
        visibilityResources.remote_view_projection = entry.view_projection;
        visibilityResources.remote_view_origin = entry.view_origin;
        visibilityResources.remote_history_near = entry.near_plane;
        visibilityResources.remote_history_far = entry.far_plane;
        break;
    }
    if (visibilityResources.texture_remote_history_depth == nullptr)
    {
        visibilityResources.remote_indirect_diffuse_weight = 0.0f;
        visibilityResources.remote_ao_weight = 0.0f;
        visibilityResources.remote_specular_indirect_weight = 0.0f;
        visibilityResources.remote_primary_light_visibility_weight = 0.0f;
    }
    visibilityResources.texture_elastic_indirect_diffuse =
        elastic_indirect_diffuse.IsValid() ? &elastic_indirect_diffuse : nullptr;
    visibilityResources.texture_elastic_ao = elastic_ao.IsValid() ? &elastic_ao : nullptr;
    visibilityResources.texture_elastic_specular_indirect_pre_ao =
        elastic_specular_indirect_pre_ao.IsValid()
        ? &elastic_specular_indirect_pre_ao : nullptr;
    visibilityResources.texture_elastic_primary_light_visibility =
        elastic_primary_light_visibility.IsValid()
        ? &elastic_primary_light_visibility : nullptr;
    visibilityResources.texture_specular_indirect_pre_ao = local_specular_indirect_pre_ao.IsValid()
        ? &local_specular_indirect_pre_ao : nullptr;
    visibilityResources.texture_primary_light_visibility = local_primary_light_visibility.IsValid()
        ? &local_primary_light_visibility : nullptr;
    const uint64_t primary_light_id = GetNewPipelineSunStableId(local_scene);
    const uint32_t primary_shadow_index =
        ResolveStableDirectionalLightShadowIndex(local_scene, primary_light_id);
    visibilityResources.primary_light_shadow_index = primary_shadow_index < 16
        ? static_cast<int>(primary_shadow_index) : -1;
}

void NewPipelineClientRenderPath::ApplyEnvironmentProbeSettings(bool log_changes)
{
    if (!scene_initialized)
        return;

    if (environment_probe_entity == wi::ecs::INVALID_ENTITY)
    {
        const wi::ecs::Entity existing = local_scene.Entity_FindByName(kClientEnvironmentProbeName);
        if (existing != wi::ecs::INVALID_ENTITY && local_scene.probes.Contains(existing))
        {
            environment_probe_entity = existing;
            environment_probe_created_by_client = false;
        }
    }

    if (!render_settings.environment_probe_enabled)
    {
        if (wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity))
            InitializeBlackEnvironmentProbe(*probe);
        environment_probe_load_attempted = false;
        reflection_probe_mip_subresources.clear();
        if (log_changes)
        {
            wi::backlog::post("Client environment probe: disabled");
        }
        return;
    }

    if (environment_probe_entity == wi::ecs::INVALID_ENTITY)
    {
        environment_probe_entity = local_scene.Entity_CreateEnvironmentProbe(kClientEnvironmentProbeName, XMFLOAT3(0, 0, 0));
        environment_probe_created_by_client = true;
        environment_probe_load_attempted = false;
        if (wi::scene::TransformComponent* transform = local_scene.transforms.GetComponent(environment_probe_entity))
            PlaceNewEnvironmentProbe(*transform);
    }

    if (!environment_probe_load_attempted && !IsReflectionProbeBakeActive())
        LoadEnvironmentProbeAsset();

    if (log_changes)
    {
        wi::backlog::post("Client baked environment probe: enabled; " + reflection_probe_status);
    }
}

void NewPipelineClientRenderPath::PlaceNewEnvironmentProbe(wi::scene::TransformComponent& transform) const
{
    wi::primitive::AABB bounds = local_scene.bounds;
    if (!bounds.IsValid())
    {
        for (size_t i = 0; i < local_scene.objects.GetCount(); ++i)
        {
            const wi::scene::ObjectComponent& object = local_scene.objects[i];
            const wi::scene::MeshComponent* mesh = local_scene.meshes.GetComponent(object.meshID);
            const wi::scene::TransformComponent* object_transform =
                local_scene.transforms.GetComponent(local_scene.objects.GetEntity(i));
            if (mesh != nullptr && mesh->aabb.IsValid() && object_transform != nullptr)
                bounds = wi::primitive::AABB::Merge(bounds, mesh->aabb.transform(object_transform->GetWorldMatrix()));
        }
    }

    XMFLOAT3 center = {};
    XMFLOAT3 half_width = XMFLOAT3(1, 1, 1);
    if (bounds.IsValid())
    {
        center = bounds.getCenter();
        half_width = bounds.getHalfWidth();
        half_width.x = std::max(1.0f, half_width.x * 1.05f);
        half_width.y = std::max(1.0f, half_width.y * 1.05f);
        half_width.z = std::max(1.0f, half_width.z * 1.05f);
    }
    transform.ClearTransform();
    transform.Translate(center);
    transform.Scale(half_width);
    transform.UpdateTransform();
}

void NewPipelineClientRenderPath::InitializeBlackEnvironmentProbe(wi::scene::EnvironmentProbeComponent& probe)
{
    probe.resource = {};
    probe.textureName.clear();
    probe.texture = {};
    probe.resolution = kClientReflectionProbeResolution;
    probe.SetRealTime(false);
    probe.SetMSAA(false);
    probe.CreateRenderData();
    // CreateRenderData allocates a zeroed BC6H cube and marks it dirty. This
    // explicit reset is what makes a missing package stay black instead of
    // silently triggering a runtime capture on the next frame.
    probe.SetDirty(false);
    probe.render_dirty = false;
    probe.first_render = false;
    probe.subresource = -1;
}

void NewPipelineClientRenderPath::LoadEnvironmentProbeAsset()
{
    environment_probe_load_attempted = true;
    reflection_probe_mip_subresources.clear();
    reflection_probe_asset_path = scene_asset_path.empty()
        ? std::string{}
        : ClientReflectionProbePackage::PackagePathForScene(scene_asset_path);

    wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity);
    if (probe == nullptr)
    {
        reflection_probe_status = "Reflection Probe: unavailable (entity missing)";
        client_static_lighting.SetProbeStatus(ClientLightingAssetState::Unavailable, reflection_probe_status);
        return;
    }

    if (reflection_probe_asset_path.empty())
    {
        InitializeBlackEnvironmentProbe(*probe);
        reflection_probe_status = "Reflection Probe: UNAVAILABLE no persistent .wiscene";
        client_static_lighting.SetProbeStatus(ClientLightingAssetState::Unavailable, reflection_probe_status);
        CreateEnvironmentProbeMipViews();
        return;
    }

    const ClientReflectionProbeDescriptor descriptor = ClientReflectionProbePackage::Describe(
        local_scene, environment_probe_entity, kClientReflectionProbeResolution);
    ClientReflectionProbePackageResult load_result = client_static_lighting.LoadProbe(scene_asset_path, descriptor);
    reflection_probe_status = load_result.diagnostic;
    if (!load_result.success)
    {
        InitializeBlackEnvironmentProbe(*probe);
        wi::backlog::post(load_result.diagnostic + "; black fallback");
        CreateEnvironmentProbeMipViews();
        return;
    }

    probe->resource = load_result.resource;
    probe->texture = load_result.resource.GetTexture();
    probe->textureName.clear();
    probe->resolution = descriptor.resolution;
    probe->SetRealTime(false);
    probe->SetDirty(false);
    probe->render_dirty = false;
    probe->first_render = false;
    CreateEnvironmentProbeMipViews();
    wi::backlog::post(load_result.diagnostic + " -> " + reflection_probe_asset_path);
}

void NewPipelineClientRenderPath::LoadStaticLightingAssets()
{
    if (scene_asset_path.empty())
        return;
    const ClientLightmapPackageResult lightmap_result =
        client_static_lighting.LoadLightmaps(scene_asset_path, local_scene);
    client_volumetric_lightmap =
        lightmap_result.success
            ? lightmap_result.volumetric_lightmap
            : ClientVolumetricLightmapData{};
    if (lightmap_result.scene_replaced)
    {
        scene_source_root_entity = wi::ecs::INVALID_ENTITY;
        environment_probe_entity = wi::ecs::INVALID_ENTITY;
        environment_probe_created_by_client = false;
        AdvanceSceneGeneration("static lighting scene reload");
    }
    lightmap_bake_status = client_static_lighting.GetLightmapStatus();
    wi::backlog::post(lightmap_result.diagnostic);
    environment_probe_load_attempted = false;
    if (render_settings.environment_probe_enabled)
        ApplyEnvironmentProbeSettings(false);
    if (!render_settings.baked_lightmaps_enabled)
        client_static_lighting.DisableLightmaps(local_scene);
}

void NewPipelineClientRenderPath::CreateEnvironmentProbeMipViews()
{
    reflection_probe_mip_subresources.clear();
    wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity);
    if (probe == nullptr || !probe->texture.IsValid())
        return;

    const wi::graphics::TextureDesc& desc = probe->texture.GetDesc();
    reflection_probe_mip_subresources.reserve(desc.mip_levels);
    for (uint32_t mip = 0; mip < desc.mip_levels; ++mip)
    {
        reflection_probe_mip_subresources.push_back(wi::graphics::GetDevice()->CreateSubresource(
            &probe->texture,
            wi::graphics::SubresourceType::SRV,
            0,
            desc.array_size,
            mip,
            1));
    }
    reflection_probe_debug_mip = std::min<uint32_t>(
        reflection_probe_debug_mip,
        reflection_probe_mip_subresources.empty() ? 0u : uint32_t(reflection_probe_mip_subresources.size() - 1));
}

void NewPipelineClientRenderPath::SetReflectionProbeDebugMip(uint32_t mip)
{
    const uint32_t count = GetReflectionProbeDebugMipCount();
    reflection_probe_debug_mip = count == 0 ? 0 : std::min(mip, count - 1);
    debug_preview_invalid_logged = false;
}

uint32_t NewPipelineClientRenderPath::GetReflectionProbeDebugMipCount() const
{
    return static_cast<uint32_t>(reflection_probe_mip_subresources.size());
}

void NewPipelineClientRenderPath::ApplyBakedLightmapSettings(bool previous_enabled, bool log_changes, bool force_log)
{
    if (!scene_initialized)
        return;

    const bool changed = previous_enabled != render_settings.baked_lightmaps_enabled;
    if (changed && render_settings.baked_lightmaps_enabled && !client_static_lighting.IsStale())
    {
        RestoreBakedLightmaps();
    }
    else if (changed)
    {
        DisableBakedLightmaps();
    }

    if (log_changes && (changed || force_log))
    {
        wi::backlog::post("Client baked lightmaps: " + EnabledString(render_settings.baked_lightmaps_enabled));
    }
}

void NewPipelineClientRenderPath::DisableBakedLightmaps()
{
    client_static_lighting.DisableLightmaps(local_scene);
}

void NewPipelineClientRenderPath::RestoreBakedLightmaps()
{
    client_static_lighting.RestoreLightmaps(local_scene);
}

void NewPipelineClientRenderPath::RequestStaticLightingBake()
{
    if (IsStaticLightingBakeActive())
    {
        wi::backlog::post("Client static lighting request ignored: a bake is already active");
        return;
    }
    static_lighting_bake_requested = true;
    RequestLightmapBake();
    if (!IsLightmapBakeActive())
        static_lighting_bake_requested = false;
}

void NewPipelineClientRenderPath::CancelStaticLightingBake()
{
    static_lighting_bake_requested = false;
    if (IsLightmapBakeActive())
        CancelLightmapBake();
    else if (IsReflectionProbeBakeActive())
        CancelReflectionProbeBake();
}

bool NewPipelineClientRenderPath::IsStaticLightingBakeActive() const
{
    return static_lighting_bake_requested || IsLightmapBakeActive() || IsReflectionProbeBakeActive();
}

std::string NewPipelineClientRenderPath::GetStaticLightingBakeStatus() const
{
    if (static_lighting_bake_requested && IsLightmapBakeActive())
        return "Client Lighting 1/2 (2D + VLM) - " + lightmap_bake_status;
    if (static_lighting_bake_requested && IsReflectionProbeBakeActive())
        return "Client Lighting 2/2 - " + reflection_probe_status;
    return client_static_lighting.GetStatusSummary();
}

void NewPipelineClientRenderPath::RequestLightmapBake()
{
    if (IsLightmapBakeActive())
    {
        wi::backlog::post("Client lightmap bake request ignored: a bake is already active");
        return;
    }
    if (IsReflectionProbeBakeActive())
    {
        wi::backlog::post("Client lightmap bake request ignored: reflection probe bake is active");
        return;
    }

    lightmap_bake_denoiser_available =
        wi::scene::ObjectComponent::IsLightmapDenoiserAvailable();
    lightmap_bake_denoiser_required = true;
    wi::backlog::post(
        "Client lightmap production quality: integrator=vertex-nee-mis-v4-geometric-bias"
        " sampler=independent-owen-sobol-batches"
        " light_selection=spatial-candidate-contribution-cdf"
        " guiding=first-bounce-tile-mixture denoiser=OIDN-required"
        " samples=" + std::to_string(lightmap_bake_settings.sample_count));
    if (!scene_initialized || scene_asset_path.empty())
    {
        render_settings.lightmap_bake_requested = false;
        lightmap_bake_state = LightmapBakeState::Failed;
        lightmap_bake_status = "Lightmap: UNAVAILABLE no persistent .wiscene";
        client_static_lighting.SetLightmapStatus(ClientLightingAssetState::Unavailable, lightmap_bake_status);
        wi::backlog::post(lightmap_bake_status);
        return;
    }
    if (lightmap_bake_denoiser_required && !lightmap_bake_denoiser_available)
    {
        render_settings.lightmap_bake_requested = false;
        lightmap_bake_state = LightmapBakeState::Failed;
        lightmap_bake_status =
            "Lightmap: UNAVAILABLE OpenImageDenoise is required for production bakes";
        client_static_lighting.SetLightmapStatus(
            ClientLightingAssetState::Unavailable, lightmap_bake_status);
        static_lighting_bake_requested = false;
        wi::backlog::post(lightmap_bake_status);
        return;
    }
    source_scene_hash_before_bake = ClientLightmapPackage::HashFile(scene_asset_path);
    if (source_scene_hash_before_bake == 0)
    {
        render_settings.lightmap_bake_requested = false;
        lightmap_bake_state = LightmapBakeState::Failed;
        lightmap_bake_status = "Lightmap: failed - source scene is unreadable";
        client_static_lighting.SetLightmapStatus(ClientLightingAssetState::Unavailable, lightmap_bake_status);
        wi::backlog::post(lightmap_bake_status);
        return;
    }
    if (environment_probe_entity != wi::ecs::INVALID_ENTITY)
        ClientReflectionProbePackage::EnsureProbeId(local_scene, environment_probe_entity);
    lightmap_bake_scene_fingerprint = ComputeSceneParityFingerprint(local_scene);
    lightmap_bake_scene_fingerprint_valid = true;
    wi::backlog::post("Client lightmap scene baseline: " +
        FormatSceneParityFingerprint(lightmap_bake_scene_fingerprint));
    render_settings.baked_lightmaps_enabled = true;
    render_settings.lightmap_bake_requested = true;
    previous_raytrace_bounce_count = wi::renderer::GetRaytraceBounceCount();
    lightmap_cancel_requested = false;
    lightmap_bake_state = LightmapBakeState::Preparing;
    lightmap_bake_status = "Lightmap: preparing atlas and scene metadata";
    client_static_lighting.SetLightmapStatus(ClientLightingAssetState::Baking, lightmap_bake_status);
    wi::backlog::post("Client lightmap bake requested: " + scene_asset_path);
}

void NewPipelineClientRenderPath::RequestReflectionProbeBake()
{
    if (IsReflectionProbeBakeActive())
    {
        wi::backlog::post("Client reflection probe bake request ignored: a bake is already active");
        return;
    }
    if (IsLightmapBakeActive())
    {
        wi::backlog::post("Client reflection probe bake request ignored: lightmap bake is active");
        return;
    }
    if (!scene_initialized || scene_asset_path.empty())
    {
        static_lighting_bake_requested = false;
        reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
        reflection_probe_status = "Reflection Probe: UNAVAILABLE no persistent .wiscene";
        client_static_lighting.SetProbeStatus(ClientLightingAssetState::Unavailable, reflection_probe_status);
        wi::backlog::post(reflection_probe_status);
        return;
    }

    render_settings.environment_probe_enabled = true;
    if (environment_probe_entity == wi::ecs::INVALID_ENTITY)
        ApplyEnvironmentProbeSettings(false);
    wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity);
    if (probe == nullptr)
    {
        static_lighting_bake_requested = false;
        reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
        reflection_probe_status = "Reflection Probe: failed (entity unavailable)";
        client_static_lighting.SetProbeStatus(ClientLightingAssetState::Unavailable, reflection_probe_status);
        wi::backlog::post(reflection_probe_status);
        return;
    }
    if (ClientReflectionProbePackage::GetProbeId(local_scene, environment_probe_entity).empty())
    {
        static_lighting_bake_requested = false;
        reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
        reflection_probe_status =
            "Reflection Probe: STALE placement is not persisted; run Generate Client Lighting";
        client_static_lighting.SetProbeStatus(ClientLightingAssetState::Stale, reflection_probe_status);
        wi::backlog::post(reflection_probe_status);
        return;
    }

    // Detach the previously loaded DDS before requesting a capture. Otherwise
    // CreateRenderData would keep resolving the cached asset and clear DIRTY.
    probe->resource = {};
    probe->textureName.clear();
    probe->texture = {};
    probe->resolution = kClientReflectionProbeResolution;
    probe->SetRealTime(false);
    probe->SetMSAA(false);
    probe->CreateRenderData();
    probe->first_render = true;
    probe->SetDirty(true);
    probe->render_dirty = false;

    environment_probe_load_attempted = true;
    reflection_probe_mip_subresources.clear();
    reflection_probe_asset_path = ClientReflectionProbePackage::PackagePathForScene(scene_asset_path);
    reflection_probe_bake_state = ReflectionProbeBakeState::Capturing;
    reflection_probe_status = "Reflection Probe: capturing 128px BC6H cubemap";
    client_static_lighting.SetProbeStatus(ClientLightingAssetState::Baking, reflection_probe_status);
    wi::backlog::post("Client Reflection Probe bake requested: " + reflection_probe_asset_path);
}

void NewPipelineClientRenderPath::CancelReflectionProbeBake()
{
    if (!IsReflectionProbeBakeActive())
        return;
    const std::string cancellation = "Reflection Probe: cancelled; previous package preserved";
    reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
    environment_probe_load_attempted = false;
    LoadEnvironmentProbeAsset();
    reflection_probe_status = cancellation;
    wi::backlog::post(reflection_probe_status);
}

bool NewPipelineClientRenderPath::IsReflectionProbeBakeActive() const
{
    return reflection_probe_bake_state == ReflectionProbeBakeState::Capturing ||
        reflection_probe_bake_state == ReflectionProbeBakeState::Saving;
}

void NewPipelineClientRenderPath::UpdateReflectionProbeBake()
{
    if (reflection_probe_bake_state != ReflectionProbeBakeState::Capturing)
        return;

    wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity);
    if (probe == nullptr)
    {
        reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
        reflection_probe_status = "Reflection Probe: failed (entity disappeared)";
        client_static_lighting.SetProbeStatus(ClientLightingAssetState::Corrupt, reflection_probe_status);
        static_lighting_bake_requested = false;
        wi::backlog::post(reflection_probe_status);
        return;
    }
    if (probe->IsDirty() || probe->render_dirty || probe->first_render)
        return;
    if (!probe->texture.IsValid() ||
        probe->texture.GetDesc().format != wi::graphics::Format::BC6H_UF16 ||
        !has_flag(probe->texture.GetDesc().misc_flags, wi::graphics::ResourceMiscFlag::TEXTURECUBE))
    {
        const std::string failure = "Reflection Probe: failed (capture texture is not BC6H cubemap)";
        reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
        static_lighting_bake_requested = false;
        environment_probe_load_attempted = false;
        LoadEnvironmentProbeAsset();
        reflection_probe_status = failure;
        wi::backlog::post(reflection_probe_status);
        return;
    }

    reflection_probe_bake_state = ReflectionProbeBakeState::Saving;
    reflection_probe_status = "Reflection Probe: saving validated .clientprobe package";
    client_static_lighting.SetProbeStatus(ClientLightingAssetState::Baking, reflection_probe_status);
    const ClientReflectionProbeDescriptor descriptor = ClientReflectionProbePackage::Describe(
        local_scene, environment_probe_entity, kClientReflectionProbeResolution);
    std::string error;
    if (!client_static_lighting.ProbePackage().Save(scene_asset_path, descriptor, probe->texture, error))
    {
        const std::string failure = "Reflection Probe: failed - " + error;
        reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
        static_lighting_bake_requested = false;
        environment_probe_load_attempted = false;
        LoadEnvironmentProbeAsset();
        reflection_probe_status = failure;
        wi::backlog::post(reflection_probe_status);
        return;
    }

    ClientReflectionProbePackageResult verify_result =
        client_static_lighting.LoadProbe(scene_asset_path, descriptor);
    if (!verify_result.success)
    {
        reflection_probe_bake_state = ReflectionProbeBakeState::Failed;
        reflection_probe_status =
            "Reflection Probe: committed but reload verification failed - " + verify_result.diagnostic;
        client_static_lighting.SetProbeStatus(ClientLightingAssetState::Corrupt, reflection_probe_status);
        static_lighting_bake_requested = false;
        wi::backlog::post(reflection_probe_status);
        return;
    }
    probe->resource = verify_result.resource;
    probe->texture = verify_result.resource.GetTexture();
    probe->textureName.clear();
    probe->SetDirty(false);
    probe->render_dirty = false;
    probe->first_render = false;
    CreateEnvironmentProbeMipViews();
    reflection_probe_bake_state = ReflectionProbeBakeState::Completed;
    reflection_probe_status = "Reflection Probe: VALID 128px, " +
        std::to_string(probe->texture.GetDesc().mip_levels) + " mips -> " + reflection_probe_asset_path;
    client_static_lighting.SetProbeStatus(ClientLightingAssetState::Valid, reflection_probe_status);
    client_static_lighting.ClearStale();
    baked_sun_state = sun_state;
    baked_sun_reference_valid = true;
    static_lighting_bake_requested = false;
    wi::backlog::post(reflection_probe_status);
}

void NewPipelineClientRenderPath::CancelLightmapBake()
{
    render_settings.lightmap_bake_requested = false;
    if (IsLightmapBakeActive())
    {
        lightmap_cancel_requested = true;
        lightmap_bake_status = "Lightmap: cancelling";
    }
}

bool NewPipelineClientRenderPath::IsLightmapBakeActive() const
{
    return lightmap_bake_state == LightmapBakeState::Preparing ||
        lightmap_bake_state == LightmapBakeState::Baking ||
        lightmap_bake_state == LightmapBakeState::BakingVolume ||
        lightmap_bake_state == LightmapBakeState::ReadingVolume ||
        lightmap_bake_state == LightmapBakeState::Saving;
}

std::string NewPipelineClientRenderPath::GetLightmapBakeStatus() const
{
    return lightmap_bake_status;
}

bool NewPipelineClientRenderPath::SavePreparedScene(const std::string& path, std::string& error)
{
    for (size_t i = 0; i < local_scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = local_scene.objects[i];
        object.SetLightmapRenderRequest(false);
        object.lightmap_render = {};
        object.lightmapTextureData.clear();
    }

    // LoadModel(attached=true) creates an identity grouping root that is not
    // part of the source asset. Remove it before persisting, otherwise every
    // bake/load/save cycle would add another hierarchy level.
    if (scene_source_root_entity != wi::ecs::INVALID_ENTITY &&
        local_scene.transforms.Contains(scene_source_root_entity))
    {
        local_scene.Component_DetachChildren(scene_source_root_entity);
        local_scene.Entity_Remove(scene_source_root_entity);
        scene_source_root_entity = wi::ecs::INVALID_ENTITY;
    }

    // Persist the authored/generated probe entity, placement and stable ID in
    // the derived Client scene. The canonical source and large BC6H cubemap
    // remain external and untouched.
    if (wi::scene::EnvironmentProbeComponent* probe =
        local_scene.probes.GetComponent(environment_probe_entity))
    {
        probe->textureName.clear();
    }

    {
        wi::Archive archive(path, false);
        if (!archive.IsOpen())
        {
            error = "cannot create prepared scene archive: " + path;
            return false;
        }
        local_scene.Serialize(archive);
        // Archive owns the output path and saves it from its destructor.
        // Calling Close() here is unsafe because Close() keeps fileName and
        // pos after clearing DATA; the destructor would then call Close() a
        // second time and try to write a non-zero byte count from nullptr.
        // MSVC's ostream invalid-parameter handler terminates the process in
        // that case (FAST_FAIL_INVALID_ARG).
    }

    if (!wi::helper::FileExists(path))
    {
        error = "prepared scene archive was not written: " + path;
        return false;
    }
    return true;
}

bool NewPipelineClientRenderPath::PrepareLightmapBake()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(scene_asset_path, ec) || ec)
    {
        FailLightmapBake("source scene is missing or unreadable: " + scene_asset_path);
        return false;
    }
    std::string source_error;
    if (!VerifySourceSceneUnchanged(source_error))
    {
        FailLightmapBake(source_error);
        return false;
    }

    lightmap_bake_queue.clear();
    lightmap_bake_completed.clear();
    ResetLightmapBakeScheduling();
    lightmap_bake_dimensions.clear();
    lightmap_bake_next_index = 0;
    lightmap_bake_skipped = 0;
    prepared_derived_scene_hash = 0;
    const float max_float = std::numeric_limits<float>::max();
    lightmap_irradiance_min = XMFLOAT3(max_float, max_float, max_float);
    lightmap_irradiance_sum = {};
    lightmap_irradiance_max = {};
    lightmap_valid_texel_count = 0;
    lightmap_missing_texel_count = 0;
    lightmap_invalid_texel_count = 0;

    std::unordered_map<wi::ecs::Entity, XMUINT2> mesh_dimensions;
    std::unordered_set<std::string> object_ids;
    LightmapBakeCoverage coverage;
    coverage.total = static_cast<uint32_t>(local_scene.objects.GetCount());
    uint32_t skipped_logged = 0;
    for (size_t i = 0; i < local_scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = local_scene.objects[i];
        const wi::ecs::Entity entity = local_scene.objects.GetEntity(i);
        const LightmapBakeEligibility eligibility =
            ClassifyLightmapBakeEligibility(local_scene, entity, object);
        AccumulateCoverage(coverage, eligibility);
        if (eligibility != LightmapBakeEligibility::Eligible &&
            eligibility != LightmapBakeEligibility::ForcedEligible)
        {
            ++lightmap_bake_skipped;
            continue;
        }

        wi::scene::MeshComponent* mesh = local_scene.meshes.GetComponent(object.meshID);
        XMUINT2 dimensions = {kMobileLightmapResolution, kMobileLightmapResolution};
        const auto known_dimensions = mesh_dimensions.find(object.meshID);
        if (known_dimensions != mesh_dimensions.end())
        {
            dimensions = known_dimensions->second;
        }
        else if (mesh != nullptr && mesh->vertex_atlas.empty())
        {
            std::string atlas_error;
            if (!GenerateClientLightmapAtlas(
                local_scene,
                object.meshID,
                lightmap_bake_settings.resolution,
                dimensions.x,
                dimensions.y,
                atlas_error))
            {
                ++lightmap_bake_skipped;
                ++coverage.atlas_failures;
                if (skipped_logged++ < 8)
                    wi::backlog::post("Client lightmap atlas skipped object " + std::to_string(entity) + ": " + atlas_error);
                continue;
            }
        }
        else
        {
            if (object.lightmapWidth > 0 && object.lightmapHeight > 0)
                dimensions = {object.lightmapWidth, object.lightmapHeight};
        }

        dimensions.x = FinalizeClientLightmapDimension(dimensions.x);
        dimensions.y = FinalizeClientLightmapDimension(dimensions.y);
        if (dimensions.x == 0 || dimensions.y == 0)
        {
            FailLightmapBake("lightmap dimensions exceed the supported 16384 texel limit");
            return false;
        }

        mesh_dimensions[object.meshID] = dimensions;

        std::string id = ClientLightmapPackage::EnsureObjectId(local_scene, entity);
        if (!object_ids.insert(id).second)
        {
            wi::scene::MetadataComponent* metadata = local_scene.metadatas.GetComponent(entity);
            id += "-" + std::to_string(entity);
            metadata->string_values.set(ClientLightmapPackage::kObjectIdMetadataKey, id);
            object_ids.insert(id);
        }

        object.ClearLightmap();
        object.lightmapWidth = dimensions.x;
        object.lightmapHeight = dimensions.y;
        object.SetLightmapDisableBlockCompression(false);
        lightmap_bake_dimensions[entity] = dimensions;
        lightmap_bake_queue.push_back(entity);
    }

    wi::backlog::post("Client lightmap coverage: " +
        FormatLightmapBakeCoverage(coverage, lightmap_bake_queue.size()));

    if (lightmap_bake_queue.empty())
    {
        FailLightmapBake("no eligible static opaque objects were found");
        return false;
    }

    prepared_scene_temp_path = ClientLightmapPackage::DerivedScenePathForScene(scene_asset_path) + ".tmp";
    prepared_package_temp_path = ClientLightmapPackage::PackagePathForScene(scene_asset_path) + ".tmp";
    CleanupLightmapBakeTemps();
    std::string save_error;
    if (!SavePreparedScene(prepared_scene_temp_path, save_error))
    {
        FailLightmapBake(save_error);
        return false;
    }
    prepared_derived_scene_hash = ClientLightmapPackage::HashFile(prepared_scene_temp_path);
    if (prepared_derived_scene_hash == 0)
    {
        FailLightmapBake("failed to hash prepared scene");
        return false;
    }

    previous_raytrace_bounce_count = wi::renderer::GetRaytraceBounceCount();
    wi::renderer::SetRaytraceBounceCount(lightmap_bake_settings.bounce_count);
    local_scene.SetAccelerationStructureUpdateRequested(true);
    lightmap_bake_state = LightmapBakeState::Baking;
    lightmap_bake_rate_time_usec = NowUsec();
    wi::backlog::post("Client lightmap bake prepared: objects=" + std::to_string(lightmap_bake_queue.size()) +
        " skipped=" + std::to_string(lightmap_bake_skipped) +
        " samples=" + std::to_string(lightmap_bake_settings.sample_count) +
        " bounces=" + std::to_string(lightmap_bake_settings.bounce_count));
    if (!FillLightmapBakeBatch())
    {
        FailLightmapBake("eligible lightmap objects disappeared during preparation");
        return false;
    }
    UpdateLightmapBakeProgress();
    return true;
}

uint64_t NewPipelineClientRenderPath::GetActiveLightmapTexelCount() const
{
    uint64_t texels = 0;
    for (const wi::ecs::Entity entity : lightmap_bake_active)
    {
        const auto dimensions = lightmap_bake_dimensions.find(entity);
        if (dimensions != lightmap_bake_dimensions.end())
            texels += uint64_t(dimensions->second.x) * uint64_t(dimensions->second.y);
    }
    return texels;
}

uint64_t NewPipelineClientRenderPath::GetLightmapBakeTotalSamples() const
{
    uint64_t samples = uint64_t(lightmap_bake_completed.size() + lightmap_bake_pending_save.size()) *
        uint64_t(lightmap_bake_settings.sample_count);
    for (const wi::ecs::Entity entity : lightmap_bake_active)
    {
        const wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity);
        if (object != nullptr)
        {
            samples += std::min(object->lightmapIterationCount, lightmap_bake_settings.sample_count);
        }
    }
    return samples;
}

bool NewPipelineClientRenderPath::FillLightmapBakeBatch()
{
    const wi::graphics::GraphicsDevice::MemoryUsage memory = wi::graphics::GetDevice()->GetMemoryUsage();
    const uint64_t reserve = std::max(kLightmapMinimumVRAMReserve, memory.budget / uint64_t(10));
    const uint64_t transient_cap = memory.budget / 4ull;
    uint64_t estimated_inflight_bytes = 0;
    uint64_t newly_planned_bytes = 0;

    const auto accumulate_estimate = [&](const wi::vector<wi::ecs::Entity>& entities) {
        for (const wi::ecs::Entity entity : entities)
        {
            const auto dimensions = lightmap_bake_dimensions.find(entity);
            if (dimensions != lightmap_bake_dimensions.end())
                estimated_inflight_bytes += EstimateLightmapTransientBytes(dimensions->second);
        }
    };
    accumulate_estimate(lightmap_bake_active);
    accumulate_estimate(lightmap_bake_pending_save);

    bool changed = false;
    while (lightmap_bake_next_index < lightmap_bake_queue.size() &&
        lightmap_bake_active.size() + lightmap_bake_pending_save.size() < kMaxLightmapsInFlight)
    {
        const wi::ecs::Entity entity = lightmap_bake_queue[lightmap_bake_next_index];
        wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity);
        const auto dimensions = lightmap_bake_dimensions.find(entity);
        if (object == nullptr || dimensions == lightmap_bake_dimensions.end())
        {
            ++lightmap_bake_next_index;
            continue;
        }

        const uint64_t candidate_bytes = EstimateLightmapTransientBytes(dimensions->second);
        const bool no_inflight = lightmap_bake_active.empty() && lightmap_bake_pending_save.empty();
        bool within_budget = false;
        if (memory.budget > 0)
        {
            const uint64_t usable_budget = memory.budget > reserve ? memory.budget - reserve : 0;
            const bool within_transient_cap =
                candidate_bytes <= transient_cap &&
                estimated_inflight_bytes <= transient_cap - candidate_bytes;
            const bool within_device_headroom =
                memory.usage < usable_budget &&
                newly_planned_bytes <= usable_budget - memory.usage &&
                candidate_bytes <= usable_budget - memory.usage - newly_planned_bytes;
            within_budget = within_transient_cap && within_device_headroom;
        }

        // Always admit one object so low/unknown reported budgets make
        // progress. Subsequent allocations remain strictly budget controlled.
        if (!within_budget && !no_inflight)
            break;

        ++lightmap_bake_next_index;
        object->ClearLightmap();
        object->lightmapWidth = dimensions->second.x;
        object->lightmapHeight = dimensions->second.y;
        object->SetLightmapDisableBlockCompression(false);
        object->SetLightmapRenderRequest(true);
        lightmap_bake_active.push_back(entity);
        estimated_inflight_bytes += candidate_bytes;
        newly_planned_bytes += candidate_bytes;
        changed = true;
    }

    if (changed)
    {
        lightmap_bake_last_progress_time_usec = NowUsec();
        lightmap_bake_last_progress_sample_total = GetLightmapBakeTotalSamples();
        wi::backlog::post(
            "Client lightmap batch: active=" + std::to_string(lightmap_bake_active.size()) +
            " pending_save=" + std::to_string(lightmap_bake_pending_save.size()) +
            " estimated_transient_mb=" + std::to_string(estimated_inflight_bytes / (1024ull * 1024ull)) +
            " vram_mb=" + std::to_string(memory.usage / (1024ull * 1024ull)) + "/" +
                std::to_string(memory.budget / (1024ull * 1024ull)));
    }

    return !lightmap_bake_active.empty();
}

bool NewPipelineClientRenderPath::FinalizeOnePendingLightmap()
{
    if (lightmap_bake_pending_save.empty())
        return true;

    const wi::ecs::Entity entity = lightmap_bake_pending_save.front();
    wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity);
    const wi::scene::NameComponent* name = local_scene.names.GetComponent(entity);
    const std::string object_name = name != nullptr && !name->name.empty() ? name->name : std::to_string(entity);
    if (object == nullptr)
    {
        FailLightmapBake("pending lightmap object disappeared: " + object_name);
        return false;
    }

    object->SaveLightmap();
    if (!object->lightmap.IsValid() || object->lightmapTextureData.empty() ||
        object->lightmap.GetDesc().format != wi::graphics::Format::BC6H_UF16)
    {
        FailLightmapBake("BC6H compression failed for object " + object_name);
        return false;
    }

    const auto dimensions = lightmap_bake_dimensions.find(entity);
    const wi::graphics::TextureDesc& texture_desc = object->lightmap.GetDesc();
    if (dimensions == lightmap_bake_dimensions.end() ||
        texture_desc.width != object->lightmapWidth || texture_desc.height != object->lightmapHeight ||
        texture_desc.width != dimensions->second.x || texture_desc.height != dimensions->second.y ||
        object->lightmapCoverageData.size() != uint64_t(texture_desc.width) * texture_desc.height)
    {
        FailLightmapBake("lightmap dimension/coverage validation failed for object " + object_name);
        return false;
    }
    if (!object->lightmap_coverage.IsValid())
    {
        FailLightmapBake("lightmap coverage texture creation failed for object " + object_name);
        return false;
    }
    const wi::graphics::TextureDesc& coverage_desc = object->lightmap_coverage.GetDesc();
    if (coverage_desc.format != wi::graphics::Format::R8_UNORM ||
        coverage_desc.width != texture_desc.width || coverage_desc.height != texture_desc.height)
    {
        FailLightmapBake("lightmap coverage texture dimensions mismatch for object " + object_name);
        return false;
    }

    const wi::scene::ObjectComponent::LightmapStatistics& statistics = object->lightmapStatistics;
    if (lightmap_bake_denoiser_required && !statistics.denoiser_applied)
    {
        FailLightmapBake(
            "production denoiser did not complete for object " + object_name +
            "; previous lightmap package was preserved");
        return false;
    }
    if (statistics.valid_texel_count > 0)
    {
        lightmap_irradiance_min.x = std::min(lightmap_irradiance_min.x, statistics.irradiance_min.x);
        lightmap_irradiance_min.y = std::min(lightmap_irradiance_min.y, statistics.irradiance_min.y);
        lightmap_irradiance_min.z = std::min(lightmap_irradiance_min.z, statistics.irradiance_min.z);
        lightmap_irradiance_max.x = std::max(lightmap_irradiance_max.x, statistics.irradiance_max.x);
        lightmap_irradiance_max.y = std::max(lightmap_irradiance_max.y, statistics.irradiance_max.y);
        lightmap_irradiance_max.z = std::max(lightmap_irradiance_max.z, statistics.irradiance_max.z);
        const float count = static_cast<float>(statistics.valid_texel_count);
        lightmap_irradiance_sum.x += statistics.irradiance_average.x * count;
        lightmap_irradiance_sum.y += statistics.irradiance_average.y * count;
        lightmap_irradiance_sum.z += statistics.irradiance_average.z * count;
        lightmap_valid_texel_count += statistics.valid_texel_count;
    }
    lightmap_missing_texel_count += statistics.missing_texel_count;
    lightmap_invalid_texel_count += statistics.invalid_sample_texel_count;
    lightmap_bake_completed.push_back(entity);
    lightmap_bake_pending_save.erase(lightmap_bake_pending_save.begin());
    return true;
}

void NewPipelineClientRenderPath::UpdateLightmapBakeWorkload(float dt)
{
    const uint64_t active_texels = GetActiveLightmapTexelCount();
    if (active_texels != lightmap_bake_scheduled_active_texels)
    {
        if (active_texels == 0 || lightmap_bake_scheduled_active_texels == 0)
        {
            lightmap_bake_iterations_per_frame = 1;
        }
        else
        {
            const uint64_t scaled =
                (uint64_t(lightmap_bake_iterations_per_frame) * lightmap_bake_scheduled_active_texels +
                    active_texels / 2ull) /
                active_texels;
            lightmap_bake_iterations_per_frame = std::clamp(
                static_cast<uint32_t>(std::max<uint64_t>(scaled, 1ull)),
                1u,
                kMaxClientLightmapIterationsPerFrame);
        }
        lightmap_bake_scheduled_active_texels = active_texels;
        lightmap_bake_adaptation_frames = 0;
    }

    if (lightmap_bake_active.empty())
    {
        lightmap_bake_effective_iterations_per_frame = 1;
        setLightmapRefreshIterationsPerFrame(1);
        return;
    }

    const float frame_time = std::clamp(dt, 0.0f, 0.5f);
    if (lightmap_bake_frame_time_ema <= 0.0f)
        lightmap_bake_frame_time_ema = frame_time;
    else
        lightmap_bake_frame_time_ema +=
            (frame_time - lightmap_bake_frame_time_ema) * kLightmapFrameTimeEMAWeight;

    if (frame_time > kLightmapFrameTimeEmergency)
    {
        lightmap_bake_iterations_per_frame = 1;
        lightmap_bake_adaptation_frames = 0;
    }
    else if (++lightmap_bake_adaptation_frames >= kLightmapAdaptationIntervalFrames)
    {
        lightmap_bake_adaptation_frames = 0;
        if (lightmap_bake_frame_time_ema < kLightmapFrameTimeRampUp)
        {
            lightmap_bake_iterations_per_frame = std::min(
                lightmap_bake_iterations_per_frame * 2u,
                kMaxClientLightmapIterationsPerFrame);
        }
        else if (lightmap_bake_frame_time_ema > kLightmapFrameTimeRampDown)
        {
            lightmap_bake_iterations_per_frame = std::max(lightmap_bake_iterations_per_frame / 2u, 1u);
        }
    }

    uint32_t minimum_remaining = lightmap_bake_settings.sample_count;
    for (const wi::ecs::Entity entity : lightmap_bake_active)
    {
        const wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity);
        if (object != nullptr)
        {
            const uint32_t completed = std::min(object->lightmapIterationCount, lightmap_bake_settings.sample_count);
            minimum_remaining = std::min(minimum_remaining, lightmap_bake_settings.sample_count - completed);
        }
    }
    lightmap_bake_effective_iterations_per_frame = std::clamp(
        std::min(lightmap_bake_iterations_per_frame, std::max(minimum_remaining, 1u)),
        1u,
        kMaxClientLightmapIterationsPerFrame);
    setLightmapRefreshIterationsPerFrame(lightmap_bake_effective_iterations_per_frame);
}

void NewPipelineClientRenderPath::UpdateLightmapBakeProgress()
{
    const uint64_t now = NowUsec();
    const uint64_t total_samples = GetLightmapBakeTotalSamples();
    const uint64_t elapsed = now >= lightmap_bake_rate_time_usec ? now - lightmap_bake_rate_time_usec : 0;
    if (lightmap_bake_rate_time_usec == 0)
    {
        lightmap_bake_rate_time_usec = now;
        lightmap_bake_rate_sample_total = total_samples;
    }
    else if (elapsed >= 500'000)
    {
        const uint64_t sample_delta = total_samples >= lightmap_bake_rate_sample_total
            ? total_samples - lightmap_bake_rate_sample_total
            : 0;
        lightmap_bake_samples_per_second = elapsed > 0
            ? static_cast<float>(double(sample_delta) * 1'000'000.0 / double(elapsed))
            : 0.0f;
        lightmap_bake_rate_time_usec = now;
        lightmap_bake_rate_sample_total = total_samples;
    }

    uint32_t minimum_samples = lightmap_bake_settings.sample_count;
    uint32_t maximum_samples = 0;
    for (const wi::ecs::Entity entity : lightmap_bake_active)
    {
        const wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity);
        if (object == nullptr)
            continue;
        const uint32_t samples = std::min(object->lightmapIterationCount, lightmap_bake_settings.sample_count);
        minimum_samples = std::min(minimum_samples, samples);
        maximum_samples = std::max(maximum_samples, samples);
    }
    if (lightmap_bake_active.empty())
        minimum_samples = lightmap_bake_pending_save.empty() ? 0u : lightmap_bake_settings.sample_count;

    const wi::graphics::GraphicsDevice::MemoryUsage memory = wi::graphics::GetDevice()->GetMemoryUsage();
    lightmap_bake_status =
        "Lightmap: " + std::to_string(lightmap_bake_completed.size()) + "/" +
        std::to_string(lightmap_bake_queue.size()) +
        " active=" + std::to_string(lightmap_bake_active.size()) +
        " pending=" + std::to_string(lightmap_bake_pending_save.size()) +
        " samples=" + std::to_string(minimum_samples) +
        (maximum_samples > minimum_samples ? "-" + std::to_string(maximum_samples) : std::string{}) +
        "/" + std::to_string(lightmap_bake_settings.sample_count) +
        " iter/frame=" + std::to_string(lightmap_bake_effective_iterations_per_frame) +
        " rate=" + std::to_string(static_cast<uint32_t>(std::round(lightmap_bake_samples_per_second))) + " samples/s" +
        " VRAM=" + std::to_string(memory.usage / (1024ull * 1024ull)) + "/" +
            std::to_string(memory.budget / (1024ull * 1024ull)) + "MB";
}

void NewPipelineClientRenderPath::ClearLightmapBakeRequests(bool clear_lightmaps)
{
    const auto clear_entities = [&](const wi::vector<wi::ecs::Entity>& entities) {
        for (const wi::ecs::Entity entity : entities)
        {
            if (wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity))
            {
                if (clear_lightmaps)
                    object->ClearLightmap();
                else
                    object->SetLightmapRenderRequest(false);
            }
        }
    };
    clear_entities(lightmap_bake_active);
    clear_entities(lightmap_bake_pending_save);
}

void NewPipelineClientRenderPath::ResetLightmapBakeScheduling()
{
    setLightmapRefreshIterationsPerFrame(1);
    lightmap_bake_active.clear();
    lightmap_bake_pending_save.clear();
    lightmap_bake_iterations_per_frame = 1;
    lightmap_bake_effective_iterations_per_frame = 1;
    lightmap_bake_adaptation_frames = 0;
    lightmap_bake_scheduled_active_texels = 0;
    lightmap_bake_last_progress_time_usec = 0;
    lightmap_bake_last_progress_sample_total = 0;
    lightmap_bake_frame_time_ema = 0.0f;
    lightmap_bake_samples_per_second = 0.0f;
    lightmap_bake_rate_time_usec = 0;
    lightmap_bake_rate_sample_total = 0;
}

void NewPipelineClientRenderPath::UpdateLightmapBake(float dt)
{
    if (!IsLightmapBakeActive())
        return;
    if (lightmap_cancel_requested)
    {
        ClearLightmapBakeRequests(true);
        RestoreVolumetricLightmapBakeRendererState();
        wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
        ResetLightmapBakeScheduling();
        CleanupLightmapBakeTemps();
        ReloadSceneAfterLightmapBakeAbort();
        std::string source_error;
        const bool source_unchanged = VerifySourceSceneUnchanged(source_error);
        LogLightmapSceneParity("cancelled");
        lightmap_bake_state = LightmapBakeState::Cancelled;
        lightmap_bake_status = source_unchanged
            ? "Lightmap: cancelled; source and previous sidecars preserved"
            : "Lightmap: cancelled; previous sidecars preserved, but " + source_error;
        static_lighting_bake_requested = false;
        lightmap_cancel_requested = false;
        wi::backlog::post(lightmap_bake_status);
        return;
    }
    if (lightmap_bake_state == LightmapBakeState::Preparing)
    {
        PrepareLightmapBake();
        return;
    }
    if (lightmap_bake_state == LightmapBakeState::BakingVolume ||
        lightmap_bake_state == LightmapBakeState::ReadingVolume)
    {
        UpdateVolumetricLightmapBake();
        return;
    }
    if (lightmap_bake_state != LightmapBakeState::Baking)
        return;

    for (size_t i = 0; i < lightmap_bake_active.size();)
    {
        const wi::ecs::Entity entity = lightmap_bake_active[i];
        wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity);
        if (object == nullptr)
        {
            FailLightmapBake("active lightmap object disappeared: " + std::to_string(entity));
            return;
        }
        if (object->lightmapIterationCount >= lightmap_bake_settings.sample_count)
        {
            object->SetLightmapRenderRequest(false);
            lightmap_bake_pending_save.push_back(entity);
            lightmap_bake_active.erase(lightmap_bake_active.begin() + i);
            continue;
        }
        ++i;
    }

    // SaveLightmap performs a synchronous GPU readback and BC6H readback. Keep
    // it to one object per update while the next active batch keeps sampling.
    if (!FinalizeOnePendingLightmap())
        return;

    FillLightmapBakeBatch();
    if (lightmap_bake_active.empty() && lightmap_bake_pending_save.empty() &&
        lightmap_bake_next_index >= lightmap_bake_queue.size())
    {
        BeginVolumetricLightmapBake();
        return;
    }

    const uint64_t progress_now = NowUsec();
    const uint64_t progress_samples = GetLightmapBakeTotalSamples();
    if (lightmap_bake_last_progress_time_usec == 0 ||
        progress_samples > lightmap_bake_last_progress_sample_total)
    {
        lightmap_bake_last_progress_time_usec = progress_now;
        lightmap_bake_last_progress_sample_total = progress_samples;
    }
    else if (!lightmap_bake_active.empty() &&
        progress_now - lightmap_bake_last_progress_time_usec > kLightmapNoProgressTimeoutUsec)
    {
        FailLightmapBake("GPU lightmap work made no progress for 30 seconds; check texture allocation and ray tracing support");
        return;
    }

    UpdateLightmapBakeWorkload(dt);
    UpdateLightmapBakeProgress();
}

void NewPipelineClientRenderPath::BeginVolumetricLightmapBake()
{
    ResetLightmapBakeScheduling();

    const XMFLOAT3 bounds_min = local_scene.bounds.getMin();
    const XMFLOAT3 bounds_max = local_scene.bounds.getMax();
    const float extents[] = {
        bounds_max.x - bounds_min.x,
        bounds_max.y - bounds_min.y,
        bounds_max.z - bounds_min.z,
    };
    const float longest_extent =
        std::max(extents[0], std::max(extents[1], extents[2]));
    if (!std::isfinite(longest_extent) || longest_extent <= 0.001f)
    {
        FailLightmapBake(
            "volumetric lightmap scene bounds are empty or invalid");
        return;
    }

    uint32_t dimensions[3] = {};
    for (size_t axis = 0; axis < 3; ++axis)
    {
        dimensions[axis] = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::lround(
                extents[axis] / longest_extent *
                float(kClientVLMGridLongestAxis))),
            kClientVLMGridMinimumAxis,
            kClientVLMGridLongestAxis);
    }
    auto probe_count = [&]() {
        return static_cast<uint64_t>(dimensions[0]) *
            dimensions[1] * dimensions[2];
    };
    while (probe_count() > kClientVLMMaxProbeCount)
    {
        size_t largest_axis = 3;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            if (dimensions[axis] <= kClientVLMGridMinimumAxis)
                continue;
            if (largest_axis == 3 ||
                dimensions[axis] > dimensions[largest_axis])
                largest_axis = axis;
        }
        if (largest_axis == 3)
        {
            FailLightmapBake(
                "volumetric lightmap probe budget cannot fit scene grid");
            return;
        }
        --dimensions[largest_axis];
    }

    previous_ddgi_enabled = wi::renderer::GetDDGIEnabled();
    previous_ddgi_ray_count = wi::renderer::GetDDGIRayCount();
    previous_ddgi_min_ray_count = wi::renderer::GetDDGIMinRayCount();
    previous_ddgi_instance_inclusion_mask =
        wi::renderer::GetDDGIInstanceInclusionMask();
    previous_ddgi_blend_speed = wi::renderer::GetDDGIBlendSpeed();
    volumetric_renderer_state_saved = true;

    previous_ddgi_scene_state = std::move(local_scene.ddgi);
    local_scene.ddgi = {};
    local_scene.ddgi.grid_dimensions =
        uint3(dimensions[0], dimensions[1], dimensions[2]);
    client_volumetric_lightmap.Clear();
    volumetric_ray_readback = {};
    volumetric_ray_count_readback = {};
    volumetric_readback_requested = false;
    volumetric_readback_scheduled = false;
    volumetric_readback_submit_frame = 0;
    volumetric_readback_error.clear();

    wi::renderer::SetDDGIEnabled(true);
    wi::renderer::SetDDGIRayCount(kClientVLMFinalProjectionRays);
    wi::renderer::SetDDGIMinRayCount(kClientVLMConvergenceMinRays);
    wi::renderer::SetDDGIInstanceInclusionMask(
        wi::renderer::raytracing_inclusion_mask_lightmap);
    wi::renderer::SetDDGIBlendSpeed(0.15f);

    lightmap_bake_state = LightmapBakeState::BakingVolume;
    lightmap_bake_status =
        "Lightmap: baking Client volumetric lightmap " +
        std::to_string(dimensions[0]) + "x" +
        std::to_string(dimensions[1]) + "x" +
        std::to_string(dimensions[2]) +
        " probes (UE Mobile per-primitive runtime contract)";
    wi::backlog::post(lightmap_bake_status);
}

void NewPipelineClientRenderPath::UpdateVolumetricLightmapBake()
{
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    if (lightmap_bake_state == LightmapBakeState::BakingVolume)
    {
        if (!local_scene.ddgi.ray_buffer.IsValid() ||
            !local_scene.ddgi.raycount_buffer.IsValid() ||
            local_scene.ddgi.frame_index < kClientVLMConvergenceFrames)
        {
            lightmap_bake_status =
                "Lightmap: Client volumetric lightmap converging " +
                std::to_string(std::min(
                    local_scene.ddgi.frame_index,
                    kClientVLMConvergenceFrames)) +
                "/" + std::to_string(kClientVLMConvergenceFrames);
            return;
        }

        // The saved L2 projection must use a complete, uniformly distributed
        // ray set. Adaptive DDGI ray counts remain enabled during convergence,
        // then the final frame is explicitly sampled at the production budget.
        wi::renderer::SetDDGIMinRayCount(
            kClientVLMFinalProjectionRays);
        volumetric_readback_requested = true;
        lightmap_bake_state = LightmapBakeState::ReadingVolume;
        lightmap_bake_status =
            "Lightmap: projecting final volumetric SH and reading back";
        return;
    }

    if (!volumetric_readback_scheduled)
        return;

    if (!volumetric_readback_error.empty())
    {
        const std::string error = volumetric_readback_error;
        FailLightmapBake(error);
        return;
    }
    if (device->GetFrameCount() <= volumetric_readback_submit_frame)
        return;

    // The copy was submitted by the host at the end of the previous frame.
    // One terminal bake stall is acceptable and avoids a persistent readback
    // ring or any RenderPath-owned SubmitCommandLists call.
    device->WaitForGPU();
    std::string error;
    if (!CompleteVolumetricLightmapReadback(error))
    {
        FailLightmapBake(error);
        return;
    }
    RestoreVolumetricLightmapBakeRendererState();
    FinishLightmapBake();
}

bool NewPipelineClientRenderPath::CompleteVolumetricLightmapReadback(
    std::string& error)
{
    if (volumetric_ray_readback.mapped_data == nullptr ||
        volumetric_ray_count_readback.mapped_data == nullptr)
    {
        error = "volumetric lightmap GPU readback is not CPU accessible";
        return false;
    }

    const XMUINT3 dimensions(
        local_scene.ddgi.grid_dimensions.x,
        local_scene.ddgi.grid_dimensions.y,
        local_scene.ddgi.grid_dimensions.z);
    const uint64_t probe_count =
        static_cast<uint64_t>(dimensions.x) *
        dimensions.y * dimensions.z;
    if (probe_count == 0 ||
        probe_count > kClientVLMMaxProbeCount)
    {
        error = "volumetric lightmap probe count is invalid";
        return false;
    }
    const uint64_t expected_ray_bytes =
        probe_count * DDGI_MAX_RAYCOUNT *
        sizeof(DDGIRayDataPacked);
    if (volumetric_ray_readback.GetDesc().size <
            expected_ray_bytes ||
        volumetric_ray_count_readback.GetDesc().size < probe_count)
    {
        error = "volumetric lightmap GPU readback is truncated";
        return false;
    }

    ClientVolumetricLightmapData result;
    result.dimensions = dimensions;
    result.bounds_min = XMFLOAT3(
        local_scene.ddgi.grid_min.x,
        local_scene.ddgi.grid_min.y,
        local_scene.ddgi.grid_min.z);
    result.bounds_max = XMFLOAT3(
        local_scene.ddgi.grid_max.x,
        local_scene.ddgi.grid_max.y,
        local_scene.ddgi.grid_max.z);
    result.probes.resize(static_cast<size_t>(probe_count));

    const auto* rays = static_cast<const DDGIRayDataPacked*>(
        volumetric_ray_readback.mapped_data);
    const auto* ray_counts = static_cast<const uint8_t*>(
        volumetric_ray_count_readback.mapped_data);
    constexpr float sqrt_pi = 1.7724538509055160273f;
    constexpr float basis_l0 = 1.0f / (2.0f * sqrt_pi);
    constexpr float basis_l1 =
        1.7320508075688772935f / (2.0f * sqrt_pi);
    constexpr float basis_l2_mn2 =
        3.8729833462074168852f / (2.0f * sqrt_pi);
    constexpr float basis_l2_mn1 =
        3.8729833462074168852f / (2.0f * sqrt_pi);
    constexpr float basis_l2_m0 =
        2.2360679774997896964f / (4.0f * sqrt_pi);
    constexpr float basis_l2_m1 =
        3.8729833462074168852f / (2.0f * sqrt_pi);
    constexpr float basis_l2_m2 =
        3.8729833462074168852f / (4.0f * sqrt_pi);

    for (uint64_t probe_index = 0;
        probe_index < probe_count;
        ++probe_index)
    {
        const uint32_t ray_count = std::min<uint32_t>(
            uint32_t(ray_counts[probe_index]) *
                DDGI_RAY_BUCKET_COUNT,
            DDGI_MAX_RAYCOUNT);
        if (ray_count != kClientVLMFinalProjectionRays)
        {
            error =
                "volumetric lightmap final probe sampling was incomplete";
            return false;
        }

        ClientVolumetricLightmapProbe& destination =
            result.probes[static_cast<size_t>(probe_index)];
        uint32_t valid_rays = 0;
        for (uint32_t ray_index = 0;
            ray_index < ray_count;
            ++ray_index)
        {
            const DDGIRayDataPacked& packed =
                rays[probe_index * DDGI_MAX_RAYCOUNT + ray_index];
            XMFLOAT3 direction(
                wi::math::f16tof32(packed.data.x),
                wi::math::f16tof32(packed.data.x >> 16u),
                wi::math::f16tof32(packed.data.y));
            const XMFLOAT3 radiance(
                wi::math::f16tof32(packed.data.z),
                wi::math::f16tof32(packed.data.z >> 16u),
                wi::math::f16tof32(packed.data.w));
            const float direction_length = std::sqrt(
                direction.x * direction.x +
                direction.y * direction.y +
                direction.z * direction.z);
            if (!std::isfinite(direction_length) ||
                direction_length <= 1e-5f ||
                !std::isfinite(radiance.x) ||
                !std::isfinite(radiance.y) ||
                !std::isfinite(radiance.z))
                continue;
            direction.x /= direction_length;
            direction.y /= direction_length;
            direction.z /= direction_length;

            const float basis[9] = {
                basis_l0,
                basis_l1 * direction.y,
                basis_l1 * direction.z,
                basis_l1 * direction.x,
                basis_l2_mn2 * direction.x * direction.y,
                basis_l2_mn1 * direction.y * direction.z,
                basis_l2_m0 *
                    (3.0f * direction.z * direction.z - 1.0f),
                basis_l2_m1 * direction.x * direction.z,
                basis_l2_m2 *
                    (direction.x * direction.x -
                        direction.y * direction.y),
            };
            for (size_t coefficient = 0;
                coefficient < destination.radiance_sh.size();
                ++coefficient)
            {
                destination.radiance_sh[coefficient].x +=
                    radiance.x * basis[coefficient];
                destination.radiance_sh[coefficient].y +=
                    radiance.y * basis[coefficient];
                destination.radiance_sh[coefficient].z +=
                    radiance.z * basis[coefficient];
            }
            ++valid_rays;
        }
        if (valid_rays < ray_count * 95u / 100u)
        {
            error =
                "volumetric lightmap contains too many invalid final rays";
            return false;
        }
        const float normalization =
            (4.0f * XM_PI) / static_cast<float>(valid_rays);
        for (XMFLOAT3& coefficient : destination.radiance_sh)
        {
            coefficient.x *= normalization;
            coefficient.y *= normalization;
            coefficient.z *= normalization;
        }
    }

    if (!result.IsValid())
    {
        error = "volumetric lightmap projection produced invalid data";
        return false;
    }
    client_volumetric_lightmap = std::move(result);
    volumetric_ray_readback = {};
    volumetric_ray_count_readback = {};
    volumetric_readback_scheduled = false;
    wi::backlog::post(
        "Client volumetric lightmap complete: probes=" +
        std::to_string(client_volumetric_lightmap.probes.size()) +
        " SH=L2 runtime_sampling=CPU-per-primitive");
    return true;
}

void NewPipelineClientRenderPath::
    RestoreVolumetricLightmapBakeRendererState()
{
    volumetric_readback_requested = false;
    volumetric_readback_scheduled = false;
    volumetric_readback_error.clear();
    volumetric_ray_readback = {};
    volumetric_ray_count_readback = {};
    if (!volumetric_renderer_state_saved)
        return;

    wi::renderer::SetDDGIEnabled(previous_ddgi_enabled);
    wi::renderer::SetDDGIRayCount(previous_ddgi_ray_count);
    wi::renderer::SetDDGIMinRayCount(
        previous_ddgi_min_ray_count);
    wi::renderer::SetDDGIInstanceInclusionMask(
        previous_ddgi_instance_inclusion_mask);
    wi::renderer::SetDDGIBlendSpeed(previous_ddgi_blend_speed);
    volumetric_renderer_state_saved = false;
    local_scene.ddgi = std::move(previous_ddgi_scene_state);
    previous_ddgi_scene_state = {};
}

void NewPipelineClientRenderPath::UpdateClientVolumetricLightmapInstances()
{
    visibilityResources.buffer_client_vlm_instances = nullptr;
    visibilityResources.buffer_client_vlm_instances_upload = nullptr;
    if (!render_settings.baked_lightmaps_enabled ||
        !render_settings.dynamic_object_vlm_enabled ||
        !client_volumetric_lightmap.IsValid() ||
        local_scene.instanceArraySize == 0)
        return;

    const uint64_t element_count =
        static_cast<uint64_t>(local_scene.instanceArraySize) *
        kClientVLMFloat4sPerInstance;
    const uint64_t buffer_size =
        element_count * sizeof(XMFLOAT4);
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();

    if (!volumetric_instance_buffer.IsValid() ||
        volumetric_instance_buffer.GetDesc().size != buffer_size)
    {
        wi::graphics::GPUBufferDesc default_desc;
        default_desc.size = buffer_size;
        // visibility_shadeCS consumes this through Buffer<float4>, so this
        // must be a typed texel buffer on DX12, Metal and Vulkan rather than
        // a StructuredBuffer descriptor.
        default_desc.format =
            wi::graphics::Format::R32G32B32A32_FLOAT;
        default_desc.bind_flags =
            wi::graphics::BindFlag::SHADER_RESOURCE;
        volumetric_instance_buffer = {};
        if (!device->CreateBuffer(
                &default_desc,
                nullptr,
                &volumetric_instance_buffer))
            return;
        device->SetName(
            &volumetric_instance_buffer,
            "newpipeline.client.vlm_instances");

        wi::graphics::GPUBufferDesc upload_desc;
        upload_desc.size = buffer_size;
        upload_desc.usage = wi::graphics::Usage::UPLOAD;
        for (size_t index = 0;
            index < volumetric_instance_upload.size();
            ++index)
        {
            volumetric_instance_upload[index] = {};
            if (!device->CreateBuffer(
                    &upload_desc,
                    nullptr,
                    &volumetric_instance_upload[index]))
            {
                volumetric_instance_buffer = {};
                return;
            }
            device->SetName(
                &volumetric_instance_upload[index],
                "newpipeline.client.vlm_instances_upload");
        }
    }

    wi::graphics::GPUBuffer& upload =
        volumetric_instance_upload[device->GetBufferIndex()];
    if (upload.mapped_data == nullptr ||
        upload.GetDesc().size != buffer_size)
        return;
    std::memset(
        upload.mapped_data,
        0,
        static_cast<size_t>(buffer_size));

    auto* destination =
        static_cast<XMFLOAT4*>(upload.mapped_data);
    const size_t object_count = std::min(
        local_scene.objects.GetCount(),
        local_scene.aabb_objects.size());
    for (size_t object_index = 0;
        object_index < object_count;
        ++object_index)
    {
        std::array<XMFLOAT3, 9> radiance_sh = {};
        if (!client_volumetric_lightmap.SampleRadianceSH(
                local_scene.aabb_objects[object_index].getCenter(),
                radiance_sh))
            continue;

        float flattened[28] = {};
        size_t component = 0;
        for (const XMFLOAT3& coefficient : radiance_sh)
        {
            flattened[component++] = std::clamp(
                coefficient.x, -40000.0f, 40000.0f);
            flattened[component++] = std::clamp(
                coefficient.y, -40000.0f, 40000.0f);
            flattened[component++] = std::clamp(
                coefficient.z, -40000.0f, 40000.0f);
        }
        flattened[27] = 1.0f;
        std::memcpy(
            destination +
                object_index * kClientVLMFloat4sPerInstance,
            flattened,
            sizeof(flattened));
    }

    visibilityResources.buffer_client_vlm_instances =
        &volumetric_instance_buffer;
    visibilityResources.buffer_client_vlm_instances_upload =
        &upload;
}

bool NewPipelineClientRenderPath::CommitLightmapBakeFiles(std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path scene_final(ClientLightmapPackage::DerivedScenePathForScene(scene_asset_path));
    const fs::path package_final(ClientLightmapPackage::PackagePathForScene(scene_asset_path));
    const fs::path scene_temp(prepared_scene_temp_path);
    const fs::path package_temp(prepared_package_temp_path);
    const fs::path scene_backup(scene_final.string() + ".backup");
    const fs::path package_backup(package_final.string() + ".backup");
    std::error_code ec;

    if (!fs::is_regular_file(scene_temp, ec) || ec)
    {
        error = "prepared derived scene is missing";
        return false;
    }
    ec.clear();
    if (!fs::is_regular_file(package_temp, ec) || ec)
    {
        error = "prepared lightmap package is missing";
        return false;
    }

    // Recover the previous valid output if an earlier process stopped after
    // moving a sidecar to its backup but before installing the replacement.
    ec.clear();
    if (!fs::exists(scene_final, ec) && fs::exists(scene_backup, ec))
        fs::rename(scene_backup, scene_final, ec);
    if (ec)
    {
        error = "failed to recover previous derived scene: " + ec.message();
        return false;
    }
    ec.clear();
    if (!fs::exists(package_final, ec) && fs::exists(package_backup, ec))
        fs::rename(package_backup, package_final, ec);
    if (ec)
    {
        error = "failed to recover previous lightmap package: " + ec.message();
        return false;
    }

    fs::remove(scene_backup, ec);
    ec.clear();
    fs::remove(package_backup, ec);
    ec.clear();
    const bool had_scene = fs::exists(scene_final, ec) && !ec;
    ec.clear();
    const bool had_package = fs::exists(package_final, ec) && !ec;
    ec.clear();
    if (had_scene)
    {
        fs::rename(scene_final, scene_backup, ec);
        if (ec)
        {
            error = "failed to preserve previous derived scene: " + ec.message();
            return false;
        }
    }
    if (had_package)
    {
        fs::rename(package_final, package_backup, ec);
        if (ec)
        {
            std::error_code rollback_ec;
            if (had_scene)
                fs::rename(scene_backup, scene_final, rollback_ec);
            error = "failed to preserve previous lightmap package: " + ec.message();
            return false;
        }
    }

    fs::rename(scene_temp, scene_final, ec);
    if (!ec)
        fs::rename(package_temp, package_final, ec);
    if (ec)
    {
        std::error_code rollback_ec;
        fs::remove(scene_final, rollback_ec);
        fs::remove(package_final, rollback_ec);
        if (had_scene)
            fs::rename(scene_backup, scene_final, rollback_ec);
        if (had_package)
            fs::rename(package_backup, package_final, rollback_ec);
        error = "failed to commit derived scene/lightmap sidecars: " + ec.message();
        return false;
    }

    if (had_scene)
        fs::remove(scene_backup, ec);
    ec.clear();
    if (had_package)
        fs::remove(package_backup, ec);
    return true;
}

bool NewPipelineClientRenderPath::VerifySourceSceneUnchanged(std::string& error) const
{
    const uint64_t current_hash = ClientLightmapPackage::HashFile(scene_asset_path);
    if (source_scene_hash_before_bake == 0 || current_hash == 0)
    {
        error = "canonical source scene could not be hashed";
        return false;
    }
    if (current_hash != source_scene_hash_before_bake)
    {
        error = "canonical source scene changed during Client lightmap generation";
        return false;
    }
    return true;
}

void NewPipelineClientRenderPath::FinishLightmapBake()
{
    ResetLightmapBakeScheduling();
    lightmap_bake_state = LightmapBakeState::Saving;
    lightmap_bake_status = "Lightmap: saving external package";
    const float reciprocal_valid = lightmap_valid_texel_count > 0
        ? 1.0f / static_cast<float>(lightmap_valid_texel_count)
        : 0.0f;
    if (lightmap_valid_texel_count == 0)
        lightmap_irradiance_min = {};
    wi::backlog::post(
        "Client lightmap irradiance statistics: valid_texels=" + std::to_string(lightmap_valid_texel_count) +
        " missing_texels=" + std::to_string(lightmap_missing_texel_count) +
        " invalid_texels=" + std::to_string(lightmap_invalid_texel_count) +
        " min=" + std::to_string(lightmap_irradiance_min.x) + "," +
            std::to_string(lightmap_irradiance_min.y) + "," + std::to_string(lightmap_irradiance_min.z) +
        " average=" + std::to_string(lightmap_irradiance_sum.x * reciprocal_valid) + "," +
            std::to_string(lightmap_irradiance_sum.y * reciprocal_valid) + "," +
            std::to_string(lightmap_irradiance_sum.z * reciprocal_valid) +
        " max=" + std::to_string(lightmap_irradiance_max.x) + "," +
            std::to_string(lightmap_irradiance_max.y) + "," + std::to_string(lightmap_irradiance_max.z));
    std::string error;
    if (!VerifySourceSceneUnchanged(error))
    {
        FailLightmapBake(error);
        return;
    }
    if (!client_static_lighting.LightmapPackage().Save(
        prepared_package_temp_path,
        source_scene_hash_before_bake,
        prepared_derived_scene_hash,
        local_scene,
        lightmap_bake_completed,
        lightmap_bake_settings,
        client_volumetric_lightmap,
        error))
    {
        wi::backlog::post("Client lightmap package diagnostics: save_failures=1 load_failures=0");
        FailLightmapBake(error);
        return;
    }

    // Validate the exact temporary outputs through the cold-start path before
    // either final sidecar is moved. A malformed archive/package therefore
    // cannot displace the previous known-good pair.
    wi::scene::Scene validation_scene;
    const ClientLightmapPackageResult temp_load_result =
        client_static_lighting.LightmapPackage().LoadFromPaths(
            scene_asset_path,
            prepared_scene_temp_path,
            prepared_package_temp_path,
            validation_scene);
    if (!temp_load_result.success || temp_load_result.loaded_count != lightmap_bake_completed.size())
    {
        wi::backlog::post("Client lightmap package diagnostics: temp_validation_failures=1");
        FailLightmapBake("temporary sidecar validation failed - " + temp_load_result.diagnostic);
        return;
    }
    if (!VerifySourceSceneUnchanged(error))
    {
        FailLightmapBake(error);
        return;
    }
    if (!CommitLightmapBakeFiles(error))
    {
        wi::backlog::post("Client lightmap package diagnostics: commit_failures=1 load_failures=0");
        FailLightmapBake(error);
        return;
    }

    // Exercise the exact cold-start load path before reporting success.  This
    // replaces the transient accumulation textures with the committed BC6H
    // package and catches hash, object-ID, CRC and GPU upload failures now
    // instead of only after the next process launch.
    const ClientLightmapPackageResult load_result =
        client_static_lighting.LoadLightmaps(scene_asset_path, local_scene);
    if (!load_result.success || load_result.loaded_count != lightmap_bake_completed.size())
    {
        wi::backlog::post("Client lightmap package diagnostics: save_failures=0 load_failures=1");
        wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
        render_settings.lightmap_bake_requested = false;
        ResetLightmapBakeScheduling();
        lightmap_bake_state = LightmapBakeState::Failed;
        lightmap_bake_status = "Lightmap: committed but reload verification failed - " + load_result.diagnostic;
        client_static_lighting.SetLightmapStatus(ClientLightingAssetState::Corrupt, lightmap_bake_status);
        static_lighting_bake_requested = false;
        wi::backlog::post(lightmap_bake_status);
        LogLightmapSceneParity("reload verification failed");
        return;
    }
    client_volumetric_lightmap = load_result.volumetric_lightmap;
    scene_source_root_entity = wi::ecs::INVALID_ENTITY;
    environment_probe_entity = wi::ecs::INVALID_ENTITY;
    environment_probe_created_by_client = false;
    environment_probe_load_attempted = false;
    AdvanceSceneGeneration("committed lightmap scene reload");
    ApplyEnvironmentProbeSettings(false);
    if (!VerifySourceSceneUnchanged(error))
    {
        wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
        render_settings.lightmap_bake_requested = false;
        ResetLightmapBakeScheduling();
        lightmap_bake_state = LightmapBakeState::Failed;
        lightmap_bake_status = "Lightmap: sidecars committed but " + error;
        client_static_lighting.SetLightmapStatus(ClientLightingAssetState::Stale, lightmap_bake_status);
        static_lighting_bake_requested = false;
        wi::backlog::post(lightmap_bake_status);
        return;
    }
    wi::backlog::post("Client lightmap package diagnostics: save_failures=0 load_failures=0 loaded=" +
        std::to_string(load_result.loaded_count));
    LogLightmapSceneParity("completed");
    wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
    render_settings.lightmap_bake_requested = false;
    lightmap_bake_state = LightmapBakeState::Completed;
    lightmap_bake_status = "Lightmap: complete and reload-verified " +
        std::to_string(load_result.loaded_count) + " objects -> " +
        ClientLightmapPackage::DerivedScenePathForScene(scene_asset_path) + " + " +
        ClientLightmapPackage::PackagePathForScene(scene_asset_path);
    client_static_lighting.SetLightmapStatus(ClientLightingAssetState::Valid, lightmap_bake_status);
    client_static_lighting.ClearStale();
    baked_sun_state = sun_state;
    baked_sun_reference_valid = true;
    prepared_scene_temp_path.clear();
    prepared_package_temp_path.clear();
    wi::backlog::post(lightmap_bake_status);
    if (static_lighting_bake_requested)
    {
        // The lightmap commit also persisted the probe ID and placement. The
        // following capture is therefore validated against the final scene.
        if (wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity))
            InitializeBlackEnvironmentProbe(*probe);
        RequestReflectionProbeBake();
    }
    else
    {
        client_static_lighting.SetProbeStatus(
            ClientLightingAssetState::Stale,
            "Reflection Probe: STALE scene changed; regenerate Client Lighting");
        if (wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity))
            InitializeBlackEnvironmentProbe(*probe);
    }
}

void NewPipelineClientRenderPath::FailLightmapBake(const std::string& reason)
{
    ClearLightmapBakeRequests(true);
    RestoreVolumetricLightmapBakeRendererState();
    wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
    ResetLightmapBakeScheduling();
    CleanupLightmapBakeTemps();
    ReloadSceneAfterLightmapBakeAbort();
    std::string source_error;
    const bool source_unchanged = VerifySourceSceneUnchanged(source_error);
    LogLightmapSceneParity("failed");
    render_settings.lightmap_bake_requested = false;
    lightmap_bake_state = LightmapBakeState::Failed;
    lightmap_bake_status = "Lightmap: failed - " + reason;
    if (!source_unchanged)
        lightmap_bake_status += "; " + source_error;
    static_lighting_bake_requested = false;
    wi::backlog::post(lightmap_bake_status);
}

void NewPipelineClientRenderPath::CleanupLightmapBakeTemps()
{
    std::error_code ec;
    if (!prepared_scene_temp_path.empty())
        std::filesystem::remove(prepared_scene_temp_path, ec);
    ec.clear();
    if (!prepared_package_temp_path.empty())
        std::filesystem::remove(prepared_package_temp_path, ec);
}

void NewPipelineClientRenderPath::ReloadSceneAfterLightmapBakeAbort()
{
    if (scene_asset_path.empty())
        return;
    environment_probe_entity = wi::ecs::INVALID_ENTITY;
    environment_probe_created_by_client = false;
    const SceneInitializationResult result = InitializeDefaultScene(local_scene);
    if (!result.loaded_asset_path.empty())
        scene_asset_path = result.loaded_asset_path;
    scene_source_root_entity = result.loaded_root_entity;
    ApplySunStateToScene(local_scene, sun_state);
    const ClientLightmapPackageResult package_result =
        client_static_lighting.LoadLightmaps(scene_asset_path, local_scene);
    client_volumetric_lightmap =
        package_result.success
            ? package_result.volumetric_lightmap
            : ClientVolumetricLightmapData{};
    if (package_result.scene_replaced)
        scene_source_root_entity = wi::ecs::INVALID_ENTITY;
    wi::backlog::post(package_result.diagnostic);
    lightmap_bake_status = client_static_lighting.GetLightmapStatus();
    AdvanceSceneGeneration("lightmap abort scene reload");
    ApplyEnvironmentProbeSettings(false);
    if (baked_sun_reference_valid && !SunMatches(sun_state, baked_sun_state))
    {
        client_static_lighting.MarkStale("runtime sun differs from baked sun");
        lightmap_bake_status = client_static_lighting.GetLightmapStatus();
        reflection_probe_status = client_static_lighting.GetProbeStatus();
        client_static_lighting.DisableLightmaps(local_scene);
        if (wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity))
            InitializeBlackEnvironmentProbe(*probe);
    }
}

void NewPipelineClientRenderPath::LogLightmapSceneParity(const char* phase)
{
    if (!lightmap_bake_scene_fingerprint_valid)
        return;

    const SceneParityFingerprint current = ComputeSceneParityFingerprint(local_scene);
    const bool matches = current.hash == lightmap_bake_scene_fingerprint.hash;
    wi::backlog::post(
        std::string{"Client lightmap scene parity ["} + phase + "]: " +
        (matches ? "MATCH " : "MISMATCH expected=0x") +
        (matches ? FormatSceneParityFingerprint(current) :
            [&]() {
                std::ostringstream stream;
                stream << std::hex << lightmap_bake_scene_fingerprint.hash << " actual="
                       << FormatSceneParityFingerprint(current);
                return stream.str();
            }()));
    lightmap_bake_scene_fingerprint_valid = false;
}

void NewPipelineClientRenderPath::UpdateLocalCamera(float dt)
{
    if (!input_active)
    {
        camera_control_start = true;
        wi::input::HidePointer(false);
        local_camera.UpdateCamera();
        return;
    }
    const bool mouse_look_down = IsDown(wi::input::MOUSE_BUTTON_MIDDLE) || IsDown(wi::input::MOUSE_BUTTON_RIGHT);
    const bool gui_interacting = GetGUI().IsTyping() ||
        (GetGUI().HasFocus() && !mouse_look_down && IsDown(wi::input::MOUSE_BUTTON_LEFT));
    if (gui_interacting)
    {
        if (!mouse_look_down)
        {
            camera_control_start = true;
            wi::input::HidePointer(false);
        }
        local_camera.UpdateCamera();
        return;
    }

    const float base_speed = IsDown(wi::input::KEYBOARD_BUTTON_LSHIFT) || IsDown(wi::input::KEYBOARD_BUTTON_RSHIFT) ? 12.0f : 4.0f;
    const float move_speed = base_speed * dt;

    if (camera_control_start)
    {
        camera_control_origin = wi::input::GetPointer();
    }

    if (mouse_look_down)
    {
        camera_control_start = false;
        const wi::input::MouseState mouse = wi::input::GetMouseState();
        camera_rotation.x += wi::math::DegreesToRadians(mouse.delta_position.y * 0.1f);
        camera_rotation.y += wi::math::DegreesToRadians(mouse.delta_position.x * 0.1f);
        camera_rotation.x = std::max(wi::math::DegreesToRadians(-89.0f), std::min(wi::math::DegreesToRadians(89.0f), camera_rotation.x));
        wi::input::SetPointer(camera_control_origin);
        wi::input::HidePointer(true);
    }
    else
    {
        camera_control_start = true;
        wi::input::HidePointer(false);
    }

    wi::scene::TransformComponent transform;
    transform.ClearTransform();
    transform.RotateRollPitchYaw(camera_rotation);
    transform.UpdateTransform();

    XMFLOAT3 forward = transform.GetForward();
    XMFLOAT3 right = transform.GetRight();

    if (IsCharacterDown('W') || IsDown(wi::input::KEYBOARD_BUTTON_UP))
    {
        camera_position.x += forward.x * move_speed;
        camera_position.y += forward.y * move_speed;
        camera_position.z += forward.z * move_speed;
    }
    if (IsCharacterDown('S') || IsDown(wi::input::KEYBOARD_BUTTON_DOWN))
    {
        camera_position.x -= forward.x * move_speed;
        camera_position.y -= forward.y * move_speed;
        camera_position.z -= forward.z * move_speed;
    }
    if (IsCharacterDown('D') || IsDown(wi::input::KEYBOARD_BUTTON_RIGHT))
    {
        camera_position.x += right.x * move_speed;
        camera_position.y += right.y * move_speed;
        camera_position.z += right.z * move_speed;
    }
    if (IsCharacterDown('A') || IsDown(wi::input::KEYBOARD_BUTTON_LEFT))
    {
        camera_position.x -= right.x * move_speed;
        camera_position.y -= right.y * move_speed;
        camera_position.z -= right.z * move_speed;
    }

    transform.ClearTransform();
    transform.Translate(camera_position);
    transform.RotateRollPitchYaw(camera_rotation);
    transform.UpdateTransform();

    local_camera.TransformCamera(transform);
    local_camera.UpdateCamera();
}

bool NewPipelineClientRenderPath::IsControlPacketChanged(const ClientControlPacket& packet) const
{
    if (!has_published_control_packet)
        return true;

    const ClientControlPacket& previous = last_published_control_packet;
    return packet.viewport_width != previous.viewport_width ||
        packet.viewport_height != previous.viewport_height ||
        packet.scene_generation != previous.scene_generation ||
        !NearlyEqual(packet.near_plane, previous.near_plane) ||
        !NearlyEqual(packet.far_plane, previous.far_plane) ||
        !NearlyEqual(packet.eye, previous.eye) ||
        !NearlyEqual(packet.at, previous.at) ||
        !NearlyEqual(packet.up, previous.up) ||
        !NearlyEqual(packet.view, previous.view) ||
        !NearlyEqual(packet.projection, previous.projection) ||
        packet.sun_enabled != previous.sun_enabled ||
        !NearlyEqual(packet.sun_direction, previous.sun_direction) ||
        !NearlyEqual(packet.sun_color, previous.sun_color) ||
        !NearlyEqual(packet.sun_intensity, previous.sun_intensity) ||
        !NearlyEqual(packet.ambient, previous.ambient) ||
        !NearlyEqual(packet.horizon, previous.horizon) ||
        !NearlyEqual(packet.zenith, previous.zenith) ||
        packet.supported_protocol_versions != previous.supported_protocol_versions ||
        packet.supported_quality_tiers != previous.supported_quality_tiers ||
        packet.supported_encoding_profiles != previous.supported_encoding_profiles ||
        packet.preferred_protocol_version != previous.preferred_protocol_version ||
        packet.preferred_quality_tier != previous.preferred_quality_tier;
}

void NewPipelineClientRenderPath::PublishControlPacket(float dt)
{
    control_publish_accumulator += dt;
    ClientControlPacket packet = MakeControlPacketFromCameraAndScene(local_camera, local_scene, frame_id + 1, scene_generation);
    packet.preferred_quality_tier = config.remote_quality_tier;
    packet.control_frame_id = has_published_control_packet
        ? last_published_control_packet.control_frame_id
        : packet.frame_id;
    const bool changed = IsControlPacketChanged(packet);
    const float publish_interval = changed ? kControlDirtyPublishIntervalSeconds : kControlHeartbeatIntervalSeconds;
    if (control_publish_accumulator < publish_interval)
        return;

    control_publish_accumulator = 0.0f;
    packet.frame_id = ++frame_id;
    // frame_id is the heartbeat/transport sequence. control_frame_id changes
    // only when the camera, viewport, scene or lighting state changes, so
    // returned semantics can refer to one exact Client GBuffer history entry
    // across unchanged heartbeats and region-cadenced video frames.
    packet.control_frame_id = changed || !has_published_control_packet
        ? packet.frame_id
        : last_published_control_packet.control_frame_id;
    packet.timestamp_usec = NowUsec();

    if (!control_publish_logged)
    {
        wi::backlog::post(
            "Client WebRTC control DataChannel publish active: client->server only");
        control_publish_logged = true;
    }

    if (!webrtc_transport.SendControl(packet))
        return;

    last_published_control_packet = packet;
    has_published_control_packet = true;
}

bool NewPipelineClientRenderPath::UploadRemoteVideoTextures(
    const RetainedI420Frame& frame, const RemoteVideoFrameLayout& layout)
{
    if (!frame.IsValid() || !layout.metadata.valid ||
        frame.width != layout.video_width || frame.height != layout.video_height)
        return false;

    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    RemoteVideoUploadSlot& slot = remote_video_upload_ring[device->GetBufferIndex()];
    if (slot.width != frame.width || slot.height != frame.height ||
        !slot.upload_y.IsValid() || !slot.upload_uv.IsValid() || !slot.gpu_y.IsValid() || !slot.gpu_uv.IsValid())
    {
        slot = {};
        wi::graphics::TextureDesc y_desc;
        y_desc.width = frame.width;
        y_desc.height = frame.height;
        y_desc.format = wi::graphics::Format::R8_UNORM;
        y_desc.usage = wi::graphics::Usage::UPLOAD;
        y_desc.bind_flags = wi::graphics::BindFlag::NONE;
        y_desc.layout = wi::graphics::ResourceState::COPY_SRC;
        if (!device->CreateTexture(&y_desc, nullptr, &slot.upload_y))
            return false;
        y_desc.usage = wi::graphics::Usage::DEFAULT;
        y_desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
        y_desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        if (!device->CreateTexture(&y_desc, nullptr, &slot.gpu_y))
            return false;

        wi::graphics::TextureDesc uv_desc;
        uv_desc.width = frame.width / 2u;
        uv_desc.height = frame.height / 2u;
        uv_desc.format = wi::graphics::Format::R8G8_UNORM;
        uv_desc.usage = wi::graphics::Usage::UPLOAD;
        uv_desc.bind_flags = wi::graphics::BindFlag::NONE;
        uv_desc.layout = wi::graphics::ResourceState::COPY_SRC;
        if (!device->CreateTexture(&uv_desc, nullptr, &slot.upload_uv))
            return false;
        uv_desc.usage = wi::graphics::Usage::DEFAULT;
        uv_desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
        uv_desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        if (!device->CreateTexture(&uv_desc, nullptr, &slot.gpu_uv))
            return false;

        slot.width = frame.width;
        slot.height = frame.height;
        device->SetName(&slot.upload_y, "newpipeline.client.remote.upload_y");
        device->SetName(&slot.upload_uv, "newpipeline.client.remote.upload_uv");
        device->SetName(&slot.gpu_y, "newpipeline.client.remote.gpu_y");
        device->SetName(&slot.gpu_uv, "newpipeline.client.remote.gpu_uv");
    }

    if (slot.upload_y.mapped_subresources == nullptr || slot.upload_y.mapped_subresource_count == 0 ||
        slot.upload_uv.mapped_subresources == nullptr || slot.upload_uv.mapped_subresource_count == 0)
        return false;
    const wi::graphics::SubresourceData& upload_y = slot.upload_y.mapped_subresources[0];
    const wi::graphics::SubresourceData& upload_uv = slot.upload_uv.mapped_subresources[0];
    for (uint32_t row = 0; row < frame.height; ++row)
    {
        std::memcpy(static_cast<uint8_t*>(const_cast<void*>(upload_y.data_ptr)) +
                static_cast<size_t>(row) * upload_y.row_pitch,
            frame.y_plane + static_cast<size_t>(row) * frame.y_stride, frame.width);
    }
    for (uint32_t row = 0; row < frame.height / 2u; ++row)
    {
        uint8_t* destination = static_cast<uint8_t*>(const_cast<void*>(upload_uv.data_ptr)) +
            static_cast<size_t>(row) * upload_uv.row_pitch;
        const uint8_t* source_u = frame.u_plane + static_cast<size_t>(row) * frame.u_stride;
        const uint8_t* source_v = frame.v_plane + static_cast<size_t>(row) * frame.v_stride;
        for (uint32_t x = 0; x < frame.width / 2u; ++x)
        {
            destination[x * 2u + 0u] = source_u[x];
            destination[x * 2u + 1u] = source_v[x];
        }
    }
    remote_gpu_upload_bytes += static_cast<uint64_t>(frame.width) * frame.height * 3u / 2u;

    uint32_t uploaded_mask = 0;
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        if (!tile.available)
            continue;
        const size_t semantic_index = static_cast<size_t>(tile.semantic);
        if (semantic_index >= slot.semantic_outputs.size())
            return false;
        const bool hdr = tile.encoding == RemoteBufferEncoding::LogHDR16F;
        const wi::graphics::Format format = hdr
            ? wi::graphics::Format::R16G16B16A16_FLOAT
            : wi::graphics::Format::R8G8B8A8_UNORM;
        wi::graphics::Texture& output = slot.semantic_outputs[semantic_index];
        if (!output.IsValid() || output.GetDesc().width != tile.width ||
            output.GetDesc().height != tile.height || output.GetDesc().format != format)
        {
            wi::graphics::TextureDesc desc;
            desc.width = tile.width;
            desc.height = tile.height;
            desc.format = format;
            desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
            desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
            output = {};
            if (!device->CreateTexture(&desc, nullptr, &output))
                return false;
            const std::string name = std::string{"newpipeline.client."} + ToString(tile.semantic) +
                (hdr ? ".gpu.rgba16f" : ".gpu.rgba8");
            device->SetName(&output, name.c_str());
            ++remote_texture_creation_count;
        }
        uploaded_mask |= RemoteBufferKindMask(tile.semantic);
    }
    if ((uploaded_mask & static_cast<uint32_t>(RemoteBufferKind::IndirectDiffuse)) == 0)
        return false;

    wi::graphics::CommandList cmd = device->BeginCommandList();
    wi::graphics::GPUBarrier copy_barriers[] = {
        wi::graphics::GPUBarrier::Image(
            &slot.gpu_y, slot.gpu_y.GetDesc().layout, wi::graphics::ResourceState::COPY_DST),
        wi::graphics::GPUBarrier::Image(
            &slot.gpu_uv, slot.gpu_uv.GetDesc().layout, wi::graphics::ResourceState::COPY_DST),
    };
    device->Barrier(copy_barriers, arraysize(copy_barriers), cmd);
    device->CopyResource(&slot.gpu_y, &slot.upload_y, cmd);
    device->CopyResource(&slot.gpu_uv, &slot.upload_uv, cmd);
    for (wi::graphics::GPUBarrier& barrier : copy_barriers)
        std::swap(barrier.image.layout_before, barrier.image.layout_after);
    device->Barrier(copy_barriers, arraysize(copy_barriers), cmd);

    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        if (!tile.available)
            continue;
        const size_t semantic_index = static_cast<size_t>(tile.semantic);
        wi::renderer::YUV_to_RGB_Region(
            slot.gpu_y,
            slot.gpu_uv,
            slot.semantic_outputs[semantic_index],
            XMUINT2(tile.origin_x, tile.origin_y),
            tile.encoding == RemoteBufferEncoding::ScalarLuma8,
            tile.encoding == RemoteBufferEncoding::LogHDR16F ? 16.0f : 0.0f,
            cmd);
    }
    // The upload/unpack command list was opened before RenderPath3D records the
    // visibility pass which consumes these textures. Wicked submits both lists
    // in BeginCommandList order at the application frame boundary. Do not call
    // SubmitCommandLists() from a RenderPath: it advances the global buffered
    // frame index and would split one application frame into multiple resource
    // lifetimes.
    accepted_remote_textures = slot.semantic_outputs;
    accepted_remote_buffer_mask = uploaded_mask;
    return true;
}

bool NewPipelineClientRenderPath::ValidateRemoteVideoLayout(
    const RemoteVideoFrameLayout& layout, std::string& reason) const
{
    const RemoteFrameMetadata& metadata = layout.metadata;
    if (layout.protocol_version != kRemoteVideoWireVersionV3)
    {
        reason = "unsupported remote video protocol";
        return false;
    }
    if (layout.encoding_profile_id != kRemoteEncodingProfileI420V3 ||
            layout.source_control_frame_id == 0 ||
            layout.layout_checksum == 0 ||
            layout.descriptor_checksum == 0 ||
            !ValidateRemoteFrameContractV3(layout.contract_v3, nullptr) ||
            layout.contract_v3.source_control_frame_id != layout.source_control_frame_id ||
            layout.contract_v3.atlas_width != layout.video_width ||
            layout.contract_v3.atlas_height != layout.video_height)
    {
        reason = "invalid V3 descriptor contract";
        return false;
    }
    if (negotiated_stream_selection_valid &&
        (layout.protocol_version !=
                negotiated_stream_selection.protocol_version ||
            layout.encoding_profile_id !=
                negotiated_stream_selection.encoding_profile_id ||
            layout.quality_tier !=
                negotiated_stream_selection.quality_tier))
    {
        reason = "video frame does not match acknowledged negotiation";
        return false;
    }
    if (metadata.source_stream_id != kRemoteFrameStreamId)
    {
        reason = "unexpected stream id";
        return false;
    }
    if (!metadata.valid || metadata.width == 0 || metadata.height == 0)
    {
        reason = "metadata invalid or empty resolution";
        return false;
    }
    if (metadata.dynamic_range != RemoteDynamicRange::HDR)
    {
        reason = "unexpected dynamic range";
        return false;
    }
    if ((metadata.continuity_mask & static_cast<uint32_t>(RemoteBufferKind::IndirectDiffuse)) == 0)
    {
        reason = "missing GI continuity bit";
        return false;
    }
    const uint64_t now = NowUsec();
    if (metadata.local_receive_timestamp_usec == 0 ||
        metadata.local_receive_timestamp_usec + kMaxRemoteFrameAgeUsec < now)
    {
        reason = "stale locally received video frame";
        return false;
    }
    const RemoteVideoTileLayout& indirect =
        layout.tiles[static_cast<size_t>(RemoteBufferSemantic::RemoteIndirectDiffuse)];
    if (!indirect.available || indirect.width == 0 || indirect.height == 0)
    {
        reason = "missing indirect diffuse video tile";
        return false;
    }
    uint32_t observed_available_mask = 0;
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        const RemoteBufferSemantic expected_semantic =
            static_cast<RemoteBufferSemantic>(index);
        const RemoteBufferEncoding expected_encoding =
            RemoteBufferTransportEncoding(expected_semantic);
        if (tile.semantic != expected_semantic || tile.encoding != expected_encoding)
        {
            reason = std::string{"semantic/encoding contract mismatch at slot "} +
                std::to_string(index);
            return false;
        }
        if (!tile.available)
            continue;
        observed_available_mask |= RemoteBufferKindMask(tile.semantic);
        if (tile.width == 0 || tile.height == 0 ||
            tile.origin_x + tile.width > layout.video_width ||
            tile.origin_y + tile.height > layout.video_height)
        {
            reason = std::string{"invalid video tile for "} + ToString(tile.semantic);
            return false;
        }
    }
    if (observed_available_mask != metadata.available_buffer_mask ||
        (metadata.continuity_mask & observed_available_mask) != observed_available_mask)
    {
        reason = "video tile availability mask mismatch";
        return false;
    }
    if (metadata.confidence < 0.5f)
    {
        reason = "placeholder confidence";
        return false;
    }
    return true;
}

void NewPipelineClientRenderPath::InvalidateRemote(const std::string& reason)
{
    if (remote_consume.accepted_valid || remote_consume.invalid_reason != reason)
    {
        wi::backlog::post("Client remote invalidated: " + reason);
    }

    remote_consume.accepted_valid = false;
    remote_consume.invalid_reason = reason;
    elastic_remote_quality = 0.0f;
    elastic_remote_gi_weight = 0.0f;
    elastic_remote_ao_weight = 0.0f;
    elastic_remote_specular_weight = 0.0f;
    elastic_remote_shadow_weight = 0.0f;
    ApplyElasticLightingResources();
}

void NewPipelineClientRenderPath::AcceptRemoteVideoFrame(
    const RetainedI420Frame& frame, const RemoteVideoFrameLayout& layout)
{
    if (!UploadRemoteVideoTextures(frame, layout))
    {
        InvalidateRemote("GPU video upload/unpack failed");
        return;
    }
    accepted_remote_contract_v3_valid = true;
    accepted_remote_contract_v3 = layout.contract_v3;
    accepted_remote_source_control_frame_id =
        layout.source_control_frame_id;
    CommitAcceptedRemoteMetadata(layout.metadata);
}

void NewPipelineClientRenderPath::CommitAcceptedRemoteMetadata(const RemoteFrameMetadata& metadata)
{

    if (remote_consume.accepted_valid && remote_consume.accepted_generation == metadata.source_generation)
    {
        remote_consume.history_frame_id = remote_consume.accepted_frame_id;
        remote_consume.history_valid = metadata.history_valid;
    }
    else
    {
        remote_consume.history_frame_id = 0;
        remote_consume.history_valid = false;
    }

    remote_consume.accepted_frame_id = metadata.frame_id;
    remote_consume.accepted_generation = metadata.source_generation;
    remote_consume.width = metadata.width;
    remote_consume.height = metadata.height;
    remote_consume.confidence = metadata.confidence;
    remote_consume.accepted_valid = true;
    accepted_remote_metadata = metadata;
    remote_ddgi_frame_index = metadata.ddgi_frame_index;
    remote_ddgi_reset_reason = metadata.ddgi_reset_reason;
    remote_consume.placeholder = false;
    remote_consume.stale_timer = 0.0f;
    remote_consume.stale_logged = false;
    remote_consume.invalid_reason.clear();

    wi::backlog::post("Client remote accepted frame " + std::to_string(remote_consume.accepted_frame_id) +
        " generation " + std::to_string(remote_consume.accepted_generation) + " " +
        std::to_string(remote_consume.width) + "x" + std::to_string(remote_consume.height) +
        " confidence=" + std::to_string(remote_consume.confidence));
}

void NewPipelineClientRenderPath::AcquireRemoteVideoFrame(float dt)
{
    if (!remote_acquire_logged)
    {
        wi::backlog::post(
            "Client WebRTC video acquire active: retained I420 plus paired np.frame_meta validation");
        remote_acquire_logged = true;
    }

    RetainedI420Frame retained_frame;
    RemoteVideoFrameLayout video_layout;
    std::string error;
    std::string last_decode_error;
    bool had_decode_error = false;
    bool received = false;
    bool queued_video_without_metadata = false;
    for (size_t receive_count = 0; receive_count < kMaxPendingRemotePairs;
        ++receive_count)
    {
        RetainedI420Frame acquired_frame;
        if (!webrtc_transport.TryAcquireI420Frame(acquired_frame))
            break;
        RemoteVideoFrameLayout pixel_layout;
        if (!DecodeRemoteVideoFrameLayout(
                acquired_frame, pixel_layout, &error))
        {
            had_decode_error = true;
            last_decode_error = error.empty()
                ? "video metadata-band decode failed" : error;
            wi::backlog::post(
                "Client dropped malformed decoded video frame: " +
                last_decode_error);
            error.clear();
            continue;
        }
        error.clear();
        auto duplicate = std::find_if(
            pending_remote_video_frames.begin(), pending_remote_video_frames.end(),
            [&pixel_layout](const PendingRemoteVideoFrame& pending) {
                return pending.pixel_layout.metadata.frame_id ==
                        pixel_layout.metadata.frame_id &&
                    pending.pixel_layout.metadata.source_generation ==
                        pixel_layout.metadata.source_generation;
            });
        if (duplicate != pending_remote_video_frames.end())
            pending_remote_video_frames.erase(duplicate);
        PendingRemoteVideoFrame pending;
        pending.frame = std::move(acquired_frame);
        pending.pixel_layout = std::move(pixel_layout);
        pending.local_receive_timestamp_usec = NowUsec();
        pending_remote_video_frames.push_back(std::move(pending));
        queued_video_without_metadata = true;
        while (pending_remote_video_frames.size() > kMaxPendingRemotePairs)
        {
            pending_remote_video_frames.pop_front();
            ++downstream_pair_expirations;
        }
    }

    PrunePendingRemoteFrames(NowUsec());
    received = TryMatchRemoteVideoFrame(retained_frame, video_layout);
    if (!received)
    {
        if (queued_video_without_metadata)
            ++downstream_metadata_misses;
        if (!pending_remote_video_frames.empty() &&
            !downstream_metadata_cache.empty())
        {
            if (!remote_consume.accepted_valid)
                remote_consume.invalid_reason =
                    "waiting for matching pixel-band frame identity";
        }
        else if (!pending_remote_video_frames.empty())
        {
            if (!remote_consume.accepted_valid)
                remote_consume.invalid_reason = "waiting for frame metadata";
        }
        else if (!downstream_metadata_cache.empty() &&
            !remote_consume.accepted_valid)
        {
            remote_consume.invalid_reason = "waiting for matching video frame";
        }

        if (had_decode_error && !remote_consume.accepted_valid &&
            pending_remote_video_frames.empty() &&
            downstream_metadata_cache.empty())
        {
            InvalidateRemote(last_decode_error);
        }
        else if (remote_consume.accepted_valid)
        {
            remote_consume.stale_timer += dt;
            if (remote_consume.stale_timer > kRemoteStaleTimeoutSeconds && !remote_consume.stale_logged)
            {
                remote_consume.stale_logged = true;
                InvalidateRemote("stale remote video frame > 5s");
            }
        }
        else if (!remote_consume.no_remote_logged &&
            pending_remote_video_frames.empty() && downstream_metadata_cache.empty())
        {
            wi::backlog::post(
                "Client WebRTC video track: no frame yet, using local scene.");
            remote_consume.no_remote_logged = true;
        }
        return;
    }

    RemoteVideoFrameLayout pixel_layout;
    const bool agrees =
        DecodeRemoteVideoFrameLayout(retained_frame, pixel_layout, &error) &&
        video_layout.protocol_version == pixel_layout.protocol_version &&
        video_layout.video_width == pixel_layout.video_width &&
        video_layout.video_height == pixel_layout.video_height &&
        video_layout.metadata.frame_id == pixel_layout.metadata.frame_id &&
        video_layout.metadata.source_generation ==
            pixel_layout.metadata.source_generation &&
        video_layout.descriptor_checksum ==
            pixel_layout.descriptor_checksum &&
        video_layout.source_control_frame_id ==
            pixel_layout.source_control_frame_id;
    if (!agrees)
    {
        ++downstream_metadata_mismatches;
        InvalidateRemote("pixel-band/frame-metadata mismatch");
        return;
    }

    const RemoteFrameMetadata& metadata = video_layout.metadata;
    const bool same_latest = metadata.frame_id == remote_consume.latest_frame_id &&
        metadata.source_generation == remote_consume.latest_generation;
    if (same_latest)
    {
        if (!remote_unchanged_skip_logged)
        {
            wi::backlog::post("Client packed-video skipped unchanged frame without rereading payload.");
            remote_unchanged_skip_logged = true;
        }
        if (remote_consume.accepted_valid)
        {
            remote_consume.stale_timer += dt;
            if (remote_consume.stale_timer > kRemoteStaleTimeoutSeconds && !remote_consume.stale_logged)
            {
                remote_consume.stale_logged = true;
                InvalidateRemote("stale remote video frame > 5s");
            }
        }
        return;
    }

    if (remote_consume.latest_generation == metadata.source_generation &&
        remote_consume.latest_frame_id != 0 && metadata.frame_id <= remote_consume.latest_frame_id)
    {
        ++downstream_out_of_order_drops;
        return;
    }

    const bool generation_reset =
        metadata.reset_this_frame ||
        (remote_consume.latest_generation != 0 && metadata.source_generation != remote_consume.latest_generation);
    if (generation_reset)
    {
        remote_consume.history_frame_id = 0;
        remote_consume.history_valid = false;
        remote_consume.accepted_valid = false;
        wi::backlog::post("Client remote generation reset: generation " +
            std::to_string(metadata.source_generation) +
            " frame " + std::to_string(metadata.frame_id));
    }

    remote_consume.latest_frame_id = metadata.frame_id;
    remote_consume.latest_generation = metadata.source_generation;
    remote_consume.stale_timer = 0.0f;
    remote_consume.no_remote_logged = false;

    std::string validation_reason;
    if (!remote_payload_read_logged)
    {
        const uint64_t i420_bytes =
            static_cast<uint64_t>(retained_frame.width) *
            retained_frame.height * 3u / 2u;
        wi::backlog::post(
            "Client WebRTC retained I420 frame: " +
            std::to_string(i420_bytes) +
            " bytes, zero bridge copy, GPU semantic unpack");
        remote_payload_read_logged = true;
    }

    if (!ValidateRemoteVideoLayout(video_layout, validation_reason))
    {
        remote_consume.placeholder = validation_reason == "placeholder confidence";
        remote_consume.confidence = metadata.confidence;
        wi::backlog::post("Client remote rejected payload frame " + std::to_string(metadata.frame_id) +
            ": " + validation_reason);
        InvalidateRemote(validation_reason);
        return;
    }

    remote_consume.placeholder_logged = false;
    AcceptRemoteVideoFrame(retained_frame, video_layout);
}

const wi::graphics::Texture* NewPipelineClientRenderPath::GetDebugPreviewTexture() const
{
    switch (debug_preview_mode)
    {
    case DebugPreviewMode::GBufferDepth:
        return depthBuffer_Copy.IsValid() ? &depthBuffer_Copy : nullptr;
    case DebugPreviewMode::GBufferNormalRoughness:
    case DebugPreviewMode::GBufferNormalXY:
    case DebugPreviewMode::GBufferRoughness:
        return visibilityResources.texture_normal_roughness.IsValid() ? &visibilityResources.texture_normal_roughness : nullptr;
    case DebugPreviewMode::LocalIndirectFinalInput:
        return local_indirect_final_input.IsValid() ? &local_indirect_final_input : nullptr;
    case DebugPreviewMode::LocalAO:
        return local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr;
    case DebugPreviewMode::LocalSpecularIndirect:
        return local_specular_indirect.IsValid() ? &local_specular_indirect : nullptr;
    case DebugPreviewMode::LocalSpecularIndirectPreAO:
        return local_specular_indirect_pre_ao.IsValid() ? &local_specular_indirect_pre_ao : nullptr;
    case DebugPreviewMode::LocalReflectionProbe:
        if (const wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity))
            return probe->texture.IsValid() ? &probe->texture : nullptr;
        return nullptr;
    case DebugPreviewMode::LocalShadowVisibility:
        return local_primary_light_visibility.IsValid() ? &local_primary_light_visibility : nullptr;
    case DebugPreviewMode::RemoteIndirectDiffuse:
    {
        const RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteIndirectDiffuse;
        const size_t index = static_cast<size_t>(semantic);
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(semantic)) &&
            accepted_remote_textures[index].IsValid() ? &accepted_remote_textures[index] : nullptr;
    }
    case DebugPreviewMode::RemoteAO:
    {
        const RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteAO;
        const size_t index = static_cast<size_t>(semantic);
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(semantic)) &&
            accepted_remote_textures[index].IsValid() ? &accepted_remote_textures[index] : nullptr;
    }
    case DebugPreviewMode::RemoteSpecularIndirect:
    {
        const RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteSpecularIndirect;
        const size_t index = static_cast<size_t>(semantic);
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(semantic)) &&
            accepted_remote_textures[index].IsValid() ? &accepted_remote_textures[index] : nullptr;
    }
    case DebugPreviewMode::RemoteShadowVisibility:
    {
        const RemoteBufferSemantic semantic = RemoteBufferSemantic::RemoteShadowVisibility;
        const size_t index = static_cast<size_t>(semantic);
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(semantic)) &&
            accepted_remote_textures[index].IsValid() ? &accepted_remote_textures[index] : nullptr;
    }
    case DebugPreviewMode::ElasticIndirectDiffuse:
        return elastic_indirect_diffuse.IsValid() ? &elastic_indirect_diffuse : nullptr;
    case DebugPreviewMode::ElasticAO:
        return elastic_ao.IsValid() ? &elastic_ao : nullptr;
    case DebugPreviewMode::ElasticSpecularIndirectPreAO:
        return elastic_specular_indirect_pre_ao.IsValid()
            ? &elastic_specular_indirect_pre_ao : nullptr;
    case DebugPreviewMode::ElasticPrimaryLightVisibility:
        return elastic_primary_light_visibility.IsValid()
            ? &elastic_primary_light_visibility : nullptr;
    case DebugPreviewMode::Final:
    default:
        return nullptr;
    }
}

void NewPipelineClientRenderPath::Compose(wi::graphics::CommandList cmd) const
{
    if (debug_preview_mode == DebugPreviewMode::Final)
    {
        wi::RenderPath3D::Compose(cmd);
        return;
    }
    if (const wi::graphics::Texture* debug_texture = GetDebugPreviewTexture())
    {
        wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
        const wi::graphics::ResourceState source_layout =
            debug_texture->GetDesc().layout;
        const bool source_needs_pixel_state =
            source_layout != wi::graphics::ResourceState::SHADER_RESOURCE;
        if (source_needs_pixel_state)
        {
            device->Barrier(wi::graphics::GPUBarrier::Image(
                debug_texture,
                source_layout,
                wi::graphics::ResourceState::SHADER_RESOURCE), cmd);
        }
        wi::image::Params fx;
        fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
        // Keep explicit single-buffer views pixel-exact.
        fx.quality = wi::image::QUALITY_NEAREST;
        fx.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
        fx.enableFullScreen();
        if (debug_preview_mode == DebugPreviewMode::LocalReflectionProbe &&
            reflection_probe_debug_mip < reflection_probe_mip_subresources.size())
        {
            fx.image_subresource = reflection_probe_mip_subresources[reflection_probe_debug_mip];
        }
        if (debug_preview_mode == DebugPreviewMode::GBufferNormalXY)
            fx.color = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f);
        else if (debug_preview_mode == DebugPreviewMode::GBufferRoughness)
            fx.color = XMFLOAT4(0.0f, 0.0f, 4.0f, 1.0f);
        else if (debug_preview_mode == DebugPreviewMode::LocalIndirectFinalInput ||
            debug_preview_mode == DebugPreviewMode::LocalSpecularIndirect ||
            debug_preview_mode == DebugPreviewMode::LocalSpecularIndirectPreAO ||
            debug_preview_mode == DebugPreviewMode::LocalReflectionProbe ||
            debug_preview_mode == DebugPreviewMode::RemoteIndirectDiffuse ||
            debug_preview_mode == DebugPreviewMode::RemoteSpecularIndirect ||
            debug_preview_mode == DebugPreviewMode::ElasticIndirectDiffuse ||
            debug_preview_mode ==
                DebugPreviewMode::ElasticSpecularIndirectPreAO)
            fx.enableDebugTonemap();
        if (debug_preview_mode == DebugPreviewMode::LocalAO ||
            debug_preview_mode == DebugPreviewMode::LocalShadowVisibility ||
            debug_preview_mode == DebugPreviewMode::RemoteAO ||
            debug_preview_mode == DebugPreviewMode::RemoteShadowVisibility ||
            debug_preview_mode == DebugPreviewMode::ElasticAO ||
            debug_preview_mode ==
                DebugPreviewMode::ElasticPrimaryLightVisibility)
            fx.enableExtractChannelR();
        wi::image::Draw(debug_texture, fx, cmd);
        if (source_needs_pixel_state)
        {
            device->Barrier(wi::graphics::GPUBarrier::Image(
                debug_texture,
                wi::graphics::ResourceState::SHADER_RESOURCE,
                source_layout), cmd);
        }
        wi::RenderPath2D::Compose(cmd);
        return;
    }

    if (!debug_preview_invalid_logged)
    {
        wi::backlog::post(std::string{"Client debug preview unavailable: "} +
            ToString(debug_preview_mode));
        debug_preview_invalid_logged = true;
    }

    DrawUnavailablePreview(cmd);
}

void NewPipelineClientRenderPath::DrawUnavailablePreview(wi::graphics::CommandList cmd) const
{
    wi::image::Params image;
    image.blendFlag = wi::enums::BLENDMODE_OPAQUE;
    image.enableFullScreen();
    wi::image::Draw(wi::texturehelper::getBlack(), image, cmd);
    wi::font::Params text;
    text.position = XMFLOAT3(GetLogicalWidth() * 0.5f, GetLogicalHeight() * 0.5f, 0);
    text.h_align = wi::font::WIFALIGN_CENTER;
    text.v_align = wi::font::WIFALIGN_CENTER;
    text.size = 28;
    text.color = wi::Color::Red();
    text.shadowColor = wi::Color::Black();
    wi::font::Draw(std::string{"UNAVAILABLE: "} + ToString(debug_preview_mode), text, cmd);
    wi::RenderPath2D::Compose(cmd);
}
} // namespace wicked_newpipeline
