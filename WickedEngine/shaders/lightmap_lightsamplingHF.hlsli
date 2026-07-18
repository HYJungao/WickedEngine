#ifndef WI_LIGHTMAP_LIGHTSAMPLING_HF
#define WI_LIGHTMAP_LIGHTSAMPLING_HF

// Analytic emitter sampling for the offline lightmap integrator.  Wicked's
// authored light color is treated as total diffuse strength.  Finite source
// shapes define a normalized directional distribution, so changing radius
// changes penumbra width without accidentally changing total energy.

struct LightmapLightSample
{
	float3 direction;
	float distance;
	float3 energy_over_shape_pdf;
	float shape_pdf;
	uint entity_index;
	bool valid;
	bool delta;
	bool supports_mis;
};

float LightmapPowerHeuristic(float pdf_a, float pdf_b)
{
	const float a2 = pdf_a * pdf_a;
	const float b2 = pdf_b * pdf_b;
	return a2 / max(a2 + b2, 1e-20);
}

float3 LightmapSampleUniformCone(float3 axis, float cos_theta_max, float2 u)
{
	const float cos_theta = lerp(1.0, cos_theta_max, u.x);
	const float sin_theta = sqrt(saturate(1.0 - cos_theta * cos_theta));
	const float phi = 2.0 * PI * u.y;
	const float3 local = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
	return normalize(mul(local, get_tangentspace(normalize(axis))));
}

float LightmapUniformConePdf(float cos_theta_max)
{
	return rcp(max(2.0 * PI * (1.0 - cos_theta_max), 1e-12));
}

float LightmapRaySphereDistance(float3 origin, float3 direction, float3 center, float radius)
{
	const float3 oc = origin - center;
	const float b = dot(oc, direction);
	const float c = dot(oc, oc) - radius * radius;
	const float h = b * b - c;
	if (h <= 0)
		return FLT_MAX;
	const float root = sqrt(h);
	const float near_t = -b - root;
	const float far_t = -b + root;
	return near_t > 0.001 ? near_t : (far_t > 0.001 ? far_t : FLT_MAX);
}

float EstimateLightmapEmitter(ShaderEntity light, float3 P, float3 N)
{
	const float power = max3(max(0, light.GetColor().rgb));
	if (power <= 0)
		return 0;

	switch (light.GetType())
	{
	case ENTITY_TYPE_DIRECTIONALLIGHT:
		// A conservative upper bound keeps every direction in a finite sun disk
		// reachable even when its center is just below the tangent plane.
		return power;
	case ENTITY_TYPE_POINTLIGHT:
	case ENTITY_TYPE_SPOTLIGHT:
	{
		const float3 delta = light.position - P;
		const float dist2 = dot(delta, delta);
		const float range = light.GetRange();
		if (dist2 <= 1e-8 || dist2 >= range * range)
			return 0;
		float estimate = power * attenuation_pointlight(dist2, range, light.GetRange2Rcp());
		if (light.GetType() == ENTITY_TYPE_SPOTLIGHT)
		{
			const float spot_factor = dot(normalize(delta), light.GetDirection());
			estimate *= attenuation_spotlight(
				dist2, range, light.GetRange2Rcp(), spot_factor,
				light.GetAngleScale(), light.GetAngleOffset()) /
				max(attenuation_pointlight(dist2, range, light.GetRange2Rcp()), 1e-12);
		}
		return max(estimate, 0);
	}
	case ENTITY_TYPE_RECTLIGHT:
	{
		const half4 quaternion = light.GetQuaternion();
		const float3 right = rotate_vector(half3(1, 0, 0), quaternion);
		const float3 up = rotate_vector(half3(0, 1, 0), quaternion);
		const float3 forward = cross(up, right);
		if (dot(P - light.position, forward) <= 0)
			return 0;
		const float3 delta = light.position - P;
		const float dist2 = dot(delta, delta);
		const float range = light.GetRange();
		if (dist2 <= 1e-8 || dist2 >= range * range)
			return 0;
		const float area = max(light.GetLength() * light.GetHeight(), 1e-8);
		return power * attenuation_pointlight(dist2, range, light.GetRange2Rcp()) * area * PI;
	}
	}
	return 0;
}

