#include "NewPipelineClientRenderPath.h"

#include "wiArchive.h"
#include "wiHelper.h"
#include "wiImage.h"
#include "wiFont.h"
#include "wiTextureHelper.h"

#include <chrono>
#include <cmath>
#include <filesystem>
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
constexpr float kControlDirtyPublishIntervalSeconds = 1.0f / 30.0f;
constexpr float kControlHeartbeatIntervalSeconds = 1.0f / 5.0f;
constexpr const char* kClientEnvironmentProbeName = "NewPipelineEnvironmentProbe";
constexpr uint32_t kMobileShadow2DResolution = 1024;
constexpr uint32_t kMobileShadowCubeResolution = 512;
constexpr uint32_t kMobileLightmapResolution = 256;

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

std::string EnabledString(bool value)
{
    return value ? "enabled" : "disabled";
}
} // namespace

void NewPipelineClientRenderPath::SetRuntimeConfig(const RuntimeConfig& value)
{
    config        = value;
    switch (config.remote_debug_mode)
    {
    case RemoteDebugMode::Raw:
        debug_preview_mode = DebugPreviewMode::RemoteIndirectDiffuse;
        break;
    case RemoteDebugMode::DebugComposite:
        debug_preview_mode = DebugPreviewMode::RemoteOverview;
        break;
    case RemoteDebugMode::Local:
    default:
        debug_preview_mode = DebugPreviewMode::Final;
        break;
    }
    status_logged = false;
    remote_acquire_logged = false;
}

void NewPipelineClientRenderPath::SetSunState(const NewPipelineSunState& value)
{
    sun_state = value;
    sun_state.direction = NormalizeDirectionOrDefault(sun_state.direction);
    if (scene_initialized)
        ApplySunStateToScene(local_scene, sun_state);
}

void NewPipelineClientRenderPath::SetDebugPreviewMode(DebugPreviewMode mode)
{
    debug_preview_mode = mode;
    setDDGIOutputDebugPreview(wi::RenderPath3D::DDGIOutputDebugPreview::Disabled);
    debug_preview_invalid_logged = false;
    wi::backlog::post(std::string{"Client debug preview mode: "} + ToString(debug_preview_mode));
}

std::string NewPipelineClientRenderPath::GetEffectiveAlgorithmSummary() const
{
    return std::string{render_settings.shadow_maps_enabled ? "Shadow Map 1024/512" : "Shadow Map off"} +
        " | " + (render_settings.ssao_enabled ? "SSAO" : "SSAO off") +
        " | " + (render_settings.baked_lightmaps_enabled ? "Baked Lightmap" : "Baked Lightmap off") +
        " | " + (render_settings.environment_probe_enabled ? "Static Probe 128" : "Static Probe off") +
        " | local DDGI/RT/SSR off";
}

std::string NewPipelineClientRenderPath::GetDebugStatusSummary() const
{
    return GetEffectiveAlgorithmSummary() + "\n" + lightmap_bake_status + "\nRemote decoded: " +
        (remote_consume.accepted_valid ? std::string{"available"} : std::string{"unavailable"}) +
        "\nRemote DDGI: frame " + std::to_string(remote_ddgi_frame_index) +
        (remote_consume.history_valid ? " converged" : " warming") +
        " reset=" + ToString(remote_ddgi_reset_reason);
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
}

void NewPipelineClientRenderPath::Start()
{
    InitializeSceneIfNeeded();
    ConfigureLowEndLocalRendering();
    setVisibilitySurfaceResourcesForced(true);
    ApplyRenderSettings(true);
    wi::RenderPath3D::Start();
    if (config.remote_source == RemoteSourceMode::WebRTC)
    {
        std::string error;
        if (!webrtc_transport.Start(false, config, &error))
            wi::backlog::post("Client WebRTC start failed: " + error);
    }

    wi::backlog::post("NewPipeline_Wicked Client render path started.");
    wi::backlog::post(std::string{"Client remote source: "} + ToString(config.remote_source));
    wi::backlog::post(std::string{"Client remote debug mode: "} + ToString(config.remote_debug_mode));
    wi::backlog::post(std::string{"Client debug preview mode: "} + ToString(debug_preview_mode));
    if (config.remote_debug_mode == RemoteDebugMode::DebugComposite)
    {
        wi::backlog::post("Client remote debug composite is a preview mode, not final material GI composite.");
    }
    status_logged = true;
}

void NewPipelineClientRenderPath::ResizeBuffers()
{
    wi::RenderPath3D::ResizeBuffers();
    local_ao_snapshot = {};
    local_lightmap_irradiance = {};
    if (rtAO.IsValid())
    {
        wi::graphics::TextureDesc desc = rtAO.GetDesc();
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_ao_snapshot);
        wi::graphics::GetDevice()->SetName(&local_ao_snapshot, "newpipeline.client.local_ao_snapshot");
    }
    if (visibilityResources.texture_normal_roughness.IsValid())
    {
        wi::graphics::TextureDesc desc = visibilityResources.texture_normal_roughness.GetDesc();
        desc.format = wi::graphics::Format::R16G16B16A16_FLOAT;
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        if (wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_lightmap_irradiance))
        {
            wi::graphics::GetDevice()->SetName(&local_lightmap_irradiance, "newpipeline.client.local_lightmap_irradiance");
            visibilityResources.texture_lightmap_irradiance = &local_lightmap_irradiance;
        }
    }
}

