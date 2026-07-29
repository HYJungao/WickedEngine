#include "NewPipelineRuntime.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

namespace wicked_newpipeline
{
namespace
{
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

float SafeBlendWeight(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

float Blend(float local, float remote, float weight)
{
    const float w = SafeBlendWeight(weight);
    const float safe_local = std::isfinite(local) ? local : 0.0f;
    if (w <= 0.0f || !std::isfinite(remote))
        return safe_local;
    if (w >= 1.0f)
        return remote;
    return safe_local + (remote - safe_local) * w;
}

float BlendVisibility(float local, float remote, float weight)
{
    const float w = SafeBlendWeight(weight);
    const float safe_local = std::isfinite(local) ? std::clamp(local, 0.0f, 1.0f) : 1.0f;
    if (w <= 0.0f || !std::isfinite(remote))
        return safe_local;
    const float safe_remote = std::clamp(remote, 0.0f, 1.0f);
    if (w >= 1.0f)
        return safe_remote;
    return safe_local + (safe_remote - safe_local) * w;
}

XMFLOAT3 Blend(const XMFLOAT3& local, const XMFLOAT3& remote, float weight)
{
    return XMFLOAT3(
        Blend(local.x, remote.x, weight),
        Blend(local.y, remote.y, weight),
        Blend(local.z, remote.z, weight));
}

bool NearlyEqual(float a, float b, float epsilon = 1e-5f)
{
    return std::abs(a - b) <= epsilon * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
}
} // namespace

FormalLightingBlendV3Result EvaluateFormalLightingBlendV3(const FormalLightingBlendV3& input)
{
    FormalLightingBlendV3Result result;
    result.diffuse = Blend(input.diffuse_local, input.diffuse_remote, input.diffuse_weight);
    result.specular_pre_ao = Blend(
        input.specular_pre_ao_local, input.specular_pre_ao_remote, input.specular_weight);
    result.ambient_visibility = BlendVisibility(
        input.ambient_visibility_local, input.ambient_visibility_remote, input.ao_weight);
    result.primary_visibility = BlendVisibility(
        input.primary_visibility_local,
        input.primary_visibility_remote,
        input.primary_visibility_weight);
    return result;
}

bool ValidateFormalLightingBlendV3Reference(std::string* error)
{
    FormalLightingBlendV3 input;
    input.diffuse_local = XMFLOAT3(0.0f, 1.0f, 16.0f);
    input.diffuse_remote = XMFLOAT3(64.0f, 4.0f, 0.0f); // HDR values must remain unclamped.
    input.specular_pre_ao_local = XMFLOAT3(2.0f, 8.0f, 32.0f);
    input.specular_pre_ao_remote = XMFLOAT3(10.0f, 4.0f, 0.5f);
    input.ambient_visibility_local = 0.25f;
    input.ambient_visibility_remote = 0.75f;
    input.primary_visibility_local = 0.0f;
    input.primary_visibility_remote = 1.0f;

    FormalLightingBlendV3Result result = EvaluateFormalLightingBlendV3(input);
    if (!NearlyEqual(result.diffuse.z, 16.0f) || !NearlyEqual(result.specular_pre_ao.z, 32.0f) ||
        !NearlyEqual(result.ambient_visibility, 0.25f) || !NearlyEqual(result.primary_visibility, 0.0f))
    {
        if (error != nullptr) *error = "V3 zero-weight endpoint changed the local formal result";
        return false;
    }

    input.diffuse_weight = input.ao_weight = input.specular_weight = input.primary_visibility_weight = 1.0f;
    result = EvaluateFormalLightingBlendV3(input);
    if (!NearlyEqual(result.diffuse.x, 64.0f) || !NearlyEqual(result.specular_pre_ao.z, 0.5f) ||
        !NearlyEqual(result.ambient_visibility, 0.75f) || !NearlyEqual(result.primary_visibility, 1.0f))
    {
        if (error != nullptr) *error = "V3 one-weight endpoint changed the remote formal result";
        return false;
    }

    input.diffuse_weight = input.ao_weight = input.specular_weight = input.primary_visibility_weight = 0.5f;
    result = EvaluateFormalLightingBlendV3(input);
    if (!NearlyEqual(result.diffuse.x, 32.0f) || !NearlyEqual(result.diffuse.y, 2.5f) ||
        !NearlyEqual(result.specular_pre_ao.x, 6.0f) || !NearlyEqual(result.ambient_visibility, 0.5f) ||
        !NearlyEqual(result.primary_visibility, 0.5f))
    {
        if (error != nullptr) *error = "V3 linear-scene reference ramp mismatch";
        return false;
    }

    input.diffuse_weight = std::numeric_limits<float>::quiet_NaN();
    input.ao_weight = std::numeric_limits<float>::infinity();
    input.primary_visibility_weight = std::numeric_limits<float>::quiet_NaN();
    input.diffuse_remote.x = std::numeric_limits<float>::infinity();
    input.specular_pre_ao_remote.y = std::numeric_limits<float>::quiet_NaN();
    input.specular_weight = 0.5f;
    result = EvaluateFormalLightingBlendV3(input);
    if (!NearlyEqual(result.diffuse.z, input.diffuse_local.z) ||
        !NearlyEqual(result.diffuse.x, input.diffuse_local.x) ||
        !NearlyEqual(result.specular_pre_ao.y, input.specular_pre_ao_local.y) ||
        !NearlyEqual(result.ambient_visibility, input.ambient_visibility_local) ||
        !NearlyEqual(result.primary_visibility, input.primary_visibility_local))
    {
        if (error != nullptr) *error = "V3 non-finite weights did not fail closed to local";
        return false;
    }

    // Scalar visibility is a strict [0,1] contract even if a malformed local
    // producer or future decoder hands the CPU reference out-of-range values.
    input.ambient_visibility_local = -2.0f;
    input.ambient_visibility_remote = 3.0f;
    input.ao_weight = 0.5f;
    result = EvaluateFormalLightingBlendV3(input);
    if (!NearlyEqual(result.ambient_visibility, 0.5f))
    {
        if (error != nullptr) *error = "V3 scalar visibility escaped its normalized range";
        return false;
    }

    // A hard confidence edge must preserve both sides exactly; the reference
    // blend must not introduce a halo by implicitly smoothing the weight.
    constexpr float hard_edge_weights[] = { 0.0f, 0.0f, 1.0f, 1.0f };
    constexpr float hard_edge_expected[] = { 2.0f, 2.0f, 10.0f, 10.0f };
    for (size_t i = 0; i < std::size(hard_edge_weights); ++i)
    {
        FormalLightingBlendV3 edge;
        edge.diffuse_local = XMFLOAT3(2.0f, 2.0f, 2.0f);
        edge.diffuse_remote = XMFLOAT3(10.0f, 10.0f, 10.0f);
        edge.diffuse_weight = hard_edge_weights[i];
        const FormalLightingBlendV3Result edge_result = EvaluateFormalLightingBlendV3(edge);
        if (!NearlyEqual(edge_result.diffuse.x, hard_edge_expected[i]))
        {
            if (error != nullptr) *error = "V3 hard-edge reference vector was filtered";
            return false;
        }
    }
    return true;
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
    case DebugPreviewMode::LocalSpecularIndirectPreAO: return "local_specular_indirect_pre_ao";
    case DebugPreviewMode::LocalShadowVisibility: return "local_shadow_visibility";
    case DebugPreviewMode::RemoteIndirectDiffuse: return "remote_indirect_diffuse";
    case DebugPreviewMode::RemoteAO: return "remote_ao";
    case DebugPreviewMode::RemoteSpecularIndirect: return "remote_specular_indirect";
    case DebugPreviewMode::RemoteShadowVisibility: return "remote_shadow_visibility";
    case DebugPreviewMode::ElasticIndirectDiffuse: return "elastic_indirect_diffuse";
    case DebugPreviewMode::ElasticAO: return "elastic_ao";
    case DebugPreviewMode::ElasticSpecularIndirectPreAO:
        return "elastic_specular_indirect_pre_ao";
    case DebugPreviewMode::ElasticPrimaryLightVisibility:
        return "elastic_primary_light_visibility";
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

const char* ToString(RemoteQualityTierV3 tier)
{
    switch (tier)
    {
    case RemoteQualityTierV3::High: return "high";
    case RemoteQualityTierV3::Balanced: return "balanced";
    case RemoteQualityTierV3::Low: return "low";
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

RuntimeConfig ParseRuntimeConfig(int argc, char* argv[], RuntimeConfig fallback)
{
    RuntimeConfig config = fallback;
    config.parse_warnings.clear();
    const std::vector<std::string> args = CollectArguments(argc, argv);

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
