#include "NewPipelineWebRTCWindowsCodecFactory.h"
#include "NewPipelineWindowsD3DInterop.h"
#include "NewPipelineVideoFrameIdentity.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

#include <windows.h>
#include <codecapi.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "common_video/h264/sps_parser.h"
#include "libyuv/convert.h"
#include "libyuv/convert_from.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr size_t kMaxPendingFrames = 8;
constexpr size_t kMaxSubmittedFrames = 32;
constexpr size_t kMaxPendingEncoderFrames = 4;
constexpr uint32_t kMaxVideoDimension = 8192;
constexpr auto kWorkerIdleWait = std::chrono::milliseconds(2);
constexpr auto kEncoderStartupTimeout = std::chrono::seconds(5);
constexpr uint8_t kAnnexBStartCode[] = {0, 0, 0, 1};

std::atomic_bool g_hardware_encoder_runtime_disabled{false};
std::atomic_bool g_hardware_decoder_runtime_disabled{false};
std::mutex g_hardware_probe_mutex;
std::unordered_map<uint64_t, bool> g_hardware_encoder_probe_cache;
std::unordered_map<uint64_t, bool> g_hardware_decoder_probe_cache;
std::mutex g_native_buffer_mutex;
std::unordered_set<webrtc::VideoFrameBuffer*> g_native_buffers;

class WindowsNativeNV12Buffer : public webrtc::VideoFrameBuffer
{
public:
    WindowsNativeNV12Buffer(
        NPWindowsNativeNV12Surface surface,
        std::function<void()> release,
        std::function<void()> completion_scheduled) :
        surface_(surface),
        release_(std::move(release)),
        completion_scheduled_(std::move(completion_scheduled))
    {
        std::lock_guard lock(g_native_buffer_mutex);
        g_native_buffers.insert(this);
    }

    Type type() const override { return Type::kNative; }
    int width() const override { return static_cast<int>(surface_.width); }
    int height() const override { return static_cast<int>(surface_.height); }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override
    {
        return nullptr;
    }
    const NPWindowsNativeNV12Surface& surface() const { return surface_; }
    void MarkCompletionScheduled()
    {
        if (!completion_marked_.exchange(true, std::memory_order_acq_rel) &&
            completion_scheduled_)
            completion_scheduled_();
    }

protected:
    ~WindowsNativeNV12Buffer() override
    {
        {
            std::lock_guard lock(g_native_buffer_mutex);
            g_native_buffers.erase(this);
        }
        if (release_)
            release_();
    }

private:
    NPWindowsNativeNV12Surface surface_;
    std::function<void()> release_;
    std::function<void()> completion_scheduled_;
    std::atomic_bool completion_marked_{false};
};

WindowsNativeNV12Buffer* AsWindowsNativeNV12Buffer(
    webrtc::VideoFrameBuffer* buffer)
{
    if (buffer == nullptr ||
        buffer->type() != webrtc::VideoFrameBuffer::Type::kNative)
        return nullptr;
    std::lock_guard lock(g_native_buffer_mutex);
    return g_native_buffers.contains(buffer)
        ? static_cast<WindowsNativeNV12Buffer*>(buffer) : nullptr;
}

class ScopedCOM final
{
public:
    ScopedCOM() : status_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedCOM()
    {
        if (status_ == S_OK || status_ == S_FALSE)
            CoUninitialize();
    }

    bool IsAvailable() const
    {
        return SUCCEEDED(status_) || status_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT status_ = E_FAIL;
};

class MediaFoundationRuntime final
{
public:
    MediaFoundationRuntime() : started_(
        SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
    {
    }

    ~MediaFoundationRuntime()
    {
        if (started_)
            MFShutdown();
    }

    bool IsStarted() const { return started_; }

private:
    bool started_ = false;
};

bool EnsureMediaFoundationStarted()
{
    static MediaFoundationRuntime runtime;
    return runtime.IsStarted();
}

bool IsH264(const webrtc::SdpVideoFormat& format)
{
    std::string name = format.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return name == "h264";
}

bool IsVp8(const webrtc::SdpVideoFormat& format)
{
    std::string name = format.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return name == "vp8";
}

bool ContainsFormat(
    const std::vector<webrtc::SdpVideoFormat>& formats,
    const webrtc::SdpVideoFormat& candidate)
{
    return std::any_of(formats.begin(), formats.end(), [&](const auto& format) {
        return format == candidate;
    });
}

struct NalUnit
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

std::vector<NalUnit> SplitAnnexB(const uint8_t* data, size_t size)
{
    std::vector<NalUnit> units;
    if (data == nullptr || size < 4)
        return units;
    const auto start_code_size = [&](size_t offset) -> size_t {
        if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 1)
            return 3;
        if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 0 && data[offset + 3] == 1)
            return 4;
        return 0;
    };
    size_t cursor = 0;
    while (cursor < size)
    {
        size_t prefix = 0;
        while (cursor < size && (prefix = start_code_size(cursor)) == 0)
            ++cursor;
        if (cursor >= size)
            break;
        const size_t nal_begin = cursor + prefix;
        size_t next = nal_begin;
        while (next < size && start_code_size(next) == 0)
            ++next;
        if (next > nal_begin)
            units.push_back({data + nal_begin, next - nal_begin});
        cursor = next;
    }
    return units;
}

std::optional<std::pair<uint32_t, uint32_t>> ParseDimensions(
    const uint8_t* data, size_t size)
{
    for (const NalUnit& unit : SplitAnnexB(data, size))
    {
        if (unit.size < 2 || (unit.data[0] & 0x1fu) != 7)
            continue;
        const auto state = webrtc::SpsParser::ParseSps(
            std::span<const uint8_t>(unit.data + 1, unit.size - 1));
        if (state && state->width > 0 && state->height > 0)
            return std::pair<uint32_t, uint32_t>{state->width, state->height};
    }
    return std::nullopt;
}

void ReleaseActivations(IMFActivate** activations, UINT32 count)
{
    if (activations == nullptr)
        return;
    for (UINT32 index = 0; index < count; ++index)
    {
        if (activations[index] != nullptr)
            activations[index]->Release();
    }
    CoTaskMemFree(activations);
}

LUID UnpackAdapterLuid(uint64_t adapter_luid)
{
    LUID luid = {};
    luid.LowPart = static_cast<DWORD>(adapter_luid & 0xffffffffull);
    luid.HighPart = static_cast<LONG>(adapter_luid >> 32u);
    return luid;
}

HRESULT EnumerateHardwareH264Transforms(
    const GUID& category,
    const MFT_REGISTER_TYPE_INFO& input,
    const MFT_REGISTER_TYPE_INFO& output,
    uint64_t adapter_luid,
    IMFActivate*** activations,
    UINT32* count)
{
    if (activations == nullptr || count == nullptr)
        return E_POINTER;
    *activations = nullptr;
    *count = 0;
    if (adapter_luid == 0)
    {
        return MFTEnumEx(
            category,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &input,
            &output,
            activations,
            count);
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT status = MFCreateAttributes(&attributes, 1);
    if (FAILED(status))
        return status;
    const LUID luid = UnpackAdapterLuid(adapter_luid);
    status = attributes->SetBlob(
        MFT_ENUM_ADAPTER_LUID,
        reinterpret_cast<const UINT8*>(&luid),
        sizeof(luid));
    if (FAILED(status))
        return status;
    return MFTEnum2(
        category,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input,
        &output,
        attributes.Get(),
        activations,
        count);
}

HRESULT EnumerateHardwareH264Decoders(
    uint64_t adapter_luid, IMFActivate*** activations, UINT32* count)
{
    const MFT_REGISTER_TYPE_INFO input = {
        MFMediaType_Video, MFVideoFormat_H264};
    const MFT_REGISTER_TYPE_INFO output = {
        MFMediaType_Video, MFVideoFormat_NV12};
    return EnumerateHardwareH264Transforms(
        MFT_CATEGORY_VIDEO_DECODER, input, output, adapter_luid,
        activations, count);
}

HRESULT EnumerateHardwareH264Encoders(
    uint64_t adapter_luid, IMFActivate*** activations, UINT32* count)
{
    MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, MFVideoFormat_H264};
    return EnumerateHardwareH264Transforms(
        MFT_CATEGORY_VIDEO_ENCODER, input, output, adapter_luid,
        activations, count);
}

bool CreateD3D11VideoDevice(
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context,
    uint64_t adapter_luid = 0)
{
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_11_0;
    const UINT flags =
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    ComPtr<IDXGIAdapter1> selected_adapter;
    if (adapter_luid != 0)
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(candidate->GetDesc1(&desc)))
            {
                const uint64_t candidate_luid =
                    static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.LowPart)) |
                    (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32u);
                if (candidate_luid == adapter_luid)
                {
                    selected_adapter = std::move(candidate);
                    break;
                }
            }
        }
        if (!selected_adapter)
            return false;
    }
    const HRESULT status = D3D11CreateDevice(
        selected_adapter.Get(),
        selected_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        &selected,
        &context);
    if (FAILED(status))
        return false;
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);
    return true;
}

HRESULT ValidateD3D11CompatibleNV12Profile(
    ID3D11Device* d3d11_device,
    ID3D12Device* d3d12_device,
    UINT d3d11_bind_flags)
{
    if (d3d11_device == nullptr || d3d12_device == nullptr)
        return E_INVALIDARG;

    ComPtr<ID3D12Resource> resource;
    HRESULT status =
        wicked_newpipeline::CreateWindowsD3D11CompatibleNV12Texture(
            d3d12_device, 64, 32, d3d11_bind_flags, &resource);
    HANDLE shared_handle = nullptr;
    if (SUCCEEDED(status))
    {
        status = d3d12_device->CreateSharedHandle(
            resource.Get(), nullptr, GENERIC_ALL, nullptr, &shared_handle);
    }

    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> opened_texture;
    if (SUCCEEDED(status))
        status = d3d11_device->QueryInterface(IID_PPV_ARGS(&device1));
    if (SUCCEEDED(status))
    {
        status = device1->OpenSharedResource1(
            shared_handle, IID_PPV_ARGS(&opened_texture));
    }
    if (shared_handle != nullptr)
        CloseHandle(shared_handle);
    if (FAILED(status))
        return status;

    D3D11_TEXTURE2D_DESC opened_desc = {};
    opened_texture->GetDesc(&opened_desc);
    if (opened_desc.Width != 64 || opened_desc.Height != 32 ||
        opened_desc.Format != DXGI_FORMAT_NV12 ||
        (opened_desc.BindFlags & d3d11_bind_flags) != d3d11_bind_flags)
        return E_UNEXPECTED;
    return S_OK;
}

HRESULT ValidateD3D11D3D12NV12Sharing(
    uint64_t adapter_luid,
    bool validate_encoder_profile,
    bool validate_decoder_profile,
    const char** failed_profile)
{
    if (failed_profile != nullptr)
        *failed_profile = nullptr;
    if (!validate_encoder_profile && !validate_decoder_profile)
        return S_OK;

    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext> d3d11_context;
    if (!CreateD3D11VideoDevice(
            d3d11_device, d3d11_context, adapter_luid))
        return E_FAIL;

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> d3d12_device;
    HRESULT status = d3d11_device.As(&dxgi_device);
    if (SUCCEEDED(status))
        status = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(status))
    {
        status = D3D12CreateDevice(
            adapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&d3d12_device));
    }
    if (SUCCEEDED(status) && validate_decoder_profile)
    {
        if (failed_profile != nullptr)
            *failed_profile = "decoder";
        status = ValidateD3D11CompatibleNV12Profile(
            d3d11_device.Get(), d3d12_device.Get(),
            D3D11_BIND_SHADER_RESOURCE);
    }
    if (SUCCEEDED(status) && validate_encoder_profile)
    {
        if (failed_profile != nullptr)
            *failed_profile = "encoder";
        status = ValidateD3D11CompatibleNV12Profile(
            d3d11_device.Get(), d3d12_device.Get(),
            D3D11_BIND_VIDEO_ENCODER);
    }
    if (SUCCEEDED(status) && failed_profile != nullptr)
        *failed_profile = nullptr;
    return status;
}

bool D3D11DeviceSupportsH264Decode(ID3D11Device* device)
{
    if (device == nullptr)
        return false;
    ComPtr<ID3D11VideoDevice> video_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video_device))))
        return false;
    const UINT profile_count = video_device->GetVideoDecoderProfileCount();
    for (UINT index = 0; index < profile_count; ++index)
    {
        GUID profile = {};
        if (FAILED(video_device->GetVideoDecoderProfile(index, &profile)))
            continue;
        if (profile != D3D11_DECODER_PROFILE_H264_VLD_NOFGT &&
            profile != D3D11_DECODER_PROFILE_H264_VLD_FGT &&
            profile != D3D11_DECODER_PROFILE_H264_VLD_WITHFMOASO_NOFGT)
            continue;
        BOOL nv12_supported = FALSE;
        if (SUCCEEDED(video_device->CheckVideoDecoderFormat(
                &profile, DXGI_FORMAT_NV12, &nv12_supported)) &&
            nv12_supported)
            return true;
    }
    return false;
}

bool SystemH264TransformSupportsD3D11()
{
    ComPtr<IMFTransform> transform;
    if (FAILED(CoCreateInstance(
            CLSID_CMSH264DecoderMFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&transform))))
        return false;
    ComPtr<IMFAttributes> attributes;
    UINT32 d3d11_aware = FALSE;
    return SUCCEEDED(transform->GetAttributes(&attributes)) && attributes &&
        SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware)) &&
        d3d11_aware;
}

bool ProbeHardwareH264Decoder(uint64_t adapter_luid)
{
    if (g_hardware_decoder_runtime_disabled.load(std::memory_order_acquire))
        return false;
    ScopedCOM com;
    if (!com.IsAvailable() || !EnsureMediaFoundationStarted())
        return false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateD3D11VideoDevice(device, context, adapter_luid) ||
        !D3D11DeviceSupportsH264Decode(device.Get()))
        return false;
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT status = EnumerateHardwareH264Decoders(
        adapter_luid, &activations, &count);
    ReleaseActivations(activations, count);
    return (SUCCEEDED(status) && count > 0) ||
        SystemH264TransformSupportsD3D11();
}

