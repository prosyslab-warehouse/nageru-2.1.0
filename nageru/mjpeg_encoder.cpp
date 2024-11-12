#include "mjpeg_encoder.h"

#include <assert.h>
#include <jpeglib.h>
#include <unistd.h>
#if __SSE2__
#include <immintrin.h>
#endif
#include <list>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include "defs.h"
#include "shared/ffmpeg_raii.h"
#include "flags.h"
#include "shared/httpd.h"
#include "shared/memcpy_interleaved.h"
#include "shared/metrics.h"
#include "pbo_frame_allocator.h"
#include "shared/timebase.h"
#include "shared/va_display.h"

#include <movit/colorspace_conversion_effect.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_x11.h>

using namespace Eigen;
using namespace bmusb;
using namespace movit;
using namespace std;

static VAImageFormat uyvy_format, nv12_format;

extern void memcpy_with_pitch(uint8_t *dst, const uint8_t *src, size_t src_width, size_t dst_pitch, size_t height);

// The inverse of memcpy_interleaved(), with (slow) support for pitch.
void interleave_with_pitch(uint8_t *dst, const uint8_t *src1, const uint8_t *src2, size_t src_width, size_t dst_pitch, size_t height)
{
#if __SSE2__
	if (dst_pitch == src_width * 2 && (src_width * height) % 16 == 0) {
		__m128i *dptr = reinterpret_cast<__m128i *>(dst);
		const __m128i *sptr1 = reinterpret_cast<const __m128i *>(src1);
		const __m128i *sptr2 = reinterpret_cast<const __m128i *>(src2);
		for (size_t i = 0; i < src_width * height / 16; ++i) {
			__m128i data1 = _mm_loadu_si128(sptr1++);
			__m128i data2 = _mm_loadu_si128(sptr2++);
			_mm_storeu_si128(dptr++, _mm_unpacklo_epi8(data1, data2));
			_mm_storeu_si128(dptr++, _mm_unpackhi_epi8(data1, data2));
		}
		return;
	}
#endif

	for (size_t y = 0; y < height; ++y) {
		uint8_t *dptr = dst + y * dst_pitch;
		const uint8_t *sptr1 = src1 + y * src_width;
		const uint8_t *sptr2 = src2 + y * src_width;
		for (size_t x = 0; x < src_width; ++x) {
			*dptr++ = *sptr1++;
			*dptr++ = *sptr2++;
		}
	}
}

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

struct VectorDestinationManager {
	jpeg_destination_mgr pub;
	std::vector<uint8_t> dest;

	VectorDestinationManager()
	{
		pub.init_destination = init_destination_thunk;
		pub.empty_output_buffer = empty_output_buffer_thunk;
		pub.term_destination = term_destination_thunk;
	}

	static void init_destination_thunk(j_compress_ptr ptr)
	{
		((VectorDestinationManager *)(ptr->dest))->init_destination();
	}

	inline void init_destination()
	{
		make_room(0);
	}

	static boolean empty_output_buffer_thunk(j_compress_ptr ptr)
	{
		return ((VectorDestinationManager *)(ptr->dest))->empty_output_buffer();
	}

	inline bool empty_output_buffer()
	{
		make_room(dest.size());  // Should ignore pub.free_in_buffer!
		return true;
	}

	inline void make_room(size_t bytes_used)
	{
		dest.resize(bytes_used + 4096);
		dest.resize(dest.capacity());
		pub.next_output_byte = dest.data() + bytes_used;
		pub.free_in_buffer = dest.size() - bytes_used;
	}

	static void term_destination_thunk(j_compress_ptr ptr)
	{
		((VectorDestinationManager *)(ptr->dest))->term_destination();
	}

	inline void term_destination()
	{
		dest.resize(dest.size() - pub.free_in_buffer);
	}
};
static_assert(std::is_standard_layout<VectorDestinationManager>::value, "");

int MJPEGEncoder::write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	WritePacket2Context *ctx = (WritePacket2Context *)opaque;
	return ctx->mjpeg_encoder->write_packet2(ctx->stream_id, buf, buf_size, type, time);
}

int MJPEGEncoder::write_packet2(HTTPD::StreamID stream_id, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	string *mux_header = &streams[stream_id].mux_header;
	if (type == AVIO_DATA_MARKER_HEADER) {
		mux_header->append((char *)buf, buf_size);
		httpd->set_header(stream_id, *mux_header);
	} else {
		httpd->add_data(stream_id, (char *)buf, buf_size, /*keyframe=*/true, AV_NOPTS_VALUE, AVRational{ AV_TIME_BASE, 1 });
	}
	return buf_size;
}

