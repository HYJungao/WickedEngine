#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

PUSHCONSTANT(postprocess, PostProcess);

Texture2D<uint4> input : register(t0);

RWTexture2DArray<unorm float> output : register(u0);

float load_shadow(in uint shadow_index, in uint4 shadow_mask)
{
	uint mask_shift = (shadow_index % 4) * 8;
	uint mask_bucket = shadow_index / 4;
	uint mask = (shadow_mask[mask_bucket] >> mask_shift) & 0xFF;
	return mask / 255.0;
}

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
	ShaderCamera camera = GetCamera();
	uint2 pixel = DTid.xy;
	const float2 uv = (pixel + 0.5f) * postprocess.resolution_rcp;

	uint2 dim;
	uint MAX_RTSHADOWS;
	output.GetDimensions(dim.x, dim.y, MAX_RTSHADOWS);
	
	const uint2 tileIndex = uint2(floor(pixel / TILED_CULLING_BLOCKSIZE));
	const uint flatTileIndex = flatten2D(tileIndex, camera.entity_culling_tilecount.xy) * SHADER_ENTITY_TILE_BUCKET_COUNT;
	
	const float2 lowres_size = postprocess.params1.xy;
	const float2 lowres_texel_size = postprocess.params1.zw;
	
	float2 sam_pixel = uv * lowres_size + (-0.5 + 1.0 / 512.0); // (1.0 / 512.0) correction is described here: https://www.reedbeta.com/blog/texture-gathers-and-coordinate-precision/
	float2 sam_pixel_frac = frac(sam_pixel);

	const uint downscale = max(1u, (uint)round((float)dim.x / lowres_size.x));
	const uint depth_mip = firstbithigh(downscale);
	const uint2 lowres_max = uint2(lowres_size) - 1;
	// Gather pattern. At full resolution all four taps intentionally collapse
	// to the exact source texel, avoiding the old hard-coded /2 offset.
	const uint2 base_pixel = min(DTid.xy / downscale, lowres_max);
	uint2 pixel0 = min(base_pixel + uint2(0, downscale > 1 ? 1 : 0), lowres_max);
	uint2 pixel1 = min(base_pixel + uint2(downscale > 1 ? 1 : 0, downscale > 1 ? 1 : 0), lowres_max);
	uint2 pixel2 = base_pixel;
	uint2 pixel3 = min(base_pixel + uint2(downscale > 1 ? 1 : 0, 0), lowres_max);
	uint4 shadow_mask0 = input[pixel0];
	uint4 shadow_mask1 = input[pixel1];
	uint4 shadow_mask2 = input[pixel2];
	uint4 shadow_mask3 = input[pixel3];
	float lineardepth0 = compute_lineardepth(texture_depth.Load(uint3(pixel0, depth_mip)));
	float lineardepth1 = compute_lineardepth(texture_depth.Load(uint3(pixel1, depth_mip)));
	float lineardepth2 = compute_lineardepth(texture_depth.Load(uint3(pixel2, depth_mip)));
	float lineardepth3 = compute_lineardepth(texture_depth.Load(uint3(pixel3, depth_mip)));
	float lineardepth_highres = compute_lineardepth(texture_depth[pixel]);

	float threshold = 2;
	float4 weights = max(0.001, 1 - saturate(abs(float4(lineardepth0, lineardepth1, lineardepth2, lineardepth3) - lineardepth_highres) * threshold));
	float weights_norm = rcp(bilinear(weights, sam_pixel_frac));
	
	uint shadow_index = 0;
	
	[branch]
	if (!lights().empty())
	{
		// Loop through light buckets in the tile:
		ShaderEntityIterator iterator = lights();
		for (uint bucket = iterator.first_bucket(); bucket <= iterator.last_bucket(); ++bucket)
		{
			uint bucket_bits = load_entitytile(camera, flatTileIndex + bucket);
			bucket_bits = iterator.mask_entity(bucket, bucket_bits);

			// Bucket scalarizer - Siggraph 2017 - Improved Culling [Michal Drobot]:
			bucket_bits = WaveReadLaneFirst(WaveActiveBitOr(bucket_bits));

			[loop]
			while (bucket_bits != 0)
			{
				// Retrieve global entity index from local bucket, then remove bit from local bucket:
				const uint bucket_bit_index = firstbitlow(bucket_bits);
				bucket_bits ^= 1u << bucket_bit_index;

				const uint entity_index = bucket * 32 + bucket_bit_index;
				shadow_index = entity_index - lights().first_item();
				if (shadow_index >= MAX_RTSHADOWS)
					break;

				ShaderEntity light = load_entity(entity_index);

				if (!light.IsCastingShadow())
				{
					continue;
				}

				if (light.IsStaticLight())
				{
					continue; // static lights will be skipped (they are used in lightmap baking)
				}
					
				float shadow0 = load_shadow(shadow_index, shadow_mask0);
				float shadow1 = load_shadow(shadow_index, shadow_mask1);
				float shadow2 = load_shadow(shadow_index, shadow_mask2);
				float shadow3 = load_shadow(shadow_index, shadow_mask3);

				float shadow = bilinear(float4(shadow0,shadow1,shadow2,shadow3) * weights, sam_pixel_frac);
				shadow *= weights_norm;
		
				output[uint3(pixel, shadow_index)] = shadow;
			}
		}
	}
}
