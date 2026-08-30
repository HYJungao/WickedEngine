#include "NewPipelineServerRenderPath.h"

#include "wiHelper.h"
#include "wiImage.h"
#include "wiFont.h"
#include "wiTextureHelper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

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

NewPipelineServerRenderPath::~NewPipelineServerRenderPath()
{
    StopPublishWorker();
}

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
    setDDGIOutputDebugPreview(mode == DebugPreviewMode::LocalIndirectDiffuse
        ? wi::RenderPath3D::DDGIOutputDebugPreview::RemoteIndirectDiffuseFormal
        : wi::RenderPath3D::DDGIOutputDebugPreview::Disabled);
    debug_preview_invalid_logged = false;
    wi::backlog::post(std::string{"Server debug preview mode: "} + ToString(debug_preview_mode));
}

std::string NewPipelineServerRenderPath::GetEffectiveAlgorithmSummary() const
{
    return std::string{"DDGI | "} + (hardware_raytracing
        ? "RTAO full-res | RT Reflection High full-res | RT Shadow full-res"
        : "RTAO unavailable | RT Reflection unavailable | RT Shadow unavailable");
}

std::string NewPipelineServerRenderPath::GetDebugStatusSummary() const
{
    size_t pending_count = 0;
    for (const PackedReadbackSlot& slot : packed_readback_ring)
        pending_count += slot.pending ? 1u : 0u;
    const std::string shadow = authoritative_shadow_index < 16
        ? std::to_string(authoritative_shadow_index)
        : std::string{"unavailable"};
    const WebRTCTransportStats transport = webrtc_transport.GetStats();
    const bool native_capture_active =
        transport.native_codec && !native_nv12_runtime_disabled;
    const std::string capture_surface = native_nv12_runtime_disabled
        ? "i420-readback (native disabled: " + native_nv12_runtime_failure + ")"
        : transport.input_surface;
    std::string semantic_status;
    if (packed_layout_contract_valid &&
        packed_layout_contract.protocol_version ==
            kRemoteVideoWireVersionV3)
    {
        semantic_status = "\nSemantic age/area/updates:";
        for (size_t index = 0;
            index < packed_layout_contract.contract_v3.descriptors.size();
            ++index)
        {
            const RemoteBufferDescriptorV3& descriptor =
                packed_layout_contract.contract_v3.descriptors[index];
            if ((descriptor.flags & kRemoteBufferDescriptorAvailableV3) == 0)
                continue;
            const uint64_t age = remote_frame_id >= descriptor.content_frame_id
                ? remote_frame_id - descriptor.content_frame_id : 0;
            semantic_status += " " +
                std::string{ToString(descriptor.semantic)} + "=" +
                std::to_string(age) + "/" +
                std::to_string(
                    static_cast<uint64_t>(descriptor.atlas_width) *
                    descriptor.atlas_height) + "/" +
                std::to_string(remote_content_updates[index]);
        }
    }
    const std::string transport_status =
        "\nWebRTC: " + std::string{ToString(transport.state)} +
            "\nEncoder: requested=" + transport.requested_encoder_mode +
            " active=" + transport.active_encoder_mode +
            " codec=" + transport.codec_name +
            " profile=" + transport.codec_profile +
            " impl=" + transport.codec_implementation +
            "\nSurface: " + capture_surface +
            " zero-raw-readback=" + (native_capture_active ? "yes" : "no") +
            " power-efficient=" + (transport.power_efficient_codec ? "yes" : "no") +
            " fallback=" + transport.fallback_reason +
            " encode-avg=" + std::to_string(
                transport.frames_encoded > 0
                    ? transport.total_encode_time_usec /
                        transport.frames_encoded / 1000u
                    : 0u) + " ms" +
            " net=" + std::to_string(transport.compressed_bytes_sent / 1024u) + " KiB" +
            " bitrate=" + std::to_string(transport_bitrate_bps / 1000u) + " kbps" +
            " captures=" + std::to_string(remote_capture_count) +
            " capture-drop=" + std::to_string(remote_capture_drops) +
            " stale-readback-drop=" + std::to_string(remote_readback_latency_drops) +
            " queue-drop=" + std::to_string(publish_queue_drops.load(std::memory_order_relaxed)) +
            " readback=" + std::to_string(gpu_readback_bytes / 1024u) + " KiB" +
            " cpu-copy=" + std::to_string(
                transport.cpu_full_frame_copy_bytes / 1024u) + " KiB" +
            " convert=" + std::to_string(transport.cpu_conversion_usec / 1000u) + " ms";
    return GetEffectiveAlgorithmSummary() + "\nSun shadow slice: " + shadow +
        " stable-id=" + std::to_string(authoritative_shadow_light_id) +
        " generation=" + std::to_string(authoritative_shadow_light_generation) +
        "\nRemote stream: protocol=" + std::to_string(remote_stream_selection.protocol_version) +
        " quality=" + std::to_string(static_cast<uint32_t>(remote_stream_selection.quality_tier)) +
        " atlas=" + std::to_string(packed_layout_width) + "x" +
        std::to_string(packed_layout_height) +
        "\nCapture transfer: " +
            (native_capture_active
                ? "DX12 retained NV12 ring + shared fence, raw readback disabled"
                : "retained I420 async readback ring 3") +
            ", pending " + std::to_string(pending_count) +
        semantic_status +
        transport_status +
        "\nDDGI: frame " + std::to_string(local_scene.ddgi.frame_index) +
        (local_scene.ddgi.frame_index >= 64 ? " converged" : " warming") +
        " reset=" + ToString(ddgi_reset_reason);
}

void NewPipelineServerRenderPath::Start()
{
    native_nv12_runtime_disabled = false;
    native_nv12_runtime_failure.clear();
    InitializeSceneIfNeeded();
    ConfigureDDGI();
    // SpecularIndirectPreAO is emitted by Visibility_Shade. Keep Server and
    // Client on the same formal-output stage.
    setVisibilityComputeShadingEnabled(true);
    wi::RenderPath3D::Start();
    StartPublishWorker();
    std::string error;
    if (!webrtc_transport.RequestStart(
            true, config, GetWindowsDX12AdapterLuid(), &error))
        wi::backlog::post("Server WebRTC start failed: " + error);

    wi::backlog::post("NewPipeline_Wicked Server render path started.");
    wi::backlog::post("Server remote source: WebRTC V3");
    wi::backlog::post(std::string{"Server DDGI: "} + (settings.ddgi_enabled ? "enabled" : "disabled"));
    wi::backlog::post("Server DDGI ray count: " + std::to_string(settings.ddgi_ray_count));
    wi::backlog::post("Server remote publish FPS: " + std::to_string(settings.remote_publish_fps));
    wi::backlog::post(std::string{"Server remote encoder requested: "} +
        ToString(config.remote_encoder));
    wi::backlog::post(std::string{"Server DDGI formal debug preview: "} +
        (settings.ddgi_enabled && settings.ddgi_debug_formal ? "enabled" : "disabled"));
    status_logged = true;
}

void NewPipelineServerRenderPath::Update(float dt)
{
    InitializeSceneIfNeeded();
    MaintainWebRTC(dt);
    ApplyLatestControlPacket();

    // Apply Scene::Update() before deriving the wire-visible light generation.
    // This matches the Client lifecycle and prevents transform normalization
    // from changing the identity after it has already been published.
    wi::RenderPath3D::Update(dt);
    RefreshAuthoritativeShadowIdentity();
    visibilityResources.texture_primary_light_visibility =
        local_primary_light_visibility.IsValid()
        ? &local_primary_light_visibility : nullptr;
    visibilityResources.primary_light_shadow_index =
        authoritative_shadow_index < 16
        ? static_cast<int>(authoritative_shadow_index) : -1;

    LogDDGIStatusIfNeeded();
    PublishRemotePayload(dt);

    if (!status_logged)
    {
        wi::backlog::post("Server remote source: WebRTC V3");
        wi::backlog::post(std::string{"Server DDGI: "} + (settings.ddgi_enabled ? "enabled" : "disabled"));
        wi::backlog::post("Server DDGI ray count: " + std::to_string(settings.ddgi_ray_count));
        wi::backlog::post("Server remote publish FPS: " + std::to_string(settings.remote_publish_fps));
        wi::backlog::post(std::string{"Server DDGI formal debug preview: "} +
            (settings.ddgi_enabled && settings.ddgi_debug_formal ? "enabled" : "disabled"));
        status_logged = true;
    }
}