LightmapLightSample SampleLightmapEmitter(
	ShaderEntity light,
	uint entity_index,
	float3 P,
	float3 N,
	float3 sample_value)
{
	LightmapLightSample sample = (LightmapLightSample)0;
	sample.entity_index = entity_index;
	sample.distance = FLT_MAX;
	sample.supports_mis = true;

	switch (light.GetType())
	{
	case ENTITY_TYPE_DIRECTIONALLIGHT:
	{
		const float3 axis = normalize(light.GetDirection().xyz);
		const float angular_radius = atan(max(light.GetRadius(), 0.0));
		if (angular_radius <= 1e-5)
		{
			sample.direction = axis;
			sample.shape_pdf = 1;
			sample.delta = true;
		}
		else
		{
			const float cos_theta_max = cos(min(angular_radius, PI * 0.499));
			sample.direction = LightmapSampleUniformCone(axis, cos_theta_max, sample_value.xy);
			sample.shape_pdf = LightmapUniformConePdf(cos_theta_max);
		}
		float3 transmittance = 1;
		if (GetFrame().options & OPTION_BIT_REALISTIC_SKY)
		{
			transmittance = GetAtmosphericLightTransmittance(
				GetWeather().atmosphere, P, sample.direction, texture_transmittancelut);
		}
		sample.energy_over_shape_pdf = light.GetColor().rgb * transmittance;
		sample.valid = any(sample.energy_over_shape_pdf > 0);
	}
	break;
	case ENTITY_TYPE_POINTLIGHT:
	case ENTITY_TYPE_SPOTLIGHT:
	{
		float3 center = light.position;
		if (light.GetLength() > 0)
		{
			center += light.GetDirection() * (sample_value.x - 0.5) * light.GetLength();
			sample.supports_mis = false; // conditional line/sphere mixture; NEE remains exact
		}
		const float3 center_delta = center - P;
		const float center_dist2 = dot(center_delta, center_delta);
		const float range = light.GetRange();
		if (center_dist2 <= 1e-8 || center_dist2 >= range * range)
			break;
		const float center_distance = sqrt(center_dist2);
		const float radius = min(max(light.GetRadius(), 0.0), center_distance * 0.995);
		if (radius <= 1e-5)
		{
			sample.direction = center_delta / center_distance;
			sample.distance = center_distance;
			sample.shape_pdf = 1;
			sample.delta = true;
		}
		else
		{
			const float sin2_theta_max = saturate(radius * radius / center_dist2);
			const float cos_theta_max = sqrt(saturate(1.0 - sin2_theta_max));
			sample.direction = LightmapSampleUniformCone(
				center_delta / center_distance, cos_theta_max, sample_value.yz);
			sample.shape_pdf = LightmapUniformConePdf(cos_theta_max);
			const float cos_theta = dot(sample.direction, center_delta / center_distance);
			const float perpendicular2 = center_dist2 * (1.0 - cos_theta * cos_theta);
			sample.distance = center_distance * cos_theta - sqrt(max(radius * radius - perpendicular2, 0.0));
		}
		float attenuation = attenuation_pointlight(center_dist2, range, light.GetRange2Rcp());
		if (light.GetType() == ENTITY_TYPE_SPOTLIGHT)
		{
			const float spot_factor = dot(normalize(light.position - P), light.GetDirection());
			attenuation = attenuation_spotlight(
				center_dist2, range, light.GetRange2Rcp(), spot_factor,
				light.GetAngleScale(), light.GetAngleOffset());
		}
		sample.energy_over_shape_pdf = light.GetColor().rgb * attenuation;
		sample.valid = sample.distance > 0 && any(sample.energy_over_shape_pdf > 0);
	}
	break;
	case ENTITY_TYPE_RECTLIGHT:
	{
		const half4 quaternion = light.GetQuaternion();
		const float3 right = rotate_vector(half3(1, 0, 0), quaternion);
		const float3 up = rotate_vector(half3(0, 1, 0), quaternion);
		const float3 forward = cross(up, right);
		if (dot(P - light.position, forward) <= 0)
			break;
		const float width = max(light.GetLength(), 1e-4);
		const float height = max(light.GetHeight(), 1e-4);
		const float area = width * height;
		const float3 light_point = light.position +
			right * (sample_value.x - 0.5) * width +
			up * (sample_value.y - 0.5) * height;
		const float3 delta = light_point - P;
		const float dist2 = dot(delta, delta);
		const float range = light.GetRange();
		if (dist2 <= 1e-8 || dist2 >= range * range)
			break;
		sample.distance = sqrt(dist2);
		sample.direction = delta / sample.distance;
		const float cos_light = saturate(dot(-sample.direction, forward));
		if (cos_light <= 1e-6)
			break;
		sample.shape_pdf = dist2 / max(cos_light * area, 1e-12);
		// Preserve Wicked's authored rect-light convention: total diffuse
		// strength grows with emitting area while the sample distribution is
		// normalized over that area.
		sample.energy_over_shape_pdf = light.GetColor().rgb *
			attenuation_pointlight(dist2, range, light.GetRange2Rcp()) * area * PI;
		sample.valid = any(sample.energy_over_shape_pdf > 0);
	}
	break;
	}

	return sample;
}

#endif // WI_LIGHTMAP_LIGHTSAMPLING_HF
