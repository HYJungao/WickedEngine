#include "NewPipelineClientRenderPath.h"

#include "wiHelper.h"
#include "wiImage.h"

#include <chrono>
#include <cmath>
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

const char* ToString(ClientDebugPreviewMode mode)
{
    switch (mode)
    {
    case ClientDebugPreviewMode::Final:
        return "final";
    case ClientDebugPreviewMode::GBufferDepth:
        return "gbuffer_depth";
    case ClientDebugPreviewMode::GBufferNormalRoughness:
        return "gbuffer_normal_roughness";
    case ClientDebugPreviewMode::GBufferNormalXY:
        return "gbuffer_normal_xy";
    case ClientDebugPreviewMode::GBufferRoughness:
        return "gbuffer_roughness";
    default:
        return "unknown";
    }
}

void NewPipelineClientRenderPath::SetRuntimeConfig(const RuntimeConfig& value)
{
    config        = value;
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

void NewPipelineClientRenderPath::SetDebugPreviewMode(ClientDebugPreviewMode mode)
{
    debug_preview_mode = mode;
    debug_preview_invalid_logged = false;
    wi::backlog::post(std::string{"Client debug preview mode: "} + ToString(debug_preview_mode));
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
        if (previous.lightmap_bake_requested && !render_settings.lightmap_bake_requested)
        {
            ApplyLightmapBakeRequest(previous.lightmap_bake_requested, true);
        }
        ApplyBakedLightmapSettings(previous.baked_lightmaps_enabled, true);
        if (!previous.lightmap_bake_requested || render_settings.lightmap_bake_requested)
        {
            ApplyLightmapBakeRequest(previous.lightmap_bake_requested, true);
        }
    }

    ApplyShadowSettings(shadows_changed);
    ApplySSAOSettings(ssao_changed);
    if (scene_initialized)
        ApplyEnvironmentProbeSettings(probe_changed);
}

void NewPipelineClientRenderPath::Start()
{
    InitializeSceneIfNeeded();
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

void NewPipelineClientRenderPath::Update(float dt)
{
    InitializeSceneIfNeeded();
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
    ApplySunStateToScene(local_scene, sun_state);
    InitializeDefaultCamera(local_camera, (uint32_t)GetLogicalWidth(), (uint32_t)GetLogicalHeight(), result.kind);
    const NewPipelineCameraPreset camera_preset = GetDefaultCameraPreset(result.kind);
    camera_position = camera_preset.position;
    camera_rotation = camera_preset.rotation;

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
        ApplyLightmapBakeRequest(render_settings.lightmap_bake_requested, log_changes, true);
    }
}

void NewPipelineClientRenderPath::ApplyShadowSettings(bool log_changes)
{
    setShadowsEnabled(render_settings.shadow_maps_enabled);
    wi::renderer::SetShadowsEnabled(render_settings.shadow_maps_enabled);
    wi::renderer::SetShadowProps2D(render_settings.shadow_maps_enabled ? kMobileShadow2DResolution : 0);
    wi::renderer::SetShadowPropsCube(render_settings.shadow_maps_enabled ? kMobileShadowCubeResolution : 0);
    if (log_changes)
    {
        wi::backlog::post("Client shadow maps: " + EnabledString(render_settings.shadow_maps_enabled));
    }
}

