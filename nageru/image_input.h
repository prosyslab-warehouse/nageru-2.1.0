#ifndef _IMAGE_INPUT_H
#define _IMAGE_INPUT_H 1

#include <epoxy/gl.h>
#include <movit/flat_input.h>
#include <stdbool.h>
#include <time.h>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "shared/ref_counted_texture.h"
#include "tweaked_inputs.h"

class QSurface;

// An output that takes its input from a static image, loaded with ffmpeg.
// comes from a single 2D array with chunky pixels. The image is refreshed
// from disk about every second.
class ImageInput : public sRGBSwitchingFlatInput {
public:
	// For loading images.
	// NOTE: You will need to call start_update_thread() yourself, once per program.
	struct Image {
		unsigned width, height;
		UniqueTexture tex;
		timespec last_modified;
	};
	static std::shared_ptr<const Image> load_image(const std::string &filename, const std::string &pathname);

	// Actual members.

	ImageInput();  // Construct an empty input, which can't be used until you call switch_image().
	ImageInput(const std::string &filename);

	std::string effect_type_id() const override { return "ImageInput"; }
	void set_gl_state(GLuint glsl_program_num, const std::string& prefix, unsigned *sampler_num) override;

	// Switch to a different image. The image must be previously loaded using load_image().
	void switch_image(const std::string &pathname);

	std::string get_pathname() const { return pathname; }

	static void start_update_thread(QSurface *surface);
	static void end_update_thread();
	
private:
	std::string pathname;
	std::shared_ptr<const Image> current_image;

	static std::shared_ptr<const Image> load_image_raw(const std::string &pathname);
	static void update_thread_func(QSurface *surface);
	static std::mutex all_images_lock;
	static std::map<std::string, std::shared_ptr<const Image>> all_images;  // Under all_images_lock.

	static std::thread update_thread;
	static std::mutex update_thread_should_quit_mu;
	static bool update_thread_should_quit;  // Under thread_should_quit_mu.
	static std::condition_variable update_thread_should_quit_modified;  // Signals when threads_should_quit is set.
};

#endif // !defined(_IMAGE_INPUT_H)