void NewPipelineServerRenderPath::Render() const
{
    wi::RenderPath3D::Render();
    // All formal lighting resources and the matching camera GBuffer have now
    // been recorded for this frame. Capture only here so frame pixels and
    // source_control_frame_id never straddle two different views.
    const_cast<NewPipelineServerRenderPath*>(this)
        ->CaptureRequestedRemotePayload();
}

void NewPipelineServerRenderPath::ResizeBuffers()
{
    wi::RenderPath3D::ResizeBuffers();
    transport_preview_available_mask = 0;
    packed_layout_contract_valid = false;
    local_ao_snapshot = {};
    local_specular_indirect_pre_ao = {};
    local_primary_light_visibility = {};
    visibilityResources.texture_specular_indirect_pre_ao = nullptr;
    visibilityResources.texture_primary_light_visibility = nullptr;
    visibilityResources.primary_light_shadow_index = -1;
    if (rtAO.IsValid())
    {
        wi::graphics::TextureDesc desc = rtAO.GetDesc();
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
        wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_ao_snapshot);
        wi::graphics::GetDevice()->SetName(&local_ao_snapshot, "newpipeline.server.local_ao_snapshot");
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
        if (wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &local_specular_indirect_pre_ao))
        {
            wi::graphics::GetDevice()->SetName(
                &local_specular_indirect_pre_ao, "newpipeline.server.local_specular_indirect_pre_ao");
            visibilityResources.texture_specular_indirect_pre_ao = &local_specular_indirect_pre_ao;
        }
        else
        {
            wi::backlog::post("Server Local Specular Indirect Pre-AO texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" + std::to_string(internal_resolution.y));
        }

        wi::graphics::TextureDesc primary_visibility_desc = desc;
        primary_visibility_desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
        if (wi::graphics::GetDevice()->CreateTexture(
                &primary_visibility_desc, nullptr,
                &local_primary_light_visibility))
        {
            wi::graphics::GetDevice()->SetName(
                &local_primary_light_visibility,
                "newpipeline.server.local_primary_light_visibility");
            visibilityResources.texture_primary_light_visibility =
                &local_primary_light_visibility;
        }
        else
        {
            wi::backlog::post(
                "Server Local Primary Light Visibility texture creation failed: " +
                std::to_string(internal_resolution.x) + "x" +
                std::to_string(internal_resolution.y));
        }
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

