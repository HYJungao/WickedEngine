#include "NewPipelineServerRenderPath.h"

#include "wiHelper.h"
#include "wiImage.h"
#include "wiFont.h"
#include "wiTextureHelper.h"

#include <algorithm>
#include <chrono>
#include <cstring>

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
    for (const ReadbackSlot& slot : readback_ring)
        pending_count += slot.pending ? 1u : 0u;
    for (const PackedReadbackSlot& slot : packed_readback_ring)
        pending_count += slot.pending ? 1u : 0u;
    const std::string shadow = authoritative_shadow_index < 16
        ? std::to_string(authoritative_shadow_index)
        : std::string{"unavailable"};
    const WebRTCTransportStats transport = webrtc_transport.GetStats();
    const std::string transport_status = config.remote_source == RemoteSourceMode::WebRTC
        ? "\nWebRTC: " + std::string{ToString(transport.state)} + " " + transport.codec_name +
            (transport.native_codec ? " native-surface" :
                (transport.power_efficient_codec ? " power-efficient" : " software-surface")) +
            " impl=" + transport.codec_implementation +
            " encode=" + std::to_string(transport.total_encode_time_usec / 1000u) + " ms" +
            " net=" + std::to_string(transport.compressed_bytes_sent / 1024u) + " KiB" +
            " captures=" + std::to_string(remote_capture_count) +
            " capture-drop=" + std::to_string(remote_capture_drops) +
            " queue-drop=" + std::to_string(publish_queue_drops.load(std::memory_order_relaxed)) +
            " readback=" + std::to_string(gpu_readback_bytes / 1024u) + " KiB" +
            " cpu-copy=" + std::to_string(
                (cpu_readback_copy_bytes + transport.cpu_full_frame_copy_bytes) / 1024u) + " KiB" +
            " convert=" + std::to_string(transport.cpu_conversion_usec / 1000u) + " ms" +
            (transport.native_codec || transport.codec_fallback_reason.empty()
                ? std::string{} : " fallback=" + transport.codec_fallback_reason)
        : std::string{};
    return GetEffectiveAlgorithmSummary() + "\nSun shadow slice: " + shadow +
        " stable-id=" + std::to_string(authoritative_shadow_light_id) +
        " generation=" + std::to_string(authoritative_shadow_light_generation) +
        "\nReadback: async ring 3, pending " + std::to_string(pending_count) +
        transport_status +
        "\nDDGI: frame " + std::to_string(local_scene.ddgi.frame_index) +
        (local_scene.ddgi.frame_index >= 64 ? " converged" : " warming") +
        " reset=" + ToString(ddgi_reset_reason);
}

