#include "globals.hlsli"
#include "ShaderInterop_I420.h"

PUSHCONSTANT(i420, I420AtlasPackPush);

Texture2D<float4> input_atlas : register(t0);
ByteAddressBuffer metadata_luma : register(t1);
RWByteAddressBuffer output_i420 : register(u0);

float3 LoadAtlasRGB(uint2 coordinate)
{
    // The canonical RGBA atlas is cleared outside its tiles and V3 tile
    // padding is explicitly edge-dilated while the atlas is assembled.
    // Sampling that canonical surface directly keeps the root-constant ABI
    // fixed and makes the preview and encoder consume identical pixels.
    return saturate(input_atlas.Load(int3(coordinate, 0)).rgb);
}

uint PackByte4(uint4 value)
{
    return value.x | (value.y << 8u) | (value.z << 16u) | (value.w << 24u);
}

uint RGBToY(float3 rgb)
{
    return (uint)round(saturate(dot(rgb, float3(66.0, 129.0, 25.0)) / 256.0 + 16.0 / 255.0) * 255.0);
}

uint2 RGBToUV(float3 rgb)
{
    const float u = dot(rgb, float3(-38.0, -74.0, 112.0)) / 256.0 + 0.5;
    const float v = dot(rgb, float3(112.0, -94.0, -18.0)) / 256.0 + 0.5;
    return (uint2)round(saturate(float2(u, v)) * 255.0);
}

uint LoadMetadataByte(uint index)
{
    const uint aligned_index = index & ~3u;
    const uint packed = metadata_luma.Load(aligned_index);
    return (packed >> ((index & 3u) * 8u)) & 0xffu;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if ((i420.abi_version != 1u && i420.abi_version != 2u) || i420.struct_size != 48u)
        return;

    const uint x_base = DTid.x * 4u;
    if (DTid.z == 0)
    {
        if (DTid.y >= i420.video_resolution.y || x_base >= i420.video_resolution.x)
            return;
        uint4 y_values = 16u;
        [unroll]
        for (uint lane = 0; lane < 4; ++lane)
        {
            const uint2 coordinate = uint2(x_base + lane, DTid.y);
            if (coordinate.x < i420.video_resolution.x &&
                coordinate.y < i420.metadata_rows)
                y_values[lane] = LoadMetadataByte(coordinate.y * i420.video_resolution.x + coordinate.x);
            else if (coordinate.x < i420.video_resolution.x)
                y_values[lane] = RGBToY(LoadAtlasRGB(coordinate));
        }
        output_i420.Store(DTid.y * i420.y_stride + x_base, PackByte4(y_values));
        return;
    }

    const uint chroma_width = i420.video_resolution.x / 2u;
    const uint chroma_height = i420.video_resolution.y / 2u;
    if (DTid.y >= chroma_height || x_base >= chroma_width)
        return;
    uint4 u_values = 128u;
    uint4 v_values = 128u;
    [unroll]
    for (uint lane = 0; lane < 4; ++lane)
    {
        const uint chroma_x = x_base + lane;
        if (chroma_x >= chroma_width)
            continue;
        const uint2 source = uint2(chroma_x * 2u, DTid.y * 2u);
        const float3 rgb = (LoadAtlasRGB(source) + LoadAtlasRGB(source + uint2(1, 0)) +
            LoadAtlasRGB(source + uint2(0, 1)) + LoadAtlasRGB(source + uint2(1, 1))) * 0.25;
        const uint2 uv = RGBToUV(rgb);
        u_values[lane] = uv.x;
        v_values[lane] = uv.y;
    }
    if (i420.abi_version == 1u)
    {
        output_i420.Store(i420.u_offset + DTid.y * i420.uv_stride + x_base, PackByte4(u_values));
        output_i420.Store(i420.v_offset + DTid.y * i420.uv_stride + x_base, PackByte4(v_values));
    }
    else
    {
        // NV12 stores four chroma samples as U0,V0,U1,V1,U2,V2,U3,V3.
        const uint first = u_values.x | (v_values.x << 8u) |
            (u_values.y << 16u) | (v_values.y << 24u);
        const uint second = u_values.z | (v_values.z << 8u) |
            (u_values.w << 16u) | (v_values.w << 24u);
        const uint destination = i420.u_offset + DTid.y * i420.uv_stride + x_base * 2u;
        output_i420.Store(destination, first);
        output_i420.Store(destination + 4u, second);
    }
}