bool HardwareH264DecoderAvailable(uint64_t adapter_luid)
{
    if (g_hardware_decoder_runtime_disabled.load(std::memory_order_acquire))
        return false;
    std::lock_guard lock(g_hardware_probe_mutex);
    const auto found = g_hardware_decoder_probe_cache.find(adapter_luid);
    if (found != g_hardware_decoder_probe_cache.end())
        return found->second;
    const bool detected = ProbeHardwareH264Decoder(adapter_luid);
    g_hardware_decoder_probe_cache.emplace(adapter_luid, detected);
    return detected;
}

bool ProbeHardwareH264Encoder(uint64_t adapter_luid)
{
    if (g_hardware_encoder_runtime_disabled.load(std::memory_order_acquire))
        return false;
    ScopedCOM com;
    if (!com.IsAvailable() || !EnsureMediaFoundationStarted())
        return false;
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT status = EnumerateHardwareH264Encoders(
        adapter_luid, &activations, &count);
    ReleaseActivations(activations, count);
    return SUCCEEDED(status) && count > 0;
}

bool HardwareH264EncoderAvailable(uint64_t adapter_luid)
{
    if (g_hardware_encoder_runtime_disabled.load(std::memory_order_acquire))
        return false;
    std::lock_guard lock(g_hardware_probe_mutex);
    const auto found = g_hardware_encoder_probe_cache.find(adapter_luid);
    if (found != g_hardware_encoder_probe_cache.end())
        return found->second;
    const bool detected = ProbeHardwareH264Encoder(adapter_luid);
    g_hardware_encoder_probe_cache.emplace(adapter_luid, detected);
    return detected;
}

void AppendAnnexBNal(
    std::vector<uint8_t>& output, const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0)
        return;
    output.insert(output.end(), std::begin(kAnnexBStartCode),
        std::end(kAnnexBStartCode));
    output.insert(output.end(), data, data + size);
}

bool ConvertLengthPrefixedToAnnexB(
    const uint8_t* data, size_t size, size_t length_size,
    std::vector<uint8_t>& output)
{
    if (data == nullptr || size == 0 || length_size == 0 || length_size > 4)
        return false;
    std::vector<uint8_t> converted;
    size_t cursor = 0;
    while (cursor + length_size <= size)
    {
        uint32_t nal_size = 0;
        for (size_t index = 0; index < length_size; ++index)
            nal_size = (nal_size << 8u) | data[cursor + index];
        cursor += length_size;
        if (nal_size == 0 || cursor + nal_size > size)
            return false;
        AppendAnnexBNal(converted, data + cursor, nal_size);
        cursor += nal_size;
    }
    if (cursor != size || converted.empty())
        return false;
    output = std::move(converted);
    return true;
}

bool NormalizeH264Sample(
    const uint8_t* data, size_t size, std::vector<uint8_t>& output)
{
    output.clear();
    if (data == nullptr || size == 0)
        return false;
    if (ConvertLengthPrefixedToAnnexB(data, size, 4, output) ||
        ConvertLengthPrefixedToAnnexB(data, size, 2, output) ||
        ConvertLengthPrefixedToAnnexB(data, size, 1, output))
        return true;
    if (!SplitAnnexB(data, size).empty())
    {
        output.assign(data, data + size);
        return true;
    }
    AppendAnnexBNal(output, data, size);
    return true;
}

using wicked_newpipeline::video_identity::PrependFrameIdentitySEI;

std::optional<int64_t> ParseFrameIdentitySEI(const uint8_t* data, size_t size)
{
    std::vector<uint8_t> annex_b;
    if (!NormalizeH264Sample(data, size, annex_b))
        return std::nullopt;
    return wicked_newpipeline::video_identity::ParseFrameIdentitySEI(annex_b.data(), annex_b.size());
}

bool ParseAvcConfigurationRecord(
    const uint8_t* data, size_t size, std::vector<uint8_t>& output)
{
    if (data == nullptr || size < 7 || data[0] != 1)
        return false;
    std::vector<uint8_t> parameter_sets;
    size_t cursor = 5;
    const uint8_t sps_count = data[cursor++] & 0x1fu;
    for (uint8_t index = 0; index < sps_count; ++index)
    {
        if (cursor + 2 > size)
            return false;
        const size_t nal_size =
            (static_cast<size_t>(data[cursor]) << 8u) | data[cursor + 1];
        cursor += 2;
        if (nal_size == 0 || cursor + nal_size > size)
            return false;
        AppendAnnexBNal(parameter_sets, data + cursor, nal_size);
        cursor += nal_size;
    }
    if (cursor >= size)
        return false;
    const uint8_t pps_count = data[cursor++];
    for (uint8_t index = 0; index < pps_count; ++index)
    {
        if (cursor + 2 > size)
            return false;
        const size_t nal_size =
            (static_cast<size_t>(data[cursor]) << 8u) | data[cursor + 1];
        cursor += 2;
        if (nal_size == 0 || cursor + nal_size > size)
            return false;
        AppendAnnexBNal(parameter_sets, data + cursor, nal_size);
        cursor += nal_size;
    }
    if (parameter_sets.empty())
        return false;
    output = std::move(parameter_sets);
    return true;
}

bool AnnexBContainsNalType(const std::vector<uint8_t>& bytes, uint8_t type)
{
    for (const NalUnit& unit : SplitAnnexB(bytes.data(), bytes.size()))
    {
        if (unit.size > 0 && (unit.data[0] & 0x1fu) == type)
            return true;
    }
    return false;
}

bool AnnexBHasConstrainedBaselineSps(const std::vector<uint8_t>& bytes)
{
    for (const NalUnit& unit : SplitAnnexB(bytes.data(), bytes.size()))
    {
        if (unit.size >= 4 && (unit.data[0] & 0x1fu) == 7)
        {
            constexpr uint8_t kBaselineProfileIdc = 66;
            constexpr uint8_t kConstraintSet1Flag = 0x40;
            return unit.data[1] == kBaselineProfileIdc &&
                (unit.data[2] & kConstraintSet1Flag) != 0;
        }
    }
    return false;
}

class MediaFoundationH264Encoder final : public webrtc::VideoEncoder
{
public:
    explicit MediaFoundationH264Encoder(
        std::shared_ptr<NPWindowsHardwareEncoderFailureSignal> failure,
        uint64_t adapter_luid) :
        failure_(std::move(failure)),
        adapter_luid_(adapter_luid)
    {
    }

    ~MediaFoundationH264Encoder() override { Release(); }

    int InitEncode(
        const webrtc::VideoCodec* settings,
        const webrtc::VideoEncoder::Settings&) override
    {
        StopWorker(false);
        if (settings == nullptr || settings->codecType != webrtc::kVideoCodecH264 ||
            settings->width == 0 || settings->height == 0 ||
            settings->width > kMaxVideoDimension ||
            settings->height > kMaxVideoDimension ||
            (settings->width & 1u) != 0 || (settings->height & 1u) != 0 ||
            settings->numberOfSimulcastStreams > 1 ||
            !HardwareH264EncoderAvailable(adapter_luid_))
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        width_ = settings->width;
        height_ = settings->height;
        bitrate_bps_.store(
            static_cast<uint32_t>(std::max(1u, settings->startBitrate)) * 1000u,
            std::memory_order_release);
        framerate_.store(std::max(1u, settings->maxFramerate),
            std::memory_order_release);
        force_next_keyframe_.store(true, std::memory_order_release);
        failed_.store(false, std::memory_order_release);
        next_sample_time_ = 0;
        if (failure_)
        {
            failure_->failed.store(false, std::memory_order_release);
            failure_->native_surface_failed.store(
                false, std::memory_order_release);
            failure_->encode_calls.store(0, std::memory_order_relaxed);
            failure_->native_frames.store(0, std::memory_order_relaxed);
            failure_->submitted_frames.store(0, std::memory_order_relaxed);
            failure_->output_frames.store(0, std::memory_order_relaxed);
            failure_->failure_stage.store(0, std::memory_order_relaxed);
            failure_->failure_hresult.store(0, std::memory_order_relaxed);
        }
        stop_requested_.store(false, std::memory_order_release);
        initialized_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(startup_mutex_);
            startup_complete_ = false;
            startup_succeeded_ = false;
        }
        try
        {
            worker_ = std::thread(&MediaFoundationH264Encoder::WorkerMain, this);
        }
        catch (...)
        {
            SignalFailure(1, E_FAIL);
            return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        }

