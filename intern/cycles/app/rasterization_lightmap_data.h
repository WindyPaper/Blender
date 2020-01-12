#ifndef _RASTERIZATION_LIGHTMAP_DATA_
#define _RASTERIZATION_LIGHTMAP_DATA_

#pragma once

#include <vector>
#include <float.h>
#include <math.h>

CCL_NAMESPACE_BEGIN
class Mesh;
class float3;
class float2;
class BakeData;
CCL_NAMESPACE_END

struct lightmap_uv_differential
{
	float dudx, dudy, dvdx, dvdy;
};

class RasterizationLightmapData
{
public:
	RasterizationLightmapData();
	RasterizationLightmapData(int multi_sample_grid_resolution);
	~RasterizationLightmapData();

	void bake_differentials(const float* uv1, const float* uv2, const float* uv3, lightmap_uv_differential* out_uv_diff);

	void raster_triangle(const ccl::Mesh **mesh, const int mesh_num, const int img_w, const int img_h);

	void image_pixel_triangle_to_parameterization(const int img_w, const int img_h,
		const int prim, const lightmap_uv_differential *uv_diff,
		const ccl::float2 uv1, const ccl::float2 uv2, const ccl::float2 uv3);

	ccl::BakeData* get_bake_data() { return mp_baker_data; }

private:
	ccl::BakeData* mp_baker_data;
	int m_multi_sample_grid_resolution;
	std::vector<bool> m_bool_main_sample_pixels;
};

#endif