namespace {

void add_video_stream(AVFormatContext *avctx)
{
	AVStream *stream = avformat_new_stream(avctx, nullptr);
	if (stream == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		abort();
	}

	// FFmpeg is very picky about having audio at 1/48000 timebase,
	// no matter what we write. Even though we'd prefer our usual 1/120000,
	// put the video on the same one, so that we can have locked audio.
	stream->time_base = AVRational{ 1, OUTPUT_FREQUENCY };
	stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;

	// Used for aspect ratio only. Can change without notice (the mux won't care).
	stream->codecpar->width = global_flags.width;
	stream->codecpar->height = global_flags.height;

	// TODO: We could perhaps use the interpretation for each card here
	// (or at least the command-line flags) instead of the defaults,
	// but what would we do when they change?
	stream->codecpar->color_primaries = AVCOL_PRI_BT709;
	stream->codecpar->color_trc = AVCOL_TRC_IEC61966_2_1;
	stream->codecpar->color_space = AVCOL_SPC_BT709;
	stream->codecpar->color_range = AVCOL_RANGE_MPEG;
	stream->codecpar->chroma_location = AVCHROMA_LOC_LEFT;
	stream->codecpar->field_order = AV_FIELD_PROGRESSIVE;
}

void add_audio_stream(AVFormatContext *avctx)
{
	AVStream *stream = avformat_new_stream(avctx, nullptr);
	if (stream == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		abort();
	}
	stream->time_base = AVRational{ 1, OUTPUT_FREQUENCY };
	stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
	stream->codecpar->codec_id = AV_CODEC_ID_PCM_S32LE;
	stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
	stream->codecpar->channels = 2;
	stream->codecpar->sample_rate = OUTPUT_FREQUENCY;
}

void finalize_mux(AVFormatContext *avctx)
{
	AVDictionary *options = NULL;
	vector<pair<string, string>> opts = MUX_OPTS;
	for (pair<string, string> opt : opts) {
		av_dict_set(&options, opt.first.c_str(), opt.second.c_str(), 0);
	}
	if (avformat_write_header(avctx, &options) < 0) {
		fprintf(stderr, "avformat_write_header() failed\n");
		abort();
	}
}

}  // namespace

MJPEGEncoder::MJPEGEncoder(HTTPD *httpd, const string &va_display)
	: httpd(httpd)
{
	create_ffmpeg_context(HTTPD::StreamID{ HTTPD::MULTICAM_STREAM, 0 });
	for (unsigned stream_idx = 0; stream_idx < MAX_VIDEO_CARDS; ++stream_idx) {
		create_ffmpeg_context(HTTPD::StreamID{ HTTPD::SIPHON_STREAM, stream_idx });
	}

	add_stream(HTTPD::StreamID{ HTTPD::MULTICAM_STREAM, 0 });

	// Initialize VA-API.
	VAConfigID config_id_422, config_id_420;
	string error;
	va_dpy = try_open_va(va_display, { VAProfileJPEGBaseline }, VAEntrypointEncPicture,
		{
			{ "4:2:2", VA_RT_FORMAT_YUV422, VA_FOURCC_UYVY, &config_id_422, &uyvy_format },
			// We'd prefer VA_FOURCC_I420, but it's not supported by Intel's driver.
			{ "4:2:0", VA_RT_FORMAT_YUV420, VA_FOURCC_NV12, &config_id_420, &nv12_format }
		},
		/*chosen_profile=*/nullptr, &error);
	if (va_dpy == nullptr) {
		fprintf(stderr, "Could not initialize VA-API for MJPEG encoding: %s. JPEGs will be encoded in software if needed.\n", error.c_str());
	}

	encoder_thread = thread(&MJPEGEncoder::encoder_thread_func, this);
	if (va_dpy != nullptr) {
		va_pool.reset(new VAResourcePool(va_dpy->va_dpy, uyvy_format, nv12_format, config_id_422, config_id_420, /*with_data_buffer=*/true));
		va_receiver_thread = thread(&MJPEGEncoder::va_receiver_thread_func, this);
	}

	global_metrics.add("mjpeg_frames", {{ "status", "dropped" }, { "reason", "zero_size" }}, &metric_mjpeg_frames_zero_size_dropped);
	global_metrics.add("mjpeg_frames", {{ "status", "dropped" }, { "reason", "interlaced" }}, &metric_mjpeg_frames_interlaced_dropped);
	global_metrics.add("mjpeg_frames", {{ "status", "dropped" }, { "reason", "unsupported_pixel_format" }}, &metric_mjpeg_frames_unsupported_pixel_format_dropped);
	global_metrics.add("mjpeg_frames", {{ "status", "dropped" }, { "reason", "oversized" }}, &metric_mjpeg_frames_oversized_dropped);
	global_metrics.add("mjpeg_frames", {{ "status", "dropped" }, { "reason", "overrun" }}, &metric_mjpeg_overrun_dropped);
	global_metrics.add("mjpeg_frames", {{ "status", "submitted" }}, &metric_mjpeg_overrun_submitted);

	running = true;
}

MJPEGEncoder::~MJPEGEncoder()
{
	for (auto &id_and_stream : streams) {
		av_free(id_and_stream.second.avctx->pb->buffer);
	}

	global_metrics.remove("mjpeg_frames", {{ "status", "dropped" }, { "reason", "zero_size" }});
	global_metrics.remove("mjpeg_frames", {{ "status", "dropped" }, { "reason", "interlaced" }});
	global_metrics.remove("mjpeg_frames", {{ "status", "dropped" }, { "reason", "unsupported_pixel_format" }});
	global_metrics.remove("mjpeg_frames", {{ "status", "dropped" }, { "reason", "oversized" }});
	global_metrics.remove("mjpeg_frames", {{ "status", "dropped" }, { "reason", "overrun" }});
	global_metrics.remove("mjpeg_frames", {{ "status", "submitted" }});
}

void MJPEGEncoder::stop()
{
	if (!running) {
		return;
	}
	running = false;
	should_quit = true;
	any_frames_to_be_encoded.notify_all();
	any_frames_encoding.notify_all();
	encoder_thread.join();
	if (va_dpy != nullptr) {
		va_receiver_thread.join();
	}
}

namespace {

bool is_uyvy(RefCountedFrame frame)
{
	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)frame->userdata;
	return userdata->pixel_format == PixelFormat_8BitYCbCr && frame->interleaved;
}

