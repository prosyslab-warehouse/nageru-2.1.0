#undef Success

#include "mixer.h"

#include <assert.h>
#include <epoxy/egl.h>
#include <movit/effect.h>
#include <movit/effect_chain.h>
#include <movit/effect_util.h>
#include <movit/flat_input.h>
#include <movit/image_format.h>
#include <movit/init.h>
#include <movit/resource_pool.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ratio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DeckLinkAPI.h"
#include "LinuxCOM.h"
#include "alsa_output.h"
#include "basic_stats.h"
#include "bmusb/bmusb.h"
#include "bmusb/fake_capture.h"
#ifdef HAVE_CEF
#include "cef_capture.h"
#endif
#include "chroma_subsampler.h"
#include "shared/context.h"
#include "decklink_capture.h"
#include "decklink_output.h"
#include "decklink_util.h"
#include "defs.h"
#include "shared/disk_space_estimator.h"
#include "ffmpeg_capture.h"
#include "flags.h"
#include "image_input.h"
#include "input_mapping.h"
#include "shared/metrics.h"
#include "shared/va_display.h"
#include "mjpeg_encoder.h"
#include "pbo_frame_allocator.h"
#include "shared/ref_counted_gl_sync.h"
#include "resampling_queue.h"
#include "shared/timebase.h"
#include "timecode_renderer.h"
#include "v210_converter.h"
#include "video_encoder.h"

#undef Status
#include <google/protobuf/util/json_util.h>
#include "json.pb.h"

#ifdef HAVE_SRT
// Must come after CEF, since it includes <syslog.h>, which has #defines
// that conflict with CEF logging constants.
#include <srt/srt.h>
#endif

class IDeckLink;
class QOpenGLContext;

using namespace movit;
using namespace std;
using namespace std::chrono;
using namespace std::placeholders;
using namespace bmusb;

Mixer *global_mixer = nullptr;

namespace {

void insert_new_frame(RefCountedFrame frame, unsigned field_num, bool interlaced, unsigned card_index, InputState *input_state)
{
	if (interlaced) {
		for (unsigned frame_num = FRAME_HISTORY_LENGTH; frame_num --> 1; ) {  // :-)
			input_state->buffered_frames[card_index][frame_num] =
				input_state->buffered_frames[card_index][frame_num - 1];
		}
		input_state->buffered_frames[card_index][0] = { frame, field_num };
	} else {
		for (unsigned frame_num = 0; frame_num < FRAME_HISTORY_LENGTH; ++frame_num) {
			input_state->buffered_frames[card_index][frame_num] = { frame, field_num };
		}
	}
}

void ensure_texture_resolution(PBOFrameAllocator::Userdata *userdata, unsigned field, unsigned width, unsigned height, unsigned cbcr_width, unsigned cbcr_height, unsigned v210_width)
{
	bool first;
	switch (userdata->pixel_format) {
	case PixelFormat_10BitYCbCr:
		first = userdata->tex_v210[field] == 0 || userdata->tex_444[field] == 0;
		break;
	case PixelFormat_8BitYCbCr:
		first = userdata->tex_y[field] == 0 || userdata->tex_cbcr[field] == 0;
		break;
	case PixelFormat_8BitBGRA:
		first = userdata->tex_rgba[field] == 0;
		break;
	case PixelFormat_8BitYCbCrPlanar:
		first = userdata->tex_y[field] == 0 || userdata->tex_cb[field] == 0 || userdata->tex_cr[field] == 0;
		break;
	default:
		assert(false);
	}

	const bool recreate_main_texture =
		first ||
		width != userdata->last_width[field] ||
		height != userdata->last_height[field] ||
		cbcr_width != userdata->last_cbcr_width[field] ||
		cbcr_height != userdata->last_cbcr_height[field];
	const bool recreate_v210_texture =
		global_flags.ten_bit_input &&
		(first || v210_width != userdata->last_v210_width[field] || height != userdata->last_height[field]);

	if (recreate_main_texture) {
		// We changed resolution since last use of this texture, so we need to create
		// a new object. Note that this each card has its own PBOFrameAllocator,
		// we don't need to worry about these flip-flopping between resolutions.
		switch (userdata->pixel_format) {
		case PixelFormat_10BitYCbCr:
			glBindTexture(GL_TEXTURE_2D, userdata->tex_444[field]);
			check_error();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
			check_error();
			break;
		case PixelFormat_8BitYCbCr: {
			glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr[field]);
			check_error();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, cbcr_width, height, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
			check_error();
			glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
			check_error();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
			check_error();
			break;
		}
		case PixelFormat_8BitYCbCrPlanar: {
			glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
			check_error();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
			check_error();
			glBindTexture(GL_TEXTURE_2D, userdata->tex_cb[field]);
			check_error();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, cbcr_width, cbcr_height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
			check_error();
			glBindTexture(GL_TEXTURE_2D, userdata->tex_cr[field]);
			check_error();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, cbcr_width, cbcr_height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
			check_error();
			break;
		}
		case PixelFormat_8BitBGRA:
			glBindTexture(GL_TEXTURE_2D, userdata->tex_rgba[field]);
			check_error();
			// NOTE: sRGB may be disabled by sRGBSwitchingFlatInput.
			glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
			check_error();
			break;
		default:
			assert(false);
		}
		userdata->last_width[field] = width;
		userdata->last_height[field] = height;
		userdata->last_cbcr_width[field] = cbcr_width;
		userdata->last_cbcr_height[field] = cbcr_height;
	}
	if (recreate_v210_texture) {
		// Same as above; we need to recreate the texture.
		glBindTexture(GL_TEXTURE_2D, userdata->tex_v210[field]);
		check_error();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, v210_width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
		check_error();
		userdata->last_v210_width[field] = v210_width;
		userdata->last_height[field] = height;
	}
}

void upload_texture(GLuint tex, GLuint width, GLuint height, GLuint stride, bool interlaced_stride, GLenum format, GLenum type, GLintptr offset)
{
	if (interlaced_stride) {
		stride *= 2;
	}
	if (global_flags.flush_pbos) {
		glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, offset, stride * height);
		check_error();
	}

	glBindTexture(GL_TEXTURE_2D, tex);
	check_error();
	if (interlaced_stride) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, width * 2);
		check_error();
	} else {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		check_error();
	}

	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, BUFFER_OFFSET(offset));
	check_error();
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	check_error();
}

}  // namespace

void JitterHistory::register_metrics(const vector<pair<string, string>> &labels)
{
	global_metrics.add("input_underestimated_jitter_frames", labels, &metric_input_underestimated_jitter_frames);
	global_metrics.add("input_estimated_max_jitter_seconds", labels, &metric_input_estimated_max_jitter_seconds, Metrics::TYPE_GAUGE);
}

void JitterHistory::unregister_metrics(const vector<pair<string, string>> &labels)
{
	global_metrics.remove("input_underestimated_jitter_frames", labels);
	global_metrics.remove("input_estimated_max_jitter_seconds", labels);
}

void JitterHistory::frame_arrived(steady_clock::time_point now, int64_t frame_duration, size_t dropped_frames)
{
	if (frame_duration != last_duration) {
		// If the frame rate changed, the input clock is also going to change,
		// so our historical data doesn't make much sense anymore.
		// Also, format changes typically introduce blips that are not representative
		// of the typical frame stream. (We make the assumption that format changes
		// don't happen all the time in regular use; if they did, we should probably
		// rather keep the history so that we take jitter they may introduce into account.)
		clear();
		last_duration = frame_duration;
	}
	if (expected_timestamp > steady_clock::time_point::min()) {
		expected_timestamp += dropped_frames * nanoseconds(frame_duration * 1000000000 / TIMEBASE);
		double jitter_seconds = fabs(duration<double>(expected_timestamp - now).count());
		history.push_back(orders.insert(jitter_seconds));
		if (jitter_seconds > estimate_max_jitter()) {
			++metric_input_underestimated_jitter_frames;
		}

		metric_input_estimated_max_jitter_seconds = estimate_max_jitter();

		if (history.size() > history_length) {
			orders.erase(history.front());
			history.pop_front();
		}
		assert(history.size() <= history_length);
	}
	expected_timestamp = now + nanoseconds(frame_duration * 1000000000 / TIMEBASE);
}

double JitterHistory::estimate_max_jitter() const
{
	if (orders.empty()) {
		return 0.0;
	}
	size_t elem_idx = lrint((orders.size() - 1) * percentile);
	if (percentile <= 0.5) {
		return *next(orders.begin(), elem_idx) * multiplier;
	} else {
		return *prev(orders.end(), orders.size() - elem_idx) * multiplier;
	}
}

void QueueLengthPolicy::register_metrics(const vector<pair<string, string>> &labels)
{
	global_metrics.add("input_queue_safe_length_frames", labels, &metric_input_queue_safe_length_frames, Metrics::TYPE_GAUGE);
}

void QueueLengthPolicy::unregister_metrics(const vector<pair<string, string>> &labels)
{
	global_metrics.remove("input_queue_safe_length_frames", labels);
}

void QueueLengthPolicy::update_policy(steady_clock::time_point now,
                                      steady_clock::time_point expected_next_input_frame,
                                      int64_t input_frame_duration,
                                      int64_t master_frame_duration,
                                      double max_input_card_jitter_seconds,
                                      double max_master_card_jitter_seconds)
{
	double input_frame_duration_seconds = input_frame_duration / double(TIMEBASE);
	double master_frame_duration_seconds = master_frame_duration / double(TIMEBASE);

	// Figure out when we can expect the next frame for this card, assuming
	// worst-case jitter (ie., the frame is maximally late).
	double seconds_until_next_frame = max(duration<double>(expected_next_input_frame - now).count() + max_input_card_jitter_seconds, 0.0);

	// How many times are the master card expected to tick in that time?
	// We assume the master clock has worst-case jitter but not any rate
	// discrepancy, ie., it ticks as early as possible every time, but not
	// cumulatively.
	double frames_needed = (seconds_until_next_frame + max_master_card_jitter_seconds) / master_frame_duration_seconds;

	// As a special case, if the master card ticks faster than the input card,
	// we expect the queue to drain by itself even without dropping. But if
	// the difference is small (e.g. 60 Hz master and 59.94 input), it would
	// go slowly enough that the effect wouldn't really be appreciable.
	// We account for this by looking at the situation five frames ahead,
	// assuming everything else is the same.
	double frames_allowed;
	if (master_frame_duration < input_frame_duration) {
		frames_allowed = frames_needed + 5 * (input_frame_duration_seconds - master_frame_duration_seconds) / master_frame_duration_seconds;
	} else {
		frames_allowed = frames_needed;
	}

	safe_queue_length = max<int>(floor(frames_allowed), 0);
	metric_input_queue_safe_length_frames = safe_queue_length;
}

