#include "NewPipelineWindowsGPUInterop.h"

#include "NewPipelineTransport.h"

#include <string_view>

#if defined(_WIN32)
#include "wiGraphicsDevice_DX12.h"

#include <d3d12.h>
#include <wrl/client.h>
#endif

#if defined(_WIN32)
namespace wicked_newpipeline
{
namespace
{
constexpr uint32_t kMaxVideoDimension = 8192;

void SetError(std::string* error, const char* message)
{
    if (error != nullptr)
        *error = message;
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

uint64_t PackLuid(const LUID& luid)
{
    return static_cast<uint64_t>(static_cast<uint32_t>(luid.LowPart)) |
        (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32u);
}

wi::graphics::GraphicsDevice_DX12* GetDX12Device()
{
    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();
    if (device == nullptr || std::string_view(device->GetTag()) != "[DX12]")
        return nullptr;
    return static_cast<wi::graphics::GraphicsDevice_DX12*>(device);
}

struct ServerState
{
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_handle = nullptr;
    HANDLE texture_handle = nullptr;

    ~ServerState()
    {
        if (fence_handle != nullptr)
            CloseHandle(fence_handle);
        if (texture_handle != nullptr)
            CloseHandle(texture_handle);
    }
};

struct ClientState
{
    ComPtr<ID3D12Fence> fence;
};
#endif
} // namespace

uint64_t GetWindowsDX12AdapterLuid()
{
#if defined(_WIN32)
    auto* device = GetDX12Device();
    return device != nullptr ? PackLuid(device->GetAdapterLuid()) : 0;
#else
    return 0;
#endif
}

bool EnsureWindowsServerNV12Surface(
    uint32_t width,
    uint32_t height,
    WindowsServerNV12Surface& surface,
    WindowsNV12Footprint& footprint,
    std::string* error)
{
#if defined(_WIN32)
    auto* device = GetDX12Device();
    if (device == nullptr)
    {
        SetError(error, "native NV12 requires the WickedEngine DX12 backend");
        return false;
    }
    if (width == 0 || height == 0 ||
        width > kMaxVideoDimension || height > kMaxVideoDimension ||
        (width & 1u) != 0 || (height & 1u) != 0)
    {
        SetError(error, "native NV12 dimensions must be non-zero and even");
        return false;
    }
    if (!surface.texture.IsValid() || surface.width != width || surface.height != height)
    {
        WindowsServerNV12Surface replacement;
        auto state = std::make_shared<ServerState>();
        wi::graphics::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = wi::graphics::Format::NV12;
        desc.bind_flags = wi::graphics::BindFlag::NONE;
        desc.misc_flags = wi::graphics::ResourceMiscFlag::SHARED;
        // D3D12 owns the allocation and Media Foundation's D3D11 device opens
        // its NT handle. This is the documented D3D11/D3D12 interop direction
        // and avoids relying on incompatible D3D11 shared-resource flags.
        desc.layout = wi::graphics::ResourceState::COMMON;
        if (!device->CreateTexture(&desc, nullptr, &replacement.texture) ||
            replacement.texture.shared_handle == nullptr)
        {
            SetError(error, "DX12 shared NV12 texture creation failed");
            return false;
        }
        state->texture_handle = static_cast<HANDLE>(
            replacement.texture.shared_handle);

        if (FAILED(device->GetD3D12Device()->CreateFence(
                0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state->fence))) ||
            FAILED(device->GetD3D12Device()->CreateSharedHandle(
                state->fence.Get(), nullptr, GENERIC_ALL, nullptr,
                &state->fence_handle)))
        {
            SetError(error, "DX12 shared NV12 fence creation failed");
            return false;
        }
        replacement.width = width;
        replacement.height = height;
        replacement.adapter_luid = PackLuid(device->GetAdapterLuid());
        replacement.texture_shared_handle = state->texture_handle;
        replacement.fence_shared_handle = state->fence_handle;
        replacement.native_state = std::move(state);
        device->SetName(&replacement.texture, "newpipeline.server.native_nv12");
        surface = std::move(replacement);
    }