bool is_i420(RefCountedFrame frame)
{
	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)frame->userdata;
	return userdata->pixel_format == PixelFormat_8BitYCbCrPlanar &&
		userdata->ycbcr_format.chroma_subsampling_x == 2 &&
		userdata->ycbcr_format.chroma_subsampling_y == 2;
}

}  // namespace

void MJPEGEncoder::upload_frame(int64_t pts, unsigned card_index, RefCountedFrame frame, const bmusb::VideoFormat &video_format, size_t y_offset, size_t cbcr_offset, vector<int32_t> audio, const RGBTriplet &white_balance)
{
	if (video_format.width == 0 || video_format.height == 0) {
		++metric_mjpeg_frames_zero_size_dropped;
		return;
	}
	if (video_format.interlaced) {
		fprintf(stderr, "Card %u: Ignoring JPEG encoding for interlaced frame\n", card_index);
		++metric_mjpeg_frames_interlaced_dropped;
		return;
	}
	if (!is_uyvy(frame) && !is_i420(frame)) {
		fprintf(stderr, "Card %u: Ignoring JPEG encoding for unsupported pixel format\n", card_index);
		++metric_mjpeg_frames_unsupported_pixel_format_dropped;
		return;
	}
	if (video_format.width > 4096 || video_format.height > 4096) {
		fprintf(stderr, "Card %u: Ignoring JPEG encoding for oversized frame\n", card_index);
		++metric_mjpeg_frames_oversized_dropped;
		return;
	}

	lock_guard<mutex> lock(mu);
	if (frames_to_be_encoded.size() + frames_encoding.size() > 50) {
		fprintf(stderr, "WARNING: MJPEG encoding doesn't keep up, discarding frame.\n");
		++metric_mjpeg_overrun_dropped;
		return;
	}
	++metric_mjpeg_overrun_submitted;
	frames_to_be_encoded.push(QueuedFrame{ pts, card_index, frame, video_format, y_offset, cbcr_offset, move(audio), white_balance });
	any_frames_to_be_encoded.notify_all();
}

bool MJPEGEncoder::should_encode_mjpeg_for_card(unsigned card_index)
{
	// Only bother doing MJPEG encoding if there are any connected clients
	// that want the stream.
	if (httpd->get_num_connected_multicam_clients() == 0 &&
	    httpd->get_num_connected_siphon_clients(card_index) == 0) {
		return false;
	}

	auto it = global_flags.card_to_mjpeg_stream_export.find(card_index);
	return (it != global_flags.card_to_mjpeg_stream_export.end());
}

void MJPEGEncoder::encoder_thread_func()
{
	pthread_setname_np(pthread_self(), "MJPEG_Encode");
	posix_memalign((void **)&tmp_y, 4096, 4096 * 8);
	posix_memalign((void **)&tmp_cbcr, 4096, 4096 * 8);
	posix_memalign((void **)&tmp_cb, 4096, 4096 * 8);
	posix_memalign((void **)&tmp_cr, 4096, 4096 * 8);

	for (;;) {
		QueuedFrame qf;
		{
			unique_lock<mutex> lock(mu);
			any_frames_to_be_encoded.wait(lock, [this] { return !frames_to_be_encoded.empty() || should_quit; });
			if (should_quit) break;
			qf = move(frames_to_be_encoded.front());
			frames_to_be_encoded.pop();
		}

		assert(global_flags.card_to_mjpeg_stream_export.count(qf.card_index));  // Or should_encode_mjpeg_for_card() would have returned false.
		int stream_index = global_flags.card_to_mjpeg_stream_export[qf.card_index];

		if (va_dpy != nullptr) {
			// Will call back in the receiver thread.
			encode_jpeg_va(move(qf));
		} else {
			update_siphon_streams();

			HTTPD::StreamID multicam_id{ HTTPD::MULTICAM_STREAM, 0 };
			HTTPD::StreamID siphon_id{ HTTPD::SIPHON_STREAM, qf.card_index };
			assert(streams.count(multicam_id));

			// Write audio before video, since Futatabi expects it.
			if (qf.audio.size() > 0) {
				write_audio_packet(streams[multicam_id].avctx.get(), qf.pts, stream_index + global_flags.card_to_mjpeg_stream_export.size(), qf.audio);
				if (streams.count(siphon_id)) {
					write_audio_packet(streams[siphon_id].avctx.get(), qf.pts, /*stream_index=*/1, qf.audio);
				}
			}

			// Encode synchronously, in the same thread.
			vector<uint8_t> jpeg = encode_jpeg_libjpeg(qf);
			write_mjpeg_packet(streams[multicam_id].avctx.get(), qf.pts, stream_index, jpeg.data(), jpeg.size());
			if (streams.count(siphon_id)) {
				write_mjpeg_packet(streams[siphon_id].avctx.get(), qf.pts, /*stream_index=*/0, jpeg.data(), jpeg.size());
			}
		}
	}

	free(tmp_y);
	free(tmp_cbcr);
	free(tmp_cb);
	free(tmp_cr);
}

void MJPEGEncoder::write_mjpeg_packet(AVFormatContext *avctx, int64_t pts, unsigned stream_index, const uint8_t *jpeg, size_t jpeg_size)
{
	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.buf = nullptr;
	pkt.data = const_cast<uint8_t *>(jpeg);
	pkt.size = jpeg_size;
	pkt.stream_index = stream_index;
	pkt.flags = AV_PKT_FLAG_KEY;
	AVRational time_base = avctx->streams[pkt.stream_index]->time_base;
	pkt.pts = pkt.dts = av_rescale_q(pts, AVRational{ 1, TIMEBASE }, time_base);
	pkt.duration = 0;

	if (av_write_frame(avctx, &pkt) < 0) {
		fprintf(stderr, "av_write_frame() failed\n");
		abort();
	}
}

