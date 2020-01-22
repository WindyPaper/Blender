#include "dll_functions.h"
#include "util/util_logging.h"
#include "util/util_math_float3.h"
#include "util/util_math_float4.h"
#include "render/mesh.h"
#include "render/graph.h"
#include "render/nodes.h"
#include "render/scene.h"
#include "render/camera.h"
#include "render/light.h"
#include "render/integrator.h"
#include "util/util_path.h"

// CCL_NAMESPACE_BEGIN

#define RAD2DEGF(_rad) ((_rad) * (float)(180.0 / M_PI))
#define DEG2RADF(_deg) ((_deg) * (float)(M_PI / 180.0))

using namespace ccl;

static ccl::half *unity_output_buffer = NULL;

void assign_session_specific(const int w,
                             const int h,
                             const int render_device,
                             const std::string &device_working_folder,
                             const bool enable_denoise)
{
  options.filepath = "";
  options.session = NULL;
  options.quiet = false;

  enum RenderDeviceOptions { CUDA = 0, CPU = 1 };

  /* device names */
  string device_names = "";
  string select_device_name;
  switch (render_device) {
    case CUDA:
      select_device_name = "CUDA";
      break;
    case CPU:
      select_device_name = "CPU";
      break;
  }

  //bool list = false;

  vector<DeviceType> &types = Device::available_types();

  /* TODO(sergey): Here's a feedback loop happens: on the one hand we want
   * the device list to be printed in help message, on the other hand logging
   * is not initialized yet so we wouldn't have debug log happening in the
   * device initialization.
   */
  foreach (DeviceType type, types) {
    if (device_names != "")
      device_names += ", ";

    device_names += Device::string_from_type(type);
  }

  /* shading system */
  string ssname = "svm";

  options.quiet = false;
  options.session_params.samples = 4;
  // options.output_path = "./unity_dll_test_image/";
  options.output_path = "./Assets/out_render_image.tga";
  options.width = w;
  options.height = h;
  options.session_params.tile_size.x = 32;
  options.session_params.tile_size.y = 32;
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
  if (enable_denoise) {
    options.session_params.progressive = false;
    options.session_params.background = true;  // only for CPU interactive rendering
  }
  else {
    options.session_params.progressive = true;
    options.session_params.background = false;  // only for CPU interactive rendering
  }

  /* find matching device */
  DeviceType device_type = Device::type_from_string(select_device_name.c_str());
  vector<DeviceInfo> &devices = Device::available_devices();
  bool device_available = false;

  foreach (DeviceInfo &device, devices) {
    if (device_type == device.type) {
      options.session_params.device = device;
      device_available = true;
      break;
    }
  }

  /* handle invalid configurations */
  if (options.session_params.device.type == DEVICE_NONE || !device_available) {
    fprintf(stderr, "Unknown device: %s\n", select_device_name.c_str());
    // exit(EXIT_FAILURE);
  }
  else if (options.session_params.samples < 0) {
    fprintf(stderr, "Invalid number of samples: %d\n", options.session_params.samples);
    // exit(EXIT_FAILURE);
  }
  else if (options.filepath == "") {
    fprintf(stderr, "No file path specified\n");
    // exit(EXIT_FAILURE);
  }

  /* For smoother Viewport */
  options.session_params.start_resolution = 64;
}

static void denoise_render_cb(RenderTile &rtile)
{
  RenderBuffers *p_bufs = rtile.buffers;
  bool copy_from_device_ret = (p_bufs->copy_from_device());
  assert(copy_from_device_ret);

  float exposure = options.scene->film->exposure;
  vector<float> pixels(rtile.w * rtile.h * 4);

  int samples = rtile.sample;
  int component = 4;
  bool ret_val = p_bufs->get_pass_rect("Combined", exposure, samples, component, &pixels[0]);

  if (unity_output_buffer == NULL) {
    int size = options.width * options.height * component;
    unity_output_buffer = new ccl::half[size];
    memset(unity_output_buffer, 0, size * sizeof(ccl::half));
  }

  // update subpixel
  int curr_pixel_index = 0;
  for (int y = rtile.y; y < rtile.y + rtile.h; ++y) {
    for (int x = rtile.x; x < rtile.x + rtile.w; ++x) {     
      ccl::half *curr = unity_output_buffer + ((y * options.width) + x) * component;
      ccl::half4 in_color;
      float4 f4_color = make_float4(pixels[curr_pixel_index],
                                    pixels[curr_pixel_index + 1],
                                    pixels[curr_pixel_index + 2],
                                    pixels[curr_pixel_index + 3]);
      float4_store_half(&in_color.x, f4_color, 1.0f);
      memcpy(curr, &in_color.x, sizeof(ccl::half4));

      curr_pixel_index += component;
	}
  }  

  float progress = options.session->get_progress();
  options.session->render_icb(
      (ccl::half *)unity_output_buffer, options.width, options.height, 0, progress);
}