const wi::graphics::Texture* NewPipelineServerRenderPath::GetDebugPreviewTexture() const
{
    const auto get_transport_texture =
        [this](RemoteBufferSemantic semantic)
        -> const wi::graphics::Texture*
        {
            const size_t index = static_cast<size_t>(semantic);
            if ((transport_preview_available_mask &
                    RemoteBufferKindMask(semantic)) == 0 ||
                !transport_textures[index].IsValid())
            {
                return nullptr;
            }
            return &transport_textures[index];
        };
    switch (debug_preview_mode)
    {
    case DebugPreviewMode::LocalIndirectDiffuse:
        return GetDDGIRemoteIndirectDiffuseFormal().IsValid() ? &GetDDGIRemoteIndirectDiffuseFormal() : nullptr;
    case DebugPreviewMode::LocalAO:
        return local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr;
    case DebugPreviewMode::LocalSpecularIndirect:
        return rtSSR.IsValid() ? &rtSSR : nullptr;
    case DebugPreviewMode::LocalSpecularIndirectPreAO:
        return local_specular_indirect_pre_ao.IsValid() ? &local_specular_indirect_pre_ao : nullptr;
    case DebugPreviewMode::LocalShadowVisibility:
        return authoritative_shadow_index < 16 &&
            local_primary_light_visibility.IsValid()
            ? &local_primary_light_visibility : nullptr;
    case DebugPreviewMode::TransportIndirectDiffuse:
        return get_transport_texture(
            RemoteBufferSemantic::RemoteIndirectDiffuse);
    case DebugPreviewMode::TransportAO:
        return get_transport_texture(RemoteBufferSemantic::RemoteAO);
    case DebugPreviewMode::TransportSpecularIndirect:
        return get_transport_texture(
            RemoteBufferSemantic::RemoteSpecularIndirect);
    case DebugPreviewMode::TransportShadowVisibility:
        return get_transport_texture(
            RemoteBufferSemantic::RemoteShadowVisibility);
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
    if (debug_preview_mode == DebugPreviewMode::LocalIndirectDiffuse)
    {
        if (GetDDGIRemoteIndirectDiffuseFormal().IsValid())
            wi::RenderPath3D::Compose(cmd);
        else
            DrawUnavailablePreview(cmd);
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
        // Buffer inspection must not add a presentation-time blur. The
        // production texture is sampled one texel at a time in this view.
        fx.quality = wi::image::QUALITY_NEAREST;
        fx.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
        fx.enableFullScreen();
        if (debug_preview_mode == DebugPreviewMode::LocalSpecularIndirect ||
            debug_preview_mode == DebugPreviewMode::LocalSpecularIndirectPreAO)
            fx.enableDebugTonemap();
        if (debug_preview_mode == DebugPreviewMode::TransportIndirectDiffuse ||
            debug_preview_mode == DebugPreviewMode::TransportSpecularIndirect)
        {
            fx.enableHDRTransportDecode();
            fx.enableDebugTonemap();
        }
        if (debug_preview_mode == DebugPreviewMode::LocalAO ||
            debug_preview_mode == DebugPreviewMode::LocalShadowVisibility ||
            debug_preview_mode == DebugPreviewMode::TransportAO ||
            debug_preview_mode == DebugPreviewMode::TransportShadowVisibility)
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
        wi::backlog::post(std::string{"Server debug preview unavailable: "} +
            ToString(debug_preview_mode));
        debug_preview_invalid_logged = true;
    }
    DrawUnavailablePreview(cmd);
}

void NewPipelineServerRenderPath::DrawUnavailablePreview(wi::graphics::CommandList cmd) const
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

void NewPipelineServerRenderPath::MaintainWebRTC(float dt)
{
    webrtc_transport.Tick();
    const WebRTCTransportStats stats = webrtc_transport.GetStats();
    transport_telemetry_window_seconds += std::max(0.0f, dt);
    if (transport_telemetry_window_seconds >= 1.0f)
    {
        const uint64_t delta = stats.compressed_bytes_sent >=
                transport_telemetry_previous_bytes
            ? stats.compressed_bytes_sent -
                transport_telemetry_previous_bytes
            : 0;
        transport_bitrate_bps = static_cast<uint64_t>(
            static_cast<double>(delta) * 8.0 /
            transport_telemetry_window_seconds);
        transport_telemetry_previous_bytes =
            stats.compressed_bytes_sent;
        transport_telemetry_window_seconds = 0.0f;
    }
    if (stats.state == previous_webrtc_state)
        return;
    wi::backlog::post("Server WebRTC " + std::string{ToString(previous_webrtc_state)} + " -> " +
        ToString(stats.state) + (stats.status.empty() ? std::string{} : ": " + stats.status));
    if (previous_webrtc_state == WebRTCTransportState::Connected &&
        stats.state != WebRTCTransportState::Connected)
    {
        for (PackedReadbackSlot& slot : packed_readback_ring)
        {
            slot.pending = false;
            // Drop the render path's ownership immediately at a connection or
            // codec-generation boundary. Any frame still owned by the encoder
            // retains its own shared_ptr and closes the handles only after the
            // MFT has released it.
            slot.native_surface.reset();
            slot.native_completion_scheduled.reset();
            slot.native_consumer_fence_value = 0;
            slot.native_footprint = {};
        }
        packed_readback_write_index = 0;
        transport_preview_available_mask = 0;
        packed_layout_contract_valid = false;
        std::lock_guard lock(publish_mutex);
        pending_video_frame.reset();
    }
    if (stats.state == WebRTCTransportState::Connected &&
        previous_webrtc_state != WebRTCTransportState::Connected)
    {
        native_nv12_runtime_disabled = false;
        native_nv12_runtime_failure.clear();
    }
    previous_webrtc_state = stats.state;
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
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE |
        wi::graphics::BindFlag::RENDER_TARGET |
        wi::graphics::BindFlag::UNORDERED_ACCESS;
    desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE;

    transport_preview_available_mask &=
        ~RemoteBufferKindMask(semantic);
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

bool NewPipelineServerRenderPath::EnsureTransportAtlasTexture(uint32_t width, uint32_t height)
{
    if (transport_atlas_texture.IsValid())
    {
        const wi::graphics::TextureDesc& existing = transport_atlas_texture.GetDesc();
        if (existing.width == width && existing.height == height &&
            existing.format == wi::graphics::Format::R8G8B8A8_UNORM)
            return true;
    }

    wi::graphics::TextureDesc desc;
    desc.type = wi::graphics::TextureDesc::Type::TEXTURE_2D;
    desc.width = width;
    desc.height = height;
    desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::RENDER_TARGET;
    desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE_COMPUTE;
    transport_atlas_texture = {};
    if (!wi::graphics::GetDevice()->CreateTexture(&desc, nullptr, &transport_atlas_texture))
    {
        wi::backlog::post("Server remote: failed to create canonical transport atlas " +
            std::to_string(width) + "x" + std::to_string(height));
        return false;
    }
    wi::graphics::GetDevice()->SetName(
        &transport_atlas_texture, "newpipeline.remote.canonical_atlas.rgba8");
    return true;
}

bool NewPipelineServerRenderPath::EncodeTransportTexture(
    RemoteBufferSemantic semantic,
    const wi::graphics::Texture& source,
    wi::graphics::Texture& destination,
    wi::graphics::CommandList cmd) const
{
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    const wi::graphics::TextureDesc& source_desc = source.GetDesc();
    const wi::graphics::TextureDesc& destination_desc = destination.GetDesc();
    const bool reduced = destination_desc.width < source_desc.width ||
        destination_desc.height < source_desc.height;
    if (reduced && depthBuffer_Copy.IsValid() &&
        visibilityResources.texture_normal_roughness.IsValid() &&
        depthBuffer_Copy.GetDesc().width == source_desc.width &&
        depthBuffer_Copy.GetDesc().height == source_desc.height &&
        visibilityResources.texture_normal_roughness.GetDesc().width == source_desc.width &&
        visibilityResources.texture_normal_roughness.GetDesc().height == source_desc.height)
    {
        const uint32_t mode = semantic == RemoteBufferSemantic::RemoteAO ? 1u :
            (semantic == RemoteBufferSemantic::RemoteShadowVisibility ? 2u : 0u);
        wi::renderer::Postprocess_DownsampleJointLighting(
            source,
            depthBuffer_Copy,
            visibilityResources.texture_normal_roughness,
            destination,
            cmd,
            mode,
            semantic == RemoteBufferSemantic::RemoteIndirectDiffuse ||
                semantic == RemoteBufferSemantic::RemoteSpecularIndirect);
        return true;
    }
    if (reduced)
    {
        // A negotiated reduced semantic is valid only when the matching depth
        // and normal/roughness guides are available. Silently falling back to
        // nearest filtering violates the formal quality contract and creates
        // cross-surface leakage.
        return false;
    }
    const wi::graphics::ResourceState source_layout = source_desc.layout;
    const bool source_needs_pixel_state =
        source_layout != wi::graphics::ResourceState::SHADER_RESOURCE;
    if (source_needs_pixel_state)
    {
        device->Barrier(wi::graphics::GPUBarrier::Image(
            &source,
            source_layout,
            wi::graphics::ResourceState::SHADER_RESOURCE), cmd);
    }
    const wi::graphics::RenderPassImage renderpass = wi::graphics::RenderPassImage::RenderTarget(
        &destination,
        wi::graphics::RenderPassImage::LoadOp::CLEAR,
        wi::graphics::RenderPassImage::StoreOp::STORE,
        destination.GetDesc().layout,
        destination.GetDesc().layout);
    device->RenderPassBegin(&renderpass, 1, cmd);
    wi::graphics::Viewport viewport;
    viewport.width = static_cast<float>(destination.GetDesc().width);
    viewport.height = static_cast<float>(destination.GetDesc().height);
    device->BindViewports(1, &viewport, cmd);
    wi::image::Params params;
    params.blendFlag = wi::enums::BLENDMODE_OPAQUE;
    params.quality = wi::image::QUALITY_NEAREST;
    params.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
    params.enableFullScreen();
    if (semantic == RemoteBufferSemantic::RemoteIndirectDiffuse ||
        semantic == RemoteBufferSemantic::RemoteSpecularIndirect)
        params.enableHDRTransportEncode();
    else
        params.enableExtractChannelR();
    wi::image::Draw(&source, params, cmd);
    device->RenderPassEnd(cmd);
    if (source_needs_pixel_state)
    {
        device->Barrier(wi::graphics::GPUBarrier::Image(
            &source,
            wi::graphics::ResourceState::SHADER_RESOURCE,
            source_layout), cmd);
    }
    return true;
}

void NewPipelineServerRenderPath::PublishRemotePayload(float dt)
{
    if (webrtc_transport.GetStats().state != WebRTCTransportState::Connected)
    {
        publish_accumulator = 0.0f;
        remote_capture_requested = false;
        return;
    }

    if (pending_stream_status.has_value())
    {
        // A selected profile must be announced before any frame using it can
        // enter the capture/publish pipeline. If the non-blocking channel is
        // temporarily busy, retain the status and keep capture disabled.
        if (!webrtc_transport.SendStreamStatus(*pending_stream_status))
        {
            remote_capture_requested = false;
            return;
        }
        pending_stream_status.reset();
    }

    ConsumeCompletedPackedReadback();

    if (settings.remote_publish_fps <= 0.0f)
    {
        remote_capture_requested = false;
        if (!remote_publish_disabled_logged)
        {
            wi::backlog::post("Server remote publish disabled: --remote_fps 0");
            remote_publish_disabled_logged = true;
        }
        return;
    }

    if (!remote_publish_logged)
    {
        wi::backlog::post(
            "Server WebRTC video-track publish active: " +
            config.signaling_url + " room=" + config.room_id);
        remote_publish_logged = true;
    }

    const float publish_interval = 1.0f / std::max(0.001f, settings.remote_publish_fps);
    publish_accumulator = std::min(
        publish_accumulator + std::max(0.0f, dt), publish_interval * 2.0f);
    if (publish_accumulator < publish_interval)
        return;
    publish_accumulator = std::fmod(publish_accumulator, publish_interval);
    remote_capture_requested = true;
}

void NewPipelineServerRenderPath::CaptureRequestedRemotePayload()
{
    if (!remote_capture_requested)
        return;
    remote_capture_requested = false;
    std::array<const wi::graphics::Texture*, static_cast<size_t>(RemoteBufferSemantic::Count)> sources = {};
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteIndirectDiffuse)] =
        settings.ddgi_enabled ? &GetDDGIRemoteIndirectDiffuseFormal() : nullptr;
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteAO)] =
        local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr;
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteSpecularIndirect)] =
        local_specular_indirect_pre_ao.IsValid() ? &local_specular_indirect_pre_ao : nullptr;
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteShadowVisibility)] =
        authoritative_shadow_index < 16 &&
            local_primary_light_visibility.IsValid()
        ? &local_primary_light_visibility : nullptr;
    CapturePackedRemoteFrame(sources);
}

