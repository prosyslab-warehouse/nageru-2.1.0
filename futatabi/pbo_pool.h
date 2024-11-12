#ifndef _PBO_POOL_H
#define _PBO_POOL_H 1

// Keeps a pool of persistently mapped PBOs around that can be used as staging
// buffers for texture uploads. (Uploading from a PBO is asynchronous and done
// by the GPU, so assuming we don't need an extra copy into the PBO, this is a
// significant win over uploading from regular malloc-ed RAM.)
//
// Unlike Nageru's PBOFrameAllocator, these are not connected to
// a given frame, since we can have thousands of frames in the cache
// at any given time. Thus, we need to have separate fences for each PBO
// to know that the upload is done.

#include <mutex>
#include <queue>

#include <epoxy/gl.h>

#include "shared/ref_counted_gl_sync.h"

struct PBO {
	GLuint pbo;
	uint8_t *ptr;  // Mapped memory.
	RefCountedGLsync upload_done;
};

class PBOPool {
public:
	PBOPool(size_t pbo_size = 8 << 20,  // 8 MB, large enough for 1080p 4:2:2.
                size_t num_pbos = 8,
                GLenum buffer = GL_PIXEL_UNPACK_BUFFER_ARB,
                GLenum permissions = GL_MAP_WRITE_BIT,
                GLenum map_bits = GL_MAP_FLUSH_EXPLICIT_BIT);

	PBO alloc_pbo();
	void release_pbo(PBO pbo);  // Set a fence on upload_done if the PBO may still be in use.

private:
	PBO create_pbo();

	std::mutex freelist_mutex;
	std::queue<PBO> freelist;

	size_t pbo_size;
	GLenum buffer, permissions, map_bits;
};

extern PBOPool *global_pbo_pool;
void init_pbo_pool();  // Idempotent.

#endif  // !defined(_PBO_POOL_H)
