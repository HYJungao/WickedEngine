#pragma once

#include "wiGraphicsDevice.h"

#include <cstdint>
#include <memory>
#include <string>

namespace wicked_newpipeline
{
struct RetainedNV12Frame;

struct WindowsNV12Footprint
{
    uint32_t y_stride = 0;
    uint32_t uv_stride = 0;
    uint64_t uv_offset = 0;
    uint64_t total_size = 0;
};

struct WindowsServerNV12Surface
{
    wi::graphics::Texture texture;
    std::shared_ptr<void> native_state;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t adapter_luid = 0;
    void* texture_shared_handle = nullptr;
    void* fence_shared_handle = nullptr;
    uint64_t next_fence_value = 0;
};

struct WindowsClientNV12Surface
{
    wi::graphics::Texture texture;
    std::shared_ptr<void> native_state;
    int luminance_subresource = -1;
    int chrominance_subresource = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t adapter_luid = 0;
    uint64_t last_producer_fence_value = 0;
    void* texture_shared_handle = nullptr;
    void* fence_shared_handle = nullptr;
};

#if defined(_WIN32)
uint64_t GetWindowsDX12AdapterLuid();
bool EnsureWindowsServerNV12Surface(
    uint32_t width,
    uint32_t height,
    WindowsServerNV12Surface& surface,
    WindowsNV12Footprint& footprint,
    std::string* error = nullptr);
bool QueueWindowsServerNV12Copy(
    WindowsServerNV12Surface& surface,
    const WindowsNV12Footprint& footprint,
    const wi::graphics::GPUBuffer& packed_nv12,
    uint64_t wait_fence_value,
    uint64_t signal_fence_value,
    wi::graphics::CommandList cmd,
    std::string* error = nullptr);
bool OpenWindowsClientNV12Surface(
    const RetainedNV12Frame& frame,
    WindowsClientNV12Surface& surface,
    std::string* error = nullptr);
bool QueueWindowsClientNV12Wait(
    const WindowsClientNV12Surface& surface,
    uint64_t fence_value,
    wi::graphics::CommandList cmd,
    std::string* error = nullptr);
bool QueueWindowsClientNV12Signal(
    const WindowsClientNV12Surface& surface,
    uint64_t fence_value,
    wi::graphics::CommandList cmd,
    std::string* error = nullptr);
#else
inline uint64_t GetWindowsDX12AdapterLuid()
{
    return 0;
}
inline bool EnsureWindowsServerNV12Surface(
    uint32_t, uint32_t, WindowsServerNV12Surface&, WindowsNV12Footprint&,
    std::string* error = nullptr)
{
    if (error != nullptr)
        *error = "Windows DX12 NV12 interop is unavailable on this platform";
    return false;
}
inline bool QueueWindowsServerNV12Copy(
    WindowsServerNV12Surface&, const WindowsNV12Footprint&,
    const wi::graphics::GPUBuffer&, uint64_t, uint64_t,
    wi::graphics::CommandList, std::string* error = nullptr)
{
    if (error != nullptr)
        *error = "Windows DX12 NV12 interop is unavailable on this platform";
    return false;
}
inline bool OpenWindowsClientNV12Surface(
    const RetainedNV12Frame&, WindowsClientNV12Surface&,
    std::string* error = nullptr)
{
    if (error != nullptr)
        *error = "Windows DX12 NV12 interop is unavailable on this platform";
    return false;
}
inline bool QueueWindowsClientNV12Wait(
    const WindowsClientNV12Surface&, uint64_t, wi::graphics::CommandList,
    std::string* error = nullptr)
{
    if (error != nullptr)
        *error = "Windows DX12 NV12 interop is unavailable on this platform";
    return false;
}
inline bool QueueWindowsClientNV12Signal(
    const WindowsClientNV12Surface&, uint64_t, wi::graphics::CommandList,
    std::string* error = nullptr)
{
    if (error != nullptr)
        *error = "Windows DX12 NV12 interop is unavailable on this platform";
    return false;
}
#endif
} // namespace wicked_newpipeline
