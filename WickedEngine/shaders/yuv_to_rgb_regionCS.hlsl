#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

PUSHCONSTANT(postprocess, PostProcess);

Texture2D<float> input_luminance : register(t0);
Texture2D<float2> input_chrominance : register(t1);
RWTexture2D<float4> output : register(u0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= postprocess.resolution))
        return;

    const uint2 source = uint2(postprocess.params0.xy) + DTid.xy;
    const float luminance = input_luminance.Load(int3(source, 0));
    const float2 chrominance = input_chrominance.Load(int3(source / 2, 0));

    const float C = luminance - 16.0 / 255.0;
    const float D = chrominance.x - 0.5;
    const float E = chrominance.y - 0.5;
    float3 rgb = saturate(float3(
        1.164383 * C + 1.596027 * E,
        1.164383 * C - 0.391762 * D - 0.812968 * E,
        1.164383 * C + 2.017232 * D));

    if (postprocess.params0.z > 0.5)
        rgb = rgb.xxx;
    if (postprocess.params0.w > 0.5)
        rgb = exp2(rgb * log2(1.0 + postprocess.params1.x)) - 1.0;

    output[DTid.xy] = float4(rgb, 1.0);
}
