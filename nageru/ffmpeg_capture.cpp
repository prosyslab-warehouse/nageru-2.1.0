#include "ffmpeg_capture.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/LU>
#include <movit/colorspace_conversion_effect.h>

#include "bmusb/bmusb.h"
#include "shared/ffmpeg_raii.h"
#include "ffmpeg_util.h"
#include "flags.h"
#include "image_input.h"
#include "ref_counted_frame.h"
#include "shared/timebase.h"

#ifdef HAVE_SRT
#include <srt/srt.h>
#endif

#define FRAME_SIZE (8 << 20)  // 8 MB.

using namespace std;
using namespace std::chrono;
using namespace bmusb;
using namespace movit;
using namespace Eigen;

namespace {

steady_clock::time_point compute_frame_start(int64_t frame_pts, int64_t pts_origin, const AVRational &video_timebase, const steady_clock::time_point &origin, double rate)
{
	const duration<double> pts((frame_pts - pts_origin) * double(video_timebase.num) / double(video_timebase.den));
	return origin + duration_cast<steady_clock::duration>(pts / rate);
}

bool changed_since(const std::string &pathname, const timespec &ts)
{
	if (ts.tv_sec < 0) {
		return false;
	}
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		fprintf(stderr, "%s: Couldn't check for new version, leaving the old in place.\n", pathname.c_str());
		return false;
	}
	return (buf.st_mtim.tv_sec != ts.tv_sec || buf.st_mtim.tv_nsec != ts.tv_nsec);
}

bool is_full_range(const AVPixFmtDescriptor *desc)
{
	// This is horrible, but there's no better way that I know of.
	return (strchr(desc->name, 'j') != nullptr);
}

