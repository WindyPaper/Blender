/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>

#include "render/buffers.h"
#include "render/camera.h"
#include "device/device.h"
#include "render/scene.h"
#include "render/session.h"
#include "render/integrator.h"
#include "render/mesh.h"
#include "render/nodes.h"
#include "render/object.h"

#include "util/util_args.h"
#include "util/util_foreach.h"
#include "util/util_function.h"
#include "util/util_logging.h"
#include "util/util_path.h"
#include "util/util_progress.h"
#include "util/util_string.h"
#include "util/util_time.h"
#include "util/util_transform.h"
#include "util/util_unique_ptr.h"
#include "util/util_version.h"

#ifdef WITH_CYCLES_STANDALONE_GUI
#include "util/util_view.h"
#endif

#include "app/cycles_xml.h"

#include "Importer.hpp"
#include "scene.h"
#include "postprocess.h"
#include "material.h"

#include "mikktspace.h"
#include "GenerateMikkTangent.h"

CCL_NAMESPACE_BEGIN

struct Options {
	Session *session;
	Scene *scene;
	string filepath;
	int width, height;
	SceneParams scene_params;
	SessionParams session_params;
	bool quiet;
	bool show_help, interactive, pause;
	string output_path;
} options;

static void session_print(const string& str)
{
	/* print with carriage return to overwrite previous */
	printf("\r%s", str.c_str());

	/* add spaces to overwrite longer previous print */
	static int maxlen = 0;
	int len = str.size();
	maxlen = max(len, maxlen);

	for(int i = len; i < maxlen; i++)
		printf(" ");

	/* flush because we don't write an end of line */
	fflush(stdout);
}

static void session_print_status()
{
	string status, substatus;

	/* get status */
	float progress = options.session->progress.get_progress();
	options.session->progress.get_status(status, substatus);

	if(substatus != "")
		status += ": " + substatus;

	/* print status */
	status = string_printf("Progress %05.2f   %s", (double) progress*100, status.c_str());
	session_print(status);
}

static bool write_render(const uchar *pixels, int w, int h, int channels)
{
	string msg = string_printf("Writing image %s", options.output_path.c_str());
	session_print(msg);

	unique_ptr<ImageOutput> out = unique_ptr<ImageOutput>(ImageOutput::create(options.output_path));
	if(!out) {
		return false;
	}

	ImageSpec spec(w, h, channels, TypeDesc::UINT8);
	if(!out->open(options.output_path, spec)) {
		return false;
	}

	/* conversion for different top/bottom convention */
	out->write_image(TypeDesc::UINT8,
		pixels + (h - 1) * w * channels,
		AutoStride,
		-w * channels,
		AutoStride);

	out->close();

	return true;
}

static BufferParams& session_buffer_params()
{
	static BufferParams buffer_params;
	buffer_params.width = options.width;
	buffer_params.height = options.height;
	buffer_params.full_width = options.width;
	buffer_params.full_height = options.height;

	return buffer_params;
}

static int mikk_get_num_faces(const SMikkTSpaceContext* context)
{
	const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
	if (userdata->mesh->subd_faces.size()) {
		return userdata->mesh->subd_faces.size();
	}
	else {
		return userdata->mesh->num_triangles();
	}
}

static int mikk_get_num_verts_of_face(const SMikkTSpaceContext* context,
	const int face_num)
{
	return 3;
}

static int mikk_vertex_index(const Mesh* mesh, const int face_num, const int vert_num)
{
	if (mesh->subd_faces.size()) {
		const Mesh::SubdFace& face = mesh->subd_faces[face_num];
		return mesh->subd_face_corners[face.start_corner + vert_num];
	}
	else {
		return mesh->triangles[face_num * 3 + vert_num];
	}
}

static int mikk_corner_index(const Mesh* mesh, const int face_num, const int vert_num)
{
	return face_num * 3 + vert_num;
}

static void mikk_get_position(const SMikkTSpaceContext* context,
	float P[3],
	const int face_num, const int vert_num)
{
	const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
	const Mesh* mesh = userdata->mesh;
	const int vertex_index = mikk_vertex_index(mesh, face_num, vert_num);
	const float3 vP = mesh->verts[vertex_index];
	P[0] = vP.x;
	P[1] = vP.y;
	P[2] = vP.z;
}

