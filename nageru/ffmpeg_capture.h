#ifndef _FFMPEG_CAPTURE_H
#define _FFMPEG_CAPTURE_H 1

// FFmpegCapture looks much like a capture card, but the frames it spits out
// come from a video in real time, looping. Because it decodes the video using
// FFmpeg (thus the name), this means it can handle a very wide array of video
// formats, and also things like network streaming and V4L capture, but it is
// also significantly less integrated and optimized than the regular capture
// cards. In particular, the frames are always scaled and converted to 8-bit
// RGBA on the CPU before being sent on to the GPU.
//
// Since we don't really know much about the video when building the chains,
// there are some limitations. In particular, frames are always assumed to be
// sRGB even if the video container says something else. We could probably
// try to load the video on startup and pick out the parameters at that point,
// but it would require some more plumbing, and it would also fail if the file
// changes parameters midway, which is allowed in some formats.
//
// You can get out the audio either as decoded or in raw form (Kaeru uses this).
//
// If there's a subtitle track, you can also get out the last subtitle at the
// point of the frame. Note that once we get a video frame, we don't look for
// subtitle, so if subtitles and a frame comes at the same time, you might not
// see the subtitle until the next frame.

#include <assert.h>
#include <stdint.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <movit/effect.h>
#include <movit/ycbcr.h>

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

#include "bmusb/bmusb.h"
#include "shared/ffmpeg_raii.h"
#include "ref_counted_frame.h"
#include "quittable_sleeper.h"

struct AVFormatContext;
struct AVFrame;
struct AVRational;
struct AVPacket;

class FFmpegCapture : public bmusb::CaptureInterface
{
public:
	FFmpegCapture(const std::string &filename, unsigned width, unsigned height);
#ifdef HAVE_SRT
	// Takes ownership of the SRT client socket.
	FFmpegCapture(int srt_sock, const std::string &stream_id);
#endif
	~FFmpegCapture();

	void set_card_index(int card_index)
	{
		this->card_index = card_index;
	}

	int get_card_index() const
	{
		return card_index;
	}

	void rewind()
	{
		std::lock_guard<std::mutex> lock(queue_mu);
		command_queue.push_back(QueuedCommand { QueuedCommand::REWIND });
		producer_thread_should_quit.wakeup();
	}

	void change_rate(double new_rate)
	{
		std::lock_guard<std::mutex> lock(queue_mu);
		command_queue.push_back(QueuedCommand { QueuedCommand::CHANGE_RATE, new_rate });
		producer_thread_should_quit.wakeup();
	}

	std::string get_filename() const
	{
		std::lock_guard<std::mutex> lock(filename_mu);
		return filename;
	}

	void change_filename(const std::string &new_filename)
	{
		std::lock_guard<std::mutex> lock(filename_mu);
		filename = new_filename;
		should_interrupt = true;
	}

	// Will stop the stream even if it's hung on blocking I/O.
	void disconnect()
	{
		should_interrupt = true;
	}

	// CaptureInterface.
	void set_video_frame_allocator(bmusb::FrameAllocator *allocator) override
	{
		video_frame_allocator = allocator;
		if (owned_video_frame_allocator.get() != allocator) {
			owned_video_frame_allocator.reset();
		}
	}

	bmusb::FrameAllocator *get_video_frame_allocator() override
	{
		return video_frame_allocator;
	}

	// Does not take ownership.
	void set_audio_frame_allocator(bmusb::FrameAllocator *allocator) override
	{
		audio_frame_allocator = allocator;
		if (owned_audio_frame_allocator.get() != allocator) {
			owned_audio_frame_allocator.reset();
		}
	}

	bmusb::FrameAllocator *get_audio_frame_allocator() override
	{
		return audio_frame_allocator;
	}

	// FFmpegCapture-specific overload of set_frame_callback that also gives
	// the raw original pts from the video. Negative pts means a dummy frame.
	typedef std::function<void(int64_t video_pts, AVRational video_timebase, int64_t audio_pts, AVRational audio_timebase,
	                           uint16_t timecode,
	                           bmusb::FrameAllocator::Frame video_frame, size_t video_offset, bmusb::VideoFormat video_format,
				   bmusb::FrameAllocator::Frame audio_frame, size_t audio_offset, bmusb::AudioFormat audio_format)>
		frame_callback_t;
	void set_frame_callback(frame_callback_t callback)
	{
		frame_callback = callback;
	}