        bool started = false;
        {
            std::unique_lock lock(startup_mutex_);
            started = startup_condition_.wait_for(lock, kEncoderStartupTimeout, [&] {
                return startup_complete_;
            }) && startup_succeeded_;
        }
        if (!started)
        {
            SignalFailure(2, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            StopWorker(false);
            return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        }
        initialized_.store(true, std::memory_order_release);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(
        webrtc::EncodedImageCallback* callback) override
    {
        std::lock_guard lock(callback_mutex_);
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        StopWorker(true);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(
        const webrtc::VideoFrame& frame,
        const std::vector<webrtc::VideoFrameType>* frame_types) override
    {
        if (failed_.load(std::memory_order_acquire))
            return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        if (!initialized_.load(std::memory_order_acquire))
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        PendingFrame pending;
        if (failure_)
            failure_->encode_calls.fetch_add(1, std::memory_order_relaxed);
        const auto source_buffer = frame.video_frame_buffer();
        NPWindowsNativeNV12Surface native_surface;
        if (source_buffer && np_get_windows_native_nv12_surface(
                source_buffer.get(), native_surface))
        {
            if (native_surface.width != width_ || native_surface.height != height_ ||
                native_surface.adapter_luid == 0 ||
                (adapter_luid_ != 0 && native_surface.adapter_luid != adapter_luid_))
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            pending.native_buffer = source_buffer;
            pending.source_timestamp_usec = native_surface.source_timestamp_usec;
            if (failure_)
                failure_->native_frames.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            const auto i420 = source_buffer ? source_buffer->ToI420() : nullptr;
            if (!i420 || i420->width() != static_cast<int>(width_) ||
                i420->height() != static_cast<int>(height_))
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            const size_t y_size = static_cast<size_t>(width_) * height_;
            const size_t uv_size = y_size / 2u;
            if (y_size > static_cast<size_t>(MAXDWORD) - uv_size)
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            pending.nv12.resize(y_size + uv_size);
            if (libyuv::I420ToNV12(
                    i420->DataY(), i420->StrideY(),
                    i420->DataU(), i420->StrideU(),
                    i420->DataV(), i420->StrideV(),
                    pending.nv12.data(), static_cast<int>(width_),
                    pending.nv12.data() + y_size, static_cast<int>(width_),
                    static_cast<int>(width_), static_cast<int>(height_)) != 0)
                return WEBRTC_VIDEO_CODEC_ERROR;
        }

        pending.rtp_timestamp = frame.rtp_timestamp();
        pending.timestamp_usec = frame.timestamp_us();
        if (pending.source_timestamp_usec <= 0)
            pending.source_timestamp_usec = pending.timestamp_usec;
        pending.force_keyframe =
            force_next_keyframe_.exchange(false, std::memory_order_acq_rel);
        if (frame_types != nullptr)
        {
            pending.force_keyframe = pending.force_keyframe ||
                std::find(frame_types->begin(), frame_types->end(),
                    webrtc::VideoFrameType::kVideoFrameKey) != frame_types->end();
        }

        {
            std::lock_guard lock(queue_mutex_);
            if (pending_frames_.size() >= kMaxPendingEncoderFrames)
            {
                pending.force_keyframe = pending.force_keyframe ||
                    pending_frames_.front().force_keyframe;
                pending_frames_.pop_front();
            }
            pending_frames_.push_back(std::move(pending));
        }
        queue_condition_.notify_one();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& parameters) override
    {
        const uint32_t bitrate = parameters.bitrate.get_sum_bps();
        if (bitrate > 0)
            bitrate_bps_.store(bitrate, std::memory_order_release);
        if (parameters.framerate_fps > 0.0)
        {
            framerate_.store(std::max(1u, static_cast<uint32_t>(
                parameters.framerate_fps + 0.5)), std::memory_order_release);
        }
        queue_condition_.notify_one();
    }

    EncoderInfo GetEncoderInfo() const override
    {
        EncoderInfo info;
        info.implementation_name = "Media Foundation H264 Hardware";
        info.is_hardware_accelerated = true;
        info.supports_native_handle = true;
        info.requested_resolution_alignment = 2;
        info.apply_alignment_to_all_simulcast_layers = false;
        info.has_trusted_rate_controller = false;
        return info;
    }

private:
    struct PendingFrame
    {
        std::vector<uint8_t> nv12;
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> native_buffer;
        uint32_t rtp_timestamp = 0;
        int64_t timestamp_usec = 0;
        int64_t source_timestamp_usec = 0;
        bool force_keyframe = false;
    };

    struct SubmittedFrame
    {
        LONGLONG sample_time = 0;
        uint32_t rtp_timestamp = 0;
        int64_t timestamp_usec = 0;
        int64_t source_timestamp_usec = 0;
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> native_buffer;
        ComPtr<ID3D11Fence> native_fence;
        uint64_t native_consumer_fence_value = 0;
    };

    void SignalFailure(uint32_t stage, HRESULT status)
    {
        failed_.store(true, std::memory_order_release);
        initialized_.store(false, std::memory_order_release);
        g_hardware_encoder_runtime_disabled.store(true, std::memory_order_release);
        if (failure_)
        {
            failure_->failure_stage.store(stage, std::memory_order_relaxed);
            failure_->failure_hresult.store(
                static_cast<uint32_t>(status), std::memory_order_relaxed);
            failure_->failed.store(true, std::memory_order_release);
        }
        queue_condition_.notify_all();
        startup_condition_.notify_all();
    }

    void StopWorker(bool clear_callback)
    {
        initialized_.store(false, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
        queue_condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
        {
            std::lock_guard lock(queue_mutex_);
            pending_frames_.clear();
        }
        submitted_frames_.clear();
        if (clear_callback)
        {
            std::lock_guard lock(callback_mutex_);
            callback_ = nullptr;
        }
    }

    static void SetCodecUInt32(ICodecAPI* codec_api, const GUID& key, ULONG value)
    {
        if (codec_api == nullptr)
            return;
        VARIANT variant;
        VariantInit(&variant);
        variant.vt = VT_UI4;
        variant.ulVal = value;
        codec_api->SetValue(&key, &variant);
        VariantClear(&variant);
    }

    static void SetCodecBool(ICodecAPI* codec_api, const GUID& key, bool value)
    {
        if (codec_api == nullptr)
            return;
        VARIANT variant;
        VariantInit(&variant);
        variant.vt = VT_BOOL;
        variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        codec_api->SetValue(&key, &variant);
        VariantClear(&variant);
    }

    bool CreateD3DManager()
    {
        if (!CreateD3D11VideoDevice(
                d3d_device_, d3d_context_, adapter_luid_))
            return false;
        d3d_device_.As(&d3d_device5_);
        d3d_context_.As(&d3d_context4_);
        if (FAILED(MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_)))
            return false;
        return SUCCEEDED(dxgi_manager_->ResetDevice(
            d3d_device_.Get(), dxgi_reset_token_));
    }

    bool ConfigureTransformObject(ComPtr<IMFTransform> transform)
    {
        if (!transform)
            return false;
        ComPtr<IMFAttributes> attributes;
        if (FAILED(transform->GetAttributes(&attributes)) || !attributes)
            return false;
        UINT32 asynchronous = FALSE;
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
        if (asynchronous &&
            FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))
            return false;
        ComPtr<IMFMediaEventGenerator> event_generator;
        if (asynchronous && FAILED(transform.As(&event_generator)))
            return false;
        UINT32 d3d11_aware = FALSE;
        if (SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware)) &&
            d3d11_aware)
        {
            if (!dxgi_manager_ && !CreateD3DManager())
                return false;
            if (FAILED(transform->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get()))))
                return false;
        }

        DWORD input_count = 0;
        DWORD output_count = 0;
        if (FAILED(transform->GetStreamCount(&input_count, &output_count)) ||
            input_count != 1 || output_count != 1)
            return false;
        DWORD input_id = 0;
        DWORD output_id = 0;
        const HRESULT id_status = transform->GetStreamIDs(
            1, &input_id, 1, &output_id);
        if (FAILED(id_status) && id_status != E_NOTIMPL)
            return false;

        const uint32_t bitrate = bitrate_bps_.load(std::memory_order_acquire);
        const uint32_t framerate = framerate_.load(std::memory_order_acquire);
        ComPtr<IMFMediaType> output_type;
        if (FAILED(MFCreateMediaType(&output_type)) ||
            FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
            FAILED(MFSetAttributeSize(
                output_type.Get(), MF_MT_FRAME_SIZE, width_, height_)) ||
            FAILED(MFSetAttributeRatio(
                output_type.Get(), MF_MT_FRAME_RATE, framerate, 1)) ||
            FAILED(MFSetAttributeRatio(
                output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(output_type->SetUINT32(
                MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate)) ||
            FAILED(output_type->SetUINT32(
                MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_ConstrainedBase)) ||
            FAILED(transform->SetOutputType(output_id, output_type.Get(), 0)))
            return false;

        const uint64_t frame_bytes =
            static_cast<uint64_t>(width_) * height_ * 3u / 2u;
        if (frame_bytes > MAXDWORD)
            return false;
        ComPtr<IMFMediaType> input_type;
        if (FAILED(MFCreateMediaType(&input_type)) ||
            FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
            FAILED(MFSetAttributeSize(
                input_type.Get(), MF_MT_FRAME_SIZE, width_, height_)) ||
            FAILED(MFSetAttributeRatio(
                input_type.Get(), MF_MT_FRAME_RATE, framerate, 1)) ||
            FAILED(MFSetAttributeRatio(
                input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(input_type->SetUINT32(
                MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, width_)) ||
            FAILED(input_type->SetUINT32(
                MF_MT_SAMPLE_SIZE, static_cast<UINT32>(frame_bytes))) ||
            FAILED(transform->SetInputType(input_id, input_type.Get(), 0)))
            return false;

        ComPtr<ICodecAPI> codec_api;
        transform.As(&codec_api);
        SetCodecUInt32(codec_api.Get(), CODECAPI_AVEncCommonRateControlMode,
            eAVEncCommonRateControlMode_CBR);
        SetCodecUInt32(codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
        SetCodecBool(codec_api.Get(), CODECAPI_AVEncCommonLowLatency, true);
        SetCodecBool(codec_api.Get(), CODECAPI_AVLowLatencyMode, true);
        SetCodecUInt32(codec_api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetCodecUInt32(codec_api.Get(), CODECAPI_AVEncVideoMaxKeyframeDistance,
            std::max(1u, framerate * 2u));
        SetCodecUInt32(codec_api.Get(), CODECAPI_AVEncVideoContentType,
            eAVEncVideoContentType_FixedCameraAngle);

        MFT_OUTPUT_STREAM_INFO output_info = {};
        if (FAILED(transform->GetOutputStreamInfo(output_id, &output_info)) ||
            FAILED(transform->ProcessMessage(
                MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(transform->ProcessMessage(
                MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
            return false;

        transform_ = std::move(transform);
        event_generator_ = std::move(event_generator);
        output_type_ = std::move(output_type);
        codec_api_ = std::move(codec_api);
        input_stream_id_ = input_id;
        output_stream_id_ = output_id;
        output_stream_info_ = output_info;
        asynchronous_ = asynchronous != FALSE;
        need_input_count_ = asynchronous_ ? 0u : 1u;
        have_output_count_ = 0;
        last_applied_bitrate_ = bitrate;
        last_applied_framerate_ = framerate;
        RefreshParameterSets();
        return true;
    }

    bool CreateTransform()
    {
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        bool configured = false;
        if (SUCCEEDED(EnumerateHardwareH264Encoders(
                adapter_luid_, &activations, &count)))
        {
            for (UINT32 index = 0; index < count && !configured; ++index)
            {
                ComPtr<IMFTransform> transform;
                if (activations[index] != nullptr && SUCCEEDED(
                        activations[index]->ActivateObject(IID_PPV_ARGS(&transform))) &&
                    ConfigureTransformObject(std::move(transform)))
                {
                    activation_ = activations[index];
                    configured = true;
                }
                else if (activations[index] != nullptr)
                {
                    activations[index]->ShutdownObject();
                }
            }
        }
        ReleaseActivations(activations, count);
        return configured;
    }

    void CompleteNativeFrame(SubmittedFrame& frame)
    {
        if (!frame.native_buffer)
            return;
        HRESULT completion_status = E_NOINTERFACE;
        if (frame.native_fence && d3d_context4_)
        {
            completion_status = d3d_context4_->Signal(
                frame.native_fence.Get(), frame.native_consumer_fence_value);
        }
        if (SUCCEEDED(completion_status))
        {
            np_mark_windows_native_nv12_completion_scheduled(
                frame.native_buffer.get());
        }
        else if (failure_)
        {
            // The encoder already consumed this texture. Do not let the
            // producer reuse it without a consumer fence; disable only the
            // native input optimization and retain hardware H264 through I420.
            failure_->failure_stage.store(70, std::memory_order_relaxed);
            failure_->failure_hresult.store(
                static_cast<uint32_t>(completion_status),
                std::memory_order_relaxed);
            failure_->native_surface_failed.store(
                true, std::memory_order_release);
        }
    }

    void ShutdownTransform()
    {
        if (transform_)
        {
            transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        codec_api_.Reset();
        output_type_.Reset();
        event_generator_.Reset();
        transform_.Reset();
        if (activation_)
            activation_->ShutdownObject();
        activation_.Reset();
        for (SubmittedFrame& frame : submitted_frames_)
            CompleteNativeFrame(frame);
        submitted_frames_.clear();
        dxgi_manager_.Reset();
        d3d_context4_.Reset();
        d3d_device5_.Reset();
        d3d_context_.Reset();
        d3d_device_.Reset();
        parameter_sps_.clear();
        parameter_pps_.clear();
        need_input_count_ = 0;
        have_output_count_ = 0;
        asynchronous_ = false;
    }

    bool PumpEvents()
    {
        if (!event_generator_)
            return false;
        bool progressed = false;
        for (;;)
        {
            ComPtr<IMFMediaEvent> event;
            const HRESULT status = event_generator_->GetEvent(
                MF_EVENT_FLAG_NO_WAIT, &event);
            if (status == MF_E_NO_EVENTS_AVAILABLE)
                break;
            if (FAILED(status) || !event)
            {
                SignalFailure(4, status);
                break;
            }
            progressed = true;
            HRESULT event_status = S_OK;
            MediaEventType type = MEUnknown;
            event->GetStatus(&event_status);
            event->GetType(&type);
            if (FAILED(event_status) || type == MEError)
            {
                SignalFailure(5, FAILED(event_status) ? event_status : E_FAIL);
                break;
            }
            if (type == METransformNeedInput)
                ++need_input_count_;
            else if (type == METransformHaveOutput)
                ++have_output_count_;
        }
        return progressed;
    }

    void ApplyRateControl()
    {
        const uint32_t bitrate = bitrate_bps_.load(std::memory_order_acquire);
        const uint32_t framerate = framerate_.load(std::memory_order_acquire);
        if (bitrate != last_applied_bitrate_)
        {
            SetCodecUInt32(codec_api_.Get(),
                CODECAPI_AVEncCommonMeanBitRate, bitrate);
            last_applied_bitrate_ = bitrate;
        }
        if (framerate != last_applied_framerate_)
        {
            SetCodecUInt32(codec_api_.Get(),
                CODECAPI_AVEncVideoMaxKeyframeDistance,
                std::max(1u, framerate * 2u));
            last_applied_framerate_ = framerate;
        }
    }

    HRESULT SubmitInput(PendingFrame& frame)
    {
        if (frame.force_keyframe)
        {
            SetCodecUInt32(codec_api_.Get(),
                CODECAPI_AVEncVideoForceKeyFrame, TRUE);
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT status = MFCreateSample(&sample);
        ComPtr<ID3D11Fence> native_fence;
        uint64_t native_consumer_fence_value = 0;
        uint32_t native_failure_stage = 0;
        const auto native_failure = [&](uint32_t stage, HRESULT result) {
            native_failure_stage = stage;
            if (failure_)
            {
                failure_->failure_stage.store(stage, std::memory_order_relaxed);
                failure_->failure_hresult.store(
                    static_cast<uint32_t>(result), std::memory_order_relaxed);
            }
            return result;
        };
        if (frame.native_buffer)
        {
            NPWindowsNativeNV12Surface native_surface;
            ComPtr<ID3D11Texture2D> texture;
            if (!d3d_device5_ || !d3d_context4_)
                status = native_failure(60, E_NOINTERFACE);
            if (!np_get_windows_native_nv12_surface(
                    frame.native_buffer.get(), native_surface))
            {
                status = native_failure(61, E_INVALIDARG);
            }
            if (SUCCEEDED(status))
            {
                ComPtr<ID3D11Device1> device1;
                HRESULT direct_status = d3d_device_.As(&device1);
                if (SUCCEEDED(direct_status))
                    direct_status = device1->OpenSharedResource1(
                        static_cast<HANDLE>(native_surface.texture_shared_handle),
                        IID_PPV_ARGS(&texture));
                if (FAILED(direct_status))
                    status = native_failure(62, direct_status);
                if (SUCCEEDED(direct_status))
                {
                    D3D11_TEXTURE2D_DESC texture_desc = {};
                    texture->GetDesc(&texture_desc);
                    if (texture_desc.Width != width_ ||
                        texture_desc.Height != height_ ||
                        texture_desc.Format != DXGI_FORMAT_NV12)
                    {
                        direct_status = E_INVALIDARG;
                        status = native_failure(65, direct_status);
                    }
                }
                if (SUCCEEDED(direct_status))
                    direct_status = d3d_device5_->OpenSharedFence(
                        static_cast<HANDLE>(native_surface.fence_shared_handle),
                        IID_PPV_ARGS(&native_fence));
                if (FAILED(direct_status) && SUCCEEDED(status))
                    status = native_failure(63, direct_status);
                if (SUCCEEDED(direct_status))
                    direct_status = d3d_context4_->Wait(
                        native_fence.Get(), native_surface.producer_fence_value);
                if (FAILED(direct_status) && SUCCEEDED(status))
                    status = native_failure(64, direct_status);
                if (FAILED(status))
                {
                    texture.Reset();
                    native_fence.Reset();
                }
                else
                    status = S_OK;
            }
            if (SUCCEEDED(status))
            {
                status = MFCreateDXGISurfaceBuffer(
                    __uuidof(ID3D11Texture2D), texture.Get(), 0, FALSE, &buffer);
                if (FAILED(status))
                    native_failure(68, status);
                else
                {
                    DWORD maximum_length = 0;
                    const HRESULT maximum_status = buffer->GetMaxLength(&maximum_length);
                    if (SUCCEEDED(maximum_status) && maximum_length > 0)
                        status = buffer->SetCurrentLength(maximum_length);
                    else
                        status = FAILED(maximum_status) ? maximum_status : E_UNEXPECTED;
                    if (FAILED(status))
                        native_failure(69, status);
                }
            }
            native_consumer_fence_value = native_surface.consumer_fence_value;
        }
        else
        {
            if (SUCCEEDED(status))
                status = MFCreateMemoryBuffer(
                    static_cast<DWORD>(frame.nv12.size()), &buffer);
            BYTE* destination = nullptr;
            if (SUCCEEDED(status))
                status = buffer->Lock(&destination, nullptr, nullptr);
            if (SUCCEEDED(status) && destination == nullptr)
            {
                buffer->Unlock();
                status = E_UNEXPECTED;
            }
            if (SUCCEEDED(status))
            {
                std::memcpy(destination, frame.nv12.data(), frame.nv12.size());
                buffer->Unlock();
                destination = nullptr;
                status = buffer->SetCurrentLength(
                    static_cast<DWORD>(frame.nv12.size()));
            }
            else if (destination != nullptr)
            {
                buffer->Unlock();
            }
        }
        if (SUCCEEDED(status))
            status = sample->AddBuffer(buffer.Get());
        const uint32_t framerate = std::max(
            1u, framerate_.load(std::memory_order_acquire));
        const LONGLONG duration = 10'000'000ll / framerate;
        // Keep the MFT timeline monotonic across runtime frame-rate changes.
        // Recomputing sequence * duration with a new duration can move sample
        // time backwards and detach encoder output from submitted metadata.
        const LONGLONG sample_time = next_sample_time_;
        next_sample_time_ += duration;
        if (SUCCEEDED(status))
            status = sample->SetSampleTime(sample_time);
        if (SUCCEEDED(status))
            status = sample->SetSampleDuration(duration);
        if (SUCCEEDED(status) && frame.force_keyframe)
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (FAILED(status) && frame.native_buffer && native_failure_stage == 0)
            native_failure(66, status);
        if (SUCCEEDED(status))
            status = transform_->ProcessInput(input_stream_id_, sample.Get(), 0);
        if (FAILED(status) && status != MF_E_NOTACCEPTING &&
            frame.native_buffer && native_failure_stage == 0)
            native_failure(67, status);
        if (SUCCEEDED(status))
        {
            submitted_frames_.push_back({sample_time,
                frame.rtp_timestamp, frame.timestamp_usec,
                frame.source_timestamp_usec,
                frame.native_buffer, native_fence,
                native_consumer_fence_value});
            if (submitted_frames_.size() > kMaxSubmittedFrames)
                SignalFailure(11, HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
            if (failure_)
                failure_->submitted_frames.fetch_add(1, std::memory_order_relaxed);
        }
        return status;
    }

    void CacheParameterSets(const std::vector<uint8_t>& annex_b)
    {
        for (const NalUnit& unit : SplitAnnexB(annex_b.data(), annex_b.size()))
        {
            if (unit.size == 0)
                continue;
            const uint8_t type = unit.data[0] & 0x1fu;
            if (type == 7)
            {
                parameter_sps_.clear();
                AppendAnnexBNal(parameter_sps_, unit.data, unit.size);
            }
            else if (type == 8)
            {
                parameter_pps_.clear();
                AppendAnnexBNal(parameter_pps_, unit.data, unit.size);
            }
        }
    }

    void RefreshParameterSets()
    {
        if (!transform_)
            return;
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(transform_->GetOutputCurrentType(
                output_stream_id_, &current)) && current)
            output_type_ = std::move(current);
        if (!output_type_)
            return;
        UINT32 blob_size = 0;
        if (FAILED(output_type_->GetBlobSize(
                MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) || blob_size == 0)
            return;
        std::vector<uint8_t> blob(blob_size);
        UINT32 written = 0;
        if (FAILED(output_type_->GetBlob(
                MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), blob_size, &written)) ||
            written == 0)
            return;
        blob.resize(written);
        std::vector<uint8_t> annex_b;
        if (ParseAvcConfigurationRecord(blob.data(), blob.size(), annex_b) ||
            NormalizeH264Sample(blob.data(), blob.size(), annex_b))
            CacheParameterSets(annex_b);
    }

    bool DeliverOutput(IMFSample* sample)
    {
        if (sample == nullptr)
            return false;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer)
            return false;
        BYTE* data = nullptr;
        DWORD size = 0;
        const HRESULT lock_status = buffer->Lock(&data, nullptr, &size);
        if (FAILED(lock_status))
            return false;
        if (data == nullptr)
        {
            buffer->Unlock();
            return false;
        }
        std::vector<uint8_t> annex_b;
        const bool normalized = NormalizeH264Sample(data, size, annex_b);
        buffer->Unlock();
        if (!normalized || annex_b.empty())
            return false;

        CacheParameterSets(annex_b);
        const bool has_vcl = AnnexBContainsNalType(annex_b, 1) ||
            AnnexBContainsNalType(annex_b, 5);
        if (!has_vcl)
            return true;

        RefreshParameterSets();
        UINT32 clean_point = FALSE;
        bool keyframe = AnnexBContainsNalType(annex_b, 5) ||
            (SUCCEEDED(sample->GetUINT32(
                MFSampleExtension_CleanPoint, &clean_point)) && clean_point);
        if (keyframe)
        {
            std::vector<uint8_t> complete;
            if (!AnnexBContainsNalType(annex_b, 7) && !parameter_sps_.empty())
                complete.insert(complete.end(), parameter_sps_.begin(), parameter_sps_.end());
            if (!AnnexBContainsNalType(annex_b, 8) && !parameter_pps_.empty())
                complete.insert(complete.end(), parameter_pps_.begin(), parameter_pps_.end());
            complete.insert(complete.end(), annex_b.begin(), annex_b.end());
            annex_b = std::move(complete);
        }

        LONGLONG sample_time = 0;
        const bool has_sample_time = SUCCEEDED(sample->GetSampleTime(&sample_time));
        auto metadata = submitted_frames_.begin();
        if (has_sample_time)
        {
            metadata = std::find_if(
                submitted_frames_.begin(), submitted_frames_.end(),
                [&](const SubmittedFrame& candidate) {
                    return candidate.sample_time == sample_time;
                });
        }
        if (metadata == submitted_frames_.end())
            return false;
        const SubmittedFrame frame_metadata = *metadata;
        for (auto iterator = submitted_frames_.begin();;
            ++iterator)
        {
            CompleteNativeFrame(*iterator);
            if (iterator == metadata)
                break;
        }
        submitted_frames_.erase(submitted_frames_.begin(), std::next(metadata));

        PrependFrameIdentitySEI(
            annex_b, frame_metadata.source_timestamp_usec);

        webrtc::EncodedImage image;
        image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
            annex_b.data(), annex_b.size()));
        image.SetRtpTimestamp(frame_metadata.rtp_timestamp);
        image.capture_time_ms_ = frame_metadata.timestamp_usec / 1000;
        image._encodedWidth = width_;
        image._encodedHeight = height_;
        image.SetFrameType(keyframe
            ? webrtc::VideoFrameType::kVideoFrameKey
            : webrtc::VideoFrameType::kVideoFrameDelta);
        webrtc::CodecSpecificInfo codec_info;
        codec_info.codecType = webrtc::kVideoCodecH264;
        codec_info.codecSpecific.H264.packetization_mode =
            webrtc::H264PacketizationMode::NonInterleaved;
        codec_info.codecSpecific.H264.temporal_idx = 0;
        codec_info.codecSpecific.H264.base_layer_sync = false;
        codec_info.codecSpecific.H264.idr_frame = keyframe;
        webrtc::EncodedImageCallback* callback = nullptr;
        {
            std::lock_guard lock(callback_mutex_);
            callback = callback_;
        }
        if (callback != nullptr)
        {
            callback->OnEncodedImage(image, &codec_info);
            if (failure_)
                failure_->output_frames.fetch_add(1, std::memory_order_relaxed);
        }
        return callback != nullptr;
    }

    bool ProcessOneOutput()
    {
        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = output_stream_id_;
        ComPtr<IMFSample> caller_sample;
        if ((output_stream_info_.dwFlags &
                (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                 MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0)
        {
            const uint64_t raw_size =
                static_cast<uint64_t>(width_) * height_ * 3u / 2u;
            const DWORD capacity = std::max<DWORD>(output_stream_info_.cbSize,
                static_cast<DWORD>(std::min<uint64_t>(raw_size, MAXDWORD)));
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(&caller_sample)) ||
                FAILED(MFCreateMemoryBuffer(std::max<DWORD>(capacity, 1u), &buffer)) ||
                FAILED(caller_sample->AddBuffer(buffer.Get())))
            {
                SignalFailure(7, E_OUTOFMEMORY);
                return false;
            }
            output.pSample = caller_sample.Get();
        }
        DWORD status = 0;
        const HRESULT result = transform_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents != nullptr)
            output.pEvents->Release();
        if (result == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            RefreshParameterSets();
            if (FAILED(transform_->GetOutputStreamInfo(
                    output_stream_id_, &output_stream_info_)))
                SignalFailure(8, E_FAIL);
            return true;
        }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            have_output_count_ = 0;
            if (!asynchronous_)
                need_input_count_ = 1;
            return false;
        }
        if (FAILED(result))
        {
            SignalFailure(9, result);
            return false;
        }
        if (asynchronous_ && have_output_count_ > 0)
            --have_output_count_;
        else if (!asynchronous_)
            need_input_count_ = 1;
        ComPtr<IMFSample> provided_sample;
        IMFSample* output_sample = caller_sample.Get();
        if (!caller_sample && output.pSample != nullptr)
        {
            provided_sample.Attach(output.pSample);
            output_sample = provided_sample.Get();
        }
        if (!DeliverOutput(output_sample))
        {
            SignalFailure(10, E_FAIL);
            return false;
        }
        if ((output.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) != 0)
            have_output_count_ = std::max<uint32_t>(have_output_count_, 1);
        return true;
    }

    void WorkerMain()
    {
        ScopedCOM com;
        const bool configured = com.IsAvailable() &&
            EnsureMediaFoundationStarted() && CreateTransform();
        {
            std::lock_guard lock(startup_mutex_);
            startup_succeeded_ = configured;
            startup_complete_ = true;
        }
        startup_condition_.notify_all();
        if (!configured)
        {
            SignalFailure(3, E_FAIL);
            ShutdownTransform();
            return;
        }

        while (!stop_requested_.load(std::memory_order_acquire) &&
            !failed_.load(std::memory_order_acquire))
        {
            bool progressed = PumpEvents();
            ApplyRateControl();
            while (have_output_count_ > 0 &&
                !failed_.load(std::memory_order_acquire))
            {
                progressed = ProcessOneOutput() || progressed;
                if (have_output_count_ == 0)
                    break;
            }

            while (need_input_count_ > 0 &&
                !failed_.load(std::memory_order_acquire))
            {
                PendingFrame frame;
                {
                    std::lock_guard lock(queue_mutex_);
                    if (pending_frames_.empty())
                        break;
                    frame = std::move(pending_frames_.front());
                    pending_frames_.pop_front();
                }
                const HRESULT status = SubmitInput(frame);
                if (status == MF_E_NOTACCEPTING)
                {
                    std::lock_guard lock(queue_mutex_);
                    pending_frames_.push_front(std::move(frame));
                    break;
                }
                if (FAILED(status))
                {
                    const uint32_t recorded_stage = failure_
                        ? failure_->failure_stage.load(std::memory_order_relaxed)
                        : 0u;
                    if (frame.native_buffer && failure_ &&
                        recorded_stage >= 60u && recorded_stage <= 69u)
                    {
                        // Zero-copy is an optimization, not a codec contract.
                        // Drop this native frame, request an IDR, and let the
                        // producer switch to retained I420 on the next tick
                        // without tearing down the hardware H264 session.
                        failure_->native_surface_failed.store(
                            true, std::memory_order_release);
                        force_next_keyframe_.store(
                            true, std::memory_order_release);
                        progressed = true;
                        continue;
                    }
                    SignalFailure(
                        frame.native_buffer && recorded_stage != 0
                            ? recorded_stage : 6,
                        status);
                    break;
                }
                --need_input_count_;
                if (!asynchronous_)
                    have_output_count_ = 1;
                progressed = true;
            }

            if (!progressed)
            {
                std::unique_lock lock(queue_mutex_);
                queue_condition_.wait_for(lock, kWorkerIdleWait, [&] {
                    return stop_requested_.load(std::memory_order_acquire) ||
                        failed_.load(std::memory_order_acquire) ||
                        !pending_frames_.empty();
                });
            }
        }
        ShutdownTransform();
    }

    std::shared_ptr<NPWindowsHardwareEncoderFailureSignal> failure_;
    uint64_t adapter_luid_ = 0;
    std::atomic_bool initialized_{false};
    std::atomic_bool stop_requested_{false};
    std::atomic_bool failed_{false};
    std::atomic_bool force_next_keyframe_{true};
    std::atomic_uint32_t bitrate_bps_{1'000'000};
    std::atomic_uint32_t framerate_{30};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::thread worker_;
    std::mutex startup_mutex_;
    std::condition_variable startup_condition_;
    bool startup_complete_ = false;
    bool startup_succeeded_ = false;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::deque<PendingFrame> pending_frames_;
    std::mutex callback_mutex_;
    webrtc::EncodedImageCallback* callback_ = nullptr;

    ComPtr<ID3D11Device> d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    ComPtr<ID3D11Device5> d3d_device5_;
    ComPtr<ID3D11DeviceContext4> d3d_context4_;
    ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
    UINT dxgi_reset_token_ = 0;
    ComPtr<IMFActivate> activation_;
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaEventGenerator> event_generator_;
    ComPtr<IMFMediaType> output_type_;
    ComPtr<ICodecAPI> codec_api_;
    DWORD input_stream_id_ = 0;
    DWORD output_stream_id_ = 0;
    MFT_OUTPUT_STREAM_INFO output_stream_info_ = {};
    uint32_t need_input_count_ = 0;
    uint32_t have_output_count_ = 0;
    bool asynchronous_ = false;
    LONGLONG next_sample_time_ = 0;
    uint32_t last_applied_bitrate_ = 0;
    uint32_t last_applied_framerate_ = 0;
    std::deque<SubmittedFrame> submitted_frames_;
    std::vector<uint8_t> parameter_sps_;
    std::vector<uint8_t> parameter_pps_;
};

class MediaFoundationH264Decoder final : public webrtc::VideoDecoder
{
public:
    explicit MediaFoundationH264Decoder(
        std::shared_ptr<NPWindowsHardwareDecoderFailureSignal> failure,
        uint64_t adapter_luid) :
        failure_(std::move(failure)),
        adapter_luid_(adapter_luid)
    {
    }

    ~MediaFoundationH264Decoder() override { Release(); }

    bool Configure(const Settings&) override
    {
        StopWorker(false);
        if (!HardwareH264DecoderAvailable(adapter_luid_))
            return false;
        if (failure_)
        {
            failure_->failed.store(false, std::memory_order_release);
            failure_->native_surface_failed.store(
                false, std::memory_order_release);
            failure_->decode_calls.store(0, std::memory_order_relaxed);
            failure_->keyframes_received.store(0, std::memory_order_relaxed);
            failure_->dimension_requests.store(0, std::memory_order_relaxed);
            failure_->queued_frames.store(0, std::memory_order_relaxed);
            failure_->submitted_frames.store(0, std::memory_order_relaxed);
            failure_->decoded_frames.store(0, std::memory_order_relaxed);
            failure_->native_surface_attempts.store(0, std::memory_order_relaxed);
            failure_->native_surface_outputs.store(0, std::memory_order_relaxed);
            failure_->native_allocation_failures.store(0, std::memory_order_relaxed);
            failure_->native_signal_failures.store(0, std::memory_order_relaxed);
            failure_->native_ring_drops.store(0, std::memory_order_relaxed);
            failure_->native_failure_stage.store(0, std::memory_order_relaxed);
            failure_->native_failure_hresult.store(0, std::memory_order_relaxed);
        }
        failed_.store(false, std::memory_order_release);
        waiting_for_keyframe_.store(true, std::memory_order_release);
        request_keyframe_.store(false, std::memory_order_release);
        stop_requested_.store(false, std::memory_order_release);
        last_input_width_.store(0, std::memory_order_relaxed);
        last_input_height_.store(0, std::memory_order_relaxed);
        next_sample_time_ = 0;
        consecutive_output_failures_ = 0;
        configured_.store(true, std::memory_order_release);
        try
        {
            worker_ = std::thread(
                &MediaFoundationH264Decoder::WorkerMain, this);
        }
        catch (...)
        {
            SignalFailure();
            return false;
        }
        return true;
    }

    int32_t Decode(const webrtc::EncodedImage& input, int64_t) override
    {
        if (failure_)
        {
            failure_->decode_calls.fetch_add(1, std::memory_order_relaxed);
            if (input.IsKey())
                failure_->keyframes_received.fetch_add(1, std::memory_order_relaxed);
        }
        if (failed_.load(std::memory_order_acquire))
            return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        if (!configured_.load(std::memory_order_acquire))
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        if (input.data() == nullptr || input.size() == 0)
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

        uint32_t width = input._encodedWidth;
        uint32_t height = input._encodedHeight;
        if (width == 0 || height == 0)
        {
            const auto dimensions = ParseDimensions(input.data(), input.size());
            if (dimensions)
            {
                width = dimensions->first;
                height = dimensions->second;
            }
        }
        // WebRTC's H264 depacketizer normally exposes the coded dimensions on
        // the SPS/IDR access unit only. Delta access units do not carry SPS and
        // may therefore arrive with _encodedWidth/_encodedHeight unset. Reuse
        // the last validated dimensions instead of dropping every delta frame
        // and continuously requesting replacement keyframes.
        if (width == 0 || height == 0)
        {
            width = last_input_width_.load(std::memory_order_relaxed);
            height = last_input_height_.load(std::memory_order_relaxed);
        }
        if (width == 0 || height == 0)
        {
            if (failure_)
                failure_->dimension_requests.fetch_add(1, std::memory_order_relaxed);
            return WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME;
        }
        if (width > kMaxVideoDimension || height > kMaxVideoDimension ||
            (width & 1u) != 0 || (height & 1u) != 0 ||
            input.size() > static_cast<size_t>(MAXDWORD))
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        last_input_width_.store(width, std::memory_order_relaxed);
        last_input_height_.store(height, std::memory_order_relaxed);

        if (waiting_for_keyframe_.load(std::memory_order_acquire) && !input.IsKey())
            return WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME;

        PendingFrame frame;
        frame.bytes.assign(input.data(), input.data() + input.size());
        frame.width = width;
        frame.height = height;
        frame.rtp_timestamp = input.RtpTimestamp();
        frame.timestamp_usec = ParseFrameIdentitySEI(input.data(), input.size())
            .value_or(0);
        frame.keyframe = input.IsKey();

        bool overflow = false;
        {
            std::lock_guard lock(queue_mutex_);
            if (pending_frames_.size() >= kMaxPendingFrames)
            {
                pending_frames_.clear();
                overflow = true;
            }
            if (!overflow || frame.keyframe)
            {
                pending_frames_.push_back(std::move(frame));
                if (failure_)
                    failure_->queued_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (input.IsKey())
        {
            waiting_for_keyframe_.store(false, std::memory_order_release);
            request_keyframe_.store(false, std::memory_order_release);
        }
        else if (overflow)
        {
            waiting_for_keyframe_.store(true, std::memory_order_release);
            request_keyframe_.store(true, std::memory_order_release);
        }
        queue_condition_.notify_one();
        return overflow || request_keyframe_.exchange(false, std::memory_order_acq_rel)
            ? WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME
            : WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterDecodeCompleteCallback(
        webrtc::DecodedImageCallback* callback) override
    {
        std::lock_guard lock(callback_mutex_);
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override
    {
        StopWorker(true);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    DecoderInfo GetDecoderInfo() const override
    {
        DecoderInfo info;
        info.implementation_name = "Media Foundation H264 Hardware";
        info.is_hardware_accelerated = true;
        return info;
    }

private:
    struct PendingFrame
    {
        std::vector<uint8_t> bytes;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rtp_timestamp = 0;
        int64_t timestamp_usec = 0;
        bool keyframe = false;
    };

    struct SubmittedFrame
    {
        LONGLONG sample_time = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rtp_timestamp = 0;
        int64_t timestamp_usec = 0;
    };

    void StopWorker(bool clear_callback)
    {
        configured_.store(false, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
        queue_condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
        {
            std::lock_guard lock(queue_mutex_);
            pending_frames_.clear();
        }
        submitted_frames_.clear();
        if (clear_callback)
        {
            std::lock_guard lock(callback_mutex_);
            callback_ = nullptr;
        }
    }

    void SignalFailure()
    {
        failed_.store(true, std::memory_order_release);
        configured_.store(false, std::memory_order_release);
        g_hardware_decoder_runtime_disabled.store(true, std::memory_order_release);
        if (failure_)
            failure_->failed.store(true, std::memory_order_release);
        queue_condition_.notify_all();
    }

    bool CreateD3DDevice()
    {
        if (!CreateD3D11VideoDevice(d3d_device_, d3d_context_, adapter_luid_) ||
            !D3D11DeviceSupportsH264Decode(d3d_device_.Get()))
            return false;
        bool native_surface_supported =
            SUCCEEDED(d3d_device_.As(&d3d_device1_)) &&
            SUCCEEDED(d3d_device_.As(&d3d_device5_)) &&
            SUCCEEDED(d3d_context_.As(&d3d_context4_));
        if (native_surface_supported)
        {
            ComPtr<IDXGIDevice> dxgi_device;
            ComPtr<IDXGIAdapter> adapter;
            native_surface_supported =
                SUCCEEDED(d3d_device_.As(&dxgi_device)) &&
                SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
                SUCCEEDED(D3D12CreateDevice(
                    adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                    IID_PPV_ARGS(&d3d12_device_)));
        }
        if (!native_surface_supported && failure_)
        {
            // Hardware decode remains valid through the I420 copy fallback;
            // only native D3D11/D3D12 surface retention is unavailable.
            failure_->native_surface_failed.store(
                true, std::memory_order_release);
        }
        const HRESULT status = MFCreateDXGIDeviceManager(
            &dxgi_reset_token_, &dxgi_manager_);
        return SUCCEEDED(status) &&
            SUCCEEDED(dxgi_manager_->ResetDevice(d3d_device_.Get(), dxgi_reset_token_));
    }

    bool ConfigureOutputType()
    {
        if (!transform_)
            return false;
        ComPtr<IMFMediaType> selected;
        for (DWORD index = 0;; ++index)
        {
            ComPtr<IMFMediaType> candidate;
            const HRESULT status = transform_->GetOutputAvailableType(
                output_stream_id_, index, &candidate);
            if (status == MF_E_NO_MORE_TYPES)
                break;
            if (FAILED(status))
                return false;
            GUID subtype = {};
            if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12 &&
                SUCCEEDED(transform_->SetOutputType(
                    output_stream_id_, candidate.Get(), 0)))
            {
                selected = std::move(candidate);
                break;
            }
        }
        if (!selected)
            return false;
        output_type_ = std::move(selected);
        UINT32 width = 0;
        UINT32 height = 0;
        const HRESULT frame_size_status = MFGetAttributeSize(
            output_type_.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (SUCCEEDED(frame_size_status))
        {
            if (width == 0 || height == 0 ||
                width > kMaxVideoDimension || height > kMaxVideoDimension ||
                (width & 1u) != 0 || (height & 1u) != 0)
                return false;
            output_width_ = width;
            output_height_ = height;
        }
        UINT32 stride = 0;
        output_stride_ = output_width_;
        if (SUCCEEDED(output_type_->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)))
            output_stride_ = static_cast<LONG>(stride);
        return SUCCEEDED(transform_->GetOutputStreamInfo(
            output_stream_id_, &output_stream_info_));
    }

    bool ConfigureTransformObject(
        ComPtr<IMFTransform> transform, uint32_t width, uint32_t height)
    {
        if (!transform)
            return false;
        ComPtr<IMFAttributes> attributes;
        if (FAILED(transform->GetAttributes(&attributes)) || !attributes)
            return false;
        UINT32 d3d11_aware = FALSE;
        if (FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware)) ||
            !d3d11_aware)
            return false;
        // The WebRTC receive pipeline retains metadata for a bounded number of
        // outstanding decode callbacks. The stock H264 MFT otherwise buffers
        // roughly a GOP of output, so every callback can arrive after WebRTC
        // has evicted its RTP timestamp. Force the real-time decoder path.
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 asynchronous = FALSE;
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
        if (asynchronous &&
            FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)))
            return false;
        ComPtr<IMFMediaEventGenerator> event_generator;
        if (asynchronous && FAILED(transform.As(&event_generator)))
            return false;
        if (FAILED(transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get()))))
            return false;
        ComPtr<ICodecAPI> codec_api;
        if (SUCCEEDED(transform.As(&codec_api)))
        {
            VARIANT value;
            VariantInit(&value);
            // The Media Foundation H264 decoder is the documented exception
            // to the nominal VT_BOOL low-latency contract: it requires VT_UI4.
            // AVDecVideoAcceleration_H264 is also a VT_UI4 property.
            value.vt = VT_UI4;
            value.ulVal = TRUE;
            codec_api->SetValue(&CODECAPI_AVDecVideoAcceleration_H264, &value);
            codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &value);
            VariantClear(&value);
        }

        DWORD input_count = 0;
        DWORD output_count = 0;
        if (FAILED(transform->GetStreamCount(&input_count, &output_count)) ||
            input_count != 1 || output_count != 1)
            return false;
        DWORD input_id = 0;
        DWORD output_id = 0;
        const HRESULT id_status = transform->GetStreamIDs(
            1, &input_id, 1, &output_id);
        if (FAILED(id_status) && id_status != E_NOTIMPL)
            return false;

        ComPtr<IMFMediaType> input_type;
        if (FAILED(MFCreateMediaType(&input_type)) ||
            FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
            FAILED(MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
            FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, 30, 1)) ||
            FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(input_type->SetUINT32(
                MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) ||
            FAILED(transform->SetInputType(input_id, input_type.Get(), 0)))
            return false;

        transform_ = std::move(transform);
        event_generator_ = std::move(event_generator);
        asynchronous_ = asynchronous != FALSE;
        input_stream_id_ = input_id;
        output_stream_id_ = output_id;
        output_width_ = width;
        output_height_ = height;
        if (!ConfigureOutputType())
        {
            transform_.Reset();
            event_generator_.Reset();
            return false;
        }
        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        {
            transform_.Reset();
            event_generator_.Reset();
            return false;
        }
        active_width_ = width;
        active_height_ = height;
        need_input_count_ = asynchronous_ ? 0u : 1u;
        have_output_count_ = 0;
        return true;
    }

    bool ConfigureActivation(
        IMFActivate* activation, uint32_t width, uint32_t height)
    {
        ComPtr<IMFTransform> transform;
        return activation != nullptr &&
            SUCCEEDED(activation->ActivateObject(IID_PPV_ARGS(&transform))) &&
            ConfigureTransformObject(std::move(transform), width, height);
    }

    bool ConfigureSystemTransform(uint32_t width, uint32_t height)
    {
        ComPtr<IMFTransform> transform;
        return SUCCEEDED(CoCreateInstance(
                   CLSID_CMSH264DecoderMFT,
                   nullptr,
                   CLSCTX_INPROC_SERVER,
                   IID_PPV_ARGS(&transform))) &&
            ConfigureTransformObject(std::move(transform), width, height);
    }

    bool CreateTransform(uint32_t width, uint32_t height)
    {
        if (!CreateD3DDevice())
            return false;
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        bool configured = false;
        if (SUCCEEDED(EnumerateHardwareH264Decoders(
                adapter_luid_, &activations, &count)))
        {
            for (UINT32 index = 0; index < count && !configured; ++index)
            {
                configured = ConfigureActivation(
                    activations[index], width, height);
                if (configured)
                    activation_ = activations[index];
                else if (activations[index] != nullptr)
                    activations[index]->ShutdownObject();
            }
        }
        ReleaseActivations(activations, count);
        return configured || ConfigureSystemTransform(width, height);
    }

    void ShutdownTransform()
    {
        if (transform_)
        {
            transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        output_type_.Reset();
        event_generator_.Reset();
        transform_.Reset();
        if (activation_)
            activation_->ShutdownObject();
        activation_.Reset();
        staging_texture_.Reset();
        decoded_surface_slots_.clear();
        surface_ring_exhausted_ = false;
        dxgi_manager_.Reset();
        d3d12_device_.Reset();
        d3d_context4_.Reset();
        d3d_device5_.Reset();
        d3d_device1_.Reset();
        d3d_context_.Reset();
        d3d_device_.Reset();
        submitted_frames_.clear();
        need_input_count_ = 0;
        have_output_count_ = 0;
        asynchronous_ = false;
        active_width_ = 0;
        active_height_ = 0;
        output_width_ = 0;
        output_height_ = 0;
        output_stride_ = 0;
    }

    bool PumpEvents()
    {
        if (!event_generator_)
            return false;
        bool progressed = false;
        for (;;)
        {
            ComPtr<IMFMediaEvent> event;
            const HRESULT status = event_generator_->GetEvent(
                MF_EVENT_FLAG_NO_WAIT, &event);
            if (status == MF_E_NO_EVENTS_AVAILABLE)
                break;
            if (FAILED(status) || !event)
            {
                SignalFailure();
                break;
            }
            progressed = true;
            HRESULT event_status = S_OK;
            MediaEventType type = MEUnknown;
            event->GetStatus(&event_status);
            event->GetType(&type);
            if (FAILED(event_status) || type == MEError)
            {
                SignalFailure();
                break;
            }
            if (type == METransformNeedInput)
                ++need_input_count_;
            else if (type == METransformHaveOutput)
                ++have_output_count_;
        }
        return progressed;
    }

    HRESULT SubmitInput(const PendingFrame& frame)
    {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT status = MFCreateSample(&sample);
        if (SUCCEEDED(status))
            status = MFCreateMemoryBuffer(static_cast<DWORD>(frame.bytes.size()), &buffer);
        BYTE* destination = nullptr;
        if (SUCCEEDED(status))
            status = buffer->Lock(&destination, nullptr, nullptr);
        if (SUCCEEDED(status) && destination == nullptr)
        {
            buffer->Unlock();
            status = E_UNEXPECTED;
        }
        if (SUCCEEDED(status))
        {
            std::memcpy(destination, frame.bytes.data(), frame.bytes.size());
            buffer->Unlock();
            destination = nullptr;
            status = buffer->SetCurrentLength(static_cast<DWORD>(frame.bytes.size()));
        }
        else if (destination != nullptr)
        {
            buffer->Unlock();
        }
        if (SUCCEEDED(status))
            status = sample->AddBuffer(buffer.Get());
        constexpr LONGLONG kSampleDuration = 333'333;
        const LONGLONG sample_time = next_sample_time_;
        next_sample_time_ += kSampleDuration;
        if (SUCCEEDED(status))
            status = sample->SetSampleTime(sample_time);
        if (SUCCEEDED(status))
            status = sample->SetSampleDuration(kSampleDuration);
        if (SUCCEEDED(status) && frame.keyframe)
            status = sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (SUCCEEDED(status))
            status = transform_->ProcessInput(input_stream_id_, sample.Get(), 0);
        if (SUCCEEDED(status))
        {
            submitted_frames_.push_back({sample_time, frame.width, frame.height,
                frame.rtp_timestamp, frame.timestamp_usec});
            if (failure_)
                failure_->submitted_frames.fetch_add(1, std::memory_order_relaxed);
            while (submitted_frames_.size() > kMaxSubmittedFrames)
                submitted_frames_.pop_front();
        }
        return status;
    }

    bool EnsureStagingTexture(ID3D11Texture2D* source)
    {
        if (source == nullptr || !d3d_device_)
            return false;
        D3D11_TEXTURE2D_DESC source_desc = {};
        source->GetDesc(&source_desc);
        if (source_desc.Format != DXGI_FORMAT_NV12)
            return false;
        if (staging_texture_)
        {
            D3D11_TEXTURE2D_DESC staging_desc = {};
            staging_texture_->GetDesc(&staging_desc);
            if (staging_desc.Width == source_desc.Width &&
                staging_desc.Height == source_desc.Height &&
                staging_desc.Format == source_desc.Format)
                return true;
            staging_texture_.Reset();
        }
        source_desc.MipLevels = 1;
        source_desc.ArraySize = 1;
        source_desc.SampleDesc.Count = 1;
        source_desc.SampleDesc.Quality = 0;
        source_desc.Usage = D3D11_USAGE_STAGING;
        source_desc.BindFlags = 0;
        source_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        source_desc.MiscFlags = 0;
        return SUCCEEDED(d3d_device_->CreateTexture2D(
            &source_desc, nullptr, &staging_texture_));
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> ConvertDXGIToI420(
        IMFDXGIBuffer* dxgi_buffer, uint32_t width, uint32_t height)
    {
        if (dxgi_buffer == nullptr || !d3d_context_)
            return nullptr;
        ComPtr<ID3D11Texture2D> source;
        UINT subresource = 0;
        if (FAILED(dxgi_buffer->GetResource(IID_PPV_ARGS(&source))) ||
            FAILED(dxgi_buffer->GetSubresourceIndex(&subresource)) ||
            !EnsureStagingTexture(source.Get()))
            return nullptr;
        D3D11_TEXTURE2D_DESC source_desc = {};
        source->GetDesc(&source_desc);
        if (width > source_desc.Width || height > source_desc.Height)
            return nullptr;
        d3d_context_->CopySubresourceRegion(
            staging_texture_.Get(), 0, 0, 0, 0, source.Get(), subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(d3d_context_->Map(
                staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return nullptr;
        auto i420 = webrtc::I420Buffer::Create(
            static_cast<int>(width), static_cast<int>(height));
        const uint8_t* y_plane = static_cast<const uint8_t*>(mapped.pData);
        const uint8_t* uv_plane = y_plane +
            static_cast<size_t>(mapped.RowPitch) * source_desc.Height;
        const int result = i420 ? libyuv::NV12ToI420(
            y_plane,
            static_cast<int>(mapped.RowPitch),
            uv_plane,
            static_cast<int>(mapped.RowPitch),
            i420->MutableDataY(),
            i420->StrideY(),
            i420->MutableDataU(),
            i420->StrideU(),
            i420->MutableDataV(),
            i420->StrideV(),
            static_cast<int>(width),
            static_cast<int>(height)) : -1;
        d3d_context_->Unmap(staging_texture_.Get(), 0);
        return result == 0 ? i420 : nullptr;
    }

    struct DecodedSurfaceSlot
    {
        ComPtr<ID3D12Resource> shared_texture;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11Fence> fence;
        HANDLE texture_handle = nullptr;
        HANDLE fence_handle = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t next_fence_value = 0;
        uint64_t last_consumer_fence_value = 0;
        std::shared_ptr<std::atomic_bool> last_completion_scheduled;

        ~DecodedSurfaceSlot()
        {
            if (fence_handle != nullptr)
                CloseHandle(fence_handle);
            if (texture_handle != nullptr)
                CloseHandle(texture_handle);
        }
    };

    std::shared_ptr<DecodedSurfaceSlot> AcquireDecodedSurface(
        uint32_t width, uint32_t height)
    {
        surface_ring_exhausted_ = false;
        if (failure_ && failure_->native_surface_failed.load(
                std::memory_order_acquire))
            return nullptr;
        const auto consumer_finished = [](const auto& slot) {
            if (!slot)
                return true;
            const bool completion_was_scheduled =
                slot->last_completion_scheduled &&
                slot->last_completion_scheduled->load(std::memory_order_acquire);
            return !completion_was_scheduled || !slot->fence ||
                slot->fence->GetCompletedValue() >=
                    slot->last_consumer_fence_value;
        };
        decoded_surface_slots_.erase(std::remove_if(
            decoded_surface_slots_.begin(), decoded_surface_slots_.end(),
            [&](const auto& slot) {
                return slot && slot.use_count() == 1 &&
                    (slot->width != width || slot->height != height) &&
                    consumer_finished(slot);
            }), decoded_surface_slots_.end());
        for (const auto& slot : decoded_surface_slots_)
        {
            if (!slot || slot.use_count() != 1 ||
                slot->width != width || slot->height != height)
                continue;
            if (consumer_finished(slot))
                return slot;
        }
        if (decoded_surface_slots_.size() >= kMaxPendingFrames ||
            !d3d_device1_ || !d3d_device5_ || !d3d12_device_)
        {
            surface_ring_exhausted_ =
                decoded_surface_slots_.size() >= kMaxPendingFrames;
            if (failure_)
            {
                if (surface_ring_exhausted_)
                    failure_->native_ring_drops.fetch_add(
                        1, std::memory_order_relaxed);
                else
                    failure_->native_allocation_failures.fetch_add(
                        1, std::memory_order_relaxed);
            }
            return nullptr;
        }

        auto slot = std::make_shared<DecodedSurfaceSlot>();
        uint32_t failure_stage = 1;
        HRESULT status =
            wicked_newpipeline::CreateWindowsD3D11CompatibleNV12Texture(
                d3d12_device_.Get(), width, height,
                D3D11_BIND_SHADER_RESOURCE, &slot->shared_texture);
        if (SUCCEEDED(status))
        {
            failure_stage = 2;
            status = d3d12_device_->CreateSharedHandle(
                slot->shared_texture.Get(), nullptr, GENERIC_ALL, nullptr,
                &slot->texture_handle);
        }
        if (SUCCEEDED(status))
        {
            failure_stage = 3;
            status = d3d_device1_->OpenSharedResource1(
                slot->texture_handle, IID_PPV_ARGS(&slot->texture));
        }
        if (SUCCEEDED(status))
        {
            failure_stage = 4;
            status = d3d_device5_->CreateFence(
                0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&slot->fence));
        }
        if (SUCCEEDED(status))
        {
            failure_stage = 5;
            status = slot->fence->CreateSharedHandle(
                nullptr, GENERIC_ALL, nullptr, &slot->fence_handle);
        }
        if (FAILED(status))
        {
            if (failure_)
            {
                failure_->native_surface_failed.store(
                    true, std::memory_order_release);
                failure_->native_allocation_failures.fetch_add(
                    1, std::memory_order_relaxed);
                failure_->native_failure_stage.store(
                    failure_stage, std::memory_order_relaxed);
                failure_->native_failure_hresult.store(
                    static_cast<uint32_t>(status), std::memory_order_relaxed);
            }
            return nullptr;
        }
        slot->width = width;
        slot->height = height;
        decoded_surface_slots_.push_back(slot);
        return slot;
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> RetainDXGINV12(
        IMFDXGIBuffer* dxgi_buffer, uint32_t width, uint32_t height,
        int64_t source_timestamp_usec)
    {
        if (dxgi_buffer == nullptr || !d3d_context4_ || adapter_luid_ == 0)
            return nullptr;
        if (failure_)
            failure_->native_surface_attempts.fetch_add(
                1, std::memory_order_relaxed);
        ComPtr<ID3D11Texture2D> source;
        UINT subresource = 0;
        if (FAILED(dxgi_buffer->GetResource(IID_PPV_ARGS(&source))) ||
            FAILED(dxgi_buffer->GetSubresourceIndex(&subresource)))
            return nullptr;
        D3D11_TEXTURE2D_DESC source_desc = {};
        source->GetDesc(&source_desc);
        if (source_desc.Format != DXGI_FORMAT_NV12 ||
            width > source_desc.Width || height > source_desc.Height)
            return nullptr;
        auto slot = AcquireDecodedSurface(width, height);
        if (!slot)
            return nullptr;
        surface_ring_exhausted_ = false;

        const uint64_t producer_value = slot->next_fence_value + 1u;
        const uint64_t consumer_value = producer_value + 1u;
        slot->next_fence_value = consumer_value;
        auto completion_scheduled = std::make_shared<std::atomic_bool>(false);
        slot->last_completion_scheduled = completion_scheduled;
        slot->last_consumer_fence_value = consumer_value;
        if (source_desc.Width == width && source_desc.Height == height)
        {
            d3d_context_->CopySubresourceRegion(
                slot->texture.Get(), 0, 0, 0, 0,
                source.Get(), subresource, nullptr);
        }
        else
        {
            const D3D11_BOX visible_region = {
                0, 0, 0, width, height, 1};
            d3d_context_->CopySubresourceRegion(
                slot->texture.Get(), 0, 0, 0, 0,
                source.Get(), subresource, &visible_region);
        }
        const HRESULT producer_signal_status = d3d_context4_->Signal(
            slot->fence.Get(), producer_value);
        if (FAILED(producer_signal_status))
        {
            if (failure_)
            {
                failure_->native_surface_failed.store(
                    true, std::memory_order_release);
                failure_->native_signal_failures.fetch_add(
                    1, std::memory_order_relaxed);
                failure_->native_failure_stage.store(
                    6, std::memory_order_relaxed);
                failure_->native_failure_hresult.store(
                    static_cast<uint32_t>(producer_signal_status),
                    std::memory_order_relaxed);
            }
            return nullptr;
        }

        NPWindowsNativeNV12Surface surface;
        surface.width = width;
        surface.height = height;
        surface.texture_shared_handle = slot->texture_handle;
        surface.fence_shared_handle = slot->fence_handle;
        surface.producer_fence_value = producer_value;
        surface.consumer_fence_value = consumer_value;
        surface.adapter_luid = adapter_luid_;
        surface.source_timestamp_usec = source_timestamp_usec;
        // The render path normally signals consumer_value after its D3D12
        // unpack. A decoded frame can also be superseded in WebRTC's receive
        // queue or in the RTP/metadata pairing queue before it ever reaches
        // that path. Return such an unconsumed slot from the buffer destructor
        // or the retained-surface ring permanently loses one entry per drop.
        // CreateD3D11VideoDevice enables multithread protection, so this
        // fallback Signal is safe when the last reference is released on a
        // WebRTC/client thread instead of the decoder worker.
        ComPtr<ID3D11DeviceContext4> completion_context = d3d_context4_;
        const auto decoder_failure = failure_;
        auto retained = np_create_windows_native_nv12_buffer(
            surface,
            [slot, completion_scheduled, completion_context,
                consumer_value, decoder_failure]() {
                if (completion_scheduled->exchange(
                        true, std::memory_order_acq_rel))
                    return;
                const HRESULT status = completion_context
                    ? completion_context->Signal(
                        slot->fence.Get(), consumer_value)
                    : E_NOINTERFACE;
                if (FAILED(status))
                {
                    if (decoder_failure)
                    {
                        decoder_failure->native_surface_failed.store(
                            true, std::memory_order_release);
                        decoder_failure->native_signal_failures.fetch_add(
                            1, std::memory_order_relaxed);
                        decoder_failure->native_failure_stage.store(
                            7, std::memory_order_relaxed);
                        decoder_failure->native_failure_hresult.store(
                            static_cast<uint32_t>(status),
                            std::memory_order_relaxed);
                    }
                }
            },
            [completion_scheduled]() {
                completion_scheduled->exchange(true, std::memory_order_acq_rel);
            });
        if (retained && failure_)
            failure_->native_surface_outputs.fetch_add(
                1, std::memory_order_relaxed);
        return retained;
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> ConvertSystemMemoryToI420(
        IMFMediaBuffer* buffer, uint32_t width, uint32_t height,
        uint32_t coded_height)
    {
        if (buffer == nullptr || coded_height < height)
            return nullptr;
        auto i420 = webrtc::I420Buffer::Create(
            static_cast<int>(width), static_cast<int>(height));
        if (!i420)
            return nullptr;
        ComPtr<IMF2DBuffer> buffer_2d;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer_2d))))
        {
            BYTE* scanline = nullptr;
            LONG pitch = 0;
            if (FAILED(buffer_2d->Lock2D(&scanline, &pitch)))
                return nullptr;
            if (scanline == nullptr || pitch < static_cast<LONG>(width))
            {
                buffer_2d->Unlock2D();
                return nullptr;
            }
            const uint8_t* uv_plane = scanline +
                static_cast<size_t>(pitch) * coded_height;
            const int result = libyuv::NV12ToI420(
                scanline,
                pitch,
                uv_plane,
                pitch,
                i420->MutableDataY(),
                i420->StrideY(),
                i420->MutableDataU(),
                i420->StrideU(),
                i420->MutableDataV(),
                i420->StrideV(),
                static_cast<int>(width),
                static_cast<int>(height));
            buffer_2d->Unlock2D();
            return result == 0 ? i420 : nullptr;
        }

        BYTE* data = nullptr;
        DWORD current_length = 0;
        const HRESULT lock_status = buffer->Lock(
            &data, nullptr, &current_length);
        if (FAILED(lock_status))
            return nullptr;
        if (data == nullptr)
        {
            buffer->Unlock();
            return nullptr;
        }
        const LONG stride = output_stride_ > 0
            ? output_stride_ : static_cast<LONG>(width);
        if (stride < static_cast<LONG>(width))
        {
            buffer->Unlock();
            return nullptr;
        }
        const size_t required = static_cast<size_t>(stride) * coded_height +
            static_cast<size_t>(stride) * ((coded_height + 1u) / 2u);
        int result = -1;
        if (current_length >= required)
        {
            const uint8_t* uv_plane = data +
                static_cast<size_t>(stride) * coded_height;
            result = libyuv::NV12ToI420(
                data,
                stride,
                uv_plane,
                stride,
                i420->MutableDataY(),
                i420->StrideY(),
                i420->MutableDataU(),
                i420->StrideU(),
                i420->MutableDataV(),
                i420->StrideV(),
                static_cast<int>(width),
                static_cast<int>(height));
        }
        buffer->Unlock();
        return result == 0 ? i420 : nullptr;
    }

    bool DeliverOutput(IMFSample* sample)
    {
        if (sample == nullptr)
            return false;
        LONGLONG sample_time = 0;
        const bool has_sample_time = SUCCEEDED(sample->GetSampleTime(&sample_time));
        auto metadata = submitted_frames_.begin();
        if (has_sample_time)
        {
            metadata = std::find_if(
                submitted_frames_.begin(), submitted_frames_.end(),
                [&](const SubmittedFrame& candidate) {
                    return candidate.sample_time == sample_time;
                });
        }
        if (metadata == submitted_frames_.end())
            return false;
        const SubmittedFrame frame_metadata = *metadata;
        submitted_frames_.erase(metadata);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer)) || !buffer)
            return false;
        // The decoder surface can be macroblock-aligned (for example 640x368
        // for a 640x360 stream). Keep that coded height for locating the NV12
        // chroma plane, but expose/copy only the visible dimensions carried by
        // the submitted RTP frame.
        const uint32_t width = frame_metadata.width;
        const uint32_t height = frame_metadata.height;
        const uint32_t coded_height = output_height_ > 0
            ? output_height_ : frame_metadata.height;
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> decoded_buffer;
        ComPtr<IMFDXGIBuffer> dxgi_buffer;
        if (SUCCEEDED(buffer.As(&dxgi_buffer)))
        {
            // A missing source identity must use the pixel-band I420 path.
            // Receiver capture_time_ms_ is not a substitute for producer time.
            if (frame_metadata.timestamp_usec > 0)
                decoded_buffer = RetainDXGINV12(
                    dxgi_buffer.Get(), width, height,
                    frame_metadata.timestamp_usec);
            else
                surface_ring_exhausted_ = false;
            // A DXGI-backed decoder output must remain valid even when native
            // cross-API retention is unavailable (for example, a missing LUID
            // or a transient shared-surface allocation failure). Falling back
            // to an I420 copy here keeps the negotiated H264 session alive;
            // treating this as a codec failure caused a full VP8 reconnect.
            if (!decoded_buffer && !surface_ring_exhausted_)
                decoded_buffer = ConvertDXGIToI420(
                    dxgi_buffer.Get(), width, height);
        }
        else
            decoded_buffer = ConvertSystemMemoryToI420(
                buffer.Get(), width, height, coded_height);
        if (!decoded_buffer)
            return false;

        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder{}
            .set_video_frame_buffer(decoded_buffer)
            .set_rtp_timestamp(frame_metadata.rtp_timestamp)
            .set_timestamp_us(frame_metadata.timestamp_usec)
            .build();
        webrtc::DecodedImageCallback* callback = nullptr;
        {
            std::lock_guard lock(callback_mutex_);
            callback = callback_;
        }
        if (callback != nullptr)
        {
            callback->Decoded(frame, std::nullopt, std::nullopt);
            if (failure_)
                failure_->decoded_frames.fetch_add(1, std::memory_order_relaxed);
        }
        return callback != nullptr;
    }

    bool ProcessOneOutput()
    {
        MFT_OUTPUT_DATA_BUFFER output = {};
        output.dwStreamID = output_stream_id_;
        ComPtr<IMFSample> caller_sample;
        if ((output_stream_info_.dwFlags &
                (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                 MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0)
        {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateSample(&caller_sample)) ||
                FAILED(MFCreateMemoryBuffer(
                    std::max<DWORD>(output_stream_info_.cbSize, 1u), &buffer)) ||
                FAILED(caller_sample->AddBuffer(buffer.Get())))
            {
                SignalFailure();
                return false;
            }
            output.pSample = caller_sample.Get();
        }
        DWORD status = 0;
        const HRESULT result = transform_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents != nullptr)
            output.pEvents->Release();
        if (result == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            const bool configured = ConfigureOutputType();
            if (!asynchronous_ && configured)
                have_output_count_ = 1;
            return configured;
        }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            have_output_count_ = 0;
            if (!asynchronous_)
                need_input_count_ = 1;
            return false;
        }
        if (FAILED(result))
        {
            SignalFailure();
            return false;
        }
        if (asynchronous_ && have_output_count_ > 0)
            --have_output_count_;
        else if (!asynchronous_)
            have_output_count_ = 1;
        ComPtr<IMFSample> provided_sample;
        IMFSample* sample = caller_sample.Get();
        if (!caller_sample && output.pSample != nullptr)
        {
            provided_sample.Attach(output.pSample);
            sample = provided_sample.Get();
        }
        if (DeliverOutput(sample))
            consecutive_output_failures_ = 0;
        else if (surface_ring_exhausted_)
        {
            // Bounded retained-surface pressure is a frame drop, not a codec
            // failure. Never wait the decoder thread for the render queue.
            surface_ring_exhausted_ = false;
            consecutive_output_failures_ = 0;
        }
        else if (++consecutive_output_failures_ >= 3)
            SignalFailure();
        if ((output.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) != 0)
            have_output_count_ = std::max<uint32_t>(have_output_count_, 1);
        return true;
    }

    void WorkerMain()
    {
        ScopedCOM com;
        if (!com.IsAvailable() || !EnsureMediaFoundationStarted())
        {
            SignalFailure();
            return;
        }

        while (!stop_requested_.load(std::memory_order_acquire) &&
            !failed_.load(std::memory_order_acquire))
        {
            bool progressed = PumpEvents();

            while (have_output_count_ > 0 &&
                !failed_.load(std::memory_order_acquire))
            {
                progressed = ProcessOneOutput() || progressed;
                if (have_output_count_ == 0)
                    break;
            }

            PendingFrame frame;
            bool has_frame = false;
            {
                std::lock_guard lock(queue_mutex_);
                if (!pending_frames_.empty())
                {
                    frame = std::move(pending_frames_.front());
                    pending_frames_.pop_front();
                    has_frame = true;
                }
            }
            if (has_frame)
            {
                if (!transform_)
                {
                    if (!frame.keyframe || !CreateTransform(frame.width, frame.height))
                    {
                        if (!frame.keyframe)
                        {
                            waiting_for_keyframe_.store(true, std::memory_order_release);
                            request_keyframe_.store(true, std::memory_order_release);
                        }
                        else
                        {
                            SignalFailure();
                        }
                        continue;
                    }
                    std::lock_guard lock(queue_mutex_);
                    pending_frames_.push_front(std::move(frame));
                    progressed = true;
                }
                else if (frame.width != active_width_ || frame.height != active_height_)
                {
                    if (!frame.keyframe)
                    {
                        waiting_for_keyframe_.store(true, std::memory_order_release);
                        request_keyframe_.store(true, std::memory_order_release);
                    }
                    else
                    {
                        ShutdownTransform();
                        std::lock_guard lock(queue_mutex_);
                        pending_frames_.push_front(std::move(frame));
                    }
                    progressed = true;
                }
                else if (need_input_count_ > 0)
                {
                    const HRESULT status = SubmitInput(frame);
                    if (status == MF_E_NOTACCEPTING)
                    {
                        std::lock_guard lock(queue_mutex_);
                        pending_frames_.push_front(std::move(frame));
                    }
                    else if (FAILED(status))
                    {
                        SignalFailure();
                    }
                    else
                    {
                        --need_input_count_;
                        if (!asynchronous_)
                            have_output_count_ = 1;
                    }
                    progressed = true;
                }
                else
                {
                    std::lock_guard lock(queue_mutex_);
                    pending_frames_.push_front(std::move(frame));
                }
            }

            if (!progressed)
            {
                std::unique_lock lock(queue_mutex_);
                queue_condition_.wait_for(lock, kWorkerIdleWait);
            }
        }
        ShutdownTransform();
    }

    std::shared_ptr<NPWindowsHardwareDecoderFailureSignal> failure_;
    uint64_t adapter_luid_ = 0;
    std::atomic_bool configured_{false};
    std::atomic_bool stop_requested_{false};
    std::atomic_bool failed_{false};
    std::atomic_bool waiting_for_keyframe_{true};
    std::atomic_bool request_keyframe_{false};
    std::atomic_uint32_t last_input_width_{0};
    std::atomic_uint32_t last_input_height_{0};
    std::thread worker_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::deque<PendingFrame> pending_frames_;
    std::mutex callback_mutex_;
    webrtc::DecodedImageCallback* callback_ = nullptr;

    ComPtr<ID3D11Device> d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    ComPtr<ID3D11Device1> d3d_device1_;
    ComPtr<ID3D11Device5> d3d_device5_;
    ComPtr<ID3D11DeviceContext4> d3d_context4_;
    ComPtr<ID3D12Device> d3d12_device_;
    ComPtr<ID3D11Texture2D> staging_texture_;
    std::vector<std::shared_ptr<DecodedSurfaceSlot>> decoded_surface_slots_;
    ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
    UINT dxgi_reset_token_ = 0;
    ComPtr<IMFActivate> activation_;
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaEventGenerator> event_generator_;
    ComPtr<IMFMediaType> output_type_;
    DWORD input_stream_id_ = 0;
    DWORD output_stream_id_ = 0;
    MFT_OUTPUT_STREAM_INFO output_stream_info_ = {};
    uint32_t need_input_count_ = 0;
    uint32_t have_output_count_ = 0;
    bool asynchronous_ = false;
    uint32_t active_width_ = 0;
    uint32_t active_height_ = 0;
    uint32_t output_width_ = 0;
    uint32_t output_height_ = 0;
    LONG output_stride_ = 0;
    LONGLONG next_sample_time_ = 0;
    uint32_t consecutive_output_failures_ = 0;
    bool surface_ring_exhausted_ = false;
    std::deque<SubmittedFrame> submitted_frames_;
};

class MediaFoundationEncoderFactory final : public webrtc::VideoEncoderFactory
{
public:
    explicit MediaFoundationEncoderFactory(
        std::shared_ptr<NPWindowsHardwareEncoderFailureSignal> failure,
        uint64_t adapter_luid) :
        failure_(std::move(failure)), adapter_luid_(adapter_luid)
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        return HardwareH264EncoderAvailable(adapter_luid_)
            ? std::vector<webrtc::SdpVideoFormat>{webrtc::SdpVideoFormat::H264()}
            : std::vector<webrtc::SdpVideoFormat>{};
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode,
        std::optional<webrtc::Resolution>) const override
    {
        const bool supported = IsH264(format) && !scalability_mode.has_value() &&
            HardwareH264EncoderAvailable(adapter_luid_);
        return {supported, supported};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment&,
        const webrtc::SdpVideoFormat& format) override
    {
        return IsH264(format) && HardwareH264EncoderAvailable(adapter_luid_)
            ? std::make_unique<MediaFoundationH264Encoder>(failure_, adapter_luid_)
            : nullptr;
    }

private:
    std::shared_ptr<NPWindowsHardwareEncoderFailureSignal> failure_;
    uint64_t adapter_luid_ = 0;
};