AVPixelFormat decide_dst_format(AVPixelFormat src_format, bmusb::PixelFormat dst_format_type)
{
	if (dst_format_type == bmusb::PixelFormat_8BitBGRA) {
		return AV_PIX_FMT_BGRA;
	}
	if (dst_format_type == FFmpegCapture::PixelFormat_NV12) {
		return AV_PIX_FMT_NV12;
	}

	assert(dst_format_type == bmusb::PixelFormat_8BitYCbCrPlanar);

	// If this is a non-Y'CbCr format, just convert to 4:4:4 Y'CbCr
	// and be done with it. It's too strange to spend a lot of time on.
	// (Let's hope there's no alpha.)
	const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(src_format);
	if (src_desc == nullptr ||
	    src_desc->nb_components != 3 ||
	    (src_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
		return AV_PIX_FMT_YUV444P;
	}

	// The best for us would be Cb and Cr together if possible,
	// but FFmpeg doesn't support that except in the special case of
	// NV12, so we need to go to planar even for the case of NV12.
	// Thus, look for the closest (but no worse) 8-bit planar Y'CbCr format
	// that matches in color range. (This will also include the case of
	// the source format already being acceptable.)
	bool src_full_range = is_full_range(src_desc);
	const char *best_format = "yuv444p";
	unsigned best_score = numeric_limits<unsigned>::max();
	for (const AVPixFmtDescriptor *desc = av_pix_fmt_desc_next(nullptr);
	     desc;
	     desc = av_pix_fmt_desc_next(desc)) {
		// Find planar Y'CbCr formats only.
		if (desc->nb_components != 3) continue;
		if (desc->flags & AV_PIX_FMT_FLAG_RGB) continue;
		if (!(desc->flags & AV_PIX_FMT_FLAG_PLANAR)) continue;
		if (desc->comp[0].plane != 0 ||
		    desc->comp[1].plane != 1 ||
		    desc->comp[2].plane != 2) continue;

		// 8-bit formats only.
		if (desc->flags & AV_PIX_FMT_FLAG_BE) continue;
		if (desc->comp[0].depth != 8) continue;

		// Same or better chroma resolution only.
		int chroma_w_diff = desc->log2_chroma_w - src_desc->log2_chroma_w;
		int chroma_h_diff = desc->log2_chroma_h - src_desc->log2_chroma_h;
		if (chroma_w_diff < 0 || chroma_h_diff < 0)
			continue;

		// Matching full/limited range only.
		if (is_full_range(desc) != src_full_range)
			continue;

		// Pick something with as little excess chroma resolution as possible.
		unsigned score = (1 << (chroma_w_diff)) << chroma_h_diff;
		if (score < best_score) {
			best_score = score;
			best_format = desc->name;
		}
	}
	return av_get_pix_fmt(best_format);
}

YCbCrFormat decode_ycbcr_format(const AVPixFmtDescriptor *desc, const AVFrame *frame, bool is_mjpeg, AVColorSpace *last_colorspace, AVChromaLocation *last_chroma_location)
{
	YCbCrFormat format;
	AVColorSpace colorspace = frame->colorspace;
	switch (colorspace) {
	case AVCOL_SPC_BT709:
		format.luma_coefficients = YCBCR_REC_709;
		break;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		format.luma_coefficients = YCBCR_REC_601;
		break;
	case AVCOL_SPC_BT2020_NCL:
		format.luma_coefficients = YCBCR_REC_2020;
		break;
	case AVCOL_SPC_UNSPECIFIED:
		format.luma_coefficients = (frame->height >= 720 ? YCBCR_REC_709 : YCBCR_REC_601);
		break;
	default:
		if (colorspace != *last_colorspace) {
			fprintf(stderr, "Unknown Y'CbCr coefficient enum %d from FFmpeg; choosing Rec. 709.\n",
				colorspace);
		}
		format.luma_coefficients = YCBCR_REC_709;
		break;
	}
	*last_colorspace = colorspace;

	format.full_range = is_full_range(desc);
	format.num_levels = 1 << desc->comp[0].depth;
	format.chroma_subsampling_x = 1 << desc->log2_chroma_w;
	format.chroma_subsampling_y = 1 << desc->log2_chroma_h;

	switch (frame->chroma_location) {
	case AVCHROMA_LOC_LEFT:
		format.cb_x_position = 0.0;
		format.cb_y_position = 0.5;
		break;
	case AVCHROMA_LOC_CENTER:
		format.cb_x_position = 0.5;
		format.cb_y_position = 0.5;
		break;
	case AVCHROMA_LOC_TOPLEFT:
		format.cb_x_position = 0.0;
		format.cb_y_position = 0.0;
		break;
	case AVCHROMA_LOC_TOP:
		format.cb_x_position = 0.5;
		format.cb_y_position = 0.0;
		break;
	case AVCHROMA_LOC_BOTTOMLEFT:
		format.cb_x_position = 0.0;
		format.cb_y_position = 1.0;
		break;
	case AVCHROMA_LOC_BOTTOM:
		format.cb_x_position = 0.5;
		format.cb_y_position = 1.0;
		break;
	default:
		if (frame->chroma_location != *last_chroma_location) {
			fprintf(stderr, "Unknown chroma location coefficient enum %d from FFmpeg; choosing center.\n",
				frame->chroma_location);
		}
		format.cb_x_position = 0.5;
		format.cb_y_position = 0.5;
		break;
	}
	*last_chroma_location = frame->chroma_location;

	if (is_mjpeg && !format.full_range) {
		// Limited-range MJPEG is only detected by FFmpeg whenever a special
		// JPEG comment is set, which means that in practice, the stream is
		// almost certainly generated by Futatabi. Override FFmpeg's forced
		// MJPEG defaults (it disregards the values set in the mux) with what
		// Futatabi sets.
		format.luma_coefficients = YCBCR_REC_709;
		format.cb_x_position = 0.0;
		format.cb_y_position = 0.5;
	}

	format.cr_x_position = format.cb_x_position;
	format.cr_y_position = format.cb_y_position;
	return format;
}

RGBTriplet get_neutral_color(AVDictionary *metadata)
{
	if (metadata == nullptr) {
		return RGBTriplet(1.0f, 1.0f, 1.0f);
	}
	AVDictionaryEntry *entry = av_dict_get(metadata, "WhitePoint", nullptr, 0);
	if (entry == nullptr) {
		return RGBTriplet(1.0f, 1.0f, 1.0f);
	}

	unsigned x_nom, x_den, y_nom, y_den;
	if (sscanf(entry->value, " %u:%u , %u:%u", &x_nom, &x_den, &y_nom, &y_den) != 4) {
		fprintf(stderr, "WARNING: Unable to parse white point '%s', using default white point\n", entry->value);
		return RGBTriplet(1.0f, 1.0f, 1.0f);
	}

	double x = double(x_nom) / x_den;
	double y = double(y_nom) / y_den;
	double z = 1.0 - x - y;

	Matrix3d rgb_to_xyz_matrix = movit::ColorspaceConversionEffect::get_xyz_matrix(COLORSPACE_sRGB);
	Vector3d rgb = rgb_to_xyz_matrix.inverse() * Vector3d(x, y, z);

	return RGBTriplet(rgb[0], rgb[1], rgb[2]);
}

}  // namespace

FFmpegCapture::FFmpegCapture(const string &filename, unsigned width, unsigned height)
	: filename(filename), width(width), height(height), video_timebase{1, 1}
{
	description = "Video: " + filename;

	last_frame = steady_clock::now();

	avformat_network_init();  // In case someone wants this.
}

#ifdef HAVE_SRT
FFmpegCapture::FFmpegCapture(int srt_sock, const string &stream_id)
	: srt_sock(srt_sock),
	  width(0),  // Don't resize; SRT streams typically have stable resolution, and should behave much like regular cards in general.
	  height(0),
	  pixel_format(bmusb::PixelFormat_8BitYCbCrPlanar),
	  video_timebase{1, 1}
{
	if (stream_id.empty()) {
		description = "SRT stream";
	} else {
		description = stream_id;
	}
	play_as_fast_as_possible = true;
	play_once = true;
	last_frame = steady_clock::now();
}
#endif

FFmpegCapture::~FFmpegCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
	swr_free(&resampler);
#ifdef HAVE_SRT
	if (srt_sock != -1) {
		srt_close(srt_sock);
	}
#endif
}

void FFmpegCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
	if (audio_frame_allocator == nullptr) {
		// Audio can come out in pretty large chunks, so increase from the default 1 MB.
		owned_audio_frame_allocator.reset(new MallocFrameAllocator(1 << 20, NUM_QUEUED_AUDIO_FRAMES));
		set_audio_frame_allocator(owned_audio_frame_allocator.get());
	}
}

void FFmpegCapture::start_bm_capture()
{
	if (running) {
		return;
	}
	running = true;
	producer_thread_should_quit.unquit();
	producer_thread = thread(&FFmpegCapture::producer_thread_func, this);
}

