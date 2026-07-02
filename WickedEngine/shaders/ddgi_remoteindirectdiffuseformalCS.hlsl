#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"
#include "ShaderInterop_DDGI.h"

PUSHCONSTANT(postprocess, PostProcess);

RWTexture2D<float4> output_remote_indirect_diffuse_formal : register(u0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= postprocess.resolution.x || DTid.y >= postprocess.resolution.y)
	{
		return;
	}

	float3 remote_indirect_diffuse_formal = 0;

	if (GetScene().ddgi.probe_buffer >= 0)
	{
		const float2 uv = ((float2)DTid.xy + 0.5) * postprocess.resolution_rcp;
		const float depth = texture_depth.SampleLevel(sampler_linear_clamp, uv, 0);

		if (depth > 0)
		{
			const float3 P = reconstruct_position(uv, depth);
			const half3 N = decode_normal(texture_normal_roughness[DTid.xy].rg);
			half3 dominant_lightdir = 0;
			half3 dominant_lightcolor = 0;

			// Wicked's DDGI shading helper returns the diffuse term with the Lambert PI divide included.
			// This formal buffer is material-decoupled irradiance, consumed as (BaseColor / PI) * buffer * AO.
			remote_indirect_diffuse_formal = ddgi_sample_irradiance(P, N, dominant_lightdir, dominant_lightcolor) * PI;
		}
	}

	output_remote_indirect_diffuse_formal[DTid.xy] = float4(remote_indirect_diffuse_formal, 1);
}