class MediaFoundationDecoderFactory final : public webrtc::VideoDecoderFactory
{
public:
    explicit MediaFoundationDecoderFactory(
        std::shared_ptr<NPWindowsHardwareDecoderFailureSignal> failure,
        uint64_t adapter_luid) :
        failure_(std::move(failure)), adapter_luid_(adapter_luid)
    {
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        return HardwareH264DecoderAvailable(adapter_luid_)
            ? std::vector<webrtc::SdpVideoFormat>{webrtc::SdpVideoFormat::H264()}
            : std::vector<webrtc::SdpVideoFormat>{};
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        bool reference_scaling,
        std::optional<webrtc::Resolution>) const override
    {
        const bool supported = IsH264(format) && !reference_scaling &&
            HardwareH264DecoderAvailable(adapter_luid_);
        return {supported, supported};
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment&,
        const webrtc::SdpVideoFormat& format) override
    {
        return IsH264(format) && HardwareH264DecoderAvailable(adapter_luid_)
            ? std::make_unique<MediaFoundationH264Decoder>(failure_, adapter_luid_)
            : nullptr;
    }

private:
    std::shared_ptr<NPWindowsHardwareDecoderFailureSignal> failure_;
    uint64_t adapter_luid_ = 0;
};

class CombinedVideoEncoderFactory final : public webrtc::VideoEncoderFactory
{
public:
    void Add(std::unique_ptr<webrtc::VideoEncoderFactory> factory)
    {
        if (factory)
            factories_.push_back(std::move(factory));
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        std::vector<webrtc::SdpVideoFormat> result;
        for (const auto& factory : factories_)
        {
            for (const auto& format : factory->GetSupportedFormats())
            {
                if (!ContainsFormat(result, format))
                    result.push_back(format);
            }
        }
        return result;
    }