void NewPipelineClientRenderPath::RenderAO(wi::graphics::CommandList cmd) const
{
    wi::RenderPath3D::RenderAO(cmd);
    if (rtAO.IsValid() && local_ao_snapshot.IsValid())
        wi::renderer::CopyTexture2D(local_ao_snapshot, rtAO, cmd);
}

void NewPipelineClientRenderPath::Update(float dt)
{
    InitializeSceneIfNeeded();
    UpdateLightmapBake();
    UpdateLocalCamera(dt);
    MaintainWebRTC(dt);
    PublishControlPacket(dt);

    wi::RenderPath3D::Update(dt);
    AcquireRemoteVideoFrame(dt);

    if (!status_logged)
    {
        wi::backlog::post(std::string{"Client remote source: "} + ToString(config.remote_source));
        wi::backlog::post(std::string{"Client remote debug mode: "} + ToString(config.remote_debug_mode));
        if (config.remote_debug_mode == RemoteDebugMode::DebugComposite)
        {
            wi::backlog::post("Client remote debug composite is a preview mode, not final material GI composite.");
        }
        status_logged = true;
    }
}

void NewPipelineClientRenderPath::MaintainWebRTC(float dt)
{
    webrtc_transport.Tick();
    if (config.remote_source != RemoteSourceMode::WebRTC)
        return;
    const WebRTCTransportStats stats = webrtc_transport.GetStats();
    if (stats.state != WebRTCTransportState::Failed)
    {
        webrtc_retry_accumulator = 0.0f;
        return;
    }
    webrtc_retry_accumulator += dt;
    if (webrtc_retry_accumulator < 2.0f)
        return;
    webrtc_retry_accumulator = 0.0f;
    std::string error;
    if (!webrtc_transport.Start(false, config, &error))
        wi::backlog::post("Client WebRTC retry failed: " + error);
    else
        wi::backlog::post("Client WebRTC retrying signaling: " + config.signaling_url);
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

    scene_asset_path = result.loaded_asset_path;
    scene_source_root_entity = result.loaded_root_entity;
    if (!scene_asset_path.empty())
    {
        const ClientLightmapPackageResult package_result = client_lightmap_package.Load(scene_asset_path, local_scene);
        wi::backlog::post(package_result.diagnostic);
    }
    else
    {
        ClientLightmapPackage::ClearSceneLightmaps(local_scene);
        wi::backlog::post("Client Lightmap package unavailable: procedural scene has no persistent source asset");
    }

    scene_initialized = true;
    ApplyEnvironmentProbeSettings(false);
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
    wi::renderer::SetRaytracedShadowsEnabled(false);
    wi::renderer::SetScreenSpaceShadowsEnabled(false);
    if (log_changes)
    {
        wi::backlog::post(std::string{"Client local shadows (raster Shadow Map 2D 1024 / Cube 512): "} +
            EnabledString(render_settings.shadow_maps_enabled));
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
        if (environment_probe_entity != wi::ecs::INVALID_ENTITY)
        {
            local_scene.Entity_Remove(environment_probe_entity);
            environment_probe_entity = wi::ecs::INVALID_ENTITY;
            environment_probe_created_by_client = false;
        }
        if (log_changes)
        {
            wi::backlog::post("Client environment probe: disabled");
        }
        return;
    }

    if (environment_probe_entity == wi::ecs::INVALID_ENTITY)
    {
        environment_probe_entity = local_scene.Entity_CreateEnvironmentProbe(kClientEnvironmentProbeName, XMFLOAT3(0.0f, 3.0f, 0.0f));
        environment_probe_created_by_client = true;
    }

    if (wi::scene::EnvironmentProbeComponent* probe = local_scene.probes.GetComponent(environment_probe_entity))
    {
        probe->resolution = 128;
        probe->SetRealTime(false);
        probe->SetMSAA(false);
        probe->SetDirty(true);
    }
    if (wi::scene::TransformComponent* transform = local_scene.transforms.GetComponent(environment_probe_entity))
    {
        transform->ClearTransform();
        transform->Translate(XMFLOAT3(0.0f, 3.0f, 0.0f));
        transform->Scale(XMFLOAT3(18.0f, 8.0f, 18.0f));
        transform->UpdateTransform();
    }

    if (log_changes)
    {
        wi::backlog::post("Client environment probe: enabled");
    }
}