Mixer::Mixer(const QSurfaceFormat &format)
	: httpd(),
	  mixer_surface(create_surface(format)),
	  h264_encoder_surface(create_surface(format)),
	  decklink_output_surface(create_surface(format)),
	  image_update_surface(create_surface(format))
{
	memcpy(ycbcr_interpretation, global_flags.ycbcr_interpretation, sizeof(ycbcr_interpretation));
	CHECK(init_movit(MOVIT_SHADER_DIR, MOVIT_DEBUG_OFF));
	check_error();

	if (!epoxy_has_gl_extension("GL_EXT_texture_sRGB_decode") ||
	    !epoxy_has_gl_extension("GL_ARB_sampler_objects")) {
		fprintf(stderr, "Nageru requires GL_EXT_texture_sRGB_decode and GL_ARB_sampler_objects to run.\n");
		exit(1);
	}

	// Since we allow non-bouncing 4:2:2 YCbCrInputs, effective subpixel precision
	// will be halved when sampling them, and we need to compensate here.
	movit_texel_subpixel_precision /= 2.0;

	resource_pool.reset(new ResourcePool);
	for (unsigned i = 0; i < NUM_OUTPUTS; ++i) {
		output_channel[i].parent = this;
		output_channel[i].channel = i;
	}

	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	// Matches the 4:2:0 format created by the main chain.
	YCbCrFormat ycbcr_format;
	ycbcr_format.chroma_subsampling_x = 2;
	ycbcr_format.chroma_subsampling_y = 2;
	if (global_flags.ycbcr_rec709_coefficients) {
		ycbcr_format.luma_coefficients = YCBCR_REC_709;
	} else {
		ycbcr_format.luma_coefficients = YCBCR_REC_601;
	}
	ycbcr_format.full_range = false;
	ycbcr_format.num_levels = 1 << global_flags.x264_bit_depth;
	ycbcr_format.cb_x_position = 0.0f;
	ycbcr_format.cr_x_position = 0.0f;
	ycbcr_format.cb_y_position = 0.5f;
	ycbcr_format.cr_y_position = 0.5f;

	// Initialize the neutral colors to sane values.
	for (unsigned i = 0; i < MAX_VIDEO_CARDS; ++i) {
		last_received_neutral_color[i] = RGBTriplet(1.0f, 1.0f, 1.0f);
	}

	// Display chain; shows the live output produced by the main chain (or rather, a copy of it).
	display_chain.reset(new EffectChain(global_flags.width, global_flags.height, resource_pool.get()));
	check_error();
	GLenum type = global_flags.x264_bit_depth > 8 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
	display_input = new YCbCrInput(inout_format, ycbcr_format, global_flags.width, global_flags.height, YCBCR_INPUT_SPLIT_Y_AND_CBCR, type);
	display_chain->add_input(display_input);
	display_chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
	display_chain->set_dither_bits(0);  // Don't bother.
	display_chain->finalize();

	video_encoder.reset(new VideoEncoder(resource_pool.get(), h264_encoder_surface, global_flags.va_display, global_flags.width, global_flags.height, &httpd, global_disk_space_estimator));
	if (!global_flags.card_to_mjpeg_stream_export.empty()) {
		mjpeg_encoder.reset(new MJPEGEncoder(&httpd, global_flags.va_display));
	}

	// Must be instantiated after VideoEncoder has initialized global_flags.use_zerocopy.
	theme.reset(new Theme(global_flags.theme_filename, global_flags.theme_dirs, resource_pool.get()));

	// Must be instantiated after the theme, as the theme decides the number of FFmpeg inputs.
	std::vector<FFmpegCapture *> video_inputs = theme->get_video_inputs();
	audio_mixer.reset(new AudioMixer);

	httpd.add_endpoint("/channels", bind(&Mixer::get_channels_json, this), HTTPD::ALLOW_ALL_ORIGINS);
	for (int channel_idx = 0; channel_idx < theme->get_num_channels(); ++channel_idx) {
		char url[256];
		snprintf(url, sizeof(url), "/channels/%d/color", channel_idx + 2);
		httpd.add_endpoint(url, bind(&Mixer::get_channel_color_http, this, unsigned(channel_idx + 2)), HTTPD::ALLOW_ALL_ORIGINS);
	}

	// Start listening for clients only once VideoEncoder has written its header, if any.
	httpd.start(global_flags.http_port);

	// First try initializing the then PCI devices, then USB, then
	// fill up with fake cards until we have the desired number of cards.
	unsigned num_pci_devices = 0;
	unsigned card_index = 0;

	{
		IDeckLinkIterator *decklink_iterator = CreateDeckLinkIteratorInstance();
		if (decklink_iterator != nullptr) {
			for ( ; card_index < unsigned(global_flags.max_num_cards); ++card_index) {
				IDeckLink *decklink;
				if (decklink_iterator->Next(&decklink) != S_OK) {
					break;
				}

				if (!decklink_card_is_active(decklink, card_index)) {
					fprintf(stderr, "DeckLink card %u is inactive in current profile, skipping (try changing it in Desktop Video Setup)\n", card_index);
					decklink->Release();
					continue;
				}

				DeckLinkCapture *capture = new DeckLinkCapture(decklink, card_index);
				DeckLinkOutput *output = new DeckLinkOutput(resource_pool.get(), decklink_output_surface, global_flags.width, global_flags.height, card_index);
				if (!output->set_device(decklink, capture->get_input())) {
					delete output;
					output = nullptr;
				}
				configure_card(card_index, capture, CardType::LIVE_CARD, output, /*is_srt_card=*/false);
				++num_pci_devices;
			}
			decklink_iterator->Release();
			fprintf(stderr, "Found %u DeckLink PCI card(s).\n", num_pci_devices);
		} else {
			fprintf(stderr, "DeckLink drivers not found. Probing for USB cards only.\n");
		}
	}

	unsigned num_usb_devices = BMUSBCapture::num_cards();
	for (unsigned usb_card_index = 0; usb_card_index < num_usb_devices && card_index < unsigned(global_flags.max_num_cards); ++usb_card_index, ++card_index) {
		BMUSBCapture *capture = new BMUSBCapture(usb_card_index);
		capture->set_card_disconnected_callback(bind(&Mixer::bm_hotplug_remove, this, card_index));
		configure_card(card_index, capture, CardType::LIVE_CARD, /*output=*/nullptr, /*is_srt_card=*/false);
	}
	fprintf(stderr, "Found %u USB card(s).\n", num_usb_devices);

	// Fill up with fake cards for as long as we can, so that the FFmpeg
	// and HTML cards always come last.
	unsigned num_fake_cards = 0;
#ifdef HAVE_CEF
	size_t num_html_inputs = theme->get_html_inputs().size();
#else
	size_t num_html_inputs = 0;
#endif
	for ( ; card_index < MAX_VIDEO_CARDS - video_inputs.size() - num_html_inputs; ++card_index) {
		// Only bother to activate fake capture cards to satisfy the minimum.
		bool is_active = card_index < unsigned(global_flags.min_num_cards) || cards[card_index].force_active;
		if (is_active) {
			FakeCapture *capture = new FakeCapture(global_flags.width, global_flags.height, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
			configure_card(card_index, capture, CardType::FAKE_CAPTURE, /*output=*/nullptr, /*is_srt_card=*/false);
			++num_fake_cards;
		} else {
			configure_card(card_index, nullptr, CardType::FAKE_CAPTURE, /*output=*/nullptr, /*is_srt_card=*/false);
		}
	}

	if (num_fake_cards > 0) {
		fprintf(stderr, "Initialized %u fake cards.\n", num_fake_cards);
	}

	// Initialize all video inputs the theme asked for.
	for (unsigned video_card_index = 0; video_card_index < video_inputs.size(); ++card_index, ++video_card_index) {
		if (card_index >= MAX_VIDEO_CARDS) {
			fprintf(stderr, "ERROR: Not enough card slots available for the videos the theme requested.\n");
			abort();
		}
		configure_card(card_index, video_inputs[video_card_index], CardType::FFMPEG_INPUT, /*output=*/nullptr, /*is_srt_card=*/false);
		video_inputs[video_card_index]->set_card_index(card_index);
	}
	num_video_inputs = video_inputs.size();

#ifdef HAVE_CEF
	// Same, for HTML inputs.
	std::vector<CEFCapture *> html_inputs = theme->get_html_inputs();
	for (unsigned html_card_index = 0; html_card_index < html_inputs.size(); ++card_index, ++html_card_index) {
		if (card_index >= MAX_VIDEO_CARDS) {
			fprintf(stderr, "ERROR: Not enough card slots available for the HTML inputs the theme requested.\n");
			abort();
		}
		configure_card(card_index, html_inputs[html_card_index], CardType::CEF_INPUT, /*output=*/nullptr, /*is_srt_card=*/false);
		html_inputs[html_card_index]->set_card_index(card_index);
	}
	num_html_inputs = html_inputs.size();
#endif

	BMUSBCapture::set_card_connected_callback(bind(&Mixer::bm_hotplug_add, this, _1));
	BMUSBCapture::start_bm_thread();

#ifdef HAVE_SRT
	if (global_flags.srt_port >= 0) {
		start_srt();
	}
#endif

	chroma_subsampler.reset(new ChromaSubsampler(resource_pool.get()));

	if (global_flags.ten_bit_input) {
		if (!v210Converter::has_hardware_support()) {
			fprintf(stderr, "ERROR: --ten-bit-input requires support for OpenGL compute shaders\n");
			fprintf(stderr, "       (OpenGL 4.3, or GL_ARB_compute_shader + GL_ARB_shader_image_load_store).\n");
			abort();
		}
		v210_converter.reset(new v210Converter());

		// These are all the widths listed in the Blackmagic SDK documentation
		// (section 2.7.3, “Display Modes”).
		v210_converter->precompile_shader(720);
		v210_converter->precompile_shader(1280);
		v210_converter->precompile_shader(1920);
		v210_converter->precompile_shader(2048);
		v210_converter->precompile_shader(3840);
		v210_converter->precompile_shader(4096);
	}
	if (global_flags.ten_bit_output) {
		if (!v210Converter::has_hardware_support()) {
			fprintf(stderr, "ERROR: --ten-bit-output requires support for OpenGL compute shaders\n");
			fprintf(stderr, "       (OpenGL 4.3, or GL_ARB_compute_shader + GL_ARB_shader_image_load_store).\n");
			abort();
		}
	}

	timecode_renderer.reset(new TimecodeRenderer(resource_pool.get(), global_flags.width, global_flags.height));
	display_timecode_in_stream = global_flags.display_timecode_in_stream;
	display_timecode_on_stdout = global_flags.display_timecode_on_stdout;

	if (global_flags.enable_alsa_output) {
		alsa.reset(new ALSAOutput(OUTPUT_FREQUENCY, /*num_channels=*/2));
	}
	output_card_is_master = global_flags.output_card_is_master;
	if (global_flags.output_card != -1) {
		desired_output_card_index = global_flags.output_card;
		set_output_card_internal(global_flags.output_card);
	}

	output_jitter_history.register_metrics({{ "card", "output" }});

	ImageInput::start_update_thread(image_update_surface);
}

Mixer::~Mixer()
{
	ImageInput::end_update_thread();

	if (mjpeg_encoder != nullptr) {
		mjpeg_encoder->stop();
	}
	httpd.stop();
	BMUSBCapture::stop_bm_thread();

	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		if (cards[card_index].capture != nullptr) {  // Active.
			cards[card_index].capture->stop_dequeue_thread();
		}
		if (cards[card_index].output) {
			cards[card_index].output->end_output();
			cards[card_index].output.reset();
		}
	}

	video_encoder.reset(nullptr);
}

