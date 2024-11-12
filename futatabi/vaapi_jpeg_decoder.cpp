#include "vaapi_jpeg_decoder.h"

#include "jpeg_destroyer.h"
#include "jpeg_frame.h"
#include "jpeglib_error_wrapper.h"
#include "pbo_pool.h"
#include "shared/memcpy_interleaved.h"
#include "shared/va_display.h"
#include "shared/va_resource_pool.h"

#include <X11/Xlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <jpeglib.h>
#include <list>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_x11.h>

#define BUFFER_OFFSET(i) ((char *)nullptr + (i))

using namespace std;

// TODO: Deduplicate between Nageru and this.
static void memcpy_with_pitch(uint8_t *dst, const uint8_t *src, size_t src_width, size_t dst_pitch, size_t height)
{
	if (src_width == dst_pitch) {
		memcpy(dst, src, src_width * height);
	} else {
		for (size_t y = 0; y < height; ++y) {
			const uint8_t *sptr = src + y * src_width;
			uint8_t *dptr = dst + y * dst_pitch;
			memcpy(dptr, sptr, src_width);
		}
	}
}

static unique_ptr<VADisplayWithCleanup> va_dpy;
static unique_ptr<VAResourcePool> va_pool;

bool vaapi_jpeg_decoding_usable = false;

// From libjpeg (although it's of course identical between implementations).
static const int jpeg_natural_order[DCTSIZE2] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

static unique_ptr<VADisplayWithCleanup> try_open_va_mjpeg(const string &va_display)
{
	VAConfigID config_id_422, config_id_420;
	VAImageFormat uyvy_format, nv12_format;

	// Seemingly VA_FOURCC_422H is no good for vaGetImage(). :-/
	unique_ptr<VADisplayWithCleanup> va_dpy =
		try_open_va(va_display, { VAProfileJPEGBaseline }, VAEntrypointVLD,
			{ { "4:2:2", VA_RT_FORMAT_YUV422, VA_FOURCC_UYVY, &config_id_422, &uyvy_format },
			  { "4:2:0", VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, &config_id_420, &nv12_format } },
			/*chosen_profile=*/nullptr, /*error=*/nullptr);
	if (va_dpy == nullptr) {
		return va_dpy;
	}

	va_pool.reset(new VAResourcePool(va_dpy->va_dpy, uyvy_format, nv12_format, config_id_422, config_id_420, /*with_data_buffer=*/false));

	return va_dpy;
}

string get_usable_va_display()
{
	// Reduce the amount of chatter while probing,
	// unless the user has specified otherwise.
	bool need_env_reset = false;
	if (getenv("LIBVA_MESSAGING_LEVEL") == nullptr) {
		setenv("LIBVA_MESSAGING_LEVEL", "0", true);
		need_env_reset = true;
	}

	// First try the default (ie., whatever $DISPLAY is set to).
	unique_ptr<VADisplayWithCleanup> va_dpy = try_open_va_mjpeg("");
	if (va_dpy != nullptr) {
		if (need_env_reset) {
			unsetenv("LIBVA_MESSAGING_LEVEL");
		}
		return "";
	}

	fprintf(stderr, "The X11 display did not expose a VA-API JPEG decoder.\n");

	// Try all /dev/dri/render* in turn. TODO: Accept /dev/dri/card*, too?
	glob_t g;
	int err = glob("/dev/dri/renderD*", 0, nullptr, &g);
	if (err != 0) {
		fprintf(stderr, "Couldn't list render nodes (%s) when trying to autodetect a replacement.\n", strerror(errno));
	} else {
		for (size_t i = 0; i < g.gl_pathc; ++i) {
			string path = g.gl_pathv[i];
			va_dpy = try_open_va_mjpeg(path);
			if (va_dpy != nullptr) {
				fprintf(stderr, "Autodetected %s as a suitable replacement; using it.\n",
				        path.c_str());
				globfree(&g);
				if (need_env_reset) {
					unsetenv("LIBVA_MESSAGING_LEVEL");
				}
				return path;
			}
		}
	}

	fprintf(stderr, "No suitable VA-API JPEG decoders were found in /dev/dri; giving up.\n");
	fprintf(stderr, "Note that if you are using an Intel CPU with an external GPU,\n");
	fprintf(stderr, "you may need to enable the integrated Intel GPU in your BIOS\n");
	fprintf(stderr, "to expose Quick Sync.\n");
	return "none";
}