void NewPipelineServerRenderPath::CapturePackedRemoteFrame(
    const std::array<const wi::graphics::Texture*, static_cast<size_t>(RemoteBufferSemantic::Count)>& sources)
{
    if (remote_stream_selection_initialized && remote_stream_selection.protocol_version == 0)
    {
        if (!remote_protocol_mismatch_logged)
        {
            wi::backlog::post(
                "Server remote protocol mismatch: no common version/encoding profile; video publication stopped.");
            remote_protocol_mismatch_logged = true;
        }
        return;
    }
    const bool use_native_nv12 = !native_nv12_runtime_disabled &&
        webrtc_transport.SupportsNativeNV12() &&
        GetWindowsDX12AdapterLuid() != 0;
    if (!use_native_nv12)
    {
        for (PackedReadbackSlot& candidate : packed_readback_ring)
        {
            candidate.native_surface.reset();
            candidate.native_completion_scheduled.reset();
            candidate.native_consumer_fence_value = 0;
            candidate.native_footprint = {};
        }
    }
    PackedReadbackSlot* selected_slot = nullptr;
    size_t selected_slot_index = packed_readback_write_index;
    for (size_t offset = 0; offset < packed_readback_ring.size(); ++offset)
    {
        const size_t index =
            (packed_readback_write_index + offset) % packed_readback_ring.size();
        PackedReadbackSlot& candidate = packed_readback_ring[index];
        const bool retained_surface_available = use_native_nv12
            ? (!candidate.native_surface || candidate.native_surface.use_count() == 1)
            : (!candidate.readback || candidate.readback.use_count() == 1);
        if (!candidate.pending && retained_surface_available)
        {
            selected_slot = &candidate;
            selected_slot_index = index;
            break;
        }
    }
    if (selected_slot == nullptr)
    {
        ++remote_capture_drops;
        return;
    }
    PackedReadbackSlot& slot = *selected_slot;

    RemoteFrameSource contract;
    uint32_t available_mask = 0;
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const wi::graphics::Texture* source = sources[index];
        RemoteSemanticSource& buffer = contract.buffers[index];
        buffer.semantic = static_cast<RemoteBufferSemantic>(index);
        buffer.encoding = RemoteBufferTransportEncoding(buffer.semantic);
        if (source == nullptr || !source->IsValid())
            continue;
        const wi::graphics::TextureDesc& desc = source->GetDesc();
        buffer.width = desc.width;
        buffer.height = desc.height;
        buffer.available = true;
        available_mask |= RemoteBufferKindMask(buffer.semantic);
    }
    if (available_mask == 0)
        return;

    RemoteFrameMetadata& metadata = contract.metadata;
    metadata.frame_id = remote_frame_id + 1u;
    metadata.timestamp_usec = NowUsec();
    metadata.source_generation = remote_generation;
    metadata.continuity_mask = available_mask;
    metadata.available_buffer_mask = available_mask;
    metadata.dynamic_range = RemoteDynamicRange::HDR;
    metadata.source_stream_id = kRemoteFrameStreamId;
    metadata.view_origin = local_camera.Eye;
    XMStoreFloat3(&metadata.view_forward,
        XMVector3Normalize(XMLoadFloat3(&local_camera.At)));
    metadata.view = local_camera.View;
    metadata.projection = local_camera.Projection;
    XMStoreFloat4x4(&metadata.view_projection,
        XMMatrixMultiply(XMLoadFloat4x4(&local_camera.View), XMLoadFloat4x4(&local_camera.Projection)));
    XMStoreFloat4x4(&metadata.inverse_view, XMMatrixInverse(nullptr, XMLoadFloat4x4(&metadata.view)));
    XMStoreFloat4x4(&metadata.inverse_projection, XMMatrixInverse(nullptr, XMLoadFloat4x4(&metadata.projection)));
    XMStoreFloat4x4(&metadata.inverse_view_projection,
        XMMatrixInverse(nullptr, XMLoadFloat4x4(&metadata.view_projection)));
    metadata.near_plane = local_camera.zNearP;
    metadata.far_plane = local_camera.zFarP;
    metadata.history_valid = local_scene.ddgi.frame_index >= 64;
    metadata.reset_this_frame = ddgi_announced_reset_serial != ddgi_reset_serial;
    metadata.camera_cut = camera_cut_pending;
    metadata.confidence = 1.0f;
    metadata.valid = true;
    metadata.ddgi_frame_index = local_scene.ddgi.frame_index;
    metadata.ddgi_reset_reason = ddgi_reset_reason;

    const uint64_t control_frame_id =
        last_applied_control.control_frame_id != 0
        ? last_applied_control.control_frame_id
        : last_applied_control.frame_id;
    auto proposed_content_states = remote_content_states;
    uint32_t content_update_mask = 0;
    const auto cadence_divisor = [&](RemoteBufferSemantic semantic) {
        if (remote_stream_selection.quality_tier ==
            RemoteQualityTierV3::High)
            return 1u;
        if (remote_stream_selection.quality_tier ==
            RemoteQualityTierV3::Balanced)
        {
            return semantic == RemoteBufferSemantic::RemoteIndirectDiffuse ||
                semantic == RemoteBufferSemantic::RemoteAO ? 2u : 1u;
        }
        return semantic == RemoteBufferSemantic::RemoteIndirectDiffuse ||
            semantic == RemoteBufferSemantic::RemoteAO ? 3u :
            (semantic == RemoteBufferSemantic::RemoteSpecularIndirect
                ? 2u : 1u);
    };
    const auto refresh_content_states = [&](bool force_all) {
        for (size_t index = 0; index < sources.size(); ++index)
        {
            const RemoteBufferSemantic semantic =
                static_cast<RemoteBufferSemantic>(index);
            if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            {
                proposed_content_states[index] = {};
                continue;
            }
            const uint32_t divisor = cadence_divisor(semantic);
            const bool update = force_all ||
                proposed_content_states[index].frame_id == 0 ||
                proposed_content_states[index].generation !=
                    metadata.source_generation ||
                (metadata.frame_id % divisor) == 0;
            if (!update)
                continue;
            proposed_content_states[index].frame_id = metadata.frame_id;
            proposed_content_states[index].generation =
                metadata.source_generation;
            proposed_content_states[index].confidence_unorm =
                static_cast<uint16_t>(
                    std::clamp(metadata.confidence, 0.0f, 1.0f) *
                        65535.0f + 0.5f);
            content_update_mask |= RemoteBufferKindMask(semantic);
        }
    };
    refresh_content_states(
        !packed_layout_contract_valid ||
        control_frame_id == 0 ||
        control_frame_id != remote_content_control_frame_id);

    RemoteVideoFrameLayout layout;
    std::vector<uint8_t> metadata_luma;
    std::string layout_error;
    const auto build_layout = [&]() {
        return BuildRemoteVideoFrameLayoutV3(
            contract,
            remote_stream_selection,
            control_frame_id,
            authoritative_shadow_light_id,
            authoritative_shadow_light_generation,
            layout,
            metadata_luma,
            &layout_error,
            &proposed_content_states);
    };
    if (!build_layout())
    {
        wi::backlog::post("Server GPU I420 layout failed: " + layout_error);
        return;
    }
    const uint32_t generation_before_surfaces = remote_generation;
    for (const RemoteVideoTileLayout& tile : layout.tiles)
    {
        if (tile.available && !EnsureTransportTexture(tile.semantic, tile.width, tile.height))
        {
            wi::backlog::post(std::string{"Server GPU I420 canonical surface failed: "} +
                ToString(tile.semantic));
            return;
        }
    }
    if (remote_generation != generation_before_surfaces)
    {
        metadata.source_generation = remote_generation;
        content_update_mask = 0;
        refresh_content_states(true);
        layout_error.clear();
        if (!build_layout())
        {
            wi::backlog::post("Server GPU I420 layout surface-generation rebuild failed: " + layout_error);
            return;
        }
    }
    const auto same_layout_geometry = [](const RemoteVideoFrameLayout& a, const RemoteVideoFrameLayout& b) {
        if (a.protocol_version != b.protocol_version ||
            a.encoding_profile_id != b.encoding_profile_id ||
            a.quality_tier != b.quality_tier ||
            a.video_width != b.video_width || a.video_height != b.video_height)
            return false;
        return a.layout_checksum != 0 &&
            a.layout_checksum == b.layout_checksum;
    };
    const bool layout_boundary =
        !packed_layout_contract_valid ||
        !same_layout_geometry(packed_layout_contract, layout);
    if (layout_boundary)
    {
        ++remote_generation;
        metadata.source_generation = remote_generation;
        content_update_mask = 0;
        refresh_content_states(true);
        if (!build_layout())
        {
            wi::backlog::post("Server GPU I420 layout rebuild failed: " + layout_error);
            return;
        }
        uint64_t content_area = 0;
        for (const RemoteVideoTileLayout& tile : layout.tiles)
        {
            if (tile.available)
                content_area += static_cast<uint64_t>(tile.width) * tile.height;
        }
        const uint64_t full_cell_area = 4ull * layout.metadata.width * layout.metadata.height;
        const uint64_t reduction_percent = full_cell_area > 0 && content_area <= full_cell_area
            ? (full_cell_area - content_area) * 100ull / full_cell_area
            : 0ull;
        wi::backlog::post("Server remote layout boundary: protocol=" +
            std::to_string(layout.protocol_version) +
            " atlas=" + std::to_string(layout.video_width) + "x" +
            std::to_string(layout.video_height) +
            " content_reduction=" + std::to_string(reduction_percent) +
            "% generation=" + std::to_string(remote_generation));
    }
    available_mask = layout.metadata.available_buffer_mask;
    if (!EnsureTransportAtlasTexture(layout.video_width, layout.video_height))
        return;

    uint32_t y_stride = (layout.video_width + 3u) & ~3u;
    uint32_t uv_stride = (layout.video_width / 2u + 3u) & ~3u;
    uint32_t u_offset = y_stride * layout.video_height;
    uint32_t v_offset = u_offset + uv_stride * (layout.video_height / 2u);
    uint64_t packed_size = static_cast<uint64_t>(v_offset) +
        static_cast<uint64_t>(uv_stride) * (layout.video_height / 2u);
    if (use_native_nv12)
    {
        if (!slot.native_surface)
            slot.native_surface = std::make_shared<WindowsServerNV12Surface>();
        std::string native_error;
        if (!EnsureWindowsServerNV12Surface(
                layout.video_width,
                layout.video_height,
                *slot.native_surface,
                slot.native_footprint,
                &native_error) ||
            slot.native_footprint.uv_offset >
                std::numeric_limits<uint32_t>::max())
        {
            ++remote_capture_drops;
            native_nv12_runtime_disabled = true;
            native_nv12_runtime_failure = native_error.empty()
                ? "surface allocation failed" : native_error;
            wi::backlog::post("Server native NV12 surface failed: " + native_error);
            return;
        }
        y_stride = slot.native_footprint.y_stride;
        uv_stride = slot.native_footprint.uv_stride;
        u_offset = static_cast<uint32_t>(slot.native_footprint.uv_offset);
        v_offset = u_offset;
        packed_size = slot.native_footprint.total_size;
    }
    if (packed_size == 0 || packed_size > std::numeric_limits<uint32_t>::max())
    {
        wi::backlog::post("Server GPU I420 packed buffer size is invalid.");
        return;
    }

    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    const uint64_t metadata_buffer_size = (metadata_luma.size() + 3u) & ~3ull;
    if (!slot.metadata_upload.IsValid() || slot.metadata_upload.GetDesc().size < metadata_buffer_size)
    {
        wi::graphics::GPUBufferDesc desc;
        desc.size = metadata_buffer_size;
        desc.usage = wi::graphics::Usage::UPLOAD;
        desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
        desc.misc_flags = wi::graphics::ResourceMiscFlag::BUFFER_RAW;
        slot.metadata_upload = {};
        if (!device->CreateBuffer(&desc, nullptr, &slot.metadata_upload))
            return;
        device->SetName(&slot.metadata_upload, "newpipeline.server.i420.metadata_upload");
    }
    if (!slot.packed_gpu.IsValid() || slot.packed_gpu.GetDesc().size != packed_size)
    {
        wi::graphics::GPUBufferDesc desc;
        desc.size = packed_size;
        desc.bind_flags = wi::graphics::BindFlag::UNORDERED_ACCESS;
        desc.misc_flags = wi::graphics::ResourceMiscFlag::BUFFER_RAW;
        slot.packed_gpu = {};
        if (!device->CreateBuffer(&desc, nullptr, &slot.packed_gpu))
            return;
        slot.packed_gpu_state = wi::graphics::ResourceState::UNORDERED_ACCESS;
        device->SetName(&slot.packed_gpu, "newpipeline.server.i420.packed_gpu");
    }
    if (!use_native_nv12 &&
        (!slot.readback || !slot.readback->buffer.IsValid() ||
            slot.readback->buffer.GetDesc().size != packed_size))
    {
        slot.readback = std::make_shared<PackedReadbackStorage>();
        wi::graphics::GPUBufferDesc desc;
        desc.size = packed_size;
        desc.usage = wi::graphics::Usage::READBACK;
        if (!device->CreateBuffer(&desc, nullptr, &slot.readback->buffer))
        {
            slot.readback.reset();
            return;
        }
        device->SetName(&slot.readback->buffer, "newpipeline.server.i420.readback");
    }
    if (slot.metadata_upload.mapped_data == nullptr ||
        (!use_native_nv12 &&
            (!slot.readback || slot.readback->buffer.mapped_data == nullptr)))
        return;
    std::memcpy(slot.metadata_upload.mapped_data, metadata_luma.data(), metadata_luma.size());

    wi::renderer::I420AtlasPackDesc pack_desc;
    pack_desc.video_resolution = XMUINT2(layout.video_width, layout.video_height);
    pack_desc.metadata_rows = static_cast<uint32_t>(metadata_luma.size() / layout.video_width);
    pack_desc.y_stride = y_stride;
    pack_desc.uv_stride = uv_stride;
    pack_desc.u_offset = u_offset;
    pack_desc.v_offset = v_offset;
    pack_desc.available_mask = available_mask;
    pack_desc.tile_padding = kRemoteVideoV3CodecAlignment * 2u;

    wi::graphics::CommandList cmd = device->BeginCommandList();
    // These are the canonical pre-I420 transport surfaces. The Server preview
    // and the encoder now observe the same resources, so semantic inspection
    // cannot diverge from the data actually submitted to WebRTC.
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        if ((content_update_mask & RemoteBufferKindMask(semantic)) != 0)
        {
            if (!EncodeTransportTexture(
                    semantic,
                    *sources[index],
                    transport_textures[index],
                    cmd))
            {
                ++remote_capture_drops;
                wi::backlog::post(
                    std::string{
                        "Server remote capture rejected: joint guides unavailable for reduced "} +
                    ToString(semantic));
                return;
            }
        }
    }

    // Assemble one persistent canonical RGBA8 atlas. V3 padding is filled by
    // sampling only the corresponding edge row/column/corner. Scaling the
    // complete tile into a larger rectangle is not dilation: it puts interior
    // colors in the border and contaminates I420 chroma at region boundaries.
    std::array<wi::graphics::GPUBarrier,
        static_cast<size_t>(RemoteBufferSemantic::Count)>
        atlas_source_barriers = {};
    uint32_t atlas_source_barrier_count = 0;
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const RemoteBufferSemantic semantic =
            static_cast<RemoteBufferSemantic>(index);
        if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        const wi::graphics::ResourceState layout =
            transport_textures[index].GetDesc().layout;
        if (layout == wi::graphics::ResourceState::SHADER_RESOURCE)
            continue;
        atlas_source_barriers[atlas_source_barrier_count++] =
            wi::graphics::GPUBarrier::Image(
                &transport_textures[index],
                layout,
                wi::graphics::ResourceState::SHADER_RESOURCE);
    }
    if (atlas_source_barrier_count > 0)
    {
        device->Barrier(
            atlas_source_barriers.data(),
            atlas_source_barrier_count,
            cmd);
    }

    const wi::graphics::RenderPassImage atlas_renderpass = wi::graphics::RenderPassImage::RenderTarget(
        &transport_atlas_texture,
        wi::graphics::RenderPassImage::LoadOp::CLEAR,
        wi::graphics::RenderPassImage::StoreOp::STORE,
        transport_atlas_texture.GetDesc().layout,
        transport_atlas_texture.GetDesc().layout);
    device->RenderPassBegin(&atlas_renderpass, 1, cmd);
    wi::graphics::Viewport atlas_viewport;
    atlas_viewport.width = static_cast<float>(layout.video_width);
    atlas_viewport.height = static_cast<float>(layout.video_height);
    device->BindViewports(1, &atlas_viewport, cmd);
    // wi::image applies an explicit Y flip when a custom projection is used,
    // because custom projections are treated as world-space (Y-up).  The
    // transport atlas uses pixel-space (Y-down), so using a Y-down custom
    // projection here flipped every tile and drew it into the preceding
    // vertical slot.  Use the image renderer's canonical pixel-space Canvas
    // path so destination rectangles and descriptor origins share exactly the
    // same coordinate convention.
    wi::Canvas atlas_canvas;
    atlas_canvas.init(layout.video_width, layout.video_height, 96.0f);
    wi::image::SetCanvas(atlas_canvas);
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        wi::image::Params image;
        image.blendFlag = wi::enums::BLENDMODE_OPAQUE;
        image.quality = wi::image::QUALITY_LINEAR;
        image.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
        constexpr uint32_t padding = kRemoteVideoV3CodecAlignment * 2u;
        const float x = static_cast<float>(tile.origin_x);
        const float y = static_cast<float>(tile.origin_y);
        const float width = static_cast<float>(tile.width);
        const float height = static_cast<float>(tile.height);
        const float pad = static_cast<float>(padding);

        const auto draw_dilated_rect = [&](float destination_x, float destination_y,
            float destination_width, float destination_height,
            float source_x, float source_y, float source_width, float source_height) {
            wi::image::Params edge = image;
            edge.quality = wi::image::QUALITY_NEAREST;
            edge.pos = XMFLOAT3(destination_x, destination_y, 0);
            edge.siz = XMFLOAT2(destination_width, destination_height);
            edge.enableDrawRect(XMFLOAT4(source_x, source_y, source_width, source_height));
            wi::image::Draw(&transport_textures[index], edge, cmd);
        };

        // Edge strips:
        draw_dilated_rect(x - pad, y, pad, height, 0, 0, 1, height);
        draw_dilated_rect(x + width, y, pad, height, width - 1, 0, 1, height);
        draw_dilated_rect(x, y - pad, width, pad, 0, 0, width, 1);
        draw_dilated_rect(
            x, y + height, width, pad, 0, height - 1, width, 1);
        // Corners:
        draw_dilated_rect(x - pad, y - pad, pad, pad, 0, 0, 1, 1);
        draw_dilated_rect(x + width, y - pad, pad, pad, width - 1, 0, 1, 1);
        draw_dilated_rect(x - pad, y + height, pad, pad, 0, height - 1, 1, 1);
        draw_dilated_rect(
            x + width, y + height, pad, pad, width - 1, height - 1, 1, 1);
        image.pos = XMFLOAT3(
            static_cast<float>(tile.origin_x),
            static_cast<float>(tile.origin_y),
            0);
        image.siz = XMFLOAT2(static_cast<float>(tile.width), static_cast<float>(tile.height));
        wi::image::Draw(&transport_textures[index], image, cmd);
    }
    device->RenderPassEnd(cmd);
    wi::image::SetCanvas(*this);
    if (atlas_source_barrier_count > 0)
    {
        for (uint32_t index = 0;
            index < atlas_source_barrier_count;
            ++index)
        {
            std::swap(
                atlas_source_barriers[index].image.layout_before,
                atlas_source_barriers[index].image.layout_after);
        }
        device->Barrier(
            atlas_source_barriers.data(),
            atlas_source_barrier_count,
            cmd);
    }

    if (slot.packed_gpu_state != wi::graphics::ResourceState::UNORDERED_ACCESS)
    {
        device->Barrier(wi::graphics::GPUBarrier::Buffer(
            &slot.packed_gpu,
            slot.packed_gpu_state,
            wi::graphics::ResourceState::UNORDERED_ACCESS), cmd);
        slot.packed_gpu_state = wi::graphics::ResourceState::UNORDERED_ACCESS;
    }
    const bool packed = use_native_nv12
        ? wi::renderer::RGB_to_NV12_Atlas(
            transport_atlas_texture, slot.metadata_upload,
            slot.packed_gpu, pack_desc, cmd)
        : wi::renderer::RGB_to_I420_Atlas(
            transport_atlas_texture, slot.metadata_upload,
            slot.packed_gpu, pack_desc, cmd);
    if (!packed)
    {
        ++remote_capture_drops;
        wi::backlog::post(
            use_native_nv12
                ? "Server remote capture rejected: GPU NV12 pack contract is incompatible"
                : "Server remote capture rejected: GPU I420 pack contract is incompatible",
            wi::backlog::LogLevel::Error);
        return;
    }
    device->Barrier(wi::graphics::GPUBarrier::Buffer(
        &slot.packed_gpu, wi::graphics::ResourceState::UNORDERED_ACCESS, wi::graphics::ResourceState::COPY_SRC), cmd);
    slot.packed_gpu_state = wi::graphics::ResourceState::COPY_SRC;
    RetainedNV12Frame native_frame;
    if (use_native_nv12)
    {
        const uint64_t wait_value =
            slot.native_completion_scheduled &&
                slot.native_completion_scheduled->load(std::memory_order_acquire)
            ? slot.native_consumer_fence_value : 0;
        const uint64_t producer_value =
            slot.native_surface->next_fence_value + 1u;
        const uint64_t consumer_value = producer_value + 1u;
        std::string native_error;
        if (!QueueWindowsServerNV12Copy(
                *slot.native_surface,
                slot.native_footprint,
                slot.packed_gpu,
                wait_value,
                producer_value,
                cmd,
                &native_error))
        {
            ++remote_capture_drops;
            native_nv12_runtime_disabled = true;
            native_nv12_runtime_failure = native_error.empty()
                ? "GPU copy/fence setup failed" : native_error;
            wi::backlog::post("Server native NV12 GPU copy failed: " + native_error);
            return;
        }
        slot.native_surface->next_fence_value = consumer_value;
        slot.native_completion_scheduled =
            std::make_shared<std::atomic_bool>(false);
        slot.native_consumer_fence_value = consumer_value;
        struct NativeLifetime
        {
            std::shared_ptr<WindowsServerNV12Surface> surface;
            std::shared_ptr<std::atomic_bool> completion_scheduled;
        };
        auto lifetime = std::make_shared<NativeLifetime>();
        lifetime->surface = slot.native_surface;
        lifetime->completion_scheduled = slot.native_completion_scheduled;
        native_frame.width = layout.video_width;
        native_frame.height = layout.video_height;
        native_frame.texture_shared_handle =
            slot.native_surface->texture_shared_handle;
        native_frame.fence_shared_handle =
            slot.native_surface->fence_shared_handle;
        native_frame.producer_fence_value = producer_value;
        native_frame.consumer_fence_value = consumer_value;
        native_frame.adapter_luid = slot.native_surface->adapter_luid;
        native_frame.rtp_timestamp =
            RemoteVideoRtpTimestamp(layout.metadata.timestamp_usec);
        native_frame.timestamp_usec =
            static_cast<int64_t>(layout.metadata.timestamp_usec);
        native_frame.frame_lifetime = std::move(lifetime);
        native_frame.mark_completion_scheduled = [](void* context) {
            static_cast<std::atomic_bool*>(context)->store(
                true, std::memory_order_release);
        };
        native_frame.completion_context =
            slot.native_completion_scheduled.get();
    }
    else
    {
        device->CopyResource(&slot.readback->buffer, &slot.packed_gpu, cmd);
    }
    device->Barrier(wi::graphics::GPUBarrier::Buffer(
        &slot.packed_gpu, wi::graphics::ResourceState::COPY_SRC, wi::graphics::ResourceState::UNORDERED_ACCESS), cmd);

    slot.layout = std::move(layout);
    slot.y_stride = y_stride;
    slot.uv_stride = uv_stride;
    slot.u_offset = u_offset;
    slot.v_offset = v_offset;
    slot.pending = !use_native_nv12;
    // Record the frame whose graphics-queue fence will cover this copy. The
    // consumer polls that fence and never waits on the render thread.
    slot.gpu_submit_frame = device->GetFrameCount();
    remote_frame_id = metadata.frame_id;
    packed_layout_width = slot.layout.video_width;
    packed_layout_height = slot.layout.video_height;
    // Commit layout/content state only after every GPU resource and command
    // required by this frame has been created and recorded successfully.
    packed_layout_contract = slot.layout;
    packed_layout_contract_valid = true;
    transport_preview_available_mask = available_mask;
    remote_content_states = proposed_content_states;
    remote_content_control_frame_id = control_frame_id;
    for (size_t index = 0; index < remote_content_updates.size(); ++index)
    {
        if ((content_update_mask & (1u << static_cast<uint32_t>(index))) != 0)
            ++remote_content_updates[index];
    }
    if (metadata.reset_this_frame)
        ddgi_announced_reset_serial = ddgi_reset_serial;
    camera_cut_pending = false;
    ++remote_capture_count;
    if (use_native_nv12)
        QueueNV12FrameForPublish(std::move(native_frame), slot.layout);
    else
        gpu_readback_bytes += packed_size;
    packed_readback_write_index = (selected_slot_index + 1u) % kReadbackRingSize;
}