void Mixer::configure_card(unsigned card_index, CaptureInterface *capture, CardType card_type, DeckLinkOutput *output, bool is_srt_card)
{
	bool is_active = capture != nullptr;
	if (is_active) {
		printf("Configuring card %d...\n", card_index);
	} else {
		assert(card_type == CardType::FAKE_CAPTURE);
	}

	CaptureCard *card = &cards[card_index];
	if (card->capture != nullptr) {
		card_mutex.unlock();  // The dequeue thread could be waiting for bm_frame().
		card->capture->stop_dequeue_thread();
		card_mutex.lock();
	}
	card->capture.reset(capture);
	card->is_fake_capture = (card_type == CardType::FAKE_CAPTURE);
	if (card->is_fake_capture) {
		card->fake_capture_counter = fake_capture_counter++;
	}
	card->is_cef_capture = (card_type == CardType::CEF_INPUT);
	card->may_have_dropped_last_frame = false;
	card->type = card_type;
	if (card->output.get() != output) {
		card->output.reset(output);
	}

	PixelFormat pixel_format;
	if (card_type == CardType::FFMPEG_INPUT) {
		pixel_format = capture->get_current_pixel_format();
	} else if (card_type == CardType::CEF_INPUT) {
		pixel_format = PixelFormat_8BitBGRA;
	} else if (global_flags.ten_bit_input) {
		pixel_format = PixelFormat_10BitYCbCr;
	} else {
		pixel_format = PixelFormat_8BitYCbCr;
	}

	if (is_active) {
		card->capture->set_frame_callback(bind(&Mixer::bm_frame, this, card_index, _1, _2, _3, _4, _5, _6, _7));
		if (card->frame_allocator == nullptr) {
			card->frame_allocator.reset(new PBOFrameAllocator(pixel_format, 8 << 20, global_flags.width, global_flags.height, card_index, mjpeg_encoder.get()));  // 8 MB.
		} else {
			// The format could have changed, but we cannot reset the allocator
			// and create a new one from scratch, since there may be allocated
			// frames from it that expect to call release_frame() on it.
			// Instead, ask the allocator to create new frames for us and discard
			// any old ones as they come back. This takes the mutex while
			// allocating, but nothing should really be sending frames in there
			// right now anyway (start_bm_capture() has not been called yet).
			card->frame_allocator->reconfigure(pixel_format, 8 << 20, global_flags.width, global_flags.height, card_index, mjpeg_encoder.get());
		}
		card->capture->set_video_frame_allocator(card->frame_allocator.get());
		if (card->surface == nullptr) {
			card->surface = create_surface_with_same_format(mixer_surface);
		}
		while (!card->new_frames.empty()) card->new_frames.pop_front();
		card->last_timecode = -1;
		card->capture->set_pixel_format(pixel_format);
		card->capture->configure_card();

		// NOTE: start_bm_capture() happens in thread_func().
	}

	if (is_srt_card) {
		assert(card_type == CardType::FFMPEG_INPUT);
	}

	DeviceSpec device{InputSourceType::CAPTURE_CARD, card_index};
	unsigned num_channels = card_type == CardType::LIVE_CARD ? 8 : 2;
	if (is_active) {
		audio_mixer->set_device_parameters(device, card->capture->get_description(), card_type, num_channels, /*active=*/true);
	} else {
		// Note: Keeps the previous name, if any.
		char name[32];
		snprintf(name, sizeof(name), "Fake card %u", card_index + 1);
		audio_mixer->set_device_parameters(device, name, card_type, num_channels, /*active=*/false);
	}
	audio_mixer->reset_resampler(device);
	audio_mixer->trigger_state_changed_callback();

	// Unregister old metrics, if any.
	if (!card->labels.empty()) {
		const vector<pair<string, string>> &labels = card->labels;
		card->jitter_history.unregister_metrics(labels);
		card->queue_length_policy.unregister_metrics(labels);
		global_metrics.remove_if_exists("input_received_frames", labels);
		global_metrics.remove_if_exists("input_dropped_frames_jitter", labels);
		global_metrics.remove_if_exists("input_dropped_frames_error", labels);
		global_metrics.remove_if_exists("input_dropped_frames_resets", labels);
		global_metrics.remove_if_exists("input_queue_length_frames", labels);
		global_metrics.remove_if_exists("input_queue_duped_frames", labels);

		global_metrics.remove_if_exists("input_has_signal_bool", labels);
		global_metrics.remove_if_exists("input_is_connected_bool", labels);
		global_metrics.remove_if_exists("input_interlaced_bool", labels);
		global_metrics.remove_if_exists("input_width_pixels", labels);
		global_metrics.remove_if_exists("input_height_pixels", labels);
		global_metrics.remove_if_exists("input_frame_rate_nom", labels);
		global_metrics.remove_if_exists("input_frame_rate_den", labels);
		global_metrics.remove_if_exists("input_sample_rate_hz", labels);

		// SRT metrics.

		// Global measurements (counters).
		global_metrics.remove_if_exists("srt_uptime_seconds", labels);
		global_metrics.remove_if_exists("srt_send_duration_seconds", labels);
		global_metrics.remove_if_exists("srt_sent_bytes", labels);
		global_metrics.remove_if_exists("srt_received_bytes", labels);

		vector<pair<string, string>> packet_labels = card->labels;
		packet_labels.emplace_back("type", "normal");
		global_metrics.remove_if_exists("srt_sent_packets", packet_labels);
		global_metrics.remove_if_exists("srt_received_packets", packet_labels);

		packet_labels.back().second = "lost";
		global_metrics.remove_if_exists("srt_sent_packets", packet_labels);
		global_metrics.remove_if_exists("srt_received_packets", packet_labels);

		packet_labels.back().second = "retransmitted";
		global_metrics.remove_if_exists("srt_sent_packets", packet_labels);
		global_metrics.remove_if_exists("srt_sent_bytes", packet_labels);

		packet_labels.back().second = "ack";
		global_metrics.remove_if_exists("srt_sent_packets", packet_labels);
		global_metrics.remove_if_exists("srt_received_packets", packet_labels);

		packet_labels.back().second = "nak";
		global_metrics.remove_if_exists("srt_sent_packets", packet_labels);
		global_metrics.remove_if_exists("srt_received_packets", packet_labels);

		packet_labels.back().second = "dropped";
		global_metrics.remove_if_exists("srt_sent_packets", packet_labels);
		global_metrics.remove_if_exists("srt_received_packets", packet_labels);
		global_metrics.remove_if_exists("srt_sent_bytes", packet_labels);
		global_metrics.remove_if_exists("srt_received_bytes", packet_labels);

		packet_labels.back().second = "undecryptable";
		global_metrics.remove_if_exists("srt_received_packets", packet_labels);
		global_metrics.remove_if_exists("srt_received_bytes", packet_labels);

		global_metrics.remove_if_exists("srt_filter_sent_extra_packets", labels);
		global_metrics.remove_if_exists("srt_filter_received_extra_packets", labels);
		global_metrics.remove_if_exists("srt_filter_received_rebuilt_packets", labels);
		global_metrics.remove_if_exists("srt_filter_received_lost_packets", labels);

		// Instant measurements (gauges).
		global_metrics.remove_if_exists("srt_packet_sending_period_seconds", labels);
		global_metrics.remove_if_exists("srt_flow_window_packets", labels);
		global_metrics.remove_if_exists("srt_congestion_window_packets", labels);
		global_metrics.remove_if_exists("srt_flight_size_packets", labels);
		global_metrics.remove_if_exists("srt_rtt_seconds", labels);
		global_metrics.remove_if_exists("srt_estimated_bandwidth_bits_per_second", labels);
		global_metrics.remove_if_exists("srt_bandwidth_ceiling_bits_per_second", labels);
		global_metrics.remove_if_exists("srt_send_buffer_available_bytes", labels);
		global_metrics.remove_if_exists("srt_receive_buffer_available_bytes", labels);
		global_metrics.remove_if_exists("srt_mss_bytes", labels);

		global_metrics.remove_if_exists("srt_sender_unacked_packets", labels);
		global_metrics.remove_if_exists("srt_sender_unacked_bytes", labels);
		global_metrics.remove_if_exists("srt_sender_unacked_timespan_seconds", labels);
		global_metrics.remove_if_exists("srt_sender_delivery_delay_seconds", labels);

		global_metrics.remove_if_exists("srt_receiver_unacked_packets", labels);
		global_metrics.remove_if_exists("srt_receiver_unacked_bytes", labels);
		global_metrics.remove_if_exists("srt_receiver_unacked_timespan_seconds", labels);
		global_metrics.remove_if_exists("srt_receiver_delivery_delay_seconds", labels);
	}

	if (is_active) {
		// Register metrics.
		vector<pair<string, string>> labels;
		char card_name[64];
		snprintf(card_name, sizeof(card_name), "%d", card_index);
		labels.emplace_back("card", card_name);

		switch (card_type) {
		case CardType::LIVE_CARD:
			labels.emplace_back("cardtype", "live");
			break;
		case CardType::FAKE_CAPTURE:
			labels.emplace_back("cardtype", "fake");
			break;
		case CardType::FFMPEG_INPUT:
			if (is_srt_card) {
				labels.emplace_back("cardtype", "srt");
			} else {
				labels.emplace_back("cardtype", "ffmpeg");
			}
			break;
		case CardType::CEF_INPUT:
			labels.emplace_back("cardtype", "cef");
			break;
		default:
			assert(false);
		}
		card->jitter_history.register_metrics(labels);
		card->queue_length_policy.register_metrics(labels);
		global_metrics.add("input_received_frames", labels, &card->metric_input_received_frames);
		global_metrics.add("input_dropped_frames_jitter", labels, &card->metric_input_dropped_frames_jitter);
		global_metrics.add("input_dropped_frames_error", labels, &card->metric_input_dropped_frames_error);
		global_metrics.add("input_dropped_frames_resets", labels, &card->metric_input_resets);
		global_metrics.add("input_queue_length_frames", labels, &card->metric_input_queue_length_frames, Metrics::TYPE_GAUGE);
		global_metrics.add("input_queue_duped_frames", labels, &card->metric_input_duped_frames);

		global_metrics.add("input_has_signal_bool", labels, &card->metric_input_has_signal_bool, Metrics::TYPE_GAUGE);
		global_metrics.add("input_is_connected_bool", labels, &card->metric_input_is_connected_bool, Metrics::TYPE_GAUGE);
		global_metrics.add("input_interlaced_bool", labels, &card->metric_input_interlaced_bool, Metrics::TYPE_GAUGE);
		global_metrics.add("input_width_pixels", labels, &card->metric_input_width_pixels, Metrics::TYPE_GAUGE);
		global_metrics.add("input_height_pixels", labels, &card->metric_input_height_pixels, Metrics::TYPE_GAUGE);
		global_metrics.add("input_frame_rate_nom", labels, &card->metric_input_frame_rate_nom, Metrics::TYPE_GAUGE);
		global_metrics.add("input_frame_rate_den", labels, &card->metric_input_frame_rate_den, Metrics::TYPE_GAUGE);
		global_metrics.add("input_sample_rate_hz", labels, &card->metric_input_sample_rate_hz, Metrics::TYPE_GAUGE);

		if (is_srt_card) {
			// Global measurements (counters).
			global_metrics.add("srt_uptime_seconds", labels, &card->metric_srt_uptime_seconds);
			global_metrics.add("srt_send_duration_seconds", labels, &card->metric_srt_send_duration_seconds);
			global_metrics.add("srt_sent_bytes", labels, &card->metric_srt_sent_bytes);
			global_metrics.add("srt_received_bytes", labels, &card->metric_srt_received_bytes);

			vector<pair<string, string>> packet_labels = labels;
			packet_labels.emplace_back("type", "normal");
			global_metrics.add("srt_sent_packets", packet_labels, &card->metric_srt_sent_packets_normal);
			global_metrics.add("srt_received_packets", packet_labels, &card->metric_srt_received_packets_normal);

			packet_labels.back().second = "lost";
			global_metrics.add("srt_sent_packets", packet_labels, &card->metric_srt_sent_packets_lost);
			global_metrics.add("srt_received_packets", packet_labels, &card->metric_srt_received_packets_lost);

			packet_labels.back().second = "retransmitted";
			global_metrics.add("srt_sent_packets", packet_labels, &card->metric_srt_sent_packets_retransmitted);
			global_metrics.add("srt_sent_bytes", packet_labels, &card->metric_srt_sent_bytes_retransmitted);

			packet_labels.back().second = "ack";
			global_metrics.add("srt_sent_packets", packet_labels, &card->metric_srt_sent_packets_ack);
			global_metrics.add("srt_received_packets", packet_labels, &card->metric_srt_received_packets_ack);

			packet_labels.back().second = "nak";
			global_metrics.add("srt_sent_packets", packet_labels, &card->metric_srt_sent_packets_nak);
			global_metrics.add("srt_received_packets", packet_labels, &card->metric_srt_received_packets_nak);

			packet_labels.back().second = "dropped";
			global_metrics.add("srt_sent_packets", packet_labels, &card->metric_srt_sent_packets_dropped);
			global_metrics.add("srt_received_packets", packet_labels, &card->metric_srt_received_packets_dropped);
			global_metrics.add("srt_sent_bytes", packet_labels, &card->metric_srt_sent_bytes_dropped);
			global_metrics.add("srt_received_bytes", packet_labels, &card->metric_srt_received_bytes_dropped);

			packet_labels.back().second = "undecryptable";
			global_metrics.add("srt_received_packets", packet_labels, &card->metric_srt_received_packets_undecryptable);
			global_metrics.add("srt_received_bytes", packet_labels, &card->metric_srt_received_bytes_undecryptable);

			global_metrics.add("srt_filter_sent_extra_packets", labels, &card->metric_srt_filter_sent_packets);
			global_metrics.add("srt_filter_received_extra_packets", labels, &card->metric_srt_filter_received_extra_packets);
			global_metrics.add("srt_filter_received_rebuilt_packets", labels, &card->metric_srt_filter_received_rebuilt_packets);
			global_metrics.add("srt_filter_received_lost_packets", labels, &card->metric_srt_filter_received_lost_packets);

			// Instant measurements (gauges).
			global_metrics.add("srt_packet_sending_period_seconds", labels, &card->metric_srt_packet_sending_period_seconds, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_flow_window_packets", labels, &card->metric_srt_flow_window_packets, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_congestion_window_packets", labels, &card->metric_srt_congestion_window_packets, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_flight_size_packets", labels, &card->metric_srt_flight_size_packets, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_rtt_seconds", labels, &card->metric_srt_rtt_seconds, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_estimated_bandwidth_bits_per_second", labels, &card->metric_srt_estimated_bandwidth_bits_per_second, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_bandwidth_ceiling_bits_per_second", labels, &card->metric_srt_bandwidth_ceiling_bits_per_second, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_send_buffer_available_bytes", labels, &card->metric_srt_send_buffer_available_bytes, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_receive_buffer_available_bytes", labels, &card->metric_srt_receive_buffer_available_bytes, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_mss_bytes", labels, &card->metric_srt_mss_bytes, Metrics::TYPE_GAUGE);

			global_metrics.add("srt_sender_unacked_packets", labels, &card->metric_srt_sender_unacked_packets, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_sender_unacked_bytes", labels, &card->metric_srt_sender_unacked_bytes, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_sender_unacked_timespan_seconds", labels, &card->metric_srt_sender_unacked_timespan_seconds, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_sender_delivery_delay_seconds", labels, &card->metric_srt_sender_delivery_delay_seconds, Metrics::TYPE_GAUGE);

			global_metrics.add("srt_receiver_unacked_packets", labels, &card->metric_srt_receiver_unacked_packets, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_receiver_unacked_bytes", labels, &card->metric_srt_receiver_unacked_bytes, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_receiver_unacked_timespan_seconds", labels, &card->metric_srt_receiver_unacked_timespan_seconds, Metrics::TYPE_GAUGE);
			global_metrics.add("srt_receiver_delivery_delay_seconds", labels, &card->metric_srt_receiver_delivery_delay_seconds, Metrics::TYPE_GAUGE);
		}

		card->labels = labels;
	} else {
		card->labels.clear();
	}
}

void Mixer::set_output_card_internal(int card_index)
{
	// We don't really need to take card_mutex, since we're in the mixer
	// thread and don't mess with any queues (which is the only thing that happens
	// from other threads), but it's probably the safest in the long run.
	unique_lock<mutex> lock(card_mutex);
	if (output_card_index != -1) {
		// Switch the old card from output to input.
		CaptureCard *old_card = &cards[output_card_index];
		old_card->output->end_output();

		// Stop the fake card that we put into place.
		// This needs to _not_ happen under the mutex, to avoid deadlock
		// (delivering the last frame needs to take the mutex).
		CaptureInterface *fake_capture = old_card->capture.get();
		lock.unlock();
		fake_capture->stop_dequeue_thread();
		lock.lock();
		old_card->capture = move(old_card->parked_capture);  // TODO: reset the metrics
		old_card->is_fake_capture = false;
		old_card->capture->start_bm_capture();
	}
	if (card_index != -1) {
		CaptureCard *card = &cards[card_index];
		CaptureInterface *capture = card->capture.get();
		// TODO: DeckLinkCapture::stop_dequeue_thread can actually take
		// several seconds to complete (blocking on DisableVideoInput);
		// see if we can maybe do it asynchronously.
		lock.unlock();
		capture->stop_dequeue_thread();
		lock.lock();
		card->parked_capture = move(card->capture);
		CaptureInterface *fake_capture = new FakeCapture(global_flags.width, global_flags.height, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
		configure_card(card_index, fake_capture, CardType::FAKE_CAPTURE, card->output.release(), /*is_srt_card=*/false);
		card->jitter_history.clear();
		card->capture->start_bm_capture();
		desired_output_video_mode = output_video_mode = card->output->pick_video_mode(desired_output_video_mode);
		card->output->start_output(desired_output_video_mode, pts_int, /*is_master_card=*/output_card_is_master);
	}
	output_card_index = card_index;
	output_jitter_history.clear();
}

namespace {

int unwrap_timecode(uint16_t current_wrapped, int last)
{
	uint16_t last_wrapped = last & 0xffff;
	if (current_wrapped > last_wrapped) {
		return (last & ~0xffff) | current_wrapped;
	} else {
		return 0x10000 + ((last & ~0xffff) | current_wrapped);
	}
}

}  // namespace

void Mixer::bm_frame(unsigned card_index, uint16_t timecode,
                     FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
		     FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)
{
	DeviceSpec device{InputSourceType::CAPTURE_CARD, card_index};
	CaptureCard *card = &cards[card_index];

	++card->metric_input_received_frames;
	card->metric_input_has_signal_bool = video_format.has_signal;
	card->metric_input_is_connected_bool = video_format.is_connected;
	card->metric_input_interlaced_bool = video_format.interlaced;
	card->metric_input_width_pixels = video_format.width;
	card->metric_input_height_pixels = video_format.height;
	card->metric_input_frame_rate_nom = video_format.frame_rate_nom;
	card->metric_input_frame_rate_den = video_format.frame_rate_den;
	card->metric_input_sample_rate_hz = audio_format.sample_rate;

	if (is_mode_scanning[card_index]) {
		if (video_format.has_signal) {
			// Found a stable signal, so stop scanning.
			is_mode_scanning[card_index] = false;
		} else {
			static constexpr double switch_time_s = 0.1;  // Should be enough time for the signal to stabilize.
			steady_clock::time_point now = steady_clock::now();
			double sec_since_last_switch = duration<double>(steady_clock::now() - last_mode_scan_change[card_index]).count();
			if (sec_since_last_switch > switch_time_s) {
				// It isn't this mode; try the next one.
				mode_scanlist_index[card_index]++;
				mode_scanlist_index[card_index] %= mode_scanlist[card_index].size();
				cards[card_index].capture->set_video_mode(mode_scanlist[card_index][mode_scanlist_index[card_index]]);
				last_mode_scan_change[card_index] = now;
			}
		}
	}

	int64_t frame_length = int64_t(TIMEBASE) * video_format.frame_rate_den / video_format.frame_rate_nom;
	assert(frame_length > 0);

	size_t num_samples = (audio_frame.len > audio_offset) ? (audio_frame.len - audio_offset) / audio_format.num_channels / (audio_format.bits_per_sample / 8) : 0;
	if (num_samples > OUTPUT_FREQUENCY / 10 && card->type != CardType::FFMPEG_INPUT) {
		printf("%s: Dropping frame with implausible audio length (len=%d, offset=%d) [timecode=0x%04x video_len=%d video_offset=%d video_format=%x)\n",
			description_for_card(card_index).c_str(), int(audio_frame.len), int(audio_offset),
			timecode, int(video_frame.len), int(video_offset), video_format.id);
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}
		if (audio_frame.owner) {
			audio_frame.owner->release_frame(audio_frame);
		}
		return;
	}

	int dropped_frames = 0;
	if (card->last_timecode != -1) {
		dropped_frames = unwrap_timecode(timecode, card->last_timecode) - card->last_timecode - 1;
	}

	// Number of samples per frame if we need to insert silence.
	// (Could be nonintegral, but resampling will save us then.)
	const int silence_samples = OUTPUT_FREQUENCY * video_format.frame_rate_den / video_format.frame_rate_nom;

	if (dropped_frames > MAX_FPS * 2) {
		fprintf(stderr, "%s lost more than two seconds (or time code jumping around; from 0x%04x to 0x%04x), resetting resampler\n",
			description_for_card(card_index).c_str(), card->last_timecode, timecode);
		audio_mixer->reset_resampler(device);
		dropped_frames = 0;
		++card->metric_input_resets;
	} else if (dropped_frames > 0) {
		// Insert silence as needed.
		fprintf(stderr, "%s dropped %d frame(s) (before timecode 0x%04x), inserting silence.\n",
			description_for_card(card_index).c_str(), dropped_frames, timecode);
		card->metric_input_dropped_frames_error += dropped_frames;

		bool success;
		do {
			success = audio_mixer->add_silence(device, silence_samples, dropped_frames);
		} while (!success);
	}

	if (num_samples > 0) {
		audio_mixer->add_audio(device, audio_frame.data + audio_offset, num_samples, audio_format, audio_frame.received_timestamp);

		// Audio for the MJPEG stream. We don't resample; audio that's not in 48 kHz
		// just gets dropped for now.
		//
		// Only bother doing MJPEG encoding if there are any connected clients
		// that want the stream.
		if (httpd.get_num_connected_multicam_clients() > 0 ||
		    httpd.get_num_connected_siphon_clients(card_index) > 0) {
			vector<int32_t> converted_samples = convert_audio_to_fixed32(audio_frame.data + audio_offset, num_samples, audio_format, 2);
			lock_guard<mutex> lock(card_mutex);
			if (card->new_raw_audio.empty()) {
				card->new_raw_audio = move(converted_samples);
			} else {
				// For raw audio, we don't really synchronize audio and video;
				// we just put the audio in frame by frame, and if a video frame is
				// dropped, we still keep the audio, which means it will be added
				// to the beginning of the next frame. It would probably be better
				// to move the audio pts earlier to show this, but most players can
				// live with some jitter, and in a lot of ways, it's much nicer for
				// Futatabi to have all audio locked to a video frame.
				card->new_raw_audio.insert(card->new_raw_audio.end(), converted_samples.begin(), converted_samples.end());

				// Truncate to one second, just to be sure we don't have infinite buildup in case of weirdness.
				if (card->new_raw_audio.size() > OUTPUT_FREQUENCY * 2) {
					size_t excess_samples = card->new_raw_audio.size() - OUTPUT_FREQUENCY * 2;
					card->new_raw_audio.erase(card->new_raw_audio.begin(), card->new_raw_audio.begin() + excess_samples);
				}
			}
		}
	}

	// Done with the audio, so release it.
	if (audio_frame.owner) {
		audio_frame.owner->release_frame(audio_frame);
	}

	card->last_timecode = timecode;

	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)video_frame.userdata;
	if (card->type == CardType::FFMPEG_INPUT && userdata != nullptr) {
		FFmpegCapture *ffmpeg_capture = static_cast<FFmpegCapture *>(card->capture.get());
		userdata->has_last_subtitle = ffmpeg_capture->get_has_last_subtitle();
		userdata->last_subtitle = ffmpeg_capture->get_last_subtitle();
	}
#ifdef HAVE_SRT
	if (card->type == CardType::FFMPEG_INPUT) {
		int srt_sock = static_cast<FFmpegCapture *>(card->capture.get())->get_srt_sock();
		if (srt_sock != -1) {
			update_srt_stats(srt_sock, card);
		}
	}
#endif

	size_t y_offset, cbcr_offset;
	size_t expected_length = video_format.stride * (video_format.height + video_format.extra_lines_top + video_format.extra_lines_bottom);
	if (userdata != nullptr && userdata->pixel_format == PixelFormat_8BitYCbCrPlanar) {
		// The calculation above is wrong for planar Y'CbCr, so just override it.
		assert(card->type == CardType::FFMPEG_INPUT);
		assert(video_offset == 0);
		expected_length = video_frame.len;

		userdata->ycbcr_format = (static_cast<FFmpegCapture *>(card->capture.get()))->get_current_frame_ycbcr_format();
		y_offset = 0;
		cbcr_offset = video_format.width * video_format.height;
	} else {
		// All the other Y'CbCr formats are 4:2:2.
		y_offset = video_frame.size / 2 + video_offset / 2;
		cbcr_offset = video_offset / 2;
	}
	if (video_frame.len - video_offset == 0 ||
	    video_frame.len - video_offset != expected_length) {
		if (video_frame.len != 0) {
			printf("%s: Dropping video frame with wrong length (%zu; expected %zu)\n",
				description_for_card(card_index).c_str(), video_frame.len - video_offset, expected_length);
		}
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}

		// Still send on the information that we _had_ a frame, even though it's corrupted,
		// so that pts can go up accordingly.
		{
			lock_guard<mutex> lock(card_mutex);
			CaptureCard::NewFrame new_frame;
			new_frame.frame = RefCountedFrame(FrameAllocator::Frame());
			new_frame.length = frame_length;
			new_frame.interlaced = false;
			new_frame.dropped_frames = dropped_frames;
			new_frame.received_timestamp = video_frame.received_timestamp;
			card->new_frames.push_back(move(new_frame));
			card->jitter_history.frame_arrived(video_frame.received_timestamp, frame_length, dropped_frames);
		}
		card->new_frames_changed.notify_all();
		return;
	}

	unsigned num_fields = video_format.interlaced ? 2 : 1;
	steady_clock::time_point frame_upload_start;
	if (video_format.interlaced) {
		// Send the two fields along as separate frames; the other side will need to add
		// a deinterlacer to actually get this right.
		assert(video_format.height % 2 == 0);
		video_format.height /= 2;
		assert(frame_length % 2 == 0);
		frame_length /= 2;
		num_fields = 2;
		frame_upload_start = steady_clock::now();
	}
	assert(userdata != nullptr);
	userdata->last_interlaced = video_format.interlaced;
	userdata->last_has_signal = video_format.has_signal;
	userdata->last_is_connected = video_format.is_connected;
	userdata->last_frame_rate_nom = video_format.frame_rate_nom;
	userdata->last_frame_rate_den = video_format.frame_rate_den;
	RefCountedFrame frame(video_frame);

	// Send the frames on to the main thread, which will upload and process htem.
	// It is entirely possible to upload them in the same thread (and it might even be
	// faster, depending on the GPU and driver), but it appears to be trickling
	// driver bugs very easily.
	//
	// Note that this means we must hold on to the actual frame data in <userdata>
	// until the upload is done, but we hold on to <frame> much longer than that
	// (in fact, all the way until we no longer use the texture in rendering).
	for (unsigned field = 0; field < num_fields; ++field) {
		if (field == 1) {
			// Don't upload the second field as fast as we can; wait until
			// the field time has approximately passed. (Otherwise, we could
			// get timing jitter against the other sources, and possibly also
			// against the video display, although the latter is not as critical.)
			// This requires our system clock to be reasonably close to the
			// video clock, but that's not an unreasonable assumption.
			steady_clock::time_point second_field_start = frame_upload_start +
				nanoseconds(frame_length * 1000000000 / TIMEBASE);
			this_thread::sleep_until(second_field_start);
		}

		{
			lock_guard<mutex> lock(card_mutex);
			CaptureCard::NewFrame new_frame;
			new_frame.frame = frame;
			new_frame.length = frame_length;
			new_frame.field = field;
			new_frame.interlaced = video_format.interlaced;
			new_frame.dropped_frames = dropped_frames;
			new_frame.received_timestamp = video_frame.received_timestamp;  // Ignore the audio timestamp.
			new_frame.video_format = video_format;
			new_frame.video_offset = video_offset;
			new_frame.y_offset = y_offset;
			new_frame.cbcr_offset = cbcr_offset;
			new_frame.texture_uploaded = false;
			if (card->type == CardType::FFMPEG_INPUT) {
				FFmpegCapture *ffmpeg_capture = static_cast<FFmpegCapture *>(card->capture.get());
				new_frame.neutral_color = ffmpeg_capture->get_last_neutral_color();
			}
			card->new_frames.push_back(move(new_frame));
			card->jitter_history.frame_arrived(video_frame.received_timestamp, frame_length, dropped_frames);
			card->may_have_dropped_last_frame = false;
		}
		card->new_frames_changed.notify_all();
	}
}