void MJPEGEncoder::write_audio_packet(AVFormatContext *avctx, int64_t pts, unsigned stream_index, const vector<int32_t> &audio)
{
	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.buf = nullptr;
	pkt.data = reinterpret_cast<uint8_t *>(const_cast<int32_t *>(&audio[0]));
	pkt.size = audio.size() * sizeof(audio[0]);
	pkt.stream_index = stream_index;
	pkt.flags = AV_PKT_FLAG_KEY;
	AVRational time_base = avctx->streams[pkt.stream_index]->time_base;
	pkt.pts = pkt.dts = av_rescale_q(pts, AVRational{ 1, TIMEBASE }, time_base);
	size_t num_stereo_samples = audio.size() / 2;
	pkt.duration = av_rescale_q(num_stereo_samples, AVRational{ 1, OUTPUT_FREQUENCY }, time_base);

	if (av_write_frame(avctx, &pkt) < 0) {
		fprintf(stderr, "av_write_frame() failed\n");
		abort();
	}
}

class VABufferDestroyer {
public:
	VABufferDestroyer(VADisplay dpy, VABufferID buf)
		: dpy(dpy), buf(buf) {}

	~VABufferDestroyer() {
		VAStatus va_status = vaDestroyBuffer(dpy, buf);
		CHECK_VASTATUS(va_status, "vaDestroyBuffer");
	}

private:
	VADisplay dpy;
	VABufferID buf;
};

namespace {

void push16(uint16_t val, string *str)
{
	str->push_back(val >> 8);
	str->push_back(val & 0xff);
}

void push32(uint32_t val, string *str)
{
	str->push_back(val >> 24);
	str->push_back((val >> 16) & 0xff);
	str->push_back((val >> 8) & 0xff);
	str->push_back(val & 0xff);
}

}  // namespace

void MJPEGEncoder::init_jpeg(unsigned width, unsigned height, const RGBTriplet &white_balance, VectorDestinationManager *dest, jpeg_compress_struct *cinfo, int y_h_samp_factor, int y_v_samp_factor)
{
	jpeg_error_mgr jerr;
	cinfo->err = jpeg_std_error(&jerr);
	jpeg_create_compress(cinfo);

	cinfo->dest = (jpeg_destination_mgr *)dest;

	cinfo->input_components = 3;
	jpeg_set_defaults(cinfo);
	jpeg_set_quality(cinfo, quality, /*force_baseline=*/false);

	cinfo->image_width = width;
	cinfo->image_height = height;
	cinfo->raw_data_in = true;
	jpeg_set_colorspace(cinfo, JCS_YCbCr);
	cinfo->comp_info[0].h_samp_factor = y_h_samp_factor;
	cinfo->comp_info[0].v_samp_factor = y_v_samp_factor;
	cinfo->comp_info[1].h_samp_factor = 1;
	cinfo->comp_info[1].v_samp_factor = 1;
	cinfo->comp_info[2].h_samp_factor = 1;
	cinfo->comp_info[2].v_samp_factor = 1;
	cinfo->CCIR601_sampling = true;  // Seems to be mostly ignored by libjpeg, though.
	jpeg_start_compress(cinfo, true);

	if (fabs(white_balance.r - 1.0f) > 1e-3 ||
	    fabs(white_balance.g - 1.0f) > 1e-3 ||
	    fabs(white_balance.b - 1.0f) > 1e-3) {
		// Convert from (linear) RGB to XYZ.
		Matrix3d rgb_to_xyz_matrix = movit::ColorspaceConversionEffect::get_xyz_matrix(COLORSPACE_sRGB);
		Vector3d xyz = rgb_to_xyz_matrix * Vector3d(white_balance.r, white_balance.g, white_balance.b);

		// Convert from XYZ to xyz by normalizing.
		xyz /= (xyz[0] + xyz[1] + xyz[2]);

		// Create a very rudimentary EXIF header to hold our white point.
		string exif;

		// Exif header, followed by some padding.
		exif = "Exif";
		push16(0, &exif);

		// TIFF header first:
		exif += "MM";  // Big endian.

		// Magic number.
		push16(42, &exif);

		// Offset of first IFD (relative to the MM, immediately after the header).
		push32(exif.size() - 6 + 4, &exif);

		// Now the actual IFD.

		// One entry.
		push16(1, &exif);

		// WhitePoint tag ID.
		push16(0x13e, &exif);

		// Rational type.
		push16(5, &exif);

		// Two values (x and y; z is implicit due to normalization).
		push32(2, &exif);

		// Offset (relative to the MM, immediately after the last IFD).
		push32(exif.size() - 6 + 8, &exif);

		// No more IFDs.
		push32(0, &exif);

		// The actual values.
		push32(lrintf(xyz[0] * 10000.0f), &exif);
		push32(10000, &exif);
		push32(lrintf(xyz[1] * 10000.0f), &exif);
		push32(10000, &exif);

		jpeg_write_marker(cinfo, JPEG_APP0 + 1, (const JOCTET *)exif.data(), exif.size());
	}

	// This comment marker is private to FFmpeg. It signals limited Y'CbCr range
	// (and nothing else).
	jpeg_write_marker(cinfo, JPEG_COM, (const JOCTET *)"CS=ITU601", strlen("CS=ITU601"));
}

