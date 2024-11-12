#ifndef _VIDEO_STREAM_H
#define _VIDEO_STREAM_H 1

#include <epoxy/gl.h>
#include <stdint.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include "frame_on_disk.h"
#include "jpeg_frame_view.h"
#include "queue_spot_holder.h"
#include "shared/mux.h"
#include "shared/ref_counted_gl_sync.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <movit/effect_chain.h>
#include <movit/mix_effect.h>
#include <movit/ycbcr_input.h>
#include <mutex>
#include <string>
#include <thread>

class ChromaSubsampler;
class DISComputeFlow;
class Interpolate;
class QSurface;
class QSurfaceFormat;
class YCbCrConverter;

class VideoStream {
public:
	VideoStream(AVFormatContext *file_avctx);  // nullptr if output to stream.
	~VideoStream();
	void start();
	void stop();
	void clear_queue();

	// “display_func” is called after the frame has been calculated (if needed)
	// and has gone out to the stream.
	void schedule_original_frame(std::chrono::steady_clock::time_point,
	                             int64_t output_pts, std::function<void()> &&display_func,
	                             QueueSpotHolder &&queue_spot_holder,
	                             FrameOnDisk frame, const std::string &subtitle,
	                             bool include_audio);
	void schedule_faded_frame(std::chrono::steady_clock::time_point, int64_t output_pts,
	                          std::function<void()> &&display_func,
	                          QueueSpotHolder &&queue_spot_holder,
	                          FrameOnDisk frame1, FrameOnDisk frame2,
	                          float fade_alpha, const std::string &subtitle);  // Always no audio.
	void schedule_interpolated_frame(std::chrono::steady_clock::time_point, int64_t output_pts,
	                                 std::function<void(std::shared_ptr<Frame>)> &&display_func,
	                                 QueueSpotHolder &&queue_spot_holder,
	                                 FrameOnDisk frame1, FrameOnDisk frame2,
	                                 float alpha, FrameOnDisk secondary_frame,  // Empty = no secondary (fade) frame.
	                                 float fade_alpha, const std::string &subtitle,
	                                 bool include_audio);
	void schedule_refresh_frame(std::chrono::steady_clock::time_point, int64_t output_pts,
	                            std::function<void()> &&display_func,
	                            QueueSpotHolder &&queue_spot_holder, const std::string &subtitle);  // Always no audio.
	void schedule_silence(std::chrono::steady_clock::time_point, int64_t output_pts,
	                      int64_t length_pts, QueueSpotHolder &&queue_spot_holder);

private:
	struct QueuedFrame;

	FrameReader frame_reader;

	void encode_thread_func();
	std::thread encode_thread;
	std::atomic<bool> should_quit{ false };

	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	void add_silence(int64_t pts, int64_t length_pts);
	void add_audio_or_silence(const QueuedFrame &qf);

	// Allocated at the very start; if we're empty, we start dropping frames
	// (so that we don't build up an infinite interpolation backlog).
	struct InterpolatedFrameResources {
		VideoStream *owner;  // Used only for IFRReleaser, below.

		GLuint input_tex;  // Layered (contains both input frames), Y'CbCr.
		GLuint gray_tex;  // Same, but Y only.
		GLuint input_fbos[2];  // For rendering to the two layers of input_tex.

		// Destination textures and FBO if there is a fade.
		GLuint fade_y_output_tex, fade_cbcr_output_tex;
		GLuint fade_fbo;

		GLuint cb_tex, cr_tex;  // Subsampled, final output.

		GLuint pbo;  // For reading the data back.
		void *pbo_contents;  // Persistently mapped.
	};
	std::mutex queue_lock;
	std::deque<std::unique_ptr<InterpolatedFrameResources>> interpolate_resources;  // Under <queue_lock>.
	static constexpr size_t num_interpolate_slots = 15;  // Should be larger than Player::max_queued_frames, or we risk mass-dropping frames.

	struct IFRReleaser {
		void operator()(InterpolatedFrameResources *ifr) const
		{
			if (ifr != nullptr) {
				std::lock_guard<std::mutex> lock(ifr->owner->queue_lock);
				ifr->owner->interpolate_resources.emplace_back(ifr);
			}
		}
	};
	using BorrowedInterpolatedFrameResources = std::unique_ptr<InterpolatedFrameResources, IFRReleaser>;

	struct QueuedFrame {
		std::chrono::steady_clock::time_point local_pts;

		int64_t output_pts;
		enum Type { ORIGINAL, FADED, INTERPOLATED, FADED_INTERPOLATED, REFRESH, SILENCE } type;

		// For original frames only. Made move-only so we know explicitly
		// we don't copy these ~200 kB files around inadvertedly.
		std::unique_ptr<std::string> encoded_jpeg;

		// For everything except original frames and silence.
		FrameOnDisk frame1;

		// For fades only (including fades against interpolated frames).
		FrameOnDisk secondary_frame;

		// For interpolated frames only.
		FrameOnDisk frame2;
		float alpha;
		BorrowedInterpolatedFrameResources resources;
		RefCountedGLsync fence;  // Set when the interpolated image is read back to the CPU.
		std::chrono::steady_clock::time_point fence_created;
		GLuint flow_tex, output_tex, cbcr_tex;  // Released in the receiving thread; not really used for anything else. flow_tex will typically even be from a previous frame.
		FrameOnDisk id;

		std::function<void()> display_func;  // Called when the image is done decoding.
		std::function<void(std::shared_ptr<Frame>)> display_decoded_func;  // Same, except for INTERPOLATED and FADED_INTERPOLATED.

		std::string subtitle;  // Blank for none.
		std::string exif_data;  // Blank for none.

		// Audio, in stereo interleaved 32-bit PCM. If empty and not of type SILENCE, one frame's worth of silence samples
		// is synthesized.
		std::string audio;

		// For silence frames only.
		int64_t silence_length_pts;

		QueueSpotHolder queue_spot_holder;
	};
	std::deque<QueuedFrame> frame_queue;  // Under <queue_lock>.
	std::condition_variable queue_changed;

	AVFormatContext *avctx;
	std::unique_ptr<Mux> mux;  // To HTTP, or to file.
	std::string stream_mux_header;  // Only used in HTTP.
	bool seen_sync_markers = false;
	bool output_fast_forward;

	std::unique_ptr<YCbCrConverter> ycbcr_converter;
	std::unique_ptr<YCbCrConverter> ycbcr_semiplanar_converter;

	// Frame interpolation.
	std::unique_ptr<DISComputeFlow> compute_flow;
	std::unique_ptr<Interpolate> interpolate, interpolate_no_split;
	std::unique_ptr<ChromaSubsampler> chroma_subsampler;

	// Cached flow computation from previous frame, if any.
	GLuint last_flow_tex = 0;
	FrameOnDisk last_frame1, last_frame2;

	std::string last_frame;
	Mux::WithSubtitles with_subtitles;  // true for streaming, false for export to file.
};

#endif  // !defined(_VIDEO_STREAM_H)
