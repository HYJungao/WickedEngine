#define SURFACE_LOAD_QUAD_DERIVATIVES
#define SURFACE_LOAD_ENABLE_WIND
#define SVT_FEEDBACK
#define TEXTURE_SLOT_NONUNIFORM
#define SHADOW_MASK_ENABLED
#define PRIMITIVEID_FROM_MESHLET_OPTIMIZED
//#define DISABLE_VOXELGI
//#define DISABLE_ENVMAPS
//#define DISABLE_SOFT_SHADOWMAP
//#define DISABLE_TRANSPARENT_SHADOWMAP

#ifdef PLANARREFLECTION
#define DISABLE_ENVMAPS
#define DISABLE_VOXELGI
#endif // PLANARREFLECTION

#include "globals.hlsli"
#include "ShaderInterop_Renderer.h"
#include "raytracingHF.hlsli"
#include "brdf.hlsli"
#include "shadingHF.hlsli"

// This shader computes per-pixel lighting based on primitiveID

struct VisibilityPushConstants
{
	uint global_tile_offset;
	int specular_indirect_uav;
	int local_indirect_diffuse_uav;
	int local_indirect_padding0;
	int local_indirect_padding1;
	int local_indirect_padding2;
	int elastic_indirect_diffuse_uav;
	int elastic_ao_uav;
	int remote_indirect_diffuse_texture;
	int remote_ao_texture;
	float remote_indirect_diffuse_weight;
	float remote_ao_weight;
	float4 remote_clip_x;
	float4 remote_clip_y;
	float4 remote_clip_w;
};
PUSHCONSTANT(push, VisibilityPushConstants);

StructuredBuffer<VisibilityTile> binned_tiles : register(t0);
StructuredBuffer<PrimitiveVisibilityTile> primitive_binned_tiles : register(t1);

RWTexture2D<float4> output : register(u0);

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

	surface.update();

	if (!surface.IsGIApplied())
	{
		half3 ambient = GetAmbient(surface.N);
		surface.gi = lerp(ambient, ambient * surface.sss.rgb, saturate(surface.sss.a));
	}

	// Exact material-independent local diffuse GI consumed by Final before the
	// remote elastic blend. Dynamic and otherwise unbaked surfaces use the same
	// ambient/SSS fallback as the regular Final path.
	[branch]
	if (push.local_indirect_diffuse_uav >= 0)
	{
		RWTexture2D<float4> output_local_indirect_diffuse =
			bindless_rwtextures[descriptor_index(push.local_indirect_diffuse_uav)];
		output_local_indirect_diffuse[pixel] = float4(max(0, surface.gi * PI), 1);
	}

	// Remote DDGI and RTAO are generated in the Server frame's screen space.
	// Reproject this frame's world-space surface into that view instead of
	// sampling with the current screen UV. A source-depth companion is not part
	// of the V2 transport yet, so this validates projected viewport coverage but cannot
	// reject every old-view disocclusion.
	float2 remote_uv = 0;
	bool remote_reprojection_valid = false;
	[branch]
	if ((push.remote_indirect_diffuse_texture >= 0 && push.remote_indirect_diffuse_weight > 0) ||
		(push.remote_ao_texture >= 0 && push.remote_ao_weight > 0))
	{
		const float4 world_position = float4(surface.P, 1);
		const float remote_clip_w = dot(push.remote_clip_w, world_position);
		if (remote_clip_w > 0)
		{
			const float2 remote_ndc = float2(
				dot(push.remote_clip_x, world_position),
				dot(push.remote_clip_y, world_position)) / remote_clip_w;
			remote_uv = clipspace_to_uv(remote_ndc);
			remote_reprojection_valid = all(remote_uv >= 0) && all(remote_uv <= 1);
		}
	}

	half3 elastic_indirect_diffuse = surface.gi;
	[branch]
	if (remote_reprojection_valid && push.remote_indirect_diffuse_texture >= 0 &&
		push.remote_indirect_diffuse_weight > 0)
	{
		Texture2D<half4> remote_indirect_diffuse =
			bindless_textures_half4[descriptor_index(push.remote_indirect_diffuse_texture)];
		// RemoteIndirectDiffuseFormal is irradiance (includes PI), while Wicked's
		// internal diffuse GI term is stored after the Lambert PI divide.
		half3 remote_gi = remote_indirect_diffuse.SampleLevel(sampler_linear_clamp, remote_uv, 0).rgb / PI;
		elastic_indirect_diffuse = lerp(
			elastic_indirect_diffuse,
			remote_gi,
			saturate(push.remote_indirect_diffuse_weight));
	}

	Lighting lighting;
	lighting.create(0, 0, elastic_indirect_diffuse, 0);

	TiledLighting(surface, lighting, entity_flat_tile_index, camera);

	half elastic_screen_ao = 1;