static void unity_session_init(const bool enable_denoise)
{
  if (!enable_denoise) {
    options.session_params.write_render_cb = write_render;
  }

  options.session = new Session(options.session_params);

  if (enable_denoise) {
    options.session->write_render_tile_cb = denoise_render_cb;

    options.session->params.run_denoising = true;
    options.session->params.full_denoising = true;
    options.session->params.optix_denoising = false;
    options.session->params.write_denoising_passes = false;
    options.session->tile_manager.schedule_denoising = true;
    /*options.session->params.denoising.radius = 8.0f;
      options.session->params.denoising.strength = get_float(crl, "denoising_strength");
    options.session->params.denoising.feature_strength =
    get_float(crl,"denoising_feature_strength"); options.session->params.denoising.relative_pca =
    get_boolean(crl, "denoising_relative_pca");
    options.session->params.denoising.optix_input_passes =
    get_enum(crl,"denoising_optix_input_passes");
      options.session->tile_manager.schedule_denoising = session->params.run_denoising;*/
  }
  // options.session->tile_manager.schedule_denoising = true;

  //	if (options.session_params.background && !options.quiet)
  //		options.session->progress.set_update_callback(function_bind(&session_print_status));
  //#ifdef WITH_CYCLES_STANDALONE_GUI
  //	else
  //		options.session->progress.set_update_callback(function_bind(&view_redraw));
  //#endif
}

int create_unity2cycles_shader(Scene *scene, const CyclesMtlData *mtl_data)
{
  ShaderGraph *graph = new ShaderGraph();

  TextureCoordinateNode *tex_uv_coord_node = new TextureCoordinateNode();
  graph->add(tex_uv_coord_node);
  MappingNode *tex_scale_mapping_node = new MappingNode();
  tex_scale_mapping_node->scale.x = mtl_data->tiling_x;
  tex_scale_mapping_node->scale.y = mtl_data->tiling_y;
  tex_scale_mapping_node->type = NodeMappingType::NODE_MAPPING_TYPE_VECTOR;
  graph->add(tex_scale_mapping_node);

  graph->connect(tex_uv_coord_node->output("UV"), tex_scale_mapping_node->input("Vector"));

  ImageTextureNode *diff_img_node = new ImageTextureNode();
  diff_img_node->filename = mtl_data->diffuse_tex_name;
  graph->add(diff_img_node);
  graph->connect(tex_scale_mapping_node->output("Vector"), diff_img_node->input("Vector"));

  ImageTextureNode *mtl_img_node = new ImageTextureNode();
  mtl_img_node->filename = mtl_data->mtl_tex_name;
  mtl_img_node->colorspace = u_colorspace_raw;
  graph->add(mtl_img_node);
  graph->connect(tex_scale_mapping_node->output("Vector"), mtl_img_node->input("Vector"));

  SeparateRGBNode *mtl_separate = new SeparateRGBNode();
  graph->add(mtl_separate);
  graph->connect(mtl_img_node->output("Color"), mtl_separate->input("Image"));
  MathNode *mtl_math_sub = new MathNode();
  graph->add(mtl_math_sub);
  mtl_math_sub->type = NodeMathType::NODE_MATH_SUBTRACT;
  mtl_math_sub->value1 = 1.0f;
  graph->connect(mtl_img_node->output("Alpha"), mtl_math_sub->input("Value2"));

  ImageTextureNode *normal_img_node = new ImageTextureNode();
  normal_img_node->filename = mtl_data->normal_tex_name;
  // normal_img_node->color_space = NODE_COLOR_SPACE_NONE;
  normal_img_node->colorspace = u_colorspace_raw;
  graph->add(normal_img_node);
  graph->connect(tex_scale_mapping_node->output("Vector"), normal_img_node->input("Vector"));

  NormalMapNode *change_to_normalmap_node = new NormalMapNode();
  change_to_normalmap_node->space = NODE_NORMAL_MAP_TANGENT;
  graph->add(change_to_normalmap_node);
  // change_to_normalmap_node->normal_osl = make_float3(1, 0, 0);
  graph->connect(normal_img_node->output("Color"), change_to_normalmap_node->input("Color"));

  // DiffuseBsdfNode* diffuse = new DiffuseBsdfNode(); Only for baking
  PrincipledBsdfNode *pbr = new PrincipledBsdfNode();
  // diffuse->color = make_float3(0.8f, 0.8f, 0.8f);
  graph->add(pbr);

  graph->connect(diff_img_node->output("Color"), pbr->input("Base Color"));
  graph->connect(diff_img_node->output("Alpha"), pbr->input("Alpha"));
  graph->connect(mtl_math_sub->output("Value"), pbr->input("Roughness"));
  graph->connect(mtl_separate->output("R"), pbr->input("Metallic"));
  // pbr->metallic = 1.0f;
  // pbr->roughness = 0.5f;

  // not comment for baking
  graph->connect(change_to_normalmap_node->output("Normal"), pbr->input("Normal"));

  graph->connect(pbr->output("BSDF"), graph->output()->input("Surface"));

  Shader *shader = new Shader();
  shader->name = "pbr_default_surface";
  shader->graph = graph;
  scene->shaders.push_back(shader);
  // scene->default_surface = shader;
  shader->tag_update(scene);

  return scene->shaders.size() - 1;
}

