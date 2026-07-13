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
    return std::string{"DDGI | "} + (hardware_raytracing ? "RTAO | RT Reflection High | RT Shadow" :
        "MSAO full-res | SSR High | Screen Space Shadow");
}

std::string NewPipelineServerRenderPath::GetDebugStatusSummary() const
{
    size_t pending_count = 0;
    for (const ReadbackSlot& slot : readback_ring)
        pending_count += slot.pending ? 1u : 0u;
    const std::string shadow = authoritative_shadow_index < 16
        ? std::to_string(authoritative_shadow_index)
        : std::string{"unavailable"};
    return GetEffectiveAlgorithmSummary() + "\nSun shadow slice: " + shadow +
        "\nReadback: async ring 3, pending " + std::to_string(pending_count) +
        "\nDDGI: frame " + std::to_string(local_scene.ddgi.frame_index) +
        (local_scene.ddgi.frame_index >= 64 ? " converged" : " warming") +
        " reset=" + ToString(ddgi_reset_reason);
}

void NewPipelineServerRenderPath::Start()
{
    std::string codec_test_error;
    if (!ValidateRemoteVideoV2RoundTrip(&codec_test_error))
        wi::backlog::post("Remote video V2 self-test failed: " + codec_test_error);
    else
        wi::backlog::post("Remote video V2 self-test passed: LogHDR + scalar luma + padding.");
    InitializeSceneIfNeeded();
    ConfigureDDGI();
    wi::RenderPath3D::Start();
    StartPublishWorker();
    if (config.remote_source == RemoteSourceMode::WebRTC)
    {
        std::string error;
        std::lock_guard lock(webrtc_mutex);
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
    if (!rtShadow.IsValid())
    {
        shadow_snapshot_valid = false;
        return;
    }
    if (!EnsureShadowSliceTexture(rtShadow.GetDesc().width, rtShadow.GetDesc().height))
    {
        shadow_snapshot_valid = false;
        return;
    }
    authoritative_shadow_index = GetNewPipelineSunShadowIndex(local_scene);
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
    case DebugPreviewMode::LocalShadowVisibility:
        return shadow_snapshot_valid && shadow_slice_texture.IsValid() ? &shadow_slice_texture : nullptr;
    case DebugPreviewMode::TransportIndirectDiffuse:
        return transport_textures[0].IsValid() ? &transport_textures[0] : nullptr;
    case DebugPreviewMode::TransportAO:
        return transport_textures[1].IsValid() ? &transport_textures[1] : nullptr;
    case DebugPreviewMode::TransportSpecularIndirect:
        return transport_textures[2].IsValid() ? &transport_textures[2] : nullptr;
    case DebugPreviewMode::TransportShadowVisibility:
        return transport_textures[3].IsValid() ? &transport_textures[3] : nullptr;
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
        fx.quality = wi::image::QUALITY_LINEAR;
        fx.sampleFlag = wi::image::SAMPLEMODE_CLAMP;
        fx.enableFullScreen();
        if (debug_preview_mode == DebugPreviewMode::LocalSpecularIndirect)
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
    webrtc_transport.Tick();
    if (config.remote_source != RemoteSourceMode::WebRTC)
        return;
    std::lock_guard lock(webrtc_mutex);
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

    const std::array<const wi::graphics::Texture*, static_cast<size_t>(RemoteBufferSemantic::Count)> sources = {
        settings.ddgi_enabled ? &GetDDGIRemoteIndirectDiffuseFormal() : nullptr,
        local_ao_snapshot.IsValid() ? &local_ao_snapshot : nullptr,
        rtSSR.IsValid() ? &rtSSR : nullptr,
        shadow_slice_texture.IsValid() ? &shadow_slice_texture : nullptr,
    };
    ReadbackSlot& slot = readback_ring[readback_write_index];
    if (slot.pending)
    {
        // The ring only advances when a copy is submitted. A still-pending slot
        // means the producer outran the three-frame latency, so drop this capture
        // instead of ever waiting for the GPU on the render thread.
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
    slot.metadata.confidence = available_mask == static_cast<uint32_t>(RemoteBufferKind::All) ? 1.0f : 0.75f;
    slot.metadata.valid = true;
    slot.metadata.ddgi_frame_index = local_scene.ddgi.frame_index;
    slot.metadata.ddgi_reset_reason = ddgi_reset_reason;
    slot.available_mask = available_mask;
    slot.pending = true;
    device->SubmitCommandLists();
    readback_write_index = (readback_write_index + 1) % kReadbackRingSize;
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
        destination.encoding =
            semantic == RemoteBufferSemantic::RemoteIndirectDiffuse ||
            semantic == RemoteBufferSemantic::RemoteSpecularIndirect
            ? RemoteBufferEncoding::LogHDR16F
            : RemoteBufferEncoding::ScalarLuma8;
        destination.payload_rgba8.resize(static_cast<size_t>(desc.width) * desc.height * 4);
        const uint8_t* source = static_cast<const uint8_t*>(readback.mapped_data);
        const uint32_t source_pitch = readback.mapped_subresources[0].row_pitch;
        const size_t destination_pitch = static_cast<size_t>(desc.width) * 4;
        for (uint32_t y = 0; y < desc.height; ++y)
        {
            std::memcpy(destination.payload_rgba8.data() + y * destination_pitch,
                source + static_cast<size_t>(y) * source_pitch, destination_pitch);
        }
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
        for (;;)
        {
            RemoteRawFrame frame;
            {
                std::unique_lock lock(publish_mutex);
                publish_cv.wait(lock, [this]() { return publish_worker_stop || pending_publish_frame.has_value(); });
                if (publish_worker_stop && !pending_publish_frame.has_value())
                    return;
                frame = std::move(*pending_publish_frame);
                pending_publish_frame.reset();
            }

            std::string error;
            bool published = false;
            if (config.remote_source == RemoteSourceMode::Mock)
            {
                published = mock_remote_mailbox.PublishLatest(frame, &error);
            }
            else
            {
                std::lock_guard transport_lock(webrtc_mutex);
                published = webrtc_transport.SendFrame(frame);
                if (!published)
                    error = webrtc_transport.GetStats().status;
            }
            if (!published)
                wi::backlog::post("Server remote publish failed: " +
                    (error.empty() ? std::string{"unknown transport error"} : error));
            else if (frame.metadata.frame_id == 1)
                wi::backlog::post("Server remote published first asynchronous frame: " +
                    std::to_string(frame.metadata.width) + "x" + std::to_string(frame.metadata.height));
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
        pending_publish_frame = std::move(frame);
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
    wi::backlog::post(config.remote_source == RemoteSourceMode::Mock
        ? "Server using file mock control source: " + mock_control_mailbox.GetRootDirectory()
        : "Server using WebRTC DataChannel for client control only; frame output is video-track only.");
    wi::backlog::post("Server DDGI grid dimensions: " +
        std::to_string(local_scene.ddgi.grid_dimensions.x) + " x " +
        std::to_string(local_scene.ddgi.grid_dimensions.y) + " x " +
        std::to_string(local_scene.ddgi.grid_dimensions.z) + " (scene/Editor setting).");

    scene_initialized = true;
    ResetDDGI(DDGIResetReason::InitialScene);
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
    setSSRQuality(wi::renderer::PostProcessQuality::High);
    setAO(hardware_raytracing ? wi::RenderPath3D::AO_RTAO : wi::RenderPath3D::AO_MSAO);
    setRaytracedReflectionsEnabled(hardware_raytracing);
    setSSREnabled(!hardware_raytracing);
    setShadowsEnabled(true);
    wi::renderer::SetShadowsEnabled(true);
    wi::renderer::SetRaytracedShadowsEnabled(hardware_raytracing);
    wi::renderer::SetScreenSpaceShadowsEnabled(!hardware_raytracing);

    wi::backlog::post(std::string{"Server remote algorithms: IndirectDiffuse=DDGI AO="} +
        (hardware_raytracing ? "RTAO" : "MSAO(full-res fallback)") +
        " SpecularIndirect=" + (hardware_raytracing ? "RTReflection(High)" : "SSR(High fallback)") +
        " ShadowVisibility=" + (hardware_raytracing ? "RTShadow" : "ScreenSpaceShadow(fallback)") +
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
