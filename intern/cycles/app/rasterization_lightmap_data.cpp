#include "rasterization_lightmap_data.h"
#include "render/bake.h"

#include "render/mesh.h"

RasterizationLightmapData::RasterizationLightmapData() :
	mp_baker_data(nullptr)
{

}

RasterizationLightmapData::~RasterizationLightmapData()
{
	if (mp_baker_data)
	{
		delete mp_baker_data;
		mp_baker_data = NULL;
	}
}

void RasterizationLightmapData::bake_differentials(const float* uv1, const float* uv2, const float* uv3, lightmap_uv_differential* out_uv_diff)
{
	float A;

	/* assumes dPdu = P1 - P3 and dPdv = P2 - P3 */
	A = (uv2[0] - uv1[0]) * (uv3[1] - uv1[1]) - (uv3[0] - uv1[0]) * (uv2[1] - uv1[1]);

	if (fabsf(A) > FLT_EPSILON) {
		A = 0.5f / A;

		out_uv_diff->dudx = (uv2[1] - uv3[1]) * A;
		out_uv_diff->dvdx = (uv3[1] - uv1[1]) * A;

		out_uv_diff->dudy = (uv3[0] - uv2[0]) * A;
		out_uv_diff->dvdy = (uv1[0] - uv3[0]) * A;
	}
	else {
		out_uv_diff->dudx = out_uv_diff->dudy = 0.0f;
		out_uv_diff->dvdx = out_uv_diff->dvdy = 0.0f;
	}
}

//float dot(ccl::float2 a, ccl::float2 b)
//{
//	return a[0] * b[0] + a[1] * b[1];
//}

void lm_toBarycentric(
	const ccl::float2 p1, const ccl::float2 p2, const ccl::float2 p3, const ccl::float2 p, ccl::float2 &out_uv)
{
	// http://www.blackpawn.com/texts/pointinpoly/
	// Compute vectors
	ccl::float2 v0 = p1 - p3;

	ccl::float2 v1 = p2 - p3;
	ccl::float2 v2 = p - p3;

	// Compute dot products
	float dot00 = dot(v0, v0);
	float dot01 = dot(v0, v1);
	float dot02 = dot(v0, v2);
	float dot11 = dot(v1, v1);
	float dot12 = dot(v1, v2);
	// Compute barycentric coordinates
	float invDenom = 1.0f / (dot00 * dot11 - dot01 * dot01);
	out_uv[0] = (dot11 * dot02 - dot01 * dot12) * invDenom;
	out_uv[1] = (dot00 * dot12 - dot01 * dot02) * invDenom;
}

int orient2d(const ccl::float2 a, const ccl::float2 b, const ccl::float2 c)
{
	float ret = ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
	return (int)(ret);
}

bool PointInTriangle(ccl::float3 A, ccl::float3 B, ccl::float3 C, ccl::float3 P)
{
	// Prepare our barycentric variables
	ccl::float3 u = B - A;
	ccl::float3 v = C - A;
	ccl::float3 w = P - A;

	ccl::float3 vCrossW = ccl::cross(v, w);
	ccl::float3 vCrossU = ccl::cross(v, u);

	// Test sign of r
	if (ccl::dot(vCrossW, vCrossU) < 0)
		return false;

	ccl::float3 uCrossW = ccl::cross(u, w);
	ccl::float3 uCrossV = ccl::cross(u, v);

	// Test sign of t
	if (dot(uCrossW, uCrossV) < 0)
		return false;

	// At this point, we know that r and t and both > 0.
	// Therefore, as long as their sum is <= 1, each must be less <= 1
	float denom = len(uCrossV);
	float r = len(vCrossW) / denom;
	float t = len(uCrossW) / denom;

	return (r + t <= 1);
}

bool is_top_left(const ccl::float2 v0, const ccl::float2 v1)
{
	float y_offset = (v1[1] - v0[1]);
	float abs_y_offset = fabsf(y_offset);

	const float eps = 0.0002f;

	if (abs_y_offset < eps && (v1[0] - v0[0] < eps)) {
		return true;
	}

	if (y_offset < eps)
	{
		return true;
	}

	return false;
}

