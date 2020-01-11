#pragma once

#ifndef _CYCLES_STANDALONE_H_
#define _CYCLES_STANDALONE_H_

#include <string>
#include <util/util_transform.h>
#include <render/scene.h>
#include <render/session.h>

#include <render/mesh.h>
#include <render/object.h>

CCL_NAMESPACE_BEGIN

struct Options {
	Session* session;
	Scene* scene;
	std::string filepath;
	int width, height;
	SceneParams scene_params;
	SessionParams session_params;
	bool quiet;
	bool show_help, interactive, pause;
	string output_path;
};
extern Options options;

bool write_render(const uchar* pixels, int w, int h, int channels);
bool write_float_map(const float* pixels, int w, int h, int channels);

void start_render_image();
void bake_light_map();
void end_session();

int create_pbr_shader(Scene* scene, const std::string& diff_tex, const std::string& mtl_tex, const std::string& normal_tex);
void fbx_add_default_shader(Scene* scene);
Mesh* fbx_add_mesh(Scene* scene, const Transform& tfm);
void create_mikk_tangent(Mesh* cycle_mesh);

CCL_NAMESPACE_END

#endif
