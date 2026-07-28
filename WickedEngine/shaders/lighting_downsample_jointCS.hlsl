#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

PUSHCONSTANT(postprocess, PostProcess);

Texture2D<float4> input_lighting : register(t0);
Texture2D<float> input_depth : register(t1);
Texture2D<half4> input_normal_roughness : register(t2);
RWTexture2D<float4> output_lighting : register(u0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= postprocess.resolution))
        return;

    const uint2 input_size = uint2(postprocess.params0.xy);
    const float2 scale = postprocess.params1.xy;
    const uint mode = (uint)postprocess.params0.z;
    const bool encode_hdr = postprocess.params0.w > 0.5;
    const float2 footprint_min = float2(DTid.xy) * scale;
    const uint2 center_coord = min(
        uint2(footprint_min + scale * 0.5),
        input_size - 1);
    const float center_depth = compute_lineardepth(input_depth[center_coord]);
    const half4 center_nr = input_normal_roughness[center_coord];
    const float3 center_normal = decode_normal(center_nr.rg);
    const float center_roughness = center_nr.b;

    float4 accumulated = 0;
    float weight_sum = 0;
    // Seed the conservative minimum with the footprint's reference surface.
    // This avoids a white bias if all stratified taps are rejected at a very
    // thin or highly discontinuous footprint.
    float minimum_visibility = input_lighting[center_coord].r;
    [unroll]
    for (uint y = 0; y < 4; ++y)
    {
        [unroll]
        for (uint x = 0; x < 4; ++x)
        {
            const float2 sample_position = footprint_min +
                (float2(x, y) + 0.5) * scale * 0.25;
            const uint2 coord = min(uint2(sample_position), input_size - 1);
            const float sample_depth = compute_lineardepth(input_depth[coord]);
            const half4 sample_nr = input_normal_roughness[coord];
            const float3 sample_normal = decode_normal(sample_nr.rg);
            const float relative_depth_delta =
                abs(sample_depth - center_depth) / max(0.01, center_depth);
            const float depth_weight = exp2(-64.0 * relative_depth_delta);
            const float normal_weight = pow(saturate(dot(center_normal, sample_normal)), 16.0);
            const float roughness_weight = mode == 0
                ? exp2(-8.0 * abs(sample_nr.b - center_roughness))
                : 1.0;
            const bool compatible_surface =
                relative_depth_delta <= 0.05 &&
                dot(center_normal, sample_normal) >= 0.5;
            const float weight = compatible_surface
                ? depth_weight * normal_weight * roughness_weight
                : 0.0;
            const float4 value = input_lighting[coord];
            accumulated += value * weight;
            weight_sum += weight;
            // A conservative visibility minimum is useful for retaining thin
            // occluders, but it must never bypass the geometry-aware filter.
            // Otherwise an unrelated foreground/background surface can grow a
            // black AO or shadow halo across a depth discontinuity.
            const bool same_surface =
                relative_depth_delta <= 0.02 &&
                dot(center_normal, sample_normal) >= 0.8;
            if (same_surface)
                minimum_visibility = min(minimum_visibility, value.r);
        }
    }

    float4 result = weight_sum > 1e-4
        ? accumulated / weight_sum
        : input_lighting[center_coord];
    if (mode == 1)
        result = lerp(result, minimum_visibility.xxxx, 0.25);
    else if (mode == 2)
        result = lerp(result, minimum_visibility.xxxx, 0.5);
    if (mode != 0)
        result = float4(result.rrr, 1);
    else if (encode_hdr)
        result = float4(log2(1 + clamp(result.rgb, 0, 16)) / log2(17.0), 1);
    output_lighting[DTid.xy] = saturate(result);
}
