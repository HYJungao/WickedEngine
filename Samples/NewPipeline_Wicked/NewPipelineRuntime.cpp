#include "NewPipelineRuntime.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace wicked_newpipeline
{
namespace
{
std::string NormalizeToken(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsNameMatch(const std::string& arg, const std::string& name)
{
    return arg == name || arg == ("--" + name);
}

std::string GetInlineValue(const std::string& arg, const std::string& name)
{
    const std::string plain_prefix = name + "=";
    const std::string dash_prefix  = "--" + name + "=";
    if (arg.rfind(plain_prefix, 0) == 0)
        return arg.substr(plain_prefix.size());
    if (arg.rfind(dash_prefix, 0) == 0)
        return arg.substr(dash_prefix.size());
    return {};
}

std::vector<std::string> CollectArguments(int argc, char* argv[])
{
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0u);
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] != nullptr)
            args.emplace_back(argv[i]);
    }
    return args;
}

std::string GetArgumentValue(const std::vector<std::string>& args, const std::string& name)
{
    for (size_t i = 0; i < args.size(); ++i)
    {
        std::string value = GetInlineValue(args[i], name);
        if (!value.empty())
            return value;

        if (IsNameMatch(args[i], name) && i + 1 < args.size())
            return args[i + 1];
    }
    return {};
}

bool HasArgument(const std::vector<std::string>& args, const std::string& name)
{
    return std::any_of(args.begin(), args.end(), [&name](const std::string& arg) {
        return IsNameMatch(arg, name);
    });
}
} // namespace

const char* ToString(RemoteSourceMode mode)
{
    switch (mode)
    {
    case RemoteSourceMode::Mock:
        return "mock";
    case RemoteSourceMode::WebRTC:
        return "webrtc";
    default:
        return "unknown";
    }
}

const char* ToString(RemoteBufferSemantic semantic)
{
    switch (semantic)
    {
    case RemoteBufferSemantic::RemoteIndirectDiffuse:
        return "RemoteIndirectDiffuse";
    case RemoteBufferSemantic::RemoteAO:
        return "RemoteAO";
    case RemoteBufferSemantic::RemoteSpecularIndirect:
        return "RemoteSpecularIndirect";
    case RemoteBufferSemantic::RemoteShadowVisibility:
        return "RemoteShadowVisibility";
    case RemoteBufferSemantic::Count:
    default:
        return "Unknown";
    }
}

const char* ToString(RemoteDynamicRange range)
{
    switch (range)
    {
    case RemoteDynamicRange::LDR:
        return "LDR";
    case RemoteDynamicRange::HDR:
        return "HDR";
    case RemoteDynamicRange::Unknown:
    default:
        return "unknown";
    }
}

const char* ToString(RemoteDebugMode mode)
{
    switch (mode)
    {
    case RemoteDebugMode::Local:
        return "local";
    case RemoteDebugMode::Raw:
        return "raw";
    case RemoteDebugMode::DebugComposite:
        return "debug_composite";
    default:
        return "unknown";
    }
}

const char* ToString(DebugPreviewMode mode)
{
    switch (mode)
    {
    case DebugPreviewMode::Final: return "final";
    case DebugPreviewMode::GBufferDepth: return "gbuffer_depth";
    case DebugPreviewMode::GBufferNormalRoughness: return "gbuffer_normal_roughness";
    case DebugPreviewMode::GBufferNormalXY: return "gbuffer_normal_xy";
    case DebugPreviewMode::GBufferRoughness: return "gbuffer_roughness";
    case DebugPreviewMode::LocalIndirectDiffuse: return "local_indirect_diffuse";
    case DebugPreviewMode::LocalAO: return "local_ao";
    case DebugPreviewMode::LocalSpecularIndirect: return "local_specular_indirect";
    case DebugPreviewMode::LocalShadowVisibility: return "local_shadow_visibility";
    case DebugPreviewMode::RemoteIndirectDiffuse: return "remote_indirect_diffuse";
    case DebugPreviewMode::RemoteAO: return "remote_ao";
    case DebugPreviewMode::RemoteSpecularIndirect: return "remote_specular_indirect";
    case DebugPreviewMode::RemoteShadowVisibility: return "remote_shadow_visibility";
    case DebugPreviewMode::ElasticIndirectDiffuse: return "elastic_indirect_diffuse";
    case DebugPreviewMode::ElasticAO: return "elastic_ao";
    case DebugPreviewMode::RemoteOverview: return "remote_overview";
    case DebugPreviewMode::TransportIndirectDiffuse: return "transport_indirect_diffuse";
    case DebugPreviewMode::TransportAO: return "transport_ao";
    case DebugPreviewMode::TransportSpecularIndirect: return "transport_specular_indirect";
    case DebugPreviewMode::TransportShadowVisibility: return "transport_shadow_visibility";
    case DebugPreviewMode::LocalReflectionProbe: return "local_reflection_probe";
    case DebugPreviewMode::LocalIndirectFinalInput: return "local_indirect_final_input";
    default: return "unknown";
    }
}