void NewPipelineClientRenderPath::ApplyBakedLightmapSettings(bool previous_enabled, bool log_changes, bool force_log)
{
    if (!scene_initialized)
        return;

    const bool changed = previous_enabled != render_settings.baked_lightmaps_enabled;
    if (changed && render_settings.baked_lightmaps_enabled)
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
    saved_lightmaps.clear();
    for (size_t i = 0; i < local_scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = local_scene.objects[i];
        if (object.lightmap.IsValid())
        {
            SavedLightmap saved;
            saved.entity = local_scene.objects.GetEntity(i);
            saved.width = object.lightmapWidth;
            saved.height = object.lightmapHeight;
            saved.texture = object.lightmap;
            saved_lightmaps.push_back(std::move(saved));
        }

        object.SetLightmapRenderRequest(false);
        object.lightmap = {};
        object.lightmap_render = {};
        object.lightmapTextureData.clear();
    }
}

void NewPipelineClientRenderPath::RestoreBakedLightmaps()
{
    for (const SavedLightmap& saved : saved_lightmaps)
    {
        wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(saved.entity);
        if (object == nullptr)
            continue;

        object->lightmapWidth = saved.width;
        object->lightmapHeight = saved.height;
        object->lightmapTextureData.clear();
        object->lightmap = saved.texture;
        object->lightmap_render = {};
    }
    saved_lightmaps.clear();
}

bool NewPipelineClientRenderPath::ObjectSupportsLightmapBake(const wi::scene::ObjectComponent& object) const
{
    if (!object.IsRenderable() || object.IsDynamic() || object.meshID == wi::ecs::INVALID_ENTITY ||
        (object.GetFilterMask() & wi::enums::FILTER_TRANSPARENT) != 0)
        return false;

    const wi::scene::MeshComponent* mesh = local_scene.meshes.GetComponent(object.meshID);
    if (mesh == nullptr || !mesh->IsRenderable() || mesh->IsDynamic() || mesh->IsSkinned())
        return false;
    for (const wi::scene::MeshComponent::MeshSubset& subset : mesh->subsets)
    {
        const wi::scene::MaterialComponent* material = local_scene.materials.GetComponent(subset.materialID);
        if (material != nullptr && (material->GetFilterMask() & wi::enums::FILTER_TRANSPARENT) != 0)
            return false;
    }
    return true;
}

void NewPipelineClientRenderPath::RequestLightmapBake()
{
    if (IsLightmapBakeActive())
    {
        wi::backlog::post("Client lightmap bake request ignored: a bake is already active");
        return;
    }
    if (!scene_initialized || scene_asset_path.empty())
    {
        render_settings.lightmap_bake_requested = false;
        lightmap_bake_state = LightmapBakeState::Failed;
        lightmap_bake_status = "Lightmap: unavailable (no persistent .wiscene)";
        wi::backlog::post(lightmap_bake_status);
        return;
    }
    render_settings.baked_lightmaps_enabled = true;
    render_settings.lightmap_bake_requested = true;
    previous_raytrace_bounce_count = wi::renderer::GetRaytraceBounceCount();
    lightmap_cancel_requested = false;
    lightmap_bake_state = LightmapBakeState::Preparing;
    lightmap_bake_status = "Lightmap: preparing atlas and scene metadata";
    wi::backlog::post("Client lightmap bake requested: " + scene_asset_path);
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

    const bool recreate_client_probe = environment_probe_created_by_client &&
        environment_probe_entity != wi::ecs::INVALID_ENTITY;
    if (recreate_client_probe)
    {
        local_scene.Entity_Remove(environment_probe_entity);
        environment_probe_entity = wi::ecs::INVALID_ENTITY;
        environment_probe_created_by_client = false;
    }

    {
        wi::Archive archive(path, false);
        if (!archive.IsOpen())
        {
            error = "cannot create prepared scene archive: " + path;
            if (recreate_client_probe)
                ApplyEnvironmentProbeSettings(false);
            return false;
        }
        local_scene.Serialize(archive);
        archive.Close();
    }

    if (recreate_client_probe)
        ApplyEnvironmentProbeSettings(false);
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

    lightmap_bake_queue.clear();
    lightmap_bake_completed.clear();
    lightmap_bake_dimensions.clear();
    lightmap_bake_next_index = 0;
    lightmap_bake_skipped = 0;
    active_lightmap_bake_entity = wi::ecs::INVALID_ENTITY;
    prepared_scene_hash = 0;

    std::unordered_map<wi::ecs::Entity, XMUINT2> mesh_dimensions;
    std::unordered_set<std::string> object_ids;
    uint32_t skipped_logged = 0;
    for (size_t i = 0; i < local_scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = local_scene.objects[i];
        const wi::ecs::Entity entity = local_scene.objects.GetEntity(i);
        if (!ObjectSupportsLightmapBake(object))
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
                if (skipped_logged++ < 8)
                    wi::backlog::post("Client lightmap atlas skipped object " + std::to_string(entity) + ": " + atlas_error);
                continue;
            }
            mesh_dimensions[object.meshID] = dimensions;
        }
        else
        {
            if (object.lightmapWidth > 0 && object.lightmapHeight > 0)
                dimensions = {object.lightmapWidth, object.lightmapHeight};
            mesh_dimensions[object.meshID] = dimensions;
        }

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

    if (lightmap_bake_queue.empty())
    {
        FailLightmapBake("no eligible static opaque objects were found");
        return false;
    }

    prepared_scene_temp_path = scene_asset_path + ".clientlightmap.scene.tmp";
    prepared_package_temp_path = ClientLightmapPackage::PackagePathForScene(scene_asset_path) + ".tmp";
    CleanupLightmapBakeTemps();
    std::string save_error;
    if (!SavePreparedScene(prepared_scene_temp_path, save_error))
    {
        FailLightmapBake(save_error);
        return false;
    }
    prepared_scene_hash = ClientLightmapPackage::HashFile(prepared_scene_temp_path);
    if (prepared_scene_hash == 0)
    {
        FailLightmapBake("failed to hash prepared scene");
        return false;
    }

    previous_raytrace_bounce_count = wi::renderer::GetRaytraceBounceCount();
    wi::renderer::SetRaytraceBounceCount(lightmap_bake_settings.bounce_count);
    local_scene.SetAccelerationStructureUpdateRequested(true);
    lightmap_bake_state = LightmapBakeState::Baking;
    wi::backlog::post("Client lightmap bake prepared: objects=" + std::to_string(lightmap_bake_queue.size()) +
        " skipped=" + std::to_string(lightmap_bake_skipped) +
        " samples=" + std::to_string(lightmap_bake_settings.sample_count) +
        " bounces=" + std::to_string(lightmap_bake_settings.bounce_count));
    return StartNextLightmapBake();
}

