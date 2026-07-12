#include "NewPipelineServerRenderPath.h"

#include "wiHelper.h"
#include "wiImage.h"

#include <algorithm>
#include <chrono>

namespace wicked_newpipeline
{
namespace
{
uint64_t NowUsec()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count());
}
} // namespace

void NewPipelineServerRenderPath::SetRuntimeConfig(const RuntimeConfig& value)
{
    config        = value;
    status_logged = false;
}

void NewPipelineServerRenderPath::SetServerSettings(const NewPipelineServerSettings& value)
{
    settings = value;
    status_logged = false;
    ddgi_formal_status_logged = false;
}

void NewPipelineServerRenderPath::SetDebugPreviewMode(DebugPreviewMode mode)
{
    debug_preview_mode = mode;
    debug_preview_invalid_logged = false;
    wi::backlog::post(std::string{"Server debug preview mode: "} + ToString(debug_preview_mode));
}

std::string NewPipelineServerRenderPath::GetEffectiveAlgorithmSummary() const
{
    return std::string{"DDGI | "} + (hardware_raytracing ? "RTAO | RT Reflection | RT Shadow" :
        "SSAO | SSR | Screen Space Shadow");
}

void NewPipelineServerRenderPath::Start()
{
    InitializeSceneIfNeeded();
    ConfigureDDGI();
    wi::RenderPath3D::Start();
    if (config.remote_source == RemoteSourceMode::WebRTC)
    {
        std::string error;
        if (!webrtc_transport.Start(true, config, &error))
            wi::backlog::post("Server WebRTC start failed: " + error);
    }

    wi::backlog::post("NewPipeline_Wicked Server render path started.");
    wi::backlog::post(std::string{"Server remote source: "} + ToString(config.remote_source));
    wi::backlog::post(std::string{"Server DDGI: "} + (settings.ddgi_enabled ? "enabled" : "disabled"));
    wi::backlog::post("Server DDGI ray count: " + std::to_string(settings.ddgi_ray_count));
    wi::backlog::post("Server remote publish FPS: " + std::to_string(settings.remote_publish_fps));
    wi::backlog::post(std::string{"Server DDGI formal debug preview: "} +
        (settings.ddgi_enabled && settings.ddgi_debug_formal ? "enabled" : "disabled"));
    status_logged = true;
}

void NewPipelineServerRenderPath::Update(float dt)
{
    InitializeSceneIfNeeded();
    MaintainWebRTC(dt);
    ApplyLatestControlPacket();

    wi::RenderPath3D::Update(dt);
    LogDDGIStatusIfNeeded();
    PublishRemotePayload(dt);

    if (!status_logged)
    {
        wi::backlog::post(std::string{"Server remote source: "} + ToString(config.remote_source));
        wi::backlog::post(std::string{"Server DDGI: "} + (settings.ddgi_enabled ? "enabled" : "disabled"));
        wi::backlog::post("Server DDGI ray count: " + std::to_string(settings.ddgi_ray_count));
        wi::backlog::post("Server remote publish FPS: " + std::to_string(settings.remote_publish_fps));
        wi::backlog::post(std::string{"Server DDGI formal debug preview: "} +
            (settings.ddgi_enabled && settings.ddgi_debug_formal ? "enabled" : "disabled"));
        status_logged = true;
    }
}

void NewPipelineServerRenderPath::ResizeBuffers()
{
    wi::RenderPath3D::ResizeBuffers();
    local_ao_snapshot = {};
    if (rtAO.IsValid())
    {
        wi::graphics::TextureDesc desc = rtAO.GetDesc();
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_ao_snapshot);
        wi::graphics::GetDevice()->SetName(&local_ao_snapshot, "newpipeline.server.local_ao_snapshot");
    }
    if (rtShadow.IsValid())
    {
        EnsureShadowSliceTexture(rtShadow.GetDesc().width, rtShadow.GetDesc().height);
    }
}

void NewPipelineServerRenderPath::RenderAO(wi::graphics::CommandList cmd) const
{
    wi::RenderPath3D::RenderAO(cmd);
    // rtAO aliases particle-distortion memory in RenderPath3D. Preserve the AO
    // result while it is authoritative, before the base renderer reuses it.
    if (rtAO.IsValid() && local_ao_snapshot.IsValid())
        wi::renderer::CopyTexture2D(local_ao_snapshot, rtAO, cmd);
}