void FFmpegCapture::stop_dequeue_thread()
{
	if (!running) {
		return;
	}
	running = false;
	producer_thread_should_quit.quit();
	producer_thread.join();
}

std::map<uint32_t, VideoMode> FFmpegCapture::get_available_video_modes() const
{
	// Note: This will never really be shown in the UI.
	VideoMode mode;

	char buf[256];
	snprintf(buf, sizeof(buf), "%ux%u", sws_last_width, sws_last_height);
	mode.name = buf;
	
	mode.autodetect = false;
	mode.width = sws_last_width;
	mode.height = sws_last_height;
	mode.frame_rate_num = 60;
	mode.frame_rate_den = 1;
	mode.interlaced = false;

	return {{ 0, mode }};
}

void FFmpegCapture::producer_thread_func()
{
	char thread_name[16];
	snprintf(thread_name, sizeof(thread_name), "FFmpeg_C_%d", card_index);
	pthread_setname_np(pthread_self(), thread_name);

	while (!producer_thread_should_quit.should_quit()) {
		string filename_copy;
		{
			lock_guard<mutex> lock(filename_mu);
			filename_copy = filename;
		}

		string pathname;
		if (srt_sock == -1) {
			pathname = search_for_file(filename_copy);
		} else {
			pathname = description;
		}
		if (pathname.empty()) {
			send_disconnected_frame();
			if (play_once) {
				break;
			}
			producer_thread_should_quit.sleep_for(seconds(1));
			fprintf(stderr, "%s not found, sleeping one second and trying again...\n", filename_copy.c_str());
			continue;
		}
		should_interrupt = false;
		if (!play_video(pathname)) {
			// Error.
			send_disconnected_frame();
			if (play_once) {
				break;
			}
			fprintf(stderr, "Error when playing %s, sleeping one second and trying again...\n", pathname.c_str());
			producer_thread_should_quit.sleep_for(seconds(1));
			continue;
		}

		if (play_once) {
			send_disconnected_frame();
			break;
		}

		// Probably just EOF, will exit the loop above on next test.
	}

	if (has_dequeue_callbacks) {
                dequeue_cleanup_callback();
		has_dequeue_callbacks = false;
        }
}

void FFmpegCapture::send_disconnected_frame()
{
	// Send an empty frame to signal that we have no signal anymore.
	FrameAllocator::Frame video_frame = video_frame_allocator->alloc_frame();
	size_t frame_width = width == 0 ? global_flags.width : width;
	size_t frame_height = height == 0 ? global_flags.height : height;
	if (video_frame.data) {
		VideoFormat video_format;
		video_format.width = frame_width;
		video_format.height = frame_height;
		video_format.frame_rate_nom = 60;
		video_format.frame_rate_den = 1;
		video_format.is_connected = false;
		if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
			video_format.stride = frame_width * 4;
			video_frame.len = frame_width * frame_height * 4;
			memset(video_frame.data, 0, video_frame.len);
		} else {
			video_format.stride = frame_width;
			current_frame_ycbcr_format.luma_coefficients = YCBCR_REC_709;
			current_frame_ycbcr_format.full_range = true;
			current_frame_ycbcr_format.num_levels = 256;
			current_frame_ycbcr_format.chroma_subsampling_x = 2;
			current_frame_ycbcr_format.chroma_subsampling_y = 2;
			current_frame_ycbcr_format.cb_x_position = 0.0f;
			current_frame_ycbcr_format.cb_y_position = 0.0f;
			current_frame_ycbcr_format.cr_x_position = 0.0f;
			current_frame_ycbcr_format.cr_y_position = 0.0f;
			video_frame.len = frame_width * frame_height * 2;
			memset(video_frame.data, 0, frame_width * frame_height);
			memset(video_frame.data + frame_width * frame_height, 128, frame_width * frame_height);  // Valid for both NV12 and planar.
		}

		if (frame_callback != nullptr) {
			frame_callback(-1, AVRational{1, TIMEBASE}, -1, AVRational{1, TIMEBASE}, timecode++,
				video_frame, /*video_offset=*/0, video_format,
				FrameAllocator::Frame(), /*audio_offset=*/0, AudioFormat());
		}
		last_frame_was_connected = false;
	}

	if (play_once) {
		disconnected = true;
		if (card_disconnected_callback != nullptr) {
			card_disconnected_callback();
		}
	}
}

AVPixelFormat get_vaapi_hw_format(AVCodecContext *ctx, const AVPixelFormat *fmt)
{
	for (const AVPixelFormat *fmt_ptr = fmt; *fmt_ptr != -1; ++fmt_ptr) {
		for (int i = 0;; ++i) {  // Termination condition inside loop.
			const AVCodecHWConfig *config = avcodec_get_hw_config(ctx->codec, i);
			if (config == nullptr) {  // End of list.
				fprintf(stderr, "Decoder %s does not support device.\n", ctx->codec->name);
				break;
			}
			if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
			    config->device_type == AV_HWDEVICE_TYPE_VAAPI &&
			    config->pix_fmt == *fmt_ptr) {
				return config->pix_fmt;
			}
		}
	}

	// We found no VA-API formats, so take the best software format.
	return fmt[0];
}