void Mixer::upload_texture_for_frame(
	int field, bmusb::VideoFormat video_format,
	size_t y_offset, size_t cbcr_offset, size_t video_offset, PBOFrameAllocator::Userdata *userdata)
{
	size_t cbcr_width, cbcr_height;
	if (userdata != nullptr && userdata->pixel_format == PixelFormat_8BitYCbCrPlanar) {
		cbcr_width = video_format.width / userdata->ycbcr_format.chroma_subsampling_x;
		cbcr_height = video_format.height / userdata->ycbcr_format.chroma_subsampling_y;
	} else {
		// All the other Y'CbCr formats are 4:2:2.
		cbcr_width = video_format.width / 2;
		cbcr_height = video_format.height;
	}

	bool interlaced_stride = video_format.interlaced && (video_format.second_field_start == 1);
	if (video_format.interlaced) {
		cbcr_height /= 2;
	}

	unsigned field_start_line;
	if (field == 1) {
		field_start_line = video_format.second_field_start;
	} else {
		field_start_line = video_format.extra_lines_top;
	}

	// For anything not FRAME_FORMAT_YCBCR_10BIT, v210_width will be nonsensical but not used.
	size_t v210_width = video_format.stride / sizeof(uint32_t);
	ensure_texture_resolution(userdata, field, video_format.width, video_format.height, cbcr_width, cbcr_height, v210_width);

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, userdata->pbo);
	check_error();

	switch (userdata->pixel_format) {
		case PixelFormat_10BitYCbCr: {
			size_t field_start = video_offset + video_format.stride * field_start_line;
			upload_texture(userdata->tex_v210[field], v210_width, video_format.height, video_format.stride, interlaced_stride, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, field_start);
			v210_converter->convert(userdata->tex_v210[field], userdata->tex_444[field], video_format.width, video_format.height);
			break;
		}
		case PixelFormat_8BitYCbCr: {
			size_t field_y_start = y_offset + video_format.width * field_start_line;
			size_t field_cbcr_start = cbcr_offset + cbcr_width * field_start_line * sizeof(uint16_t);

			// Make up our own strides, since we are interleaving.
			upload_texture(userdata->tex_y[field], video_format.width, video_format.height, video_format.width, interlaced_stride, GL_RED, GL_UNSIGNED_BYTE, field_y_start);
			upload_texture(userdata->tex_cbcr[field], cbcr_width, cbcr_height, cbcr_width * sizeof(uint16_t), interlaced_stride, GL_RG, GL_UNSIGNED_BYTE, field_cbcr_start);
			break;
		}
		case PixelFormat_8BitYCbCrPlanar: {
			assert(field_start_line == 0);  // We don't really support interlaced here.
			size_t field_y_start = y_offset;
			size_t field_cb_start = cbcr_offset;
			size_t field_cr_start = cbcr_offset + cbcr_width * cbcr_height;

			// Make up our own strides, since we are interleaving.
			upload_texture(userdata->tex_y[field], video_format.width, video_format.height, video_format.width, interlaced_stride, GL_RED, GL_UNSIGNED_BYTE, field_y_start);
			upload_texture(userdata->tex_cb[field], cbcr_width, cbcr_height, cbcr_width, interlaced_stride, GL_RED, GL_UNSIGNED_BYTE, field_cb_start);
			upload_texture(userdata->tex_cr[field], cbcr_width, cbcr_height, cbcr_width, interlaced_stride, GL_RED, GL_UNSIGNED_BYTE, field_cr_start);
			break;
		}
		case PixelFormat_8BitBGRA: {
			size_t field_start = video_offset + video_format.stride * field_start_line;
			upload_texture(userdata->tex_rgba[field], video_format.width, video_format.height, video_format.stride, interlaced_stride, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, field_start);
			// These could be asked to deliver mipmaps at any time.
			glBindTexture(GL_TEXTURE_2D, userdata->tex_rgba[field]);
			check_error();
			glGenerateMipmap(GL_TEXTURE_2D);
			check_error();
			glBindTexture(GL_TEXTURE_2D, 0);
			check_error();
			break;
		}
		default:
			assert(false);
	}

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	check_error();
}

