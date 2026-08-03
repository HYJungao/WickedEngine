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
	int specular_indirect_pre_ao_uav;
	int primary_light_visibility_uav;
	int primary_light_shadow_index;
	int local_indirect_diffuse_uav;
	int elastic_indirect_diffuse_uav;
	int elastic_ao_uav;
	int point_light_diagnostic;
};
PUSHCONSTANT(push, VisibilityPushConstants);

StructuredBuffer<VisibilityTile> binned_tiles : register(t0);
StructuredBuffer<PrimitiveVisibilityTile> primitive_binned_tiles : register(t1);

RWTexture2D<float4> output : register(u0);

inline bool external_project_surface(
	in Surface surface,
	out float2 remote_uv,
	out float expected_linear_depth)
{
	remote_uv = 0;
	expected_linear_depth = 0;
	if (external_history_depth_texture < 0 ||
		external_history_normal_roughness_texture < 0)
		return false;

	const float4 remote_clip = mul(external_view_projection, float4(surface.P, 1));
	if (remote_clip.w <= 0)
		return false;
	const float3 remote_ndc = remote_clip.xyz / remote_clip.w;
	remote_uv = clipspace_to_uv(remote_ndc.xy);
	if (any(remote_uv < 0) || any(remote_uv > 1))
		return false;
	expected_linear_depth = compute_lineardepth(
		remote_ndc.z,
		external_reprojection_params.x,
		external_reprojection_params.y);
	return isfinite(expected_linear_depth) && expected_linear_depth > 0;
}

inline half4 sample_external_joint(
	int texture_index,
	float2 remote_uv,
	float expected_linear_depth,
	half3 current_normal,
	half current_roughness,
	float3 current_position,
	out bool valid)
{
	valid = false;
	if (texture_index < 0)
		return 0;
	if (texture_index == external_specular_texture)
	{
		const float3 current_view_delta =
			GetCamera().position - current_position;
		const float3 remote_view_delta =
			external_remote_view_origin.xyz - current_position;
		const float current_view_length_sq =
			dot(current_view_delta, current_view_delta);
		const float remote_view_length_sq =
			dot(remote_view_delta, remote_view_delta);
		if (current_view_length_sq < 1e-8 ||
			remote_view_length_sq < 1e-8)
			return 0;
		const float3 current_view = current_view_delta *
			rsqrt(current_view_length_sq);
		const float3 remote_view = remote_view_delta *
			rsqrt(remote_view_length_sq);
		if (dot(current_view, remote_view) <
			external_remote_view_origin.w)
			return 0;
	}

	Texture2D<half4> remote_texture =
		bindless_textures_half4[descriptor_index(texture_index)];
	Texture2D<float> history_depth =
		bindless_textures_float[descriptor_index(external_history_depth_texture)];
	Texture2D<half4> history_normal_roughness =
		bindless_textures_half4[descriptor_index(external_history_normal_roughness_texture)];

	uint remote_width = 0;
	uint remote_height = 0;
	remote_texture.GetDimensions(remote_width, remote_height);
	if (remote_width == 0 || remote_height == 0)
		return 0;

	const float2 remote_size = float2(remote_width, remote_height);
	const float2 texel_position = remote_uv * remote_size - 0.5;
	const int2 base_coord = int2(floor(texel_position));
	const float2 fraction = frac(texel_position);
	half4 accumulated = 0;
	float weight_sum = 0;

	[unroll]
	for (int y = 0; y < 2; ++y)
	{
		[unroll]
		for (int x = 0; x < 2; ++x)
		{
			const int2 coord = clamp(
				base_coord + int2(x, y),
				int2(0, 0),
				int2(remote_width - 1, remote_height - 1));
			const float2 sample_uv = (float2(coord) + 0.5) / remote_size;
			const float stored_linear_depth = compute_lineardepth(
				history_depth.SampleLevel(sampler_point_clamp, sample_uv, 0),
				external_reprojection_params.x,
				external_reprojection_params.y);
			const half4 stored_normal_roughness =
				history_normal_roughness.SampleLevel(
					sampler_point_clamp, sample_uv, 0);
			const half3 stored_normal =
				decode_normal(stored_normal_roughness.rg);
			const float relative_depth_delta =
				abs(stored_linear_depth - expected_linear_depth) /
				max(0.01, expected_linear_depth);
			const float normal_dot = dot(current_normal, stored_normal);
			const bool view_sensitive =
				texture_index == external_specular_texture ||
				texture_index == external_primary_visibility_texture;
			const bool specular_sensitive =
				texture_index == external_specular_texture;
			const float roughness_delta =
				abs(stored_normal_roughness.b -
					current_roughness);
			const float depth_threshold = view_sensitive
				? external_reprojection_params.z * 0.5
				: external_reprojection_params.z;
			const float normal_threshold = view_sensitive
				? max(0.9, external_reprojection_params.w)
				: external_reprojection_params.w;
			if (relative_depth_delta > depth_threshold ||
				normal_dot < normal_threshold ||
				(specular_sensitive &&
					roughness_delta > 0.25))
				continue;

			const float2 bilinear_axis = float2(
				x == 0 ? 1 - fraction.x : fraction.x,
				y == 0 ? 1 - fraction.y : fraction.y);
			const float spatial_weight = bilinear_axis.x * bilinear_axis.y;
			const float geometry_weight =
				exp2(-64.0 * relative_depth_delta) *
				pow(saturate(normal_dot), 16.0) *
				(specular_sensitive
					? exp2(-8.0 * roughness_delta)
					: 1.0);
			const float weight = spatial_weight * geometry_weight;
			accumulated += remote_texture.Load(int3(coord, 0)) * weight;
			weight_sum += weight;
		}
	}

	valid = weight_sum > 1e-4;
	return valid ? accumulated / weight_sum : 0;
}