static void mikk_get_texture_coordinate(const SMikkTSpaceContext* context,
	float uv[2],
	const int face_num, const int vert_num)
{
	const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
	const Mesh* mesh = userdata->mesh;
	if (userdata->texface != NULL) {
		const int corner_index = mikk_corner_index(mesh, face_num, vert_num);
		float3 tfuv = userdata->texface[corner_index];
		uv[0] = tfuv.x;
		uv[1] = tfuv.y;
	}	
	else {
		uv[0] = 0.0f;
		uv[1] = 0.0f;
	}
}

static void mikk_get_normal(const SMikkTSpaceContext * context, float N[3],
	const int face_num, const int vert_num)
{
	const MikkUserData* userdata = (const MikkUserData*)context->m_pUserData;
	const Mesh* mesh = userdata->mesh;
	float3 vN;
	if (mesh->subd_faces.size()) {
		const Mesh::SubdFace& face = mesh->subd_faces[face_num];
		if (face.smooth) {
			const int vertex_index = mikk_vertex_index(mesh, face_num, vert_num);
			vN = userdata->vertex_normal[vertex_index];
		}
		else {
			vN = face.normal(mesh);
		}
	}
	else {
		if (mesh->smooth[face_num]) {
			const int vertex_index = mikk_vertex_index(mesh, face_num, vert_num);
			vN = userdata->vertex_normal[vertex_index];
		}
		else {
			const Mesh::Triangle tri = mesh->get_triangle(face_num);
			vN = tri.compute_normal(&mesh->verts[0]);
		}
	}
	N[0] = vN.x;
	N[1] = vN.y;
	N[2] = vN.z;
}

static void mikk_set_tangent_space(const SMikkTSpaceContext * context,
	const float T[],
	const float sign,
	const int face_num, const int vert_num)
{
	MikkUserData* userdata = (MikkUserData*)context->m_pUserData;
	const Mesh* mesh = userdata->mesh;
	const int corner_index = mikk_corner_index(mesh, face_num, vert_num);
	userdata->tangent[corner_index] = make_float3(T[0], T[1], T[2]);
	if (userdata->tangent_sign != NULL) {
		userdata->tangent_sign[corner_index] = sign;
	}
}

static void create_mikk_tangent(Mesh* cycle_mesh)
{	
	/* Setup userdata. */
	//MikkUserData userdata(b_mesh, layer_name, mesh, tangent, tangent_sign);
	/* Setup interface. */
	SMikkTSpaceInterface sm_interface;
	MikkUserData mikk_user_data(cycle_mesh);
	memset(&sm_interface, 0, sizeof(sm_interface));
	sm_interface.m_getNumFaces = mikk_get_num_faces;
	sm_interface.m_getNumVerticesOfFace = mikk_get_num_verts_of_face;
	sm_interface.m_getPosition = mikk_get_position;
	sm_interface.m_getTexCoord = mikk_get_texture_coordinate;
	sm_interface.m_getNormal = mikk_get_normal;
	sm_interface.m_setTSpaceBasic = mikk_set_tangent_space;
	/* Setup context. */
	SMikkTSpaceContext context;
	memset(&context, 0, sizeof(context));
	context.m_pUserData = &mikk_user_data;
	context.m_pInterface = &sm_interface;
	/* Compute tangents. */
	genTangSpaceDefault(&context);
}

static Mesh* fbx_add_mesh(Scene* scene, const Transform& tfm)
{
	Mesh* mesh = new Mesh();
	scene->meshes.push_back(mesh);

	Object* object = new Object();
	object->mesh = mesh;
	object->tfm = tfm;
	scene->objects.push_back(object);

	return mesh;
}