vector<uint8_t> MJPEGEncoder::get_jpeg_header(unsigned width, unsigned height, const RGBTriplet &white_balance, int y_h_samp_factor, int y_v_samp_factor, jpeg_compress_struct *cinfo)
{
	VectorDestinationManager dest;
	init_jpeg(width, height, white_balance, &dest, cinfo, y_h_samp_factor, y_v_samp_factor);

	// Make a dummy black image; there's seemingly no other easy way of
	// making libjpeg outputting all of its headers.
	assert(y_v_samp_factor <= 2);  // Or we'd need larger JSAMPROW arrays below.
	size_t block_height_y = 8 * y_v_samp_factor;
	size_t block_height_cbcr = 8;

	JSAMPROW yptr[16], cbptr[16], crptr[16];
	JSAMPARRAY data[3] = { yptr, cbptr, crptr };
	memset(tmp_y, 0, 4096);
	memset(tmp_cb, 0, 4096);
	memset(tmp_cr, 0, 4096);
	for (unsigned yy = 0; yy < block_height_y; ++yy) {
		yptr[yy] = tmp_y;
	}
	for (unsigned yy = 0; yy < block_height_cbcr; ++yy) {
		cbptr[yy] = tmp_cb;
		crptr[yy] = tmp_cr;
	}
	for (unsigned y = 0; y < height; y += block_height_y) {
		jpeg_write_raw_data(cinfo, data, block_height_y);
	}
	jpeg_finish_compress(cinfo);

	// We're only interested in the header, not the data after it.
	dest.term_destination();
	for (size_t i = 0; i < dest.dest.size() - 1; ++i) {
		if (dest.dest[i] == 0xff && dest.dest[i + 1] == 0xda) {  // Start of scan (SOS).
			unsigned len = dest.dest[i + 2] * 256 + dest.dest[i + 3];
			dest.dest.resize(i + len + 2);
			break;
		}
	}

	return dest.dest;
}

