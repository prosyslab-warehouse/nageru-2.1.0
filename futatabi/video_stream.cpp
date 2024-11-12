#include "video_stream.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
}

#include "chroma_subsampler.h"
#include "exif_parser.h"
#include "flags.h"
#include "flow.h"
#include "jpeg_frame_view.h"
#include "movit/util.h"
#include "pbo_pool.h"
#include "player.h"
#include "shared/context.h"
#include "shared/httpd.h"
#include "shared/metrics.h"
#include "shared/shared_defs.h"
#include "shared/mux.h"
#include "util.h"
#include "ycbcr_converter.h"

#include <epoxy/glx.h>
#include <jpeglib.h>
#include <unistd.h>

using namespace movit;
using namespace std;
using namespace std::chrono;

namespace {

once_flag video_metrics_inited;
Summary metric_jpeg_encode_time_seconds;
Summary metric_fade_latency_seconds;
Summary metric_interpolation_latency_seconds;
Summary metric_fade_fence_wait_time_seconds;
Summary metric_interpolation_fence_wait_time_seconds;

void wait_for_upload(shared_ptr<Frame> &frame)
{
	if (frame->uploaded_interpolation != nullptr) {
		glWaitSync(frame->uploaded_interpolation.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
		frame->uploaded_interpolation.reset();
	}
}

}  // namespace

extern HTTPD *global_httpd;

struct VectorDestinationManager {
	jpeg_destination_mgr pub;
	string dest;

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
		pub.next_output_byte = (uint8_t *)dest.data() + bytes_used;
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

string encode_jpeg(const uint8_t *y_data, const uint8_t *cb_data, const uint8_t *cr_data, unsigned width, unsigned height, const string exif_data)
{
	steady_clock::time_point start = steady_clock::now();
	VectorDestinationManager dest;

	jpeg_compress_struct cinfo;
	jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);

	cinfo.dest = (jpeg_destination_mgr *)&dest;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	constexpr int quality = 90;
	jpeg_set_quality(&cinfo, quality, /*force_baseline=*/false);

	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.raw_data_in = true;
	jpeg_set_colorspace(&cinfo, JCS_YCbCr);
	cinfo.comp_info[0].h_samp_factor = 2;
	cinfo.comp_info[0].v_samp_factor = 1;
	cinfo.comp_info[1].h_samp_factor = 1;
	cinfo.comp_info[1].v_samp_factor = 1;
	cinfo.comp_info[2].h_samp_factor = 1;
	cinfo.comp_info[2].v_samp_factor = 1;
	cinfo.CCIR601_sampling = true;  // Seems to be mostly ignored by libjpeg, though.
	jpeg_start_compress(&cinfo, true);

	// This comment marker is private to FFmpeg. It signals limited Y'CbCr range
	// (and nothing else).
	jpeg_write_marker(&cinfo, JPEG_COM, (const JOCTET *)"CS=ITU601", strlen("CS=ITU601"));

	if (!exif_data.empty()) {
		jpeg_write_marker(&cinfo, JPEG_APP0 + 1, (const JOCTET *)exif_data.data(), exif_data.size());
	}