void NewPipelineServerRenderPath::ConsumeCompletedPackedReadback()
{
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    PackedReadbackSlot* completed = nullptr;
    for (PackedReadbackSlot& candidate : packed_readback_ring)
    {
        if (candidate.pending &&
            (candidate.layout.metadata.source_generation != remote_generation ||
                candidate.layout.protocol_version != remote_stream_selection.protocol_version))
        {
            candidate.pending = false;
            continue;
        }
        if (!candidate.pending || !candidate.readback ||
            candidate.readback->buffer.mapped_data == nullptr ||
            !device->IsFrameComplete(
                candidate.gpu_submit_frame, wi::graphics::QUEUE_GRAPHICS))
            continue;
        if (completed == nullptr ||
            candidate.layout.metadata.frame_id > completed->layout.metadata.frame_id)
            completed = &candidate;
    }
    if (completed == nullptr)
        return;

    for (PackedReadbackSlot& candidate : packed_readback_ring)
    {
        if (&candidate == completed || !candidate.pending || !candidate.readback ||
            candidate.readback->buffer.mapped_data == nullptr ||
            !device->IsFrameComplete(
                candidate.gpu_submit_frame, wi::graphics::QUEUE_GRAPHICS))
        {
            continue;
        }
        candidate.pending = false;
        ++remote_readback_latency_drops;
    }

    PackedReadbackSlot& slot = *completed;
    const uint8_t* bytes = static_cast<const uint8_t*>(slot.readback->buffer.mapped_data);
    RetainedI420Frame frame;
    frame.width = slot.layout.video_width;
    frame.height = slot.layout.video_height;
    frame.y_plane = bytes;
    frame.y_stride = slot.y_stride;
    frame.u_plane = bytes + slot.u_offset;
    frame.u_stride = slot.uv_stride;
    frame.v_plane = bytes + slot.v_offset;
    frame.v_stride = slot.uv_stride;
    frame.timestamp_usec = static_cast<int64_t>(slot.layout.metadata.timestamp_usec);
    frame.frame_lifetime = slot.readback;
    slot.pending = false;
    QueueI420FrameForPublish(std::move(frame), slot.layout);
}

