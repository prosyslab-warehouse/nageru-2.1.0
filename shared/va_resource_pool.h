#ifndef _VA_RESOURCE_POOL
#define _VA_RESOURCE_POOL 1

#include <inttypes.h>
#include <va/va.h>

#include <list>
#include <mutex>

#define CHECK_VASTATUS(va_status, func) \
	if (va_status != VA_STATUS_SUCCESS) { \
		fprintf(stderr, "%s:%d (%s) failed: %s\n", __func__, __LINE__, func, vaErrorStr(va_status)); \
		exit(1); \
	}

#define CHECK_VASTATUS_RET(va_status, func) \
	if (va_status != VA_STATUS_SUCCESS) { \
		fprintf(stderr, "%s:%d (%s) failed with %d\n", __func__, __LINE__, func, va_status); \
		return nullptr; \
	}

class VAResourcePool {
public:
	struct VAResources {
		unsigned width, height;
		uint32_t fourcc;
		VASurfaceID surface;
		VAContextID context;
		VABufferID data_buffer;
		VAImage image;
	};

	VAResourcePool(VADisplay va_dpy, VAImageFormat uyvy_format, VAImageFormat nv12_format, VAConfigID config_id_422, VAConfigID config_id_420, bool with_data_buffer)
		: va_dpy(va_dpy),
		  uyvy_format(uyvy_format),
		  nv12_format(nv12_format),
		  config_id_422(config_id_422),
		  config_id_420(config_id_420),
		  with_data_buffer(with_data_buffer) {}
	VAResources get_va_resources(unsigned width, unsigned height, uint32_t fourcc);
	void release_va_resources(VAResources resources);

private:
	const VADisplay va_dpy;
	VAImageFormat uyvy_format, nv12_format;
	const VAConfigID config_id_422, config_id_420;
	const bool with_data_buffer;

	std::mutex mu;
	std::list<VAResources> freelist;  // Under mu.
};

// RAII wrapper to release VAResources on return (even on error).
class ReleaseVAResources {
public:
	ReleaseVAResources() : committed(true) {}

	ReleaseVAResources(VAResourcePool *pool, const VAResourcePool::VAResources &resources)
		: pool(pool), resources(resources) {}

	ReleaseVAResources(ReleaseVAResources &) = delete;

	ReleaseVAResources(ReleaseVAResources &&other)
		: pool(other.pool), resources(other.resources), committed(other.committed) {
		other.commit();
	}

	ReleaseVAResources &operator= (ReleaseVAResources &) = delete;

	ReleaseVAResources &operator= (ReleaseVAResources &&other) {
		if (!committed) {
			pool->release_va_resources(resources);
		}
		pool = other.pool;
		resources = std::move(other.resources);
		committed = other.committed;
		other.commit();
		return *this;
	}

	~ReleaseVAResources()
	{
		if (!committed) {
			pool->release_va_resources(resources);
		}
	}

	void commit() { committed = true; }

private:
	VAResourcePool *pool = nullptr;
	VAResourcePool::VAResources resources;
	bool committed = false;
};

#endif  // !defined(_VA_RESOURCE_POOL)