	JSAMPROW yptr[8], cbptr[8], crptr[8];
	JSAMPARRAY data[3] = { yptr, cbptr, crptr };
	for (unsigned y = 0; y < height; y += 8) {
		for (unsigned yy = 0; yy < 8; ++yy) {
			yptr[yy] = const_cast<JSAMPROW>(&y_data[(y + yy) * width]);
			cbptr[yy] = const_cast<JSAMPROW>(&cb_data[(y + yy) * width / 2]);
			crptr[yy] = const_cast<JSAMPROW>(&cr_data[(y + yy) * width / 2]);
		}

		jpeg_write_raw_data(&cinfo, data, /*num_lines=*/8);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	steady_clock::time_point stop = steady_clock::now();
	metric_jpeg_encode_time_seconds.count_event(duration<double>(stop - start).count());

	return move(dest.dest);
}

string encode_jpeg_from_pbo(void *contents, unsigned width, unsigned height, const string exif_data)
{
	unsigned chroma_width = width / 2;

	const uint8_t *y = (const uint8_t *)contents;
	const uint8_t *cb = (const uint8_t *)contents + width * height;
	const uint8_t *cr = (const uint8_t *)contents + width * height + chroma_width * height;
	return encode_jpeg(y, cb, cr, width, height, move(exif_data));
}

VideoStream::VideoStream(AVFormatContext *file_avctx)
	: avctx(file_avctx), output_fast_forward(file_avctx != nullptr)
{
	call_once(video_metrics_inited, [] {
		vector<double> quantiles{ 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99 };
		metric_jpeg_encode_time_seconds.init(quantiles, 60.0);
		global_metrics.add("jpeg_encode_time_seconds", &metric_jpeg_encode_time_seconds);
		metric_fade_fence_wait_time_seconds.init(quantiles, 60.0);
		global_metrics.add("fade_fence_wait_time_seconds", &metric_fade_fence_wait_time_seconds);
		metric_interpolation_fence_wait_time_seconds.init(quantiles, 60.0);
		global_metrics.add("interpolation_fence_wait_time_seconds", &metric_interpolation_fence_wait_time_seconds);
		metric_fade_latency_seconds.init(quantiles, 60.0);
		global_metrics.add("fade_latency_seconds", &metric_fade_latency_seconds);
		metric_interpolation_latency_seconds.init(quantiles, 60.0);
		global_metrics.add("interpolation_latency_seconds", &metric_interpolation_latency_seconds);
	});

	ycbcr_converter.reset(new YCbCrConverter(YCbCrConverter::OUTPUT_TO_DUAL_YCBCR, /*resource_pool=*/nullptr));
	ycbcr_semiplanar_converter.reset(new YCbCrConverter(YCbCrConverter::OUTPUT_TO_SEMIPLANAR, /*resource_pool=*/nullptr));

	GLuint input_tex[num_interpolate_slots], gray_tex[num_interpolate_slots];
	GLuint fade_y_output_tex[num_interpolate_slots], fade_cbcr_output_tex[num_interpolate_slots];
	GLuint cb_tex[num_interpolate_slots], cr_tex[num_interpolate_slots];

	glCreateTextures(GL_TEXTURE_2D_ARRAY, num_interpolate_slots, input_tex);
	glCreateTextures(GL_TEXTURE_2D_ARRAY, num_interpolate_slots, gray_tex);
	glCreateTextures(GL_TEXTURE_2D, num_interpolate_slots, fade_y_output_tex);
	glCreateTextures(GL_TEXTURE_2D, num_interpolate_slots, fade_cbcr_output_tex);
	glCreateTextures(GL_TEXTURE_2D, num_interpolate_slots, cb_tex);
	glCreateTextures(GL_TEXTURE_2D, num_interpolate_slots, cr_tex);
	check_error();

	size_t width = global_flags.width, height = global_flags.height;
	int levels = find_num_levels(width, height);
	for (size_t i = 0; i < num_interpolate_slots; ++i) {
		glTextureStorage3D(input_tex[i], levels, GL_RGBA8, width, height, 2);
		check_error();
		glTextureStorage3D(gray_tex[i], levels, GL_R8, width, height, 2);
		check_error();
		glTextureStorage2D(fade_y_output_tex[i], 1, GL_R8, width, height);
		check_error();
		glTextureStorage2D(fade_cbcr_output_tex[i], 1, GL_RG8, width, height);
		check_error();
		glTextureStorage2D(cb_tex[i], 1, GL_R8, width / 2, height);
		check_error();
		glTextureStorage2D(cr_tex[i], 1, GL_R8, width / 2, height);
		check_error();

		unique_ptr<InterpolatedFrameResources> resource(new InterpolatedFrameResources);
		resource->owner = this;
		resource->input_tex = input_tex[i];
		resource->gray_tex = gray_tex[i];
		resource->fade_y_output_tex = fade_y_output_tex[i];
		resource->fade_cbcr_output_tex = fade_cbcr_output_tex[i];
		resource->cb_tex = cb_tex[i];
		resource->cr_tex = cr_tex[i];
		glCreateFramebuffers(2, resource->input_fbos);
		check_error();
		glCreateFramebuffers(1, &resource->fade_fbo);
		check_error();

		glNamedFramebufferTextureLayer(resource->input_fbos[0], GL_COLOR_ATTACHMENT0, input_tex[i], 0, 0);
		check_error();
		glNamedFramebufferTextureLayer(resource->input_fbos[0], GL_COLOR_ATTACHMENT1, gray_tex[i], 0, 0);
		check_error();
		glNamedFramebufferTextureLayer(resource->input_fbos[1], GL_COLOR_ATTACHMENT0, input_tex[i], 0, 1);
		check_error();
		glNamedFramebufferTextureLayer(resource->input_fbos[1], GL_COLOR_ATTACHMENT1, gray_tex[i], 0, 1);
		check_error();
		glNamedFramebufferTexture(resource->fade_fbo, GL_COLOR_ATTACHMENT0, fade_y_output_tex[i], 0);
		check_error();
		glNamedFramebufferTexture(resource->fade_fbo, GL_COLOR_ATTACHMENT1, fade_cbcr_output_tex[i], 0);
		check_error();

		GLuint bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glNamedFramebufferDrawBuffers(resource->input_fbos[0], 2, bufs);
		check_error();
		glNamedFramebufferDrawBuffers(resource->input_fbos[1], 2, bufs);
		check_error();
		glNamedFramebufferDrawBuffers(resource->fade_fbo, 2, bufs);
		check_error();

		glCreateBuffers(1, &resource->pbo);
		check_error();
		glNamedBufferStorage(resource->pbo, width * height * 4, nullptr, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
		check_error();
		resource->pbo_contents = glMapNamedBufferRange(resource->pbo, 0, width * height * 4, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
		interpolate_resources.push_back(move(resource));
	}

	check_error();

	OperatingPoint op;
	if (global_flags.interpolation_quality == 0 ||
	    global_flags.interpolation_quality == 1) {
		op = operating_point1;
	} else if (global_flags.interpolation_quality == 2) {
		op = operating_point2;
	} else if (global_flags.interpolation_quality == 3) {
		op = operating_point3;
	} else if (global_flags.interpolation_quality == 4) {
		op = operating_point4;
	} else {
		// Quality 0 will be changed to 1 in flags.cpp.
		assert(false);
	}

	compute_flow.reset(new DISComputeFlow(width, height, op));
	interpolate.reset(new Interpolate(op, /*split_ycbcr_output=*/true));
	interpolate_no_split.reset(new Interpolate(op, /*split_ycbcr_output=*/false));
	chroma_subsampler.reset(new ChromaSubsampler);
	check_error();

	// The “last frame” is initially black.
	unique_ptr<uint8_t[]> y(new uint8_t[global_flags.width * global_flags.height]);
	unique_ptr<uint8_t[]> cb_or_cr(new uint8_t[(global_flags.width / 2) * global_flags.height]);
	memset(y.get(), 16, global_flags.width * global_flags.height);
	memset(cb_or_cr.get(), 128, (global_flags.width / 2) * global_flags.height);
	last_frame = encode_jpeg(y.get(), cb_or_cr.get(), cb_or_cr.get(), global_flags.width, global_flags.height, /*exif_data=*/"");

	if (file_avctx != nullptr) {
		with_subtitles = Mux::WITHOUT_SUBTITLES;
	} else {
		with_subtitles = Mux::WITH_SUBTITLES;
	}
}

VideoStream::~VideoStream()
{
	if (last_flow_tex != 0) {
		compute_flow->release_texture(last_flow_tex);
	}

	for (const unique_ptr<InterpolatedFrameResources> &resource : interpolate_resources) {
		glUnmapNamedBuffer(resource->pbo);
		check_error();
		glDeleteBuffers(1, &resource->pbo);
		check_error();
		glDeleteFramebuffers(2, resource->input_fbos);
		check_error();
		glDeleteFramebuffers(1, &resource->fade_fbo);
		check_error();
		glDeleteTextures(1, &resource->input_tex);
		check_error();
		glDeleteTextures(1, &resource->gray_tex);
		check_error();
		glDeleteTextures(1, &resource->fade_y_output_tex);
		check_error();
		glDeleteTextures(1, &resource->fade_cbcr_output_tex);
		check_error();
		glDeleteTextures(1, &resource->cb_tex);
		check_error();
		glDeleteTextures(1, &resource->cr_tex);
		check_error();
	}
	assert(interpolate_resources.size() == num_interpolate_slots);
}

void VideoStream::start()
{
	if (avctx == nullptr) {
		avctx = avformat_alloc_context();

		// We use Matroska, because it's pretty much the only mux where FFmpeg
		// allows writing chroma location to override JFIF's default center placement.
		// (Note that at the time of writing, however, FFmpeg does not correctly
		// _read_ this information!)
		avctx->oformat = av_guess_format("matroska", nullptr, nullptr);

		uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
		avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, nullptr, nullptr);
		avctx->pb->write_data_type = &VideoStream::write_packet2_thunk;
		avctx->pb->ignore_boundary_point = 1;

		avctx->flags = AVFMT_FLAG_CUSTOM_IO;
	}

	AVCodecParameters *audio_codecpar = avcodec_parameters_alloc();

	audio_codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
	audio_codecpar->codec_id = AV_CODEC_ID_PCM_S32LE;
	audio_codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
	audio_codecpar->channels = 2;
	audio_codecpar->sample_rate = OUTPUT_FREQUENCY;

	size_t width = global_flags.width, height = global_flags.height;  // Doesn't matter for MJPEG.
	mux.reset(new Mux(avctx, width, height, Mux::CODEC_MJPEG, /*video_extradata=*/"", audio_codecpar,
	                  AVCOL_SPC_BT709, COARSE_TIMEBASE, /*write_callback=*/nullptr, Mux::WRITE_FOREGROUND, {}, with_subtitles));

	avcodec_parameters_free(&audio_codecpar);
	encode_thread = thread(&VideoStream::encode_thread_func, this);
}

void VideoStream::stop()
{
	should_quit = true;
	queue_changed.notify_all();
	clear_queue();
	encode_thread.join();
}

void VideoStream::clear_queue()
{
	deque<QueuedFrame> q;

	{
		lock_guard<mutex> lock(queue_lock);
		q = move(frame_queue);
	}

	// These are not RAII-ed, unfortunately, so we'll need to clean them ourselves.
	// Note that release_texture() is thread-safe.
	for (const QueuedFrame &qf : q) {
		if (qf.type == QueuedFrame::INTERPOLATED ||
		    qf.type == QueuedFrame::FADED_INTERPOLATED) {
			if (qf.flow_tex != 0) {
				compute_flow->release_texture(qf.flow_tex);
			}
		}
		if (qf.type == QueuedFrame::INTERPOLATED) {
			interpolate->release_texture(qf.output_tex);
			interpolate->release_texture(qf.cbcr_tex);
		}
	}

	// Destroy q outside the mutex, as that would be a double-lock.
}

void VideoStream::schedule_original_frame(steady_clock::time_point local_pts,
                                          int64_t output_pts, function<void()> &&display_func,
                                          QueueSpotHolder &&queue_spot_holder,
                                          FrameOnDisk frame, const string &subtitle, bool include_audio)
{
	fprintf(stderr, "output_pts=%" PRId64 "  original      input_pts=%" PRId64 "\n", output_pts, frame.pts);

	QueuedFrame qf;
	qf.local_pts = local_pts;
	qf.type = QueuedFrame::ORIGINAL;
	qf.output_pts = output_pts;
	qf.display_func = move(display_func);
	qf.queue_spot_holder = move(queue_spot_holder);
	qf.subtitle = subtitle;
	FrameReader::Frame read_frame = frame_reader.read_frame(frame, /*read_video=*/true, include_audio);
	qf.encoded_jpeg.reset(new string(move(read_frame.video)));
	qf.audio = move(read_frame.audio);

	lock_guard<mutex> lock(queue_lock);
	frame_queue.push_back(move(qf));
	queue_changed.notify_all();
}

void VideoStream::schedule_faded_frame(steady_clock::time_point local_pts, int64_t output_pts,
                                       function<void()> &&display_func,
                                       QueueSpotHolder &&queue_spot_holder,
                                       FrameOnDisk frame1_spec, FrameOnDisk frame2_spec,
                                       float fade_alpha, const string &subtitle)
{
	fprintf(stderr, "output_pts=%" PRId64 "  faded         input_pts=%" PRId64 ",%" PRId64 "  fade_alpha=%.2f\n", output_pts, frame1_spec.pts, frame2_spec.pts, fade_alpha);

	// Get the temporary OpenGL resources we need for doing the fade.
	// (We share these with interpolated frames, which is slightly
	// overkill, but there's no need to waste resources on keeping
	// separate pools around.)
	BorrowedInterpolatedFrameResources resources;
	{
		lock_guard<mutex> lock(queue_lock);
		if (interpolate_resources.empty()) {
			fprintf(stderr, "WARNING: Too many interpolated frames already in transit; dropping one.\n");
			return;
		}
		resources = BorrowedInterpolatedFrameResources(interpolate_resources.front().release());
		interpolate_resources.pop_front();
	}

	bool did_decode;

	shared_ptr<Frame> frame1 = decode_jpeg_with_cache(frame1_spec, DECODE_IF_NOT_IN_CACHE, &frame_reader, &did_decode);
	shared_ptr<Frame> frame2 = decode_jpeg_with_cache(frame2_spec, DECODE_IF_NOT_IN_CACHE, &frame_reader, &did_decode);
	wait_for_upload(frame1);
	wait_for_upload(frame2);

	ycbcr_semiplanar_converter->prepare_chain_for_fade(frame1, frame2, fade_alpha)->render_to_fbo(resources->fade_fbo, global_flags.width, global_flags.height);

	QueuedFrame qf;
	qf.local_pts = local_pts;
	qf.type = QueuedFrame::FADED;
	qf.output_pts = output_pts;
	qf.frame1 = frame1_spec;
	qf.display_func = move(display_func);
	qf.queue_spot_holder = move(queue_spot_holder);
	qf.subtitle = subtitle;

	qf.secondary_frame = frame2_spec;

	// Subsample and split Cb/Cr.
	chroma_subsampler->subsample_chroma(resources->fade_cbcr_output_tex, global_flags.width, global_flags.height, resources->cb_tex, resources->cr_tex);

	// Read it down (asynchronously) to the CPU.
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, resources->pbo);
	check_error();
	glGetTextureImage(resources->fade_y_output_tex, 0, GL_RED, GL_UNSIGNED_BYTE, global_flags.width * global_flags.height * 4, BUFFER_OFFSET(0));
	check_error();
	glGetTextureImage(resources->cb_tex, 0, GL_RED, GL_UNSIGNED_BYTE, global_flags.width * global_flags.height * 3, BUFFER_OFFSET(global_flags.width * global_flags.height));
	check_error();
	glGetTextureImage(resources->cr_tex, 0, GL_RED, GL_UNSIGNED_BYTE, global_flags.width * global_flags.height * 3 - (global_flags.width / 2) * global_flags.height, BUFFER_OFFSET(global_flags.width * global_flags.height + (global_flags.width / 2) * global_flags.height));
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	// Set a fence we can wait for to make sure the CPU sees the read.
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
	check_error();
	qf.fence_created = steady_clock::now();
	qf.fence = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
	check_error();
	qf.resources = move(resources);
	qf.local_pts = local_pts;

	lock_guard<mutex> lock(queue_lock);
	frame_queue.push_back(move(qf));
	queue_changed.notify_all();
}

void VideoStream::schedule_interpolated_frame(steady_clock::time_point local_pts,
                                              int64_t output_pts, function<void(shared_ptr<Frame>)> &&display_func,
                                              QueueSpotHolder &&queue_spot_holder,
                                              FrameOnDisk frame1, FrameOnDisk frame2,
                                              float alpha, FrameOnDisk secondary_frame, float fade_alpha, const string &subtitle,
                                              bool play_audio)
{
	if (secondary_frame.pts != -1) {
		fprintf(stderr, "output_pts=%" PRId64 "  interpolated  input_pts1=%" PRId64 " input_pts2=%" PRId64 " alpha=%.3f  secondary_pts=%" PRId64 "  fade_alpha=%.2f\n", output_pts, frame1.pts, frame2.pts, alpha, secondary_frame.pts, fade_alpha);
	} else {
		fprintf(stderr, "output_pts=%" PRId64 "  interpolated  input_pts1=%" PRId64 " input_pts2=%" PRId64 " alpha=%.3f\n", output_pts, frame1.pts, frame2.pts, alpha);
	}

	// Get the temporary OpenGL resources we need for doing the interpolation.
	BorrowedInterpolatedFrameResources resources;
	{
		lock_guard<mutex> lock(queue_lock);
		if (interpolate_resources.empty()) {
			fprintf(stderr, "WARNING: Too many interpolated frames already in transit; dropping one.\n");
			return;
		}
		resources = BorrowedInterpolatedFrameResources(interpolate_resources.front().release());
		interpolate_resources.pop_front();
	}

	QueuedFrame qf;
	qf.type = (secondary_frame.pts == -1) ? QueuedFrame::INTERPOLATED : QueuedFrame::FADED_INTERPOLATED;
	qf.output_pts = output_pts;
	qf.display_decoded_func = move(display_func);
	qf.queue_spot_holder = move(queue_spot_holder);
	qf.local_pts = local_pts;
	qf.subtitle = subtitle;

	if (play_audio) {
		qf.audio = frame_reader.read_frame(frame1, /*read_video=*/false, /*read_audio=*/true).audio;
	}

	check_error();

	// Convert frame0 and frame1 to OpenGL textures.
	for (size_t frame_no = 0; frame_no < 2; ++frame_no) {
		FrameOnDisk frame_spec = frame_no == 1 ? frame2 : frame1;
		bool did_decode;
		shared_ptr<Frame> frame = decode_jpeg_with_cache(frame_spec, DECODE_IF_NOT_IN_CACHE, &frame_reader, &did_decode);
		wait_for_upload(frame);
		ycbcr_converter->prepare_chain_for_conversion(frame)->render_to_fbo(resources->input_fbos[frame_no], global_flags.width, global_flags.height);
		if (frame_no == 1) {
			qf.exif_data = frame->exif_data;  // Use the white point from the last frame.
		}
	}

	glGenerateTextureMipmap(resources->input_tex);
	check_error();
	glGenerateTextureMipmap(resources->gray_tex);
	check_error();

	GLuint flow_tex;
	if (last_flow_tex != 0 && frame1 == last_frame1 && frame2 == last_frame2) {
		// Reuse the flow from previous computation. This frequently happens
		// if we slow down by more than 2x, so that there are multiple interpolated
		// frames between each original.
		flow_tex = last_flow_tex;
		qf.flow_tex = 0;
	} else {
		// Cache miss, so release last_flow_tex.
		qf.flow_tex = last_flow_tex;

		// Compute the flow.
		flow_tex = compute_flow->exec(resources->gray_tex, DISComputeFlow::FORWARD_AND_BACKWARD, DISComputeFlow::DO_NOT_RESIZE_FLOW);
		check_error();

		// Store the flow texture for possible reuse next frame.
		last_flow_tex = flow_tex;
		last_frame1 = frame1;
		last_frame2 = frame2;
	}

	if (secondary_frame.pts != -1) {
		// Fade. First kick off the interpolation.
		tie(qf.output_tex, ignore) = interpolate_no_split->exec(resources->input_tex, resources->gray_tex, flow_tex, global_flags.width, global_flags.height, alpha);
		check_error();

		// Now decode the image we are fading against.
		bool did_decode;
		shared_ptr<Frame> frame2 = decode_jpeg_with_cache(secondary_frame, DECODE_IF_NOT_IN_CACHE, &frame_reader, &did_decode);
		wait_for_upload(frame2);

		// Then fade against it, putting it into the fade Y' and CbCr textures.
		RGBTriplet neutral_color = get_neutral_color(qf.exif_data);
		ycbcr_semiplanar_converter->prepare_chain_for_fade_from_texture(qf.output_tex, neutral_color, global_flags.width, global_flags.height, frame2, fade_alpha)->render_to_fbo(resources->fade_fbo, global_flags.width, global_flags.height);

		// Subsample and split Cb/Cr.
		chroma_subsampler->subsample_chroma(resources->fade_cbcr_output_tex, global_flags.width, global_flags.height, resources->cb_tex, resources->cr_tex);

		interpolate_no_split->release_texture(qf.output_tex);

		// We already applied the white balance, so don't have the client redo it.
		qf.exif_data.clear();
	} else {
		tie(qf.output_tex, qf.cbcr_tex) = interpolate->exec(resources->input_tex, resources->gray_tex, flow_tex, global_flags.width, global_flags.height, alpha);
		check_error();

		// Subsample and split Cb/Cr.
		chroma_subsampler->subsample_chroma(qf.cbcr_tex, global_flags.width, global_flags.height, resources->cb_tex, resources->cr_tex);
	}

	// We could have released qf.flow_tex here, but to make sure we don't cause a stall
	// when trying to reuse it for the next frame, we can just as well hold on to it
	// and release it only when the readback is done.
	//
	// TODO: This is maybe less relevant now that qf.flow_tex contains the texture we used
	// _last_ frame, not this one.

	// Read it down (asynchronously) to the CPU.
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, resources->pbo);
	check_error();
	if (secondary_frame.pts != -1) {
		glGetTextureImage(resources->fade_y_output_tex, 0, GL_RED, GL_UNSIGNED_BYTE, global_flags.width * global_flags.height * 4, BUFFER_OFFSET(0));
	} else {
		glGetTextureImage(qf.output_tex, 0, GL_RED, GL_UNSIGNED_BYTE, global_flags.width * global_flags.height * 4, BUFFER_OFFSET(0));
	}
	check_error();
	glGetTextureImage(resources->cb_tex, 0, GL_RED, GL_UNSIGNED_BYTE, global_flags.width * global_flags.height * 3, BUFFER_OFFSET(global_flags.width * global_flags.height));
	check_error();
	glGetTextureImage(resources->cr_tex, 0, GL_RED, GL_UNSIGNED_BYTE, global_flags.width * global_flags.height * 3 - (global_flags.width / 2) * global_flags.height, BUFFER_OFFSET(global_flags.width * global_flags.height + (global_flags.width / 2) * global_flags.height));
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	// Set a fence we can wait for to make sure the CPU sees the read.
	glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
	check_error();
	qf.fence_created = steady_clock::now();
	qf.fence = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
	check_error();
	qf.resources = move(resources);

