#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

// The render target contains the mean of one independently scrambled Sobol
// replicate. Statistics are updated only at replicate boundaries; treating
// individual low-discrepancy samples as IID would underestimate error.
Texture2D<float4> lightmap_batch : register(t0);
RWTexture2D<float4> lightmap_statistics : register(u0);

static const uint LIGHTMAP_MIN_ADAPTIVE_BATCHES = 8; // 512 spp at 64/batch
static const float LIGHTMAP_ABSOLUTE_LOG_ERROR = 0.003;
static const float LIGHTMAP_RELATIVE_LOG_ERROR = 0.01;

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint width, height;
	lightmap_statistics.GetDimensions(width, height);
	if (DTid.x >= width || DTid.y >= height)
		return;

	const uint2 pixel = DTid.xy;
	const float4 batch = lightmap_batch[pixel];
	if (batch.a <= 0)
		return; // atlas padding or an invalid raster sample

	float4 state = lightmap_statistics[pixel];
	if (state.w > 0.5)
		return;

	const float luminance = max(0, dot(batch.rgb, float3(0.2126, 0.7152, 0.0722)));
	const float observation = log2(1.0 + luminance);
	const float count = state.z + 1.0;
	const float delta = observation - state.x;
	const float mean = state.x + delta / count;
	const float m2 = state.y + delta * (observation - mean);

	bool converged = false;
	if (count >= LIGHTMAP_MIN_ADAPTIVE_BATCHES && mean > 1e-6)
	{
		const float variance = m2 / max(count - 1.0, 1.0);
		const float standard_error = sqrt(max(variance, 0.0) / count);
		converged = standard_error <=
			LIGHTMAP_ABSOLUTE_LOG_ERROR + LIGHTMAP_RELATIVE_LOG_ERROR * abs(mean);
	}

	lightmap_statistics[pixel] = float4(mean, m2, count, converged ? 1.0 : 0.0);
}