void NewPipelineServerRenderPath::RenderPostprocessChain(wi::graphics::CommandList cmd) const
{
    wi::RenderPath3D::RenderPostprocessChain(cmd);
    if (!rtShadow.IsValid() || !shadow_slice_texture.IsValid())
        return;

    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    const wi::graphics::GPUBarrier before[] = {
        wi::graphics::GPUBarrier::Image(
            &rtShadow, rtShadow.GetDesc().layout, wi::graphics::ResourceState::COPY_SRC, -1, 0),
        wi::graphics::GPUBarrier::Image(
            &shadow_slice_texture, shadow_slice_texture.GetDesc().layout, wi::graphics::ResourceState::COPY_DST),
    };
    device->Barrier(before, static_cast<uint32_t>(std::size(before)), cmd);
    device->CopyTexture(&shadow_slice_texture, 0, 0, 0, 0, 0, &rtShadow, 0, 0, cmd);
    const wi::graphics::GPUBarrier after[] = {
        wi::graphics::GPUBarrier::Image(
            &rtShadow, wi::graphics::ResourceState::COPY_SRC, rtShadow.GetDesc().layout, -1, 0),
        wi::graphics::GPUBarrier::Image(
            &shadow_slice_texture, wi::graphics::ResourceState::COPY_DST, shadow_slice_texture.GetDesc().layout),
    };
    device->Barrier(after, static_cast<uint32_t>(std::size(after)), cmd);
}

const wi::graphics::Texture* NewPipelineServerRenderPath::GetDebugPreviewTexture() const
{
    switch (debug_preview_mode)
    {
    case DebugPreviewMode::LocalIndirectDiffuse:
        return GetDDGIRemoteIndirectDiffuseFormal().IsValid() ? &GetDDGIRemoteIndirectDiffuseFormal() : nullptr;
    case DebugPreviewMode::LocalAO:
        return local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr;
    case DebugPreviewMode::LocalSpecularIndirect:
        return rtSSR.IsValid() ? &rtSSR : nullptr;
    case DebugPreviewMode::LocalShadowVisibility:
        return shadow_slice_texture.IsValid() ? &shadow_slice_texture : nullptr;
    case DebugPreviewMode::Final:
    default:
        return nullptr;
    }
}

void NewPipelineServerRenderPath::Compose(wi::graphics::CommandList cmd) const
{
    if (debug_preview_mode == DebugPreviewMode::Final)
    {
        wi::RenderPath3D::Compose(cmd);
        return;
    }

    if (const wi::graphics::Texture* debug_texture = GetDebugPreviewTexture())
    {
        wi::image::Params fx;
        fx.blendFlag = wi::enums::BLENDMODE_OPAQUE;
        fx.quality = wi::image::QUALITY_LINEAR;
        fx.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
        fx.enableFullScreen();
        if (debug_preview_mode == DebugPreviewMode::LocalIndirectDiffuse ||
            debug_preview_mode == DebugPreviewMode::LocalSpecularIndirect)
            fx.enableDebugTonemap();
        if (debug_preview_mode == DebugPreviewMode::LocalAO ||
            debug_preview_mode == DebugPreviewMode::LocalShadowVisibility)
            fx.enableExtractChannelR();
        wi::image::Draw(debug_texture, fx, cmd);
        wi::RenderPath2D::Compose(cmd);
        return;
    }

    if (!debug_preview_invalid_logged)
    {
        wi::backlog::post(std::string{"Server debug preview unavailable, showing Final: "} +
            ToString(debug_preview_mode));
        debug_preview_invalid_logged = true;
    }
    wi::RenderPath3D::Compose(cmd);
}

void NewPipelineServerRenderPath::MaintainWebRTC(float dt)
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
    if (!webrtc_transport.Start(true, config, &error))
        wi::backlog::post("Server WebRTC retry failed: " + error);
    else
        wi::backlog::post("Server WebRTC retrying signaling: " + config.signaling_url);
}

