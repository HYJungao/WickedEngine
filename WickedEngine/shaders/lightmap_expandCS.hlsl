#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

Texture2D lightmap_input : register(t0);

RWTexture2D<float4> lightmap_output : register(u0);

static const int2 offsets[] = {
	int2(0, -1),
	int2(0, 1),
	int2(-1, 0),
	int2(1, 0),
	
	int2(-1, -1),
	int2(1, -1),
	int2(1, 1),
	int2(-1, 1),
};

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const uint2 pixel = DTid.xy;
	uint input_width, input_height;
	uint output_width, output_height;
	lightmap_input.GetDimensions(input_width, input_height);
	lightmap_output.GetDimensions(output_width, output_height);
	if (pixel.x >= output_width || pixel.y >= output_height)
		return;
	float4 color = lightmap_input[pixel];

	for (uint i = 0; (i < arraysize(offsets)) && (color.a < 1); ++i)
	{
		const int2 candidate = int2(pixel) + offsets[i];
		if (candidate.x >= 0 && candidate.y >= 0 &&
			candidate.x < int(input_width) && candidate.y < int(input_height))
		{
			const float4 neighbor = lightmap_input[candidate];
			if (neighbor.a >= 1)
				color = neighbor;
		}
	}
	
	lightmap_output[pixel] = color;
}