static void internal_custom_scene(const CyclesMeshData &mesh_data, const CyclesMtlData *mtls)
// std::string* mat_name, std::string* diffuse_tex, int mat_num)
{
  const float3 *vertex_array = (float3 *)mesh_data.vertex_array;
  const float2 *uvs_array = (float2 *)mesh_data.uvs_array;
  const float2 *lightmapuvs_array = (float2 *)mesh_data.lightmapuvs_array;
  const float3 *normal_array = (float3 *)mesh_data.normal_array;
  const int vertex_num = mesh_data.vertex_num;
  const int *index_array = mesh_data.index_array;
  const int *mat_index = mesh_data.mat_index;
  const int triangle_num = mesh_data.triangle_num;

  Scene *scene = options.scene;
  if (scene == NULL) {
    scene = new Scene(options.scene_params, options.session->device);
    options.scene = scene;
    options.session->scene = scene;

    /* Calculate Viewplane */
    options.scene->camera->compute_auto_viewplane();
    Transform matrix;

    matrix = transform_translate(make_float3(0.0f, 2.0f, -10.0f));
    options.scene->camera->matrix = matrix;

    fbx_add_default_shader(scene);

    // film pass
    options.scene->film->display_pass = PassType::PASS_COMBINED;
    options.scene->film->tag_passes_update(options.scene, session_buffer_params().passes);
    options.scene->film->tag_update(options.scene);
    options.scene->integrator->tag_update(options.scene);

    options.scene->film->denoising_data_pass = true;
    options.scene->film->denoising_clean_pass = false;
    options.scene->film->denoising_flags = DENOISING_CLEAN_ALL_PASSES;
  }

  int mtl_num = mesh_data.mtl_num;
  std::vector<int> cycles_shader_indexs(mtl_num);
  for (int i = 0; i < mtl_num; ++i) {
    cycles_shader_indexs[i] = create_unity2cycles_shader(scene, &mtls[i]);
  }

  bool smooth = true;

  Mesh *p_cy_mesh = fbx_add_mesh(scene, transform_identity());
  p_cy_mesh->reserve_mesh(vertex_num, triangle_num);

  p_cy_mesh->verts.resize(vertex_num);

  for (int i = 0; i < mtl_num; ++i) {
    p_cy_mesh->used_shaders.push_back(scene->shaders[cycles_shader_indexs[i]]);
  }

  Attribute *attr_N = p_cy_mesh->attributes.add(ATTR_STD_VERTEX_NORMAL);
  float3 *N = attr_N->data_float3();

  for (int i = 0; i < vertex_num; ++i, ++N) {
    p_cy_mesh->verts[i] = vertex_array[i];
    *N = normal_array[i];
  }

  for (int tri_i = 0; tri_i < triangle_num; ++tri_i) {
    p_cy_mesh->add_triangle(index_array[tri_i * 3],
                            index_array[tri_i * 3 + 1],
                            index_array[tri_i * 3 + 2],
                            mat_index[tri_i],
                            smooth);
  }

  ustring name = ustring("UVMap");
  Attribute *attr = p_cy_mesh->attributes.add(ATTR_STD_UV, name);
  ustring lightmap_name = ustring("lightmap_uv");
  Attribute *lightmap_attr = p_cy_mesh->attributes.add(ATTR_STD_UV, lightmap_name);
  float2 *fdata = attr->data_float2();
  float2 *lightmap_data = lightmap_attr->data_float2();
  for (int tri_i = 0; tri_i < triangle_num; ++tri_i) {
    int iv1 = index_array[tri_i * 3];
    int iv2 = index_array[tri_i * 3 + 1];
    int iv3 = index_array[tri_i * 3 + 2];

    if (uvs_array) {
      fdata[tri_i * 3] = make_float2(uvs_array[iv1].x, uvs_array[iv1].y);
      fdata[tri_i * 3 + 1] = make_float2(uvs_array[iv2].x, uvs_array[iv2].y);
      fdata[tri_i * 3 + 2] = make_float2(uvs_array[iv3].x, uvs_array[iv3].y);
    }

    if (lightmapuvs_array) {
      // assert(lightmapuvs_array[iv1].x < 1.1f && lightmapuvs_array[iv1].x > -0.1f);
      lightmap_data[tri_i * 3] = make_float2(lightmapuvs_array[iv1].x, lightmapuvs_array[iv1].y);
      lightmap_data[tri_i * 3 + 1] = make_float2(lightmapuvs_array[iv2].x,
                                                 lightmapuvs_array[iv2].y);
      lightmap_data[tri_i * 3 + 2] = make_float2(lightmapuvs_array[iv3].x,
                                                 lightmapuvs_array[iv3].y);
    }
  }

  create_mikk_tangent(p_cy_mesh);
}