void NewPipelineServerRenderPath::Start()
{
    std::string codec_test_error;
    if (!ValidateRemoteTransportSelfTest(&codec_test_error))
        wi::backlog::post("Remote transport self-test failed: " + codec_test_error);
    else
        wi::backlog::post("Remote transport self-test passed: V2 codec plus V3 formal contracts.");
    InitializeSceneIfNeeded();
    ConfigureDDGI();
    // SpecularIndirectPreAO is emitted by Visibility_Shade. Keep Server and
    // Client on the same formal-output stage.
    setVisibilityComputeShadingEnabled(true);
    wi::RenderPath3D::Start();
    StartPublishWorker();
    if (config.remote_source == RemoteSourceMode::WebRTC)
    {
        std::string error;
        if (!webrtc_transport.RequestStart(true, config, &error))
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
    local_specular_indirect_pre_ao = {};
    visibilityResources.texture_specular_indirect_pre_ao = nullptr;
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
    if (!hardware_raytracing || !rtShadow.IsValid())
    {
        shadow_snapshot_valid = false;
        return;
    }
    if (!EnsureShadowSliceTexture(rtShadow.GetDesc().width, rtShadow.GetDesc().height))
    {
        shadow_snapshot_valid = false;
        return;
    }
    RefreshAuthoritativeShadowIdentity();
    if (authoritative_shadow_index >= rtShadow.GetDesc().array_size)
    {
        shadow_snapshot_valid = false;
        return;
    }

    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    const wi::graphics::GPUBarrier before[] = {
        wi::graphics::GPUBarrier::Image(
            &rtShadow, rtShadow.GetDesc().layout, wi::graphics::ResourceState::COPY_SRC, -1,
            static_cast<int>(authoritative_shadow_index)),
        wi::graphics::GPUBarrier::Image(
            &shadow_slice_texture, shadow_slice_texture.GetDesc().layout, wi::graphics::ResourceState::COPY_DST),
    };
    device->Barrier(before, static_cast<uint32_t>(std::size(before)), cmd);
    device->CopyTexture(
        &shadow_slice_texture, 0, 0, 0, 0, 0,
        &rtShadow, 0, authoritative_shadow_index, cmd);
    shadow_snapshot_valid = true;
    const wi::graphics::GPUBarrier after[] = {
        wi::graphics::GPUBarrier::Image(
            &rtShadow, wi::graphics::ResourceState::COPY_SRC, rtShadow.GetDesc().layout, -1,
            static_cast<int>(authoritative_shadow_index)),
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
    case DebugPreviewMode::LocalSpecularIndirectPreAO:
        return local_specular_indirect_pre_ao.IsValid() ? &local_specular_indirect_pre_ao : nullptr;
    case DebugPreviewMode::LocalShadowVisibility:
        return shadow_snapshot_valid && shadow_slice_texture.IsValid() ? &shadow_slice_texture : nullptr;
    case DebugPreviewMode::TransportIndirectDiffuse:
    {
        const size_t index = static_cast<size_t>(RemoteBufferSemantic::RemoteIndirectDiffuse);
        return transport_textures[index].IsValid() ? &transport_textures[index] : nullptr;
    }
    case DebugPreviewMode::TransportAO:
    {
        const size_t index = static_cast<size_t>(RemoteBufferSemantic::RemoteAO);
        return transport_textures[index].IsValid() ? &transport_textures[index] : nullptr;
    }
    case DebugPreviewMode::TransportSpecularIndirect:
    {
        const size_t index = static_cast<size_t>(RemoteBufferSemantic::RemoteSpecularIndirect);
        return transport_textures[index].IsValid() ? &transport_textures[index] : nullptr;
    }
    case DebugPreviewMode::TransportShadowVisibility:
    {
        const size_t index = static_cast<size_t>(RemoteBufferSemantic::RemoteShadowVisibility);
        return transport_textures[index].IsValid() ? &transport_textures[index] : nullptr;
    }
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
    (void)dt;
    webrtc_transport.Tick();
    if (config.remote_source != RemoteSourceMode::WebRTC)
        return;
    const WebRTCTransportStats stats = webrtc_transport.GetStats();
    if (stats.state == previous_webrtc_state)
        return;
    wi::backlog::post("Server WebRTC " + std::string{ToString(previous_webrtc_state)} + " -> " +
        ToString(stats.state) + (stats.status.empty() ? std::string{} : ": " + stats.status));
    if (previous_webrtc_state == WebRTCTransportState::Connected &&
        stats.state != WebRTCTransportState::Connected)
    {
        for (ReadbackSlot& slot : readback_ring)
        {
            slot.pending = false;
            slot.available_mask = 0;
        }
        for (PackedReadbackSlot& slot : packed_readback_ring)
            slot.pending = false;
        readback_write_index = 0;
        packed_readback_write_index = 0;
        std::lock_guard lock(publish_mutex);
        pending_publish_frame.reset();
        pending_i420_frame.reset();
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
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE | wi::graphics::BindFlag::RENDER_TARGET;
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
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
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

void NewPipelineServerRenderPath::EncodeTransportTexture(
    RemoteBufferSemantic semantic,
    const wi::graphics::Texture& source,
    wi::graphics::Texture& destination,
    wi::graphics::CommandList cmd) const
{
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
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
}

bool NewPipelineServerRenderPath::EnsureShadowSliceTexture(uint32_t width, uint32_t height) const
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
    if (config.remote_source == RemoteSourceMode::WebRTC &&
        webrtc_transport.GetStats().state != WebRTCTransportState::Connected)
    {
        mock_publish_accumulator = 0.0f;
        return;
    }

    if (config.remote_source == RemoteSourceMode::WebRTC)
        ConsumeCompletedPackedReadback();
    else
        ConsumeCompletedReadback();

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

    std::array<const wi::graphics::Texture*, static_cast<size_t>(RemoteBufferSemantic::Count)> sources = {};
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteIndirectDiffuse)] =
        settings.ddgi_enabled ? &GetDDGIRemoteIndirectDiffuseFormal() : nullptr;
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteAO)] =
        local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr;
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteSpecularIndirect)] =
        rtSSR.IsValid() ? &rtSSR : nullptr;
    sources[static_cast<size_t>(RemoteBufferSemantic::RemoteShadowVisibility)] =
        shadow_snapshot_valid && shadow_slice_texture.IsValid() ? &shadow_slice_texture : nullptr;
    if (config.remote_source == RemoteSourceMode::WebRTC)
    {
        CapturePackedRemoteFrame(sources);
        return;
    }
    ReadbackSlot& slot = readback_ring[readback_write_index];
    if (slot.pending)
    {
        // The ring only advances when a copy is submitted. A still-pending slot
        // means the producer outran the three-frame latency, so drop this capture
        // instead of ever waiting for the GPU on the render thread.
        ++remote_capture_drops;
        return;
    }

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

        EncodeTransportTexture(semantic, *source, transport_textures[index], cmd);

        wi::graphics::Texture& readback = slot.textures[index];
        const auto& transport_desc = transport_textures[index].GetDesc();
        if (!readback.IsValid() || readback.GetDesc().width != transport_desc.width ||
            readback.GetDesc().height != transport_desc.height)
        {
            wi::graphics::TextureDesc readback_desc = transport_desc;
            readback_desc.usage = wi::graphics::Usage::READBACK;
            readback_desc.bind_flags = wi::graphics::BindFlag::NONE;
            readback_desc.layout = wi::graphics::ResourceState::COPY_DST;
            if (!device->CreateTexture(&readback_desc, nullptr, &readback))
                continue;
            const std::string name = "newpipeline.readback[" + std::to_string(readback_write_index) + "]." +
                ToString(semantic);
            device->SetName(&readback, name.c_str());
        }
        device->Barrier(wi::graphics::GPUBarrier::Image(
            &transport_textures[index], transport_desc.layout, wi::graphics::ResourceState::COPY_SRC), cmd);
        device->CopyResource(&readback, &transport_textures[index], cmd);
        gpu_readback_bytes += static_cast<uint64_t>(transport_desc.width) * transport_desc.height * 4u;
        device->Barrier(wi::graphics::GPUBarrier::Image(
            &transport_textures[index], wi::graphics::ResourceState::COPY_SRC, transport_desc.layout), cmd);
        available_mask |= RemoteBufferKindMask(semantic);
    }
    if (available_mask == 0)
        return;
    slot.metadata = {};
    slot.metadata.frame_id = ++remote_frame_id;
    slot.metadata.timestamp_usec = NowUsec();
    slot.metadata.source_generation = remote_generation;
    slot.metadata.continuity_mask = available_mask;
    slot.metadata.available_buffer_mask = available_mask;
    slot.metadata.dynamic_range = RemoteDynamicRange::HDR;
    slot.metadata.source_stream_id = kRemoteFrameStreamId;
    slot.metadata.view_origin = local_camera.Eye;
    XMStoreFloat3(&slot.metadata.view_forward,
        XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&local_camera.At), XMLoadFloat3(&local_camera.Eye))));
    slot.metadata.view = local_camera.View;
    slot.metadata.projection = local_camera.Projection;
    XMStoreFloat4x4(&slot.metadata.view_projection,
        XMMatrixMultiply(XMLoadFloat4x4(&local_camera.View), XMLoadFloat4x4(&local_camera.Projection)));
    XMStoreFloat4x4(&slot.metadata.inverse_view, XMMatrixInverse(nullptr, XMLoadFloat4x4(&slot.metadata.view)));
    XMStoreFloat4x4(&slot.metadata.inverse_projection, XMMatrixInverse(nullptr, XMLoadFloat4x4(&slot.metadata.projection)));
    XMStoreFloat4x4(&slot.metadata.inverse_view_projection,
        XMMatrixInverse(nullptr, XMLoadFloat4x4(&slot.metadata.view_projection)));
    slot.metadata.near_plane = local_camera.zNearP;
    slot.metadata.far_plane = local_camera.zFarP;
    slot.metadata.history_valid = local_scene.ddgi.frame_index >= 64;
    slot.metadata.reset_this_frame = ddgi_announced_reset_serial != ddgi_reset_serial;
    if (slot.metadata.reset_this_frame)
        ddgi_announced_reset_serial = ddgi_reset_serial;
    // Buffer availability is described independently by available_buffer_mask.
    // Missing an optional reflection or shadow buffer must not reduce the
    // confidence of a valid DDGI or AO tile.
    slot.metadata.confidence = 1.0f;
    slot.metadata.valid = true;
    slot.metadata.ddgi_frame_index = local_scene.ddgi.frame_index;
    slot.metadata.ddgi_reset_reason = ddgi_reset_reason;
    slot.available_mask = available_mask;
    slot.pending = true;
    ++remote_capture_count;
    device->SubmitCommandLists();
    readback_write_index = (readback_write_index + 1) % kReadbackRingSize;
}

