#include "ffmpeg_raii.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

using namespace std;

// AVFormatContext

void avformat_close_input_unique::operator() (AVFormatContext *format_ctx) const
{
	avformat_close_input(&format_ctx);
}

AVFormatContextWithCloser avformat_open_input_unique(
	const char *pathname, const AVInputFormat *fmt,
	AVDictionary **options)
{
	return avformat_open_input_unique(pathname, fmt, options, AVIOInterruptCB{ nullptr, nullptr });
}

AVFormatContextWithCloser avformat_open_input_unique(
	const char *pathname, const AVInputFormat *fmt,
	AVDictionary **options,
	const AVIOInterruptCB &interrupt_cb)
{
	AVFormatContext *format_ctx = avformat_alloc_context();
	format_ctx->interrupt_callback = interrupt_cb;
#ifdef ff_const59
	if (avformat_open_input(&format_ctx, pathname, const_cast<ff_const59 AVInputFormat *>(fmt), options) != 0) {
#else
	if (avformat_open_input(&format_ctx, pathname, fmt, options) != 0) {
#endif
		format_ctx = nullptr;
	}
	return AVFormatContextWithCloser(format_ctx);
}

AVFormatContextWithCloser avformat_open_input_unique(
	int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
	void *opaque, const AVInputFormat *fmt, AVDictionary **options,
	const AVIOInterruptCB &interrupt_cb)
{
	AVFormatContext *format_ctx = avformat_alloc_context();
	format_ctx->interrupt_callback = interrupt_cb;
	constexpr size_t buf_size = 4096;
	unsigned char *buf = (unsigned char *)av_malloc(buf_size);
	format_ctx->pb = avio_alloc_context(buf, buf_size, /*write_flag=*/false, opaque,
		read_packet, /*write_packet=*/nullptr, /*seek=*/nullptr);
#ifdef ff_const59
	if (avformat_open_input(&format_ctx, "", const_cast<ff_const59 AVInputFormat *>(fmt), options) != 0) {
#else
	if (avformat_open_input(&format_ctx, "", fmt, options) != 0) {
#endif
		format_ctx = nullptr;
	}
	return AVFormatContextWithCloser(format_ctx);
}

// AVCodecContext

void avcodec_free_context_unique::operator() (AVCodecContext *codec_ctx) const
{
	avcodec_free_context(&codec_ctx);
}

AVCodecContextWithDeleter avcodec_alloc_context3_unique(const AVCodec *codec)
{
	return AVCodecContextWithDeleter(avcodec_alloc_context3(codec));
}


// AVCodecParameters

void avcodec_parameters_free_unique::operator() (AVCodecParameters *codec_par) const
{
	avcodec_parameters_free(&codec_par);
}

// AVFrame

void av_frame_free_unique::operator() (AVFrame *frame) const
{
	av_frame_free(&frame);
}

AVFrameWithDeleter av_frame_alloc_unique()
{
	return AVFrameWithDeleter(av_frame_alloc());
}

// SwsContext

void sws_free_context_unique::operator() (SwsContext *context) const
{
	sws_freeContext(context);
}
