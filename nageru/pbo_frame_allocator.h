#ifndef _PBO_FRAME_ALLOCATOR 
#define _PBO_FRAME_ALLOCATOR 1

#include <epoxy/gl.h>
#include <stdbool.h>
#include <stddef.h>
#include <memory>
#include <mutex>
#include <queue>

#include <movit/ycbcr.h>

#include "bmusb/bmusb.h"
#include "mjpeg_encoder.h"

class MJPEGEncoder;

// An allocator that allocates straight into OpenGL pinned memory.
// Meant for video frames only. We use a queue rather than a stack,
// since we want to maximize pipelineability.
class PBOFrameAllocator : public bmusb::FrameAllocator {
public:
	// Note: You need to have an OpenGL context when calling
	// the constructor.
	PBOFrameAllocator(bmusb::PixelFormat pixel_format,
	                  size_t frame_size,
	                  GLuint width, GLuint height,
	                  unsigned card_index,
	                  MJPEGEncoder *mjpeg_encoder = nullptr,
	                  size_t num_queued_frames = 16,
	                  GLenum buffer = GL_PIXEL_UNPACK_BUFFER_ARB,
	                  GLenum permissions = GL_MAP_WRITE_BIT,
	                  GLenum map_bits = GL_MAP_FLUSH_EXPLICIT_BIT);
	~PBOFrameAllocator() override;
	Frame alloc_frame() override;
	Frame create_frame(size_t width, size_t height, size_t stride) override;
	void release_frame(Frame frame) override;

	// NOTE: Does not check the buffer types; they are just assumed to be compatible.
	void reconfigure(bmusb::PixelFormat pixel_format,
	                 size_t frame_size,
	                 GLuint width, GLuint height,
	                 unsigned card_index,
	                 MJPEGEncoder *mjpeg_encoder = nullptr,
	                 size_t num_queued_frames = 16,
	                 GLenum buffer = GL_PIXEL_UNPACK_BUFFER_ARB,
	                 GLenum permissions = GL_MAP_WRITE_BIT,
	                 GLenum map_bits = GL_MAP_FLUSH_EXPLICIT_BIT);

	struct Userdata {
		GLuint pbo;

		// NOTE: These frames typically go into LiveInputWrapper, which is
		// configured to accept one type of frame only. In other words,
		// the existence of a format field doesn't mean you can set it
		// freely at runtime.
		bmusb::PixelFormat pixel_format;

		// Used only for PixelFormat_8BitYCbCrPlanar.
		movit::YCbCrFormat ycbcr_format;

		// The second set is only used for the second field of interlaced inputs.
		GLuint tex_y[2], tex_cbcr[2];  // For PixelFormat_8BitYCbCr.
		GLuint tex_cb[2], tex_cr[2];  // For PixelFormat_8BitYCbCrPlanar (which also uses tex_y).
		GLuint tex_v210[2], tex_444[2];  // For PixelFormat_10BitYCbCr.
		GLuint tex_rgba[2];  // For PixelFormat_8BitBGRA.
		GLuint last_width[2], last_height[2];
		GLuint last_cbcr_width[2], last_cbcr_height[2];
		GLuint last_v210_width[2];  // PixelFormat_10BitYCbCr.
		bool last_interlaced, last_has_signal, last_is_connected;
		unsigned last_frame_rate_nom, last_frame_rate_den;
		bool has_last_subtitle = false;
		std::string last_subtitle;
		movit::RGBTriplet white_balance{1.0f, 1.0f, 1.0f};

		// These are the source of the “data_copy” member in Frame,
		// used for MJPEG encoding. There are three possibilities:
		//
		//  - MJPEG encoding is not active (at all, or for this specific
		//    card). Then data_copy is nullptr, and what's in here
		//    does not matter at all.
		//  - We can encode directly into VA-API buffers (ie., VA-API
		//    is active, and nothing strange happened wrt. strides);
		//    then va_resources, va_resources_release and va_image
		//    are fetched from MJPEGEncoder at create_frame() and released
		//    back when the frame is uploaded (or would have been).
		//    In this case, data_copy points into the mapped VAImage.
		//  - If not, data_copy points to data_copy_malloc, and is copied
		//    from there into VA-API buffers (by MJPEGEncoder) if needed.
		enum { FROM_MALLOC, FROM_VA_API } data_copy_current_src;
		uint8_t *data_copy_malloc;
		VAResourcePool::VAResources va_resources;
		ReleaseVAResources va_resources_release;

		int generation;
	};

private:
	void init_frame(size_t frame_idx, size_t frame_size, GLuint width, GLuint height, GLenum permissions, GLenum map_bits, int generation);
	void destroy_frame(Frame *frame);

	unsigned card_index;
	MJPEGEncoder *mjpeg_encoder;
	bmusb::PixelFormat pixel_format;
	std::mutex freelist_mutex;
	std::queue<Frame> freelist;
	GLenum buffer;
	std::unique_ptr<Userdata[]> userdata;

	// Used only for reconfigure(), to check whether we can do without.
	size_t frame_size;
	size_t num_queued_frames;
	GLuint width, height;
	GLenum permissions;
	GLenum map_bits;
	int generation = 0;  // Under freelist_mutex.

	struct LingeringGeneration {
		std::unique_ptr<Userdata[]> userdata;
		size_t num_frames_left;
	};
	std::map<int, LingeringGeneration> lingering_generations;
};

#endif  // !defined(_PBO_FRAME_ALLOCATOR)
