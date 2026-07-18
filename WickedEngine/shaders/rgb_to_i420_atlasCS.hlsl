#include "globals.hlsli"
#include "ShaderInterop_I420.h"

PUSHCONSTANT(i420, I420AtlasPackPush);

Texture2D<float4> input0 : register(t0);
Texture2D<float4> input1 : register(t1);
Texture2D<float4> input2 : register(t2);
Texture2D<float4> input3 : register(t3);
ByteAddressBuffer metadata_luma : register(t4);
RWByteAddressBuffer output_i420 : register(u0);

float3 LoadTile(uint tile_index, uint2 coordinate)
{
    float3 rgb = 0;
    switch (tile_index)
    {
    case 0: rgb = input0.Load(int3(coordinate, 0)).rgb; break;
    case 1: rgb = input1.Load(int3(coordinate, 0)).rrr; break;
    case 2: rgb = input2.Load(int3(coordinate, 0)).rgb; break;
    case 3: rgb = input3.Load(int3(coordinate, 0)).rrr; break;
    }
    if (tile_index == 0 || tile_index == 2)
        rgb = saturate(log2(1.0 + max(0, rgb)) / log2(1.0 + i420.log_hdr_maximum));
    return rgb;
}

float3 LoadAtlasRGB(uint2 coordinate)
{
    [unroll]
    for (uint tile_index = 0; tile_index < 4; ++tile_index)
    {
        if ((i420.available_mask & (1u << tile_index)) == 0)
            continue;
        const uint4 rect = i420.tile_rects[tile_index];
        if (all(coordinate >= rect.xy) && all(coordinate < rect.xy + rect.zw))
            return LoadTile(tile_index, coordinate - rect.xy);
    }
    return 0;
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
            if (coordinate.y < i420.metadata_rows)
                y_values[lane] = LoadMetadataByte(coordinate.y * i420.video_resolution.x + coordinate.x);
            else
                y_values[lane] = RGBToY(LoadAtlasRGB(coordinate));
        }
        output_i420.Store(DTid.y * i420.y_stride + x_base, PackByte4(y_values));
        return;
    }

    const uint chroma_width = i420.video_resolution.x / 2u;
    const uint chroma_height = i420.video_resolution.y / 2u;
    if (DTid.y >= chroma_height || x_base >= i420.uv_stride)
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
    output_i420.Store(i420.u_offset + DTid.y * i420.uv_stride + x_base, PackByte4(u_values));
    output_i420.Store(i420.v_offset + DTid.y * i420.uv_stride + x_base, PackByte4(v_values));
}
