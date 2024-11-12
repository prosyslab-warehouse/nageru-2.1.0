#include "image_input.h"

#include <errno.h>
#include <movit/flat_input.h>
#include <movit/image_format.h>
#include <movit/util.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <epoxy/egl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "shared/context.h"
#include "shared/ffmpeg_raii.h"
#include "ffmpeg_util.h"
#include "flags.h"

struct SwsContext;

using namespace std;

ImageInput::ImageInput()
	: sRGBSwitchingFlatInput({movit::COLORSPACE_sRGB, movit::GAMMA_sRGB}, movit::FORMAT_RGBA_POSTMULTIPLIED_ALPHA,
	                         GL_UNSIGNED_BYTE, 1280, 720)  // Resolution will be overwritten.
{}

ImageInput::ImageInput(const string &filename)
	: sRGBSwitchingFlatInput({movit::COLORSPACE_sRGB, movit::GAMMA_sRGB}, movit::FORMAT_RGBA_POSTMULTIPLIED_ALPHA,
	                         GL_UNSIGNED_BYTE, 1280, 720),  // Resolution will be overwritten.
	  pathname(search_for_file_or_die(filename)),
	  current_image(load_image(filename, pathname))
{
	if (current_image == nullptr) {  // Could happen even though search_for_file() returned.
		fprintf(stderr, "Couldn't load image, exiting.\n");
		abort();
	}
	set_width(current_image->width);
	set_height(current_image->height);
	set_texture_num(*current_image->tex);
}

void ImageInput::set_gl_state(GLuint glsl_program_num, const string& prefix, unsigned *sampler_num)
{
	// See if the background thread has given us a new version of our image.
	// Note: The old version might still be lying around in other ImageInputs
	// (in fact, it's likely), but at least the total amount of memory used
	// is bounded. Currently we don't even share textures between them,
	// so there's a fair amount of OpenGL memory waste anyway (the cache
	// is mostly there to save startup time, not RAM).
	{
		lock_guard<mutex> lock(all_images_lock);
		assert(all_images.count(pathname));
		if (all_images[pathname] != current_image) {
			current_image = all_images[pathname];
			set_texture_num(*current_image->tex);
		}
	}
	sRGBSwitchingFlatInput::set_gl_state(glsl_program_num, prefix, sampler_num);
}

shared_ptr<const ImageInput::Image> ImageInput::load_image(const string &filename, const string &pathname)
{
	lock_guard<mutex> lock(all_images_lock);  // Held also during loading.
	if (all_images.count(pathname)) {
		return all_images[pathname];
	}

	all_images[pathname] = load_image_raw(pathname);
	return all_images[pathname];
}

shared_ptr<const ImageInput::Image> ImageInput::load_image_raw(const string &pathname)
{
	// Note: Call before open, not after; otherwise, there's a race.
	// (There is now, too, but it tips the correct way. We could use fstat()
	// if we had the file descriptor.)
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		fprintf(stderr, "%s: Error stat-ing file\n", pathname.c_str());
		return nullptr;
	}
	timespec last_modified = buf.st_mtim;

	auto format_ctx = avformat_open_input_unique(pathname.c_str(), nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", pathname.c_str());
		return nullptr;
	}

	if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
		fprintf(stderr, "%s: Error finding stream info\n", pathname.c_str());
		return nullptr;
	}

	int stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_VIDEO);
	if (stream_index == -1) {
		fprintf(stderr, "%s: No video stream found\n", pathname.c_str());
		return nullptr;
	}

	const AVCodecParameters *codecpar = format_ctx->streams[stream_index]->codecpar;
	AVCodecContextWithDeleter codec_ctx = avcodec_alloc_context3_unique(nullptr);
	if (avcodec_parameters_to_context(codec_ctx.get(), codecpar) < 0) {
		fprintf(stderr, "%s: Cannot fill codec parameters\n", pathname.c_str());
		return nullptr;
	}
	const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
	if (codec == nullptr) {
		fprintf(stderr, "%s: Cannot find decoder\n", pathname.c_str());
		return nullptr;
	}
	if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
		fprintf(stderr, "%s: Cannot open decoder\n", pathname.c_str());
		return nullptr;
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> codec_ctx_cleanup(
		codec_ctx.get(), avcodec_close);

	// Read packets until we have a frame or there are none left.
	int frame_finished = 0;
	AVFrameWithDeleter frame = av_frame_alloc_unique();
	bool eof = false;
	do {
		AVPacket pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
			&pkt, av_packet_unref);
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		if (av_read_frame(format_ctx.get(), &pkt) == 0) {
			if (pkt.stream_index != stream_index) {
				continue;
			}
			if (avcodec_send_packet(codec_ctx.get(), &pkt) < 0) {
				fprintf(stderr, "%s: Cannot send packet to codec.\n", pathname.c_str());
				return nullptr;
			}
		} else {
			eof = true;  // Or error, but ignore that for the time being.
		}

		int err = avcodec_receive_frame(codec_ctx.get(), frame.get());
		if (err == 0) {
			frame_finished = true;
			break;
		} else if (err != AVERROR(EAGAIN)) {
			fprintf(stderr, "%s: Cannot receive frame from codec.\n", pathname.c_str());
			return nullptr;
		}
	} while (!eof);

	if (!frame_finished) {
		fprintf(stderr, "%s: Decoder did not output frame.\n", pathname.c_str());
		return nullptr;
	}

	uint8_t *pic_data[4] = {nullptr};
	unique_ptr<uint8_t *, decltype(av_freep)*> pic_data_cleanup(
		&pic_data[0], av_freep);
	int linesizes[4];
	if (av_image_alloc(pic_data, linesizes, frame->width, frame->height, AV_PIX_FMT_RGBA, 1) < 0) {
		fprintf(stderr, "%s: Could not allocate picture data\n", pathname.c_str());
		return nullptr;
	}
	unique_ptr<SwsContext, decltype(sws_freeContext)*> sws_ctx(
		sws_getContext(frame->width, frame->height,
			(AVPixelFormat)frame->format, frame->width, frame->height,
			AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr),
		sws_freeContext);
	if (sws_ctx == nullptr) {
		fprintf(stderr, "%s: Could not create scaler context\n", pathname.c_str());
		return nullptr;
	}
	sws_scale(sws_ctx.get(), frame->data, frame->linesize, 0, frame->height, pic_data, linesizes);

	size_t len = frame->width * frame->height * 4;
	unique_ptr<uint8_t[]> image_data(new uint8_t[len]);
	av_image_copy_to_buffer(image_data.get(), len, pic_data, linesizes, AV_PIX_FMT_RGBA, frame->width, frame->height, 1);

	// Create and upload the texture. We always make mipmaps, since we have
	// generally no idea of all the different chains that might crop up.
	GLuint tex;
	glGenTextures(1, &tex);
	check_error();
	glBindTexture(GL_TEXTURE_2D, tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	// Actual upload.
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	check_error();
	glPixelStorei(GL_UNPACK_ROW_LENGTH, linesizes[0] / 4);
	check_error();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, frame->width, frame->height, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, image_data.get());
	check_error();
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	check_error();

	glGenerateMipmap(GL_TEXTURE_2D);
	check_error();
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();

	shared_ptr<Image> image(new Image{unsigned(frame->width), unsigned(frame->height), UniqueTexture(new GLuint(tex)), last_modified});
	return image;
}

