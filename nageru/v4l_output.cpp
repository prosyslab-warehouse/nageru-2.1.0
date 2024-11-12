#include "v4l_output.h"

#include <assert.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "shared/memcpy_interleaved.h"

V4LOutput::V4LOutput(const char *device_path, unsigned width, unsigned height)
	: width(width), height(height),
	  image_size_bytes(width * height + (width / 2) * (height / 2) * 2),
	  yuv420_buf(new uint8_t[image_size_bytes])
{
	video_fd = open(device_path, O_WRONLY);
	if (video_fd == -1) {
		perror(device_path);
		exit(1);
	}

	v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = width;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = 0;
	fmt.fmt.pix.sizeimage = image_size_bytes;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	int err = ioctl(video_fd, VIDIOC_S_FMT, &fmt);
	if (err == -1) {
		perror("ioctl(VIDIOC_S_FMT)");
		exit(1);
	}
}

V4LOutput::~V4LOutput()
{
	close(video_fd);
}

void V4LOutput::send_frame(const uint8_t *data)
{
	// Seemingly NV12 isn't a very common format among V4L2 consumers,
	// so we convert from our usual NV12 to YUV420. We get an unneeded
	// memcpy() of the luma data, but hopefully, we'll manage.
	const size_t luma_size = width * height;
	const size_t chroma_size = (width / 2) * (height / 2);
	memcpy(yuv420_buf.get(), data, luma_size);
	memcpy_interleaved(
		yuv420_buf.get() + luma_size,
		yuv420_buf.get() + luma_size + chroma_size,
		data + luma_size, 2 * chroma_size);

	const uint8_t *ptr = yuv420_buf.get();
	size_t bytes_left = image_size_bytes;
	while (bytes_left > 0) {
		int err = write(video_fd, ptr, bytes_left);
		if (err == -1) {
			perror("V4L write");
			exit(1);
		}
		if (err == 0) {
			fprintf(stderr, "WARNING: Short V4L write() (only wrote %zu of %zu bytes), skipping rest of frame.\n",
				image_size_bytes - bytes_left, image_size_bytes);
			return;
		}
		assert(err > 0);
		bytes_left -= err;
		ptr += err;
	}
}
