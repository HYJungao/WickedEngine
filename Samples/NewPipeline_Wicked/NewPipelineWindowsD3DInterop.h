#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12compatibility.h>
#include <wrl/client.h>

#include <cstdint>

namespace wicked_newpipeline
{
inline HRESULT CreateWindowsD3D11CompatibleNV12Texture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    UINT d3d11_bind_flags,
    ID3D12Resource** texture)
{
    if (device == nullptr || texture == nullptr || width == 0 || height == 0 ||
        (width & 1u) != 0 || (height & 1u) != 0)
        return E_INVALIDARG;
    *texture = nullptr;

    Microsoft::WRL::ComPtr<ID3D12CompatibilityDevice> compatibility_device;
    HRESULT status = device->QueryInterface(IID_PPV_ARGS(&compatibility_device));
    if (FAILED(status))
        return status;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    // A normal D3D12 shared committed resource does not carry the D3D11 bind
    // and NT-handle metadata that OpenSharedResource1 validates. The
    // compatibility API records both API contracts on the same allocation.
    D3D11_RESOURCE_FLAGS flags11 = {};
    flags11.BindFlags = d3d11_bind_flags;
    flags11.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    return compatibility_device->CreateSharedResource(
        &heap,
        D3D12_HEAP_FLAG_SHARED,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        &flags11,
        D3D12_COMPATIBILITY_SHARED_FLAG_NONE,
        nullptr,
        nullptr,
        IID_PPV_ARGS(texture));
}
} // namespace wicked_newpipeline

#endif
