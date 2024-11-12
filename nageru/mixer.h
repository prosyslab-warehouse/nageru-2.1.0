#ifndef _MIXER_H
#define _MIXER_H 1

// The actual video mixer, running in its own separate background thread.

#include <assert.h>
#include <epoxy/gl.h>

#undef Success

#include <stdbool.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <movit/effect.h>
#include <movit/image_format.h>

#include "audio_mixer.h"
#include "bmusb/bmusb.h"
#include "defs.h"
#include "ffmpeg_capture.h"
#include "shared/httpd.h"
#include "input_state.h"
#include "libusb.h"
#include "pbo_frame_allocator.h"
#include "queue_length_policy.h"
#include "ref_counted_frame.h"
#include "shared/ref_counted_gl_sync.h"
#include "theme.h"
#include "shared/timebase.h"
#include "video_encoder.h"
#include "ycbcr_interpretation.h"

class ALSAOutput;
class ChromaSubsampler;
class DeckLinkOutput;
class MJPEGEncoder;
class QSurface;
class QSurfaceFormat;
class TimecodeRenderer;
class v210Converter;

namespace movit {
class Effect;
class EffectChain;
class ResourcePool;
class YCbCrInput;
}  // namespace movit

class Mixer {
public:
	// The surface format is used for offscreen destinations for OpenGL contexts we need.
	Mixer(const QSurfaceFormat &format);
	~Mixer();
	void start();
	void quit();

	void transition_clicked(int transition_num);
	void channel_clicked(int preview_num);

	enum Output {
		OUTPUT_LIVE = 0,
		OUTPUT_PREVIEW,
		OUTPUT_INPUT0,  // 1, 2, 3, up to 15 follow numerically.
		NUM_OUTPUTS = 18
	};

	struct DisplayFrame {
		// The chain for rendering this frame. To render a display frame,
		// first wait for <ready_fence>, then call <setup_chain>
		// to wire up all the inputs, and then finally call
		// chain->render_to_screen() or similar.
		movit::EffectChain *chain;
		std::function<void()> setup_chain;

		// Asserted when all the inputs are ready; you cannot render the chain
		// before this.
		RefCountedGLsync ready_fence;

		// Holds on to all the input frames needed for this display frame,
		// so they are not released while still rendering.
		std::vector<RefCountedFrame> input_frames;

		// Textures that should be released back to the resource pool
		// when this frame disappears, if any.
		// TODO: Refcount these as well?
		std::vector<GLuint> temp_textures;
	};
	// Implicitly frees the previous one if there's a new frame available.
	bool get_display_frame(Output output, DisplayFrame *frame) {
		return output_channel[output].get_display_frame(frame);
	}

	// NOTE: Callbacks will be called with a mutex held, so you should probably
	// not do real work in them.
	typedef std::function<void()> new_frame_ready_callback_t;
	void add_frame_ready_callback(Output output, void *key, new_frame_ready_callback_t callback)
	{
		output_channel[output].add_frame_ready_callback(key, callback);
	}

	void remove_frame_ready_callback(Output output, void *key)
	{
		output_channel[output].remove_frame_ready_callback(key);
	}

	// TODO: Should this really be per-channel? Shouldn't it just be called for e.g. the live output?
	typedef std::function<void(const std::vector<std::string> &)> transition_names_updated_callback_t;
	void set_transition_names_updated_callback(Output output, transition_names_updated_callback_t callback)
	{
		output_channel[output].set_transition_names_updated_callback(callback);
	}

	typedef std::function<void(const std::string &)> name_updated_callback_t;
	void set_name_updated_callback(Output output, name_updated_callback_t callback)
	{
		output_channel[output].set_name_updated_callback(callback);
	}

	typedef std::function<void(const std::string &)> color_updated_callback_t;
	void set_color_updated_callback(Output output, color_updated_callback_t callback)
	{
		output_channel[output].set_color_updated_callback(callback);
	}

	std::vector<std::string> get_transition_names()
	{
		return theme->get_transition_names(pts());
	}

	unsigned get_num_channels() const
	{
		return theme->get_num_channels();
	}

	std::string get_channel_name(unsigned channel) const
	{
		return theme->get_channel_name(channel);
	}

	std::string get_channel_color(unsigned channel) const
	{
		return theme->get_channel_color(channel);
	}