int create_pbr_shader(Scene* scene, const std::string& diff_tex, const std::string& mtl_tex, const std::string& normal_tex)
{
	ShaderGraph* graph = new ShaderGraph();

	ImageTextureNode* img_node = new ImageTextureNode();
	img_node->filename = diff_tex;
	graph->add(img_node);

	ImageTextureNode* mtl_img_node = new ImageTextureNode();
	mtl_img_node->filename = mtl_tex;
	graph->add(mtl_img_node);

	ImageTextureNode* normal_img_node = new ImageTextureNode();
	normal_img_node->filename = normal_tex;
	//normal_img_node->color_space = NODE_COLOR_SPACE_NONE;
	graph->add(normal_img_node);

	NormalMapNode* change_to_normalmap_node = new NormalMapNode();
	change_to_normalmap_node->space = NODE_NORMAL_MAP_TANGENT;
	graph->add(change_to_normalmap_node);
	//change_to_normalmap_node->normal_osl = make_float3(1, 0, 0);
	graph->connect(normal_img_node->output("Color"), change_to_normalmap_node->input("Color"));

	DiffuseBsdfNode* diffuse = new DiffuseBsdfNode();
	//diffuse->color = make_float3(0.8f, 0.8f, 0.8f);
	graph->add(diffuse);

	graph->connect(img_node->output("Color"), diffuse->input("Color"));
	//graph->connect(change_to_normalmap_node->output("Normal"), diffuse->input("Color"));
	graph->connect(change_to_normalmap_node->output("Normal"), diffuse->input("Normal"));

	graph->connect(diffuse->output("BSDF"), graph->output()->input("Surface"));

	Shader* shader = new Shader();
	shader->name = "pbr_default_surface";
	shader->graph = graph;
	scene->shaders.push_back(shader);
	//scene->default_surface = shader;
	shader->tag_update(scene);

	return scene->shaders.size() - 1;
}

static void create_default_shader(Scene* scene, const std::string& diff_tex, const std::string& mtl_tex, const std::string& normal_tex)
{
	/* default surface */
	{
		/*ShaderGraph* graph = new ShaderGraph();

		ImageTextureNode* img_node = new ImageTextureNode();
		img_node->filename = diffuse_tex;
		graph->add(img_node);

		DiffuseBsdfNode* diffuse = new DiffuseBsdfNode();
		diffuse->color = make_float3(0.8f, 0.8f, 0.8f);
		graph->add(diffuse);

		graph->connect(img_node->output("Color"), diffuse->input("Color"));

		graph->connect(diffuse->output("BSDF"), graph->output()->input("Surface"));

		Shader* shader = new Shader();
		shader->name = "default_surface";
		shader->graph = graph;
		scene->shaders.push_back(shader);
		scene->default_surface = shader;*/
		create_pbr_shader(scene, diff_tex, mtl_tex, normal_tex);
	}

	/* default light */
	{
		ShaderGraph* graph = new ShaderGraph();

		EmissionNode* emission = new EmissionNode();
		emission->color = make_float3(0.8f, 0.8f, 0.8f);
		emission->strength = 0.0f;
		graph->add(emission);

		graph->connect(emission->output("Emission"), graph->output()->input("Surface"));

		Shader* shader = new Shader();
		shader->name = "default_light";
		shader->graph = graph;
		scene->shaders.push_back(shader);
		scene->default_light = shader;
	}

	/* default background */
	{
		ShaderGraph* graph = new ShaderGraph();

		Shader* shader = new Shader();
		shader->name = "default_background";
		shader->graph = graph;
		scene->shaders.push_back(shader);
		scene->default_background = shader;
	}

	/* default empty */
	{
		ShaderGraph* graph = new ShaderGraph();

		Shader* shader = new Shader();
		shader->name = "default_empty";
		shader->graph = graph;
		scene->shaders.push_back(shader);
		scene->default_empty = shader;
	}
}