#ifndef CARTOON
	[branch]
	if (camera.texture_ssr_index >= 0)
	{
		half4 ssr = bindless_textures_half4[descriptor_index(camera.texture_ssr_index)].SampleLevel(sampler_linear_clamp, surface.screenUV, 0);
		lighting.indirect.specular = lerp(lighting.indirect.specular, ssr.rgb * surface.F, ssr.a);
	}
	[branch]
	if (camera.texture_ssgi_index >= 0)
	{
		surface.ssgi = bindless_textures_half4[descriptor_index(camera.texture_ssgi_index)].SampleLevel(sampler_linear_clamp, surface.screenUV, 0).rgb;
	}
	[branch]
	if (camera.texture_ao_index >= 0)
	{
		elastic_screen_ao = bindless_textures_half4[descriptor_index(camera.texture_ao_index)].SampleLevel(
			sampler_linear_clamp, surface.screenUV, 0).r;
	}
	[branch]
	if (remote_reprojection_valid && push.remote_ao_texture >= 0 && push.remote_ao_weight > 0)
	{
		Texture2D<half4> remote_ao = bindless_textures_half4[descriptor_index(push.remote_ao_texture)];
		elastic_screen_ao = lerp(
			elastic_screen_ao,
			remote_ao.SampleLevel(sampler_linear_clamp, remote_uv, 0).r,
			saturate(push.remote_ao_weight));
	}
	surface.occlusion *= elastic_screen_ao;
#endif // CARTOON

	[branch]
	if (push.elastic_indirect_diffuse_uav >= 0)
	{
		RWTexture2D<float4> output_elastic_indirect_diffuse =
			bindless_rwtextures[descriptor_index(push.elastic_indirect_diffuse_uav)];
		output_elastic_indirect_diffuse[pixel] = float4(max(0, elastic_indirect_diffuse * PI), 1);
	}
	[branch]
	if (push.elastic_ao_uav >= 0)
	{
		RWTexture2D<float4> output_elastic_ao =
			bindless_rwtextures[descriptor_index(push.elastic_ao_uav)];
		output_elastic_ao[pixel] = float4(elastic_screen_ao.xxx, 1);
	}

	// Debug output is the exact environment/SSR/RT indirect-specular term that
	// ApplyLighting contributes to Final, including material Fresnel, roughness
	// and surface occlusion. Client disables SSR/RT, so this isolates its baked
	// local probe (plus the normal global fallback at probe-volume edges).
	[branch]
	if (push.specular_indirect_uav >= 0)
	{
		RWTexture2D<float4> output_specular_indirect =
			bindless_rwtextures[descriptor_index(push.specular_indirect_uav)];
		output_specular_indirect[pixel] = float4(max(0, lighting.indirect.specular * surface.occlusion), 1);
	}

	half4 color = 0;

	ApplyLighting(surface, lighting, color);

	half4 rimHighlight = surface.inst.GetRimHighlight();
	color.rgb += rimHighlight.rgb * pow(1 - surface.NdotV, rimHighlight.w);

#ifdef INTERIORMAPPING
	surface.baseColor.rgb += surface.emissiveColor;
	surface.baseColor *= InteriorMapping(surface.P, surface.N, surface.V, surface.material, surface.inst);
#endif // INTERIORMAPPING

#if defined(UNLIT) || defined(INTERIORMAPPING)
	color = surface.baseColor;
#endif // UNLIT

	ApplyFog(surface.hit_depth, surface.V, color);

	color = saturateMediump(color);

	output[pixel] = half4(color.rgb, 1);

}
