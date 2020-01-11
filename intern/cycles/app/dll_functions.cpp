#include "dll_functions.h"
#include "cycles_standalone.h"
#include "util/util_logging.h"
#include "util/util_math_float3.h"
#include "util/util_math_float4.h"
#include "render/mesh.h"
#include "render/graph.h"
#include "render/nodes.h"
#include "render/scene.h"
#include "render/camera.h"
#include "render/light.h"
#include "util/util_path.h"

//CCL_NAMESPACE_BEGIN

#define RAD2DEGF(_rad) ((_rad) * (float)(180.0 / M_PI))
#define DEG2RADF(_deg) ((_deg) * (float)(M_PI / 180.0))

using namespace ccl;

void assign_session_specific(const int w, const int h, const int render_device, const std::string& device_working_folder)
{
	options.filepath = "";
	options.session = NULL;
	options.quiet = false;


	enum RenderDeviceOptions
	{
		CUDA = 0,
		CPU = 1
	};

	/* device names */
	string device_names = "";
	string select_device_name;
	switch (render_device)
	{
	case CUDA:
		select_device_name = "CUDA";
		break;
	case CPU:
		select_device_name = "CPU";
		break;
	}

	bool list = false;

	vector<DeviceType>& types = Device::available_types();

	/* TODO(sergey): Here's a feedback loop happens: on the one hand we want
	 * the device list to be printed in help message, on the other hand logging
	 * is not initialized yet so we wouldn't have debug log happening in the
	 * device initialization.
	 */
	foreach(DeviceType type, types) {
		if (device_names != "")
			device_names += ", ";

		device_names += Device::string_from_type(type);
	}

	/* shading system */
	string ssname = "svm";


	options.session_params.background = false; //only for CPU interactive rendering

	options.quiet = false;
	options.session_params.samples = 4;
	//options.output_path = "./unity_dll_test_image/";
	options.output_path = "./Assets/out_render_image.tga";
	options.width = w;
	options.height = h;
	options.session_params.tile_size.x = 1024;
	options.session_params.tile_size.y = 1025;
	bool debug = true;

	if (debug) {
		util_logging_start();
		util_logging_verbosity_set(1);
	}

	if (ssname == "osl")
		options.scene_params.shadingsystem = SHADINGSYSTEM_OSL;
	else if (ssname == "svm")
		options.scene_params.shadingsystem = SHADINGSYSTEM_SVM;

	/* Use progressive rendering */
	options.session_params.progressive = true;

	/* find matching device */
	DeviceType device_type = Device::type_from_string(select_device_name.c_str());
	vector<DeviceInfo>& devices = Device::available_devices();
	bool device_available = false;

	foreach(DeviceInfo & device, devices) {
		if (device_type == device.type) {
			options.session_params.device = device;
			device_available = true;
			break;
		}
	}

	/* handle invalid configurations */
	if (options.session_params.device.type == DEVICE_NONE || !device_available) {
		fprintf(stderr, "Unknown device: %s\n", select_device_name.c_str());
		//exit(EXIT_FAILURE);
	}
	else if (options.session_params.samples < 0) {
		fprintf(stderr, "Invalid number of samples: %d\n", options.session_params.samples);
		//exit(EXIT_FAILURE);
	}
	else if (options.filepath == "") {
		fprintf(stderr, "No file path specified\n");
		//exit(EXIT_FAILURE);
	}

	/* For smoother Viewport */
	options.session_params.start_resolution = 64;
}

static void unity_session_init()
{
	options.session_params.write_render_cb = write_render;
	options.session = new Session(options.session_params);

//	if (options.session_params.background && !options.quiet)
//		options.session->progress.set_update_callback(function_bind(&session_print_status));
//#ifdef WITH_CYCLES_STANDALONE_GUI
//	else
//		options.session->progress.set_update_callback(function_bind(&view_redraw));
//#endif
}