static void fbx_add_default_shader(Scene* scene)
{
	/* default surface */
	{
		//ShaderGraph* graph = new ShaderGraph();

		//UVMapNode* uv_node = new UVMapNode();
		//uv_node->attribute = ustring("UVMap");
		////uv_node->from_dupli = true;
		//graph->add(uv_node);	

		//ImageTextureNode* img_node = new ImageTextureNode();
		//img_node->filename = diffuse_tex;
		//graph->add(img_node);

		//graph->connect(uv_node->output("UV"), img_node->input("Vector"));

		//DiffuseBsdfNode* diffuse = new DiffuseBsdfNode();
		//diffuse->color = make_float3(0.8f, 0.8f, 0.8f);
		//graph->add(diffuse);

		//graph->connect(img_node->output("Color"), diffuse->input("Color"));

		//graph->connect(diffuse->output("BSDF"), graph->output()->input("Surface"));

		//Shader* shader = new Shader();
		//shader->name = "default_surface";
		//shader->graph = graph;
		//scene->shaders.push_back(shader);
		//scene->default_surface = shader;
		//shader->tag_update(scene);

		//create_pbr_shader(scene, diff_tex, mtl_tex, normal_tex);
	}

	/* default light */
	{
		ShaderGraph* graph = new ShaderGraph();

		EmissionNode* emission = new EmissionNode();
		emission->color = make_float3(0.8f, 0.8f, 0.8f);
		emission->strength = 0.0f;
		graph->add(emission);

		graph->connect(emission->output("Emission"), graph->output()->input("Surface"));

		Shader* shader = new Shader();
		shader->name = "default_light";
		shader->graph = graph;
		scene->shaders.push_back(shader);
		scene->default_light = shader;
	}

	/* default background */
	{
		ShaderGraph* graph = new ShaderGraph();

		Shader* shader = new Shader();
		shader->name = "default_background";
		shader->graph = graph;
		scene->shaders.push_back(shader);
		scene->default_background = shader;
	}

	/* default empty */
	{
		ShaderGraph* graph = new ShaderGraph();

		Shader* shader = new Shader();
		shader->name = "default_empty";
		shader->graph = graph;
		scene->shaders.push_back(shader);
		scene->default_empty = shader;
	}

	ShaderGraph *gra = scene->default_background->graph;
	BackgroundNode* bk_node = new BackgroundNode();
	gra->add(bk_node);
	gra->connect(bk_node->output("Background"), gra->output()->input("Surface"));

	ColorNode* cb_node = new ColorNode();
	cb_node->value = make_float3(0.8, 0.8, 0.8);
	gra->add(cb_node);
	gra->connect(cb_node->output("Color"), bk_node->input("Color"));

	ValueNode* v_node = new ValueNode();
	v_node->value = 1.0;
	gra->add(v_node);
	gra->connect(v_node->output("Value"), bk_node->input("Strength"));
}

int TranslateMaterialCycles(Scene* scene, const aiMaterial* ai_mat, const std::string &dir_name)
{
	aiString ai_normal_str;
	if (ai_mat->GetTexture(aiTextureType_NORMALS, 0, &ai_normal_str) == aiReturn_SUCCESS ||
		ai_mat->GetTexture(aiTextureType_HEIGHT, 0, &ai_normal_str) == aiReturn_SUCCESS)
	{		
		
	}

	aiString ai_diffuse_str;
	if (ai_mat->GetTexture(aiTextureType_DIFFUSE, 0, &ai_diffuse_str) == aiReturn_SUCCESS)
	{
		
	}

	return create_pbr_shader(scene, path_join(dir_name, ai_diffuse_str.C_Str()), "", path_join(dir_name, ai_normal_str.C_Str()));
}