bool NewPipelineClientRenderPath::StartNextLightmapBake()
{
    while (lightmap_bake_next_index < lightmap_bake_queue.size())
    {
        const wi::ecs::Entity entity = lightmap_bake_queue[lightmap_bake_next_index++];
        wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity);
        const auto dimensions = lightmap_bake_dimensions.find(entity);
        if (object == nullptr || dimensions == lightmap_bake_dimensions.end())
            continue;

        object->ClearLightmap();
        object->lightmapWidth = dimensions->second.x;
        object->lightmapHeight = dimensions->second.y;
        object->SetLightmapDisableBlockCompression(false);
        object->SetLightmapRenderRequest(true);
        active_lightmap_bake_entity = entity;

        const wi::scene::NameComponent* name = local_scene.names.GetComponent(entity);
        const std::string object_name = name != nullptr && !name->name.empty() ? name->name : std::to_string(entity);
        lightmap_bake_status = "Lightmap: " + std::to_string(lightmap_bake_completed.size()) + "/" +
            std::to_string(lightmap_bake_queue.size()) + " baking " + object_name + " (0/" +
            std::to_string(lightmap_bake_settings.sample_count) + ")";
        return true;
    }

    FinishLightmapBake();
    return lightmap_bake_state == LightmapBakeState::Completed;
}

void NewPipelineClientRenderPath::UpdateLightmapBake()
{
    if (!IsLightmapBakeActive())
        return;
    if (lightmap_cancel_requested)
    {
        if (wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(active_lightmap_bake_entity))
            object->ClearLightmap();
        wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
        CleanupLightmapBakeTemps();
        ReloadSceneAfterLightmapBakeAbort();
        active_lightmap_bake_entity = wi::ecs::INVALID_ENTITY;
        lightmap_bake_state = LightmapBakeState::Cancelled;
        lightmap_bake_status = "Lightmap: cancelled; original scene/package preserved";
        lightmap_cancel_requested = false;
        wi::backlog::post(lightmap_bake_status);
        return;
    }
    if (lightmap_bake_state == LightmapBakeState::Preparing)
    {
        PrepareLightmapBake();
        return;
    }
    if (lightmap_bake_state != LightmapBakeState::Baking)
        return;

    wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(active_lightmap_bake_entity);
    if (object == nullptr)
    {
        FailLightmapBake("active lightmap object disappeared");
        return;
    }
    const uint32_t samples = object->lightmapIterationCount;
    const wi::scene::NameComponent* name = local_scene.names.GetComponent(active_lightmap_bake_entity);
    const std::string object_name = name != nullptr && !name->name.empty() ? name->name : std::to_string(active_lightmap_bake_entity);
    lightmap_bake_status = "Lightmap: " + std::to_string(lightmap_bake_completed.size()) + "/" +
        std::to_string(lightmap_bake_queue.size()) + " baking " + object_name + " (" +
        std::to_string(std::min(samples, lightmap_bake_settings.sample_count)) + "/" +
        std::to_string(lightmap_bake_settings.sample_count) + ")";
    if (samples < lightmap_bake_settings.sample_count)
        return;

    object->SetLightmapRenderRequest(false);
    object->SaveLightmap();
    if (!object->lightmap.IsValid() || object->lightmapTextureData.empty() ||
        object->lightmap.GetDesc().format != wi::graphics::Format::BC6H_UF16)
    {
        FailLightmapBake("BC6H compression failed for object " + object_name);
        return;
    }
    lightmap_bake_completed.push_back(active_lightmap_bake_entity);
    active_lightmap_bake_entity = wi::ecs::INVALID_ENTITY;
    StartNextLightmapBake();
}