bool sample_client_volumetric_lightmap(
	uint instance_index,
	half3 normal,
	out half3 diffuse_gi)
{
	diffuse_gi = 0;
	if (external_client_vlm_buffer < 0)
		return false;

	Buffer<float4> coefficients =
		bindless_buffers_float4[
			descriptor_index(external_client_vlm_buffer)];
	const uint base = instance_index * 7;
	const float4 packed0 = coefficients[base + 0];
	const float4 packed1 = coefficients[base + 1];
	const float4 packed2 = coefficients[base + 2];
	const float4 packed3 = coefficients[base + 3];
	const float4 packed4 = coefficients[base + 4];
	const float4 packed5 = coefficients[base + 5];
	const float4 packed6 = coefficients[base + 6];
	if (packed6.w < 0.5)
		return false;

	SH::L2_RGB radiance;
	radiance.C[0] = packed0.xyz;
	radiance.C[1] = float3(packed0.w, packed1.xy);
	radiance.C[2] = float3(packed1.zw, packed2.x);
	radiance.C[3] = packed2.yzw;
	radiance.C[4] = packed3.xyz;
	radiance.C[5] = float3(packed3.w, packed4.xy);
	radiance.C[6] = float3(packed4.zw, packed5.x);
	radiance.C[7] = packed5.yzw;
	radiance.C[8] = packed6.xyz;
	diffuse_gi = max(
		0,
		SH::CalculateIrradiance(radiance, normal) / PI);
	return all(isfinite(diffuse_gi));
}

