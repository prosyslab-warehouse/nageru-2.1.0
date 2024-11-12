#include "pbo_pool.h"

#include <chrono>
#include <mutex>

#include <movit/util.h>

using namespace std;
using namespace std::chrono;

once_flag global_pbo_pool_inited;
PBOPool *global_pbo_pool = nullptr;

void init_pbo_pool()
{
	call_once(global_pbo_pool_inited, []{
		global_pbo_pool = new PBOPool;
	});
}

PBOPool::PBOPool(size_t pbo_size, size_t num_pbos, GLenum buffer, GLenum permissions, GLenum map_bits)
	: pbo_size(pbo_size), buffer(buffer), permissions(permissions), map_bits(map_bits)
{
	for (size_t i = 0; i < num_pbos; ++i) {
		freelist.push(create_pbo());
	}
}

PBO PBOPool::alloc_pbo()
{
	PBO pbo;
	bool found_pbo = false;
	{
		lock_guard<mutex> lock(freelist_mutex);
		if (!freelist.empty()) {
			pbo = move(freelist.front());
			freelist.pop();
			found_pbo = true;
		}
	}

	if (!found_pbo) {
		fprintf(stderr, "WARNING: Out of PBOs for texture upload, creating a new one\n");
		pbo = create_pbo();
	}
	if (pbo.upload_done != nullptr) {
		if (glClientWaitSync(pbo.upload_done.get(), 0, 0) == GL_TIMEOUT_EXPIRED) {
			steady_clock::time_point start = steady_clock::now();
			glClientWaitSync(pbo.upload_done.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
			steady_clock::time_point stop = steady_clock::now();

			fprintf(stderr, "WARNING: PBO was not ready after previous upload, had to wait %.1f ms before reusing\n",
				1e3 * duration<double>(stop - start).count());
		}
		pbo.upload_done.reset();
	}

	return pbo;
}

void PBOPool::release_pbo(PBO pbo)
{
	lock_guard<mutex> lock(freelist_mutex);
	freelist.push(move(pbo));
}

PBO PBOPool::create_pbo()
{
	PBO pbo;
	
	glCreateBuffers(1, &pbo.pbo);
	check_error();
	glNamedBufferStorage(pbo.pbo, pbo_size, nullptr, permissions | GL_MAP_PERSISTENT_BIT);
	check_error();
        pbo.ptr = (uint8_t *)glMapNamedBufferRange(pbo.pbo, 0, pbo_size, permissions | map_bits | GL_MAP_PERSISTENT_BIT);
	check_error();

	return pbo;
}