static void assimp_read_file(Scene *scene, std::string filename)
{	
	std::string dir_name = path_dirname(filename);

	Assimp::Importer importer;
	unsigned int flags = aiProcess_MakeLeftHanded |
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_PreTransformVertices |
		aiProcess_RemoveRedundantMaterials |
		aiProcess_OptimizeMeshes |
		aiProcess_FlipWindingOrder;
	const aiScene* import_fbx_scene = importer.ReadFile(filename, flags);

	fbx_add_default_shader(scene);

	if (import_fbx_scene == NULL)
	{
		std::string error_code = importer.GetErrorString();
		std::cout << ("load fbx file failed! " + error_code) << std::endl;
		return;
	}

	unsigned int mesh_num = import_fbx_scene->mNumMeshes;
	unsigned int mat_num = import_fbx_scene->mNumMaterials;

	std::vector<int> cycles_shader_indexs(mat_num);
	for (int i = 0; i < mat_num; ++i)
	{
		cycles_shader_indexs[i] = TranslateMaterialCycles(scene, import_fbx_scene->mMaterials[i], dir_name);
	}	

	int shader = 0;
	bool smooth = true;

	for (unsigned int mesh_i = 0; mesh_i < mesh_num; ++mesh_i)
	{
		aiMesh* mesh_ptr = import_fbx_scene->mMeshes[mesh_i];
		unsigned int triangle_num = mesh_ptr->mNumFaces;
		unsigned int vertex_num = mesh_ptr->mNumVertices;

		Mesh* p_cy_mesh = fbx_add_mesh(scene, transform_identity());
		p_cy_mesh->reserve_mesh(vertex_num, triangle_num);

		const aiVector3D* aivertices_data = mesh_ptr->mVertices;
		const aiVector3D* aiverteces_normal_data = mesh_ptr->mNormals;
		p_cy_mesh->verts.resize(vertex_num);

		//p_cy_mesh->reserve_mesh(vertex_num, triangle_num);

		p_cy_mesh->used_shaders.push_back(scene->shaders[cycles_shader_indexs[mesh_i]]);

		Attribute* attr_N = p_cy_mesh->attributes.add(ATTR_STD_VERTEX_NORMAL);
		float3* N = attr_N->data_float3();

		for (int i = 0; i < vertex_num; ++i, ++N)
		{
			p_cy_mesh->verts[i] = make_float3(aivertices_data[i].x, aivertices_data[i].y, aivertices_data[i].z);
			*N = make_float3(aiverteces_normal_data[i].x, aiverteces_normal_data[i].y, aiverteces_normal_data[i].z);
		}		

		for (int tri_i = 0; tri_i < triangle_num; ++tri_i)
		{
			const aiFace* p_face = &mesh_ptr->mFaces[tri_i];
			p_cy_mesh->add_triangle(p_face->mIndices[0], p_face->mIndices[1], p_face->mIndices[2], shader, smooth);			
		}

		ustring name = ustring("UVMap");
		Attribute* attr = p_cy_mesh->attributes.add(ATTR_STD_UV, name);
		float3* fdata = attr->data_float3();
		for (int tri_i = 0; tri_i < triangle_num; ++tri_i)
		{
			const aiFace* p_face = &mesh_ptr->mFaces[tri_i];
			int iv1 = p_face->mIndices[0];
			int iv2 = p_face->mIndices[1];
			int iv3 = p_face->mIndices[2];

			if (mesh_ptr->mTextureCoords[0])
			{
				//float3 t1 = make_float3(mesh_ptr->mTextureCoords[0][iv1].x, mesh_ptr->mTextureCoords[0][iv1].y, mesh_ptr->mTextureCoords[0][iv1].z);
				//float3 t2 = make_float3(mesh_ptr->mTextureCoords[0][iv2].x, mesh_ptr->mTextureCoords[0][iv2].y, mesh_ptr->mTextureCoords[0][iv2].z);
				//float3 t3 = make_float3(mesh_ptr->mTextureCoords[0][iv3].x, mesh_ptr->mTextureCoords[0][iv3].y, mesh_ptr->mTextureCoords[0][iv3].z);
				fdata[tri_i * 3] = make_float3(mesh_ptr->mTextureCoords[0][iv1].x, mesh_ptr->mTextureCoords[0][iv1].y, mesh_ptr->mTextureCoords[0][iv1].z);
				fdata[tri_i * 3 + 1] = make_float3(mesh_ptr->mTextureCoords[0][iv2].x, mesh_ptr->mTextureCoords[0][iv2].y, mesh_ptr->mTextureCoords[0][iv2].z);
				fdata[tri_i * 3 + 2] = make_float3(mesh_ptr->mTextureCoords[0][iv3].x, mesh_ptr->mTextureCoords[0][iv3].y, mesh_ptr->mTextureCoords[0][iv3].z);
			}
		}
		//memcpy(fdata, &mesh_ptr->mTextureCoords[0][0], sizeof(aiVector3D) * triangle_num * 3);

		

		//scene->default_background

		create_mikk_tangent(p_cy_mesh);
	}

	//create tangent	
}

