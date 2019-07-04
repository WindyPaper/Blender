#ifndef _GENERATE_MIKK_TANGENT_H_
#define _GENERATE_MIKK_TANGENT_H_

#pragma once

CCL_NAMESPACE_BEGIN
class Mesh;
class float3;
CCL_NAMESPACE_END


struct MikkUserData
{
	MikkUserData(ccl::Mesh* cycle_mesh);

	ccl::Mesh* mesh;
	ccl::float3* texface;

	ccl::float3* vertex_normal;

	ccl::float3* tangent;
	float* tangent_sign;
};

#endif // !_GENERATE_MIKK_TANGENT_H_