// Fire up a thread to update all images every second.
// We could do inotify, but this is good enough for now.
void ImageInput::update_thread_func(QSurface *surface)
{
	pthread_setname_np(pthread_self(), "Update_Images");

	eglBindAPI(EGL_OPENGL_API);
	QOpenGLContext *context = create_context(surface);
	if (!make_current(context, surface)) {
		printf("Couldn't initialize OpenGL context!\n");
		abort();
	}

	struct stat buf;
	for ( ;; ) {
		{
			unique_lock<mutex> lock(update_thread_should_quit_mu);
			update_thread_should_quit_modified.wait_for(lock, chrono::seconds(1), [] { return update_thread_should_quit; });
		}
		if (update_thread_should_quit) {
			return;
		}

		// Go through all loaded images and see if they need to be updated.
		// We do one pass first through the array with no I/O, to avoid
		// blocking the renderer.
		vector<pair<string, timespec>> images_to_check;
		{
			unique_lock<mutex> lock(all_images_lock);
			for (const auto &pathname_and_image : all_images) {
				const string pathname = pathname_and_image.first;
				const timespec last_modified = pathname_and_image.second->last_modified;
				images_to_check.emplace_back(pathname, last_modified);
			}
		}

		for (const auto &pathname_and_timespec : images_to_check) {
			const string pathname = pathname_and_timespec.first;
			const timespec last_modified = pathname_and_timespec.second;

			if (stat(pathname.c_str(), &buf) != 0) {
				fprintf(stderr, "%s: Couldn't check for new version, leaving the old in place.\n", pathname.c_str());
				continue;
			}
			if (buf.st_mtim.tv_sec == last_modified.tv_sec &&
			    buf.st_mtim.tv_nsec == last_modified.tv_nsec) {
				// Not changed.
				continue;
			}

			shared_ptr<const Image> image = load_image_raw(pathname);
			if (image == nullptr) {
				fprintf(stderr, "Couldn't load image, leaving the old in place.\n");
				continue;
			}

			unique_lock<mutex> lock(all_images_lock);
			all_images[pathname] = image;
		}
	}
}

void ImageInput::switch_image(const string &pathname)
{
#ifndef NDEBUG
	lock_guard<mutex> lock(all_images_lock);
	assert(all_images.count(pathname));
#endif
	this->pathname = pathname;
}

void ImageInput::start_update_thread(QSurface *surface)
{
	update_thread = thread(update_thread_func, surface);
}

void ImageInput::end_update_thread()

{
	{
		lock_guard<mutex> lock(update_thread_should_quit_mu);
		update_thread_should_quit = true;
		update_thread_should_quit_modified.notify_all();
	}
	update_thread.join();
}

mutex ImageInput::all_images_lock;
map<string, shared_ptr<const ImageInput::Image>> ImageInput::all_images;
thread ImageInput::update_thread;
mutex ImageInput::update_thread_should_quit_mu;
bool ImageInput::update_thread_should_quit = false;
condition_variable ImageInput::update_thread_should_quit_modified;