MJPEGEncoder::VAData MJPEGEncoder::get_va_data_for_parameters(unsigned width, unsigned height, unsigned y_h_samp_factor, unsigned y_v_samp_factor, const RGBTriplet &white_balance)
{
	VAKey key{width, height, y_h_samp_factor, y_v_samp_factor, white_balance};
	if (va_data_for_parameters.count(key)) {
		return va_data_for_parameters[key];
	}

	// Use libjpeg to generate a header and set sane defaults for e.g.
	// quantization tables. Then do the actual encode with VA-API.
	jpeg_compress_struct cinfo;
	vector<uint8_t> jpeg_header = get_jpeg_header(width, height, white_balance, y_h_samp_factor, y_v_samp_factor, &cinfo);

	// Picture parameters.
	VAEncPictureParameterBufferJPEG pic_param;
	memset(&pic_param, 0, sizeof(pic_param));
	pic_param.reconstructed_picture = VA_INVALID_ID;
	pic_param.picture_width = cinfo.image_width;
	pic_param.picture_height = cinfo.image_height;
	for (int component_idx = 0; component_idx < cinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &cinfo.comp_info[component_idx];
		pic_param.component_id[component_idx] = comp->component_id;
		pic_param.quantiser_table_selector[component_idx] = comp->quant_tbl_no;
	}
	pic_param.num_components = cinfo.num_components;
	pic_param.num_scan = 1;
	pic_param.sample_bit_depth = 8;
	pic_param.coded_buf = VA_INVALID_ID;  // To be filled out by caller.
	pic_param.pic_flags.bits.huffman = 1;
	pic_param.quality = 50;  // Don't scale the given quantization matrices. (See gen8_mfc_jpeg_fqm_state)

	// Quantization matrices.
	VAQMatrixBufferJPEG q;
	memset(&q, 0, sizeof(q));

	q.load_lum_quantiser_matrix = true;
	q.load_chroma_quantiser_matrix = true;
	for (int quant_tbl_idx = 0; quant_tbl_idx < min(4, NUM_QUANT_TBLS); ++quant_tbl_idx) {
		const JQUANT_TBL *qtbl = cinfo.quant_tbl_ptrs[quant_tbl_idx];
		assert((qtbl == nullptr) == (quant_tbl_idx >= 2));
		if (qtbl == nullptr) continue;

		uint8_t *qmatrix = (quant_tbl_idx == 0) ? q.lum_quantiser_matrix : q.chroma_quantiser_matrix;
		for (int i = 0; i < 64; ++i) {
			if (qtbl->quantval[i] > 255) {
				fprintf(stderr, "Baseline JPEG only!\n");
				abort();
			}
			qmatrix[i] = qtbl->quantval[jpeg_natural_order[i]];
		}
	}

	// Huffman tables (arithmetic is not supported).
	VAHuffmanTableBufferJPEGBaseline huff;
	memset(&huff, 0, sizeof(huff));

	for (int huff_tbl_idx = 0; huff_tbl_idx < min(2, NUM_HUFF_TBLS); ++huff_tbl_idx) {
		const JHUFF_TBL *ac_hufftbl = cinfo.ac_huff_tbl_ptrs[huff_tbl_idx];
		const JHUFF_TBL *dc_hufftbl = cinfo.dc_huff_tbl_ptrs[huff_tbl_idx];
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

	// Slice parameters (metadata about the slice).
	VAEncSliceParameterBufferJPEG parms;
	memset(&parms, 0, sizeof(parms));
	for (int component_idx = 0; component_idx < cinfo.num_components; ++component_idx) {
		const jpeg_component_info *comp = &cinfo.comp_info[component_idx];
		parms.components[component_idx].component_selector = comp->component_id;
		parms.components[component_idx].dc_table_selector = comp->dc_tbl_no;
		parms.components[component_idx].ac_table_selector = comp->ac_tbl_no;
		if (parms.components[component_idx].dc_table_selector > 1 ||
		    parms.components[component_idx].ac_table_selector > 1) {
			fprintf(stderr, "Uses too many Huffman tables\n");
			abort();
		}
	}
	parms.num_components = cinfo.num_components;
	parms.restart_interval = cinfo.restart_interval;

	jpeg_destroy_compress(&cinfo);

	VAData ret;
	ret.jpeg_header = move(jpeg_header);
	ret.pic_param = pic_param;
	ret.q = q;
	ret.huff = huff;
	ret.parms = parms;
	va_data_for_parameters[key] = ret;
	return ret;
}

void MJPEGEncoder::encode_jpeg_va(QueuedFrame &&qf)
{
	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)qf.frame->userdata;
	unsigned width = qf.video_format.width;
	unsigned height = qf.video_format.height;

	VAResourcePool::VAResources resources;
	ReleaseVAResources release;
	if (userdata->data_copy_current_src == PBOFrameAllocator::Userdata::FROM_VA_API) {
		assert(is_uyvy(qf.frame));
		resources = move(userdata->va_resources);
		release = move(userdata->va_resources_release);
	} else {
		assert(userdata->data_copy_current_src == PBOFrameAllocator::Userdata::FROM_MALLOC);
		if (is_uyvy(qf.frame)) {
			resources = va_pool->get_va_resources(width, height, VA_FOURCC_UYVY);
		} else {
			assert(is_i420(qf.frame));
			resources = va_pool->get_va_resources(width, height, VA_FOURCC_NV12);
		}
		release = ReleaseVAResources(va_pool.get(), resources);
	}

	int y_h_samp_factor, y_v_samp_factor;
	if (is_uyvy(qf.frame)) {
		// 4:2:2 (sample Y' twice as often horizontally as Cb or Cr, vertical is left alone).
		y_h_samp_factor = 2;
		y_v_samp_factor = 1;
	} else {
		// 4:2:0 (sample Y' twice as often as Cb or Cr, in both directions)
		assert(is_i420(qf.frame));
		y_h_samp_factor = 2;
		y_v_samp_factor = 2;
	}

	VAData va_data = get_va_data_for_parameters(width, height, y_h_samp_factor, y_v_samp_factor, qf.white_balance);
	va_data.pic_param.coded_buf = resources.data_buffer;

	VABufferID pic_param_buffer;
	VAStatus va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAEncPictureParameterBufferType, sizeof(va_data.pic_param), 1, &va_data.pic_param, &pic_param_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_pic_param(va_dpy->va_dpy, pic_param_buffer);

	VABufferID q_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAQMatrixBufferType, sizeof(va_data.q), 1, &va_data.q, &q_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_iq(va_dpy->va_dpy, q_buffer);

	VABufferID huff_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAHuffmanTableBufferType, sizeof(va_data.huff), 1, &va_data.huff, &huff_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_huff(va_dpy->va_dpy, huff_buffer);

	VABufferID slice_param_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAEncSliceParameterBufferType, sizeof(va_data.parms), 1, &va_data.parms, &slice_param_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_slice_param(va_dpy->va_dpy, slice_param_buffer);

	if (userdata->data_copy_current_src == PBOFrameAllocator::Userdata::FROM_VA_API) {
		// The pixel data is already put into the image by the caller.
		va_status = vaUnmapBuffer(va_dpy->va_dpy, resources.image.buf);
		CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	} else {
		assert(userdata->data_copy_current_src == PBOFrameAllocator::Userdata::FROM_MALLOC);

		// Upload the pixel data.
		uint8_t *surface_p = nullptr;
		vaMapBuffer(va_dpy->va_dpy, resources.image.buf, (void **)&surface_p);

		if (is_uyvy(qf.frame)) {
			size_t field_start_line = qf.video_format.extra_lines_top;  // No interlacing support.
			size_t field_start = qf.cbcr_offset * 2 + qf.video_format.width * field_start_line * 2;

			const uint8_t *src = qf.frame->data_copy + field_start;
			uint8_t *dst = (unsigned char *)surface_p + resources.image.offsets[0];
			memcpy_with_pitch(dst, src, qf.video_format.width * 2, resources.image.pitches[0], qf.video_format.height);
		} else {
			assert(is_i420(qf.frame));
			assert(!qf.frame->interleaved);  // Makes no sense for I420.

			size_t field_start_line = qf.video_format.extra_lines_top;  // No interlacing support.
			const uint8_t *y_src = qf.frame->data + qf.video_format.width * field_start_line;
			const uint8_t *cb_src = y_src + width * height;
			const uint8_t *cr_src = cb_src + (width / 2) * (height / 2);

			uint8_t *y_dst = (unsigned char *)surface_p + resources.image.offsets[0];
			uint8_t *cbcr_dst = (unsigned char *)surface_p + resources.image.offsets[1];

			memcpy_with_pitch(y_dst, y_src, qf.video_format.width, resources.image.pitches[0], qf.video_format.height);
			interleave_with_pitch(cbcr_dst, cb_src, cr_src, qf.video_format.width / 2, resources.image.pitches[1], qf.video_format.height / 2);
		}

		va_status = vaUnmapBuffer(va_dpy->va_dpy, resources.image.buf);
		CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	}

	qf.frame->data_copy = nullptr;

	// Seemingly vaPutImage() (which triggers a GPU copy) is much nicer to the
	// CPU than vaDeriveImage() and copying directly into the GPU's buffers.
	// Exactly why is unclear, but it seems to involve L3 cache usage when there
	// are many high-res (1080p+) images in play.
	va_status = vaPutImage(va_dpy->va_dpy, resources.surface, resources.image.image_id, 0, 0, width, height, 0, 0, width, height);
	CHECK_VASTATUS(va_status, "vaPutImage");

	// Finally, stick in the JPEG header.
	VAEncPackedHeaderParameterBuffer header_parm;
	header_parm.type = VAEncPackedHeaderRawData;
	header_parm.bit_length = 8 * va_data.jpeg_header.size();

	VABufferID header_parm_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAEncPackedHeaderParameterBufferType, sizeof(header_parm), 1, &header_parm, &header_parm_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_header(va_dpy->va_dpy, header_parm_buffer);

	VABufferID header_data_buffer;
	va_status = vaCreateBuffer(va_dpy->va_dpy, resources.context, VAEncPackedHeaderDataBufferType, va_data.jpeg_header.size(), 1, va_data.jpeg_header.data(), &header_data_buffer);
	CHECK_VASTATUS(va_status, "vaCreateBuffer");
	VABufferDestroyer destroy_header_data(va_dpy->va_dpy, header_data_buffer);

	va_status = vaBeginPicture(va_dpy->va_dpy, resources.context, resources.surface);
	CHECK_VASTATUS(va_status, "vaBeginPicture");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &pic_param_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(pic_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &q_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(q)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &huff_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(huff)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &slice_param_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(slice_param)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &header_parm_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(header_parm)");
	va_status = vaRenderPicture(va_dpy->va_dpy, resources.context, &header_data_buffer, 1);
	CHECK_VASTATUS(va_status, "vaRenderPicture(header_data)");
	va_status = vaEndPicture(va_dpy->va_dpy, resources.context);
	CHECK_VASTATUS(va_status, "vaEndPicture");

	qf.resources = move(resources);
	qf.resource_releaser = move(release);

	lock_guard<mutex> lock(mu);
	frames_encoding.push(move(qf));
	any_frames_encoding.notify_all();
}

void MJPEGEncoder::va_receiver_thread_func()
{
	pthread_setname_np(pthread_self(), "MJPEG_Receive");
	for (;;) {
		QueuedFrame qf;
		{
			unique_lock<mutex> lock(mu);
			any_frames_encoding.wait(lock, [this] { return !frames_encoding.empty() || should_quit; });
			if (should_quit) return;
			qf = move(frames_encoding.front());
			frames_encoding.pop();
		}

		update_siphon_streams();

		assert(global_flags.card_to_mjpeg_stream_export.count(qf.card_index));  // Or should_encode_mjpeg_for_card() would have returned false.
		int stream_index = global_flags.card_to_mjpeg_stream_export[qf.card_index];

		HTTPD::StreamID multicam_id{ HTTPD::MULTICAM_STREAM, 0 };
		HTTPD::StreamID siphon_id{ HTTPD::SIPHON_STREAM, qf.card_index };
		assert(streams.count(multicam_id));
		assert(streams[multicam_id].avctx != nullptr);

		// Write audio before video, since Futatabi expects it.
		if (qf.audio.size() > 0) {
			write_audio_packet(streams[multicam_id].avctx.get(), qf.pts, stream_index + global_flags.card_to_mjpeg_stream_export.size(), qf.audio);
			if (streams.count(siphon_id)) {
				write_audio_packet(streams[siphon_id].avctx.get(), qf.pts, /*stream_index=*/1, qf.audio);
			}
		}

		VAStatus va_status = vaSyncSurface(va_dpy->va_dpy, qf.resources.surface);
		CHECK_VASTATUS(va_status, "vaSyncSurface");

		VACodedBufferSegment *segment;
		va_status = vaMapBuffer(va_dpy->va_dpy, qf.resources.data_buffer, (void **)&segment);
		CHECK_VASTATUS(va_status, "vaMapBuffer");

		const uint8_t *coded_buf = reinterpret_cast<uint8_t *>(segment->buf);
		write_mjpeg_packet(streams[multicam_id].avctx.get(), qf.pts, stream_index, coded_buf, segment->size);
		if (streams.count(siphon_id)) {
			write_mjpeg_packet(streams[siphon_id].avctx.get(), qf.pts, /*stream_index=*/0, coded_buf, segment->size);
		}

		va_status = vaUnmapBuffer(va_dpy->va_dpy, qf.resources.data_buffer);
		CHECK_VASTATUS(va_status, "vaUnmapBuffer");
	}
}

vector<uint8_t> MJPEGEncoder::encode_jpeg_libjpeg(const QueuedFrame &qf)
{
	unsigned width = qf.video_format.width;
	unsigned height = qf.video_format.height;

	VectorDestinationManager dest;
	jpeg_compress_struct cinfo;

	size_t field_start_line = qf.video_format.extra_lines_top;  // No interlacing support.

	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)qf.frame->userdata;
	if (userdata->pixel_format == PixelFormat_8BitYCbCr) {
		init_jpeg(width, height, qf.white_balance, &dest, &cinfo, /*y_h_samp_factor=*/2, /*y_v_samp_factor=*/1);

		assert(qf.frame->interleaved);
		size_t field_start = qf.cbcr_offset * 2 + qf.video_format.width * field_start_line * 2;

		JSAMPROW yptr[8], cbptr[8], crptr[8];
		JSAMPARRAY data[3] = { yptr, cbptr, crptr };
		for (unsigned y = 0; y < qf.video_format.height; y += 8) {
			const uint8_t *src;
			src = qf.frame->data_copy + field_start + y * qf.video_format.width * 2;

			memcpy_interleaved(tmp_cbcr, tmp_y, src, qf.video_format.width * 8 * 2);
			memcpy_interleaved(tmp_cb, tmp_cr, tmp_cbcr, qf.video_format.width * 8);
			for (unsigned yy = 0; yy < 8; ++yy) {
				yptr[yy] = tmp_y + yy * width;
				cbptr[yy] = tmp_cb + yy * width / 2;
				crptr[yy] = tmp_cr + yy * width / 2;
			}
			jpeg_write_raw_data(&cinfo, data, /*num_lines=*/8);
		}
	} else {
		assert(userdata->pixel_format == PixelFormat_8BitYCbCrPlanar);

		const movit::YCbCrFormat &ycbcr = userdata->ycbcr_format;
		init_jpeg(width, height, qf.white_balance, &dest, &cinfo, ycbcr.chroma_subsampling_x, ycbcr.chroma_subsampling_y);
		assert(ycbcr.chroma_subsampling_y <= 2);  // Or we'd need larger JSAMPROW arrays below.

		size_t field_start_line = qf.video_format.extra_lines_top;  // No interlacing support.
		const uint8_t *y_start = qf.frame->data + qf.video_format.width * field_start_line;
		const uint8_t *cb_start = y_start + width * height;
		const uint8_t *cr_start = cb_start + (width / ycbcr.chroma_subsampling_x) * (height / ycbcr.chroma_subsampling_y);

		size_t block_height_y = 8 * ycbcr.chroma_subsampling_y;
		size_t block_height_cbcr = 8;

		JSAMPROW yptr[16], cbptr[16], crptr[16];
		JSAMPARRAY data[3] = { yptr, cbptr, crptr };
		for (unsigned y = 0; y < qf.video_format.height; y += block_height_y) {
			for (unsigned yy = 0; yy < block_height_y; ++yy) {
				yptr[yy] = const_cast<JSAMPROW>(y_start) + (y + yy) * width;
			}
			unsigned cbcr_y = y / ycbcr.chroma_subsampling_y;
			for (unsigned yy = 0; yy < block_height_cbcr; ++yy) {
				cbptr[yy] = const_cast<JSAMPROW>(cb_start) + (cbcr_y + yy) * width / ycbcr.chroma_subsampling_x;
				crptr[yy] = const_cast<JSAMPROW>(cr_start) + (cbcr_y + yy) * width / ycbcr.chroma_subsampling_x;
			}
			jpeg_write_raw_data(&cinfo, data, block_height_y);
		}
	}
	jpeg_finish_compress(&cinfo);

	return dest.dest;
}

