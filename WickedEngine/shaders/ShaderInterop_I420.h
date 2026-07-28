#ifndef WI_SHADERINTEROP_I420_H
#define WI_SHADERINTEROP_I420_H
#include "ShaderInterop.h"

struct I420AtlasPackPush
{
    uint2 video_resolution;
    uint metadata_rows;
    uint y_stride;
    uint uv_stride;
    uint u_offset;
    uint v_offset;
    // Keep this 48-byte prefix compatible with already deployed pack shaders.
    // Current shaders also verify the explicit ABI tail before writing output.
    uint available_mask;
    uint tile_padding;
    uint abi_version;
    uint struct_size;
    uint padding;
};

#endif // WI_SHADERINTEROP_I420_H
