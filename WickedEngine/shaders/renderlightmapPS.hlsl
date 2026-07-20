#define RAY_BACKFACE_CULLING
#define TEXTURE_SLOT_NONUNIFORM
#include "globals.hlsli"
#include "raytracingHF.hlsli"
#include "lightingHF.hlsli"
#include "stochasticSSRHF.hlsli"
#include "lightmap_samplingHF.hlsli"
#include "lightmap_lightsamplingHF.hlsli"

Texture2D<float4> lightmap_statistics : register(t1);
Buffer<uint> lightmap_light_candidates : register(t2);
Buffer<uint> lightmap_guiding_histogram : register(t3);
RWBuffer<uint> lightmap_guiding_histogram_write : register(u0);

//#define DEBUG_CHARTS

// This value specifies after which bounce the anyhit will be disabled:
static const uint ANYTHIT_CUTOFF_AFTER_BOUNCE_COUNT = 4;

// Per-path radiance cap for offline lightmap accumulation.  Rare emissive or
// near-specular paths can otherwise contribute an arbitrarily bright sample;
// even hundreds of temporal samples will then retain isolated colored
// fireflies which BC6H preserves and Bloom greatly amplifies.  The cap is
// applied chroma-preservingly to individual samples, not to the converged
// lightmap, so normal HDR indirect illumination remains representable.
static const float LIGHTMAP_SAMPLE_RADIANCE_MAX = 16.0;
static const uint LIGHTMAP_GUIDING_TILE_SIZE = 8u;
static const uint LIGHTMAP_GUIDING_PHI_BINS = 8u;
static const uint LIGHTMAP_GUIDING_Z_BINS = 4u;
static const uint LIGHTMAP_GUIDING_BIN_COUNT =
	LIGHTMAP_GUIDING_PHI_BINS * LIGHTMAP_GUIDING_Z_BINS;
static const uint LIGHTMAP_GUIDING_TRAINING_SAMPLES = 128u;

struct Input
{
	float4 pos : SV_POSITION;
	centroid float3 pos3D : WORLDPOSITION;
	centroid float3 normal : NORMAL;

#ifdef DEBUG_CHARTS
	uint primitiveID : SV_PrimitiveID;
#endif // DEBUG_CHARTS
};

uint LightmapGuidingTileOffset(uint2 pixel)
{
	const uint tile_width = (xTraceResolution.x + LIGHTMAP_GUIDING_TILE_SIZE - 1u) /
		LIGHTMAP_GUIDING_TILE_SIZE;
	const uint2 tile = pixel / LIGHTMAP_GUIDING_TILE_SIZE;
	return (tile.y * tile_width + tile.x) * LIGHTMAP_GUIDING_BIN_COUNT;
}

uint LightmapGuidingDirectionBin(float3 direction, float3 N)
{
	const float3 local = mul(get_tangentspace(N), direction);
	const float phi = atan2(local.y, local.x) + PI;
	const uint phi_bin = min(uint(phi * (LIGHTMAP_GUIDING_PHI_BINS / (2.0 * PI))),
		LIGHTMAP_GUIDING_PHI_BINS - 1u);
	const uint z_bin = min(uint(saturate(local.z) * LIGHTMAP_GUIDING_Z_BINS),
		LIGHTMAP_GUIDING_Z_BINS - 1u);
	return z_bin * LIGHTMAP_GUIDING_PHI_BINS + phi_bin;
}

float3 SampleLightmapGuidedPrimary(
	uint2 pixel,
	float3 N,
	float4 random_sample,
	out float throughput_weight,
	out float proposal_pdf)
{
	const uint offset = LightmapGuidingTileOffset(pixel);
	uint total = 0;
	[unroll]
	for (uint i = 0; i < LIGHTMAP_GUIDING_BIN_COUNT; ++i)
		total += lightmap_guiding_histogram[offset + i];

	if (total == 0 || random_sample.x < 0.5)
	{
		const float3 direction = normalize(lightmap_sample_hemisphere_cos(N, random_sample.yz));
		const float cosine_pdf = saturate(dot(direction, N)) / PI;
		float guide_pdf = 0;
		if (total > 0)
		{
			const uint bin = LightmapGuidingDirectionBin(direction, N);
			const float bin_probability = float(lightmap_guiding_histogram[offset + bin]) / float(total);
			guide_pdf = bin_probability * LIGHTMAP_GUIDING_BIN_COUNT / (2.0 * PI);
		}
		const float mixture_pdf = total > 0 ? 0.5 * (cosine_pdf + guide_pdf) : cosine_pdf;
		proposal_pdf = mixture_pdf;
		throughput_weight = cosine_pdf / max(mixture_pdf, 1e-8);
		return direction;
	}

	const uint target = min(uint(random_sample.y * total), total - 1u);
	uint accumulated = 0;
	uint selected_bin = 0;
	[loop]
	for (uint i = 0; i < LIGHTMAP_GUIDING_BIN_COUNT; ++i)
	{
		accumulated += lightmap_guiding_histogram[offset + i];
		if (target < accumulated)
		{
			selected_bin = i;
			break;
		}
	}
	const uint z_bin = selected_bin / LIGHTMAP_GUIDING_PHI_BINS;
	const uint phi_bin = selected_bin % LIGHTMAP_GUIDING_PHI_BINS;
	const float z = (float(z_bin) + random_sample.z) / LIGHTMAP_GUIDING_Z_BINS;
	const float phi = 2.0 * PI * (float(phi_bin) + random_sample.w) /
		LIGHTMAP_GUIDING_PHI_BINS - PI;
	const float radial = sqrt(saturate(1.0 - z * z));
	const float3 local = float3(cos(phi) * radial, sin(phi) * radial, z);
	const float3 direction = normalize(mul(local, get_tangentspace(N)));
	const float cosine_pdf = saturate(dot(direction, N)) / PI;
	const float bin_probability = float(lightmap_guiding_histogram[offset + selected_bin]) /
		float(total);
	const float guide_pdf = bin_probability * LIGHTMAP_GUIDING_BIN_COUNT / (2.0 * PI);
	proposal_pdf = 0.5 * (cosine_pdf + guide_pdf);
	throughput_weight = cosine_pdf / max(proposal_pdf, 1e-8);
	return direction;
}