void NewPipelineServerRenderPath::StartPublishWorker()
{
    if (publish_worker.joinable())
        return;
    publish_worker_stop = false;
    publish_worker = std::thread([this]() {
        bool first_packed_frame_published = false;
        uint32_t published_generation = 0;
        for (;;)
        {
            RetainedI420Frame i420_frame;
            RetainedNV12Frame nv12_frame;
            RemoteVideoFrameLayout i420_layout;
            {
                std::unique_lock lock(publish_mutex);
                publish_cv.wait(lock, [this]() {
                    return publish_worker_stop || pending_video_frame.has_value();
                });
                if (publish_worker_stop && !pending_video_frame.has_value())
                    return;
                i420_frame = std::move(pending_video_frame->i420);
                nv12_frame = std::move(pending_video_frame->nv12);
                i420_layout = std::move(pending_video_frame->layout);
                pending_video_frame.reset();
            }

            std::string error;
            const bool generation_boundary =
                i420_layout.metadata.camera_cut ||
                i420_layout.metadata.source_generation != published_generation;
            const bool keyframe_ready =
                !generation_boundary || webrtc_transport.RequestKeyframe();
            // Queue the video first. If transport backpressure rejects it, do
            // not leave an orphan metadata record that can wait for a frame
            // which was never submitted. Exact RTP/pixel-band matching still
            // protects the inverse case where metadata submission fails.
            const bool video_sent = keyframe_ready && (nv12_frame.IsValid()
                ? webrtc_transport.SendNV12Frame(nv12_frame)
                : webrtc_transport.SendI420Frame(i420_frame));
            const bool published = video_sent &&
                webrtc_transport.SendFrameMetadata(i420_layout);
            if (published)
                published_generation = i420_layout.metadata.source_generation;
            else
                error = keyframe_ready
                    ? webrtc_transport.GetStats().status
                    : "keyframe request was not acknowledged before generation boundary";
            if (!published)
                wi::backlog::post("Server remote publish failed: " +
                    (error.empty() ? std::string{"unknown transport error"} : error));
            else if (!first_packed_frame_published)
            {
                wi::backlog::post("Server remote published first asynchronous frame: " +
                    std::to_string(nv12_frame.IsValid() ? nv12_frame.width : i420_frame.width) + "x" +
                    std::to_string(nv12_frame.IsValid() ? nv12_frame.height : i420_frame.height) +
                    (nv12_frame.IsValid() ? " native NV12" : " retained I420"));
                first_packed_frame_published = true;
            }
        }
    });
}

