#include "GenerateMikkTangent.h"
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

using namespace ccl;
OIIO_NAMESPACE_USING

MikkUserData::MikkUserData(ccl::Mesh* cycle_mesh) :
	mesh(cycle_mesh),
	texface(NULL),
	vertex_normal(NULL),
	tangent(NULL),
	tangent_sign(NULL)
{
	//texface = 
	AttributeSet& attributes = (mesh->subd_faces.size()) ?
		mesh->subd_attributes : mesh->attributes;

	texface = attributes.find(ATTR_STD_UV)->data_float3();
	Attribute* attr_vN = attributes.find(ATTR_STD_VERTEX_NORMAL);
	vertex_normal = attr_vN->data_float3();

	Attribute* attr = attributes.add(ATTR_STD_UV_TANGENT, ustring("Tangent"));
	tangent = attr->data_float3();

	Attribute* attr_sign = attributes.add(ATTR_STD_UV_TANGENT_SIGN, ustring("TangentSign"));
	tangent_sign = attr_sign->data_float();
}