	int map_channel_to_signal(unsigned channel) const
	{
		return theme->map_channel_to_signal(channel);
	}

	int map_signal_to_card(int signal)
	{
		return theme->map_signal_to_card(signal);
	}

	unsigned get_master_clock() const
	{
		return master_clock_channel;
	}

	void set_master_clock(unsigned channel)
	{
		master_clock_channel = channel;
	}

	void set_signal_mapping(int signal, int card)
	{
		return theme->set_signal_mapping(signal, card);
	}

	YCbCrInterpretation get_input_ycbcr_interpretation(unsigned card_index) const;
	void set_input_ycbcr_interpretation(unsigned card_index, const YCbCrInterpretation &interpretation);

	bool get_supports_set_wb(unsigned channel) const
	{
		return theme->get_supports_set_wb(channel);
	}

	void set_wb(unsigned channel, double r, double g, double b) const
	{
		theme->set_wb(channel, r, g, b);
	}

	std::string format_status_line(const std::string &disk_space_left_text, double file_length_seconds)
	{
		return theme->format_status_line(disk_space_left_text, file_length_seconds);
	}

	// Note: You can also get this through the global variable global_audio_mixer.
	AudioMixer *get_audio_mixer() { return audio_mixer.get(); }
	const AudioMixer *get_audio_mixer() const { return audio_mixer.get(); }

	void schedule_cut()
	{
		should_cut = true;
	}

	std::string get_card_description(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].capture->get_description();
	}

	// The difference between this and the previous function is that if a card
	// is used as the current output, get_card_description() will return the
	// fake card that's replacing it for input, whereas this function will return
	// the card's actual name.
	std::string get_output_card_description(unsigned card_index) const {
		assert(card_can_be_used_as_output(card_index));
		assert(card_index < MAX_VIDEO_CARDS);
		if (cards[card_index].parked_capture) {
			return cards[card_index].parked_capture->get_description();
		} else {
			return cards[card_index].capture->get_description();
		}
	}

	bool card_can_be_used_as_output(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].output != nullptr && cards[card_index].capture != nullptr;
	}

	bool card_is_cef(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].type == CardType::CEF_INPUT;
	}

	bool card_is_ffmpeg(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		if (cards[card_index].type != CardType::FFMPEG_INPUT) {
			return false;
		}
#ifdef HAVE_SRT
		// SRT inputs are more like regular inputs than FFmpeg inputs,
		// so show them as such. (This allows the user to right-click
		// to select a different input.)
		return static_cast<FFmpegCapture *>(cards[card_index].capture.get())->get_srt_sock() == -1;
#else
		return true;
#endif
	}

	bool card_is_active(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		std::lock_guard<std::mutex> lock(card_mutex);
		return cards[card_index].capture != nullptr;
	}

	void force_card_active(unsigned card_index)
	{
		// handle_hotplugged_cards() will pick this up.
		std::lock_guard<std::mutex> lock(card_mutex);
		cards[card_index].force_active = true;
	}

	std::map<uint32_t, bmusb::VideoMode> get_available_video_modes(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].capture->get_available_video_modes();
	}

	uint32_t get_current_video_mode(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].capture->get_current_video_mode();
	}

	void set_video_mode(unsigned card_index, uint32_t mode) {
		assert(card_index < MAX_VIDEO_CARDS);
		cards[card_index].capture->set_video_mode(mode);
	}

	void start_mode_scanning(unsigned card_index);

	std::map<uint32_t, std::string> get_available_video_inputs(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].capture->get_available_video_inputs();
	}

	uint32_t get_current_video_input(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].capture->get_current_video_input();
	}

	void set_video_input(unsigned card_index, uint32_t input) {
		assert(card_index < MAX_VIDEO_CARDS);
		cards[card_index].capture->set_video_input(input);
	}

	std::map<uint32_t, std::string> get_available_audio_inputs(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].capture->get_available_audio_inputs();
	}

	uint32_t get_current_audio_input(unsigned card_index) const {
		assert(card_index < MAX_VIDEO_CARDS);
		return cards[card_index].capture->get_current_audio_input();
	}

	void set_audio_input(unsigned card_index, uint32_t input) {
		assert(card_index < MAX_VIDEO_CARDS);
		cards[card_index].capture->set_audio_input(input);
	}

	std::string get_ffmpeg_filename(unsigned card_index) const;

	void set_ffmpeg_filename(unsigned card_index, const std::string &filename);

	void change_x264_bitrate(unsigned rate_kbit) {
		video_encoder->change_x264_bitrate(rate_kbit);
	}

	int get_output_card_index() const {  // -1 = no output, just stream.
		return desired_output_card_index;
	}

	void set_output_card(int card_index) { // -1 = no output, just stream.
		desired_output_card_index = card_index;
	}

	bool get_output_card_is_master() const {
		return output_card_is_master;
	}

	std::map<uint32_t, bmusb::VideoMode> get_available_output_video_modes() const;

	uint32_t get_output_video_mode() const {
		return desired_output_video_mode;
	}

	void set_output_video_mode(uint32_t mode) {
		desired_output_video_mode = mode;
	}

	void set_display_timecode_in_stream(bool enable) {
		display_timecode_in_stream = enable;
	}

	void set_display_timecode_on_stdout(bool enable) {
		display_timecode_on_stdout = enable;
	}

	int64_t get_num_connected_clients() const {
		return httpd.get_num_connected_clients();
	}

	Theme::MenuEntry *get_theme_menu() { return theme->get_theme_menu(); }

	void theme_menu_entry_clicked(int lua_ref) { return theme->theme_menu_entry_clicked(lua_ref); }

	void set_theme_menu_callback(std::function<void()> callback)
	{
		theme->set_theme_menu_callback(callback);
	}

	void wait_for_next_frame();