static const float2 tangent_directions[] = {
	float2(1, 0),
	float2(-1, 0),
	float2(0, 1),
	float2(0, -1),
};

// Bakery pixel pushing: https://ndotl.wordpress.com/2018/08/29/baking-artifact-free-lightmaps/
//	This can push position outside of enclosed area within a pixel to remove shadow leaks
//	Instead the shadow texel reaching outside, this will make light go inside which is better in most cases
void BakeryPixelPush(inout float3 P, in float3 N, in float2 UV, inout RNG rng, inout float bakerydebug)
{
	float3 dUV1 = max(abs(ddx(P)), abs(ddy(P)));
	float dPos = max(max(dUV1.x, dUV1.y), dUV1.z);
	dPos = dPos * SQRT2; // convert to diagonal (small overshoot)
	
	float3x3 TBN = compute_tangent_frame(N, P, UV);

	bool valid = true; // AMD DX12 issue workaround for early break/return!
	
	for (uint i = 0; i < arraysize(tangent_directions) && WaveActiveAnyTrue(valid); ++i)
	{
		RayDesc ray;
		ray.Origin = P + N * 0.001;
		ray.Direction = normalize(mul(float3(tangent_directions[i], 1), TBN));
		ray.TMin = 0.001;
		ray.TMax = dPos;
	
		bool backface_hit = false;
		float3 hit_pos = 0;
		float3 hit_nor = 0;
	
		Surface surface;
		surface.init();
		surface.V = -ray.Direction;

		valid = true;

#ifdef RTAPI
		uint flags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
		flags |= RAY_FLAG_FORCE_OPAQUE;
		flags |= RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
		wiRayQuery q;
		q.TraceRayInline(
			scene_acceleration_structure,	// RaytracingAccelerationStructure AccelerationStructure
			flags,							// uint RayFlags
			xTraceUserData.y,				// uint InstanceInclusionMask
			ray								// RayDesc Ray
		);
		while (q.Proceed());
		if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && !q.CommittedTriangleFrontFace())
		{
			backface_hit = true;
			hit_pos = q.WorldRayOrigin() + q.WorldRayDirection() * q.CommittedRayT();
		
			PrimitiveID prim;
			prim.init();
			prim.primitiveIndex = q.CommittedPrimitiveIndex();
			prim.instanceIndex = q.CommittedInstanceID();
			prim.subsetIndex = q.CommittedGeometryIndex();

			surface.SetBackface(!q.CommittedTriangleFrontFace());

			surface.hit_depth = q.CommittedRayT();
			if (!surface.load(prim, q.CommittedTriangleBarycentrics()))
				valid = false;

			hit_nor = surface.facenormal;
		}
#else
		RayHit hit = TraceRay_Closest(ray, xTraceUserData.y, rng);
		if (hit.distance < FLT_MAX && hit.is_backface)
		{
			backface_hit = true;
			hit_pos = ray.Origin + ray.Direction * hit.distance;
		
			surface.SetBackface(hit.is_backface);

			surface.hit_depth = hit.distance;
			if (!surface.load(hit.primitiveID, hit.bary))
				valid = false;

			hit_nor = surface.facenormal;
		}
#endif // RTAPI

		if (valid && backface_hit)
		{
			bakerydebug = 1;
			P = hit_pos - hit_nor * 0.001;
		}
	}
}

bool IsLightmapLightEligible(ShaderEntity light, uint bounce)
{
	// Mixed-lighting policy: static lights contribute direct + indirect;
	// dynamic lights keep their direct term at runtime but can still contribute
	// bounced indirect lighting after the first surface interaction.
	return bounce > 0 || light.IsStaticLight();
}

