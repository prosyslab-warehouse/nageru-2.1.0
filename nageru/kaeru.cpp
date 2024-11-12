// Kaeru (換える), a simple transcoder intended for use with Nageru.

#include "audio_encoder.h"
#include "basic_stats.h"
#include "defs.h"
#include "flags.h"
#include "ffmpeg_capture.h"
#include "mixer.h"
#include "shared/mux.h"
#include "quittable_sleeper.h"
#include "shared/timebase.h"
#include "x264_encoder.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <string>

extern "C" {
#include <libavcodec/bsf.h>
}

using namespace bmusb;
using namespace movit;
using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

Mixer *global_mixer = nullptr;
X264Encoder *global_x264_encoder = nullptr;
int frame_num = 0;
BasicStats *global_basic_stats = nullptr;
QuittableSleeper should_quit;
MuxMetrics stream_mux_metrics;

namespace {

int write_packet(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	static bool seen_sync_markers = false;
	static string stream_mux_header;
	HTTPD *httpd = (HTTPD *)opaque;

	if (type == AVIO_DATA_MARKER_SYNC_POINT || type == AVIO_DATA_MARKER_BOUNDARY_POINT) {
		seen_sync_markers = true;
	} else if (type == AVIO_DATA_MARKER_UNKNOWN && !seen_sync_markers) {
		// We don't know if this is a keyframe or not (the muxer could
		// avoid marking it), so we just have to make the best of it.
		type = AVIO_DATA_MARKER_SYNC_POINT;
	}

	HTTPD::StreamID stream_id{ HTTPD::MAIN_STREAM, 0 };
	if (type == AVIO_DATA_MARKER_HEADER) {
		stream_mux_header.append((char *)buf, buf_size);
		httpd->set_header(stream_id, stream_mux_header);
	} else {
		httpd->add_data(stream_id, (char *)buf, buf_size, type == AVIO_DATA_MARKER_SYNC_POINT, time, AVRational{ AV_TIME_BASE, 1 });
	}
	return buf_size;
}

}  // namespace

unique_ptr<Mux> create_mux(HTTPD *httpd, const AVOutputFormat *oformat, X264Encoder *x264_encoder, AudioEncoder *audio_encoder)
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = const_cast<decltype(avctx->oformat)>(oformat);  // const_cast is a hack to work in FFmpeg both before and after 5.0.

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, httpd, nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &write_packet;
	avctx->pb->ignore_boundary_point = 1;
	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	string video_extradata = x264_encoder->get_global_headers();

	// If audio is disabled (ie., we won't ever see any audio packets),
	// set nullptr here to also not include the stream in the mux.
	AVCodecParameters *audio_codecpar =
		global_flags.enable_audio ? audio_encoder->get_codec_parameters().release() : nullptr;

	unique_ptr<Mux> mux;
	mux.reset(new Mux(avctx, global_flags.width, global_flags.height, Mux::CODEC_H264, video_extradata, audio_codecpar,
		get_color_space(global_flags.ycbcr_rec709_coefficients), COARSE_TIMEBASE,
	        /*write_callback=*/nullptr, Mux::WRITE_FOREGROUND, { &stream_mux_metrics }));
	stream_mux_metrics.init({{ "destination", "http" }});
	return mux;
}

