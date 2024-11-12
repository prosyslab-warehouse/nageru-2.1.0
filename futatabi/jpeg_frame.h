#ifndef _JPEG_FRAME_H
#define _JPEG_FRAME_H 1

#include <memory>
#include <string>

#include "shared/ref_counted_gl_sync.h"
#include "shared/ref_counted_texture.h"

struct Frame {
	bool is_semiplanar = false;
	RefCountedTexture y;
	RefCountedTexture cb, cr;  // For planar.
	RefCountedTexture cbcr;  // For semiplanar.
	unsigned width, height;
	unsigned chroma_subsampling_x, chroma_subsampling_y;
	std::string exif_data;
	RefCountedGLsync uploaded_ui_thread;
	RefCountedGLsync uploaded_interpolation;
};

#endif  // !defined(_JPEG_FRAME_H)