void RasterizationLightmapData::image_pixel_triangle_to_parameterization(const int img_w, const int img_h, const int prim, const lightmap_uv_differential* uv_diff, const ccl::float2 uv1, const ccl::float2 uv2, const ccl::float2 uv3)
{
	ccl::float2 max_uv = ccl::max(ccl::max(uv1, uv2), uv3);
	ccl::float2 min_uv = ccl::min(ccl::min(uv1, uv2), uv3);

	int bias_v1_v2 = is_top_left(uv1, uv2) ? 0 : -1;
	int bias_v2_v3 = is_top_left(uv2, uv3) ? 0 : -1;
	int bias_v3_v1 = is_top_left(uv3, uv1) ? 0 : -1;

	for (int y = min_uv.y; y < max_uv.y; ++y)
	{
		for (int x = min_uv.x; x < max_uv.x; ++x)
		{
			ccl::float2 curr_pixel = ccl::make_float2(x, y);

			/*int w0 = orient2d(uv2, uv3, curr_pixel) + bias_v2_v3;
			int w1 = orient2d(uv3, uv1, curr_pixel) + bias_v3_v1;
			int w2 = orient2d(uv1, uv2, curr_pixel) + bias_v1_v2;

			const int eps = 0;

			if (w0 >= eps && w1 >= eps && w2 >= eps) {
				ccl::float2 out_uv;
				lm_toBarycentric(uv1, uv2, uv3, curr_pixel, out_uv);

				int pixel_index = img_w * y + x;
				mp_baker_data->set(pixel_index, prim, &out_uv[0], uv_diff->dudx, uv_diff->dudy, uv_diff->dvdx, uv_diff->dvdy);
			}
			else
			{
				int k = 0;
			}*/

			if (PointInTriangle(ccl::make_float3(uv1.x, uv1.y, 0.0f), ccl::make_float3(uv2.x, uv2.y, 0.0f), ccl::make_float3(uv3.x, uv3.y, 0.0f), ccl::make_float3(curr_pixel.x, curr_pixel.y, 0.0f)))
			{
				ccl::float2 out_uv;
				lm_toBarycentric(uv1, uv2, uv3, curr_pixel, out_uv);

				int pixel_index = img_w * y + x;
				mp_baker_data->set(pixel_index, prim, &out_uv[0], uv_diff->dudx, uv_diff->dudy, uv_diff->dvdx, uv_diff->dvdy);
			}
		}
	}
}

void RasterizationLightmapData::raster_triangle(const ccl::Mesh **mesh, const int mesh_num, const int img_w, const int img_h)
{
	int pixel_num = img_w * img_h;

	if (mp_baker_data == nullptr)
	{
		//hard code 0		
		mp_baker_data = new ccl::BakeData(0, 0, pixel_num);
	}

	//init
	float zero_v2[2] = { 0.0f, 0.0f };
	for (int i = 0; i < pixel_num; ++i)
	{
		mp_baker_data->set(i, -1, zero_v2, 0.0f, 0.0f, 0.0f, 0.0f);
	}
	
	for (int mesh_i = 0; mesh_i < mesh_num; ++mesh_i)
	{
		int tri_num = mesh[mesh_i]->num_triangles();

		const ccl::Attribute* lightmap_uv = mesh[mesh_i]->attributes.find(OIIO::ustring("lightmap_uv"));
		const ccl::float3* uv_data = lightmap_uv->data_float3();
		for (int i = 0; i < tri_num; ++i)
		{
			ccl::Mesh::Triangle tri = mesh[mesh_i]->get_triangle(i);

			lightmap_uv_differential out_uv_diff;
			ccl::float2 uvs[3];

			for (int t = 0; t < 3; ++t)
			{
				uvs[t].x = uv_data[i * 3 + t].x * (float)img_w - (0.5f + 0.001f);
				uvs[t].y = uv_data[i * 3 + t].y * (float)img_h - (0.5f + 0.001f);
			}

			bake_differentials((float*)& uvs[0], (float*)& uvs[1], (float*)& uvs[2], &out_uv_diff);

			image_pixel_triangle_to_parameterization(img_w, img_h, i, &out_uv_diff, uvs[0], uvs[1], uvs[2]);
		}
	}
}