bool FFmpegCapture::play_video(const string &pathname)
{
	// Note: Call before open, not after; otherwise, there's a race.
	// (There is now, too, but it tips the correct way. We could use fstat()
	// if we had the file descriptor.)
	timespec last_modified;
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		// Probably some sort of protocol, so can't stat.
		last_modified.tv_sec = -1;
	} else {
		last_modified = buf.st_mtim;
	}
	last_colorspace = static_cast<AVColorSpace>(-1);
	last_chroma_location = static_cast<AVChromaLocation>(-1);

	AVFormatContextWithCloser format_ctx;
	if (srt_sock == -1) {
		// Regular file.
		format_ctx = avformat_open_input_unique(pathname.c_str(), /*fmt=*/nullptr,
			/*options=*/nullptr,
			AVIOInterruptCB{ &FFmpegCapture::interrupt_cb_thunk, this });
	} else {
#ifdef HAVE_SRT
		// SRT socket, already opened.
		const AVInputFormat *mpegts_fmt = av_find_input_format("mpegts");
		format_ctx = avformat_open_input_unique(&FFmpegCapture::read_srt_thunk, this,
			mpegts_fmt, /*options=*/nullptr,
			AVIOInterruptCB{ &FFmpegCapture::interrupt_cb_thunk, this });
#else
		assert(false);
#endif
	}
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", pathname.c_str());
		return false;
	}

	if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
		fprintf(stderr, "%s: Error finding stream info\n", pathname.c_str());
		return false;
	}

	int video_stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_VIDEO);
	if (video_stream_index == -1) {
		fprintf(stderr, "%s: No video stream found\n", pathname.c_str());
		return false;
	}

	int audio_stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_AUDIO);
	int subtitle_stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_SUBTITLE);
	has_last_subtitle = false;

	// Open video decoder.
	const AVCodecParameters *video_codecpar = format_ctx->streams[video_stream_index]->codecpar;
	const AVCodec *video_codec = avcodec_find_decoder(video_codecpar->codec_id);

	video_timebase = format_ctx->streams[video_stream_index]->time_base;
	AVCodecContextWithDeleter video_codec_ctx = avcodec_alloc_context3_unique(nullptr);
	if (avcodec_parameters_to_context(video_codec_ctx.get(), video_codecpar) < 0) {
		fprintf(stderr, "%s: Cannot fill video codec parameters\n", pathname.c_str());
		return false;
	}
	if (video_codec == nullptr) {
		fprintf(stderr, "%s: Cannot find video decoder\n", pathname.c_str());
		return false;
	}

	// Seemingly, it's not too easy to make something that just initializes
	// “whatever goes”, so we don't get VDPAU or CUDA here without enumerating
	// through several different types. VA-API will do for now.
	AVBufferRef *hw_device_ctx = nullptr;
	if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
		fprintf(stderr, "Failed to initialize VA-API for FFmpeg acceleration. Decoding video in software.\n");
	} else {
		video_codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
		video_codec_ctx->get_format = get_vaapi_hw_format;
	}

	if (avcodec_open2(video_codec_ctx.get(), video_codec, nullptr) < 0) {
		fprintf(stderr, "%s: Cannot open video decoder\n", pathname.c_str());
		return false;
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> video_codec_ctx_cleanup(
		video_codec_ctx.get(), avcodec_close);

	// Used in decode_ycbcr_format().
	is_mjpeg = video_codecpar->codec_id == AV_CODEC_ID_MJPEG;

	// Open audio decoder, if we have audio.
	AVCodecContextWithDeleter audio_codec_ctx;
	if (audio_stream_index != -1) {
		audio_codec_ctx = avcodec_alloc_context3_unique(nullptr);
		const AVCodecParameters *audio_codecpar = format_ctx->streams[audio_stream_index]->codecpar;
		audio_timebase = format_ctx->streams[audio_stream_index]->time_base;
		if (avcodec_parameters_to_context(audio_codec_ctx.get(), audio_codecpar) < 0) {
			fprintf(stderr, "%s: Cannot fill audio codec parameters\n", pathname.c_str());
			return false;
		}
		const AVCodec *audio_codec = avcodec_find_decoder(audio_codecpar->codec_id);
		if (audio_codec == nullptr) {
			fprintf(stderr, "%s: Cannot find audio decoder\n", pathname.c_str());
			return false;
		}
		if (avcodec_open2(audio_codec_ctx.get(), audio_codec, nullptr) < 0) {
			fprintf(stderr, "%s: Cannot open audio decoder\n", pathname.c_str());
			return false;
		}
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> audio_codec_ctx_cleanup(
		audio_codec_ctx.get(), avcodec_close);

	internal_rewind();

	// Main loop.
	bool first_frame = true;
	while (!producer_thread_should_quit.should_quit()) {
		if (process_queued_commands(format_ctx.get(), pathname, last_modified, /*rewound=*/nullptr)) {
			return true;
		}
		if (should_interrupt.load()) {
			// Check as a failsafe, so that we don't need to rely on avio if we don't have to.
			return false;
		}
		UniqueFrame audio_frame = audio_frame_allocator->alloc_frame();
		AudioFormat audio_format;

		int64_t audio_pts;
		bool error;
		AVFrameWithDeleter frame = decode_frame(format_ctx.get(), video_codec_ctx.get(), audio_codec_ctx.get(),
			pathname, video_stream_index, audio_stream_index, subtitle_stream_index, audio_frame.get(), &audio_format, &audio_pts, &error);
		if (error) {
			return false;
		}
		if (frame == nullptr) {
			// EOF. Loop back to the start if we can.
			if (format_ctx->pb != nullptr && format_ctx->pb->seekable == 0) {
				// Not seekable (but seemingly, sometimes av_seek_frame() would return 0 anyway,
				// so don't try).
				return true;
			}
			if (av_seek_frame(format_ctx.get(), /*stream_index=*/-1, /*timestamp=*/0, /*flags=*/0) < 0) {
				fprintf(stderr, "%s: Rewind failed, not looping.\n", pathname.c_str());
				return true;
			}
			if (video_codec_ctx != nullptr) {
				avcodec_flush_buffers(video_codec_ctx.get());
			}
			if (audio_codec_ctx != nullptr) {
				avcodec_flush_buffers(audio_codec_ctx.get());
			}
			// If the file has changed since last time, return to get it reloaded.
			// Note that depending on how you move the file into place, you might
			// end up corrupting the one you're already playing, so this path
			// might not trigger.
			if (changed_since(pathname, last_modified)) {
				return true;
			}
			internal_rewind();
			continue;
		}

		VideoFormat video_format = construct_video_format(frame.get(), video_timebase);
		if (video_format.frame_rate_nom == 0 || video_format.frame_rate_den == 0) {
			// Invalid frame rate; try constructing it from the previous frame length.
			// (This is especially important if we are the master card, for SRT,
			// since it affects audio. Not all senders have good timebases
			// (e.g., Larix rounds first to timebase 1000 and then multiplies by
			// 90 from there, it seems), but it's much better to have an oscillating
			// value than just locking at 60.
			if (last_pts != 0 && frame->pts > last_pts) {
				int64_t pts_diff = frame->pts - last_pts;
				video_format.frame_rate_nom = video_timebase.den;
				video_format.frame_rate_den = video_timebase.num * pts_diff;
			} else {
				video_format.frame_rate_nom = 60;
				video_format.frame_rate_den = 1;
			}
		}
		UniqueFrame video_frame = make_video_frame(frame.get(), pathname, &error);
		if (error) {
			return false;
		}

		for ( ;; ) {
			if (last_pts == 0 && pts_origin == 0) {
				pts_origin = frame->pts;	
			}
			steady_clock::time_point now = steady_clock::now();
			if (play_as_fast_as_possible) {
				video_frame->received_timestamp = now;
				audio_frame->received_timestamp = now;
				next_frame_start = now;
			} else {
				next_frame_start = compute_frame_start(frame->pts, pts_origin, video_timebase, start, rate);
				if (first_frame && last_frame_was_connected) {
					// If reconnect took more than one second, this is probably a live feed,
					// and we should reset the resampler. (Or the rate is really, really low,
					// in which case a reset on the first frame is fine anyway.)
					if (duration<double>(next_frame_start - last_frame).count() >= 1.0) {
						last_frame_was_connected = false;
					}
				}
				video_frame->received_timestamp = next_frame_start;

				// The easiest way to get all the rate conversions etc. right is to move the
				// audio PTS into the video PTS timebase and go from there. (We'll get some
				// rounding issues, but they should not be a big problem.)
				int64_t audio_pts_as_video_pts = av_rescale_q(audio_pts, audio_timebase, video_timebase);
				audio_frame->received_timestamp = compute_frame_start(audio_pts_as_video_pts, pts_origin, video_timebase, start, rate);

				if (audio_frame->len != 0) {
					// The received timestamps in Nageru are measured after we've just received the frame.
					// However, pts (especially audio pts) is at the _beginning_ of the frame.
					// If we have locked audio, the distinction doesn't really matter, as pts is
					// on a relative scale and a fixed offset is fine. But if we don't, we will have
					// a different number of samples each time, which will cause huge audio jitter
					// and throw off the resampler.
					//
					// In a sense, we should have compensated by adding the frame and audio lengths
					// to video_frame->received_timestamp and audio_frame->received_timestamp respectively,
					// but that would mean extra waiting in sleep_until(). All we need is that they
					// are correct relative to each other, though (and to the other frames we send),
					// so just align the end of the audio frame, and we're fine.
					size_t num_samples = (audio_frame->len * 8) / audio_format.bits_per_sample / audio_format.num_channels;
					double offset = double(num_samples) / OUTPUT_FREQUENCY -
						double(video_format.frame_rate_den) / video_format.frame_rate_nom;
					audio_frame->received_timestamp += duration_cast<steady_clock::duration>(duration<double>(offset));
				}

				if (duration<double>(now - next_frame_start).count() >= 0.1) {
					// If we don't have enough CPU to keep up, or if we have a live stream
					// where the initial origin was somehow wrong, we could be behind indefinitely.
					// In particular, this will give the audio resampler problems as it tries
					// to speed up to reduce the delay, hitting the low end of the buffer every time.
					fprintf(stderr, "%s: Playback %.0f ms behind, resetting time scale\n",
						pathname.c_str(),
						1e3 * duration<double>(now - next_frame_start).count());
					pts_origin = frame->pts;
					start = next_frame_start = now;
					timecode += MAX_FPS * 2 + 1;
				}
			}
			bool finished_wakeup;
			if (play_as_fast_as_possible) {
				finished_wakeup = !producer_thread_should_quit.should_quit();
			} else {
				finished_wakeup = producer_thread_should_quit.sleep_until(next_frame_start);
			}
			if (finished_wakeup) {
				if (audio_frame->len > 0) {
					assert(audio_pts != -1);
				}
				if (!last_frame_was_connected) {
					// We're recovering from an error (or really slow load, see above).
					// Make sure to get the audio resampler reset. (This is a hack;
					// ideally, the frame callback should just accept a way to signal
					// audio discontinuity.)
					timecode += MAX_FPS * 2 + 1;
				}
				last_neutral_color = get_neutral_color(frame->metadata);
				if (frame_callback != nullptr) {
					frame_callback(frame->pts, video_timebase, audio_pts, audio_timebase, timecode++,
						video_frame.get_and_release(), 0, video_format,
						audio_frame.get_and_release(), 0, audio_format);
				}
				first_frame = false;
				last_frame = steady_clock::now();
				last_frame_was_connected = true;
				break;
			} else {
				if (producer_thread_should_quit.should_quit()) break;

				bool rewound = false;
				if (process_queued_commands(format_ctx.get(), pathname, last_modified, &rewound)) {
					return true;
				}
				// If we just rewound, drop this frame on the floor and be done.
				if (rewound) {
					break;
				}
				// OK, we didn't, so probably a rate change. Recalculate next_frame_start,
				// but if it's now in the past, we'll reset the origin, so that we don't
				// generate a huge backlog of frames that we need to run through quickly.
				next_frame_start = compute_frame_start(frame->pts, pts_origin, video_timebase, start, rate);
				steady_clock::time_point now = steady_clock::now();
				if (next_frame_start < now) {
					pts_origin = frame->pts;
					start = next_frame_start = now;
				}
			}
		}
		last_pts = frame->pts;
	}
	return true;
}

void FFmpegCapture::internal_rewind()
{				
	pts_origin = last_pts = 0;
	start = next_frame_start = steady_clock::now();
}

bool FFmpegCapture::process_queued_commands(AVFormatContext *format_ctx, const std::string &pathname, timespec last_modified, bool *rewound)
{
	// Process any queued commands from other threads.
	vector<QueuedCommand> commands;
	{
		lock_guard<mutex> lock(queue_mu);
		swap(commands, command_queue);
	}
	for (const QueuedCommand &cmd : commands) {
		switch (cmd.command) {
		case QueuedCommand::REWIND:
			if (av_seek_frame(format_ctx, /*stream_index=*/-1, /*timestamp=*/0, /*flags=*/0) < 0) {
				fprintf(stderr, "%s: Rewind failed, stopping play.\n", pathname.c_str());
			}
			// If the file has changed since last time, return to get it reloaded.
			// Note that depending on how you move the file into place, you might
			// end up corrupting the one you're already playing, so this path
			// might not trigger.
			if (changed_since(pathname, last_modified)) {
				return true;
			}
			internal_rewind();
			if (rewound != nullptr) {
				*rewound = true;
			}
			break;

		case QueuedCommand::CHANGE_RATE:
			// Change the origin to the last played frame.
			start = compute_frame_start(last_pts, pts_origin, video_timebase, start, rate);
			pts_origin = last_pts;
			rate = cmd.new_rate;
			play_as_fast_as_possible = (rate >= 10.0);
			break;
		}
	}
	return false;
}

namespace {

}  // namespace

AVFrameWithDeleter FFmpegCapture::decode_frame(AVFormatContext *format_ctx, AVCodecContext *video_codec_ctx, AVCodecContext *audio_codec_ctx,
	const std::string &pathname, int video_stream_index, int audio_stream_index, int subtitle_stream_index,
	FrameAllocator::Frame *audio_frame, AudioFormat *audio_format, int64_t *audio_pts, bool *error)
{
	*error = false;

	// Read packets until we have a frame or there are none left.
	bool frame_finished = false;
	AVFrameWithDeleter audio_avframe = av_frame_alloc_unique();
	AVFrameWithDeleter video_avframe = av_frame_alloc_unique();
	bool eof = false;
	*audio_pts = -1;
	bool has_audio = false;
	do {
		AVPacket pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
			&pkt, av_packet_unref);
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		if (av_read_frame(format_ctx, &pkt) == 0) {
			if (pkt.stream_index == audio_stream_index && audio_callback != nullptr) {
				audio_callback(&pkt, format_ctx->streams[audio_stream_index]->time_base);
			}
			if (pkt.stream_index == video_stream_index && video_callback != nullptr) {
				video_callback(&pkt, format_ctx->streams[video_stream_index]->time_base);
			}
			if (pkt.stream_index == video_stream_index && global_flags.transcode_video) {
				if (avcodec_send_packet(video_codec_ctx, &pkt) < 0) {
					fprintf(stderr, "%s: Cannot send packet to video codec.\n", pathname.c_str());
					*error = true;
					return AVFrameWithDeleter(nullptr);
				}
			} else if (pkt.stream_index == audio_stream_index && global_flags.transcode_audio) {
				has_audio = true;
				if (avcodec_send_packet(audio_codec_ctx, &pkt) < 0) {
					fprintf(stderr, "%s: Cannot send packet to audio codec.\n", pathname.c_str());
					*error = true;
					return AVFrameWithDeleter(nullptr);
				}
			} else if (pkt.stream_index == subtitle_stream_index) {
				last_subtitle = string(reinterpret_cast<const char *>(pkt.data), pkt.size);
				has_last_subtitle = true;
			}
		} else {
			eof = true;  // Or error, but ignore that for the time being.
		}

		// Decode audio, if any.
		if (has_audio) {
			for ( ;; ) {
				int err = avcodec_receive_frame(audio_codec_ctx, audio_avframe.get());
				if (err == 0) {
					if (*audio_pts == -1) {
						*audio_pts = audio_avframe->pts;
					}
					convert_audio(audio_avframe.get(), audio_frame, audio_format);
				} else if (err == AVERROR(EAGAIN)) {
					break;
				} else {
					fprintf(stderr, "%s: Cannot receive frame from audio codec.\n", pathname.c_str());
					*error = true;
					return AVFrameWithDeleter(nullptr);
				}
			}
		}

		// Decode video, if we have a frame.
		int err = avcodec_receive_frame(video_codec_ctx, video_avframe.get());
		if (err == 0) {
			if (video_avframe->format == AV_PIX_FMT_VAAPI) {
				// Get the frame down to the CPU. (TODO: See if we can keep it
				// on the GPU all the way, since it will be going up again later.
				// However, this only works if the OpenGL GPU is the same one.)
				AVFrameWithDeleter sw_frame = av_frame_alloc_unique();
				int err = av_hwframe_transfer_data(sw_frame.get(), video_avframe.get(), 0);
				if (err != 0) {
					fprintf(stderr, "%s: Cannot transfer hardware video frame to software.\n", pathname.c_str());
					*error = true;
					return AVFrameWithDeleter(nullptr);
				}
				sw_frame->pts = video_avframe->pts;
				sw_frame->pkt_duration = video_avframe->pkt_duration;
				video_avframe = move(sw_frame);
			}
			frame_finished = true;
			break;
		} else if (err != AVERROR(EAGAIN)) {
			fprintf(stderr, "%s: Cannot receive frame from video codec.\n", pathname.c_str());
			*error = true;
			return AVFrameWithDeleter(nullptr);
		}
	} while (!eof);

	if (frame_finished)
		return video_avframe;
	else
		return AVFrameWithDeleter(nullptr);
}