void Mixer::bm_hotplug_add(libusb_device *dev)
{
	lock_guard<mutex> lock(hotplug_mutex);
	hotplugged_cards.push_back(dev);
}

void Mixer::bm_hotplug_remove(unsigned card_index)
{
	cards[card_index].new_frames_changed.notify_all();
}

void Mixer::thread_func()
{
	pthread_setname_np(pthread_self(), "Mixer_OpenGL");

	eglBindAPI(EGL_OPENGL_API);
	QOpenGLContext *context = create_context(mixer_surface);
	if (!make_current(context, mixer_surface)) {
		printf("oops\n");
		abort();
	}

	// Start the actual capture. (We don't want to do it before we're actually ready
	// to process output frames.)
	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		if (int(card_index) != output_card_index && cards[card_index].capture != nullptr) {
			cards[card_index].capture->start_bm_capture();
		}
	}

	BasicStats basic_stats(/*verbose=*/true, /*use_opengl=*/true);
	int stats_dropped_frames = 0;

	while (!should_quit) {
		if (desired_output_card_index != output_card_index) {
			set_output_card_internal(desired_output_card_index);
		}
		if (output_card_index != -1 &&
		    desired_output_video_mode != output_video_mode) {
			DeckLinkOutput *output = cards[output_card_index].output.get();
			output->end_output();
			desired_output_video_mode = output_video_mode = output->pick_video_mode(desired_output_video_mode);
			output->start_output(desired_output_video_mode, pts_int, /*is_master_card=*/output_card_is_master);
		}

		{
			lock_guard<mutex> lock(card_mutex);
			handle_hotplugged_cards();
		}

		CaptureCard::NewFrame new_frames[MAX_VIDEO_CARDS];
		bool has_new_frame[MAX_VIDEO_CARDS] = { false };

		bool master_card_is_output;
		unsigned master_card_index;
		if (output_card_index != -1 && output_card_is_master) {
			master_card_is_output = true;
			master_card_index = output_card_index;
		} else {
			master_card_is_output = false;
			master_card_index = theme->map_signal_to_card(master_clock_channel);
			assert(master_card_index < MAX_VIDEO_CARDS);
		}

		vector<int32_t> raw_audio[MAX_VIDEO_CARDS];  // For MJPEG encoding.
		OutputFrameInfo	output_frame_info = get_one_frame_from_each_card(master_card_index, master_card_is_output, new_frames, has_new_frame, raw_audio);
		schedule_audio_resampling_tasks(output_frame_info.dropped_frames, output_frame_info.num_samples, output_frame_info.frame_duration, output_frame_info.is_preroll, output_frame_info.frame_timestamp);
		stats_dropped_frames += output_frame_info.dropped_frames;

		for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
			if (card_index == master_card_index || !has_new_frame[card_index]) {
				continue;
			}
			if (new_frames[card_index].frame->len == 0) {
				++new_frames[card_index].dropped_frames;
			}
			if (new_frames[card_index].dropped_frames > 0) {
				printf("%s dropped %d frames before this\n",
					description_for_card(card_index).c_str(), int(new_frames[card_index].dropped_frames));
			}
		}

		// If the first card is reporting a corrupted or otherwise dropped frame,
		// just increase the pts (skipping over this frame) and don't try to compute anything new.
		if (!master_card_is_output &&
		    new_frames[master_card_index].frame != nullptr &&  // Timeout.
		    new_frames[master_card_index].frame->len == 0) {
			++stats_dropped_frames;
			pts_int += new_frames[master_card_index].length;
			continue;
		}

		for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
			if (!has_new_frame[card_index] || new_frames[card_index].frame->len == 0)
				continue;

			CaptureCard::NewFrame *new_frame = &new_frames[card_index];
			assert(new_frame->frame != nullptr);
			insert_new_frame(new_frame->frame, new_frame->field, new_frame->interlaced, card_index, &input_state);
			check_error();

			// The new texture might need uploading before use.
			if (!new_frame->texture_uploaded) {
				upload_texture_for_frame(new_frame->field, new_frame->video_format, new_frame->y_offset, new_frame->cbcr_offset,
					new_frame->video_offset, (PBOFrameAllocator::Userdata *)new_frame->frame->userdata);
				new_frame->texture_uploaded = true;
			}

			// Only set the white balance if it actually changed. This means that the user
			// is free to override the white balance in a video with no white balance information
			// actually set (ie. r=g=b=1 all the time), or one where the white point is wrong,
			// but frame-to-frame decisions will be heeded. We do this pretty much as late
			// as possible (ie., after picking out the frame from the buffer), so that we are sure
			// that the change takes effect on exactly the right frame.
			if (fabs(new_frame->neutral_color.r - last_received_neutral_color[card_index].r) > 1e-3 ||
			    fabs(new_frame->neutral_color.g - last_received_neutral_color[card_index].g) > 1e-3 ||
			    fabs(new_frame->neutral_color.b - last_received_neutral_color[card_index].b) > 1e-3) {
				theme->set_wb_for_card(card_index, new_frame->neutral_color.r, new_frame->neutral_color.g, new_frame->neutral_color.b);
				last_received_neutral_color[card_index] = new_frame->neutral_color;
			}

			if (new_frame->frame->data_copy != nullptr && mjpeg_encoder->should_encode_mjpeg_for_card(card_index)) {
				RGBTriplet neutral_color = theme->get_white_balance_for_card(card_index);
				mjpeg_encoder->upload_frame(pts_int, card_index, new_frame->frame, new_frame->video_format, new_frame->y_offset, new_frame->cbcr_offset, move(raw_audio[card_index]), neutral_color);
			}

		}

		int64_t frame_duration = output_frame_info.frame_duration;
		render_one_frame(frame_duration);
		{
			lock_guard<mutex> lock(frame_num_mutex);
			++frame_num;
		}
		frame_num_updated.notify_all();
		pts_int += frame_duration;

		basic_stats.update(frame_num, stats_dropped_frames);
		// if (frame_num % 100 == 0) chain->print_phase_timing();

		if (should_cut.exchange(false)) {  // Test and clear.
			video_encoder->do_cut(frame_num);
		}

#if 0
		// Reset every 100 frames, so that local variations in frame times
		// (especially for the first few frames, when the shaders are
		// compiled etc.) don't make it hard to measure for the entire
		// remaining duration of the program.
		if (frame == 10000) {
			frame = 0;
			start = now;
		}
#endif
		check_error();
	}

	resource_pool->clean_context();
}

bool Mixer::input_card_is_master_clock(unsigned card_index, unsigned master_card_index) const
{
	if (output_card_index != -1 && output_card_is_master) {
		// The output card (ie., cards[output_card_index].output) is the master clock,
		// so no input card (ie., cards[card_index].capture) is.
		return false;
	}
	return (card_index == master_card_index);
}

