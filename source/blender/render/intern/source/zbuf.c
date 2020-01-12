/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 */


/** \file
 * \ingroup render
 */




  /*---------------------------------------------------------------------------*/
  /* Common includes                                                           */
  /*---------------------------------------------------------------------------*/

#include <string.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.h"
#include "BLI_math_vector.h"

/* own includes */
#include "zbuf.h"

/* could enable at some point but for now there are far too many conversions */
#ifdef __GNUC__
#  pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

/* ****************** Spans ******************************* */

/* each zbuffer has coordinates transformed to local rect coordinates, so we can simply clip */
void zbuf_alloc_span(ZSpan* zspan, int rectx, int recty)
{
	memset(zspan, 0, sizeof(ZSpan));

	zspan->rectx = rectx;
	zspan->recty = recty;

	zspan->span1 = MEM_mallocN(recty * sizeof(float), "zspan");
	zspan->span2 = MEM_mallocN(recty * sizeof(float), "zspan");
}

void zbuf_free_span(ZSpan* zspan)
{
	if (zspan) {
		if (zspan->span1) {
			MEM_freeN(zspan->span1);
		}
		if (zspan->span2) {
			MEM_freeN(zspan->span2);
		}
		zspan->span1 = zspan->span2 = NULL;
	}
}

/* reset range for clipping */
static void zbuf_init_span(ZSpan* zspan)
{
	zspan->miny1 = zspan->miny2 = zspan->recty + 1;
	zspan->maxy1 = zspan->maxy2 = -1;
	zspan->minp1 = zspan->maxp1 = zspan->minp2 = zspan->maxp2 = NULL;
}

static void zbuf_add_to_span(ZSpan* zspan, const float v1[2], const float v2[2])
{
	const float* minv, * maxv;
	float* span;
	float xx1, dx0, xs0;
	int y, my0, my2;

	if (v1[1] < v2[1]) {  // y value
		minv = v1;
		maxv = v2;
	}
	else {
		minv = v2;
		maxv = v1;
	}

	my0 = ceil(minv[1]);
	my2 = floor(maxv[1]);

	if (my2 < 0 || my0 >= zspan->recty) {  // empty
		return;
	}

	/* clip top */
	if (my2 >= zspan->recty) {
		my2 = zspan->recty - 1;
	}
	/* clip bottom */
	if (my0 < 0) {
		my0 = 0;
	}

	if (my0 > my2) {
		return;
	}
	/* if (my0>my2) should still fill in, that way we get spans that skip nicely */

	xx1 = maxv[1] - minv[1];  // y offset
	if (xx1 > FLT_EPSILON) {
		dx0 = (minv[0] - maxv[0]) / xx1;
		xs0 = dx0 * (minv[1] - my2) + minv[0];
	}
	else {
		dx0 = 0.0f;
		xs0 = min_ff(minv[0], maxv[0]);
	}

	/* empty span */
	if (zspan->maxp1 == NULL) {
		span = zspan->span1;
	}
	else { /* does it complete left span? */
		if (maxv == zspan->minp1 || minv == zspan->maxp1) {
			span = zspan->span1;
		}
		else {
			span = zspan->span2;
		}
	}

	if (span == zspan->span1) {
		//      printf("left span my0 %d my2 %d\n", my0, my2);
		if (zspan->minp1 == NULL || zspan->minp1[1] > minv[1]) {
			zspan->minp1 = minv;
		}
		if (zspan->maxp1 == NULL || zspan->maxp1[1] < maxv[1]) {
			zspan->maxp1 = maxv;
		}
		if (my0 < zspan->miny1) {
			zspan->miny1 = my0;
		}
		if (my2 > zspan->maxy1) {
			zspan->maxy1 = my2;
		}
	}
	else {
		//      printf("right span my0 %d my2 %d\n", my0, my2);
		if (zspan->minp2 == NULL || zspan->minp2[1] > minv[1]) {
			zspan->minp2 = minv;
		}
		if (zspan->maxp2 == NULL || zspan->maxp2[1] < maxv[1]) {
			zspan->maxp2 = maxv;
		}
		if (my0 < zspan->miny2) {
			zspan->miny2 = my0;
		}
		if (my2 > zspan->maxy2) {
			zspan->maxy2 = my2;
		}
	}

	for (y = my2; y >= my0; y--, xs0 += dx0) {
		/* xs0 is the xcoord! */
		span[y] = xs0;
	}
}

// Get triangle centroid
void get_triangle_centroid(const float* v0, const float* v1, const float* v2, float* out)
{
	out[0] = (v0[0] + v1[0] + v2[0]) / 3.0f;
	out[1] = (v0[1] + v1[1] + v2[1]) / 3.0f;
}

void lm_toBarycentric(
	const float* p1, const float* p2, const float* p3, const float* p, float* out_uv)
{
	// http://www.blackpawn.com/texts/pointinpoly/
	// Compute vectors
	float v0[2];
	sub_v2_v2v2(v0, p1, p3);

	float v1[2];
	sub_v2_v2v2(v1, p2, p3);
	float v2[2];
	sub_v2_v2v2(v2, p, p3);

	// Compute dot products
	float dot00 = dot_v2v2(v0, v0);
	float dot01 = dot_v2v2(v0, v1);
	float dot02 = dot_v2v2(v0, v2);
	float dot11 = dot_v2v2(v1, v1);
	float dot12 = dot_v2v2(v1, v2);
	// Compute barycentric coordinates
	float invDenom = 1.0f / (dot00 * dot11 - dot01 * dot01);
	out_uv[0] = (dot11 * dot02 - dot01 * dot12) * invDenom;
	out_uv[1] = (dot00 * dot12 - dot01 * dot02) * invDenom;
}