private:
	struct CaptureCard;

	void configure_card(unsigned card_index, bmusb::CaptureInterface *capture, CardType card_type, DeckLinkOutput *output, bool is_srt_card);
	void set_output_card_internal(int card_index);  // Should only be called from the mixer thread.
	void bm_frame(unsigned card_index, uint16_t timecode,
		bmusb::FrameAllocator::Frame video_frame, size_t video_offset, bmusb::VideoFormat video_format,
		bmusb::FrameAllocator::Frame audio_frame, size_t audio_offset, bmusb::AudioFormat audio_format);
	void upload_texture_for_frame(
	        int field, bmusb::VideoFormat video_format,
		size_t y_offset, size_t cbcr_offset, size_t video_offset,
		PBOFrameAllocator::Userdata *userdata);
	void bm_hotplug_add(libusb_device *dev);
	void bm_hotplug_remove(unsigned card_index);
	void place_rectangle(movit::Effect *resample_effect, movit::Effect *padding_effect, float x0, float y0, float x1, float y1);
	void thread_func();
	void handle_hotplugged_cards();
	void schedule_audio_resampling_tasks(unsigned dropped_frames, int num_samples_per_frame, int length_per_frame, bool is_preroll, std::chrono::steady_clock::time_point frame_timestamp);
	std::string get_timecode_text() const;
	void render_one_frame(int64_t duration);
	void audio_thread_func();
	void release_display_frame(DisplayFrame *frame);
#ifdef HAVE_SRT
	void start_srt();
