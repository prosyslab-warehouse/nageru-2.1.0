#include "pbo_frame_allocator.h"

#include <bmusb/bmusb.h>
#include <movit/util.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <cstddef>

#include "flags.h"
#include "mjpeg_encoder.h"
#include "v210_converter.h"
#include "shared/va_display.h"

using namespace std;

namespace {

void set_clamp_to_edge()
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();
}

}  // namespace

PBOFrameAllocator::PBOFrameAllocator(bmusb::PixelFormat pixel_format, size_t frame_size, GLuint width, GLuint height, unsigned card_index, MJPEGEncoder *mjpeg_encoder, size_t num_queued_frames, GLenum buffer, GLenum permissions, GLenum map_bits)
        : card_index(card_index),
	  mjpeg_encoder(mjpeg_encoder),
	  pixel_format(pixel_format),
	  buffer(buffer),
	  frame_size(frame_size),
	  num_queued_frames(num_queued_frames),
	  width(width),
	  height(height),
	  permissions(permissions),
	  map_bits(map_bits)
{
	userdata.reset(new Userdata[num_queued_frames]);
	for (size_t i = 0; i < num_queued_frames; ++i) {
		init_frame(i, frame_size, width, height, permissions, map_bits, generation);
	}
	glBindBuffer(buffer, 0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
}

void PBOFrameAllocator::init_frame(size_t frame_idx, size_t frame_size, GLuint width, GLuint height, GLenum permissions, GLenum map_bits, int generation)
{
	GLuint pbo;
	glGenBuffers(1, &pbo);
	check_error();
	glBindBuffer(buffer, pbo);
	check_error();
	glBufferStorage(buffer, frame_size, nullptr, permissions | GL_MAP_PERSISTENT_BIT);
	check_error();

	Frame frame;
	frame.data = (uint8_t *)glMapBufferRange(buffer, 0, frame_size, permissions | map_bits | GL_MAP_PERSISTENT_BIT);
	frame.data2 = frame.data + frame_size / 2;
	check_error();
	frame.size = frame_size;
	Userdata *ud = &userdata[frame_idx];
	frame.userdata = ud;
	ud->generation = generation;
	ud->pbo = pbo;
	ud->pixel_format = pixel_format;
	ud->data_copy_malloc = new uint8_t[frame_size];
	frame.owner = this;

	// For 8-bit non-planar Y'CbCr, we ask the driver to split Y' and Cb/Cr
	// into separate textures. For 10-bit, the input format (v210)
	// is complicated enough that we need to interpolate up to 4:4:4,
	// which we do in a compute shader ourselves. For BGRA, the data
	// is already 4:4:4:4.
	frame.interleaved = (pixel_format == bmusb::PixelFormat_8BitYCbCr);

	// Create textures. We don't allocate any data for the second field at this point
	// (just create the texture state with the samplers), since our default assumed
	// resolution is progressive.
	switch (pixel_format) {
	case bmusb::PixelFormat_8BitYCbCr:
		glGenTextures(2, ud->tex_y);
		check_error();
		glGenTextures(2, ud->tex_cbcr);
		check_error();
		break;
	case bmusb::PixelFormat_10BitYCbCr:
		glGenTextures(2, ud->tex_v210);
		check_error();
		glGenTextures(2, ud->tex_444);
		check_error();
		break;
	case bmusb::PixelFormat_8BitBGRA:
		glGenTextures(2, ud->tex_rgba);
		check_error();
		break;
	case bmusb::PixelFormat_8BitYCbCrPlanar:
		glGenTextures(2, ud->tex_y);
		check_error();
		glGenTextures(2, ud->tex_cb);
		check_error();
		glGenTextures(2, ud->tex_cr);
		check_error();
		break;
	default:
		assert(false);
	}

	ud->last_width[0] = width;
	ud->last_height[0] = height;
	ud->last_cbcr_width[0] = width / 2;
	ud->last_cbcr_height[0] = height;
	ud->last_v210_width[0] = 0;

	ud->last_width[1] = 0;
	ud->last_height[1] = 0;
	ud->last_cbcr_width[1] = 0;
	ud->last_cbcr_height[1] = 0;
	ud->last_v210_width[1] = 0;

	ud->last_interlaced = false;
	ud->last_has_signal = false;
	ud->last_is_connected = false;
	for (unsigned field = 0; field < 2; ++field) {
		switch (pixel_format) {
		case bmusb::PixelFormat_10BitYCbCr: {
			const size_t v210_width = v210Converter::get_minimum_v210_texture_width(width);

			// Seemingly we need to set the minification filter even though
			// shader image loads don't use them, or NVIDIA will just give us
			// zero back.
			glBindTexture(GL_TEXTURE_2D, ud->tex_v210[field]);
			check_error();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			check_error();
			if (field == 0) {
				ud->last_v210_width[0] = v210_width;
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, v210_width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
				check_error();
			}

			glBindTexture(GL_TEXTURE_2D, ud->tex_444[field]);
			check_error();
			set_clamp_to_edge();
			if (field == 0) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
				check_error();
			}
			break;
		}
		case bmusb::PixelFormat_8BitYCbCr:
			glBindTexture(GL_TEXTURE_2D, ud->tex_y[field]);
			check_error();
			set_clamp_to_edge();
			if (field == 0) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
				check_error();
			}

			glBindTexture(GL_TEXTURE_2D, ud->tex_cbcr[field]);
			check_error();
			set_clamp_to_edge();
			if (field == 0) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
				check_error();
			}
			break;
		case bmusb::PixelFormat_8BitBGRA:
			glBindTexture(GL_TEXTURE_2D, ud->tex_rgba[field]);
			check_error();
			set_clamp_to_edge();
			if (field == 0) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
				check_error();
			}
			break;
		case bmusb::PixelFormat_8BitYCbCrPlanar:
			glBindTexture(GL_TEXTURE_2D, ud->tex_y[field]);
			check_error();
			set_clamp_to_edge();
			if (field == 0) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
				check_error();
			}

			glBindTexture(GL_TEXTURE_2D, ud->tex_cb[field]);
			check_error();
			set_clamp_to_edge();
			if (field == 0) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width / 2, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
				check_error();
			}

			glBindTexture(GL_TEXTURE_2D, ud->tex_cr[field]);
			check_error();
			set_clamp_to_edge();
			if (field == 0) {
				glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width / 2, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
				check_error();
			}
			break;
		default:
			assert(false);
		}
	}

	freelist.push(frame);
}