bool NewPipelineServerRenderPath::EnsureTransportTexture(RemoteBufferSemantic semantic, uint32_t width, uint32_t height)
{
    const size_t index = static_cast<size_t>(semantic);
    if (index >= transport_textures.size())
        return false;
    wi::graphics::Texture& texture = transport_textures[index];
    if (texture.IsValid())
    {
        const wi::graphics::TextureDesc& desc = texture.GetDesc();
        if (desc.width == width && desc.height == height && desc.format == wi::graphics::Format::R8G8B8A8_UNORM)
            return true;
    }

    wi::graphics::TextureDesc desc;
    desc.type = wi::graphics::TextureDesc::Type::TEXTURE_2D;
    desc.width = width;
    desc.height = height;
    desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
    desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE;

    texture = {};
    if (!wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &texture))
    {
        wi::backlog::post(std::string{"Server remote: failed to create RGBA8 transport texture for "} + ToString(semantic));
        return false;
    }

    const std::string name = std::string{"newpipeline.remote."} + ToString(semantic) + ".rgba8";
    wi::graphics::GetDevice()->SetName(&texture, name.c_str());
    ++remote_generation;
    wi::backlog::post(std::string{"Server remote: "} + ToString(semantic) + " transport texture " +
        std::to_string(width) + "x" + std::to_string(height) + " format=R8G8B8A8_UNORM.");
    return true;
}

bool NewPipelineServerRenderPath::EnsureShadowSliceTexture(uint32_t width, uint32_t height)
{
    if (shadow_slice_texture.IsValid())
    {
        const wi::graphics::TextureDesc& existing = shadow_slice_texture.GetDesc();
        if (existing.width == width && existing.height == height && existing.format == wi::graphics::Format::R8_UNORM)
            return true;
    }

    wi::graphics::TextureDesc desc;
    desc.type = wi::graphics::TextureDesc::Type::TEXTURE_2D;
    desc.width = width;
    desc.height = height;
    desc.format = wi::graphics::Format::R8_UNORM;
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
    desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE;
    shadow_slice_texture = {};
    if (!wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &shadow_slice_texture))
        return false;
    wi::graphics::GetDevice()->SetName(&shadow_slice_texture, "newpipeline.remote.primary_shadow_slice");
    return true;
}