void Mixer::trim_queue(CaptureCard *card, size_t safe_queue_length)
{
	// Count the number of frames in the queue, including any frames
	// we dropped. It's hard to know exactly how we should deal with
	// dropped (corrupted) input frames; they don't help our goal of
	// avoiding starvation, but they still add to the problem of latency.
	// Since dropped frames is going to mean a bump in the signal anyway,
	// we err on the side of having more stable latency instead.
	unsigned queue_length = 0;
	for (const CaptureCard::NewFrame &frame : card->new_frames) {
		queue_length += frame.dropped_frames + 1;
	}

	// If needed, drop frames until the queue is below the safe limit.
	// We prefer to drop from the head, because all else being equal,
	// we'd like more recent frames (less latency).
	unsigned dropped_frames = 0;
	while (queue_length > safe_queue_length) {
		assert(!card->new_frames.empty());
		assert(queue_length > card->new_frames.front().dropped_frames);
		queue_length -= card->new_frames.front().dropped_frames;

		if (queue_length <= safe_queue_length) {
			// No need to drop anything.
			break;
		}

		card->new_frames.pop_front();
		card->new_frames_changed.notify_all();
		--queue_length;
		++dropped_frames;

		if (queue_length == 0 && card->is_cef_capture) {
			card->may_have_dropped_last_frame = true;
		}
	}

	card->metric_input_dropped_frames_jitter += dropped_frames;
	card->metric_input_queue_length_frames = queue_length;

#if 0
	if (dropped_frames > 0) {
		fprintf(stderr, "Card %u dropped %u frame(s) to keep latency down.\n",
			card_index, dropped_frames);
	}
#endif
}

pair<string, string> Mixer::get_channels_json()
{
	Channels ret;
	for (int channel_idx = 0; channel_idx < theme->get_num_channels(); ++channel_idx) {
		Channel *channel = ret.add_channel();
		channel->set_index(channel_idx + 2);
		channel->set_name(theme->get_channel_name(channel_idx + 2));
		channel->set_color(theme->get_channel_color(channel_idx + 2));
	}
	string contents;
	google::protobuf::util::MessageToJsonString(ret, &contents);  // Ignore any errors.
	return make_pair(contents, "text/json");
}

pair<string, string> Mixer::get_channel_color_http(unsigned channel_idx)
{
	return make_pair(theme->get_channel_color(channel_idx), "text/plain");
}

Mixer::OutputFrameInfo Mixer::get_one_frame_from_each_card(unsigned master_card_index, bool master_card_is_output, CaptureCard::NewFrame new_frames[MAX_VIDEO_CARDS], bool has_new_frame[MAX_VIDEO_CARDS], vector<int32_t> raw_audio[MAX_VIDEO_CARDS])
{
	OutputFrameInfo output_frame_info;
	constexpr steady_clock::duration master_card_timeout = milliseconds(200);
start:
	unique_lock<mutex> lock(card_mutex, defer_lock);
	bool timed_out = false;
	if (master_card_is_output) {
		// Clocked to the output, so wait for it to be ready for the next frame.
		cards[master_card_index].output->wait_for_frame(pts_int, &output_frame_info.dropped_frames, &output_frame_info.frame_duration, &output_frame_info.is_preroll, &output_frame_info.frame_timestamp);
		lock.lock();
	} else {
		// Wait for the master card to have a new frame.
		output_frame_info.is_preroll = false;
		lock.lock();
		timed_out = !cards[master_card_index].new_frames_changed.wait_for(lock,
			master_card_timeout,
			[this, master_card_index] {
				return !cards[master_card_index].new_frames.empty() ||
					cards[master_card_index].capture == nullptr ||
					cards[master_card_index].capture->get_disconnected();
			});
		if (timed_out) {
			fprintf(stderr, "WARNING: Master card (%s) did not deliver a frame for %u ms, creating a fake one.\n",
				description_for_card(master_card_index).c_str(),
				unsigned(duration_cast<milliseconds>(master_card_timeout).count()));
		}
	}

	if (timed_out) {
		// The master card stalled for 200 ms (possible when it's e.g.
		// an SRT card). Send a frame no matter what; this also makes sure
		// any other cards get to empty their queues, and in general,
		// that we make _some_ sort of forward progress.
		handle_hotplugged_cards();
	} else if (master_card_is_output) {
		handle_hotplugged_cards();
	} else if (cards[master_card_index].new_frames.empty()) {
		// We were woken up, but not due to a new frame. Deal with it
		// and then restart.
		assert(cards[master_card_index].capture == nullptr ||
		       cards[master_card_index].capture->get_disconnected());
		handle_hotplugged_cards();
		lock.unlock();
		goto start;
	}

	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->new_frames.empty()) {  // Starvation.
			++card->metric_input_duped_frames;
#ifdef HAVE_CEF
			if (card->is_cef_capture && card->may_have_dropped_last_frame) {
				// Unlike other sources, CEF is not guaranteed to send us a steady
				// stream of frames, so we'll have to ask it to repaint the frame
				// we dropped. (may_have_dropped_last_frame is set whenever we
				// trim the queue completely away, and cleared when we actually
				// get a new frame.)
				((CEFCapture *)card->capture.get())->request_new_frame(/*ignore_if_locked=*/true);
			}
#endif
		} else {
			new_frames[card_index] = move(card->new_frames.front());
			has_new_frame[card_index] = true;
			card->new_frames.pop_front();
			card->new_frames_changed.notify_all();
		}

		raw_audio[card_index] = move(card->new_raw_audio);
	}

	if (timed_out) {
		// Pretend the frame happened a while ago and was only processed now,
		// so that we get the duration sort-of right. This isn't ideal.
		output_frame_info.dropped_frames = 0;  // Hard to define, really.
		output_frame_info.frame_duration = lrint(TIMEBASE * duration<double>(master_card_timeout).count());
		output_frame_info.frame_timestamp = steady_clock::now() - master_card_timeout;
	} else if (!master_card_is_output) {
		output_frame_info.frame_timestamp = new_frames[master_card_index].received_timestamp;
		output_frame_info.dropped_frames = new_frames[master_card_index].dropped_frames;
		output_frame_info.frame_duration = new_frames[master_card_index].length;
	}

	if (!output_frame_info.is_preroll) {
		output_jitter_history.frame_arrived(output_frame_info.frame_timestamp, output_frame_info.frame_duration, output_frame_info.dropped_frames);
	}

	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (has_new_frame[card_index] &&
		    !input_card_is_master_clock(card_index, master_card_index) &&
		    !output_frame_info.is_preroll) {
			card->queue_length_policy.update_policy(
				output_frame_info.frame_timestamp,
				card->jitter_history.get_expected_next_frame(),
				new_frames[master_card_index].length,
				output_frame_info.frame_duration,
				card->jitter_history.estimate_max_jitter(),
				output_jitter_history.estimate_max_jitter());
			trim_queue(card, min<int>(global_flags.max_input_queue_frames,
			                          card->queue_length_policy.get_safe_queue_length()));
		}
	}

	// This might get off by a fractional sample when changing master card
	// between ones with different frame rates, but that's fine.
	int64_t num_samples_times_timebase = int64_t(OUTPUT_FREQUENCY) * output_frame_info.frame_duration + fractional_samples;
	output_frame_info.num_samples = num_samples_times_timebase / TIMEBASE;
	fractional_samples = num_samples_times_timebase % TIMEBASE;
	assert(output_frame_info.num_samples >= 0);

	if (timed_out) {
		DeviceSpec device{InputSourceType::CAPTURE_CARD, master_card_index};
		bool success;
		do {
			success = audio_mixer->add_silence(device, output_frame_info.num_samples, /*dropped_frames=*/0);
		} while (!success);
	}

	return output_frame_info;
}

void Mixer::handle_hotplugged_cards()
{
	// Check for cards that have been disconnected since last frame.
	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->capture != nullptr && card->capture->get_disconnected()) {
			bool is_active = card_index < unsigned(global_flags.min_num_cards) || cards[card_index].force_active;
			if (is_active) {
				fprintf(stderr, "Card %u went away, replacing with a fake card.\n", card_index);
				FakeCapture *capture = new FakeCapture(global_flags.width, global_flags.height, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
				configure_card(card_index, capture, CardType::FAKE_CAPTURE, /*output=*/nullptr, /*is_srt_card=*/false);
				card->jitter_history.clear();
				card->capture->start_bm_capture();
			} else {
				// NOTE: The theme might end up forcing the card back at some later point
				// (ie., force_active is false now, but might immediately be true again on
				// e.g. the next frame). That should be rare, though, so we don't bother
				// adjusting the message.
				fprintf(stderr, "Card %u went away, removing. (To keep a fake card, increase --num-cards.)\n", card_index);
				theme->remove_card(card_index);
				configure_card(card_index, /*capture=*/nullptr, CardType::FAKE_CAPTURE, /*output=*/nullptr, /*is_srt_card=*/false);
				card->jitter_history.clear();
			}
		}
	}

	// Count how many active cards we already have. Used below to check that we
	// don't go past the max_cards limit set by the user. Note that (non-SRT) video
	// and HTML “cards” don't count towards this limit.
	int num_video_cards = 0;
	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->type == CardType::LIVE_CARD || is_srt_card(card)) {
			++num_video_cards;
		}
	}

	// Check for cards that have been connected since last frame.
	vector<libusb_device *> hotplugged_cards_copy;
#ifdef HAVE_SRT
	vector<int> hotplugged_srt_cards_copy;
#endif
	{
		lock_guard<mutex> lock(hotplug_mutex);
		swap(hotplugged_cards, hotplugged_cards_copy);
#ifdef HAVE_SRT
		swap(hotplugged_srt_cards, hotplugged_srt_cards_copy);
#endif
	}
	for (libusb_device *new_dev : hotplugged_cards_copy) {
		// Look for a fake capture card where we can stick this in.
		int free_card_index = -1;
		for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
			if (cards[card_index].is_fake_capture) {
				free_card_index = card_index;
				break;
			}
		}

		if (free_card_index == -1 || num_video_cards >= global_flags.max_num_cards) {
			fprintf(stderr, "New card plugged in, but no free slots -- ignoring.\n");
			libusb_unref_device(new_dev);
		} else {
			// BMUSBCapture takes ownership.
			fprintf(stderr, "New card plugged in, choosing slot %d.\n", free_card_index);
			CaptureCard *card = &cards[free_card_index];
			BMUSBCapture *capture = new BMUSBCapture(free_card_index, new_dev);
			configure_card(free_card_index, capture, CardType::LIVE_CARD, /*output=*/nullptr, /*is_srt_card=*/false);
			card->jitter_history.clear();
			capture->set_card_disconnected_callback(bind(&Mixer::bm_hotplug_remove, this, free_card_index));
			capture->start_bm_capture();
		}
	}

#ifdef HAVE_SRT
	// Same, for SRT inputs.
	for (SRTSOCKET sock : hotplugged_srt_cards_copy) {
		char name[256];
		int namelen = sizeof(name);
		srt_getsockopt(sock, /*ignored=*/0, SRTO_STREAMID, name, &namelen);
		string stream_id(name, namelen);

		// Look for a fake capture card where we can stick this in.
		// Prioritize ones that previously held SRT streams with the
		// same stream ID, if any exist -- and it multiple exist,
		// take the one that disconnected the last.
		int first_free_card_index = -1, last_matching_free_card_index = -1;
		for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
			CaptureCard *card = &cards[card_index];
			if (!card->is_fake_capture) {
				continue;
			}
			if (first_free_card_index == -1) {
				first_free_card_index = card_index;
			}
			if (card->last_srt_stream_id == stream_id &&
			    (last_matching_free_card_index == -1 ||
			     card->fake_capture_counter >
				cards[last_matching_free_card_index].fake_capture_counter)) {
				last_matching_free_card_index = card_index;
			}
		}

		const int free_card_index = (last_matching_free_card_index != -1)
			? last_matching_free_card_index : first_free_card_index;
		if (free_card_index == -1 || num_video_cards >= global_flags.max_num_cards) {
			if (stream_id.empty()) {
				stream_id = "no name";
			}
			fprintf(stderr, "New SRT stream connected (%s), but no free slots -- ignoring.\n", stream_id.c_str());
			srt_close(sock);
		} else {
			// FFmpegCapture takes ownership.
			if (stream_id.empty()) {
				fprintf(stderr, "New unnamed SRT stream connected, choosing slot %d.\n", free_card_index);
			} else {
				fprintf(stderr, "New SRT stream connected (%s), choosing slot %d.\n", stream_id.c_str(), free_card_index);
			}
			CaptureCard *card = &cards[free_card_index];
			FFmpegCapture *capture = new FFmpegCapture(sock, stream_id);
			capture->set_card_index(free_card_index);
			configure_card(free_card_index, capture, CardType::FFMPEG_INPUT, /*output=*/nullptr, /*is_srt_card=*/true);
			update_srt_stats(sock, card);  // Initial zero stats.
			card->last_srt_stream_id = stream_id;
			card->jitter_history.clear();
			capture->set_card_disconnected_callback(bind(&Mixer::bm_hotplug_remove, this, free_card_index));
			capture->start_bm_capture();
		}
	}
