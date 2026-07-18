#ifndef WI_LIGHTMAP_SAMPLING_HF
#define WI_LIGHTMAP_SAMPLING_HF

// Progressive four-dimensional Sobol sequence used only by the offline
// lightmap integrator. The full 32-bit direction table avoids truncating an
// Owen-scrambled index before it is evaluated.
//
// Every texel and logical dimension group receives an independent nested
// scramble. Unlike a digital shift, this also breaks the strong correlation
// that would result from reusing the same four Sobol dimensions at every path
// event. Callers use fixed group numbers so branch decisions cannot shift all
// subsequent random dimensions.
static const uint4 lightmap_sobol_directions[32] =
{
	uint4(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
	uint4(0x40000000u, 0xC0000000u, 0xC0000000u, 0xC0000000u),
	uint4(0x20000000u, 0xA0000000u, 0x60000000u, 0x20000000u),
	uint4(0x10000000u, 0xF0000000u, 0x90000000u, 0x50000000u),
	uint4(0x08000000u, 0x88000000u, 0xE8000000u, 0xF8000000u),
	uint4(0x04000000u, 0xCC000000u, 0x5C000000u, 0x74000000u),
	uint4(0x02000000u, 0xAA000000u, 0x8E000000u, 0xA2000000u),
	uint4(0x01000000u, 0xFF000000u, 0xC5000000u, 0x93000000u),
	uint4(0x00800000u, 0x80800000u, 0x68800000u, 0xD8800000u),
	uint4(0x00400000u, 0xC0C00000u, 0x9CC00000u, 0x25400000u),
	uint4(0x00200000u, 0xA0A00000u, 0xEE600000u, 0x59E00000u),
	uint4(0x00100000u, 0xF0F00000u, 0x55900000u, 0xE6D00000u),
	uint4(0x00080000u, 0x88880000u, 0x80680000u, 0x78080000u),
	uint4(0x00040000u, 0xCCCC0000u, 0xC09C0000u, 0xB40C0000u),
	uint4(0x00020000u, 0xAAAA0000u, 0x60EE0000u, 0x82020000u),
	uint4(0x00010000u, 0xFFFF0000u, 0x90550000u, 0xC3050000u),
	uint4(0x00008000u, 0x80008000u, 0xE8808000u, 0x208F8000u),
	uint4(0x00004000u, 0xC000C000u, 0x5CC0C000u, 0x51474000u),
	uint4(0x00002000u, 0xA000A000u, 0x8E606000u, 0xFBEA2000u),
	uint4(0x00001000u, 0xF000F000u, 0xC5909000u, 0x75D93000u),
	uint4(0x00000800u, 0x88008800u, 0x6868E800u, 0xA0858800u),
	uint4(0x00000400u, 0xCC00CC00u, 0x9C9C5C00u, 0x914E5400u),
	uint4(0x00000200u, 0xAA00AA00u, 0xEEEE8E00u, 0xDBE79E00u),
	uint4(0x00000100u, 0xFF00FF00u, 0x5555C500u, 0x25DB6D00u),
	uint4(0x00000080u, 0x80808080u, 0x8000E880u, 0x58800080u),
	uint4(0x00000040u, 0xC0C0C0C0u, 0xC0005CC0u, 0xE54000C0u),
	uint4(0x00000020u, 0xA0A0A0A0u, 0x60008E60u, 0x79E00020u),
	uint4(0x00000010u, 0xF0F0F0F0u, 0x9000C590u, 0xB6D00050u),
	uint4(0x00000008u, 0x88888888u, 0xE8006868u, 0x800800F8u),
	uint4(0x00000004u, 0xCCCCCCCCu, 0x5C009C9Cu, 0xC00C0074u),
	uint4(0x00000002u, 0xAAAAAAAAu, 0x8E00EEEEu, 0x200200A2u),
	uint4(0x00000001u, 0xFFFFFFFFu, 0xC5005555u, 0x50050093u),
};

uint lightmap_hash(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	value ^= value >> 16u;
	return value;
}

uint4 lightmap_sobol4(uint sample_index)
{
	const uint gray_code = sample_index ^ (sample_index >> 1u);
	uint4 value = 0;
	[unroll]
	for (uint bit = 0; bit < 32; ++bit)
	{
		if ((gray_code & (1u << bit)) != 0)
			value ^= lightmap_sobol_directions[bit];
	}
	return value;
}

// Approximate nested uniform scrambling (Laine-Karras permutation). Reversing
// before and after the integer permutation makes the hierarchy operate from
// the most significant sample bit to the least significant one.
uint lightmap_nested_uniform_scramble(uint value, uint seed)
{
	value = reversebits(value);
	value += seed;
	value ^= value * 0x6c50b47cu;
	value ^= value * 0xb82f1e52u;
	value ^= value * 0xc7afe638u;
	value ^= value * 0x8d22f6e6u;
	return reversebits(value);
}

struct LightmapQmcSampler
{
	uint2 pixel;
	uint sample_index;
	uint batch_index;

	void init(uint2 pixel_id, uint index, uint batch_size)
	{
		pixel = pixel_id;
		batch_size = max(batch_size, 1u);
		batch_index = index / batch_size;
		sample_index = index % batch_size;
	}

	float4 sample4D(uint dimension_group)
	{
		const uint seed = lightmap_hash(
			pixel.x * 0x9e3779b9u ^
			pixel.y * 0x85ebca6bu ^
			dimension_group * 0xc2b2ae35u ^
			batch_index * 0x27d4eb2du);
		const uint scrambled_index = lightmap_nested_uniform_scramble(
			sample_index, lightmap_hash(seed ^ 0x243f6a88u));
		uint4 bits = lightmap_sobol4(scrambled_index);
		bits.x = lightmap_nested_uniform_scramble(bits.x, lightmap_hash(seed ^ 0x68bc21ebu));
		bits.y = lightmap_nested_uniform_scramble(bits.y, lightmap_hash(seed ^ 0x02e5be93u));
		bits.z = lightmap_nested_uniform_scramble(bits.z, lightmap_hash(seed ^ 0x967a889bu));
		bits.w = lightmap_nested_uniform_scramble(bits.w, lightmap_hash(seed ^ 0x4b1d2f41u));
		// Use the upper 24 bits so integer-to-float conversion is exact and the
		// result remains strictly inside [0, 1).
		return float4(bits >> 8u) * 5.960464477539063e-8;
	}
};

float3 lightmap_sample_hemisphere_cos(float3 normal, float2 sample_value)
{
	return mul(hemispherepoint_cos(sample_value.x, sample_value.y), get_tangentspace(normal));
}

#endif // WI_LIGHTMAP_SAMPLING_HF
