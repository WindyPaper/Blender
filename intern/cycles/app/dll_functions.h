#pragma once
#ifndef _DLL_FUNCTIONS_H_

#include "cycles_standalone.h"
#include "render/session.h"


#define DLL_EXPORT __declspec(dllexport)

extern "C"
{
	struct UnityRenderOptions
	{
		int width;
		int height;
		float* camera_pos;
		float* euler_angle;

		int sample_count;
	};

	struct CyclesInitOptions
	{
		int width;
		int height;

		int sample_count;

		char device_working_folder[255];
		int render_device;

		int work_type; //RENDER / BAKDER
    int enable_denoise;
	};

	struct CyclesMeshData
	{
		float* vertex_array;
		float* uvs_array;
		float* lightmapuvs_array;
		float* normal_array;
		int vertex_num;
		int* index_array;
		int* mat_index;
		int triangle_num;
		int mtl_num;
	};

	struct CyclesMtlData
	{
		char mat_name[255];
		char diffuse_tex_name[255];
		char mtl_tex_name[255];
		char normal_tex_name[255];

		int is_transparent;
		float tiling_x, tiling_y;
		float offset_x, offset_y;
		float* diffuse_color; //float3
	};

	DLL_EXPORT bool init_cycles(CyclesInitOptions init_op);

	DLL_EXPORT int unity_add_mesh(CyclesMeshData mesh_data, CyclesMtlData *mtls);

	DLL_EXPORT int unity_add_light(const char* name, float intensity, float radius, float* color, float* dir, float* pos, int type);

	DLL_EXPORT int bake_lightmap();

	//typedef void (*render_image_cb)(const char* data, const int w, const int h, const int data_type);

	DLL_EXPORT int interactive_pt_rendering(UnityRenderOptions u3d_render_options, ccl::Session::render_image_cb icb);

	DLL_EXPORT int release_cycles();
}



#endif