	lock_guard<mutex> lock(queue_lock);
	frame_queue.push_back(move(qf));
	queue_changed.notify_all();
}

void VideoStream::schedule_refresh_frame(steady_clock::time_point local_pts,
                                         int64_t output_pts, function<void()> &&display_func,
                                         QueueSpotHolder &&queue_spot_holder, const string &subtitle)
{
	QueuedFrame qf;
	qf.type = QueuedFrame::REFRESH;
	qf.output_pts = output_pts;
	qf.display_func = move(display_func);
	qf.queue_spot_holder = move(queue_spot_holder);
	qf.subtitle = subtitle;

	lock_guard<mutex> lock(queue_lock);
	frame_queue.push_back(move(qf));
	queue_changed.notify_all();
}

void VideoStream::schedule_silence(steady_clock::time_point local_pts, int64_t output_pts,
                                   int64_t length_pts, QueueSpotHolder &&queue_spot_holder)
{
	QueuedFrame qf;
	qf.type = QueuedFrame::SILENCE;
	qf.output_pts = output_pts;
	qf.queue_spot_holder = move(queue_spot_holder);
	qf.silence_length_pts = length_pts;

	lock_guard<mutex> lock(queue_lock);
	frame_queue.push_back(move(qf));
	queue_changed.notify_all();
}

namespace {

RefCountedTexture clone_r8_texture(GLuint src_tex, unsigned width, unsigned height)
{
	GLuint tex;
	glCreateTextures(GL_TEXTURE_2D, 1, &tex);
	check_error();
	glTextureStorage2D(tex, 1, GL_R8, width, height);
	check_error();
	glCopyImageSubData(src_tex, GL_TEXTURE_2D, 0, 0, 0, 0,
	                   tex, GL_TEXTURE_2D, 0, 0, 0, 0,
	                   width, height, 1);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	return RefCountedTexture(new GLuint(tex), TextureDeleter());
}

}  // namespace

void VideoStream::encode_thread_func()
{
	pthread_setname_np(pthread_self(), "VideoStream");
	QSurface *surface = create_surface();
	QOpenGLContext *context = create_context(surface);
	bool ok = make_current(context, surface);
	if (!ok) {
		fprintf(stderr, "Video stream couldn't get an OpenGL context\n");
		abort();
	}

	init_pbo_pool();

	while (!should_quit) {
		QueuedFrame qf;
		{
			unique_lock<mutex> lock(queue_lock);

			// Wait until we have a frame to play.
			queue_changed.wait(lock, [this] {
				return !frame_queue.empty() || should_quit;
			});
			if (should_quit) {
				break;
			}
			steady_clock::time_point frame_start = frame_queue.front().local_pts;

			// Now sleep until the frame is supposed to start (the usual case),
			// _or_ clear_queue() happened.
			bool aborted;
			if (output_fast_forward) {
				aborted = frame_queue.empty() || frame_queue.front().local_pts != frame_start;
			} else {
				aborted = queue_changed.wait_until(lock, frame_start, [this, frame_start] {
					return frame_queue.empty() || frame_queue.front().local_pts != frame_start;
				});
			}
			if (aborted) {
				// clear_queue() happened, so don't play this frame after all.
				continue;
			}
			qf = move(frame_queue.front());
			frame_queue.pop_front();
		}

		// Hack: We mux the subtitle packet one time unit before the actual frame,
		// so that Nageru is sure to get it first.
		if (!qf.subtitle.empty() && with_subtitles == Mux::WITH_SUBTITLES) {
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = mux->get_subtitle_stream_idx();
			assert(pkt.stream_index != -1);
			pkt.data = (uint8_t *)qf.subtitle.data();
			pkt.size = qf.subtitle.size();
			pkt.flags = 0;
			pkt.duration = lrint(TIMEBASE / global_flags.output_framerate);  // Doesn't really matter for Nageru.
			mux->add_packet(pkt, qf.output_pts - 1, qf.output_pts - 1);
		}

		if (qf.type == QueuedFrame::ORIGINAL) {
			// Send the JPEG frame on, unchanged.
			string jpeg = move(*qf.encoded_jpeg);
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)jpeg.data();
			pkt.size = jpeg.size();
			pkt.flags = AV_PKT_FLAG_KEY;
			mux->add_packet(pkt, qf.output_pts, qf.output_pts);
			last_frame = move(jpeg);

			add_audio_or_silence(qf);
		} else if (qf.type == QueuedFrame::FADED) {
			steady_clock::time_point start = steady_clock::now();
			glClientWaitSync(qf.fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
			steady_clock::time_point stop = steady_clock::now();
			metric_fade_fence_wait_time_seconds.count_event(duration<double>(stop - start).count());
			metric_fade_latency_seconds.count_event(duration<double>(stop - qf.fence_created).count());

			// Now JPEG encode it, and send it on to the stream.
			string jpeg = encode_jpeg_from_pbo(qf.resources->pbo_contents, global_flags.width, global_flags.height, /*exif_data=*/"");

			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)jpeg.data();
			pkt.size = jpeg.size();
			pkt.flags = AV_PKT_FLAG_KEY;
			mux->add_packet(pkt, qf.output_pts, qf.output_pts);
			last_frame = move(jpeg);

			add_audio_or_silence(qf);
		} else if (qf.type == QueuedFrame::INTERPOLATED || qf.type == QueuedFrame::FADED_INTERPOLATED) {
			steady_clock::time_point start = steady_clock::now();
			glClientWaitSync(qf.fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
			steady_clock::time_point stop = steady_clock::now();
			metric_interpolation_fence_wait_time_seconds.count_event(duration<double>(stop - start).count());
			metric_interpolation_latency_seconds.count_event(duration<double>(stop - qf.fence_created).count());

			// Send it on to display.
			if (qf.display_decoded_func != nullptr) {
				shared_ptr<Frame> frame(new Frame);
				if (qf.type == QueuedFrame::FADED_INTERPOLATED) {
					frame->y = clone_r8_texture(qf.resources->fade_y_output_tex, global_flags.width, global_flags.height);
				} else {
					frame->y = clone_r8_texture(qf.output_tex, global_flags.width, global_flags.height);
				}
				frame->cb = clone_r8_texture(qf.resources->cb_tex, global_flags.width / 2, global_flags.height);
				frame->cr = clone_r8_texture(qf.resources->cr_tex, global_flags.width / 2, global_flags.height);
				frame->width = global_flags.width;
				frame->height = global_flags.height;
				frame->chroma_subsampling_x = 2;
				frame->chroma_subsampling_y = 1;
				frame->uploaded_ui_thread = RefCountedGLsync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
				qf.display_decoded_func(move(frame));
			}

			// Now JPEG encode it, and send it on to the stream.
			string jpeg = encode_jpeg_from_pbo(qf.resources->pbo_contents, global_flags.width, global_flags.height, move(qf.exif_data));
			if (qf.flow_tex != 0) {
				compute_flow->release_texture(qf.flow_tex);
			}
			if (qf.type != QueuedFrame::FADED_INTERPOLATED) {
				interpolate->release_texture(qf.output_tex);
				interpolate->release_texture(qf.cbcr_tex);
			}

			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)jpeg.data();
			pkt.size = jpeg.size();
			pkt.flags = AV_PKT_FLAG_KEY;
			mux->add_packet(pkt, qf.output_pts, qf.output_pts);
			last_frame = move(jpeg);

			add_audio_or_silence(qf);
		} else if (qf.type == QueuedFrame::REFRESH) {
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.stream_index = 0;
			pkt.data = (uint8_t *)last_frame.data();
			pkt.size = last_frame.size();
			pkt.flags = AV_PKT_FLAG_KEY;
			mux->add_packet(pkt, qf.output_pts, qf.output_pts);

			add_audio_or_silence(qf);  // Definitely silence.
		} else if (qf.type == QueuedFrame::SILENCE) {
			add_silence(qf.output_pts, qf.silence_length_pts);
		} else {
			assert(false);
		}
		if (qf.display_func != nullptr) {
			qf.display_func();
		}
	}
}