void NewPipelineClientRenderPath::ApplySSAOSettings(bool log_changes)
{
    setAORange(1.0f);
    setAOSampleCount(8);
    setAOPower(1.0f);
    setAO(render_settings.ssao_enabled ? wi::RenderPath3D::AO_SSAO : wi::RenderPath3D::AO_DISABLED);
    if (log_changes)
    {
        wi::backlog::post("Client SSAO: " + EnabledString(render_settings.ssao_enabled));
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
            environment_probe_entity = existing;
    }

    if (!render_settings.environment_probe_enabled)
    {
        if (environment_probe_entity != wi::ecs::INVALID_ENTITY)
        {
            local_scene.Entity_Remove(environment_probe_entity);
            environment_probe_entity = wi::ecs::INVALID_ENTITY;
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

void NewPipelineClientRenderPath::ApplyLightmapBakeRequest(bool previous_requested, bool log_changes, bool force_log)
{
    if (!scene_initialized)
        return;

    const bool changed = previous_requested != render_settings.lightmap_bake_requested;
    if (!changed && !force_log)
        return;

    uint32_t requested = 0;
    uint32_t skipped = 0;
    uint32_t saved = 0;
    uint32_t skipped_logged = 0;

    if (render_settings.lightmap_bake_requested && !render_settings.baked_lightmaps_enabled)
    {
        wi::backlog::post("Client lightmap bake ignored: baked lightmaps are disabled.");
        render_settings.lightmap_bake_requested = false;
        return;
    }

    for (size_t i = 0; i < local_scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = local_scene.objects[i];
        if (render_settings.lightmap_bake_requested)
        {
            if (!ObjectSupportsLightmapBake(object))
            {
                ++skipped;
                if (log_changes && skipped_logged < 8)
                {
                    const wi::ecs::Entity entity = local_scene.objects.GetEntity(i);
                    const wi::scene::NameComponent* name = local_scene.names.GetComponent(entity);
                    wi::backlog::post("Client lightmap bake skipped: " +
                        (name != nullptr && !name->name.empty() ? name->name : std::to_string(entity)) +
                        " has no usable lightmap atlas.");
                    ++skipped_logged;
                }
                continue;
            }
            if (object.lightmapWidth == 0 || object.lightmapHeight == 0)
            {
                object.lightmapWidth = kMobileLightmapResolution;
                object.lightmapHeight = kMobileLightmapResolution;
            }
            object.SetLightmapRenderRequest(true);
            ++requested;
        }
        else if (object.IsLightmapRenderRequested())
        {
            object.SetLightmapRenderRequest(false);
            if (object.lightmap.IsValid())
            {
                object.SaveLightmap();
                ++saved;
            }
        }
    }

    if (render_settings.lightmap_bake_requested && requested > 0)
    {
        local_scene.SetAccelerationStructureUpdateRequested(true);
    }

    if (log_changes)
    {
        if (render_settings.lightmap_bake_requested)
        {
            wi::backlog::post("Client lightmap bake: enabled requested=" + std::to_string(requested) +
                " skipped=" + std::to_string(skipped));
        }
        else
        {
            wi::backlog::post("Client lightmap bake: disabled saved=" + std::to_string(saved));
        }
    }
}

void NewPipelineClientRenderPath::DisableBakedLightmaps()
{
    saved_lightmaps.clear();
    for (size_t i = 0; i < local_scene.objects.GetCount(); ++i)
    {
        wi::scene::ObjectComponent& object = local_scene.objects[i];
        if (object.lightmapTextureData.empty() && object.lightmap.IsValid())
        {
            wi::helper::saveTextureToMemory(object.lightmap, object.lightmapTextureData);
        }

        if (!object.lightmapTextureData.empty() || object.lightmap.IsValid())
        {
            SavedLightmap saved;
            saved.entity = local_scene.objects.GetEntity(i);
            saved.width = object.lightmapWidth;
            saved.height = object.lightmapHeight;
            saved.texture_data = object.lightmapTextureData;
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
        object->lightmapTextureData = saved.texture_data;
        object->lightmap = {};
        object->lightmap_render = {};
    }
    saved_lightmaps.clear();
}

bool NewPipelineClientRenderPath::ObjectSupportsLightmapBake(const wi::scene::ObjectComponent& object) const
{
    if (!object.IsRenderable() || object.meshID == wi::ecs::INVALID_ENTITY)
        return false;

    const wi::scene::MeshComponent* mesh = local_scene.meshes.GetComponent(object.meshID);
    return mesh != nullptr && mesh->IsRenderable() && !mesh->vertex_atlas.empty();
}

void NewPipelineClientRenderPath::UpdateLocalCamera(float dt)
{
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

    bool moved_by_input = false;

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
        moved_by_input = true;
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
        moved_by_input = true;
    }
    if (IsCharacterDown('S') || IsDown(wi::input::KEYBOARD_BUTTON_DOWN))
    {
        camera_position.x -= forward.x * move_speed;
        camera_position.y -= forward.y * move_speed;
        camera_position.z -= forward.z * move_speed;
        moved_by_input = true;
    }
    if (IsCharacterDown('D') || IsDown(wi::input::KEYBOARD_BUTTON_RIGHT))
    {
        camera_position.x += right.x * move_speed;
        camera_position.y += right.y * move_speed;
        camera_position.z += right.z * move_speed;
        moved_by_input = true;
    }
    if (IsCharacterDown('A') || IsDown(wi::input::KEYBOARD_BUTTON_LEFT))
    {
        camera_position.x -= right.x * move_speed;
        camera_position.y -= right.y * move_speed;
        camera_position.z -= right.z * move_speed;
        moved_by_input = true;
    }

    if (!moved_by_input)
    {
        camera_rotation.y += dt * 0.05f;
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
        if (buffer.width == 0 || buffer.height == 0 ||
            buffer.payload_rgba8.size() != static_cast<size_t>(buffer.width) * buffer.height * 4u)
            return false;

        wi::graphics::TextureDesc desc;
        desc.type = wi::graphics::TextureDesc::Type::TEXTURE_2D;
        desc.width = buffer.width;
        desc.height = buffer.height;
        desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE;
        wi::vector<wi::graphics::SubresourceData> subresources;
        wi::graphics::CreateTextureSubresourceDatas(desc, const_cast<uint8_t*>(buffer.payload_rgba8.data()), subresources);
        if (!wi::graphics::GetDevice()->CreateTexture(&desc, subresources.data(), &uploaded[index]))
            return false;
        const std::string name = std::string{"newpipeline.client."} + ToString(buffer.semantic) + ".rgba8";
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
    if (metadata.dynamic_range != RemoteDynamicRange::LDR)
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
        if (buffer.payload_rgba8.size() != expected_size)
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
            payload_bytes += buffer.payload_rgba8.size();
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

bool NewPipelineClientRenderPath::ShouldDisplayRemote() const
{
    if (config.remote_debug_mode == RemoteDebugMode::Local)
        return false;
    const size_t indirect_index = static_cast<size_t>(RemoteBufferSemantic::RemoteIndirectDiffuse);
    if (!remote_consume.accepted_valid || !accepted_remote_textures[indirect_index].IsValid())
        return false;
    return config.remote_debug_mode == RemoteDebugMode::Raw ||
        config.remote_debug_mode == RemoteDebugMode::DebugComposite;
}

const wi::graphics::Texture* NewPipelineClientRenderPath::GetDebugPreviewTexture() const
{
    switch (debug_preview_mode)
    {
    case ClientDebugPreviewMode::GBufferDepth:
        return depthBuffer_Copy.IsValid() ? &depthBuffer_Copy : nullptr;
    case ClientDebugPreviewMode::GBufferNormalRoughness:
    case ClientDebugPreviewMode::GBufferNormalXY:
    case ClientDebugPreviewMode::GBufferRoughness:
        return visibilityResources.texture_normal_roughness.IsValid() ? &visibilityResources.texture_normal_roughness : nullptr;
    case ClientDebugPreviewMode::Final:
    default:
        return nullptr;
    }
}

void NewPipelineClientRenderPath::Compose(wi::graphics::CommandList cmd) const
{
    if (debug_preview_mode != ClientDebugPreviewMode::Final)
    {
        const wi::graphics::Texture* debug_texture = GetDebugPreviewTexture();
        if (debug_texture != nullptr)
        {
            wi::image::Params fx;
            fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
            fx.quality = wi::image::QUALITY_NEAREST;
            fx.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
            fx.enableFullScreen();
            if (debug_preview_mode == ClientDebugPreviewMode::GBufferNormalXY)
            {
                fx.color = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f);
            }
            else if (debug_preview_mode == ClientDebugPreviewMode::GBufferRoughness)
            {
                fx.color = XMFLOAT4(0.0f, 0.0f, 4.0f, 1.0f);
            }
            wi::image::Draw(debug_texture, fx, cmd);
            wi::RenderPath2D::Compose(cmd);
            return;
        }

        if (!debug_preview_invalid_logged)
        {
            wi::backlog::post(std::string{"Client debug preview unavailable, falling back to final: "} +
                ToString(debug_preview_mode));
            debug_preview_invalid_logged = true;
        }
    }

    if (!ShouldDisplayRemote())
    {
        wi::RenderPath3D::Compose(cmd);
        return;
    }

    if (config.remote_debug_mode == RemoteDebugMode::Raw)
    {
        wi::image::Params fx;
        fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
        fx.quality = wi::image::QUALITY_LINEAR;
        fx.enableFullScreen();
        wi::image::Draw(&accepted_remote_textures[static_cast<size_t>(RemoteBufferSemantic::RemoteIndirectDiffuse)], fx, cmd);
    }
    else
    {
        // DebugComposite deliberately visualizes the four formal video tiles. The production
        // material formula is implemented by the UE renderer injection path, not by this preview.
        const float half_width = GetLogicalWidth() * 0.5f;
        const float half_height = GetLogicalHeight() * 0.5f;
        for (size_t index = 0; index < accepted_remote_textures.size(); ++index)
        {
            if (!accepted_remote_textures[index].IsValid())
                continue;
            wi::image::Params fx;
            fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
            fx.quality = wi::image::QUALITY_LINEAR;
            fx.pos = XMFLOAT3((index & 1u) * half_width, (index / 2u) * half_height, 0.0f);
            fx.siz = XMFLOAT2(half_width, half_height);
            wi::image::Draw(&accepted_remote_textures[index], fx, cmd);
        }
    }

    wi::RenderPath2D::Compose(cmd);
}
} // namespace wicked_newpipeline