half3 point_shadow_bias_compare(
	in ShaderEntity light,
	in float3 light_vector,
	min16uint2 pixel,
	half receiverNoL)
{
	const float3 uv_slice = cubemap_to_uv(-light_vector);
	const float2 face_uv = uv_slice.xy;
	const float2 face_resolution = max(
		float2(1.0, 1.0),
		light.shadowAtlasMulAdd.xy * GetFrame().shadow_atlas_resolution);
	const float2 edge_distance_texels = min(face_uv, 1.0 - face_uv) * face_resolution;
	const float nearest_edge_texels = min(
		edge_distance_texels.x,
		edge_distance_texels.y);
	const half face_edge = 1.0 - saturate(nearest_edge_texels / 2.0);

	// R is the exact production resolution/slope-aware receiver bias. G repeats
	// the same lookup without any receiver bias.
	// This helper is diagnostic-only; it does not alter shadow_cube().
	const half3 biased_shadow = shadow_cube(
		light,
		light_vector,
		pixel,
		receiverNoL);
	const float major_axis_distance = max(
		max(abs(light_vector.x), abs(light_vector.y)),
		abs(light_vector.z));
	const float unbiased_depth =
		light.GetCubemapDepthRemapNear() +
		light.GetCubemapDepthRemapFar() / max(major_axis_distance, 1e-6);

	float2 shadow_uv = face_uv;
#ifdef SHADOW_SAMPLING_DISK
	shadow_uv.x += uv_slice.z;
	shadow_uv = mad(
		shadow_uv,
		light.shadowAtlasMulAdd.xy,
		light.shadowAtlasMulAdd.zw);
	const half3 unbiased_shadow = sample_shadow(
		shadow_uv,
		unbiased_depth,
		shadow_border_clamp(light, uv_slice.z),
		light.GetRadius(),
		pixel);
#else
	shadow_border_shrink(light, shadow_uv);
	shadow_uv.x += uv_slice.z;
	shadow_uv = mad(
		shadow_uv,
		light.shadowAtlasMulAdd.xy,
		light.shadowAtlasMulAdd.zw);
	const half3 unbiased_shadow = sample_shadow(
		shadow_uv,
		unbiased_depth,
		pixel);
#endif

	const half biased_visibility = dot(
		biased_shadow,
		half3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
	const half unbiased_visibility = dot(
		unbiased_shadow,
		half3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
	return saturate(half3(
		biased_visibility,
		unbiased_visibility,
		face_edge));
}

bool visible_instance_boundary(
	uint2 pixel,
	uint center_instance_index,
	uint2 resolution)
{
	static const int2 offsets[4] = {
		int2(-1, 0),
		int2(1, 0),
		int2(0, -1),
		int2(0, 1),
	};

	[unroll]
	for (uint i = 0; i < 4; ++i)
	{
		const int2 neighbor_pixel = int2(pixel) + offsets[i];
		if (any(neighbor_pixel < 0) ||
			any(neighbor_pixel >= int2(resolution)))
		{
			return true;
		}

		const uint neighbor_packed_id =
			texture_primitiveID[uint2(neighbor_pixel)];
		if (neighbor_packed_id == 0)
			return true;

		PrimitiveID neighbor_primitive;
		neighbor_primitive.init();
		neighbor_primitive.unpack(neighbor_packed_id);
		if (neighbor_primitive.instanceIndex != center_instance_index)
			return true;
	}

	return false;
}

half2 point_shadow_center_diagnostic(
	in ShaderEntity light,
	in float3 light_vector,
	out half3 depth_relation)
{
	depth_relation = 0;
	const float3 uv_slice = cubemap_to_uv(-light_vector);
	float2 shadow_uv = uv_slice.xy;
#ifdef SHADOW_SAMPLING_DISK
	shadow_uv.x += uv_slice.z;
	shadow_uv = mad(
		shadow_uv,
		light.shadowAtlasMulAdd.xy,
		light.shadowAtlasMulAdd.zw);
	shadow_uv = clamp(
		shadow_uv,
		shadow_border_clamp(light, uv_slice.z).xy,
		shadow_border_clamp(light, uv_slice.z).zw);
#else
	shadow_border_shrink(light, shadow_uv);
	shadow_uv.x += uv_slice.z;
	shadow_uv = mad(
		shadow_uv,
		light.shadowAtlasMulAdd.xy,
		light.shadowAtlasMulAdd.zw);
#endif

	Texture2D<half4> texture_shadowatlas =
		bindless_textures_half4[
			descriptor_index(GetFrame().texture_shadowatlas_index)];
	Texture2D<half4> texture_shadowatlas_transparent =
		bindless_textures_half4[
			descriptor_index(GetFrame().texture_shadowatlas_transparent_index)];
	const half opaque_depth = texture_shadowatlas.SampleLevel(
		sampler_point_clamp, shadow_uv, 0).r;
	const half transparent_depth = texture_shadowatlas_transparent.SampleLevel(
		sampler_point_clamp, shadow_uv, 0).a;

	// Wicked uses reversed depth and clears the opaque and transparent shadow
	// depths to zero. Any positive value therefore proves that a caster wrote
	// the selected point-light face at this direction.
	const half caster_coverage =
		max(opaque_depth, transparent_depth) > 0 ? 1 : 0;

	const float major_axis_distance = max(
		max(abs(light_vector.x), abs(light_vector.y)),
		abs(light_vector.z));
	const float unbiased_depth =
		light.GetCubemapDepthRemapNear() +
		light.GetCubemapDepthRemapFar() /
			max(major_axis_distance, 1e-6);

	// Evaluate the exact center texel without the comparison sampler. Wicked's
	// shadow maps use reversed depth, so a receiver is visible when its compare
	// depth is greater than or equal to the stored opaque depth. This deliberately
	// avoids both the 16-tap Vogel disk and the sampler's bilinear comparison.
	half3 center_visibility =
		unbiased_depth >= opaque_depth ? half3(1, 1, 1) : half3(0, 0, 0);
	const half4 transparent_shadow =
		texture_shadowatlas_transparent.SampleLevel(
			sampler_point_clamp, shadow_uv, 0);
	if (transparent_shadow.a >= unbiased_depth)
	{
		center_visibility *= transparent_shadow.rgb;
	}
	const half center_hard_visibility = dot(
		center_visibility,
		half3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));

	const float stored_depth = max(
		(float)opaque_depth,
		(float)transparent_depth);
	if (stored_depth <= 0)
	{
		// No depth was written in this direction.
		depth_relation = half3(0, 0, 1);
	}
	else
	{
		const float remap_near = light.GetCubemapDepthRemapNear();
		const float remap_far = light.GetCubemapDepthRemapFar();
		const float stored_distance =
			remap_far / (stored_depth - remap_near);
		const float relative_distance_delta =
			(stored_distance - major_axis_distance) /
			max(major_axis_distance, 1e-6);
		const float2 face_resolution_xy = max(
			float2(1.0, 1.0),
			light.shadowAtlasMulAdd.xy *
				GetFrame().shadow_atlas_resolution);
		const float face_resolution = min(
			face_resolution_xy.x,
			face_resolution_xy.y);

		// One cubemap texel spans approximately 2 / resolution projected units.
		// Treat that footprint as equal-depth so D16 quantization and the existing
		// caster raster bias cannot turn the same surface into a false "farther"
		// classification.
		const float same_surface_tolerance = max(
			2.0 / face_resolution,
			0.001);
		if (relative_distance_delta < -same_surface_tolerance)
		{
			depth_relation = half3(1, 0, 0);
		}
		else if (relative_distance_delta > same_surface_tolerance)
		{
			depth_relation = half3(0, 0, 1);
		}
		else
		{
			depth_relation = half3(0, 1, 0);
		}
	}

	return half2(caster_coverage, center_hard_visibility);
}

bool dominant_point_light_diagnostic(
	in Surface surface,
	in uint instance_index,
	uint flat_tile_index,
	ShaderCamera camera,
	out half3 shadowed_direct,
	out half3 bias_compare,
	out half3 coverage_compare,
	out half3 filter_compare,
	out half3 depth_relation)
{
	shadowed_direct = 0;
	bias_compare = 0;
	coverage_compare = 0;
	filter_compare = 0;
	depth_relation = 0;
	if (camera.buffer_entitytiles_index < 0 || pointlights().empty())
		return false;

	float best_unshadowed_luminance = 0;
	bool found = false;
	ShaderEntityIterator iterator = pointlights();
	for (uint bucket = iterator.first_bucket(); bucket <= iterator.last_bucket(); ++bucket)
	{
		uint bucket_bits = load_entitytile(camera, flat_tile_index + bucket);
		bucket_bits = iterator.mask_entity(bucket, bucket_bits);
		bucket_bits = WaveReadLaneFirst(WaveActiveBitOr(bucket_bits));

		[loop]
		while (bucket_bits != 0)
		{
			const uint bucket_bit_index = firstbitlow(bucket_bits);
			bucket_bits ^= 1u << bucket_bit_index;
			const uint entity_index = bucket * 32 + bucket_bit_index;
			ShaderEntity light = load_entity(entity_index);

			if ((light.layerMask & surface.layerMask) == 0)
				continue;

			// Reuse the production point-light path twice. This keeps the
			// diagnostic aligned with line/radius lights, light masks,
			// transparent shadows and the exact material BRDF.
			Surface unshadowed_surface = surface;
			unshadowed_surface.SetReceiveShadow(false);
			Lighting unshadowed_lighting;
			unshadowed_lighting.create(0, 0, 0, 0);
			light_point(light, unshadowed_surface, unshadowed_lighting);
			const half3 unshadowed_direct =
				(1 - surface.refraction.a) * surface.albedo *
					unshadowed_lighting.direct.diffuse / PI +
				unshadowed_lighting.direct.specular;
			const float unshadowed_luminance =
				dot(max(0, unshadowed_direct), float3(0.2126, 0.7152, 0.0722));
			if (unshadowed_luminance <= best_unshadowed_luminance)
				continue;

			Lighting shadowed_lighting;
			shadowed_lighting.create(0, 0, 0, 0);
			light_point(light, surface, shadowed_lighting);
			const half3 resolved_direct =
				(1 - surface.refraction.a) * surface.albedo *
					shadowed_lighting.direct.diffuse / PI +
				shadowed_lighting.direct.specular;

			best_unshadowed_luminance = unshadowed_luminance;
			shadowed_direct = resolved_direct;
			if (light.IsCastingShadow() && surface.IsReceiveShadow())
			{
				const float3 light_vector = light.position - surface.P;
				const half receiverNoL = abs(dot(
					surface.facenormal,
					normalize(light_vector)));
				bias_compare = point_shadow_bias_compare(
					light,
					light_vector,
					surface.pixel,
					receiverNoL);
				const bool instance_boundary = visible_instance_boundary(
					surface.pixel,
					instance_index,
					uint2(camera.internal_resolution));
				half3 center_depth_relation = 0;
				const half2 center_diagnostic =
					point_shadow_center_diagnostic(
						light,
						light_vector,
						center_depth_relation);
				coverage_compare = half3(
					instance_boundary ? 1 : 0,
					center_diagnostic.x,
					bias_compare.g);
				filter_compare = half3(
					center_diagnostic.y,
					center_diagnostic.x,
					bias_compare.g);
				depth_relation = center_depth_relation;
			}
			else
			{
				bias_compare = half3(1, 1, 0);
				coverage_compare = half3(
					visible_instance_boundary(
						surface.pixel,
						instance_index,
						uint2(camera.internal_resolution)) ? 1 : 0,
					0,
					1);
				filter_compare = half3(1, 0, 1);
				depth_relation = half3(0, 0, 1);
			}
			found = true;
		}
	}
	return found;
}

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
		half3 client_vlm_gi = 0;
		if (sample_client_volumetric_lightmap(
				prim.instanceIndex,
				surface.N,
				client_vlm_gi))
		{
			surface.gi = client_vlm_gi;
			surface.SetGIApplied(true);
		}
		else
		{
			half3 ambient = GetAmbient(surface.N);
			surface.gi = lerp(
				ambient,
				ambient * surface.sss.rgb,
				saturate(surface.sss.a));
		}
	}

	// Exact material-independent local diffuse GI consumed by Final before the
	// remote elastic blend. Dynamic and otherwise unbaked surfaces use the
	// per-instance VLM sample, with ambient only outside the baked volume.
	[branch]
	if (push.local_indirect_diffuse_uav >= 0)
	{
		RWTexture2D<float4> output_local_indirect_diffuse =
			bindless_rwtextures[descriptor_index(push.local_indirect_diffuse_uav)];
		output_local_indirect_diffuse[pixel] = float4(max(0, surface.gi * PI), 1);
	}

	// Remote lighting is generated in the Server frame's screen space. Match
	// source_control_frame_id to the Client's bounded GBuffer history before
	// binding this block; then use that history for depth/normal rejection and
	// joint upscale at each semantic's negotiated resolution.
	float2 remote_uv = 0;
	float expected_remote_depth = 0;
	bool remote_projection_valid = false;
	[branch]
	if (any(external_weights > 0))
	{
		remote_projection_valid = external_project_surface(
			surface, remote_uv, expected_remote_depth);
	}

	half3 elastic_indirect_diffuse = surface.gi;
	[branch]
	if (remote_projection_valid && external_diffuse_texture >= 0 &&
		external_weights.x > 0)
	{
		bool valid = false;
		const half4 remote_indirect_diffuse = sample_external_joint(
			external_diffuse_texture,
			remote_uv,
			expected_remote_depth,
			surface.N,
			surface.roughness,
			surface.P,
			valid);
		// RemoteIndirectDiffuseFormal is irradiance (includes PI), while Wicked's
		// internal diffuse GI term is stored after the Lambert PI divide.
		if (valid)
		{
			elastic_indirect_diffuse = lerp(
				elastic_indirect_diffuse,
				remote_indirect_diffuse.rgb / PI,
				saturate(external_weights.x));
		}
	}

	Lighting lighting;
	lighting.create(0, 0, elastic_indirect_diffuse, 0);

	half remote_primary_visibility = 1;
	bool remote_primary_visibility_valid = false;
	if (remote_projection_valid && external_primary_visibility_texture >= 0 &&
		external_weights.w > 0)
	{
		remote_primary_visibility = sample_external_joint(
			external_primary_visibility_texture,
			remote_uv,
			expected_remote_depth,
			surface.N,
			surface.roughness,
			surface.P,
			remote_primary_visibility_valid).r;
	}
	const half primary_light_visibility = TiledLightingWithPrimaryVisibility(
		surface,
		lighting,
		entity_flat_tile_index,
		camera,
		push.primary_light_shadow_index,
		remote_primary_visibility,
		remote_primary_visibility_valid ? external_weights.w : 0);
	const half elastic_primary_light_visibility = remote_primary_visibility_valid
		? lerp(primary_light_visibility, remote_primary_visibility, saturate(external_weights.w))
		: primary_light_visibility;

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
	if (remote_projection_valid && external_ao_texture >= 0 && external_weights.y > 0)
	{
		bool valid = false;
		const half remote_ao = sample_external_joint(
			external_ao_texture,
			remote_uv,
			expected_remote_depth,
			surface.N,
			surface.roughness,
			surface.P,
			valid).r;
		if (valid)
		{
			elastic_screen_ao = lerp(
				elastic_screen_ao,
				remote_ao,
				saturate(external_weights.y));
		}
	}
