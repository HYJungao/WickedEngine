#define SURFACE_LOAD_QUAD_DERIVATIVES
#define SURFACE_LOAD_ENABLE_WIND
#define SVT_FEEDBACK
#define TEXTURE_SLOT_NONUNIFORM
#define PRIMITIVEID_FROM_MESHLET_OPTIMIZED
#include "globals.hlsli"
#include "ShaderInterop_Renderer.h"
#include "raytracingHF.hlsli"
#include "surfaceHF.hlsli"
#include "shadingHF.hlsli"

// This shader extracts per-pixel surface attributes normal and roughness based on primitiveID

struct VisibilityPushConstants
{
	uint global_tile_offset;
	int lightmap_irradiance_uav;
	int lightmap_validity_uav;
	int lightmap_coverage_uav;
	int lightmap_raw_uav;
	int local_indirect_diffuse_uav;
};
PUSHCONSTANT(push, VisibilityPushConstants);

StructuredBuffer<VisibilityTile> binned_tiles : register(t0);

RWTexture2D<half3> output_normals_roughness : register(u0);

[numthreads(VISIBILITY_BLOCKSIZE * VISIBILITY_BLOCKSIZE, 1, 1)]
void main(uint Gid : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
	const uint tile_offset = push.global_tile_offset + Gid.x;
	VisibilityTile tile = binned_tiles[tile_offset];
	const uint2 GTid = remap_lane_quads(groupIndex);
	const uint2 tileID = unpack_pixel(tile.visibility_tile_id);
	const uint2 pixel = tileID * VISIBILITY_BLOCKSIZE + GTid;

	ShaderCamera camera = GetCamera();
	const uint entity_flat_tile_index = flatten2D(tileID / VISIBILITY_TILED_CULLING_GRANULARITY, camera.entity_culling_tilecount.xy) * SHADER_ENTITY_TILE_BUCKET_COUNT;

	const float2 uv = ((float2)pixel + 0.5) * camera.internal_resolution_rcp;
	RayDesc ray = CreateCameraRay(pixel);
	float3 rayDirection_quad_x = QuadReadAcrossX(ray.Direction);
	float3 rayDirection_quad_y = QuadReadAcrossY(ray.Direction);

#ifdef PRIMITIVEID_UNIFORM
	const uint primitiveID = tile.shaderType_or_primitiveID;
#else
	const uint primitiveID = texture_primitiveID[pixel];
	[branch] if (primitiveID == 0) return;
#endif // PRIMITIVEID_UNIFORM

	PrimitiveID prim;
	prim.init();
	prim.unpack(primitiveID);

#ifndef PRIMITIVEID_UNIFORM
	[branch] if (prim.shaderType != tile.shaderType_or_primitiveID) return;
#endif // PRIMITIVEID_UNIFORM

	Surface surface;
	surface.init();
	surface.pixel = pixel.xy;
	surface.screenUV = uv;

	[branch]
	if (!surface.load(prim, ray.Origin, ray.Direction, rayDirection_quad_x, rayDirection_quad_y, entity_flat_tile_index))
	{
		return;
	}

	// Surface::load() marks GI as applied only when this primitive has a valid
	// object lightmap and atlas. Wicked stores the diffuse lightmap term with
	// the Lambert PI divide included, so multiply by PI for a material-independent
	// irradiance buffer matching RemoteIndirectDiffuseFormal semantics.
	[branch]
	if (push.lightmap_irradiance_uav >= 0)
	{
		RWTexture2D<float4> output_lightmap_irradiance =
			bindless_rwtextures[descriptor_index(push.lightmap_irradiance_uav)];
		output_lightmap_irradiance[pixel] = surface.IsGIApplied()
			? float4(surface.gi * PI, 1)
			: 0;
	}

	// Primitive validity is established before this shader is dispatched, so a
	// written pixel always contains geometry. Do not classify from brightness:
	// a valid lightmap texel is allowed to be physically black.
	[branch]
	if (push.lightmap_validity_uav >= 0)
	{
		RWTexture2D<float4> output_lightmap_validity =
			bindless_rwtextures[descriptor_index(push.lightmap_validity_uav)];
		output_lightmap_validity[pixel] = surface.IsLightmapAvailable()
			? float4(0, 1, 0, 1)
			: float4(1, 0, 1, 1);
	}

	[branch]
	if (push.lightmap_coverage_uav >= 0)
	{
		RWTexture2D<float4> output_lightmap_coverage =
			bindless_rwtextures[descriptor_index(push.lightmap_coverage_uav)];
		output_lightmap_coverage[pixel] = !surface.IsLightmapAvailable()
			? float4(1, 0, 1, 1)
			: (surface.IsLightmapCovered() ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1));
	}

	[branch]
	if (push.lightmap_raw_uav >= 0)
	{
		RWTexture2D<float4> output_lightmap_raw =
			bindless_rwtextures[descriptor_index(push.lightmap_raw_uav)];
		output_lightmap_raw[pixel] = surface.IsGIApplied() ? float4(max(0, surface.gi), 1) : 0;
	}

	// This diagnostic must also be available when the main scene uses raster
	// shading. Visibility_Shade() is not dispatched in that configuration, so
	// produce the same local pre-elastic GI value from the already reconstructed
	// surface here instead of leaving the preview texture permanently black.
	[branch]
	if (push.local_indirect_diffuse_uav >= 0)
	{
		half3 local_gi = surface.gi;
		if (!surface.IsGIApplied())
		{
			half3 ambient = GetAmbient(surface.N);
			local_gi = lerp(ambient, ambient * surface.sss.rgb, saturate(surface.sss.a));
		}
		RWTexture2D<float4> output_local_indirect_diffuse =
			bindless_rwtextures[descriptor_index(push.local_indirect_diffuse_uav)];
		output_local_indirect_diffuse[pixel] = float4(max(0, local_gi * PI), 1);
	}

	// Write out sampleable attributes for post processing into textures:
#ifdef CLEARCOAT
	// Clearcoat must write out the top layer's normal and roughness to match the specs:
	output_normals_roughness[pixel] = half3(encode_normal(surface.clearcoat.N), surface.clearcoat.roughness);
#else
	output_normals_roughness[pixel] = half3(encode_normal(surface.N), surface.roughness);
#endif // CLEARCOAT

}