struct LightmapEmitterHit
{
	float distance;
	float3 radiance;
	float light_pdf;
	bool valid;
};

LightmapEmitterHit TraceLightmapEmitterHit(
	RayDesc ray,
	float3 receiver_normal,
	uint light_bounce,
	float geometry_distance)
{
	LightmapEmitterHit best = (LightmapEmitterHit)0;
	best.distance = geometry_distance;
	float total_weight = 0;
	const uint light_count = lightmap_light_candidates[0];
	for (uint i = 0; i < light_count; ++i)
	{
		ShaderEntity light = load_entity(lightmap_light_candidates[1u + i]);
		if (IsLightmapLightEligible(light, light_bounce))
			total_weight += EstimateLightmapEmitter(light, ray.Origin, receiver_normal);
	}
	if (total_weight <= 0)
		return best;

	for (uint i = 0; i < light_count; ++i)
	{
		ShaderEntity light = load_entity(lightmap_light_candidates[1u + i]);
		if (!IsLightmapLightEligible(light, light_bounce))
			continue;
		const float selection_weight = EstimateLightmapEmitter(light, ray.Origin, receiver_normal);
		if (selection_weight <= 0)
			continue;

		float hit_distance = FLT_MAX;
		float shape_pdf = 0;
		float3 energy_over_pdf = 0;
		switch (light.GetType())
		{
		case ENTITY_TYPE_DIRECTIONALLIGHT:
		{
			const float angular_radius = atan(max(light.GetRadius(), 0.0));
			if (angular_radius <= 1e-5 || geometry_distance < FLT_MAX)
				break; // delta lights cannot be reached by continuous BSDF sampling
			const float cos_theta_max = cos(min(angular_radius, PI * 0.499));
			if (dot(ray.Direction, normalize(light.GetDirection().xyz)) < cos_theta_max)
				break;
			hit_distance = FLT_MAX - 1.0;
			shape_pdf = LightmapUniformConePdf(cos_theta_max);
			float3 transmittance = 1;
			if (GetFrame().options & OPTION_BIT_REALISTIC_SKY)
				transmittance = GetAtmosphericLightTransmittance(
					GetWeather().atmosphere, ray.Origin, ray.Direction, texture_transmittancelut);
			energy_over_pdf = light.GetColor().rgb * transmittance;
		}
		break;
		case ENTITY_TYPE_POINTLIGHT:
		case ENTITY_TYPE_SPOTLIGHT:
		{
			if (light.GetRadius() <= 1e-5 || light.GetLength() > 1e-5)
				break;
			hit_distance = LightmapRaySphereDistance(
				ray.Origin, ray.Direction, light.position, light.GetRadius());
			if (hit_distance >= best.distance)
				break;
			const float3 center_delta = light.position - ray.Origin;
			const float center_dist2 = dot(center_delta, center_delta);
			const float center_distance = sqrt(max(center_dist2, 1e-12));
			const float radius = min(light.GetRadius(), center_distance * 0.995);
			const float cos_theta_max = sqrt(saturate(1.0 - radius * radius / center_dist2));
			shape_pdf = LightmapUniformConePdf(cos_theta_max);
			float attenuation = attenuation_pointlight(
				center_dist2, light.GetRange(), light.GetRange2Rcp());
			if (light.GetType() == ENTITY_TYPE_SPOTLIGHT)
			{
				const float spot_factor = dot(normalize(center_delta), light.GetDirection());
				attenuation = attenuation_spotlight(
					center_dist2, light.GetRange(), light.GetRange2Rcp(), spot_factor,
					light.GetAngleScale(), light.GetAngleOffset());
			}
			energy_over_pdf = light.GetColor().rgb * attenuation;
		}
		break;
		case ENTITY_TYPE_RECTLIGHT:
		{
			const half4 quaternion = light.GetQuaternion();
			const float3 right = rotate_vector(half3(1, 0, 0), quaternion);
			const float3 up = rotate_vector(half3(0, 1, 0), quaternion);
			const float3 forward = cross(up, right);
			const float denominator = dot(ray.Direction, forward);
			if (denominator >= -1e-6)
				break;
			hit_distance = dot(light.position - ray.Origin, forward) / denominator;
			if (hit_distance <= 0.001 || hit_distance >= best.distance)
				break;
			const float3 local = ray.Origin + ray.Direction * hit_distance - light.position;
			const float width = max(light.GetLength(), 1e-4);
			const float height = max(light.GetHeight(), 1e-4);
			if (abs(dot(local, right)) > width * 0.5 || abs(dot(local, up)) > height * 0.5)
				break;
			const float area = width * height;
			const float dist2 = hit_distance * hit_distance;
			shape_pdf = dist2 / max(-denominator * area, 1e-12);
			energy_over_pdf = light.GetColor().rgb * attenuation_pointlight(
				dist2, light.GetRange(), light.GetRange2Rcp()) * area * PI;
		}
		break;
		}

		if (hit_distance < best.distance && shape_pdf > 0 && any(energy_over_pdf > 0))
		{
			best.distance = hit_distance;
			best.radiance = energy_over_pdf * shape_pdf;
			best.light_pdf = (selection_weight / total_weight) * shape_pdf;
			best.valid = true;
		}
	}
	return best;
}