static void scene_init()
{	
	options.scene = new Scene(options.scene_params, options.session->device);

	/* Read XML */
	std::transform(options.filepath.begin(), options.filepath.end(), options.filepath.begin(), ::tolower);
	if (options.filepath.find(".fbx") != std::string::npos)
	{
		assimp_read_file(options.scene, options.filepath.c_str());
	}
	else
	{
		xml_read_file(options.scene, options.filepath.c_str());
	}

	/* Camera width/height override? */
	if(!(options.width == 0 || options.height == 0)) {
		options.scene->camera->width = options.width;
		options.scene->camera->height = options.height;
	}
	else {
		options.width = options.scene->camera->width;
		options.height = options.scene->camera->height;
	}

	/* Calculate Viewplane */
	options.scene->camera->compute_auto_viewplane();
	Transform matrix;

	matrix = transform_translate(make_float3(0.0f, 2.0f, -10.0f));
	options.scene->camera->matrix = matrix;
}

static void session_init()
{
	options.session_params.write_render_cb = write_render;
	options.session = new Session(options.session_params);

	if(options.session_params.background && !options.quiet)
		options.session->progress.set_update_callback(function_bind(&session_print_status));
#ifdef WITH_CYCLES_STANDALONE_GUI
	else
		options.session->progress.set_update_callback(function_bind(&view_redraw));
#endif

	/* load scene */
	scene_init();
	options.session->scene = options.scene;

	options.session->reset(session_buffer_params(), options.session_params.samples);
	options.session->start();
}

static void session_exit()
{
	if(options.session) {
		delete options.session;
		options.session = NULL;
	}

	if(options.session_params.background && !options.quiet) {
		session_print("Finished Rendering.");
		printf("\n");
	}
}

#ifdef WITH_CYCLES_STANDALONE_GUI
static void display_info(Progress& progress)
{
	static double latency = 0.0;
	static double last = 0;
	double elapsed = time_dt();
	string str, interactive;

	latency = (elapsed - last);
	last = elapsed;

	double total_time, sample_time;
	string status, substatus;

	progress.get_time(total_time, sample_time);
	progress.get_status(status, substatus);
	float progress_val = progress.get_progress();

	if(substatus != "")
		status += ": " + substatus;

	interactive = options.interactive? "On":"Off";

	str = string_printf(
	        "%s"
	        "        Time: %.2f"
	        "        Latency: %.4f"
	        "        Progress: %05.2f"
	        "        Average: %.4f"
	        "        Interactive: %s",
	        status.c_str(), total_time, latency, (double) progress_val*100, sample_time, interactive.c_str());

	view_display_info(str.c_str());

	if(options.show_help)
		view_display_help();
}

static void display()
{
	static DeviceDrawParams draw_params = DeviceDrawParams();

	options.session->draw(session_buffer_params(), draw_params);

	display_info(options.session->progress);
}

static void motion(int x, int y, int button)
{
	if(options.interactive) {
		Transform matrix = options.session->scene->camera->matrix;

		/* Translate */
		if(button == 0) {
			float3 translate = make_float3(x * 0.01f, -(y * 0.01f), 0.0f);
			matrix = matrix * transform_translate(translate);
		}

		/* Rotate */
		else if(button == 2) {
			float4 r1 = make_float4((float)x * 0.1f, 0.0f, 1.0f, 0.0f);
			matrix = matrix * transform_rotate(DEG2RADF(r1.x), make_float3(r1.y, r1.z, r1.w));

			float4 r2 = make_float4(y * 0.1f, 1.0f, 0.0f, 0.0f);
			matrix = matrix * transform_rotate(DEG2RADF(r2.x), make_float3(r2.y, r2.z, r2.w));
		}

		/* Update and Reset */
		options.session->scene->camera->matrix = matrix;
		options.session->scene->camera->need_update = true;
		options.session->scene->camera->need_device_update = true;

		options.session->reset(session_buffer_params(), options.session_params.samples);
	}
}