static void internal_custom_scene(float3* vertex_array, float2* uvs_array, float2* lightmapuvs_array, float3* normal_array, int vertex_num,
	int* index_array, int* mat_index, int triangle_num,
	std::string* mat_name, std::string* diffuse_tex, int mat_num)
{
	Scene* scene = options.scene;
	if (scene == NULL)
	{
		scene = new Scene(options.scene_params, options.session->device);
		options.scene = scene;
		options.session->scene = scene;

		/* Calculate Viewplane */
		options.scene->camera->compute_auto_viewplane();
		Transform matrix;

		matrix = transform_translate(make_float3(0.0f, 2.0f, -10.0f));
		options.scene->camera->matrix = matrix;

		fbx_add_default_shader(scene);
	}

	std::vector<int> cycles_shader_indexs(mat_num);
	for (int i = 0; i < mat_num; ++i)
	{
		cycles_shader_indexs[i] = create_pbr_shader(scene, diffuse_tex[i], "", "");
	}

	bool smooth = true;

	Mesh* p_cy_mesh = fbx_add_mesh(scene, transform_identity());
	p_cy_mesh->reserve_mesh(vertex_num, triangle_num);

	p_cy_mesh->verts.resize(vertex_num);

	for (int i = 0; i < mat_num; ++i)
	{
		p_cy_mesh->used_shaders.push_back(scene->shaders[cycles_shader_indexs[i]]);
	}

	Attribute* attr_N = p_cy_mesh->attributes.add(ATTR_STD_VERTEX_NORMAL);
	float3* N = attr_N->data_float3();

	for (int i = 0; i < vertex_num; ++i, ++N)
	{
		p_cy_mesh->verts[i] = vertex_array[i];
		*N = normal_array[i];
	}

	for (int tri_i = 0; tri_i < triangle_num; ++tri_i)
	{
		p_cy_mesh->add_triangle(
			index_array[tri_i * 3],
			index_array[tri_i * 3 + 1],
			index_array[tri_i * 3 + 2],
			mat_index[tri_i], smooth);
	}

	ustring name = ustring("UVMap");
	Attribute* attr = p_cy_mesh->attributes.add(ATTR_STD_UV, name);
	ustring lightmap_name = ustring("lightmap_uv");
	Attribute* lightmap_attr = p_cy_mesh->attributes.add(ATTR_STD_UV, lightmap_name);
	float3* fdata = attr->data_float3();
	float3* lightmap_data = lightmap_attr->data_float3();
	for (int tri_i = 0; tri_i < triangle_num; ++tri_i)
	{
		int iv1 = index_array[tri_i * 3];
		int iv2 = index_array[tri_i * 3 + 1];
		int iv3 = index_array[tri_i * 3 + 2];

		if (uvs_array)
		{
			fdata[tri_i * 3] = make_float3(uvs_array[iv1].x, uvs_array[iv1].y, 1.0f);
			fdata[tri_i * 3 + 1] = make_float3(uvs_array[iv2].x, uvs_array[iv2].y, 1.0f);
			fdata[tri_i * 3 + 2] = make_float3(uvs_array[iv3].x, uvs_array[iv3].y, 1.0f);
		}

		if (lightmapuvs_array)
		{
			//assert(lightmapuvs_array[iv1].x < 1.1f && lightmapuvs_array[iv1].x > -0.1f);
			lightmap_data[tri_i * 3] = make_float3(lightmapuvs_array[iv1].x, lightmapuvs_array[iv1].y, 1.0f);
			lightmap_data[tri_i * 3 + 1] = make_float3(lightmapuvs_array[iv2].x, lightmapuvs_array[iv2].y, 1.0f);
			lightmap_data[tri_i * 3 + 2] = make_float3(lightmapuvs_array[iv3].x, lightmapuvs_array[iv3].y, 1.0f);
		}
	}

	create_mikk_tangent(p_cy_mesh);
}

DLL_EXPORT bool init_cycles(CyclesInitOptions init_op)
{
	//freopen("./my_test_log.txt", "w", stdout);		
	FLAGS_alsologtostderr = 1;
	google::SetLogDestination(0, "my_test_log.txt");
	util_logging_init("./");
	path_init(init_op.device_working_folder);
	assign_session_specific(init_op.width, init_op.height, init_op.render_device, init_op.device_working_folder);

	unity_session_init();
	//options.session->wait();
	//session_exit();

	return true;
}