void NewPipelineServerRenderPath::PublishRemotePayload(float dt)
{
    if (settings.remote_publish_fps <= 0.0f)
    {
        if (!mock_remote_disabled_logged)
        {
            wi::backlog::post("Server remote publish disabled: --remote_fps 0");
            mock_remote_disabled_logged = true;
        }
        return;
    }

    if (!mock_remote_publish_logged)
    {
        wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
            ? "Server mock remote publish active: " + mock_remote_mailbox.GetRootDirectory()
            : "Server WebRTC video-track publish active: " + config.signaling_url + " room=" + config.room_id);
        mock_remote_publish_logged = true;
    }

    const float publish_interval = 1.0f / std::max(0.001f, settings.remote_publish_fps);
    mock_publish_accumulator += dt;
    if (mock_publish_accumulator < publish_interval)
        return;
    mock_publish_accumulator = 0.0f;

    const std::array<const wi::graphics::Texture*, static_cast<size_t>(RemoteBufferSemantic::Count)> sources = {
        settings.ddgi_enabled ? &GetDDGIRemoteIndirectDiffuseFormal() : nullptr,
        local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr,
        rtSSR.IsValid() ? &rtSSR : nullptr,
        shadow_slice_texture.IsValid() ? &shadow_slice_texture : nullptr,
    };
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    wi::graphics::CommandList cmd = device->BeginCommandList();
    uint32_t available_mask = 0;
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const wi::graphics::Texture* source = sources[index];
        if (source == nullptr || !source->IsValid())
            continue;
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        const wi::graphics::TextureDesc& source_desc = source->GetDesc();
        if (!EnsureTransportTexture(semantic, source_desc.width, source_desc.height))
            continue;

        wi::renderer::CopyTexture2D(transport_textures[index], *source, cmd);
        available_mask |= RemoteBufferKindMask(semantic);
    }
    if (available_mask == 0)
        return;
    device->SubmitCommandLists();

    RemoteRawFrame frame;
    frame.metadata.frame_id = ++remote_frame_id;
    frame.metadata.timestamp_usec = NowUsec();
    frame.metadata.source_generation = remote_generation;
    frame.metadata.continuity_mask = available_mask;
    frame.metadata.available_buffer_mask = available_mask;
    frame.metadata.dynamic_range = RemoteDynamicRange::LDR;
    frame.metadata.source_stream_id = kRemoteFrameStreamId;
    frame.metadata.view_origin = local_camera.Eye;
    XMStoreFloat3(&frame.metadata.view_forward,
        XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&local_camera.At), XMLoadFloat3(&local_camera.Eye))));
    frame.metadata.view = local_camera.View;
    frame.metadata.projection = local_camera.Projection;
    XMStoreFloat4x4(&frame.metadata.view_projection,
        XMMatrixMultiply(XMLoadFloat4x4(&local_camera.View), XMLoadFloat4x4(&local_camera.Projection)));
    XMStoreFloat4x4(&frame.metadata.inverse_view, XMMatrixInverse(nullptr, XMLoadFloat4x4(&frame.metadata.view)));
    XMStoreFloat4x4(&frame.metadata.inverse_projection, XMMatrixInverse(nullptr, XMLoadFloat4x4(&frame.metadata.projection)));
    XMStoreFloat4x4(&frame.metadata.inverse_view_projection,
        XMMatrixInverse(nullptr, XMLoadFloat4x4(&frame.metadata.view_projection)));
    frame.metadata.near_plane = local_camera.zNearP;
    frame.metadata.far_plane = local_camera.zFarP;
    frame.metadata.history_valid = remote_frame_id > 1;
    frame.metadata.reset_this_frame = remote_frame_id == 1;
    frame.metadata.confidence = available_mask == static_cast<uint32_t>(RemoteBufferKind::All) ? 1.0f : 0.75f;
    frame.metadata.valid = true;
    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        wi::vector<uint8_t> readback;
        if (!wi::helper::saveTextureToMemory(transport_textures[index], readback))
        {
            wi::backlog::post(std::string{"Server remote readback failed for "} + ToString(semantic));
            frame.metadata.continuity_mask &= ~RemoteBufferKindMask(semantic);
            frame.metadata.available_buffer_mask &= ~RemoteBufferKindMask(semantic);
            continue;
        }
        const wi::graphics::TextureDesc& desc = transport_textures[index].GetDesc();
        RemoteRawBuffer& destination = frame.buffers[index];
        destination.width = desc.width;
        destination.height = desc.height;
        destination.available = true;
        destination.payload_rgba8.assign(readback.begin(), readback.end());
        frame.metadata.width = std::max(frame.metadata.width, desc.width);
        frame.metadata.height = std::max(frame.metadata.height, desc.height);
    }
    if (frame.metadata.available_buffer_mask == 0)
        return;

    std::string error;
    const bool published = config.remote_source == RemoteSourceMode::Mock
        ? mock_remote_mailbox.PublishLatest(frame, &error)
        : webrtc_transport.SendFrame(frame);
    if (!published)
    {
        if (config.remote_source == RemoteSourceMode::Mock || remote_frame_id == 1)
            wi::backlog::post("Server remote publish failed: " + (error.empty() ? webrtc_transport.GetStats().status : error));
        return;
    }

    if (remote_frame_id == 1)
    {
        wi::backlog::post("Server mock remote published first frame: " +
            std::to_string(frame.metadata.width) + "x" + std::to_string(frame.metadata.height));
    }
}

void NewPipelineServerRenderPath::InitializeSceneIfNeeded()
{
    if (scene_initialized)
        return;

    scene = &local_scene;
    camera = &local_camera;

    const SceneInitializationResult result = InitializeDefaultScene(local_scene);
    InitializeDefaultCamera(
        local_camera,
        (uint32_t)GetLogicalWidth(),
        (uint32_t)GetLogicalHeight(),
        result.kind,
        &local_scene);
    local_scene.ddgi.grid_dimensions = XMUINT3(16, 8, 16);

    std::string scene_message = std::string{"Server scene initialized: "} + ToString(result.kind);
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
        wi::backlog::post("Server scene diagnostic: " + result.diagnostic);
    wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
        ? "Server using file mock control source: " + mock_control_mailbox.GetRootDirectory()
        : "Server using WebRTC DataChannel for client control only; frame output is video-track only.");
    wi::backlog::post("Server DDGI grid dimensions: 16 x 8 x 16 (quality preset).");

    scene_initialized = true;
}