void FFmpegCapture::convert_audio(const AVFrame *audio_avframe, FrameAllocator::Frame *audio_frame, AudioFormat *audio_format)
{
	// Decide on a format. If there already is one in this audio frame,
	// we're pretty much forced to use it. If not, we try to find an exact match.
	// If that still doesn't work, we default to 32-bit signed chunked
	// (float would be nice, but there's really no way to signal that yet).
	AVSampleFormat dst_format;
	if (audio_format->bits_per_sample == 0) {
		switch (audio_avframe->format) {
		case AV_SAMPLE_FMT_S16:
		case AV_SAMPLE_FMT_S16P:
			audio_format->bits_per_sample = 16;
			dst_format = AV_SAMPLE_FMT_S16;
			break;
		case AV_SAMPLE_FMT_S32:
		case AV_SAMPLE_FMT_S32P:
		default:
			audio_format->bits_per_sample = 32;
			dst_format = AV_SAMPLE_FMT_S32;
			break;
		}
	} else if (audio_format->bits_per_sample == 16) {
		dst_format = AV_SAMPLE_FMT_S16;
	} else if (audio_format->bits_per_sample == 32) {
		dst_format = AV_SAMPLE_FMT_S32;
	} else {
		assert(false);
	}
	audio_format->num_channels = 2;

	int64_t channel_layout = audio_avframe->channel_layout;
	if (channel_layout == 0) {
		channel_layout = av_get_default_channel_layout(audio_avframe->channels);
	}

	if (resampler == nullptr ||
	    audio_avframe->format != last_src_format ||
	    dst_format != last_dst_format ||
	    channel_layout != last_channel_layout ||
	    audio_avframe->sample_rate != last_sample_rate) {
		swr_free(&resampler);
		resampler = swr_alloc_set_opts(nullptr,
		                               /*out_ch_layout=*/AV_CH_LAYOUT_STEREO_DOWNMIX,
		                               /*out_sample_fmt=*/dst_format,
		                               /*out_sample_rate=*/OUTPUT_FREQUENCY,
		                               /*in_ch_layout=*/channel_layout,
		                               /*in_sample_fmt=*/AVSampleFormat(audio_avframe->format),
		                               /*in_sample_rate=*/audio_avframe->sample_rate,
		                               /*log_offset=*/0,
		                               /*log_ctx=*/nullptr);

		if (resampler == nullptr) {
			fprintf(stderr, "Allocating resampler failed.\n");
			abort();
		}

		if (swr_init(resampler) < 0) {
			fprintf(stderr, "Could not open resample context.\n");
			abort();
		}

		last_src_format = AVSampleFormat(audio_avframe->format);
		last_dst_format = dst_format;
		last_channel_layout = channel_layout;
		last_sample_rate = audio_avframe->sample_rate;
	}

	size_t bytes_per_sample = (audio_format->bits_per_sample / 8) * 2;
	size_t num_samples_room = (audio_frame->size - audio_frame->len) / bytes_per_sample;

	uint8_t *data = audio_frame->data + audio_frame->len;
	int out_samples = swr_convert(resampler, &data, num_samples_room,
		const_cast<const uint8_t **>(audio_avframe->data), audio_avframe->nb_samples);
	if (out_samples < 0) {
                fprintf(stderr, "Audio conversion failed.\n");
                abort();
        }

	audio_frame->len += out_samples * bytes_per_sample;
}