PBOFrameAllocator::~PBOFrameAllocator()
{
	while (!freelist.empty()) {
		Frame frame = freelist.front();
		freelist.pop();
		destroy_frame(&frame);
	}
}

void PBOFrameAllocator::destroy_frame(Frame *frame)
{
	Userdata *ud = (Userdata *)frame->userdata;
	delete[] ud->data_copy_malloc;

	GLuint pbo = ud->pbo;
	glBindBuffer(buffer, pbo);
	check_error();
	glUnmapBuffer(buffer);
	check_error();
	glBindBuffer(buffer, 0);
	check_error();
	glDeleteBuffers(1, &pbo);
	check_error();
	switch (ud->pixel_format) {
	case bmusb::PixelFormat_10BitYCbCr:
		glDeleteTextures(2, ud->tex_v210);
		check_error();
		glDeleteTextures(2, ud->tex_444);
		check_error();
		break;
	case bmusb::PixelFormat_8BitYCbCr:
		glDeleteTextures(2, ud->tex_y);
		check_error();
		glDeleteTextures(2, ud->tex_cbcr);
		check_error();
		break;
	case bmusb::PixelFormat_8BitBGRA:
		glDeleteTextures(2, ud->tex_rgba);
		check_error();
		break;
	case bmusb::PixelFormat_8BitYCbCrPlanar:
		glDeleteTextures(2, ud->tex_y);
		check_error();
		glDeleteTextures(2, ud->tex_cb);
		check_error();
		glDeleteTextures(2, ud->tex_cr);
		check_error();
		break;
	default:
		assert(false);
	}

	if (ud->generation != generation) {
		auto it = lingering_generations.find(ud->generation);
		assert(it != lingering_generations.end());
		if (--it->second.num_frames_left == 0) {
			lingering_generations.erase(it);  // Deallocates the userdata block.
		}
	}
}
//static int sumsum = 0;

bmusb::FrameAllocator::Frame PBOFrameAllocator::alloc_frame()
{
        Frame vf;

	lock_guard<mutex> lock(freelist_mutex);  // Meh.
	if (freelist.empty()) {
		printf("Frame overrun (no more spare PBO frames), dropping frame!\n");
	} else {
		//fprintf(stderr, "freelist has %d allocated\n", ++sumsum);
		vf = freelist.front();
		freelist.pop();  // Meh.
	}
	vf.len = 0;
	vf.overflow = 0;

	if (mjpeg_encoder != nullptr &&
	    mjpeg_encoder->should_encode_mjpeg_for_card(card_index) &&
	    vf.userdata != nullptr) {
		Userdata *ud = (Userdata *)vf.userdata;
		vf.data_copy = ud->data_copy_malloc;
		ud->data_copy_current_src = Userdata::FROM_MALLOC;
	} else {
		vf.data_copy = nullptr;
	}

	return vf;
}

