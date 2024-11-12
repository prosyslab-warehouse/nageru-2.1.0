
#include <assert.h>

#include "shared/va_resource_pool.h"

using namespace std;

VAResourcePool::VAResources VAResourcePool::get_va_resources(unsigned width, unsigned height, uint32_t fourcc)
{
	{
		lock_guard<mutex> lock(mu);
		for (auto it = freelist.begin(); it != freelist.end(); ++it) {
			if (it->width == width && it->height == height && it->fourcc == fourcc) {
				VAResources ret = *it;
				freelist.erase(it);
				return ret;
			}
		}
	}

	VAResources ret;

	ret.width = width;
	ret.height = height;
	ret.fourcc = fourcc;

	VASurfaceAttrib attrib;
	attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
	attrib.type = VASurfaceAttribPixelFormat;
	attrib.value.type = VAGenericValueTypeInteger;
	attrib.value.value.i = fourcc;

	VAStatus va_status;
	VAConfigID config_id;
	if (fourcc == VA_FOURCC_UYVY) {
		va_status = vaCreateSurfaces(va_dpy, VA_RT_FORMAT_YUV422, width, height, &ret.surface, 1, &attrib, 1);
		config_id = config_id_422;
	} else {
		assert(fourcc == VA_FOURCC_NV12);
		va_status = vaCreateSurfaces(va_dpy, VA_RT_FORMAT_YUV420, width, height, &ret.surface, 1, &attrib, 1);
		config_id = config_id_420;
	}

	va_status = vaCreateContext(va_dpy, config_id, width, height, 0, &ret.surface, 1, &ret.context);
	CHECK_VASTATUS(va_status, "vaCreateContext");

	if (with_data_buffer) {
		va_status = vaCreateBuffer(va_dpy, ret.context, VAEncCodedBufferType, width * height * 3 + 8192, 1, nullptr, &ret.data_buffer);
		CHECK_VASTATUS(va_status, "vaCreateBuffer");
	}

	if (fourcc == VA_FOURCC_UYVY) {
		va_status = vaCreateImage(va_dpy, &uyvy_format, width, height, &ret.image);
		CHECK_VASTATUS(va_status, "vaCreateImage");
	} else {
		assert(fourcc == VA_FOURCC_NV12);
		va_status = vaCreateImage(va_dpy, &nv12_format, width, height, &ret.image);
		CHECK_VASTATUS(va_status, "vaCreateImage");
	}

	return ret;
}

void VAResourcePool::release_va_resources(VAResourcePool::VAResources resources)
{
	lock_guard<mutex> lock(mu);
	if (freelist.size() > 50) {
		auto it = freelist.end();
		--it;

		VAStatus va_status;

		if (with_data_buffer) {
			va_status = vaDestroyBuffer(va_dpy, it->data_buffer);
			CHECK_VASTATUS(va_status, "vaDestroyBuffer");
		}

		va_status = vaDestroyContext(va_dpy, it->context);
		CHECK_VASTATUS(va_status, "vaDestroyContext");

		va_status = vaDestroySurfaces(va_dpy, &it->surface, 1);
		CHECK_VASTATUS(va_status, "vaDestroySurfaces");

		va_status = vaDestroyImage(va_dpy, it->image.image_id);
		CHECK_VASTATUS(va_status, "vaDestroyImage");

		freelist.erase(it);
	}

	freelist.push_front(resources);
}