float EstimateLightmapLightContribution(ShaderEntity light, float3 P, float3 N)
{
	const float radiance = max3(max(0, light.GetColor().rgb));
	if (radiance <= 0)
		return 0;

	// Keep a small non-zero floor for every active eligible light. The estimate
	// is allowed to be approximate, but assigning zero probability to a light
	// whose finite shape can still contribute would bias the estimator.
	const float minimum_weight = radiance * 1e-4;
	float estimate = 0;

	switch (light.GetType())
	{
	case ENTITY_TYPE_DIRECTIONALLIGHT:
	{
		const float3 L = normalize(light.GetDirection().xyz);
		estimate = radiance * saturate(dot(L, N));
	}
	break;
	case ENTITY_TYPE_POINTLIGHT:
	case ENTITY_TYPE_RECTLIGHT:
	case ENTITY_TYPE_SPOTLIGHT:
	{
		const float3 delta = light.position - P;
		const float distance_squared = dot(delta, delta);
		const float range = light.GetRange();
		if (distance_squared > 1e-8 && distance_squared < range * range)
		{
			const float3 L = delta * rsqrt(distance_squared);
			float angular = saturate(dot(L, N));
			if (light.GetType() == ENTITY_TYPE_RECTLIGHT)
			{
				const half4 quaternion = light.GetQuaternion();
				const half3 right = rotate_vector(half3(1, 0, 0), quaternion);
				const half3 up = rotate_vector(half3(0, 1, 0), quaternion);
				const half3 forward = cross(up, right);
				angular *= dot(P - light.position, forward) > 0 ? 1 : 0;
				angular *= attenuation_pointlight(
					distance_squared, range, light.GetRange2Rcp());
			}
			else if (light.GetType() == ENTITY_TYPE_SPOTLIGHT)
			{
				const float spot_factor = dot(L, light.GetDirection());
				angular *= spot_factor > light.GetConeAngleCos()
					? attenuation_spotlight(
						distance_squared,
						range,
						light.GetRange2Rcp(),
						spot_factor,
						light.GetAngleScale(),
						light.GetAngleOffset())
					: 0;
			}
			else
			{
				angular *= attenuation_pointlight(
					distance_squared, range, light.GetRange2Rcp());
			}
			estimate = radiance * angular;
		}
	}
	break;
	}

	return max(estimate, minimum_weight);
}