	void set_frame_callback(bmusb::frame_callback_t callback) override
	{
		frame_callback = std::bind(
			callback,
			std::placeholders::_5,
			std::placeholders::_6,
			std::placeholders::_7,
			std::placeholders::_8,
			std::placeholders::_9,
			std::placeholders::_10,
			std::placeholders::_11);
	}

	// FFmpegCapture-specific callback that gives the raw audio/video.
	typedef std::function<void(const AVPacket *pkt, const AVRational timebase)> packet_callback_t;
	void set_video_callback(packet_callback_t callback)
	{
		video_callback = callback;
	}
	void set_audio_callback(packet_callback_t callback)
	{
		audio_callback = callback;
	}

	// Used to get precise information about the Y'CbCr format used
	// for a given frame. Only valid to call during the frame callback,
	// and only when receiving a frame with pixel format PixelFormat_8BitYCbCrPlanar.
	movit::YCbCrFormat get_current_frame_ycbcr_format() const
	{
		return current_frame_ycbcr_format;
	}

	// Only valid to call during the frame callback.
	std::string get_last_subtitle() const
	{
		return last_subtitle;
	}

	// Same.
	bool get_has_last_subtitle() const
	{
		return has_last_subtitle;
	}

	// Same.
	movit::RGBTriplet get_last_neutral_color() const
	{
		return last_neutral_color;
	}

	void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup) override
	{
		dequeue_init_callback = init;
		dequeue_cleanup_callback = cleanup;
		has_dequeue_callbacks = true;
	}

	void set_card_disconnected_callback(bmusb::card_disconnected_callback_t callback)
	{
		card_disconnected_callback = callback;
	}

	std::string get_description() const override
	{
		return description;
	}

	void configure_card() override;
	void start_bm_capture() override;
	void stop_dequeue_thread() override;
	bool get_disconnected() const override { return disconnected; }  // Only if play_once == true.
	int get_srt_sock() const { return srt_sock; }

	std::map<uint32_t, bmusb::VideoMode> get_available_video_modes() const override;
	void set_video_mode(uint32_t video_mode_id) override {}  // Ignore.
	uint32_t get_current_video_mode() const override { return 0; }

	static constexpr bmusb::PixelFormat PixelFormat_NV12 = static_cast<bmusb::PixelFormat>(100);  // In the private range.
	std::set<bmusb::PixelFormat> get_available_pixel_formats() const override {
		return std::set<bmusb::PixelFormat>{ bmusb::PixelFormat_8BitBGRA, bmusb::PixelFormat_8BitYCbCrPlanar, PixelFormat_NV12 };
	}
	void set_pixel_format(bmusb::PixelFormat pixel_format) override {
		this->pixel_format = pixel_format;
	}	
	bmusb::PixelFormat get_current_pixel_format() const override {
		return pixel_format;
	}

	std::map<uint32_t, std::string> get_available_video_inputs() const override {
		return { { 0, "Auto" } }; }
	void set_video_input(uint32_t video_input_id) override {}  // Ignore.
	uint32_t get_current_video_input() const override { return 0; }

	std::map<uint32_t, std::string> get_available_audio_inputs() const override {
		return { { 0, "Embedded" } };
	}
	void set_audio_input(uint32_t audio_input_id) override {}  // Ignore.
	uint32_t get_current_audio_input() const override { return 0; }