static void resize(int width, int height)
{
	options.width = width;
	options.height = height;

	if(options.session) {
		/* Update camera */
		options.session->scene->camera->width = width;
		options.session->scene->camera->height = height;
		options.session->scene->camera->compute_auto_viewplane();
		options.session->scene->camera->need_update = true;
		options.session->scene->camera->need_device_update = true;

		options.session->reset(session_buffer_params(), options.session_params.samples);
	}
}

static void keyboard(unsigned char key)
{
	/* Toggle help */
	if(key == 'h')
		options.show_help = !(options.show_help);

	/* Reset */
	else if(key == 'r')
		options.session->reset(session_buffer_params(), options.session_params.samples);

	/* Cancel */
	else if(key == 27) // escape
		options.session->progress.set_cancel("Canceled");

	/* Pause */
	else if(key == 'p') {
		options.pause = !options.pause;
		options.session->set_pause(options.pause);
	}

	/* Interactive Mode */
	else if(key == 'i')
		options.interactive = !(options.interactive);

	/* Navigation */
	else if(options.interactive && (key == 'w' || key == 'a' || key == 's' || key == 'd')) {
		Transform matrix = options.session->scene->camera->matrix;
		float3 translate;

		if(key == 'w')
			translate = make_float3(0.0f, 0.0f, 0.1f);
		else if(key == 's')
			translate = make_float3(0.0f, 0.0f, -0.1f);
		else if(key == 'a')
			translate = make_float3(-0.1f, 0.0f, 0.0f);
		else if(key == 'd')
			translate = make_float3(0.1f, 0.0f, 0.0f);

		matrix = matrix * transform_translate(translate);

		/* Update and Reset */
		options.session->scene->camera->matrix = matrix;
		options.session->scene->camera->need_update = true;
		options.session->scene->camera->need_device_update = true;

		options.session->reset(session_buffer_params(), options.session_params.samples);
	}

	/* Set Max Bounces */
	else if(options.interactive && (key == '0' || key == '1' || key == '2' || key == '3')) {
		int bounce;
		switch(key) {
			case '0': bounce = 0; break;
			case '1': bounce = 1; break;
			case '2': bounce = 2; break;
			case '3': bounce = 3; break;
			default: bounce = 0; break;
		}

		options.session->scene->integrator->max_bounce = bounce;

		/* Update and Reset */
		options.session->scene->integrator->need_update = true;

		options.session->reset(session_buffer_params(), options.session_params.samples);
	}
}
#endif

static int files_parse(int argc, const char *argv[])
{
	if(argc > 0)
		options.filepath = argv[0];

	return 0;
}

