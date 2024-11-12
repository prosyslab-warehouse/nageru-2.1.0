#ifndef _MJPEG_ENCODER_H
#define _MJPEG_ENCODER_H 1

#include "defs.h"
#include "shared/ffmpeg_raii.h"
#include "shared/httpd.h"
#include "shared/va_resource_pool.h"
#include "ref_counted_frame.h"

extern "C" {

#include <libavformat/avio.h>

}  // extern "C"

#include <atomic>
#include <bmusb/bmusb.h>
#include <condition_variable>
#include <list>
#include <mutex>
#include <queue>
#include <stdint.h>
#include <string>
#include <thread>

#include <movit/effect.h>
#include <va/va.h>

struct jpeg_compress_struct;
struct VADisplayWithCleanup;
struct VectorDestinationManager;

#define CHECK_VASTATUS(va_status, func)                                 \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr, "%s:%d (%s) failed: %s\n", __func__, __LINE__, func, vaErrorStr(va_status)); \
        exit(1);                                                        \
    }

class MJPEGEncoder {
public:
	MJPEGEncoder(HTTPD *httpd, const std::string &va_display);
	~MJPEGEncoder();
	void stop();
	void upload_frame(int64_t pts, unsigned card_index, RefCountedFrame frame, const bmusb::VideoFormat &video_format, size_t y_offset, size_t cbcr_offset, std::vector<int32_t> audio, const movit::RGBTriplet &white_balance);
	bool using_vaapi() const { return va_dpy != nullptr; }

	bool should_encode_mjpeg_for_card(unsigned card_index);
	VAResourcePool *get_va_pool() const { return va_pool.get(); }

private:
	static constexpr int quality = 90;

	struct QueuedFrame {
		int64_t pts;
		unsigned card_index;
		RefCountedFrame frame;
		bmusb::VideoFormat video_format;
		size_t y_offset, cbcr_offset;
		std::vector<int32_t> audio;
		movit::RGBTriplet white_balance;

		// Only for frames in the process of being encoded by VA-API.
		VAResourcePool::VAResources resources;
		ReleaseVAResources resource_releaser;
	};

	void encoder_thread_func();
	void va_receiver_thread_func();
	void encode_jpeg_va(QueuedFrame &&qf);
	std::vector<uint8_t> encode_jpeg_libjpeg(const QueuedFrame &qf);
	void write_mjpeg_packet(AVFormatContext *avctx, int64_t pts, unsigned stream_index, const uint8_t *jpeg, size_t jpeg_size);
	void write_audio_packet(AVFormatContext *avctx, int64_t pts, unsigned stream_index, const std::vector<int32_t> &audio);
	void init_jpeg(unsigned width, unsigned height, const movit::RGBTriplet &white_balance, VectorDestinationManager *dest, jpeg_compress_struct *cinfo, int y_h_samp_factor, int y_v_samp_factor);
	std::vector<uint8_t> get_jpeg_header(unsigned width, unsigned height, const movit::RGBTriplet &white_balance, int y_h_samp_factor, int y_v_samp_factor, jpeg_compress_struct *cinfo);
	void add_stream(HTTPD::StreamID stream_id);  // Can only be called from the constructor, or the thread owning <streams>.
	void update_siphon_streams();  // Same.
	void create_ffmpeg_context(HTTPD::StreamID stream_id);

	struct WritePacket2Context {
		MJPEGEncoder *mjpeg_encoder;
		HTTPD::StreamID stream_id;
	};
	std::map<HTTPD::StreamID, WritePacket2Context> ffmpeg_contexts;   // Statically set up, so we never need to worry about dangling pointers.
	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(HTTPD::StreamID stream_id, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	std::thread encoder_thread, va_receiver_thread;

	std::mutex mu;
	std::queue<QueuedFrame> frames_to_be_encoded;  // Under mu.
	std::condition_variable any_frames_to_be_encoded;  // Governs changes in both frames_to_be_encoded and frames_under_encoding

	std::queue<QueuedFrame> frames_encoding;  // Under mu. Used for VA-API only.
	std::condition_variable any_frames_encoding;

	struct Stream {
		AVFormatContextWithCloser avctx;
		std::string mux_header;
	};
	std::map<HTTPD::StreamID, Stream> streams;  // Owned by the VA-API receiver thread if VA-API is active, or the encoder thread if not.
	HTTPD *httpd;
	std::atomic<bool> should_quit{false};
	bool running = false;

	std::unique_ptr<VADisplayWithCleanup> va_dpy;
	std::unique_ptr<VAResourcePool> va_pool;

	struct VAKey {
		unsigned width, height, y_h_samp_factor, y_v_samp_factor;
		movit::RGBTriplet white_balance;

		bool operator< (const VAKey &other) const {
			if (width != other.width)
				return width < other.width;
			if (height != other.height)
				return height < other.height;
			if (y_h_samp_factor != other.y_h_samp_factor)
				return y_h_samp_factor < other.y_h_samp_factor;
			if (y_v_samp_factor != other.y_v_samp_factor)
				return y_v_samp_factor < other.y_v_samp_factor;
			if (white_balance.r != other.white_balance.r)
				return white_balance.r < other.white_balance.r;
			if (white_balance.g != other.white_balance.g)
				return white_balance.g < other.white_balance.g;
			return white_balance.b < other.white_balance.b;
		}
	};
	struct VAData {
		std::vector<uint8_t> jpeg_header;
		VAEncPictureParameterBufferJPEG pic_param;
		VAQMatrixBufferJPEG q;
		VAHuffmanTableBufferJPEGBaseline huff;
		VAEncSliceParameterBufferJPEG parms;
	};
	std::map<VAKey, VAData> va_data_for_parameters;
	VAData get_va_data_for_parameters(unsigned width, unsigned height, unsigned y_h_samp_factor, unsigned y_v_samp_factor, const movit::RGBTriplet &white_balance);

	uint8_t *tmp_y, *tmp_cbcr, *tmp_cb, *tmp_cr;  // Private to the encoder thread. Used by the libjpeg backend only.

	std::atomic<int64_t> metric_mjpeg_frames_zero_size_dropped{0};
	std::atomic<int64_t> metric_mjpeg_frames_interlaced_dropped{0};
	std::atomic<int64_t> metric_mjpeg_frames_unsupported_pixel_format_dropped{0};
	std::atomic<int64_t> metric_mjpeg_frames_oversized_dropped{0};
	std::atomic<int64_t> metric_mjpeg_overrun_dropped{0};
	std::atomic<int64_t> metric_mjpeg_overrun_submitted{0};

	friend class PBOFrameAllocator;  // FIXME
};

#endif  // !defined(_MJPEG_ENCODER_H)