    std::vector<webrtc::SdpVideoFormat> GetImplementations() const override
    {
        std::vector<webrtc::SdpVideoFormat> result;
        for (const auto& factory : factories_)
        {
            for (const auto& format : factory->GetImplementations())
            {
                if (!ContainsFormat(result, format))
                    result.push_back(format);
            }
        }
        return result;
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode,
        std::optional<webrtc::Resolution> resolution) const override
    {
        for (const auto& factory : factories_)
        {
            const CodecSupport support = factory->QueryCodecSupport(
                format, scalability_mode, resolution);
            if (support.is_supported)
                return support;
        }
        return {};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& environment,
        const webrtc::SdpVideoFormat& format) override
    {
        for (const auto& factory : factories_)
        {
            if (auto encoder = factory->Create(environment, format))
                return encoder;
        }
        return nullptr;
    }

private:
    std::vector<std::unique_ptr<webrtc::VideoEncoderFactory>> factories_;
};

class CombinedVideoDecoderFactory final : public webrtc::VideoDecoderFactory
{
public:
    void Add(std::unique_ptr<webrtc::VideoDecoderFactory> factory)
    {
        if (factory)
            factories_.push_back(std::move(factory));
    }

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override
    {
        std::vector<webrtc::SdpVideoFormat> result;
        for (const auto& factory : factories_)
        {
            for (const auto& format : factory->GetSupportedFormats())
            {
                if (!ContainsFormat(result, format))
                    result.push_back(format);
            }
        }
        return result;
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        bool reference_scaling,
        std::optional<webrtc::Resolution> resolution) const override
    {
        for (const auto& factory : factories_)
        {
            const CodecSupport support = factory->QueryCodecSupport(
                format, reference_scaling, resolution);
            if (support.is_supported)
                return support;
        }
        return {};
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& environment,
        const webrtc::SdpVideoFormat& format) override
    {
        for (const auto& factory : factories_)
        {
            if (auto decoder = factory->Create(environment, format))
                return decoder;
        }
        return nullptr;
    }

private:
    std::vector<std::unique_ptr<webrtc::VideoDecoderFactory>> factories_;
};