static void options_parse(int argc, const char **argv)
{
	options.width = 0;
	options.height = 0;
	options.filepath = "";
	options.session = NULL;
	options.quiet = false;

	/* device names */
	string device_names = "";
	string devicename = "CPU";
	bool list = false;

	vector<DeviceType>& types = Device::available_types();

	/* TODO(sergey): Here's a feedback loop happens: on the one hand we want
	 * the device list to be printed in help message, on the other hand logging
	 * is not initialized yet so we wouldn't have debug log happening in the
	 * device initialization.
	 */
	foreach(DeviceType type, types) {
		if(device_names != "")
			device_names += ", ";

		device_names += Device::string_from_type(type);
	}

	/* shading system */
	string ssname = "svm";

	/* parse options */
	ArgParse ap;
	bool help = false, debug = false, version = false;
	int verbosity = 1;

	ap.options ("Usage: cycles [options] file.xml",
		"%*", files_parse, "",
		"--device %s", &devicename, ("Devices to use: " + device_names).c_str(),
#ifdef WITH_OSL
		"--shadingsys %s", &ssname, "Shading system to use: svm, osl",
#endif
		"--background", &options.session_params.background, "Render in background, without user interface",
		"--quiet", &options.quiet, "In background mode, don't print progress messages",
		"--samples %d", &options.session_params.samples, "Number of samples to render",
		"--output %s", &options.output_path, "File path to write output image",
		"--threads %d", &options.session_params.threads, "CPU Rendering Threads",
		"--width  %d", &options.width, "Window width in pixel",
		"--height %d", &options.height, "Window height in pixel",
		"--tile-width %d", &options.session_params.tile_size.x, "Tile width in pixels",
		"--tile-height %d", &options.session_params.tile_size.y, "Tile height in pixels",
		"--list-devices", &list, "List information about all available devices",
#ifdef WITH_CYCLES_LOGGING
		"--debug", &debug, "Enable debug logging",
		"--verbose %d", &verbosity, "Set verbosity of the logger",
#endif
		"--help", &help, "Print help message",
		"--version", &version, "Print version number",
		NULL);

	if(ap.parse(argc, argv) < 0) {
		fprintf(stderr, "%s\n", ap.geterror().c_str());
		ap.usage();
		exit(EXIT_FAILURE);
	}

	if(debug) {
		util_logging_start();
		util_logging_verbosity_set(verbosity);
	}

	if(list) {
		vector<DeviceInfo>& devices = Device::available_devices();
		printf("Devices:\n");

		foreach(DeviceInfo& info, devices) {
			printf("    %-10s%s%s\n",
				Device::string_from_type(info.type).c_str(),
				info.description.c_str(),
				(info.display_device)? " (display)": "");
		}

		exit(EXIT_SUCCESS);
	}
	else if(version) {
		printf("%s\n", CYCLES_VERSION_STRING);
		exit(EXIT_SUCCESS);
	}
	else if(help || options.filepath == "") {
		ap.usage();
		exit(EXIT_SUCCESS);
	}

	if(ssname == "osl")
		options.scene_params.shadingsystem = SHADINGSYSTEM_OSL;
	else if(ssname == "svm")
		options.scene_params.shadingsystem = SHADINGSYSTEM_SVM;

#ifndef WITH_CYCLES_STANDALONE_GUI
	options.session_params.background = true;
#endif

	/* Use progressive rendering */
	options.session_params.progressive = true;

	/* find matching device */
	DeviceType device_type = Device::type_from_string(devicename.c_str());
	vector<DeviceInfo>& devices = Device::available_devices();
	bool device_available = false;

	foreach(DeviceInfo& device, devices) {
		if(device_type == device.type) {
			options.session_params.device = device;
			device_available = true;
			break;
		}
	}

	/* handle invalid configurations */
	if(options.session_params.device.type == DEVICE_NONE || !device_available) {
		fprintf(stderr, "Unknown device: %s\n", devicename.c_str());
		exit(EXIT_FAILURE);
	}
#ifdef WITH_OSL
	else if(!(ssname == "osl" || ssname == "svm")) {
		fprintf(stderr, "Unknown shading system: %s\n", ssname.c_str());
		exit(EXIT_FAILURE);
	}
	else if(options.scene_params.shadingsystem == SHADINGSYSTEM_OSL && options.session_params.device.type != DEVICE_CPU) {
		fprintf(stderr, "OSL shading system only works with CPU device\n");
		exit(EXIT_FAILURE);
	}
#endif
	else if(options.session_params.samples < 0) {
		fprintf(stderr, "Invalid number of samples: %d\n", options.session_params.samples);
		exit(EXIT_FAILURE);
	}
	else if(options.filepath == "") {
		fprintf(stderr, "No file path specified\n");
		exit(EXIT_FAILURE);
	}

	/* For smoother Viewport */
	options.session_params.start_resolution = 64;
}

CCL_NAMESPACE_END

using namespace ccl;

int main(int argc, const char **argv)
{
	util_logging_init(argv[0]);
	path_init();
	options_parse(argc, argv);

#ifdef WITH_CYCLES_STANDALONE_GUI
	if(options.session_params.background) {
#endif
		session_init();
		options.session->wait();
		session_exit();
#ifdef WITH_CYCLES_STANDALONE_GUI
	}
	else {
		string title = "Cycles: " + path_filename(options.filepath);

		/* init/exit are callback so they run while GL is initialized */
		view_main_loop(title.c_str(), options.width, options.height,
			session_init, session_exit, resize, display, keyboard, motion);
	}
#endif

	system("pause");
	return 0;
}