#endif

	// Finally, newly forced-to-active fake capture cards.
	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->capture == nullptr && card->force_active) {
			FakeCapture *capture = new FakeCapture(global_flags.width, global_flags.height, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
			configure_card(card_index, capture, CardType::FAKE_CAPTURE, /*output=*/nullptr, /*is_srt_card=*/false);
			card->jitter_history.clear();
			card->capture->start_bm_capture();
		}
	}
}


void Mixer::schedule_audio_resampling_tasks(unsigned dropped_frames, int num_samples_per_frame, int length_per_frame, bool is_preroll, steady_clock::time_point frame_timestamp)
{
	// Resample the audio as needed, including from previously dropped frames.
	for (unsigned frame_num = 0; frame_num < dropped_frames + 1; ++frame_num) {
		const bool dropped_frame = (frame_num != dropped_frames);
		{
			// Signal to the audio thread to process this frame.
			// Note that if the frame is a dropped frame, we signal that
			// we don't want to use this frame as base for adjusting
			// the resampler rate. The reason for this is that the timing
			// of these frames is often way too late; they typically don't
			// “arrive” before we synthesize them. Thus, we could end up
			// in a situation where we have inserted e.g. five audio frames
			// into the queue before we then start pulling five of them
			// back out. This makes ResamplingQueue overestimate the delay,
			// causing undue resampler changes. (We _do_ use the last,
			// non-dropped frame; perhaps we should just discard that as well,
			// since dropped frames are expected to be rare, and it might be
			// better to just wait until we have a slightly more normal situation).
			lock_guard<mutex> lock(audio_mutex);
			bool adjust_rate = !dropped_frame && !is_preroll;
			audio_task_queue.push(AudioTask{pts_int, num_samples_per_frame, adjust_rate, frame_timestamp});
			audio_task_queue_changed.notify_one();
		}
		if (dropped_frame) {
			// For dropped frames, increase the pts. Note that if the format changed
			// in the meantime, we have no way of detecting that; we just have to
			// assume the frame length is always the same.
			pts_int += length_per_frame;
		}
	}
}

void Mixer::render_one_frame(int64_t duration)
{
	// Determine the time code for this frame before we start rendering.
	string timecode_text = timecode_renderer->get_timecode_text(double(pts_int) / TIMEBASE, frame_num);
	if (display_timecode_on_stdout) {
		printf("Timecode: '%s'\n", timecode_text.c_str());
	}

	// Update Y'CbCr settings for all cards.
	{
		lock_guard<mutex> lock(card_mutex);
		for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
			YCbCrInterpretation *interpretation = &ycbcr_interpretation[card_index];
			input_state.ycbcr_coefficients_auto[card_index] = interpretation->ycbcr_coefficients_auto;
			input_state.ycbcr_coefficients[card_index] = interpretation->ycbcr_coefficients;
			input_state.full_range[card_index] = interpretation->full_range;
		}
	}

	// Get the main chain from the theme, and set its state immediately.
	Theme::Chain theme_main_chain = theme->get_chain(0, pts(), global_flags.width, global_flags.height, input_state);
	EffectChain *chain = theme_main_chain.chain;
	theme_main_chain.setup_chain();
	//theme_main_chain.chain->enable_phase_timing(true);

	// If HDMI/SDI output is active and the user has requested auto mode,
	// its mode overrides the existing Y'CbCr setting for the chain.
	YCbCrLumaCoefficients ycbcr_output_coefficients;
	if (global_flags.ycbcr_auto_coefficients && output_card_index != -1) {
		ycbcr_output_coefficients = cards[output_card_index].output->preferred_ycbcr_coefficients();
	} else {
		ycbcr_output_coefficients = global_flags.ycbcr_rec709_coefficients ? YCBCR_REC_709 : YCBCR_REC_601;
	}

	// TODO: Reduce the duplication against theme.cpp.
	YCbCrFormat output_ycbcr_format;
	output_ycbcr_format.chroma_subsampling_x = 1;
	output_ycbcr_format.chroma_subsampling_y = 1;
	output_ycbcr_format.luma_coefficients = ycbcr_output_coefficients;
	output_ycbcr_format.full_range = false;
	output_ycbcr_format.num_levels = 1 << global_flags.x264_bit_depth;
	chain->change_ycbcr_output_format(output_ycbcr_format);

	// Render main chain. If we're using zerocopy Quick Sync encoding
	// (the default case), we take an extra copy of the created outputs,
	// so that we can display it back to the screen later (it's less memory
	// bandwidth than writing and reading back an RGBA texture, even at 16-bit).
	// Ideally, we'd like to avoid taking copies and just use the main textures
	// for display as well, but they're just views into VA-API memory and must be
	// unmapped during encoding, so we can't use them for display, unfortunately.
	GLuint y_tex, cbcr_full_tex, cbcr_tex;
	GLuint y_copy_tex, cbcr_copy_tex = 0;
	GLuint y_display_tex, cbcr_display_tex;
	GLenum y_type = (global_flags.x264_bit_depth > 8) ? GL_R16 : GL_R8;
	GLenum cbcr_type = (global_flags.x264_bit_depth > 8) ? GL_RG16 : GL_RG8;
	const bool is_zerocopy = video_encoder->is_zerocopy();
	if (is_zerocopy) {
		cbcr_full_tex = resource_pool->create_2d_texture(cbcr_type, global_flags.width, global_flags.height);
		y_copy_tex = resource_pool->create_2d_texture(y_type, global_flags.width, global_flags.height);
		cbcr_copy_tex = resource_pool->create_2d_texture(cbcr_type, global_flags.width / 2, global_flags.height / 2);

		y_display_tex = y_copy_tex;
		cbcr_display_tex = cbcr_copy_tex;

		// y_tex and cbcr_tex will be given by VideoEncoder.
	} else {
		cbcr_full_tex = resource_pool->create_2d_texture(cbcr_type, global_flags.width, global_flags.height);
		y_tex = resource_pool->create_2d_texture(y_type, global_flags.width, global_flags.height);
		cbcr_tex = resource_pool->create_2d_texture(cbcr_type, global_flags.width / 2, global_flags.height / 2);

		y_display_tex = y_tex;
		cbcr_display_tex = cbcr_tex;
	}

	const int64_t av_delay = lrint(global_flags.audio_queue_length_ms * 0.001 * TIMEBASE);  // Corresponds to the delay in ResamplingQueue.
	bool got_frame = video_encoder->begin_frame(pts_int + av_delay, duration, ycbcr_output_coefficients, theme_main_chain.input_frames, &y_tex, &cbcr_tex);
	assert(got_frame);

	GLuint fbo;
	if (is_zerocopy) {
		fbo = resource_pool->create_fbo(y_tex, cbcr_full_tex, y_copy_tex);
	} else {
		fbo = resource_pool->create_fbo(y_tex, cbcr_full_tex);
	}
	check_error();
	chain->render_to_fbo(fbo, global_flags.width, global_flags.height);

	if (display_timecode_in_stream) {
		// Render the timecode on top.
		timecode_renderer->render_timecode(fbo, timecode_text);
	}

	resource_pool->release_fbo(fbo);

	if (is_zerocopy) {
		chroma_subsampler->subsample_chroma(cbcr_full_tex, global_flags.width, global_flags.height, cbcr_tex, cbcr_copy_tex);
	} else {
		chroma_subsampler->subsample_chroma(cbcr_full_tex, global_flags.width, global_flags.height, cbcr_tex);
	}
	if (output_card_index != -1) {
		cards[output_card_index].output->send_frame(y_tex, cbcr_full_tex, ycbcr_output_coefficients, theme_main_chain.input_frames, pts_int, duration);
	}
	resource_pool->release_2d_texture(cbcr_full_tex);

	// Set the right state for the Y' and CbCr textures we use for display.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, y_display_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, cbcr_display_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	RefCountedGLsync fence = video_encoder->end_frame();

	// The live frame pieces the Y'CbCr texture copies back into RGB and displays them.
	// It owns y_display_tex and cbcr_display_tex now (whichever textures they are).
	DisplayFrame live_frame;
	live_frame.chain = display_chain.get();
	live_frame.setup_chain = [this, y_display_tex, cbcr_display_tex]{
		display_input->set_texture_num(0, y_display_tex);
		display_input->set_texture_num(1, cbcr_display_tex);
	};
	live_frame.ready_fence = fence;
	live_frame.input_frames = {};
	live_frame.temp_textures = { y_display_tex, cbcr_display_tex };
	output_channel[OUTPUT_LIVE].output_frame(move(live_frame));

	// Set up preview and any additional channels.
	for (int i = 1; i < theme->get_num_channels() + 2; ++i) {
		DisplayFrame display_frame;
		Theme::Chain chain = theme->get_chain(i, pts(), global_flags.width, global_flags.height, input_state);  // FIXME: dimensions
		display_frame.chain = move(chain.chain);
		display_frame.setup_chain = move(chain.setup_chain);
		display_frame.ready_fence = fence;
		display_frame.input_frames = move(chain.input_frames);
		display_frame.temp_textures = {};
		output_channel[i].output_frame(move(display_frame));
	}
}

void Mixer::audio_thread_func()
{
	pthread_setname_np(pthread_self(), "Mixer_Audio");

	while (!should_quit) {
		AudioTask task;

		{
			unique_lock<mutex> lock(audio_mutex);
			audio_task_queue_changed.wait(lock, [this]{ return should_quit || !audio_task_queue.empty(); });
			if (should_quit) {
				return;
			}
			task = audio_task_queue.front();
			audio_task_queue.pop();
		}

		ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy =
			task.adjust_rate ? ResamplingQueue::ADJUST_RATE : ResamplingQueue::DO_NOT_ADJUST_RATE;
		vector<float> samples_out = audio_mixer->get_output(
			task.frame_timestamp,
			task.num_samples,
			rate_adjustment_policy);

		// Send the samples to the sound card, then add them to the output.
		if (alsa) {
			alsa->write(samples_out);
		}
		if (output_card_index != -1) {
			const int64_t av_delay = lrint(global_flags.audio_queue_length_ms * 0.001 * TIMEBASE);  // Corresponds to the delay in ResamplingQueue.
			cards[output_card_index].output->send_audio(task.pts_int + av_delay, samples_out);
		}
		video_encoder->add_audio(task.pts_int, move(samples_out));
	}
}

void Mixer::release_display_frame(DisplayFrame *frame)
{
	for (GLuint texnum : frame->temp_textures) {
		resource_pool->release_2d_texture(texnum);
	}
	frame->temp_textures.clear();
	frame->ready_fence.reset();
	frame->input_frames.clear();
}

void Mixer::start()
{
	mixer_thread = thread(&Mixer::thread_func, this);
	audio_thread = thread(&Mixer::audio_thread_func, this);
}

void Mixer::quit()
{
	should_quit = true;
	audio_task_queue_changed.notify_one();
	mixer_thread.join();
	audio_thread.join();
#ifdef HAVE_SRT
	if (global_flags.srt_port >= 0) {
		// There's seemingly no other reasonable way to wake up the thread
		// (libsrt's epoll equivalent is busy-waiting).
		int sock = srt_create_socket();
		if (sock != -1) {
			sockaddr_in6 addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin6_family = AF_INET6;
			addr.sin6_addr = IN6ADDR_LOOPBACK_INIT;
			addr.sin6_port = htons(global_flags.srt_port);
			srt_connect(sock, (sockaddr *)&addr, sizeof(addr));
			srt_close(sock);
		}
		srt_thread.join();
	}
#endif
}

void Mixer::transition_clicked(int transition_num)
{
	theme->transition_clicked(transition_num, pts());
}

void Mixer::channel_clicked(int preview_num)
{
	theme->channel_clicked(preview_num);
}