void NewPipelineServerRenderPath::ConfigureDDGI()
{
    hardware_raytracing = wi::graphics::GetDevice()->CheckCapability(
        wi::graphics::GraphicsDeviceCapability::RAYTRACING);

    wi::renderer::SetDDGIEnabled(settings.ddgi_enabled);
    wi::renderer::SetDDGIRayCount(settings.ddgi_enabled ? settings.ddgi_ray_count : 0u);
    wi::renderer::SetDDGIBlendSpeed(0.05f);
    wi::renderer::SetDDGIDebugEnabled(false);
    setAO(hardware_raytracing ? wi::RenderPath3D::AO_RTAO : wi::RenderPath3D::AO_SSAO);
    setRaytracedReflectionsEnabled(hardware_raytracing);
    setSSREnabled(!hardware_raytracing);
    setShadowsEnabled(true);
    wi::renderer::SetShadowsEnabled(true);
    wi::renderer::SetRaytracedShadowsEnabled(hardware_raytracing);
    wi::renderer::SetScreenSpaceShadowsEnabled(!hardware_raytracing);

    wi::backlog::post(std::string{"Server remote algorithms: IndirectDiffuse=DDGI AO="} +
        (hardware_raytracing ? "RTAO" : "SSAO(fallback)") +
        " SpecularIndirect=" + (hardware_raytracing ? "RTReflection" : "SSR(fallback)") +
        " ShadowVisibility=" + (hardware_raytracing ? "RTShadow" : "ScreenSpaceShadow(fallback)") +
        " hardware_raytracing=" + (hardware_raytracing ? "1" : "0"));

    setDDGIOutputDebugPreview(
        settings.ddgi_enabled && settings.ddgi_debug_formal
            ? wi::RenderPath3D::DDGIOutputDebugPreview::RemoteIndirectDiffuseFormal
            : wi::RenderPath3D::DDGIOutputDebugPreview::Disabled);
}

void NewPipelineServerRenderPath::ApplyLatestControlPacket()
{
    if (!mock_control_source_logged)
    {
        wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
            ? "Server file mock control acquire active: " + mock_control_mailbox.GetRootDirectory()
            : "Server WebRTC control DataChannel acquire active (client to server only).");
        mock_control_source_logged = true;
    }

    ClientControlPacket packet;
    std::string error;
    const bool received = config.remote_source == RemoteSourceMode::Mock
        ? mock_control_mailbox.TryConsumeLatest(packet, &error)
        : webrtc_transport.TryReceiveControl(packet);
    if (!received)
    {
        if (!error.empty())
            wi::backlog::post(std::string{config.remote_source == RemoteSourceMode::Mock
                ? "Server file mock control acquire failed: "
                : "Server WebRTC control acquire failed: "} + error);
        return;
    }

    const bool first_control = last_applied_frame_id == 0;
    ApplyControlPacketToCameraAndScene(packet, local_camera, local_scene);
    last_applied_frame_id = packet.frame_id;
    if (first_control)
    {
        wi::backlog::post(std::string{config.remote_source == RemoteSourceMode::Mock
                ? "Server file mock control applied first frame: "
                : "Server WebRTC control applied first frame: "} + std::to_string(last_applied_frame_id) +
            " sun=" + (packet.sun_enabled ? std::string("enabled") : std::string("disabled")));
    }
}

void NewPipelineServerRenderPath::LogDDGIStatusIfNeeded()
{
    if (ddgi_formal_status_logged)
        return;

    const wi::graphics::Texture& formal = GetDDGIRemoteIndirectDiffuseFormal();
    if (!settings.ddgi_enabled)
    {
        wi::backlog::post("Server RemoteIndirectDiffuseFormal: DDGI disabled, mock remote publish skipped.");
        ddgi_formal_status_logged = true;
        return;
    }

    if (!formal.IsValid())
        return;

    const wi::graphics::TextureDesc& desc = formal.GetDesc();
    wi::backlog::post("Server RemoteIndirectDiffuseFormal: valid");
    wi::backlog::post(std::string{"Server RemoteIndirectDiffuseFormal desc: "} +
        std::to_string(desc.width) + "x" + std::to_string(desc.height) +
        " format=" + wi::graphics::GetFormatString(desc.format));
    ddgi_formal_status_logged = true;
}
} // namespace wicked_newpipeline
