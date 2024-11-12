#ifndef _V4L_OUTPUT_H
#define _V4L_OUTPUT_H 1

// Video-only V4L2 output. The intended use-case is output into
// v4l2loopback to get into videoconferencing or the likes:
//
//   sudo apt install v4l2loopback-dkms
//   sudo modprobe v4l2loopback video_nr=2 card_label='Nageru loopback' max_width=1280 max_height=720 exclusive_caps=1
//   nageru --v4l-output /dev/video2
//
// Start Nageru before any readers.
//
// Unlike DecklinkOutput, this output does not own the master clock;
// it is entirely unsynchronized, and runs off of the normal master clock.
// It comes in addition to any other output, and is not GUI-controlled.

#include <stddef.h>
#include <stdint.h>

#include <memory>

class V4LOutput {
public:
	V4LOutput(const char *device_path, unsigned width, unsigned height);
	~V4LOutput();

	// Expects NV12 data.
	void send_frame(const uint8_t *data);

private:
	const unsigned width, height;
	const size_t image_size_bytes;
	std::unique_ptr<uint8_t[]> yuv420_buf;
	int video_fd;
};

#endif  // !defined(_V4L_OUTPUT_H)