private:
	void producer_thread_func();
	void send_disconnected_frame();
	bool play_video(const std::string &pathname);
	void internal_rewind();

	// Returns true if there was an error.
	bool process_queued_commands(AVFormatContext *format_ctx, const std::string &pathname, timespec last_modified, bool *rewound);

	// Returns nullptr if no frame was decoded (e.g. EOF).
	AVFrameWithDeleter decode_frame(AVFormatContext *format_ctx, AVCodecContext *video_codec_ctx, AVCodecContext *audio_codec_ctx,
	                                const std::string &pathname, int video_stream_index, int audio_stream_index, int subtitle_stream_index,
	                                bmusb::FrameAllocator::Frame *audio_frame, bmusb::AudioFormat *audio_format, int64_t *audio_pts, bool *error);
	void convert_audio(const AVFrame *audio_avframe, bmusb::FrameAllocator::Frame *audio_frame, bmusb::AudioFormat *audio_format);

	bmusb::VideoFormat construct_video_format(const AVFrame *frame, AVRational video_timebase);
	UniqueFrame make_video_frame(const AVFrame *frame, const std::string &pathname, bool *error);

	static int interrupt_cb_thunk(void *opaque);
	int interrupt_cb();

#ifdef HAVE_SRT
	static int read_srt_thunk(void *opaque, uint8_t *buf, int buf_size);
	int read_srt(uint8_t *buf, int buf_size);
#endif

	inline unsigned frame_width(const AVFrame *frame) const;
	inline unsigned frame_height(const AVFrame *frame) const;

	mutable std::mutex filename_mu;
	std::string description, filename;
	int srt_sock = -1;
	uint16_t timecode = 0;
	unsigned width, height;  // 0 means keep input size.
	bmusb::PixelFormat pixel_format = bmusb::PixelFormat_8BitBGRA;
	movit::YCbCrFormat current_frame_ycbcr_format;
	bool running = false;
	int card_index = -1;
	double rate = 1.0;
	bool play_as_fast_as_possible = false;  // Activated iff rate >= 10.0.
	std::atomic<bool> should_interrupt{false};
	bool last_frame_was_connected = true;

	bool has_dequeue_callbacks = false;
	std::function<void()> dequeue_init_callback = nullptr;
	std::function<void()> dequeue_cleanup_callback = nullptr;

	bmusb::card_disconnected_callback_t card_disconnected_callback = nullptr;
	bool play_once = false;  // End thread after playing. Only for SRT, since the socket is ephemeral.
	std::atomic<bool> disconnected{false};

	bmusb::FrameAllocator *video_frame_allocator = nullptr;
	bmusb::FrameAllocator *audio_frame_allocator = nullptr;
	std::unique_ptr<bmusb::FrameAllocator> owned_video_frame_allocator;
	std::unique_ptr<bmusb::FrameAllocator> owned_audio_frame_allocator;
	frame_callback_t frame_callback = nullptr;
	packet_callback_t video_callback = nullptr;
	packet_callback_t audio_callback = nullptr;

	SwsContextWithDeleter sws_ctx;
	int sws_last_width = -1, sws_last_height = -1, sws_last_src_format = -1;
	AVPixelFormat sws_dst_format = AVPixelFormat(-1);  // In practice, always initialized.
	AVRational video_timebase, audio_timebase;
	bool is_mjpeg = false;

	QuittableSleeper producer_thread_should_quit;
	std::thread producer_thread;

	int64_t pts_origin, last_pts;
	std::chrono::steady_clock::time_point start, next_frame_start, last_frame;

	std::mutex queue_mu;
	struct QueuedCommand {
		enum Command { REWIND, CHANGE_RATE } command;
		double new_rate;  // For CHANGE_RATE.
	};
	std::vector<QueuedCommand> command_queue;  // Protected by <queue_mu>.

	// Audio resampler.
	SwrContext *resampler = nullptr;
	AVSampleFormat last_src_format, last_dst_format;
	int64_t last_channel_layout;
	int last_sample_rate;

	// Subtitles (no decoding done, really).
	bool has_last_subtitle = false;
	std::string last_subtitle;

	movit::RGBTriplet last_neutral_color{1.0f, 1.0f, 1.0f};

	// Used for suppressing repeated warnings. Reset when a video starts playing.
	// -1 is strictly speaking outside the range of the enum, but hopefully, it will be alright.
	AVColorSpace last_colorspace = static_cast<AVColorSpace>(-1);
	AVChromaLocation last_chroma_location = static_cast<AVChromaLocation>(-1);
};

#endif  // !defined(_FFMPEG_CAPTURE_H)
