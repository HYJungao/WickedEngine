#include "globals.hlsli"

PUSHCONSTANT(push, LightmapPushConstants);

// Counter followed by compacted ShaderEntity indices. The object sphere test
// is deliberately conservative: false positives cost a few CDF operations,
// while false negatives would bias the bake.
RWBuffer<uint> lightmap_light_candidates : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const uint local_index = DTid.x;
	if (local_index >= lights().item_count())
		return;

	const uint entity_index = lights().first_item() + local_index;
	ShaderEntity light = load_entity(entity_index);
	ShaderMeshInstance instance = load_instance(push.instanceIndex);
	if ((light.layerMask & instance.layerMask) == 0)
		return;

	bool overlaps = light.GetType() == ENTITY_TYPE_DIRECTIONALLIGHT;
	if (!overlaps)
	{
		float emitter_extent = max(0, light.GetRadius());
		if (light.GetType() == ENTITY_TYPE_RECTLIGHT)
		{
			const float2 half_extent = float2(light.GetLength(), light.GetHeight()) * 0.5;
			emitter_extent = length(half_extent);
		}
		else
		{
			emitter_extent += max(0, light.GetLength()) * 0.5;
		}
		const float support_radius = max(0, light.GetRange()) +
			instance.radius + emitter_extent;
		const float3 delta = light.position - instance.center;
		overlaps = dot(delta, delta) <= support_radius * support_radius;
	}
	if (!overlaps)
		return;

	uint destination;
	InterlockedAdd(lightmap_light_candidates[0], 1u, destination);
	lightmap_light_candidates[1u + destination] = entity_index;
}