void MJPEGEncoder::add_stream(HTTPD::StreamID stream_id)
{
	AVFormatContextWithCloser avctx;

	// Set up the mux. We don't use the Mux wrapper, because it's geared towards
	// a situation with only one video stream (and possibly one audio stream)
	// with known width/height, and we don't need the extra functionality it provides.
	avctx.reset(avformat_alloc_context());
	avctx->oformat = av_guess_format("nut", nullptr, nullptr);

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, &ffmpeg_contexts[stream_id], nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &MJPEGEncoder::write_packet2_thunk;
	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	if (stream_id.type == HTTPD::MULTICAM_STREAM) {
		for (unsigned card_idx = 0; card_idx < global_flags.card_to_mjpeg_stream_export.size(); ++card_idx) {
			add_video_stream(avctx.get());
		}
		for (unsigned card_idx = 0; card_idx < global_flags.card_to_mjpeg_stream_export.size(); ++card_idx) {
			add_audio_stream(avctx.get());
		}
	} else {
		assert(stream_id.type == HTTPD::SIPHON_STREAM);
		add_video_stream(avctx.get());
		add_audio_stream(avctx.get());
	}
	finalize_mux(avctx.get());

	Stream s;
	s.avctx = move(avctx);
	streams[stream_id] = move(s);
}

void MJPEGEncoder::update_siphon_streams()
{
	// Bring the list of streams into sync with what the clients need.
	for (auto it = streams.begin(); it != streams.end(); ) {
		if (it->first.type != HTTPD::SIPHON_STREAM) {
			++it;
			continue;
		}
		if (httpd->get_num_connected_siphon_clients(it->first.index) == 0) {
			av_free(it->second.avctx->pb->buffer);
			streams.erase(it++);
		} else {
			++it;
		}
	}
	for (unsigned stream_idx = 0; stream_idx < MAX_VIDEO_CARDS; ++stream_idx) {
		HTTPD::StreamID stream_id{ HTTPD::SIPHON_STREAM, stream_idx };
		if (streams.count(stream_id) == 0 && httpd->get_num_connected_siphon_clients(stream_idx) > 0) {
			add_stream(stream_id);
		}
	}
}

void MJPEGEncoder::create_ffmpeg_context(HTTPD::StreamID stream_id)
{
	ffmpeg_contexts.emplace(stream_id, WritePacket2Context{ this, stream_id });
}