const char* ToString(RemoteBufferEncoding encoding)
{
    switch (encoding)
    {
    case RemoteBufferEncoding::LinearRGBA8: return "linear_rgba8";
    case RemoteBufferEncoding::LogHDR16F: return "log_hdr16f";
    case RemoteBufferEncoding::ScalarLuma8: return "scalar_luma8";
    default: return "unknown";
    }
}

const char* ToString(DDGIResetReason reason)
{
    switch (reason)
    {
    case DDGIResetReason::None: return "none";
    case DDGIResetReason::InitialScene: return "initial_scene";
    case DDGIResetReason::SceneGeneration: return "scene_generation";
    case DDGIResetReason::LightingChanged: return "lighting_changed";
    case DDGIResetReason::GridChanged: return "grid_changed";
    default: return "unknown";
    }
}

RemoteSourceMode ParseRemoteSourceMode(const std::string& value, RemoteSourceMode fallback)
{
    const std::string token = NormalizeToken(value);
    if (token == "mock")
        return RemoteSourceMode::Mock;
    if (token == "webrtc")
        return RemoteSourceMode::WebRTC;
    return fallback;
}

RemoteDebugMode ParseRemoteDebugMode(const std::string& value, RemoteDebugMode fallback)
{
    const std::string token = NormalizeToken(value);
    if (token == "local")
        return RemoteDebugMode::Local;
    if (token == "raw")
        return RemoteDebugMode::Raw;
    if (token == "debug_composite" || token == "debug-composite" || token == "composite")
        return RemoteDebugMode::DebugComposite;
    return fallback;
}

RuntimeConfig ParseRuntimeConfig(int argc, char* argv[], RuntimeConfig fallback)
{
    RuntimeConfig config = fallback;
    config.parse_warnings.clear();
    const std::vector<std::string> args = CollectArguments(argc, argv);

    const std::string remote_source_arg = GetArgumentValue(args, "remote_source");
    if (!remote_source_arg.empty())
    {
        const RemoteSourceMode parsed = ParseRemoteSourceMode(remote_source_arg, config.remote_source);
        if (NormalizeToken(remote_source_arg) != ToString(parsed))
        {
            config.parse_warnings.push_back(
                "Unknown --remote_source value '" + remote_source_arg + "', using " + ToString(config.remote_source));
        }
        else
        {
            config.remote_source = parsed;
        }
    }

    const std::string remote_debug_arg = GetArgumentValue(args, "remote_debug");
    if (!remote_debug_arg.empty())
    {
        const std::string token = NormalizeToken(remote_debug_arg);
        const RemoteDebugMode parsed = ParseRemoteDebugMode(remote_debug_arg, config.remote_debug_mode);
        if (token == "composite")
        {
            config.parse_warnings.push_back(
                "--remote_debug composite is a compatibility alias for debug_composite; it is not final GI composite.");
            config.remote_debug_mode = parsed;
        }
        else if (token != ToString(parsed) && token != "debug-composite")
        {
            config.parse_warnings.push_back(
                "Unknown --remote_debug value '" + remote_debug_arg + "', using " + ToString(config.remote_debug_mode));
        }
        else
        {
            config.remote_debug_mode = parsed;
        }
    }

    if (HasArgument(args, "webrtc"))
        config.remote_source = RemoteSourceMode::WebRTC;
    if (HasArgument(args, "no_webrtc"))
        config.remote_source = RemoteSourceMode::Mock;

    const std::string signaling_url = GetArgumentValue(args, "webrtc_signal");
    if (!signaling_url.empty())
        config.signaling_url = signaling_url;

    const std::string room_id = GetArgumentValue(args, "webrtc_room");
    if (!room_id.empty())
        config.room_id = room_id;

    config.use_internet_ice = HasArgument(args, "webrtc_internet");

    return config;
}
} // namespace wicked_newpipeline