DLL_EXPORT bool init_cycles(CyclesInitOptions init_op)
{
  // freopen("./my_test_log.txt", "w", stdout);
  FLAGS_alsologtostderr = 1;
  google::SetLogDestination(0, "my_test_log.txt");
  util_logging_init("./");
  path_init(init_op.device_working_folder);
  assign_session_specific(init_op.width,
                          init_op.height,
                          init_op.render_device,
                          init_op.device_working_folder,
                          init_op.enable_denoise);

  unity_session_init(init_op.enable_denoise);
  // options.session->wait();
  // session_exit();

  return true;
}

DLL_EXPORT int unity_add_mesh(CyclesMeshData mesh_data, CyclesMtlData *mtls)
{
  // std::string* mat_name_strings = new std::string[mat_num];
  // std::string* diffuse_tex_strings = new std::string[mat_num];
  // for (int i = 0; i < mat_num; ++i)
  //{
  //	mat_name_strings[i] = mat_name[i];
  //	diffuse_tex_strings[i] = diffuse_tex[i];
  //}

  internal_custom_scene(mesh_data, mtls);

  // delete[] mat_name_strings;
  // delete[] diffuse_tex_strings;

  return 0;
}

DLL_EXPORT int unity_add_light(const char *name,
                               float intensity,
                               float radius,
                               float *color,
                               float *dir,
                               float *pos,
                               int type)
{
  // create light
  Light *l = new Light();
  l->use_mis = true;
  l->dir = *(float3 *)dir;
  l->size = radius;
  l->co = *(float3 *)(pos);

  /*
  //The type of a Light.
  Spot = 0,
  Directional = 1,
  Point = 2,
  Area = 3,
  Disc = 4
  */
  if (type == 1) {
    l->type = LIGHT_DISTANT;
  }
  else if (type == 2) {
    l->type = LIGHT_POINT;
  }

  // create light shader
  ShaderGraph *graph = new ShaderGraph();
  EmissionNode *emission = new EmissionNode();
  emission->color = *(float3 *)color;
  emission->strength = intensity;
  graph->add(emission);
  graph->connect(emission->output("Emission"), graph->output()->input("Surface"));
  Shader *p_lshader = new Shader();
  p_lshader->name = name;
  p_lshader->graph = graph;
  options.scene->shaders.push_back(p_lshader);

  // add to scene
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

// typedef void (*render_image_cb)(const char* data, const int w, const int h, const int
// data_type);

DLL_EXPORT int interactive_pt_rendering(UnityRenderOptions u3d_render_options,
                                        Session::render_image_cb icb)
{
  options.width = u3d_render_options.width;
  options.height = u3d_render_options.height;
  // Cycles camera is right hand coordinate, x for right direction, y for up.
  Transform cam_pos = transform_translate(make_float3(u3d_render_options.camera_pos[0],
                                                      u3d_render_options.camera_pos[1],
                                                      u3d_render_options.camera_pos[2]));
  Transform rotate_x = transform_rotate(DEG2RADF(u3d_render_options.euler_angle[0]),
                                        make_float3(1.0f, 0.0f, 0.0f));
  Transform rotate_y = transform_rotate(DEG2RADF(u3d_render_options.euler_angle[1]),
                                        make_float3(0.0f, 1.0f, 0.0f));
  Transform rotate_z = transform_rotate(DEG2RADF(u3d_render_options.euler_angle[2]),
                                        make_float3(0.0f, 0.0f, 1.0f));
  options.scene->camera->matrix = cam_pos * rotate_y * rotate_z * rotate_x;
  options.scene->camera->width = u3d_render_options.width;
  options.scene->camera->height = u3d_render_options.height;
  options.scene->camera->compute_auto_viewplane();
  options.scene->camera->need_update = true;
  options.scene->camera->need_device_update = true;

  options.session_params.samples = u3d_render_options.sample_count;
  // options.session->reset(session_buffer_params(), options.session_params.samples);

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

// CCL_NAMESPACE_END