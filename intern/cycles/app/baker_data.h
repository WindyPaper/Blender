#ifndef _BAKDER_DATA_H_
#define _BAKDER_DATA_H_

#pragma once

struct CyclesBakePixel {
	int primitive_id, object_id;
	float uv[2];
	float du_dx, du_dy;
	float dv_dx, dv_dy;
};

struct BakeImage {
	struct Image* image;
	int width;
	int height;
	size_t offset;
};

struct BakeImages {
	BakeImage* data; /* all the images of an object */
	int* lookup;     /* lookup table from Material to BakeImage */
	int size;
};

#endif