bool NewPipelineClientRenderPath::CommitLightmapBakeFiles(std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path scene_final(scene_asset_path);
    const fs::path package_final(ClientLightmapPackage::PackagePathForScene(scene_asset_path));
    const fs::path scene_temp(prepared_scene_temp_path);
    const fs::path package_temp(prepared_package_temp_path);
    const fs::path scene_backup(scene_asset_path + ".clientlightmap.backup");
    const fs::path package_backup(package_final.string() + ".backup");
    std::error_code ec;

    fs::remove(scene_backup, ec);
    ec.clear();
    fs::remove(package_backup, ec);
    ec.clear();
    fs::rename(scene_final, scene_backup, ec);
    if (ec)
    {
        error = "failed to back up source scene: " + ec.message();
        return false;
    }
    const bool had_package = fs::exists(package_final, ec) && !ec;
    ec.clear();
    if (had_package)
    {
        fs::rename(package_final, package_backup, ec);
        if (ec)
        {
            fs::rename(scene_backup, scene_final, ec);
            error = "failed to back up previous lightmap package";
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
        fs::rename(scene_backup, scene_final, rollback_ec);
        if (had_package)
            fs::rename(package_backup, package_final, rollback_ec);
        error = "failed to commit prepared scene/lightmap package: " + ec.message();
        return false;
    }

    fs::remove(scene_backup, ec);
    ec.clear();
    if (had_package)
        fs::remove(package_backup, ec);
    return true;
}

void NewPipelineClientRenderPath::FinishLightmapBake()
{
    lightmap_bake_state = LightmapBakeState::Saving;
    lightmap_bake_status = "Lightmap: saving external package";
    std::string error;
    if (!client_lightmap_package.Save(
        prepared_package_temp_path,
        prepared_scene_hash,
        local_scene,
        lightmap_bake_completed,
        lightmap_bake_settings,
        error))
    {
        FailLightmapBake(error);
        return;
    }
    if (!CommitLightmapBakeFiles(error))
    {
        FailLightmapBake(error);
        return;
    }

    for (wi::ecs::Entity entity : lightmap_bake_completed)
    {
        if (wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(entity))
            object->lightmapTextureData.clear();
    }
    wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
    render_settings.lightmap_bake_requested = false;
    lightmap_bake_state = LightmapBakeState::Completed;
    lightmap_bake_status = "Lightmap: complete " + std::to_string(lightmap_bake_completed.size()) +
        " objects -> " + ClientLightmapPackage::PackagePathForScene(scene_asset_path);
    prepared_scene_temp_path.clear();
    prepared_package_temp_path.clear();
    wi::backlog::post(lightmap_bake_status);
}

void NewPipelineClientRenderPath::FailLightmapBake(const std::string& reason)
{
    if (wi::scene::ObjectComponent* object = local_scene.objects.GetComponent(active_lightmap_bake_entity))
        object->SetLightmapRenderRequest(false);
    wi::renderer::SetRaytraceBounceCount(previous_raytrace_bounce_count);
    CleanupLightmapBakeTemps();
    ReloadSceneAfterLightmapBakeAbort();
    render_settings.lightmap_bake_requested = false;
    active_lightmap_bake_entity = wi::ecs::INVALID_ENTITY;
    lightmap_bake_state = LightmapBakeState::Failed;
    lightmap_bake_status = "Lightmap: failed - " + reason;
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
    const ClientLightmapPackageResult package_result = client_lightmap_package.Load(scene_asset_path, local_scene);
    wi::backlog::post(package_result.diagnostic);
    ApplyEnvironmentProbeSettings(false);
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
        packet.sun_enabled != previous.sun_enabled ||
        !NearlyEqual(packet.sun_direction, previous.sun_direction) ||
        !NearlyEqual(packet.sun_color, previous.sun_color) ||
        !NearlyEqual(packet.sun_intensity, previous.sun_intensity) ||
        !NearlyEqual(packet.ambient, previous.ambient) ||
        !NearlyEqual(packet.horizon, previous.horizon) ||
        !NearlyEqual(packet.zenith, previous.zenith);
}

void NewPipelineClientRenderPath::PublishControlPacket(float dt)
{
    control_publish_accumulator += dt;
    ClientControlPacket packet = MakeControlPacketFromCameraAndScene(local_camera, local_scene, frame_id + 1, scene_generation);
    const bool changed = IsControlPacketChanged(packet);
    const float publish_interval = changed ? kControlDirtyPublishIntervalSeconds : kControlHeartbeatIntervalSeconds;
    if (control_publish_accumulator < publish_interval)
        return;

    control_publish_accumulator = 0.0f;
    packet.frame_id = ++frame_id;
    packet.timestamp_usec = NowUsec();

    if (!mock_control_publish_logged)
    {
        wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
            ? "Client file mock control publish active: " + mock_control_mailbox.GetRootDirectory() + " dirty<=30fps heartbeat=5fps"
            : "Client WebRTC control DataChannel publish active: client->server only");
        mock_control_publish_logged = true;
    }

    std::string error;
    const bool published = config.remote_source == RemoteSourceMode::Mock
        ? mock_control_mailbox.PublishLatest(packet, &error)
        : webrtc_transport.SendControl(packet);
    if (!published)
    {
        if (config.remote_source == RemoteSourceMode::Mock && !error.empty())
            wi::backlog::post("Client file mock control publish failed: " + error);
        return;
    }

    last_published_control_packet = packet;
    has_published_control_packet = true;
}