void video_frame_callback(FFmpegCapture *video, X264Encoder *x264_encoder, AudioEncoder *audio_encoder,
                          int64_t video_pts, AVRational video_timebase,
                          int64_t audio_pts, AVRational audio_timebase,
                          uint16_t timecode,
	                  FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
	                  FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)
{
	if (video_pts >= 0 && video_frame.len > 0) {
		ReceivedTimestamps ts;
		ts.ts.push_back(steady_clock::now());

		video_pts = av_rescale_q(video_pts, video_timebase, AVRational{ 1, TIMEBASE });
		int64_t frame_duration = int64_t(TIMEBASE) * video_format.frame_rate_den / video_format.frame_rate_nom;
		x264_encoder->add_frame(video_pts, frame_duration, video->get_current_frame_ycbcr_format().luma_coefficients, video_frame.data + video_offset, ts);
		global_basic_stats->update(frame_num++, /*dropped_frames=*/0);
	}
	if (audio_frame.len > 0) {
		// FFmpegCapture takes care of this for us.
		assert(audio_format.num_channels == 2);
		assert(audio_format.sample_rate == OUTPUT_FREQUENCY);

		// TODO: Reduce some duplication against AudioMixer here.
		size_t num_samples = audio_frame.len / (audio_format.bits_per_sample / 8);
		vector<float> float_samples;
		float_samples.resize(num_samples);

		if (audio_format.bits_per_sample == 16) {
			const int16_t *src = (const int16_t *)audio_frame.data;
			float *dst = &float_samples[0];
			for (size_t i = 0; i < num_samples; ++i) {
				*dst++ = int16_t(le16toh(*src++)) * (1.0f / 32768.0f);
			}
		} else if (audio_format.bits_per_sample == 32) {
			const int32_t *src = (const int32_t *)audio_frame.data;
			float *dst = &float_samples[0];
			for (size_t i = 0; i < num_samples; ++i) {
				*dst++ = int32_t(le32toh(*src++)) * (1.0f / 2147483648.0f);
			}
		} else {
			assert(false);
		}
		audio_pts = av_rescale_q(audio_pts, audio_timebase, AVRational{ 1, TIMEBASE });
		audio_encoder->encode_audio(float_samples, audio_pts);
        }

	if (video_frame.owner) {
		video_frame.owner->release_frame(video_frame);
	}
	if (audio_frame.owner) {
		audio_frame.owner->release_frame(audio_frame);
	}
}

void raw_packet_callback(Mux *mux, int stream_index, const AVPacket *pkt, AVRational timebase)
{
	mux->add_packet(*pkt, pkt->pts, pkt->dts == AV_NOPTS_VALUE ? pkt->pts : pkt->dts, timebase, stream_index);
}

void filter_packet_callback(Mux *mux, int stream_index, AVBSFContext *bsfctx, const AVPacket *pkt, AVRational timebase)
{
	if (pkt->size <= 2 || pkt->data[0] != 0xff || (pkt->data[1] & 0xf0) != 0xf0) {
		// Not ADTS data, so just pass it through.
		mux->add_packet(*pkt, pkt->pts, pkt->dts == AV_NOPTS_VALUE ? pkt->pts : pkt->dts, timebase, stream_index);
		return;
	}

	AVPacket *in_pkt = av_packet_clone(pkt);
	unique_ptr<AVPacket, decltype(av_packet_unref) *> in_pkt_cleanup(in_pkt, av_packet_unref);
	int err = av_bsf_send_packet(bsfctx, in_pkt);
	if (err < 0) {
		fprintf(stderr, "av_bsf_send_packet() failed with %d, ignoring\n", err);
	}
	for ( ;; ) {
		AVPacket out_pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref) *> pkt_cleanup(&out_pkt, av_packet_unref);
		av_init_packet(&out_pkt);
		err = av_bsf_receive_packet(bsfctx, &out_pkt);
		if (err == AVERROR(EAGAIN)) {
			break;
		}
		if (err < 0) {
			fprintf(stderr, "av_bsf_receive_packet() failed with %d, ignoring\n", err);
			return;
		}
		mux->add_packet(out_pkt, out_pkt.pts, out_pkt.dts == AV_NOPTS_VALUE ? out_pkt.pts : out_pkt.dts, timebase, stream_index);
	}
}

void adjust_bitrate(int signal)
{
	int new_bitrate = global_flags.x264_bitrate;
	if (signal == SIGUSR1) {
		new_bitrate += 100;
		if (new_bitrate > 100000) {
			fprintf(stderr, "Ignoring SIGUSR1, can't increase bitrate below 100000 kbit/sec (currently at %d kbit/sec)\n",
				global_flags.x264_bitrate);
		} else {
			fprintf(stderr, "Increasing bitrate to %d kbit/sec due to SIGUSR1.\n", new_bitrate);
			global_flags.x264_bitrate = new_bitrate;
			global_x264_encoder->change_bitrate(new_bitrate);
		}
	} else if (signal == SIGUSR2) {
		new_bitrate -= 100;
		if (new_bitrate < 100) {
			fprintf(stderr, "Ignoring SIGUSR2, can't decrease bitrate below 100 kbit/sec (currently at %d kbit/sec)\n",
				global_flags.x264_bitrate);
		} else {
			fprintf(stderr, "Decreasing bitrate to %d kbit/sec due to SIGUSR2.\n", new_bitrate);
			global_flags.x264_bitrate = new_bitrate;
			global_x264_encoder->change_bitrate(new_bitrate);
		}
	}
}