class EncoderValidationCallback final : public webrtc::EncodedImageCallback
{
public:
    Result OnEncodedImage(
        const webrtc::EncodedImage& image,
        const webrtc::CodecSpecificInfo* codec_info) override
    {
        std::vector<uint8_t> bytes;
        if (image.data() != nullptr && image.size() > 0)
            bytes.assign(image.data(), image.data() + image.size());
        {
            std::lock_guard lock(mutex_);
            complete_ = true;
            valid_ = image.IsKey() && codec_info != nullptr &&
                codec_info->codecType == webrtc::kVideoCodecH264 &&
                image.RtpTimestamp() == 90'000 &&
                AnnexBContainsNalType(bytes, 5) &&
                AnnexBContainsNalType(bytes, 7) &&
                AnnexBContainsNalType(bytes, 8) &&
                AnnexBHasConstrainedBaselineSps(bytes);
        }
        condition_.notify_all();
        return Result(Result::OK, image.RtpTimestamp());
    }

    bool WaitForValidFrame()
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, kEncoderStartupTimeout, [&] {
            return complete_;
        }) && valid_;
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool complete_ = false;
    bool valid_ = false;
};
} // namespace

webrtc::scoped_refptr<webrtc::VideoFrameBuffer>
np_create_windows_native_nv12_buffer(
    const NPWindowsNativeNV12Surface& surface,
    std::function<void()> release,
    std::function<void()> completion_scheduled)
{
    if (surface.width == 0 || surface.height == 0 ||
        surface.width > kMaxVideoDimension ||
        surface.height > kMaxVideoDimension ||
        (surface.width & 1u) != 0 || (surface.height & 1u) != 0 ||
        surface.texture_shared_handle == nullptr ||
        surface.fence_shared_handle == nullptr ||
        surface.producer_fence_value == 0 ||
        surface.consumer_fence_value <= surface.producer_fence_value ||
        surface.adapter_luid == 0)
    {
        if (release)
            release();
        return nullptr;
    }
    return webrtc::make_ref_counted<WindowsNativeNV12Buffer>(
        surface, std::move(release), std::move(completion_scheduled));
}

