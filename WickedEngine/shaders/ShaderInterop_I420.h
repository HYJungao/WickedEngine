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
    uint available_mask;
    uint4 tile_rects[4];
};

#endif // WI_SHADERINTEROP_I420_H