void NewPipelineServerRenderPath::StopPublishWorker()
{
    {
        std::lock_guard lock(publish_mutex);
        publish_worker_stop = true;
    }
    publish_cv.notify_all();
    if (publish_worker.joinable())
        publish_worker.join();
}

void NewPipelineServerRenderPath::QueueI420FrameForPublish(
    RetainedI420Frame&& frame, const RemoteVideoFrameLayout& layout)
{
    {
        std::lock_guard lock(publish_mutex);
        if (pending_video_frame.has_value())
            publish_queue_drops.fetch_add(1, std::memory_order_relaxed);
        PendingVideoPublish pending;
        pending.i420 = std::move(frame);
        pending.layout = layout;
        pending_video_frame = std::move(pending);
    }
    publish_cv.notify_one();
}

void NewPipelineServerRenderPath::QueueNV12FrameForPublish(
    RetainedNV12Frame&& frame, const RemoteVideoFrameLayout& layout)
{
    {
        std::lock_guard lock(publish_mutex);
        if (pending_video_frame.has_value())
            publish_queue_drops.fetch_add(1, std::memory_order_relaxed);
        PendingVideoPublish pending;
        pending.nv12 = std::move(frame);
        pending.layout = layout;
        pending_video_frame = std::move(pending);
    }
    publish_cv.notify_one();
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
    wi::backlog::post("Server scene parity: " +
        FormatSceneParityFingerprint(ComputeSceneParityFingerprint(local_scene)));
    wi::backlog::post(
        "Server using WebRTC DataChannel for client control only; frame output is video-track only.");
    wi::backlog::post("Server DDGI grid dimensions: " +
        std::to_string(local_scene.ddgi.grid_dimensions.x) + " x " +
        std::to_string(local_scene.ddgi.grid_dimensions.y) + " x " +
        std::to_string(local_scene.ddgi.grid_dimensions.z) + " (scene/Editor setting).");

    RefreshAuthoritativeShadowIdentity();
    wi::backlog::post("Server primary light identity: stable-id=" +
        std::to_string(authoritative_shadow_light_id) + " generation=" +
        std::to_string(authoritative_shadow_light_generation) + " shadow-index=" +
        (authoritative_shadow_index < 16
            ? std::to_string(authoritative_shadow_index) : std::string{"unavailable"}));

    scene_initialized = true;
    ResetDDGI(DDGIResetReason::InitialScene);
}

void NewPipelineServerRenderPath::RefreshAuthoritativeShadowIdentity() const
{
    authoritative_shadow_light_id = GetNewPipelineSunStableId(local_scene);
    authoritative_shadow_light_entity =
        ResolveStableLightId(local_scene, authoritative_shadow_light_id);
    authoritative_shadow_light_generation =
        ComputeStableLightGeneration(
            local_scene, authoritative_shadow_light_id);
    authoritative_shadow_index = ResolveStableDirectionalLightShadowIndex(
        local_scene, authoritative_shadow_light_id);
}