void request_quit(int signal)
{
	should_quit.quit();
}

int main(int argc, char *argv[])
{
	parse_flags(PROGRAM_KAERU, argc, argv);
	if (optind + 1 != argc) {
		usage(PROGRAM_KAERU);
		abort();
	}
	global_flags.max_num_cards = 1;  // For latency metrics.

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	av_register_all();
#endif
	avformat_network_init();

	HTTPD httpd;

	const AVOutputFormat *oformat = av_guess_format(global_flags.stream_mux_name.c_str(), nullptr, nullptr);
	assert(oformat != nullptr);

	unique_ptr<AudioEncoder> audio_encoder;
	if (global_flags.stream_audio_codec_name.empty()) {
		audio_encoder.reset(new AudioEncoder(AUDIO_OUTPUT_CODEC_NAME, DEFAULT_AUDIO_OUTPUT_BIT_RATE, oformat));
	} else {
		audio_encoder.reset(new AudioEncoder(global_flags.stream_audio_codec_name, global_flags.stream_audio_codec_bitrate, oformat));
	}

	unique_ptr<X264Encoder> x264_encoder(new X264Encoder(oformat, /*use_separate_disk_params=*/false));
	unique_ptr<Mux> http_mux = create_mux(&httpd, oformat, x264_encoder.get(), audio_encoder.get());
	if (global_flags.transcode_audio) {
		audio_encoder->add_mux(http_mux.get());
	}
	if (global_flags.transcode_video) {
		x264_encoder->add_mux(http_mux.get());
	}
	global_x264_encoder = x264_encoder.get();

	FFmpegCapture video(argv[optind], global_flags.width, global_flags.height);
	video.set_pixel_format(FFmpegCapture::PixelFormat_NV12);
	if (global_flags.transcode_video) {
		video.set_frame_callback(bind(video_frame_callback, &video, x264_encoder.get(), audio_encoder.get(), _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11));
	} else {
		video.set_video_callback(bind(raw_packet_callback, http_mux.get(), /*stream_index=*/0, _1, _2));
	}
	if (!global_flags.transcode_audio && global_flags.enable_audio) {
		AVBSFContext *bsfctx = nullptr;
		if (strcmp(oformat->name, "mp4") == 0 && strcmp(audio_encoder->get_codec()->name, "aac") == 0) {
			// We need to insert the aac_adtstoasc filter, seemingly (or we will get warnings to do so).
			const AVBitStreamFilter *filter = av_bsf_get_by_name("aac_adtstoasc");
			int err = av_bsf_alloc(filter, &bsfctx);
			if (err < 0) {
				fprintf(stderr, "av_bsf_alloc() failed with %d\n", err);
				exit(1);
			}
		}
		if (bsfctx == nullptr) {
			video.set_audio_callback(bind(raw_packet_callback, http_mux.get(), /*stream_index=*/1, _1, _2));
		} else {
			video.set_audio_callback(bind(filter_packet_callback, http_mux.get(), /*stream_index=*/1, bsfctx, _1, _2));
		}
	}
	video.configure_card();
	video.start_bm_capture();
	video.change_rate(10.0);  // Play as fast as possible.

	BasicStats basic_stats(/*verbose=*/false, /*use_opengl=*/false);
	global_basic_stats = &basic_stats;
	httpd.start(global_flags.http_port);

	signal(SIGUSR1, adjust_bitrate);
	signal(SIGUSR2, adjust_bitrate);
	signal(SIGINT, request_quit);

	while (!should_quit.should_quit()) {
		should_quit.sleep_for(hours(1000));
	}

	video.stop_dequeue_thread();
	// Stop the x264 encoder before killing the mux it's writing to.
	global_x264_encoder = nullptr;
	x264_encoder.reset();
	return 0;
}