void NewPipelineServerRenderPath::CapturePackedRemoteFrame(
    const std::array<const wi::graphics::Texture*, static_cast<size_t>(RemoteBufferSemantic::Count)>& sources)
{
    PackedReadbackSlot& slot = packed_readback_ring[packed_readback_write_index];
    if (slot.pending || (slot.readback && slot.readback.use_count() > 1))
    {
        ++remote_capture_drops;
        return;
    }

    RemoteRawFrame contract;
    uint32_t available_mask = 0;
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const wi::graphics::Texture* source = sources[index];
        RemoteRawBuffer& buffer = contract.buffers[index];
        buffer.semantic = static_cast<RemoteBufferSemantic>(index);
        buffer.encoding = RemoteBufferTransportEncoding(buffer.semantic);
        if (source == nullptr || !source->IsValid())
            continue;
        const wi::graphics::TextureDesc& desc = source->GetDesc();
        buffer.width = desc.width;
        buffer.height = desc.height;
        if (!EnsureTransportTexture(buffer.semantic, desc.width, desc.height))
            continue;
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
        XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&local_camera.At), XMLoadFloat3(&local_camera.Eye))));
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
    metadata.confidence = 1.0f;
    metadata.valid = true;
    metadata.ddgi_frame_index = local_scene.ddgi.frame_index;
    metadata.ddgi_reset_reason = ddgi_reset_reason;

    RemoteVideoFrameLayout layout;
    std::vector<uint8_t> metadata_luma;
    std::string layout_error;
    if (!BuildRemoteVideoFrameLayout(contract, layout, metadata_luma, &layout_error))
    {
        wi::backlog::post("Server GPU I420 layout failed: " + layout_error);
        return;
    }
    if (packed_layout_width != layout.video_width || packed_layout_height != layout.video_height)
    {
        packed_layout_width = layout.video_width;
        packed_layout_height = layout.video_height;
        ++remote_generation;
        metadata.source_generation = remote_generation;
        if (!BuildRemoteVideoFrameLayout(contract, layout, metadata_luma, &layout_error))
        {
            wi::backlog::post("Server GPU I420 layout rebuild failed: " + layout_error);
            return;
        }
    }
    if (!EnsureTransportAtlasTexture(layout.video_width, layout.video_height))
        return;

    const uint32_t y_stride = (layout.video_width + 3u) & ~3u;
    const uint32_t uv_stride = (layout.video_width / 2u + 3u) & ~3u;
    const uint32_t u_offset = y_stride * layout.video_height;
    const uint32_t v_offset = u_offset + uv_stride * (layout.video_height / 2u);
    const uint64_t packed_size = static_cast<uint64_t>(v_offset) +
        static_cast<uint64_t>(uv_stride) * (layout.video_height / 2u);
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
        device->SetName(&slot.packed_gpu, "newpipeline.server.i420.packed_gpu");
    }
    if (!slot.readback || !slot.readback->buffer.IsValid() || slot.readback->buffer.GetDesc().size != packed_size)
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
    if (slot.metadata_upload.mapped_data == nullptr || slot.readback->buffer.mapped_data == nullptr)
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
    for (size_t index = 0; index < layout.tiles.size(); ++index)
    {
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        pack_desc.tile_rects[index] = XMUINT4(tile.origin_x, tile.origin_y, tile.width, tile.height);
    }

    wi::graphics::CommandList cmd = device->BeginCommandList();
    // These are the canonical pre-I420 transport surfaces. The Server preview
    // and the encoder now observe the same resources, so semantic inspection
    // cannot diverge from the data actually submitted to WebRTC.
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        EncodeTransportTexture(
            semantic,
            *sources[index],
            transport_textures[index],
            cmd);
    }

    // Assemble one canonical RGBA8 atlas with exact GPU copies. The I420 pass
    // consumes a single SRV, which makes the semantic-to-rectangle contract
    // explicit and avoids backend-dependent parallel SRV binding behavior.
    wi::graphics::GPUBarrier atlas_barriers[static_cast<size_t>(RemoteBufferSemantic::Count) + 1] = {};
    uint32_t atlas_barrier_count = 0;
    atlas_barriers[atlas_barrier_count++] = wi::graphics::GPUBarrier::Image(
        &transport_atlas_texture,
        transport_atlas_texture.GetDesc().layout,
        wi::graphics::ResourceState::COPY_DST);
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        atlas_barriers[atlas_barrier_count++] = wi::graphics::GPUBarrier::Image(
            &transport_textures[index],
            transport_textures[index].GetDesc().layout,
            wi::graphics::ResourceState::COPY_SRC);
    }
    device->Barrier(atlas_barriers, atlas_barrier_count, cmd);
    for (size_t index = 0; index < sources.size(); ++index)
    {
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if ((available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        const RemoteVideoTileLayout& tile = layout.tiles[index];
        device->CopyTexture(
            &transport_atlas_texture,
            tile.origin_x,
            tile.origin_y,
            0,
            0,
            0,
            &transport_textures[index],
            0,
            0,
            cmd);
    }
    for (uint32_t index = 0; index < atlas_barrier_count; ++index)
        std::swap(atlas_barriers[index].image.layout_before, atlas_barriers[index].image.layout_after);
    device->Barrier(atlas_barriers, atlas_barrier_count, cmd);

    wi::renderer::RGB_to_I420_Atlas(
        transport_atlas_texture, slot.metadata_upload, slot.packed_gpu, pack_desc, cmd);
    device->Barrier(wi::graphics::GPUBarrier::Buffer(
        &slot.packed_gpu, wi::graphics::ResourceState::UNORDERED_ACCESS, wi::graphics::ResourceState::COPY_SRC), cmd);
    device->CopyResource(&slot.readback->buffer, &slot.packed_gpu, cmd);
    device->Barrier(wi::graphics::GPUBarrier::Buffer(
        &slot.packed_gpu, wi::graphics::ResourceState::COPY_SRC, wi::graphics::ResourceState::UNORDERED_ACCESS), cmd);

    slot.layout = std::move(layout);
    slot.y_stride = y_stride;
    slot.uv_stride = uv_stride;
    slot.u_offset = u_offset;
    slot.v_offset = v_offset;
    slot.pending = true;
    // Wicked waits for the fence associated with a buffered frame index before
    // reusing that index.  At this frame count the copy recorded below is
    // therefore complete without a render-thread WaitForGPU().
    slot.gpu_ready_frame = device->GetFrameCount() + wi::graphics::GraphicsDevice::GetBufferCount();
    remote_frame_id = metadata.frame_id;
    if (metadata.reset_this_frame)
        ddgi_announced_reset_serial = ddgi_reset_serial;
    ++remote_capture_count;
    gpu_readback_bytes += packed_size;
    device->SubmitCommandLists();
    packed_readback_write_index = (packed_readback_write_index + 1u) % kReadbackRingSize;
}

void NewPipelineServerRenderPath::ConsumeCompletedPackedReadback()
{
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    PackedReadbackSlot* completed = nullptr;
    for (PackedReadbackSlot& candidate : packed_readback_ring)
    {
        if (!candidate.pending || !candidate.readback ||
            candidate.readback->buffer.mapped_data == nullptr ||
            device->GetFrameCount() < candidate.gpu_ready_frame)
            continue;
        if (completed == nullptr ||
            candidate.layout.metadata.frame_id < completed->layout.metadata.frame_id)
            completed = &candidate;
    }
    if (completed == nullptr)
        return;

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

void NewPipelineServerRenderPath::ConsumeCompletedReadback()
{
    ReadbackSlot& slot = readback_ring[readback_write_index];
    if (!slot.pending)
        return;

    RemoteRawFrame frame;
    frame.metadata = slot.metadata;
    for (size_t index = 0; index < frame.buffers.size(); ++index)
    {
        const RemoteBufferSemantic semantic = static_cast<RemoteBufferSemantic>(index);
        if ((slot.available_mask & RemoteBufferKindMask(semantic)) == 0)
            continue;
        const wi::graphics::Texture& readback = slot.textures[index];
        if (!readback.IsValid() || readback.mapped_data == nullptr ||
            readback.mapped_subresources == nullptr || readback.mapped_subresource_count == 0)
        {
            frame.metadata.continuity_mask &= ~RemoteBufferKindMask(semantic);
            frame.metadata.available_buffer_mask &= ~RemoteBufferKindMask(semantic);
            continue;
        }
        const auto& desc = readback.GetDesc();
        RemoteRawBuffer& destination = frame.buffers[index];
        destination.width = desc.width;
        destination.height = desc.height;
        destination.available = true;
        destination.encoding = RemoteBufferTransportEncoding(semantic);
        destination.payload_rgba8.resize(static_cast<size_t>(desc.width) * desc.height * 4);
        const uint8_t* source = static_cast<const uint8_t*>(readback.mapped_data);
        const uint32_t source_pitch = readback.mapped_subresources[0].row_pitch;
        const size_t destination_pitch = static_cast<size_t>(desc.width) * 4;
        for (uint32_t y = 0; y < desc.height; ++y)
        {
            std::memcpy(destination.payload_rgba8.data() + y * destination_pitch,
                source + static_cast<size_t>(y) * source_pitch, destination_pitch);
        }
        cpu_readback_copy_bytes += static_cast<uint64_t>(destination_pitch) * desc.height;
        frame.metadata.width = std::max(frame.metadata.width, desc.width);
        frame.metadata.height = std::max(frame.metadata.height, desc.height);
    }
    slot.pending = false;
    slot.available_mask = 0;
    if (frame.metadata.available_buffer_mask == 0)
        return;

    QueueFrameForPublish(std::move(frame));
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
            RemoteRawFrame frame;
            RetainedI420Frame i420_frame;
            RemoteVideoFrameLayout i420_layout;
            bool has_raw_frame = false;
            bool has_i420_frame = false;
            {
                std::unique_lock lock(publish_mutex);
                publish_cv.wait(lock, [this]() {
                    return publish_worker_stop || pending_publish_frame.has_value() || pending_i420_frame.has_value();
                });
                if (publish_worker_stop && !pending_publish_frame.has_value() && !pending_i420_frame.has_value())
                    return;
                if (pending_i420_frame.has_value())
                {
                    i420_frame = std::move(pending_i420_frame->frame);
                    i420_layout = std::move(pending_i420_frame->layout);
                    pending_i420_frame.reset();
                    has_i420_frame = true;
                }
                else if (pending_publish_frame.has_value())
                {
                    frame = std::move(*pending_publish_frame);
                    pending_publish_frame.reset();
                    has_raw_frame = true;
                }
            }

            std::string error;
            bool published = false;
            if (has_i420_frame)
            {
                if (i420_layout.metadata.camera_cut ||
                    i420_layout.metadata.source_generation != published_generation)
                {
                    webrtc_transport.RequestKeyframe();
                }
                const bool metadata_sent = webrtc_transport.SendFrameMetadata(i420_layout);
                published = metadata_sent && webrtc_transport.SendI420Frame(i420_frame);
                if (published)
                    published_generation = i420_layout.metadata.source_generation;
                if (!published)
                    error = webrtc_transport.GetStats().status;
            }
            else if (has_raw_frame && config.remote_source == RemoteSourceMode::Mock)
            {
                published = mock_remote_mailbox.PublishLatest(frame, &error);
            }
            else if (has_raw_frame)
            {
                published = webrtc_transport.SendFrame(frame);
                if (!published)
                    error = webrtc_transport.GetStats().status;
            }
            if (!published)
                wi::backlog::post("Server remote publish failed: " +
                    (error.empty() ? std::string{"unknown transport error"} : error));
            else if ((has_raw_frame && frame.metadata.frame_id == 1) ||
                (has_i420_frame && !first_packed_frame_published))
            {
                wi::backlog::post("Server remote published first asynchronous frame: " +
                    std::to_string(has_i420_frame ? i420_frame.width : frame.metadata.width) + "x" +
                    std::to_string(has_i420_frame ? i420_frame.height : frame.metadata.height));
                first_packed_frame_published = first_packed_frame_published || has_i420_frame;
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

void NewPipelineServerRenderPath::QueueFrameForPublish(RemoteRawFrame&& frame)
{
    {
        std::lock_guard lock(publish_mutex);
        // Latest-frame semantics: encoding never builds a backlog behind rendering.
        if (pending_publish_frame.has_value())
            publish_queue_drops.fetch_add(1, std::memory_order_relaxed);
        pending_publish_frame = std::move(frame);
    }
    publish_cv.notify_one();
}

void NewPipelineServerRenderPath::QueueI420FrameForPublish(
    RetainedI420Frame&& frame, const RemoteVideoFrameLayout& layout)
{
    {
        std::lock_guard lock(publish_mutex);
        if (pending_i420_frame.has_value())
            publish_queue_drops.fetch_add(1, std::memory_order_relaxed);
        pending_i420_frame = PendingI420Publish{std::move(frame), layout};
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
    wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
        ? "Server using file mock control source: " + mock_control_mailbox.GetRootDirectory()
        : "Server using WebRTC DataChannel for client control only; frame output is video-track only.");
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
    const wi::ecs::Entity resolved =
        ResolveStableLightId(local_scene, authoritative_shadow_light_id);
    if (resolved != authoritative_shadow_light_entity)
    {
        authoritative_shadow_light_entity = resolved;
        if (resolved != wi::ecs::INVALID_ENTITY)
        {
            ++authoritative_shadow_light_generation;
            if (authoritative_shadow_light_generation == 0)
                authoritative_shadow_light_generation = 1;
        }
    }
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
        wi::backlog::post(std::string{config.remote_source == RemoteSourceMode::Mock
                ? "Server file mock control applied first frame: "
                : "Server WebRTC control applied first frame: "} + std::to_string(last_applied_frame_id) +
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