// Evaluate diffuse next-event estimation at the current surface before a
// continuation lobe is selected.  Keeping this separate from path throughput
// is important: if direct lighting is deferred until the next iteration, its
// energy is accidentally conditioned on (and tinted by) the randomly selected
// reflection/refraction lobe.
float3 EvaluateLightmapDirect(
	Surface receiver,
	float3 diffuse_throughput,
	uint light_bounce,
	float4 light_sample,
	inout RNG rng)
{
	const uint light_count = lights().item_count();
	uint light_index = ~0u;
	float light_pick_probability = 0;
	float total_light_weight = 0;

	for (uint candidate = 0; candidate < light_count; ++candidate)
	{
		ShaderEntity candidate_light = load_entity(lights().first_item() + candidate);
		if (!IsLightmapLightEligible(candidate_light, light_bounce))
			continue;
		total_light_weight += EstimateLightmapLightContribution(
			candidate_light, receiver.P, receiver.N);
	}

	if (total_light_weight > 0)
	{
		const float target_weight = light_sample.x * total_light_weight;
		float accumulated_weight = 0;
		for (uint candidate = 0; candidate < light_count; ++candidate)
		{
			const uint candidate_index = lights().first_item() + candidate;
			ShaderEntity candidate_light = load_entity(candidate_index);
			if (!IsLightmapLightEligible(candidate_light, light_bounce))
				continue;
			const float candidate_weight = EstimateLightmapLightContribution(
				candidate_light, receiver.P, receiver.N);
			accumulated_weight += candidate_weight;
			if (target_weight < accumulated_weight)
			{
				light_index = candidate_index;
				light_pick_probability = candidate_weight / total_light_weight;
				break;
			}
		}
	}

	if (light_index == ~0u)
		return 0;

	ShaderEntity light = load_entity(light_index);
	float3 light_radiance = 0;
	float3 L = 0;
	float dist = 0;
	float NdotL = 0;

	switch (light.GetType())
	{
	case ENTITY_TYPE_DIRECTIONALLIGHT:
	{
		dist = FLT_MAX;
		L = light.GetDirection().xyz;
		L += lightmap_sample_hemisphere_cos(L, light_sample.yz) * light.GetRadius();
		NdotL = saturate(dot(L, receiver.N));
		if (NdotL > 0)
		{
			float3 atmosphereTransmittance = 1.0;
			if (GetFrame().options & OPTION_BIT_REALISTIC_SKY)
			{
				atmosphereTransmittance = GetAtmosphericLightTransmittance(
					GetWeather().atmosphere, receiver.P, L, texture_transmittancelut);
			}
			light_radiance = light.GetColor().rgb * atmosphereTransmittance;
		}
	}
	break;
	case ENTITY_TYPE_POINTLIGHT:
	{
		light.position += light.GetDirection() * (light_sample.y - 0.5) * light.GetLength();
		light.position += lightmap_sample_hemisphere_cos(
			normalize(light.position - receiver.P), light_sample.zw) * light.GetRadius();
		L = light.position - receiver.P;
		const float dist2 = dot(L, L);
		const float range = light.GetRange();
		if (dist2 < range * range && dist2 > 1e-8)
		{
			dist = sqrt(dist2);
			L /= dist;
			NdotL = saturate(dot(L, receiver.N));
			if (NdotL > 0)
			{
				light_radiance = light.GetColor().rgb *
					attenuation_pointlight(dist2, range, light.GetRange2Rcp());
			}
		}
	}
	break;
	case ENTITY_TYPE_RECTLIGHT:
	{
		const half4 quaternion = light.GetQuaternion();
		const half3 right = rotate_vector(half3(1, 0, 0), quaternion);
		const half3 up = rotate_vector(half3(0, 1, 0), quaternion);
		const half3 forward = cross(up, right);
		if (dot(receiver.P - light.position, forward) <= 0)
			break;
		light.position += right * (light_sample.y - 0.5) * light.GetLength();
		light.position += up * (light_sample.z - 0.5) * light.GetHeight();
		L = light.position - receiver.P;
		const float dist2 = dot(L, L);
		const float range = light.GetRange();
		if (dist2 < range * range && dist2 > 1e-8)
		{
			dist = sqrt(dist2);
			L /= dist;
			NdotL = saturate(dot(L, receiver.N));
			if (NdotL > 0)
			{
				light_radiance = light.GetColor().rgb *
					attenuation_pointlight(dist2, range, light.GetRange2Rcp());
			}
		}
	}
	break;
	case ENTITY_TYPE_SPOTLIGHT:
	{
		const float3 Loriginal = normalize(light.position - receiver.P);
		light.position += lightmap_sample_hemisphere_cos(
			normalize(light.position - receiver.P), light_sample.zw) * light.GetRadius();
		L = light.position - receiver.P;
		const float dist2 = dot(L, L);
		const float range = light.GetRange();
		if (dist2 < range * range && dist2 > 1e-8)
		{
			dist = sqrt(dist2);
			L /= dist;
			NdotL = saturate(dot(L, receiver.N));
			if (NdotL > 0)
			{
				const float spot_factor = dot(Loriginal, light.GetDirection());
				if (spot_factor > light.GetConeAngleCos())
				{
					light_radiance = light.GetColor().rgb * attenuation_spotlight(
						dist2, range, light.GetRange2Rcp(), spot_factor,
						light.GetAngleScale(), light.GetAngleOffset());
				}
			}
		}
	}
	break;
	}

	if (NdotL <= 0 || dist <= 0 || !any(light_radiance))
		return 0;

	float3 visibility = diffuse_throughput;
	RayDesc shadow_ray;
	shadow_ray.Origin = receiver.P + receiver.N * 0.001;
	shadow_ray.TMin = 0.001;
	shadow_ray.TMax = dist;
	shadow_ray.Direction = normalize(L + max3(receiver.sss));

#ifdef RTAPI
	uint flags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_FRONT_FACING_TRIANGLES;
	if (light_bounce > ANYTHIT_CUTOFF_AFTER_BOUNCE_COUNT)
	{
		flags |= RAY_FLAG_FORCE_OPAQUE;
	}

	wiRayQuery shadow_query;
	shadow_query.TraceRayInline(
		scene_acceleration_structure,
		flags,
		xTraceUserData.y,
		shadow_ray
	);
	while (shadow_query.Proceed())
	{
		PrimitiveID prim;
		prim.init();
		prim.primitiveIndex = shadow_query.CandidatePrimitiveIndex();
		prim.instanceIndex = shadow_query.CandidateInstanceID();
		prim.subsetIndex = shadow_query.CandidateGeometryIndex();

		Surface blocker;
		blocker.init();
		if (!blocker.load(prim, shadow_query.CandidateTriangleBarycentrics()))
			break;

		visibility *= lerp(1, blocker.albedo * blocker.transmission, blocker.opacity);
		if (!any(visibility))
		{
			shadow_query.CommitNonOpaqueTriangleHit();
		}
	}
	if (shadow_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		visibility = 0;
	}
#else
	if (TraceRay_Any(shadow_ray, xTraceUserData.y, rng))
	{
		visibility = 0;
	}
#endif // RTAPI

	return light_radiance * visibility * NdotL /
		(PI * max(light_pick_probability, 1e-8));
}