VideoFormat FFmpegCapture::construct_video_format(const AVFrame *frame, AVRational video_timebase)
{
	VideoFormat video_format;
	video_format.width = frame_width(frame);
	video_format.height = frame_height(frame);
	if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
		video_format.stride = frame_width(frame) * 4;
	} else if (pixel_format == FFmpegCapture::PixelFormat_NV12) {
		video_format.stride = frame_width(frame);
	} else {
		assert(pixel_format == bmusb::PixelFormat_8BitYCbCrPlanar);
		video_format.stride = frame_width(frame);
	}
	video_format.frame_rate_nom = video_timebase.den;
	video_format.frame_rate_den = frame->pkt_duration * video_timebase.num;
	video_format.has_signal = true;
	video_format.is_connected = true;
	return video_format;
}

UniqueFrame FFmpegCapture::make_video_frame(const AVFrame *frame, const string &pathname, bool *error)
{
	*error = false;

	UniqueFrame video_frame(video_frame_allocator->alloc_frame());
	if (video_frame->data == nullptr) {
		return video_frame;
	}

	if (sws_ctx == nullptr ||
	    sws_last_width != frame->width ||
	    sws_last_height != frame->height ||
	    sws_last_src_format != frame->format) {
		sws_dst_format = decide_dst_format(AVPixelFormat(frame->format), pixel_format);
		sws_ctx.reset(
			sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
				frame_width(frame), frame_height(frame), sws_dst_format,
				SWS_BICUBIC, nullptr, nullptr, nullptr));
		sws_last_width = frame->width;
		sws_last_height = frame->height;
		sws_last_src_format = frame->format;
	}
	if (sws_ctx == nullptr) {
		fprintf(stderr, "%s: Could not create scaler context\n", pathname.c_str());
		*error = true;
		return video_frame;
	}

	uint8_t *pic_data[4] = { nullptr, nullptr, nullptr, nullptr };
	int linesizes[4] = { 0, 0, 0, 0 };
	if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
		pic_data[0] = video_frame->data;
		linesizes[0] = frame_width(frame) * 4;
		video_frame->len = (frame_width(frame) * 4) * frame_height(frame);
	} else if (pixel_format == PixelFormat_NV12) {
		pic_data[0] = video_frame->data;
		linesizes[0] = frame_width(frame);

		pic_data[1] = pic_data[0] + frame_width(frame) * frame_height(frame);
		linesizes[1] = frame_width(frame);

		video_frame->len = (frame_width(frame) * 2) * frame_height(frame);

		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(sws_dst_format);
		current_frame_ycbcr_format = decode_ycbcr_format(desc, frame, is_mjpeg, &last_colorspace, &last_chroma_location);
	} else {
		assert(pixel_format == bmusb::PixelFormat_8BitYCbCrPlanar);
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(sws_dst_format);

		int chroma_width = AV_CEIL_RSHIFT(int(frame_width(frame)), desc->log2_chroma_w);
		int chroma_height = AV_CEIL_RSHIFT(int(frame_height(frame)), desc->log2_chroma_h);

		pic_data[0] = video_frame->data;
		linesizes[0] = frame_width(frame);

		pic_data[1] = pic_data[0] + frame_width(frame) * frame_height(frame);
		linesizes[1] = chroma_width;

		pic_data[2] = pic_data[1] + chroma_width * chroma_height;
		linesizes[2] = chroma_width;

		video_frame->len = frame_width(frame) * frame_height(frame) + 2 * chroma_width * chroma_height;

		current_frame_ycbcr_format = decode_ycbcr_format(desc, frame, is_mjpeg, &last_colorspace, &last_chroma_location);
	}
	sws_scale(sws_ctx.get(), frame->data, frame->linesize, 0, frame->height, pic_data, linesizes);

	return video_frame;
}

int FFmpegCapture::interrupt_cb_thunk(void *opaque)
{
	return reinterpret_cast<FFmpegCapture *>(opaque)->interrupt_cb();
}

int FFmpegCapture::interrupt_cb()
{
	return should_interrupt.load();
}

unsigned FFmpegCapture::frame_width(const AVFrame *frame) const
{
	if (width == 0) {
		return frame->width;
	} else {
		return width;
	}
}

unsigned FFmpegCapture::frame_height(const AVFrame *frame) const
{
	if (height == 0) {
		return frame->height;
	} else {
		return height;
	}
}

#ifdef HAVE_SRT
int FFmpegCapture::read_srt_thunk(void *opaque, uint8_t *buf, int buf_size)
{
	return reinterpret_cast<FFmpegCapture *>(opaque)->read_srt(buf, buf_size);
}

int FFmpegCapture::read_srt(uint8_t *buf, int buf_size)
{
	SRT_MSGCTRL mc = srt_msgctrl_default;
	return srt_recvmsg2(srt_sock, reinterpret_cast<char *>(buf), buf_size, &mc);
}
#endif