bool NewPipelineClientRenderPath::UploadRemoteTextures(const RemoteRawFrame& frame)
{
    if (!frame.metadata.valid)
        return false;
    std::array<wi::graphics::Texture, static_cast<size_t>(RemoteBufferSemantic::Count)> uploaded;
    uint32_t uploaded_mask = 0;
    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteRawBuffer& buffer = frame.buffers[index];
        if (!buffer.available)
            continue;
        const size_t element_count = static_cast<size_t>(buffer.width) * buffer.height * 4u;
        const bool hdr = buffer.encoding == RemoteBufferEncoding::LogHDR16F;
        if (buffer.width == 0 || buffer.height == 0 ||
            (hdr ? buffer.payload_rgba16f.size() != element_count : buffer.payload_rgba8.size() != element_count))
            return false;

        wi::graphics::TextureDesc desc;
        desc.type = wi::graphics::TextureDesc::Type::TEXTURE_2D;
        desc.width = buffer.width;
        desc.height = buffer.height;
        desc.format = hdr ? wi::graphics::Format::R16G16B16A16_FLOAT : wi::graphics::Format::R8G8B8A8_UNORM;
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE;
        wi::vector<wi::graphics::SubresourceData> subresources;
        void* texture_data = hdr
            ? static_cast<void*>(const_cast<uint16_t*>(buffer.payload_rgba16f.data()))
            : static_cast<void*>(const_cast<uint8_t*>(buffer.payload_rgba8.data()));
        wi::graphics::CreateTextureSubresourceDatas(desc, texture_data, subresources);
        if (!wi::graphics::GetDevice()->CreateTexture(&desc, subresources.data(), &uploaded[index]))
            return false;
        const std::string name = std::string{"newpipeline.client."} + ToString(buffer.semantic) +
            (hdr ? ".rgba16f" : ".rgba8");
        wi::graphics::GetDevice()->SetName(&uploaded[index], name.c_str());
        uploaded_mask |= RemoteBufferKindMask(buffer.semantic);
    }
    if ((uploaded_mask & static_cast<uint32_t>(RemoteBufferKind::IndirectDiffuse)) == 0)
        return false;
    accepted_remote_textures = std::move(uploaded);
    accepted_remote_buffer_mask = uploaded_mask;
    return true;
}

