#define RAY_BACKFACE_CULLING
#define TEXTURE_SLOT_NONUNIFORM
#include "globals.hlsli"
#include "raytracingHF.hlsli"
#include "lightingHF.hlsli"
#include "stochasticSSRHF.hlsli"
#include "lightmap_samplingHF.hlsli"

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

struct Input
{
	float4 pos : SV_POSITION;
	centroid float3 pos3D : WORLDPOSITION;
	centroid float3 normal : NORMAL;

#ifdef DEBUG_CHARTS
	uint primitiveID : SV_PrimitiveID;
#endif // DEBUG_CHARTS
};

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

[earlydepthstencil]
float4 main(Input input) : SV_TARGET
{
#ifdef DEBUG_CHARTS
	return float4(random_color(input.primitiveID), 1);
#endif // DEBUG_CHARTS

	float2 uv = input.pos.xy * xTraceResolution_rcp;

	Surface surface;
	surface.init();
	surface.N = normalize(input.normal);

	RNG rng;
	rng.init((uint2)input.pos.xy, xTraceSampleIndex);
	LightmapQmcSampler qmc;
	qmc.init((uint2)input.pos.xy, xTraceSampleIndex);

	float3 P = input.pos3D;

	float bakerydebug = 0;
	BakeryPixelPush(P, surface.N, uv, rng, bakerydebug);
	
	RayDesc ray;
	ray.Origin = P + surface.N * 0.001;
	ray.Direction = normalize(lightmap_sample_hemisphere_cos(surface.N, qmc.sample4D(0).xy));
	ray.TMin = 0.001;
	ray.TMax = FLT_MAX;
	// A baked lightmap contains traced static lights and sky/environment only.
	// GetAmbientColor() is Wicked's runtime fallback for surfaces without baked
	// GI; seeding it here would inject the authored weather tint unoccluded into
	// every valid texel and produce a uniform color cast in the baked result.
	float3 result = 0;
	float3 energy = 1;
	surface.P = ray.Origin;
	// Bounce zero is incident irradiance for the original lightmapped surface;
	// its material response is applied later by the runtime lighting shader.
	result += EvaluateLightmapDirect(surface, 1, 0, qmc.sample4D(1), rng);

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
		if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
#else
		RayHit hit = TraceRay_Closest(ray, xTraceUserData.y, rng);

		if (hit.distance >= FLT_MAX - 1)
#endif // RTAPI

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
			return 0;

#else
		// ray origin updated for next bounce:
		ray.Origin = ray.Origin + ray.Direction * hit.distance;

		surface.SetBackface(hit.is_backface);

		if (!surface.load(hit.primitiveID, hit.bary))
			return 0;

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
			result += EvaluateLightmapDirect(
				surface, diffuse_throughput, bounce + 1u, light_sample, rng);
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

			// Add a new bounce iteration, otherwise the transparent effect can disappear:
			bounce--;
		}
		else
		{
			const float specular_chance = dot(surface.F, 0.333);
			if (bsdf_sample.y < specular_chance)
			{
				// Specular reflection
				ray.Direction = ReflectionDir_GGX(
					-ray.Direction,
					surface.N,
					surface.roughness,
					bsdf_sample.zw).xyz;
				energy *= surface.F / max(0.001, specular_chance) / max(0.001, 1 - surface.transmission);
			}
			else
			{
				// Diffuse reflection
				ray.Direction = lightmap_sample_hemisphere_cos(surface.N, bsdf_sample.zw);
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

	}

	//if(bakerydebug > 0)
	//	result = float3(1,0,0);
	
	result = max(0, result);
	if (any(isnan(result)) || any(isinf(result)))
	{
		// Alpha zero keeps an invalid path out of the running average and out
		// of the persisted coverage mask.
		return 0;
	}
	else
	{
		const float peak = max3(result);
		result *= min(1.0, LIGHTMAP_SAMPLE_RADIANCE_MAX / max(peak, 1e-4));
	}

	return float4(result, xTraceAccumulationFactor);
}