bool np_get_windows_native_nv12_surface(
    webrtc::VideoFrameBuffer* buffer,
    NPWindowsNativeNV12Surface& surface)
{
    auto* native = AsWindowsNativeNV12Buffer(buffer);
    if (native == nullptr)
        return false;
    surface = native->surface();
    return true;
}

void np_mark_windows_native_nv12_completion_scheduled(
    webrtc::VideoFrameBuffer* buffer)
{
    if (auto* native = AsWindowsNativeNV12Buffer(buffer))
        native->MarkCompletionScheduled();
}

NPWindowsVideoCodecFactories np_create_windows_video_codec_factories(
    bool request_hardware_encoder,
    uint64_t adapter_luid)
{
    NPWindowsVideoCodecFactories result;
    result.hardware_encoder_failure =
        std::make_shared<NPWindowsHardwareEncoderFailureSignal>();
    result.hardware_decoder_failure =
        std::make_shared<NPWindowsHardwareDecoderFailureSignal>();
    result.hardware_encoder_available =
        request_hardware_encoder && HardwareH264EncoderAvailable(adapter_luid);
    result.hardware_decoder_available =
        HardwareH264DecoderAvailable(adapter_luid);
    auto encoders = std::make_unique<CombinedVideoEncoderFactory>();
    if (result.hardware_encoder_available)
    {
        encoders->Add(std::make_unique<MediaFoundationEncoderFactory>(
            result.hardware_encoder_failure, adapter_luid));
    }
    encoders->Add(std::make_unique<webrtc::VideoEncoderFactoryTemplate<
        webrtc::LibvpxVp8EncoderTemplateAdapter>>());
    auto decoders = std::make_unique<CombinedVideoDecoderFactory>();
    if (result.hardware_decoder_available)
    {
        decoders->Add(std::make_unique<MediaFoundationDecoderFactory>(
            result.hardware_decoder_failure, adapter_luid));
    }
    decoders->Add(std::make_unique<webrtc::VideoDecoderFactoryTemplate<
        webrtc::LibvpxVp8DecoderTemplateAdapter>>());
    result.encoder = std::move(encoders);
    result.decoder = std::move(decoders);
    return result;
}