bool NewPipelineClientRenderPath::ValidateRemoteFrame(const RemoteRawFrame& frame, std::string& reason) const
{
    const RemoteFrameMetadata& metadata = frame.metadata;
    if (metadata.source_stream_id != kRemoteFrameStreamId)
    {
        reason = "unexpected stream id";
        return false;
    }
    if (!metadata.valid)
    {
        reason = "metadata invalid flag";
        return false;
    }
    if (metadata.width == 0 || metadata.height == 0)
    {
        reason = "empty resolution";
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

    const RemoteRawBuffer* indirect = frame.FindBuffer(RemoteBufferSemantic::RemoteIndirectDiffuse);
    if (indirect == nullptr || !indirect->available || indirect->width == 0 || indirect->height == 0)
    {
        reason = "missing indirect diffuse video tile";
        return false;
    }
    for (const RemoteRawBuffer& buffer : frame.buffers)
    {
        if (!buffer.available)
            continue;
        const size_t expected_size = static_cast<size_t>(buffer.width) * buffer.height * 4u;
        const bool valid_size = buffer.encoding == RemoteBufferEncoding::LogHDR16F
            ? buffer.payload_rgba16f.size() == expected_size
            : buffer.payload_rgba8.size() == expected_size;
        if (!valid_size)
        {
            reason = std::string{"payload size mismatch for "} + ToString(buffer.semantic);
            return false;
        }
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
    if (remote_consume.accepted_valid || remote_consume.fallback_reason != reason)
    {
        wi::backlog::post("Client remote fallback: " + reason);
    }

    remote_consume.accepted_valid = false;
    remote_consume.fallback_reason = reason;
}

void NewPipelineClientRenderPath::AcceptRemoteFrame(const RemoteRawFrame& frame)
{
    if (!UploadRemoteTextures(frame))
    {
        InvalidateRemote("upload failed");
        return;
    }

    const RemoteFrameMetadata& metadata = frame.metadata;
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
    remote_ddgi_frame_index = metadata.ddgi_frame_index;
    remote_ddgi_reset_reason = metadata.ddgi_reset_reason;
    remote_consume.placeholder = false;
    remote_consume.stale_timer = 0.0f;
    remote_consume.stale_logged = false;
    remote_consume.fallback_reason.clear();

    wi::backlog::post("Client remote accepted frame " + std::to_string(remote_consume.accepted_frame_id) +
        " generation " + std::to_string(remote_consume.accepted_generation) + " " +
        std::to_string(remote_consume.width) + "x" + std::to_string(remote_consume.height) +
        " confidence=" + std::to_string(remote_consume.confidence));
}

void NewPipelineClientRenderPath::AcquireRemoteVideoFrame(float dt)
{
    if (!remote_acquire_logged)
    {
        wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
            ? "Client mock packed-video acquire active: " + mock_remote_mailbox.GetRootDirectory()
            : "Client WebRTC video-track acquire active: frame pixels and metadata are video-only");
        remote_acquire_logged = true;
    }

    RemoteRawFrame frame;
    std::string error;
    const bool received = config.remote_source == RemoteSourceMode::Mock
        ? mock_remote_mailbox.TryReadLatest(frame, &error)
        : webrtc_transport.TryReceiveFrame(frame);
    if (!received)
    {
        if (!error.empty())
        {
            if (remote_consume.fallback_reason != error)
                wi::backlog::post(std::string{config.remote_source == RemoteSourceMode::Mock
                    ? "Client mock packed-video acquire failed: "
                    : "Client WebRTC video-track acquire failed: "} + error);
            InvalidateRemote(error);
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
        else if (!remote_consume.no_remote_logged)
        {
            wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
                ? "Client mock packed-video: no latest frame, using local scene."
                : "Client WebRTC video track: no frame yet, using local scene.");
            remote_consume.no_remote_logged = true;
        }
        return;
    }

    const bool same_latest =
        frame.metadata.frame_id == remote_consume.latest_frame_id &&
        frame.metadata.source_generation == remote_consume.latest_generation;
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

    if (remote_consume.latest_generation == frame.metadata.source_generation &&
        remote_consume.latest_frame_id != 0 && frame.metadata.frame_id <= remote_consume.latest_frame_id)
    {
        InvalidateRemote("out-of-order remote video frame");
        return;
    }

    const bool generation_reset =
        frame.metadata.reset_this_frame ||
        (remote_consume.latest_generation != 0 && frame.metadata.source_generation != remote_consume.latest_generation);
    if (generation_reset)
    {
        remote_consume.history_frame_id = 0;
        remote_consume.history_valid = false;
        remote_consume.accepted_valid = false;
        wi::backlog::post("Client remote generation reset: generation " +
            std::to_string(frame.metadata.source_generation) +
            " frame " + std::to_string(frame.metadata.frame_id));
    }

    remote_consume.latest_frame_id = frame.metadata.frame_id;
    remote_consume.latest_generation = frame.metadata.source_generation;
    remote_consume.stale_timer = 0.0f;
    remote_consume.no_remote_logged = false;

    std::string validation_reason;
    if (!remote_payload_read_logged)
    {
        size_t payload_bytes = 0;
        for (const RemoteRawBuffer& buffer : frame.buffers)
            payload_bytes += buffer.payload_rgba8.size() + buffer.payload_rgba16f.size() * sizeof(uint16_t);
        wi::backlog::post(std::string{config.remote_source == RemoteSourceMode::Mock
                ? "Client mock packed-video frame decoded: "
                : "Client WebRTC video-track frame decoded: "} +
            std::to_string(payload_bytes) + " RGBA bytes");
        remote_payload_read_logged = true;
    }

    if (!ValidateRemoteFrame(frame, validation_reason))
    {
        remote_consume.placeholder = validation_reason == "placeholder confidence";
        remote_consume.confidence = frame.metadata.confidence;
        wi::backlog::post("Client remote rejected payload frame " + std::to_string(frame.metadata.frame_id) +
            ": " + validation_reason);
        InvalidateRemote(validation_reason);
        return;
    }

    remote_consume.placeholder_logged = false;
    AcceptRemoteFrame(frame);
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
    case DebugPreviewMode::LocalIndirectDiffuse:
        return local_lightmap_irradiance.IsValid() ? &local_lightmap_irradiance : nullptr;
    case DebugPreviewMode::LocalAO:
        return local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr;
    case DebugPreviewMode::LocalSpecularIndirect:
        return nullptr; // Static environment probes are part of Final.
    case DebugPreviewMode::LocalShadowVisibility:
        return nullptr; // Raster shadows live in the light shadow-map atlas.
    case DebugPreviewMode::RemoteIndirectDiffuse:
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(RemoteBufferSemantic::RemoteIndirectDiffuse)) && accepted_remote_textures[0].IsValid() ? &accepted_remote_textures[0] : nullptr;
    case DebugPreviewMode::RemoteAO:
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(RemoteBufferSemantic::RemoteAO)) && accepted_remote_textures[1].IsValid() ? &accepted_remote_textures[1] : nullptr;
    case DebugPreviewMode::RemoteSpecularIndirect:
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(RemoteBufferSemantic::RemoteSpecularIndirect)) && accepted_remote_textures[2].IsValid() ? &accepted_remote_textures[2] : nullptr;
    case DebugPreviewMode::RemoteShadowVisibility:
        return remote_consume.accepted_valid && (accepted_remote_buffer_mask & RemoteBufferKindMask(RemoteBufferSemantic::RemoteShadowVisibility)) && accepted_remote_textures[3].IsValid() ? &accepted_remote_textures[3] : nullptr;
    case DebugPreviewMode::Final:
    case DebugPreviewMode::RemoteOverview:
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
    if (debug_preview_mode == DebugPreviewMode::RemoteOverview)
    {
        if (remote_consume.accepted_valid && accepted_remote_buffer_mask != 0)
        {
            const float half_width = GetLogicalWidth() * 0.5f;
            const float half_height = GetLogicalHeight() * 0.5f;
            wi::image::Params background;
            background.blendFlag = wi::enums::BLENDMODE_OPAQUE;
            background.enableFullScreen();
            wi::image::Draw(wi::texturehelper::getBlack(), background, cmd);
            for (size_t index = 0; index < accepted_remote_textures.size(); ++index)
            {
                if ((accepted_remote_buffer_mask & RemoteBufferKindMask(static_cast<RemoteBufferSemantic>(index))) == 0 ||
                    !accepted_remote_textures[index].IsValid())
                {
                    wi::font::Params unavailable;
                    unavailable.position = XMFLOAT3(
                        ((index & 1u) + 0.5f) * half_width,
                        ((index / 2u) + 0.5f) * half_height,
                        0.0f);
                    unavailable.h_align = wi::font::WIFALIGN_CENTER;
                    unavailable.v_align = wi::font::WIFALIGN_CENTER;
                    unavailable.size = 18;
                    unavailable.color = wi::Color::Red();
                    unavailable.shadowColor = wi::Color::Black();
                    wi::font::Draw(std::string{"UNAVAILABLE: "} +
                        ToString(static_cast<RemoteBufferSemantic>(index)), unavailable, cmd);
                    continue;
                }
                wi::image::Params fx;
                fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
                fx.quality = wi::image::QUALITY_LINEAR;
                fx.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
                fx.pos = XMFLOAT3((index & 1u) * half_width, (index / 2u) * half_height, 0.0f);
                fx.siz = XMFLOAT2(half_width, half_height);
                if (index == static_cast<size_t>(RemoteBufferSemantic::RemoteAO) ||
                    index == static_cast<size_t>(RemoteBufferSemantic::RemoteShadowVisibility))
                    fx.enableExtractChannelR();
                else
                    fx.enableDebugTonemap();
                wi::image::Draw(&accepted_remote_textures[index], fx, cmd);
            }
            wi::RenderPath2D::Compose(cmd);
            return;
        }
    }
    else if (const wi::graphics::Texture* debug_texture = GetDebugPreviewTexture())
    {
        wi::image::Params fx;
        fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
        // Keep the explicit single-buffer view pixel-exact. The 2x2 overview
        // remains a deliberately scaled overview and is not used for judging
        // source resolution.
        fx.quality = wi::image::QUALITY_NEAREST;
        fx.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
        fx.enableFullScreen();
        if (debug_preview_mode == DebugPreviewMode::GBufferNormalXY)
            fx.color = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f);
        else if (debug_preview_mode == DebugPreviewMode::GBufferRoughness)
            fx.color = XMFLOAT4(0.0f, 0.0f, 4.0f, 1.0f);
        else if (debug_preview_mode == DebugPreviewMode::LocalSpecularIndirect ||
            debug_preview_mode == DebugPreviewMode::RemoteIndirectDiffuse ||
            debug_preview_mode == DebugPreviewMode::RemoteSpecularIndirect)
            fx.enableDebugTonemap();
        if (debug_preview_mode == DebugPreviewMode::LocalAO ||
            debug_preview_mode == DebugPreviewMode::LocalShadowVisibility ||
            debug_preview_mode == DebugPreviewMode::RemoteAO ||
            debug_preview_mode == DebugPreviewMode::RemoteShadowVisibility)
            fx.enableExtractChannelR();
        wi::image::Draw(debug_texture, fx, cmd);
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