YCbCrInterpretation Mixer::get_input_ycbcr_interpretation(unsigned card_index) const
{
	lock_guard<mutex> lock(card_mutex);
	return ycbcr_interpretation[card_index];
}

void Mixer::set_input_ycbcr_interpretation(unsigned card_index, const YCbCrInterpretation &interpretation)
{
	lock_guard<mutex> lock(card_mutex);
	ycbcr_interpretation[card_index] = interpretation;
}

void Mixer::start_mode_scanning(unsigned card_index)
{
	assert(card_index < MAX_VIDEO_CARDS);
	if (cards[card_index].capture != nullptr) {
		// Inactive card. Should never happen.
		return;
	}
	if (is_mode_scanning[card_index]) {
		return;
	}
	is_mode_scanning[card_index] = true;
	mode_scanlist[card_index].clear();
	for (const auto &mode : cards[card_index].capture->get_available_video_modes()) {
		mode_scanlist[card_index].push_back(mode.first);
	}
	assert(!mode_scanlist[card_index].empty());
	mode_scanlist_index[card_index] = 0;
	cards[card_index].capture->set_video_mode(mode_scanlist[card_index][0]);
	last_mode_scan_change[card_index] = steady_clock::now();
}

map<uint32_t, VideoMode> Mixer::get_available_output_video_modes() const
{
	assert(desired_output_card_index != -1);
	lock_guard<mutex> lock(card_mutex);
	return cards[desired_output_card_index].output->get_available_video_modes();
}

string Mixer::get_ffmpeg_filename(unsigned card_index) const
{
	assert(card_index < MAX_VIDEO_CARDS);
	assert(cards[card_index].type == CardType::FFMPEG_INPUT);
	return ((FFmpegCapture *)(cards[card_index].capture.get()))->get_filename();
}

void Mixer::set_ffmpeg_filename(unsigned card_index, const string &filename) {
	assert(card_index < MAX_VIDEO_CARDS);
	assert(cards[card_index].type == CardType::FFMPEG_INPUT);
	((FFmpegCapture *)(cards[card_index].capture.get()))->change_filename(filename);
}

void Mixer::wait_for_next_frame()
{
	unique_lock<mutex> lock(frame_num_mutex);
	unsigned old_frame_num = frame_num;
	frame_num_updated.wait_for(lock, seconds(1),  // Timeout is just in case.
		[old_frame_num, this]{ return this->frame_num > old_frame_num; });
}

Mixer::OutputChannel::~OutputChannel()
{
	if (has_current_frame) {
		parent->release_display_frame(&current_frame);
	}
	if (has_ready_frame) {
		parent->release_display_frame(&ready_frame);
	}
}

void Mixer::OutputChannel::output_frame(DisplayFrame &&frame)
{
	// Store this frame for display. Remove the ready frame if any
	// (it was seemingly never used).
	{
		lock_guard<mutex> lock(frame_mutex);
		if (has_ready_frame) {
			parent->release_display_frame(&ready_frame);
		}
		ready_frame = move(frame);
		has_ready_frame = true;

		// Call the callbacks under the mutex (they should be short),
		// so that we don't race against a callback removal.
		for (const auto &key_and_callback : new_frame_ready_callbacks) {
			key_and_callback.second();
		}
	}

	// Reduce the number of callbacks by filtering duplicates. The reason
	// why we bother doing this is that Qt seemingly can get into a state
	// where its builds up an essentially unbounded queue of signals,
	// consuming more and more memory, and there's no good way of collapsing
	// user-defined signals or limiting the length of the queue.
	if (transition_names_updated_callback) {
		vector<string> transition_names = global_mixer->get_transition_names();
		bool changed = false;
		if (transition_names.size() != last_transition_names.size()) {
			changed = true;
		} else {
			for (unsigned i = 0; i < transition_names.size(); ++i) {
				if (transition_names[i] != last_transition_names[i]) {
					changed = true;
					break;
				}
			}
		}
		if (changed) {
			transition_names_updated_callback(transition_names);
			last_transition_names = transition_names;
		}
	}
	if (name_updated_callback) {
		string name = global_mixer->get_channel_name(channel);
		if (name != last_name) {
			name_updated_callback(name);
			last_name = name;
		}
	}
	if (color_updated_callback) {
		string color = global_mixer->get_channel_color(channel);
		if (color != last_color) {
			color_updated_callback(color);
			last_color = color;
		}
	}
}

bool Mixer::OutputChannel::get_display_frame(DisplayFrame *frame)
{
	lock_guard<mutex> lock(frame_mutex);
	if (!has_current_frame && !has_ready_frame) {
		return false;
	}

	if (has_current_frame && has_ready_frame) {
		// We have a new ready frame. Toss the current one.
		parent->release_display_frame(&current_frame);
		has_current_frame = false;
	}
	if (has_ready_frame) {
		assert(!has_current_frame);
		current_frame = move(ready_frame);
		ready_frame.ready_fence.reset();  // Drop the refcount.
		ready_frame.input_frames.clear();  // Drop the refcounts.
		has_current_frame = true;
		has_ready_frame = false;
	}

	*frame = current_frame;
	return true;
}

void Mixer::OutputChannel::add_frame_ready_callback(void *key, Mixer::new_frame_ready_callback_t callback)
{
	lock_guard<mutex> lock(frame_mutex);
	new_frame_ready_callbacks[key] = callback;
}

void Mixer::OutputChannel::remove_frame_ready_callback(void *key)
{
	lock_guard<mutex> lock(frame_mutex);
	new_frame_ready_callbacks.erase(key);
}

void Mixer::OutputChannel::set_transition_names_updated_callback(Mixer::transition_names_updated_callback_t callback)
{
	transition_names_updated_callback = callback;
}

void Mixer::OutputChannel::set_name_updated_callback(Mixer::name_updated_callback_t callback)
{
	name_updated_callback = callback;
}

void Mixer::OutputChannel::set_color_updated_callback(Mixer::color_updated_callback_t callback)
{
	color_updated_callback = callback;
}

#ifdef HAVE_SRT
void Mixer::start_srt()
{
	SRTSOCKET sock = srt_create_socket();
	sockaddr_in6 addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(global_flags.srt_port);

	int err = srt_bind(sock, (sockaddr *)&addr, sizeof(addr));
	if (err != 0) {
		fprintf(stderr, "srt_bind: %s\n", srt_getlasterror_str());
		abort();
	}
	err = srt_listen(sock, MAX_VIDEO_CARDS);
	if (err != 0) {
		fprintf(stderr, "srt_listen: %s\n", srt_getlasterror_str());
		abort();
	}

	srt_thread = thread([this, sock] {
		sockaddr_in6 addr;
		for ( ;; ) {
			int sa_len = sizeof(addr);
			int clientsock = srt_accept(sock, (sockaddr *)&addr, &sa_len);
			if (should_quit) {
				if (clientsock != -1) {
					srt_close(clientsock);
				}
				break;
			}
			if (!global_flags.enable_srt) {  // Runtime UI toggle.
				// Perhaps not as good as never listening in the first place,
				// but much simpler to turn on and off.
				srt_close(clientsock);
				continue;
			}
			lock_guard<mutex> lock(hotplug_mutex);
			hotplugged_srt_cards.push_back(clientsock);
		}
		srt_close(sock);
	});
}
#endif

#ifdef HAVE_SRT
void Mixer::update_srt_stats(int srt_sock, Mixer::CaptureCard *card)
{
	SRT_TRACEBSTATS stats;
	srt_bistats(srt_sock, &stats, /*clear=*/0, /*instantaneous=*/1);

	card->metric_srt_uptime_seconds = stats.msTimeStamp * 1e-3;
	card->metric_srt_send_duration_seconds = stats.usSndDurationTotal * 1e-6;
	card->metric_srt_sent_bytes = stats.byteSentTotal;
	card->metric_srt_received_bytes = stats.byteRecvTotal;
	card->metric_srt_sent_packets_normal = stats.pktSentTotal;
	card->metric_srt_received_packets_normal = stats.pktRecvTotal;
	card->metric_srt_sent_packets_lost = stats.pktSndLossTotal;
	card->metric_srt_received_packets_lost = stats.pktRcvLossTotal;
	card->metric_srt_sent_packets_retransmitted = stats.pktRetransTotal;
	card->metric_srt_sent_bytes_retransmitted = stats.byteRetransTotal;
	card->metric_srt_sent_packets_ack = stats.pktSentACKTotal;
	card->metric_srt_received_packets_ack = stats.pktRecvACKTotal;
	card->metric_srt_sent_packets_nak = stats.pktSentNAKTotal;
	card->metric_srt_received_packets_nak = stats.pktRecvNAKTotal;
	card->metric_srt_sent_packets_dropped = stats.pktSndDropTotal;
	card->metric_srt_received_packets_dropped = stats.pktRcvDropTotal;
	card->metric_srt_sent_bytes_dropped = stats.byteSndDropTotal;
	card->metric_srt_received_bytes_dropped = stats.byteRcvDropTotal;
	card->metric_srt_received_packets_undecryptable = stats.pktRcvUndecryptTotal;
	card->metric_srt_received_bytes_undecryptable = stats.byteRcvUndecryptTotal;
	card->metric_srt_filter_sent_packets = stats.pktSndFilterExtraTotal;
	card->metric_srt_filter_received_extra_packets = stats.pktRcvFilterExtraTotal;
	card->metric_srt_filter_received_rebuilt_packets = stats.pktRcvFilterSupplyTotal;
	card->metric_srt_filter_received_lost_packets = stats.pktRcvFilterLossTotal;

	// Gauges.
	card->metric_srt_packet_sending_period_seconds = stats.usPktSndPeriod * 1e-6;
	card->metric_srt_flow_window_packets = stats.pktFlowWindow;
	card->metric_srt_congestion_window_packets = stats.pktCongestionWindow;
	card->metric_srt_flight_size_packets = stats.pktFlightSize;
	card->metric_srt_rtt_seconds = stats.msRTT * 1e-3;
	card->metric_srt_estimated_bandwidth_bits_per_second = stats.mbpsBandwidth * 1e6;
	card->metric_srt_bandwidth_ceiling_bits_per_second = stats.mbpsMaxBW * 1e6;
	card->metric_srt_send_buffer_available_bytes = stats.byteAvailSndBuf;
	card->metric_srt_receive_buffer_available_bytes = stats.byteAvailRcvBuf;
	card->metric_srt_mss_bytes = stats.byteMSS;
	card->metric_srt_sender_unacked_packets = stats.pktSndBuf;
	card->metric_srt_sender_unacked_bytes = stats.byteSndBuf;
	card->metric_srt_sender_unacked_timespan_seconds = stats.msSndBuf * 1e-3;
	card->metric_srt_sender_delivery_delay_seconds = stats.msSndTsbPdDelay * 1e-3;
	card->metric_srt_receiver_unacked_packets = stats.pktRcvBuf;
	card->metric_srt_receiver_unacked_bytes = stats.byteRcvBuf;
	card->metric_srt_receiver_unacked_timespan_seconds = stats.msRcvBuf * 1e-3;
	card->metric_srt_receiver_delivery_delay_seconds = stats.msRcvTsbPdDelay * 1e-3;
}
#endif

string Mixer::description_for_card(unsigned card_index)
{
	CaptureCard *card = &cards[card_index];
	if (card->capture == nullptr) {
		// Should never be called for inactive cards, but OK.
		char buf[256];
		snprintf(buf, sizeof(buf), "Inactive capture card %u", card_index);
		return buf;
	}
	if (card->type != CardType::FFMPEG_INPUT) {
		char buf[256];
		snprintf(buf, sizeof(buf), "Capture card %u (%s)", card_index, card->capture->get_description().c_str());
		return buf;
	}

	// Number (non-SRT) FFmpeg inputs from zero, separately from the capture cards,
	// since it's not too obvious for the user that they are “cards”.
	unsigned ffmpeg_index = 0;
	for (unsigned i = 0; i < card_index; ++i) {
		CaptureCard *other_card = &cards[i];
		if (other_card->type == CardType::FFMPEG_INPUT && !is_srt_card(other_card)) {
			++ffmpeg_index;
		}
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "Video input %u (%s)", ffmpeg_index, card->capture->get_description().c_str());
	return buf;
}

bool Mixer::is_srt_card(const Mixer::CaptureCard *card)
{
#ifdef HAVE_SRT
	if (card->type == CardType::FFMPEG_INPUT) {
		int srt_sock = static_cast<FFmpegCapture *>(card->capture.get())->get_srt_sock();
		return srt_sock != -1;
	}
#endif
	return false;
}

mutex RefCountedGLsync::fence_lock;