bmusb::FrameAllocator::Frame PBOFrameAllocator::create_frame(size_t width, size_t height, size_t stride)
{
        Frame vf;

	{
		lock_guard<mutex> lock(freelist_mutex);
		if (freelist.empty()) {
			printf("Frame overrun (no more spare PBO frames), dropping frame!\n");
			vf.len = 0;
			vf.overflow = 0;
			return vf;
		} else {
			vf = freelist.front();
			freelist.pop();
		}
	}
	vf.len = 0;
	vf.overflow = 0;

	Userdata *userdata = (Userdata *)vf.userdata;

	if (mjpeg_encoder != nullptr &&
	    mjpeg_encoder->should_encode_mjpeg_for_card(card_index)) {
		if (mjpeg_encoder->using_vaapi()) {
			VADisplay va_dpy = mjpeg_encoder->va_dpy->va_dpy;
			VAResourcePool::VAResources resources = mjpeg_encoder->get_va_pool()->get_va_resources(width, height, VA_FOURCC_UYVY);  // Only used by DeckLinkCapture, so always 4:2:2.
			ReleaseVAResources release(mjpeg_encoder->get_va_pool(), resources);

			if (resources.image.pitches[0] == stride) {
				userdata->va_resources = move(resources);
				userdata->va_resources_release = move(release);

				VAStatus va_status = vaMapBuffer(va_dpy, resources.image.buf, (void **)&vf.data_copy);
				CHECK_VASTATUS(va_status, "vaMapBuffer");
				vf.data_copy += resources.image.offsets[0];
				userdata->data_copy_current_src = Userdata::FROM_VA_API;
			} else {
				printf("WARNING: Could not copy directly into VA-API MJPEG buffer for %zu x %zu, since producer and consumer disagreed on stride (%zu != %d).\n", width, height, stride, resources.image.pitches[0]);
				vf.data_copy = userdata->data_copy_malloc;
				userdata->data_copy_current_src = Userdata::FROM_MALLOC;
			}
		} else {
			vf.data_copy = userdata->data_copy_malloc;
			userdata->data_copy_current_src = Userdata::FROM_MALLOC;
		}
	} else {
		vf.data_copy = nullptr;
	}

	return vf;
}

void PBOFrameAllocator::release_frame(Frame frame)
{
	if (frame.overflow > 0) {
		printf("%d bytes overflow after last (PBO) frame\n", int(frame.overflow));
	}

#if 0
	// Poison the page. (Note that this might be bogus if you don't have an OpenGL context.)
	memset(frame.data, 0, frame.size);
	Userdata *userdata = (Userdata *)frame.userdata;
	for (unsigned field = 0; field < 2; ++field) {
		glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		check_error();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, userdata->last_width[field], userdata->last_height[field], 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		check_error();

		glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr[field]);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		check_error();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, userdata->last_width[field] / 2, userdata->last_height[field], 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		check_error();
	}
#endif

	{
		// In case we never got to upload the frame to MJPEGEncoder.
		Userdata *userdata = (Userdata *)frame.userdata;
		VAResourcePool::VAResources resources __attribute__((unused)) = move(userdata->va_resources);
		ReleaseVAResources release = move(userdata->va_resources_release);

		if (frame.data_copy != nullptr && userdata->data_copy_current_src == Userdata::FROM_VA_API) {
			VADisplay va_dpy = mjpeg_encoder->va_dpy->va_dpy;
			VAStatus va_status = vaUnmapBuffer(va_dpy, resources.image.buf);
			CHECK_VASTATUS(va_status, "vaUnmapBuffer");

			frame.data_copy = nullptr;
		}
	}

	lock_guard<mutex> lock(freelist_mutex);
	Userdata *userdata = (Userdata *)frame.userdata;
	if (userdata->generation == generation) {
		freelist.push(frame);
	} else {
		destroy_frame(&frame);
	}
	//--sumsum;
}

void PBOFrameAllocator::reconfigure(bmusb::PixelFormat pixel_format,
	                 size_t frame_size,
	                 GLuint width, GLuint height,
	                 unsigned card_index,
	                 MJPEGEncoder *mjpeg_encoder,
	                 size_t num_queued_frames,
	                 GLenum buffer,
	                 GLenum permissions,
	                 GLenum map_bits)
{
	if (pixel_format == this->pixel_format &&
	    frame_size == this->frame_size &&
	    width == this->width && height == this->height &&
	    card_index == this->card_index &&
	    mjpeg_encoder == this->mjpeg_encoder &&
	    num_queued_frames == this->num_queued_frames &&
	    buffer == this->buffer &&
	    permissions == this->permissions &&
	    map_bits == this->map_bits) {
		return;
	}

	lock_guard<mutex> lock(freelist_mutex);
	lingering_generations[generation] = LingeringGeneration{ move(userdata), this->num_queued_frames };
	++generation;

	while (!freelist.empty()) {
		Frame frame = freelist.front();
		freelist.pop();
		destroy_frame(&frame);
	}

	this->pixel_format = pixel_format;
	this->frame_size = frame_size;
	this->width = width;
	this->height = height;
	this->card_index = card_index;
	this->mjpeg_encoder = mjpeg_encoder;
	this->num_queued_frames = num_queued_frames;
	this->buffer = buffer;
	this->permissions = permissions;
	this->map_bits = map_bits;

	userdata.reset(new Userdata[num_queued_frames]);
	for (size_t i = 0; i < num_queued_frames; ++i) {
		init_frame(i, frame_size, width, height, permissions, map_bits, generation);
	}

	// There may still be frames out with the old configuration
	// (for instance, living in GLWidget); they will be destroyed
	// when they come back in release_frame().
}