// Production NEE path. The emitter helper samples every finite source in a
// measure that is also evaluable by the BSDF strategy. This removes the old
// source-size-dependent estimator (rectangles were missing their area/Jacobian
// and spherical point lights were jittered without a shape PDF).
float3 EvaluateLightmapDirectMIS(
	Surface receiver,
	float3 diffuse_throughput,
	float diffuse_bsdf_probability,
	uint light_bounce,
	float4 random_sample,
	inout RNG rng)
{
	const uint light_count = lightmap_light_candidates[0];
	uint selected_entity = ~0u;
	float selected_weight = 0;
	float total_weight = 0;

	for (uint candidate = 0; candidate < light_count; ++candidate)
	{
		const uint entity_index = lightmap_light_candidates[1u + candidate];
		ShaderEntity candidate_light = load_entity(entity_index);
		if (!IsLightmapLightEligible(candidate_light, light_bounce))
			continue;
		total_weight += EstimateLightmapEmitter(candidate_light, receiver.P, receiver.N);
	}

	if (total_weight <= 0)
		return 0;

	const float target = random_sample.x * total_weight;
	float accumulated = 0;
	for (uint candidate = 0; candidate < light_count; ++candidate)
	{
		const uint entity_index = lightmap_light_candidates[1u + candidate];
		ShaderEntity candidate_light = load_entity(entity_index);
		if (!IsLightmapLightEligible(candidate_light, light_bounce))
			continue;
		const float weight = EstimateLightmapEmitter(candidate_light, receiver.P, receiver.N);
		accumulated += weight;
		if (weight > 0 && target < accumulated)
		{
			selected_entity = entity_index;
			selected_weight = weight;
			break;
		}
	}

	if (selected_entity == ~0u || selected_weight <= 0)
		return 0;

	ShaderEntity light = load_entity(selected_entity);
	LightmapLightSample light_sample = SampleLightmapEmitter(
		light, selected_entity, receiver.P, receiver.N, random_sample.yzw);
	const float NdotL = saturate(dot(light_sample.direction, receiver.N));
	if (!light_sample.valid || NdotL <= 0 || light_sample.distance <= 0)
		return 0;

	float3 visibility = diffuse_throughput;
	RayDesc shadow_ray;
	shadow_ray.Origin = receiver.P + receiver.N * 0.001;
	shadow_ray.TMin = 0.001;
	shadow_ray.TMax = light_sample.distance == FLT_MAX ? FLT_MAX : max(0.001, light_sample.distance - 0.001);
	shadow_ray.Direction = normalize(light_sample.direction + max3(receiver.sss));

#ifdef RTAPI
	uint flags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_FRONT_FACING_TRIANGLES;
	if (light_bounce > ANYTHIT_CUTOFF_AFTER_BOUNCE_COUNT)
		flags |= RAY_FLAG_FORCE_OPAQUE;
	wiRayQuery shadow_query;
	shadow_query.TraceRayInline(scene_acceleration_structure, flags, xTraceUserData.y, shadow_ray);
	while (shadow_query.Proceed())
	{
		PrimitiveID prim;
		prim.init();
		prim.primitiveIndex = shadow_query.CandidatePrimitiveIndex();
		prim.instanceIndex = shadow_query.CandidateInstanceID();
		prim.subsetIndex = shadow_query.CandidateGeometryIndex();
		Surface blocker;
		blocker.init();
		if (!blocker.load(prim, shadow_query.CandidateTriangleBarycentrics()))
			break;
		visibility *= lerp(1, blocker.albedo * blocker.transmission, blocker.opacity);
		if (!any(visibility))
			shadow_query.CommitNonOpaqueTriangleHit();
	}
	if (shadow_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		visibility = 0;
#else
	if (TraceRay_Any(shadow_ray, xTraceUserData.y, rng))
		visibility = 0;
#endif

	const float pick_pdf = selected_weight / total_weight;
	float mis_weight = 1;
	if (!light_sample.delta && light_sample.supports_mis)
	{
		const float light_pdf = pick_pdf * light_sample.shape_pdf;
		const float bsdf_pdf = diffuse_bsdf_probability * NdotL / PI;
		mis_weight = LightmapPowerHeuristic(light_pdf, bsdf_pdf);
	}
	return light_sample.energy_over_shape_pdf * visibility * NdotL * mis_weight /
		(PI * max(pick_pdf, 1e-8));
}

struct LightmapOutput
{
	float4 accumulated : SV_TARGET0;
	float4 batch : SV_TARGET1;
};

[earlydepthstencil]
LightmapOutput main(Input input)
{
	LightmapOutput output = (LightmapOutput)0;
#ifdef DEBUG_CHARTS
	output.accumulated = float4(random_color(input.primitiveID), 1);
	output.batch = output.accumulated;
	return output;
#endif // DEBUG_CHARTS

	if (lightmap_statistics[(uint2)input.pos.xy].w > 0.5)
		return output;

	float2 uv = input.pos.xy * xTraceResolution_rcp;

	Surface surface;
	surface.init();
	surface.N = normalize(input.normal);

	RNG rng;
	rng.init((uint2)input.pos.xy, xTraceSampleIndex);
	LightmapQmcSampler qmc;
	qmc.init((uint2)input.pos.xy, xTraceSampleIndex, xTraceUserData.z);

	float3 P = input.pos3D;

	float bakerydebug = 0;
	BakeryPixelPush(P, surface.N, uv, rng, bakerydebug);
	
	RayDesc ray;
	ray.Origin = P + surface.N * 0.001;
	const uint2 lightmap_pixel = (uint2)input.pos.xy;
	const float4 primary_sample = qmc.sample4D(0);
	float primary_throughput = 1;
	float previous_bsdf_pdf = 0;
	if (xTraceUserData.w != 0)
		ray.Direction = SampleLightmapGuidedPrimary(
			lightmap_pixel, surface.N, primary_sample,
			primary_throughput, previous_bsdf_pdf);
	else
	{
		ray.Direction = normalize(lightmap_sample_hemisphere_cos(surface.N, primary_sample.xy));
		previous_bsdf_pdf = saturate(dot(ray.Direction, surface.N)) / PI;
	}
	const float3 primary_direction = ray.Direction;
	const float3 lightmap_normal = surface.N;
	ray.TMin = 0.001;
	ray.TMax = FLT_MAX;
	// A baked lightmap contains traced static lights and sky/environment only.
	// GetAmbientColor() is Wicked's runtime fallback for surfaces without baked
	// GI; seeding it here would inject the authored weather tint unoccluded into
	// every valid texel and produce a uniform color cast in the baked result.
	float3 result = 0;
	float3 energy = primary_throughput;
	surface.P = ray.Origin;
	// Bounce zero is incident irradiance for the original lightmapped surface;
	// its material response is applied later by the runtime lighting shader.
	const float3 original_direct = EvaluateLightmapDirectMIS(
		surface, 1, 1, 0, qmc.sample4D(1), rng);
	result += original_direct;

	const uint bounces = xTraceUserData.x;
	uint path_event_index = 0;
	for (uint bounce = 0; bounce < bounces; ++bounce)
	{
#ifdef RTAPI
		wiRayQuery q;
#endif // RTAPI

		surface.P = ray.Origin;
		// Dimension group 1 belongs to original-surface direct lighting.  Every
		// traced interaction gets a disjoint light/BSDF/RR dimension triplet.
		const uint dimension_base = 2u + path_event_index * 3u;
		++path_event_index;
		const float4 light_sample = qmc.sample4D(dimension_base);
		const float4 bsdf_sample = qmc.sample4D(dimension_base + 1u);
		const float continuation_sample = qmc.sample4D(dimension_base + 2u).x;

		// Sample primary ray (scene materials, sky, etc):
		ray.Direction = normalize(ray.Direction);

#ifdef RTAPI

		uint flags = 0;
#ifdef RAY_BACKFACE_CULLING
		flags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
#endif // RAY_BACKFACE_CULLING
		if (bounce > ANYTHIT_CUTOFF_AFTER_BOUNCE_COUNT)
		{
			flags |= RAY_FLAG_FORCE_OPAQUE;
		}

		q.TraceRayInline(
			scene_acceleration_structure,	// RaytracingAccelerationStructure AccelerationStructure
			flags,							// uint RayFlags
			xTraceUserData.y,				// uint InstanceInclusionMask
			ray								// RayDesc Ray
		);
		while (q.Proceed());
		const float geometry_distance = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ?
			q.CommittedRayT() : FLT_MAX;
#else
		RayHit hit = TraceRay_Closest(ray, xTraceUserData.y, rng);
		const float geometry_distance = hit.distance;
#endif // RTAPI

		const LightmapEmitterHit emitter_hit = TraceLightmapEmitterHit(
			ray, surface.N, bounce, geometry_distance);
		if (emitter_hit.valid)
		{
			const float mis_weight = LightmapPowerHeuristic(
				previous_bsdf_pdf, emitter_hit.light_pdf);
			result += max(0, energy * emitter_hit.radiance * mis_weight);
			break;
		}

		if (geometry_distance >= FLT_MAX - 1)
		{
			float3 envColor;
			[branch]
			if (IsStaticSky())
			{
				// We have envmap information in a texture:
				envColor = GetStaticSkyColor(ray.Direction);
			}
			else
			{
				envColor = GetDynamicSkyColor(ray.Direction, true, false, true);
			}
			result += max(0, energy * envColor);
			break;
		}

#ifdef RTAPI
		// ray origin updated for next bounce:
		ray.Origin = q.WorldRayOrigin() + q.WorldRayDirection() * q.CommittedRayT();

		PrimitiveID prim;
		prim.init();
		prim.primitiveIndex = q.CommittedPrimitiveIndex();
		prim.instanceIndex = q.CommittedInstanceID();
		prim.subsetIndex = q.CommittedGeometryIndex();

		surface.SetBackface(!q.CommittedTriangleFrontFace());

		if (!surface.load(prim, q.CommittedTriangleBarycentrics()))
			return output;

#else
		// ray origin updated for next bounce:
		ray.Origin = ray.Origin + ray.Direction * hit.distance;

		surface.SetBackface(hit.is_backface);

		if (!surface.load(hit.primitiveID, hit.bary))
			return output;

#endif // RTAPI

		surface.update();

		result += energy * surface.emissiveColor;

		// Evaluate the diffuse light connection at this vertex before choosing
		// a continuation lobe.  The old deferred ordering used post-BSDF energy
		// and multiplied albedo again, which both double-tinted indirect light
		// and made it depend on a random specular/refraction choice.
		if (bounce + 1u < bounces)
		{
			const float3 diffuse_throughput = energy * surface.albedo *
				(1 - surface.F) * (1 - surface.transmission);
			const float diffuse_bsdf_probability = (1 - surface.transmission) *
				(1 - dot(surface.F, 0.333));
			result += EvaluateLightmapDirectMIS(
				surface, diffuse_throughput, diffuse_bsdf_probability,
				bounce + 1u, light_sample, rng);
		}

		if (bsdf_sample.x < surface.transmission)
		{
			// Refraction
			const float3 R = refract(ray.Direction, surface.N, 1 - surface.material.GetRefraction());
			float roughnessBRDF = sqr(clamp(surface.roughness, min_roughness, 1));
			ray.Direction = lerp(
				R,
				lightmap_sample_hemisphere_cos(R, bsdf_sample.zw),
				roughnessBRDF);
			energy *= surface.albedo / max(0.001, surface.transmission);
			previous_bsdf_pdf = 0; // no analytic emitter competitor for refraction

			// Add a new bounce iteration, otherwise the transparent effect can disappear:
			bounce--;
		}
		else
		{
			const float specular_chance = dot(surface.F, 0.333);
			if (bsdf_sample.y < specular_chance)
			{
				// Specular reflection
				const float4 reflection_sample = ReflectionDir_GGX(
					-ray.Direction,
					surface.N,
					surface.roughness,
					bsdf_sample.zw);
				ray.Direction = reflection_sample.xyz;
				previous_bsdf_pdf = (1 - surface.transmission) *
					specular_chance * reflection_sample.w;
				energy *= surface.F / max(0.001, specular_chance) / max(0.001, 1 - surface.transmission);
			}
			else
			{
				// Diffuse reflection
				ray.Direction = lightmap_sample_hemisphere_cos(surface.N, bsdf_sample.zw);
				previous_bsdf_pdf = (1 - surface.transmission) *
					(1 - specular_chance) * saturate(dot(ray.Direction, surface.N)) / PI;
				energy *= surface.albedo * (1 - surface.F) / max(0.001, 1 - specular_chance) / max(0.001, 1 - surface.transmission);
			}
			
			ray.Origin += surface.facenormal * 0.001; // NOTE: TMin on AMD doesn't handle CommittedTriangleFrontFace correctly, so origin is updated instead!
		}

		// Terminate ray's path or apply inverse termination bias:
		const float termination_chance = saturate(max3(energy));
		if (termination_chance <= 1e-4)
		{
			break;
		}
		if (continuation_sample > termination_chance)
		{
			break;
		}
		energy /= termination_chance;
		previous_bsdf_pdf *= termination_chance;

	}

	//if(bakerydebug > 0)
	//	result = float3(1,0,0);
	
	result = max(0, result);
	if (xTraceSampleIndex < LIGHTMAP_GUIDING_TRAINING_SAMPLES)
	{
		// Train only from the primary-path component; original-surface NEE is
		// independent of the primary direction and would flatten the guide.
		const float3 path_component = max(0, result - original_direct);
		const float luminance = dot(path_component, float3(0.2126, 0.7152, 0.0722));
		const uint fixed_weight = min(uint(luminance * 4096.0 + 0.5), 0x00FFFFFFu);
		if (fixed_weight > 0)
		{
			const uint bin = LightmapGuidingDirectionBin(primary_direction, lightmap_normal);
			InterlockedAdd(
				lightmap_guiding_histogram_write[LightmapGuidingTileOffset(lightmap_pixel) + bin],
				fixed_weight);
		}
	}
	if (any(isnan(result)) || any(isinf(result)))
	{
		// Alpha zero keeps an invalid path out of the running average and out
		// of the persisted coverage mask.
		return output;
	}
	else
	{
		const float peak = max3(result);
		result *= min(1.0, LIGHTMAP_SAMPLE_RADIANCE_MAX / max(peak, 1e-4));
	}

	// Both render targets use additive blending. RGB is an unnormalised radiance
	// sum and alpha is the local valid-sample count. UV raster jitter means a
	// texel is not necessarily covered in every object iteration, so using the
	// global iteration as a running-average weight biases late-covered texels
	// toward black and makes entire low-density triangles fail coverage.
	output.accumulated = float4(result, 1);
	output.batch = float4(result, 1);
	return output;
}