#endif
	double pts() { return double(pts_int) / TIMEBASE; }
	void trim_queue(CaptureCard *card, size_t safe_queue_length);
	std::pair<std::string, std::string> get_channels_json();
	std::pair<std::string, std::string> get_channel_color_http(unsigned channel_idx);

	HTTPD httpd;
	unsigned num_video_inputs, num_html_inputs = 0;

	QSurface *mixer_surface, *h264_encoder_surface, *decklink_output_surface, *image_update_surface;
	std::unique_ptr<movit::ResourcePool> resource_pool;
	std::unique_ptr<Theme> theme;
	std::atomic<unsigned> audio_source_channel{0};
	std::atomic<int> master_clock_channel{0};  // Gets overridden by <output_card_index> if output_card_is_master == true.
	int output_card_index = -1;  // -1 for none.
	uint32_t output_video_mode = -1;
	bool output_card_is_master = false;  // Only relevant if output_card_index != -1.

	// The mechanics of changing the output card and modes are so intricately connected
	// with the work the mixer thread is doing. Thus, we don't change it directly,
	// we just set this variable instead, which signals to the mixer thread that
	// it should do the change before the next frame. This simplifies locking
	// considerations immensely.
	std::atomic<int> desired_output_card_index{-1};
	std::atomic<uint32_t> desired_output_video_mode{0};

	std::unique_ptr<movit::EffectChain> display_chain;
	std::unique_ptr<ChromaSubsampler> chroma_subsampler;
	std::unique_ptr<v210Converter> v210_converter;
	std::unique_ptr<VideoEncoder> video_encoder;
	std::unique_ptr<MJPEGEncoder> mjpeg_encoder;

	std::unique_ptr<TimecodeRenderer> timecode_renderer;
	std::atomic<bool> display_timecode_in_stream{false};
	std::atomic<bool> display_timecode_on_stdout{false};

	// Effects part of <display_chain>. Owned by <display_chain>.
	movit::YCbCrInput *display_input;

	int64_t pts_int = 0;  // In TIMEBASE units.

	mutable std::mutex frame_num_mutex;
	std::condition_variable frame_num_updated;
	unsigned frame_num = 0;  // Under <frame_num_mutex>.

	// Accumulated errors in number of 1/TIMEBASE audio samples. If OUTPUT_FREQUENCY divided by
	// frame rate is integer, will always stay zero.
	unsigned fractional_samples = 0;

	// Monotonic counter that lets us know which slot was last turned into
	// a fake capture. Used for SRT re-plugging.
	unsigned fake_capture_counter = 0;

	mutable std::mutex card_mutex;
	bool has_bmusb_thread = false;
	struct CaptureCard {
		// If nullptr, the card is inactive, and will be hidden in the UI.
		// Only fake capture cards can be inactive.
		std::unique_ptr<bmusb::CaptureInterface> capture;
		// If true, card must always be active (typically because it's one of the
		// first cards, or because the theme has explicitly asked for it).
		bool force_active = false;
		bool is_fake_capture;
		// If is_fake_capture is true, contains a monotonic timer value for when
		// it was last changed. Otherwise undefined. Used for SRT re-plugging.
		int fake_capture_counter;
		std::string last_srt_stream_id = "<default, matches nothing>";  // Used for SRT re-plugging.
		CardType type;
		std::unique_ptr<DeckLinkOutput> output;

		// CEF only delivers frames when it actually has a change.
		// If we trim the queue for latency reasons, we could thus
		// end up in a situation trimming a frame that was meant to
		// be displayed for a long time, which is really suboptimal.
		// Thus, if we drop the last frame we have, may_have_dropped_last_frame
		// is set to true, and the next starvation event will trigger
		// us requestin a CEF repaint.
		bool is_cef_capture, may_have_dropped_last_frame = false;

		// If this card is used for output (ie., output_card_index points to it),
		// it cannot simultaneously be uesd for capture, so <capture> gets replaced
		// by a FakeCapture. However, since reconstructing the real capture object
		// with all its state can be annoying, it is not being deleted, just stopped
		// and moved here.
		std::unique_ptr<bmusb::CaptureInterface> parked_capture;

		std::unique_ptr<PBOFrameAllocator> frame_allocator;

		// Stuff for the OpenGL context (for texture uploading).
		QSurface *surface = nullptr;

		struct NewFrame {
			RefCountedFrame frame;
			int64_t length;  // In TIMEBASE units.
			bool interlaced;
			unsigned field;  // Which field (0 or 1) of the frame to use. Always 0 for progressive.
			bool texture_uploaded = false;
			unsigned dropped_frames = 0;  // Number of dropped frames before this one.
			std::chrono::steady_clock::time_point received_timestamp = std::chrono::steady_clock::time_point::min();
			movit::RGBTriplet neutral_color{1.0f, 1.0f, 1.0f};

			// Used for MJPEG encoding, and texture upload.
			// width=0 or height=0 means a broken frame, ie., do not upload.
			bmusb::VideoFormat video_format;
			size_t video_offset, y_offset, cbcr_offset;
		};
		std::deque<NewFrame> new_frames;
		std::condition_variable new_frames_changed;  // Set whenever new_frames is changed.
		QueueLengthPolicy queue_length_policy;  // Refers to the "new_frames" queue.

		std::vector<int32_t> new_raw_audio;

		int last_timecode = -1;  // Unwrapped.

		JitterHistory jitter_history;

		// Metrics.
		std::vector<std::pair<std::string, std::string>> labels;
		std::atomic<int64_t> metric_input_received_frames{0};
		std::atomic<int64_t> metric_input_duped_frames{0};
		std::atomic<int64_t> metric_input_dropped_frames_jitter{0};
		std::atomic<int64_t> metric_input_dropped_frames_error{0};
		std::atomic<int64_t> metric_input_resets{0};
		std::atomic<int64_t> metric_input_queue_length_frames{0};

		std::atomic<int64_t> metric_input_has_signal_bool{-1};
		std::atomic<int64_t> metric_input_is_connected_bool{-1};
		std::atomic<int64_t> metric_input_interlaced_bool{-1};
		std::atomic<int64_t> metric_input_width_pixels{-1};
		std::atomic<int64_t> metric_input_height_pixels{-1};
		std::atomic<int64_t> metric_input_frame_rate_nom{-1};
		std::atomic<int64_t> metric_input_frame_rate_den{-1};
		std::atomic<int64_t> metric_input_sample_rate_hz{-1};

		// SRT metrics.
		std::atomic<double> metric_srt_uptime_seconds{0.0 / 0.0};
		std::atomic<double> metric_srt_send_duration_seconds{0.0 / 0.0};
		std::atomic<int64_t> metric_srt_sent_bytes{-1};
		std::atomic<int64_t> metric_srt_received_bytes{-1};
		std::atomic<int64_t> metric_srt_sent_packets_normal{-1};
		std::atomic<int64_t> metric_srt_received_packets_normal{-1};
		std::atomic<int64_t> metric_srt_sent_packets_lost{-1};
		std::atomic<int64_t> metric_srt_received_packets_lost{-1};
		std::atomic<int64_t> metric_srt_sent_packets_retransmitted{-1};
		std::atomic<int64_t> metric_srt_sent_bytes_retransmitted{-1};
		std::atomic<int64_t> metric_srt_sent_packets_ack{-1};
		std::atomic<int64_t> metric_srt_received_packets_ack{-1};
		std::atomic<int64_t> metric_srt_sent_packets_nak{-1};
		std::atomic<int64_t> metric_srt_received_packets_nak{-1};
		std::atomic<int64_t> metric_srt_sent_packets_dropped{-1};
		std::atomic<int64_t> metric_srt_received_packets_dropped{-1};
		std::atomic<int64_t> metric_srt_sent_bytes_dropped{-1};
		std::atomic<int64_t> metric_srt_received_bytes_dropped{-1};
		std::atomic<int64_t> metric_srt_received_packets_undecryptable{-1};
		std::atomic<int64_t> metric_srt_received_bytes_undecryptable{-1};

		std::atomic<int64_t> metric_srt_filter_received_extra_packets{-1};
		std::atomic<int64_t> metric_srt_filter_received_rebuilt_packets{-1};
		std::atomic<int64_t> metric_srt_filter_received_lost_packets{-1};

		std::atomic<double> metric_srt_packet_sending_period_seconds{0.0 / 0.0};
		std::atomic<int64_t> metric_srt_flow_window_packets{-1};
		std::atomic<int64_t> metric_srt_congestion_window_packets{-1};
		std::atomic<int64_t> metric_srt_flight_size_packets{-1};
		std::atomic<double> metric_srt_rtt_seconds{0.0 / 0.0};
		std::atomic<double> metric_srt_estimated_bandwidth_bits_per_second{0.0 / 0.0};
		std::atomic<double> metric_srt_bandwidth_ceiling_bits_per_second{0.0 / 0.0};
		std::atomic<int64_t> metric_srt_send_buffer_available_bytes{-1};
		std::atomic<int64_t> metric_srt_receive_buffer_available_bytes{-1};
		std::atomic<int64_t> metric_srt_mss_bytes{-1};
		std::atomic<int64_t> metric_srt_sender_unacked_packets{-1};
		std::atomic<int64_t> metric_srt_sender_unacked_bytes{-1};
		std::atomic<double> metric_srt_sender_unacked_timespan_seconds{0.0 / 0.0};
		std::atomic<double> metric_srt_sender_delivery_delay_seconds{0.0 / 0.0};
		std::atomic<int64_t> metric_srt_receiver_unacked_packets{-1};
		std::atomic<int64_t> metric_srt_receiver_unacked_bytes{-1};
		std::atomic<double> metric_srt_receiver_unacked_timespan_seconds{0.0 / 0.0};
		std::atomic<double> metric_srt_receiver_delivery_delay_seconds{0.0 / 0.0};
		std::atomic<int64_t> metric_srt_filter_sent_packets{-1};

	};
	JitterHistory output_jitter_history;
	CaptureCard cards[MAX_VIDEO_CARDS];  // Protected by <card_mutex>.
	YCbCrInterpretation ycbcr_interpretation[MAX_VIDEO_CARDS];  // Protected by <card_mutex>.
	movit::RGBTriplet last_received_neutral_color[MAX_VIDEO_CARDS];  // Used by the mixer thread only. Constructor-initialiezd.
	std::unique_ptr<AudioMixer> audio_mixer;  // Same as global_audio_mixer (see audio_mixer.h).
	bool input_card_is_master_clock(unsigned card_index, unsigned master_card_index) const;
	struct OutputFrameInfo {
		int dropped_frames;  // Since last frame.
		int num_samples;  // Audio samples needed for this output frame.
		int64_t frame_duration;  // In TIMEBASE units.
		bool is_preroll;
		std::chrono::steady_clock::time_point frame_timestamp;
	};
	OutputFrameInfo get_one_frame_from_each_card(unsigned master_card_index, bool master_card_is_output, CaptureCard::NewFrame new_frames[MAX_VIDEO_CARDS], bool has_new_frame[MAX_VIDEO_CARDS], std::vector<int32_t> raw_audio[MAX_VIDEO_CARDS]);