int orient2d(const float* a, const float* b, const float* c)
{
	float ret = ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
	return (int)(ret);
}

bool is_top_left(const float* v0, const float* v1)
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

/*-----------------------------------------------------------*/
/* Functions                                                 */
/*-----------------------------------------------------------*/

/* Scanconvert for strand triangles, calls func for each x, y coordinate
 * and gives UV barycentrics and z. */

void zspan_scanconvert(ZSpan* zspan,
	void* handle,
	float* v1,
	float* v2,
	float* v3,
	void (*func)(void*, int, int, float, float))
{
	// float x0, y0, x1, y1, x2, y2, z0, z1, z2;
	// float u, v, uxd, uyd, vxd, vyd, uy0, vy0, xx1;
	// const float *span1, *span2;
	// int i, j, x, y, sn1, sn2, rectx = zspan->rectx, my0, my2;

	///* init */
	// zbuf_init_span(zspan);

	///* set spans */
	// zbuf_add_to_span(zspan, v1, v2);
	// zbuf_add_to_span(zspan, v2, v3);
	// zbuf_add_to_span(zspan, v3, v1);

	///* clipped */
	// if (zspan->minp2 == NULL || zspan->maxp2 == NULL) {
	//  return;
	//}

	// my0 = max_ii(zspan->miny1, zspan->miny2);
	// my2 = min_ii(zspan->maxy1, zspan->maxy2);

	////  printf("my %d %d\n", my0, my2);
	// if (my2 < my0) {
	//  return;
	//}

	///* ZBUF DX DY, in floats still */
	// x1 = v1[0] - v2[0];
	// x2 = v2[0] - v3[0];
	// y1 = v1[1] - v2[1];
	// y2 = v2[1] - v3[1];

	// z1 = 1.0f; /* (u1 - u2) */
	// z2 = 0.0f; /* (u2 - u3) */

	// x0 = y1 * z2 - z1 * y2;
	// y0 = z1 * x2 - x1 * z2;
	// z0 = x1 * y2 - y1 * x2;

	// if (z0 == 0.0f) {
	//  return;
	//}

	// xx1 = (x0 * v1[0] + y0 * v1[1]) / z0 + 1.0f;
	// uxd = -(double)x0 / (double)z0;
	// uyd = -(double)y0 / (double)z0;
	// uy0 = ((double)my2) * uyd + (double)xx1;

	// z1 = -1.0f; /* (v1 - v2) */
	// z2 = 1.0f;  /* (v2 - v3) */

	// x0 = y1 * z2 - z1 * y2;
	// y0 = z1 * x2 - x1 * z2;

	// xx1 = (x0 * v1[0] + y0 * v1[1]) / z0;
	// vxd = -(double)x0 / (double)z0;
	// vyd = -(double)y0 / (double)z0;
	// vy0 = ((double)my2) * vyd + (double)xx1;

	///* correct span */
	// span1 = zspan->span1 + my2;
	// span2 = zspan->span2 + my2;

	// for (i = 0, y = my2; y >= my0; i++, y--, span1--, span2--) {

	//  sn1 = floor(min_ff(*span1, *span2));
	//  sn2 = floor(max_ff(*span1, *span2));
	//  sn1++;

	//  if (sn2 >= rectx) {
	//    sn2 = rectx - 1;
	//  }
	//  if (sn1 < 0) {
	//    sn1 = 0;
	//  }

	//  u = (((double)sn1 * uxd) + uy0) - (i * uyd);
	//  v = (((double)sn1 * vxd) + vy0) - (i * vyd);

	//  for (j = 0, x = sn1; x <= sn2; j++, x++) {
	//    func(handle, x, y, u + (j * uxd), v + (j * vxd));
	//  }
	//}

	/*float centroid[2];
	get_triangle_centroid(v1, v2, v3, centroid);

	float uv[2];
	lm_toBarycentric(v1, v2, v3, centroid, uv);

	func(handle, 0, 0, uv[0], uv[1]);*/

	// rasterization
	int max_x = zspan->rectx;
	int max_y = zspan->recty;

	//std::vector<float> img_x{ v1[0], v2[0], v3[0] };
	//float img_y[] = { v1[1], v2[1], v3[1] };
	//float max_img_x = *std::max_element(img_x, img_x + 3);

	int bias_v1_v2 = is_top_left(v1, v2) ? 0 : -1;
	int bias_v2_v3 = is_top_left(v2, v3) ? 0 : -1;
	int bias_v3_v1 = is_top_left(v3, v1) ? 0 : -1;

	for (int y = 0; y < max_y; ++y) {
		for (int x = 0; x < max_x; ++x) {
			float curr_pixel[2] = { x, y };

			int w0 = orient2d(v2, v3, curr_pixel) + bias_v2_v3;
			int w1 = orient2d(v3, v1, curr_pixel) + bias_v3_v1;
			int w2 = orient2d(v1, v2, curr_pixel) + bias_v1_v2;			

			const int eps = 0;

			//if (fabsf(w0) < eps || fabsf(w1) < eps || fabsf(w2) < eps)
			//{
			//	continue;
			//}

			if (w0 >= eps && w1 >= eps && w2 >= eps) {
				float uv[2];
				lm_toBarycentric(v1, v2, v3, curr_pixel, uv);

				func(handle, x, y, uv[0], uv[1]);
			}
		}
	}
}

/* end of zbuf.c */