void NewPipelineServerRenderPath::ConfigureDDGI()
{
    hardware_raytracing = wi::graphics::GetDevice()->CheckCapability(
        wi::graphics::GraphicsDeviceCapability::RAYTRACING);

    wi::renderer::SetDDGIEnabled(settings.ddgi_enabled);
    wi::renderer::SetDDGIRayCount(settings.ddgi_enabled ? settings.ddgi_ray_count : 0u);
    wi::renderer::SetDDGIBlendSpeed(0.1f);
    wi::renderer::SetDDGIDebugEnabled(false);
    setRaytracedReflectionsQuality(wi::renderer::PostProcessQuality::High);
    setRTAOFullResolution(true);
    setRTShadowFullResolution(true);
    setAO(hardware_raytracing ? wi::RenderPath3D::AO_RTAO : wi::RenderPath3D::AO_DISABLED);
    setRaytracedReflectionsEnabled(hardware_raytracing);
    setSSREnabled(false);
    setShadowsEnabled(true);
    wi::renderer::SetShadowsEnabled(true);
    wi::renderer::SetRaytracedShadowsEnabled(hardware_raytracing);
    wi::renderer::SetScreenSpaceShadowsEnabled(false);

    wi::backlog::post(std::string{"Server remote algorithms: IndirectDiffuse=DDGI AO="} +
        (hardware_raytracing ? "RTAO(full-res)" : "RTAO(unavailable)") +
        " SpecularIndirect=" + (hardware_raytracing ? "RTReflection(High, full-res)" : "RTReflection(unavailable)") +
        " ShadowVisibility=" + (hardware_raytracing ? "RTShadow(full-res)" : "RTShadow(unavailable)") +
        " hardware_raytracing=" + (hardware_raytracing ? "1" : "0"));

    setDDGIOutputDebugPreview(
        settings.ddgi_enabled &&
            (settings.ddgi_debug_formal || debug_preview_mode == DebugPreviewMode::LocalIndirectDiffuse)
            ? wi::RenderPath3D::DDGIOutputDebugPreview::RemoteIndirectDiffuseFormal
            : wi::RenderPath3D::DDGIOutputDebugPreview::Disabled);
}

void NewPipelineServerRenderPath::ApplyLatestControlPacket()
{
    if (!control_source_logged)
    {
        wi::backlog::post(
            "Server WebRTC control DataChannel acquire active (client to server only).");
        control_source_logged = true;
    }

    ClientControlPacket packet;
    if (!webrtc_transport.TryReceiveControl(packet))
        return;

    const RemoteStreamSelection negotiated = NegotiateRemoteStream(packet);
    const RemoteStreamStatus stream_status = BuildRemoteStreamStatus(packet);
    pending_stream_status = stream_status;
    if (!remote_stream_selection_initialized || negotiated != remote_stream_selection)
    {
        remote_stream_selection = negotiated;
        remote_stream_selection_initialized = true;
        remote_protocol_mismatch_logged = false;
        ++remote_generation;
        packed_layout_width = 0;
        packed_layout_height = 0;
        packed_layout_contract_valid = false;
        transport_preview_available_mask = 0;
        remote_content_states = {};
        remote_content_control_frame_id = 0;
        for (PackedReadbackSlot& slot : packed_readback_ring)
            slot.pending = false;
        {
            std::lock_guard lock(publish_mutex);
            pending_video_frame.reset();
        }
        wi::backlog::post("Server remote negotiation selected protocol=" +
            std::to_string(remote_stream_selection.protocol_version) +
            " profile=" + std::to_string(remote_stream_selection.encoding_profile_id) +
            " quality=" + std::to_string(static_cast<uint32_t>(remote_stream_selection.quality_tier)) +
            " generation=" + std::to_string(remote_generation));
    }

    const bool first_control = last_applied_frame_id == 0;
    bool camera_cut = false;
    if (has_last_applied_control)
    {
        const bool projection_boundary =
            packet.scene_generation !=
                last_applied_control.scene_generation ||
            packet.viewport_width !=
                last_applied_control.viewport_width ||
            packet.viewport_height !=
                last_applied_control.viewport_height ||
            std::abs(packet.near_plane -
                last_applied_control.near_plane) > 0.0001f ||
            std::abs(packet.far_plane -
                last_applied_control.far_plane) > 0.01f;
        const XMVECTOR old_eye =
            XMLoadFloat3(&last_applied_control.eye);
        const XMVECTOR new_eye = XMLoadFloat3(&packet.eye);
        const float eye_delta_sq = XMVectorGetX(
            XMVector3LengthSq(
                XMVectorSubtract(new_eye, old_eye)));
        const XMVECTOR old_forward = XMVector3Normalize(
            XMLoadFloat3(&last_applied_control.at));
        const XMVECTOR new_forward = XMVector3Normalize(
            XMLoadFloat3(&packet.at));
        const float forward_dot = XMVectorGetX(
            XMVector3Dot(old_forward, new_forward));
        camera_cut = projection_boundary ||
            eye_delta_sq > 4.0f || forward_dot < 0.8f;
    }
    DDGIResetReason reset_reason = DDGIResetReason::None;
    if (has_ddgi_reset_reference_control)
    {
        if (packet.scene_generation != ddgi_reset_reference_control.scene_generation)
        {
            reset_reason = DDGIResetReason::SceneGeneration;
        }
        else
        {
            const XMVECTOR old_direction = XMLoadFloat3(&ddgi_reset_reference_control.sun_direction);
            const XMVECTOR new_direction = XMLoadFloat3(&packet.sun_direction);
            const float direction_dot = XMVectorGetX(XMVector3Dot(
                XMVector3Normalize(old_direction), XMVector3Normalize(new_direction)));
            const float color_delta = XMVectorGetX(XMVector3LengthSq(
                XMVectorSubtract(XMLoadFloat3(&ddgi_reset_reference_control.sun_color), XMLoadFloat3(&packet.sun_color))));
            if (packet.sun_enabled != ddgi_reset_reference_control.sun_enabled || direction_dot < 0.999f ||
                std::abs(packet.sun_intensity - ddgi_reset_reference_control.sun_intensity) > 0.05f || color_delta > 0.0025f)
            {
                reset_reason = DDGIResetReason::LightingChanged;
            }
        }
    }
    ApplyControlPacketToCameraAndScene(packet, local_camera, local_scene);
    if (reset_reason != DDGIResetReason::None)
    {
        ResetDDGI(reset_reason);
        ddgi_reset_reference_control = packet;
    }
    else if (camera_cut)
    {
        ++remote_generation;
    }
    if (camera_cut)
    {
        camera_cut_pending = true;
        packed_layout_contract_valid = false;
        transport_preview_available_mask = 0;
        remote_content_states = {};
        remote_content_control_frame_id = 0;
        for (PackedReadbackSlot& slot : packed_readback_ring)
            slot.pending = false;
        std::lock_guard lock(publish_mutex);
        pending_video_frame.reset();
    }
    last_applied_frame_id = packet.frame_id;
    last_applied_control = packet;
    has_last_applied_control = true;
    if (!has_ddgi_reset_reference_control)
    {
        ddgi_reset_reference_control = packet;
        has_ddgi_reset_reference_control = true;
    }
    if (first_control)
    {
        wi::backlog::post(
            "Server WebRTC control applied first frame: " +
            std::to_string(last_applied_frame_id) +
            " sun=" + (packet.sun_enabled ? std::string("enabled") : std::string("disabled")));
    }
}

void NewPipelineServerRenderPath::ResetDDGI(DDGIResetReason reason)
{
    // Scene::Update increments before renderer::DDGI. Wrapping UINT_MAX makes
    // the renderer observe frame_index==0 and clear every DDGI history resource.
    local_scene.ddgi.frame_index = std::numeric_limits<uint32_t>::max();
    ddgi_reset_reason = reason;
    ++ddgi_reset_serial;
    ++remote_generation;
    wi::backlog::post(std::string{"Server DDGI reset: "} + ToString(reason));
}

void NewPipelineServerRenderPath::LogDDGIStatusIfNeeded()
{
    if (ddgi_formal_status_logged)
        return;

    const wi::graphics::Texture& formal = GetDDGIRemoteIndirectDiffuseFormal();
    if (!settings.ddgi_enabled)
    {
        wi::backlog::post(
            "Server RemoteIndirectDiffuseFormal: DDGI disabled, remote publish skipped.");
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