#ifdef HAVE_SRT
	void update_srt_stats(int srt_sock, Mixer::CaptureCard *card);
#endif

	std::string description_for_card(unsigned card_index);
	static bool is_srt_card(const CaptureCard *card);

	InputState input_state;

	// Cards we have been noticed about being hotplugged, but haven't tried adding yet.
	// Protected by its own mutex.
	std::mutex hotplug_mutex;
	std::vector<libusb_device *> hotplugged_cards;
#ifdef HAVE_SRT
	std::vector<int> hotplugged_srt_cards;
#endif

	class OutputChannel {
	public:
		~OutputChannel();
		void output_frame(DisplayFrame &&frame);
		bool get_display_frame(DisplayFrame *frame);
		void add_frame_ready_callback(void *key, new_frame_ready_callback_t callback);
		void remove_frame_ready_callback(void *key);
		void set_transition_names_updated_callback(transition_names_updated_callback_t callback);
		void set_name_updated_callback(name_updated_callback_t callback);
		void set_color_updated_callback(color_updated_callback_t callback);

	private:
		friend class Mixer;

		unsigned channel;
		Mixer *parent = nullptr;  // Not owned.
		std::mutex frame_mutex;
		DisplayFrame current_frame, ready_frame;  // protected by <frame_mutex>
		bool has_current_frame = false, has_ready_frame = false;  // protected by <frame_mutex>
		std::map<void *, new_frame_ready_callback_t> new_frame_ready_callbacks;  // protected by <frame_mutex>
		transition_names_updated_callback_t transition_names_updated_callback;
		name_updated_callback_t name_updated_callback;
		color_updated_callback_t color_updated_callback;

		std::vector<std::string> last_transition_names;
		std::string last_name, last_color;
	};
	OutputChannel output_channel[NUM_OUTPUTS];

	std::thread mixer_thread;
	std::thread audio_thread;
#ifdef HAVE_SRT
	std::thread srt_thread;
#endif
	std::atomic<bool> should_quit{false};
	std::atomic<bool> should_cut{false};

	std::unique_ptr<ALSAOutput> alsa;

	struct AudioTask {
		int64_t pts_int;
		int num_samples;
		bool adjust_rate;
		std::chrono::steady_clock::time_point frame_timestamp;
	};
	std::mutex audio_mutex;
	std::condition_variable audio_task_queue_changed;
	std::queue<AudioTask> audio_task_queue;  // Under audio_mutex.

	// For mode scanning.
	bool is_mode_scanning[MAX_VIDEO_CARDS]{ false };
	std::vector<uint32_t> mode_scanlist[MAX_VIDEO_CARDS];
	unsigned mode_scanlist_index[MAX_VIDEO_CARDS]{ 0 };
	std::chrono::steady_clock::time_point last_mode_scan_change[MAX_VIDEO_CARDS];
};

extern Mixer *global_mixer;

#endif  // !defined(_MIXER_H)