DLL_EXPORT int unity_add_mesh(float* vertex_array, float* uvs_array, float* lightmapuvs_array, float* normal_array, int vertex_num,
	int* index_array, int* mat_index, int triangle_num,
	const char** mat_name, const char** diffuse_tex, int mat_num)
{
	std::string* mat_name_strings = new std::string[mat_num];
	std::string* diffuse_tex_strings = new std::string[mat_num];
	for (int i = 0; i < mat_num; ++i)
	{
		mat_name_strings[i] = mat_name[i];
		diffuse_tex_strings[i] = diffuse_tex[i];
	}

	internal_custom_scene((float3*)vertex_array, (float2*)uvs_array, (float2*)lightmapuvs_array, (float3*)normal_array, vertex_num,
		index_array, mat_index, triangle_num,
		mat_name_strings, diffuse_tex_strings, mat_num);

	delete[] mat_name_strings;
	delete[] diffuse_tex_strings;

	return 0;
}

DLL_EXPORT int unity_add_light(const char* name, float intensity, float radius, float* color, float* dir, float* pos, int type)
{
	//create light
	Light* l = new Light();
	l->use_mis = true;
	l->dir = *(float3*)dir;
	l->size = radius;
	l->co = *(float3*)(pos);

	/*
	//The type of a Light.
	Spot = 0,
	Directional = 1,
	Point = 2,
	Area = 3,
	Disc = 4
	*/
	if (type == 1)
	{
		l->type = LIGHT_DISTANT;
	}
	else if (type == 2)
	{
		l->type = LIGHT_POINT;
	}

	//create light shader
	ShaderGraph* graph = new ShaderGraph();
	EmissionNode* emission = new EmissionNode();
	emission->color = *(float3*)color;
	emission->strength = intensity;
	graph->add(emission);
	graph->connect(emission->output("Emission"), graph->output()->input("Surface"));
	Shader* p_lshader = new Shader();
	p_lshader->name = name;
	p_lshader->graph = graph;
	options.scene->shaders.push_back(p_lshader);

	//add to scene
	l->shader = p_lshader;
	options.scene->lights.push_back(l);

	return 0;
}

DLL_EXPORT int bake_lightmap()
{
	bake_light_map();

	end_session();

	return 0;
}

//typedef void (*render_image_cb)(const char* data, const int w, const int h, const int data_type);

DLL_EXPORT int interactive_pt_rendering(UnityRenderOptions u3d_render_options, Session::render_image_cb icb)
{
	options.width = u3d_render_options.width;
	options.height = u3d_render_options.height;
	//Cycles camera is right hand coordinate, x for right direction, y for up.
	Transform cam_pos = transform_translate(make_float3(u3d_render_options.camera_pos[0], u3d_render_options.camera_pos[1], u3d_render_options.camera_pos[2]));
	Transform rotate_x = transform_rotate(DEG2RADF(u3d_render_options.euler_angle[0]), make_float3(1.0f, 0.0f, 0.0f));
	Transform rotate_y = transform_rotate(DEG2RADF(u3d_render_options.euler_angle[1]), make_float3(0.0f, 1.0f, 0.0f));
	Transform rotate_z = transform_rotate(DEG2RADF(u3d_render_options.euler_angle[2]), make_float3(0.0f, 0.0f, 1.0f));
	options.scene->camera->matrix = cam_pos * rotate_y * rotate_z * rotate_x;
	options.scene->camera->width = u3d_render_options.width;
	options.scene->camera->height = u3d_render_options.height;
	options.scene->camera->compute_auto_viewplane();
	options.scene->camera->need_update = true;
	options.scene->camera->need_device_update = true;

	options.session_params.samples = u3d_render_options.sample_count;
	//options.session->reset(session_buffer_params(), options.session_params.samples);

	start_render_image();
	options.session->render_icb = icb;
	options.session->wait();

	return 0;
}

DLL_EXPORT int release_cycles()
{
	end_session();

	return 0;
}

//CCL_NAMESPACE_END