void init_jpeg_vaapi()
{
	string dpy = get_usable_va_display();
	if (dpy == "none") {
		return;
	}

	va_dpy = try_open_va_mjpeg(dpy);
	if (va_dpy == nullptr) {
		return;
	}

	fprintf(stderr, "VA-API JPEG decoding initialized.\n");
	vaapi_jpeg_decoding_usable = true;
}

class VABufferDestroyer {
public:
	VABufferDestroyer(VADisplay dpy, VABufferID buf)
		: dpy(dpy), buf(buf) {}

	~VABufferDestroyer()
	{
		VAStatus va_status = vaDestroyBuffer(dpy, buf);
		CHECK_VASTATUS(va_status, "vaDestroyBuffer");
	}

private:
	VADisplay dpy;
	VABufferID buf;
};

shared_ptr<Frame> decode_jpeg_vaapi(const string &jpeg)
{
	jpeg_decompress_struct dinfo;
	JPEGWrapErrorManager error_mgr(&dinfo);
	if (!error_mgr.run([&dinfo] { jpeg_create_decompress(&dinfo); })) {
		return nullptr;
	}
	JPEGDestroyer destroy_dinfo(&dinfo);

	jpeg_save_markers(&dinfo, JPEG_APP0 + 1, 0xFFFF);

	jpeg_mem_src(&dinfo, reinterpret_cast<const unsigned char *>(jpeg.data()), jpeg.size());
	if (!error_mgr.run([&dinfo] { jpeg_read_header(&dinfo, true); })) {
		return nullptr;
	}

	if (dinfo.num_components != 3) {
		fprintf(stderr, "Not a color JPEG. (%d components, Y=%dx%d, Cb=%dx%d, Cr=%dx%d)\n",
		        dinfo.num_components,
		        dinfo.comp_info[0].h_samp_factor, dinfo.comp_info[0].v_samp_factor,
		        dinfo.comp_info[1].h_samp_factor, dinfo.comp_info[1].v_samp_factor,
		        dinfo.comp_info[2].h_samp_factor, dinfo.comp_info[2].v_samp_factor);
		return nullptr;
	}

	const bool is_422 =
		dinfo.comp_info[0].h_samp_factor == 2 &&
		dinfo.comp_info[1].h_samp_factor == 1 &&
		dinfo.comp_info[1].v_samp_factor == dinfo.comp_info[0].v_samp_factor &&
		dinfo.comp_info[2].h_samp_factor == 1 &&
		dinfo.comp_info[2].v_samp_factor == dinfo.comp_info[0].v_samp_factor;
	const bool is_420 =
		dinfo.comp_info[0].h_samp_factor == 2 &&
		dinfo.comp_info[0].v_samp_factor == 2 &&
		dinfo.comp_info[1].h_samp_factor == 1 &&
		dinfo.comp_info[1].v_samp_factor == 1 &&
		dinfo.comp_info[2].h_samp_factor == 1 &&
		dinfo.comp_info[2].v_samp_factor == 1;
	if (!is_422 && !is_420) {
		fprintf(stderr, "Not 4:2:2 or 4:2:0. (Y=%dx%d, Cb=%dx%d, Cr=%dx%d)\n",
		        dinfo.comp_info[0].h_samp_factor, dinfo.comp_info[0].v_samp_factor,
		        dinfo.comp_info[1].h_samp_factor, dinfo.comp_info[1].v_samp_factor,
		        dinfo.comp_info[2].h_samp_factor, dinfo.comp_info[2].v_samp_factor);
		return nullptr;
	}

	// Picture parameters.
	VAPictureParameterBufferJPEGBaseline pic_param;
	memset(&pic_param, 0, sizeof(pic_param));
	pic_param.picture_width = dinfo.image_width;
	pic_param.picture_height = dinfo.image_height;
	for (int component_idx = 0; component_idx < dinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &dinfo.comp_info[component_idx];
		pic_param.components[component_idx].component_id = comp->component_id;
		pic_param.components[component_idx].h_sampling_factor = comp->h_samp_factor;
		pic_param.components[component_idx].v_sampling_factor = comp->v_samp_factor;
		pic_param.components[component_idx].quantiser_table_selector = comp->quant_tbl_no;
	}
	pic_param.num_components = dinfo.num_components;
	pic_param.color_space = 0;  // YUV.
	pic_param.rotation = VA_ROTATION_NONE;

	VAResourcePool::VAResources resources = va_pool->get_va_resources(dinfo.image_width, dinfo.image_height, is_422 ? VA_FOURCC_UYVY : VA_FOURCC_NV12);
	ReleaseVAResources release(va_pool.get(), resources);

	VABufferID pic_param_buffer;
	VAStatus va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_pic_param(va_dpy->va_dpy, pic_param_buffer);

	// Quantization matrices.
	VAIQMatrixBufferJPEGBaseline iq;
	memset(&iq, 0, sizeof(iq));

	for (int quant_tbl_idx = 0; quant_tbl_idx < min(4, NUM_QUANT_TBLS); ++quant_tbl_idx) {
		const JQUANT_TBL *qtbl = dinfo.quant_tbl_ptrs[quant_tbl_idx];
		if (qtbl == nullptr) {
			iq.load_quantiser_table[quant_tbl_idx] = 0;
		} else {
			iq.load_quantiser_table[quant_tbl_idx] = 1;
			for (int i = 0; i < 64; ++i) {
				if (qtbl->quantval[i] > 255) {
					fprintf(stderr, "Baseline JPEG only!\n");
					return nullptr;
				}
				iq.quantiser_table[quant_tbl_idx][i] = qtbl->quantval[jpeg_natural_order[i]];
			}
		}
	}

	VABufferID iq_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAIQMatrixBufferType, sizeof(iq), 1, &iq, &iq_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_iq(va_dpy->va_dpy, iq_buffer);

	// Huffman tables (arithmetic is not supported).
	VAHuffmanTableBufferJPEGBaseline huff;
	memset(&huff, 0, sizeof(huff));

	for (int huff_tbl_idx = 0; huff_tbl_idx < min(2, NUM_HUFF_TBLS); ++huff_tbl_idx) {
		const JHUFF_TBL *ac_hufftbl = dinfo.ac_huff_tbl_ptrs[huff_tbl_idx];
		const JHUFF_TBL *dc_hufftbl = dinfo.dc_huff_tbl_ptrs[huff_tbl_idx];
		if (ac_hufftbl == nullptr) {
			assert(dc_hufftbl == nullptr);
			huff.load_huffman_table[huff_tbl_idx] = 0;
		} else {
			assert(dc_hufftbl != nullptr);
			huff.load_huffman_table[huff_tbl_idx] = 1;

			for (int i = 0; i < 16; ++i) {
				huff.huffman_table[huff_tbl_idx].num_dc_codes[i] = dc_hufftbl->bits[i + 1];
			}
			for (int i = 0; i < 12; ++i) {
				huff.huffman_table[huff_tbl_idx].dc_values[i] = dc_hufftbl->huffval[i];
			}
			for (int i = 0; i < 16; ++i) {
				huff.huffman_table[huff_tbl_idx].num_ac_codes[i] = ac_hufftbl->bits[i + 1];
			}
			for (int i = 0; i < 162; ++i) {
				huff.huffman_table[huff_tbl_idx].ac_values[i] = ac_hufftbl->huffval[i];
			}
		}
	}

	VABufferID huff_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAHuffmanTableBufferType, sizeof(huff), 1, &huff, &huff_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_huff(va_dpy->va_dpy, huff_buffer);

	// Slice parameters (metadata about the slice).
	VASliceParameterBufferJPEGBaseline parms;
	memset(&parms, 0, sizeof(parms));
	parms.slice_data_size = dinfo.src->bytes_in_buffer;
	parms.slice_data_offset = 0;
	parms.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
	parms.slice_horizontal_position = 0;
	parms.slice_vertical_position = 0;
	for (int component_idx = 0; component_idx < dinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &dinfo.comp_info[component_idx];
		parms.components[component_idx].component_selector = comp->component_id;
		parms.components[component_idx].dc_table_selector = comp->dc_tbl_no;
		parms.components[component_idx].ac_table_selector = comp->ac_tbl_no;
		if (parms.components[component_idx].dc_table_selector > 1 ||
		    parms.components[component_idx].ac_table_selector > 1) {
			fprintf(stderr, "Uses too many Huffman tables\n");
			return nullptr;
		}
	}
	parms.num_components = dinfo.num_components;
	parms.restart_interval = dinfo.restart_interval;
	int horiz_mcus = (dinfo.image_width + (DCTSIZE * 2) - 1) / (DCTSIZE * 2);
	int vert_mcus = (dinfo.image_height + DCTSIZE - 1) / DCTSIZE;
	parms.num_mcus = horiz_mcus * vert_mcus;

	VABufferID slice_param_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VASliceParameterBufferType, sizeof(parms), 1, &parms, &slice_param_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_slice_param(va_dpy->va_dpy, slice_param_buffer);

	// The actual data. VA-API will destuff and all for us.
	VABufferID data_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VASliceDataBufferType, dinfo.src->bytes_in_buffer, 1, const_cast<unsigned char *>(dinfo.src->next_input_byte), &data_buffer);
	CHECK_VASTATUS_RET(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_data(va_dpy->va_dpy, data_buffer);

	va_status = vaBeginPicture(va_dpy->va_dpy, resources.context, resources.surface);
	CHECK_VASTATUS_RET(va_status, "vaBeginPicture");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &pic_param_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(pic_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &iq_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(iq)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &huff_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(huff)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &slice_param_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(slice_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &data_buffer, 1);
	CHECK_VASTATUS_RET(va_status, "vaRenderPicture(data)");
	va_status = vaEndPicture(va_dpy->va_dpy, resources.context);
	CHECK_VASTATUS_RET(va_status, "vaEndPicture");

	// vaDeriveImage() works, but the resulting image seems to live in
	// uncached memory, which makes copying data out from it very, very slow.
	// Thanks to FFmpeg for the observation that you can vaGetImage() the
	// surface onto your own image (although then, it can't be planar, which
	// is unfortunate for us).
#if 0
	VAImage image;
	va_status = vaDeriveImage(va_dpy->va_dpy, surf, &image);
	CHECK_VASTATUS_RET(va_status, "vaDeriveImage");
#else
	va_status = vaSyncSurface(va_dpy->va_dpy, resources.surface);
	CHECK_VASTATUS_RET(va_status, "vaSyncSurface");

	va_status = vaGetImage(va_dpy->va_dpy, resources.surface, 0, 0, dinfo.image_width, dinfo.image_height, resources.image.image_id);
	CHECK_VASTATUS_RET(va_status, "vaGetImage");
#endif

	void *mapped;
	va_status = vaMapBuffer(va_dpy->va_dpy, resources.image.buf, &mapped);
	CHECK_VASTATUS_RET(va_status, "vaMapBuffer");

	shared_ptr<Frame> frame(new Frame);
#if 0
	// 4:2:2 planar (for vaDeriveImage).
	frame->y.reset(new uint8_t[dinfo.image_width * dinfo.image_height]);
	frame->cb.reset(new uint8_t[(dinfo.image_width / 2) * dinfo.image_height]);
	frame->cr.reset(new uint8_t[(dinfo.image_width / 2) * dinfo.image_height]);
	for (int component_idx = 0; component_idx < dinfo.num_components; ++component_idx) {
		uint8_t *dptr;
		size_t width;
		if (component_idx == 0) {
			dptr = frame->y.get();
			width = dinfo.image_width;
		} else if (component_idx == 1) {
			dptr = frame->cb.get();
			width = dinfo.image_width / 2;
		} else if (component_idx == 2) {
			dptr = frame->cr.get();
			width = dinfo.image_width / 2;
		} else {
			assert(false);
		}
		const uint8_t *sptr = (const uint8_t *)mapped + image.offsets[component_idx];
		size_t spitch = image.pitches[component_idx];
		for (size_t y = 0; y < dinfo.image_height; ++y) {
			memcpy(dptr + y * width, sptr + y * spitch, width);
		}
	}
#else
	// Convert Y'CbCr to separate Y' and CbCr.
	frame->is_semiplanar = true;

	PBO pbo = global_pbo_pool->alloc_pbo();
	size_t cbcr_offset = dinfo.image_width * dinfo.image_height;
	uint8_t *y_pix = pbo.ptr;
	uint8_t *cbcr_pix = pbo.ptr + cbcr_offset;

	unsigned cbcr_width = dinfo.image_width / 2;
	unsigned cbcr_height;
	if (is_422) {
		const uint8_t *src = (const uint8_t *)mapped + resources.image.offsets[0];
		if (resources.image.pitches[0] == dinfo.image_width * 2) {
			memcpy_interleaved(cbcr_pix, y_pix, src, dinfo.image_width * dinfo.image_height * 2);
		} else {
			for (unsigned y = 0; y < dinfo.image_height; ++y) {
				memcpy_interleaved(cbcr_pix + y * dinfo.image_width, y_pix + y * dinfo.image_width,
						   src + y * resources.image.pitches[0], dinfo.image_width * 2);
			}
		}
		cbcr_height = dinfo.image_height;
	} else {
		assert(is_420);
		const uint8_t *src_y = (const uint8_t *)mapped + resources.image.offsets[0];
		const uint8_t *src_cbcr = (const uint8_t *)mapped + resources.image.offsets[1];
		memcpy_with_pitch(y_pix, src_y, dinfo.image_width, resources.image.pitches[0], dinfo.image_height);
		memcpy_with_pitch(cbcr_pix, src_cbcr, dinfo.image_width, resources.image.pitches[1], dinfo.image_height / 2);
		cbcr_height = dinfo.image_height / 2;
	}

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo.pbo);
	frame->y = create_texture_2d(dinfo.image_width, dinfo.image_height, GL_R8, GL_RED, GL_UNSIGNED_BYTE, BUFFER_OFFSET(0));
	frame->cbcr = create_texture_2d(cbcr_width, cbcr_height, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, BUFFER_OFFSET(cbcr_offset));
	glFlushMappedNamedBufferRange(pbo.pbo, 0, dinfo.image_width * dinfo.image_height + cbcr_width * cbcr_height * 2);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	glMemoryBarrier(GL_PIXEL_BUFFER_BARRIER_BIT);
	pbo.upload_done = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
	frame->uploaded_ui_thread = pbo.upload_done;
	frame->uploaded_interpolation = pbo.upload_done;
	global_pbo_pool->release_pbo(move(pbo));
#endif
	frame->width = dinfo.image_width;
	frame->height = dinfo.image_height;
	frame->chroma_subsampling_x = 2;
	frame->chroma_subsampling_y = is_420 ? 2 : 1;

	if (dinfo.marker_list != nullptr &&
	    dinfo.marker_list->marker == JPEG_APP0 + 1 &&
	    dinfo.marker_list->data_length >= 4 &&
	    memcmp(dinfo.marker_list->data, "Exif", 4) == 0) {
		frame->exif_data.assign(reinterpret_cast<char *>(dinfo.marker_list->data),
			dinfo.marker_list->data_length);
	}

	va_status = vaUnmapBuffer(va_dpy->va_dpy, resources.image.buf);
	CHECK_VASTATUS_RET(va_status, "vaUnmapBuffer");

	return frame;
}