    ID3D12Resource* resource = device->GetTextureInternalResource(&surface.texture);
    if (resource == nullptr)
    {
        SetError(error, "DX12 NV12 native resource is unavailable");
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[2] = {};
    UINT rows[2] = {};
    UINT64 row_sizes[2] = {};
    UINT64 total_size = 0;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    device->GetD3D12Device()->GetCopyableFootprints(
        &desc, 0, 2, 0, layouts, rows, row_sizes, &total_size);
    if (layouts[0].Footprint.RowPitch == 0 ||
        layouts[1].Footprint.RowPitch == 0 || total_size == 0)
    {
        SetError(error, "DX12 NV12 copy footprint query failed");
        return false;
    }
    footprint.y_stride = layouts[0].Footprint.RowPitch;
    footprint.uv_stride = layouts[1].Footprint.RowPitch;
    footprint.uv_offset = layouts[1].Offset;
    footprint.total_size = total_size;
    return true;
#else
    (void)width; (void)height; (void)surface; (void)footprint;
    SetError(error, "Windows DX12 NV12 interop is not built on this platform");
    return false;
#endif
}

bool QueueWindowsServerNV12Copy(
    WindowsServerNV12Surface& surface,
    const WindowsNV12Footprint& footprint,
    const wi::graphics::GPUBuffer& packed_nv12,
    uint64_t wait_fence_value,
    uint64_t signal_fence_value,
    wi::graphics::CommandList cmd,
    std::string* error)
{
#if defined(_WIN32)
    auto* device = GetDX12Device();
    auto state = std::static_pointer_cast<ServerState>(surface.native_state);
    if (device == nullptr || !state || !state->fence || !surface.texture.IsValid())
    {
        SetError(error, "DX12 NV12 surface is not initialized");
        return false;
    }
    if (wait_fence_value != 0)
        device->WaitExternalFence(state->fence.Get(), wait_fence_value, cmd);
    const wi::graphics::GPUBarrier acquire = wi::graphics::GPUBarrier::Image(
        &surface.texture,
        wi::graphics::ResourceState::COMMON,
        wi::graphics::ResourceState::COPY_DST);
    device->Barrier(&acquire, 1, cmd);
    if (!device->CopyBufferToTexturePlane(
            &packed_nv12, 0, footprint.y_stride,
            &surface.texture, 0, surface.width, surface.height, cmd) ||
        !device->CopyBufferToTexturePlane(
            &packed_nv12, footprint.uv_offset, footprint.uv_stride,
            &surface.texture, 1, surface.width / 2u, surface.height / 2u, cmd))
    {
        SetError(error, "DX12 packed-buffer to NV12 texture copy failed");
        return false;
    }
    const wi::graphics::GPUBarrier release = wi::graphics::GPUBarrier::Image(
        &surface.texture,
        wi::graphics::ResourceState::COPY_DST,
        wi::graphics::ResourceState::COMMON);
    device->Barrier(&release, 1, cmd);
    device->SignalExternalFence(state->fence.Get(), signal_fence_value, cmd);
    return true;
#else
    (void)surface; (void)footprint; (void)packed_nv12;
    (void)wait_fence_value; (void)signal_fence_value; (void)cmd;
    SetError(error, "Windows DX12 NV12 interop is not built on this platform");
    return false;
#endif
}

bool OpenWindowsClientNV12Surface(
    const RetainedNV12Frame& frame,
    WindowsClientNV12Surface& surface,
    std::string* error)
{
#if defined(_WIN32)
    auto* device = GetDX12Device();
    if (device == nullptr || !frame.IsValid() ||
        frame.width > kMaxVideoDimension ||
        frame.height > kMaxVideoDimension)
    {
        SetError(error, "native decoded NV12 frame is invalid or DX12 is unavailable");
        return false;
    }
    const uint64_t adapter_luid = PackLuid(device->GetAdapterLuid());
    if (frame.adapter_luid == 0 || frame.adapter_luid != adapter_luid)
    {
        SetError(error, "decoded NV12 surface belongs to a different GPU adapter");
        return false;
    }
    if (surface.texture.IsValid() &&
        surface.texture_shared_handle == frame.texture_shared_handle &&
        surface.fence_shared_handle == frame.fence_shared_handle &&
        surface.width == frame.width && surface.height == frame.height)
        return true;

    WindowsClientNV12Surface replacement;
    wi::graphics::TextureDesc desc;
    desc.width = frame.width;
    desc.height = frame.height;
    desc.format = wi::graphics::Format::NV12;
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
    // Media Foundation/D3D11 releases the shared decoder surface to D3D12 in
    // COMMON. The render path explicitly acquires shader-read state for unpack
    // and releases COMMON again before signaling the consumer fence.
    desc.layout = wi::graphics::ResourceState::COMMON;
    if (!device->OpenSharedTexture(
            static_cast<HANDLE>(frame.texture_shared_handle), &desc,
            &replacement.texture))
    {
        SetError(error, "DX12 could not open the decoded NV12 shared handle");
        return false;
    }
    auto state = std::make_shared<ClientState>();
    if (FAILED(device->GetD3D12Device()->OpenSharedHandle(
            static_cast<HANDLE>(frame.fence_shared_handle),
            IID_PPV_ARGS(&state->fence))))
    {
        SetError(error, "DX12 could not open the decoded NV12 shared fence");
        return false;
    }
    wi::graphics::Format luminance_format = wi::graphics::Format::R8_UNORM;
    wi::graphics::Format chrominance_format = wi::graphics::Format::R8G8_UNORM;
    wi::graphics::ImageAspect luminance_aspect = wi::graphics::ImageAspect::LUMINANCE;
    wi::graphics::ImageAspect chrominance_aspect = wi::graphics::ImageAspect::CHROMINANCE;
    replacement.luminance_subresource = device->CreateSubresource(
        &replacement.texture, wi::graphics::SubresourceType::SRV,
        0, 1, 0, 1, &luminance_format, &luminance_aspect);
    replacement.chrominance_subresource = device->CreateSubresource(
        &replacement.texture, wi::graphics::SubresourceType::SRV,
        0, 1, 0, 1, &chrominance_format, &chrominance_aspect);
    // Wicked stores the first created SRV as the texture's default view and
    // returns -1 for binding it. That is the valid Y-plane view here; the
    // second (UV) view must be an indexed subresource.
    if (replacement.chrominance_subresource < 0)
    {
        SetError(error, "DX12 NV12 plane view creation failed");
        return false;
    }
    replacement.width = frame.width;
    replacement.height = frame.height;
    replacement.adapter_luid = adapter_luid;
    replacement.texture_shared_handle = frame.texture_shared_handle;
    replacement.fence_shared_handle = frame.fence_shared_handle;
    replacement.native_state = std::move(state);
    device->SetName(&replacement.texture, "newpipeline.client.native_nv12");
    surface = std::move(replacement);
    return true;
#else
    (void)frame; (void)surface;
    SetError(error, "Windows DX12 NV12 interop is not built on this platform");
    return false;
#endif
}

bool QueueWindowsClientNV12Wait(
    const WindowsClientNV12Surface& surface,
    uint64_t fence_value,
    wi::graphics::CommandList cmd,
    std::string* error)
{
#if defined(_WIN32)
    auto* device = GetDX12Device();
    auto state = std::static_pointer_cast<ClientState>(surface.native_state);
    if (device == nullptr || !state || !state->fence || fence_value == 0)
    {
        SetError(error, "DX12 decoded-surface wait fence is invalid");
        return false;
    }
    device->WaitExternalFence(state->fence.Get(), fence_value, cmd);
    return true;
#else
    (void)surface; (void)fence_value; (void)cmd;
    SetError(error, "Windows DX12 NV12 interop is not built on this platform");
    return false;
#endif
}

bool QueueWindowsClientNV12Signal(
    const WindowsClientNV12Surface& surface,
    uint64_t fence_value,
    wi::graphics::CommandList cmd,
    std::string* error)
{
#if defined(_WIN32)
    auto* device = GetDX12Device();
    auto state = std::static_pointer_cast<ClientState>(surface.native_state);
    if (device == nullptr || !state || !state->fence || fence_value == 0)
    {
        SetError(error, "DX12 decoded-surface completion fence is invalid");
        return false;
    }
    device->SignalExternalFence(state->fence.Get(), fence_value, cmd);
    return true;
#else
    (void)surface; (void)fence_value; (void)cmd;
    SetError(error, "Windows DX12 NV12 interop is not built on this platform");
    return false;
#endif
}
} // namespace wicked_newpipeline
#endif