int VideoStream::write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	VideoStream *video_stream = (VideoStream *)opaque;
	return video_stream->write_packet2(buf, buf_size, type, time);
}

int VideoStream::write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
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
		global_httpd->set_header(stream_id, stream_mux_header);
	} else {
		global_httpd->add_data(stream_id, (char *)buf, buf_size, type == AVIO_DATA_MARKER_SYNC_POINT, time, AVRational{ AV_TIME_BASE, 1 });
	}
	return buf_size;
}

void VideoStream::add_silence(int64_t pts, int64_t length_pts)
{
	// At 59.94, this will never quite add up (even discounting refresh frames,
	// which have unpredictable length), but hopefully, the player in the other
	// end should be able to stretch silence easily enough.
	long num_samples = lrint(length_pts * double(OUTPUT_FREQUENCY) / double(TIMEBASE)) * 2;
	uint8_t *zero = (uint8_t *)calloc(num_samples, sizeof(int32_t));

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.stream_index = 1;
	pkt.data = zero;
	pkt.size = num_samples * sizeof(int32_t);
	pkt.flags = AV_PKT_FLAG_KEY;
	mux->add_packet(pkt, pts, pts);

	free(zero);
}

void VideoStream::add_audio_or_silence(const QueuedFrame &qf)
{
	if (qf.audio.empty()) {
		int64_t frame_length = lrint(double(TIMEBASE) / global_flags.output_framerate);
		add_silence(qf.output_pts, frame_length);
	} else {
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.stream_index = 1;
		pkt.data = (uint8_t *)qf.audio.data();
		pkt.size = qf.audio.size();
		pkt.flags = AV_PKT_FLAG_KEY;
		mux->add_packet(pkt, qf.output_pts, qf.output_pts);
	}
}