#endif // CARTOON

	const half3 local_specular_indirect_pre_ao = lighting.indirect.specular;
	if (remote_projection_valid && external_specular_texture >= 0 &&
		external_weights.z > 0)
	{
		bool valid = false;
		const half3 remote_specular = sample_external_joint(
			external_specular_texture,
			remote_uv,
			expected_remote_depth,
			surface.N,
			surface.roughness,
			surface.P,
			valid).rgb;
		if (valid)
		{
			lighting.indirect.specular = lerp(
				local_specular_indirect_pre_ao,
				remote_specular,
				saturate(external_weights.z));
		}
	}
	surface.occlusion *= elastic_screen_ao;

	// Formal V3 specular is captured after reflection/environment resolution
	// and before screen/material occlusion or external blending.
	[branch]
	if (push.specular_indirect_pre_ao_uav >= 0)
	{
		RWTexture2D<float4> output_specular_indirect_pre_ao =
			bindless_rwtextures[descriptor_index(push.specular_indirect_pre_ao_uav)];
		output_specular_indirect_pre_ao[pixel] =
			float4(max(0, local_specular_indirect_pre_ao), 1);
	}
	[branch]
	if (push.primary_light_visibility_uav >= 0)
	{
		RWTexture2D<float4> output_primary_light_visibility =
			bindless_rwtextures[descriptor_index(push.primary_light_visibility_uav)];
		output_primary_light_visibility[pixel] = float4(saturate(primary_light_visibility).xxx, 1);
	}
	[branch]
	if (external_elastic_specular_uav >= 0)
	{
		RWTexture2D<float4> output_elastic_specular =
			bindless_rwtextures[descriptor_index(external_elastic_specular_uav)];
		output_elastic_specular[pixel] = float4(max(0, lighting.indirect.specular), 1);
	}
	[branch]
	if (external_elastic_primary_visibility_uav >= 0)
	{
		RWTexture2D<float4> output_elastic_primary_visibility =
			bindless_rwtextures[descriptor_index(external_elastic_primary_visibility_uav)];
		output_elastic_primary_visibility[pixel] =
			float4(saturate(elastic_primary_light_visibility).xxx, 1);
	}

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
		if (push.point_light_diagnostic != 0)
		{
			half3 point_direct = 0;
			half3 point_bias_compare = 0;
			half3 point_coverage_compare = 0;
			half3 point_filter_compare = 0;
			half3 point_depth_relation = 0;
			const bool point_valid = dominant_point_light_diagnostic(
				surface,
				prim.instanceIndex,
				entity_flat_tile_index,
				camera,
				point_direct,
				point_bias_compare,
				point_coverage_compare,
				point_filter_compare,
				point_depth_relation);
			if (push.point_light_diagnostic == 1)
			{
				output_specular_indirect[pixel] =
					float4(point_valid ? max(0, point_direct) : 0, 1);
			}
			else if (push.point_light_diagnostic == 2)
			{
				output_specular_indirect[pixel] =
					float4(point_valid ? point_bias_compare : 0, 1);
			}
			else if (push.point_light_diagnostic == 3)
			{
				output_specular_indirect[pixel] =
					float4(point_valid ? point_coverage_compare : 0, 1);
			}
			else if (push.point_light_diagnostic == 4)
			{
				output_specular_indirect[pixel] =
					float4(point_valid ? point_filter_compare : 0, 1);
			}
			else
			{
				output_specular_indirect[pixel] =
					float4(point_valid ? point_depth_relation : 0, 1);
			}
		}
		else
		{
			output_specular_indirect[pixel] =
				float4(max(0, local_specular_indirect_pre_ao * surface.occlusion), 1);
		}
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