extern "C" uint64_t np_get_windows_default_video_adapter_luid()
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateD3D11VideoDevice(device, context))
        return 0;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(device.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc)))
        return 0;
    return static_cast<uint64_t>(
        static_cast<uint32_t>(desc.AdapterLuid.LowPart)) |
        (static_cast<uint64_t>(
            static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32u);
}

extern "C" int np_validate_windows_video_codec_factories(
    char* error, size_t error_capacity)
{
    const auto fail = [error, error_capacity](const char* message) {
        if (error != nullptr && error_capacity > 0)
        {
            const size_t count = std::min(std::strlen(message), error_capacity - 1u);
            std::memcpy(error, message, count);
            error[count] = '\0';
        }
        return 0;
    };
    std::atomic_uint32_t native_release_count{0};
    std::atomic_uint32_t native_completion_count{0};
    NPWindowsNativeNV12Surface native_test_surface;
    native_test_surface.width = 64;
    native_test_surface.height = 32;
    native_test_surface.texture_shared_handle = reinterpret_cast<void*>(1);
    native_test_surface.fence_shared_handle = reinterpret_cast<void*>(2);
    native_test_surface.producer_fence_value = 3;
    native_test_surface.consumer_fence_value = 4;
    native_test_surface.adapter_luid = 5;
    native_test_surface.source_timestamp_usec = 123456789;
    auto native_test = np_create_windows_native_nv12_buffer(
        native_test_surface,
        [&native_release_count]() { native_release_count.fetch_add(1); },
        [&native_completion_count]() { native_completion_count.fetch_add(1); });
    NPWindowsNativeNV12Surface native_test_roundtrip;
    if (!native_test || native_test->type() !=
            webrtc::VideoFrameBuffer::Type::kNative ||
        !np_get_windows_native_nv12_surface(
            native_test.get(), native_test_roundtrip) ||
        native_test_roundtrip.texture_shared_handle !=
            native_test_surface.texture_shared_handle ||
        native_test_roundtrip.consumer_fence_value != 4 ||
        native_test_roundtrip.source_timestamp_usec != 123456789)
        return fail("Windows retained NV12 frame-buffer roundtrip failed");
    np_mark_windows_native_nv12_completion_scheduled(native_test.get());
    np_mark_windows_native_nv12_completion_scheduled(native_test.get());
    native_test = nullptr;
    if (native_completion_count.load() != 1 || native_release_count.load() != 1)
        return fail("Windows retained NV12 completion/release was not exactly once");
    NPWindowsNativeNV12Surface invalid_native_test_surface;
    auto invalid_native_test = np_create_windows_native_nv12_buffer(
        invalid_native_test_surface,
        [&native_release_count]() { native_release_count.fetch_add(1); },
        [&native_completion_count]() { native_completion_count.fetch_add(1); });
    if (invalid_native_test || native_release_count.load() != 2 ||
        native_completion_count.load() != 1)
        return fail("Windows invalid retained NV12 surface leaked its release callback");

    std::vector<uint8_t> identity_test = {
        0, 0, 0, 1, 0x65, 0x88, 0x84,
    };
    PrependFrameIdentitySEI(identity_test, 123456789);
    const std::optional<int64_t> identity_timestamp =
        ParseFrameIdentitySEI(identity_test.data(), identity_test.size());
    if (!identity_timestamp || *identity_timestamp != 123456789)
        return fail("Windows H264 frame-identity SEI roundtrip failed");

    NPWindowsVideoCodecFactories software =
        np_create_windows_video_codec_factories(false, 0);
    if (!software.encoder || !software.decoder)
        return fail("Windows codec factory construction failed");
    const auto software_encoder_formats = software.encoder->GetSupportedFormats();
    if (std::any_of(
            software_encoder_formats.begin(), software_encoder_formats.end(), IsH264))
        return fail("Forced software mode advertised an H264 encoder");
    const bool has_vp8_encoder = std::any_of(
        software_encoder_formats.begin(), software_encoder_formats.end(),
        IsVp8);
    if (!has_vp8_encoder)
        return fail("Windows codec factory lost the VP8 fallback encoder");

    const uint64_t validation_adapter_luid =
        np_get_windows_default_video_adapter_luid();
    NPWindowsVideoCodecFactories factories =
        np_create_windows_video_codec_factories(
            true, validation_adapter_luid);
    if (!factories.encoder || !factories.decoder)
        return fail("Windows hardware codec factory construction failed");
    const char* failed_shared_profile = nullptr;
    const HRESULT shared_surface_status = ValidateD3D11D3D12NV12Sharing(
            validation_adapter_luid,
            factories.hardware_encoder_available,
            factories.hardware_decoder_available,
            &failed_shared_profile);
    if (FAILED(shared_surface_status))
    {
        char message[128] = {};
        std::snprintf(message, sizeof(message),
            "Windows D3D11/D3D12 NV12 %s shared-surface self-test failed: 0x%08X",
            failed_shared_profile != nullptr ? failed_shared_profile : "device",
            static_cast<uint32_t>(shared_surface_status));
        return fail(message);
    }
    const auto encoder_formats = factories.encoder->GetSupportedFormats();
    const bool has_h264_encoder = std::any_of(
        encoder_formats.begin(), encoder_formats.end(), IsH264);
    if (has_h264_encoder != factories.hardware_encoder_available)
        return fail("Windows H264 encoder capability telemetry is inconsistent");
    if (!std::any_of(encoder_formats.begin(), encoder_formats.end(), IsVp8))
        return fail("Windows hardware codec factory lost the VP8 fallback encoder");
    if (has_h264_encoder &&
        (encoder_formats.empty() || !IsH264(encoder_formats.front())))
        return fail("Windows hardware codec factory does not prefer H264 encoding");
    if (has_h264_encoder)
    {
        const webrtc::SdpVideoFormat h264 = webrtc::SdpVideoFormat::H264();
        const auto support = factories.encoder->QueryCodecSupport(
            h264, std::nullopt, std::nullopt);
        if (!support.is_supported || !support.is_power_efficient)
            return fail("Media Foundation H264 encoder is not power efficient");
        const webrtc::Environment environment = webrtc::CreateEnvironment();
        std::unique_ptr<webrtc::VideoEncoder> encoder =
            factories.encoder->Create(environment, h264);
        if (!encoder)
            return fail("Media Foundation H264 encoder creation failed");
        webrtc::VideoCodec codec;
        codec.codecType = webrtc::kVideoCodecH264;
        codec.width = 640;
        codec.height = 360;
        codec.startBitrate = 1000;
        codec.maxBitrate = 4000;
        codec.minBitrate = 100;
        codec.maxFramerate = 30;
        codec.numberOfSimulcastStreams = 1;
        const webrtc::VideoEncoder::Settings settings(
            webrtc::VideoEncoder::Capabilities(false), 1, 1200);
        const int result = encoder->InitEncode(&codec, settings);
        if (result != WEBRTC_VIDEO_CODEC_OK)
        {
            encoder->Release();
            return fail("Media Foundation H264 encoder initialization failed");
        }
        EncoderValidationCallback callback;
        if (encoder->RegisterEncodeCompleteCallback(&callback) !=
            WEBRTC_VIDEO_CODEC_OK)
        {
            encoder->Release();
            return fail("Media Foundation H264 callback registration failed");
        }
        auto i420 = webrtc::I420Buffer::Create(640, 360);
        if (!i420)
        {
            encoder->Release();
            return fail("Media Foundation H264 validation frame allocation failed");
        }
        webrtc::I420Buffer::SetBlack(i420.get());
        const webrtc::VideoFrame frame = webrtc::VideoFrame::Builder{}
            .set_video_frame_buffer(i420)
            .set_rtp_timestamp(90'000)
            .set_timestamp_us(1'000'000)
            .build();
        const std::vector<webrtc::VideoFrameType> frame_types = {
            webrtc::VideoFrameType::kVideoFrameKey};
        if (encoder->Encode(frame, &frame_types) != WEBRTC_VIDEO_CODEC_OK ||
            !callback.WaitForValidFrame())
        {
            encoder->Release();
            return fail("Media Foundation H264 encoder produced no valid keyframe");
        }
        encoder->Release();
    }

    const auto decoder_formats = factories.decoder->GetSupportedFormats();
    const bool has_vp8 = std::any_of(
        decoder_formats.begin(), decoder_formats.end(), IsVp8);
    if (!has_vp8)
        return fail("Windows codec factory lost the VP8 fallback decoder");
    const bool has_h264 = std::any_of(
        decoder_formats.begin(), decoder_formats.end(), IsH264);
    if (has_h264 != factories.hardware_decoder_available)
        return fail("Windows H264 decoder capability telemetry is inconsistent");
    if (has_h264 &&
        (decoder_formats.empty() || !IsH264(decoder_formats.front())))
        return fail("Windows codec factory does not prefer H264 decoding");
    if (!has_h264)
        return 1;
    const webrtc::SdpVideoFormat h264 = webrtc::SdpVideoFormat::H264();
    const auto support = factories.decoder->QueryCodecSupport(
        h264, false, std::nullopt);
    if (!support.is_supported || !support.is_power_efficient)
        return fail("Media Foundation H264 capability is not power efficient");
    const webrtc::Environment environment = webrtc::CreateEnvironment();
    if (!factories.decoder->Create(environment, h264))
        return fail("Media Foundation H264 decoder creation failed");
    return 1;
}

#endif
