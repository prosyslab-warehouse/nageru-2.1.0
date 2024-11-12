#include "frame_on_disk.h"

#include "shared/metrics.h"

#include <atomic>
#include <chrono>
#include <assert.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

using namespace std;
using namespace std::chrono;

namespace {

// There can be multiple FrameReader classes, so make all the metrics static.
once_flag frame_metrics_inited;

atomic<int64_t> metric_frame_opened_files{ 0 };
atomic<int64_t> metric_frame_closed_files{ 0 };
atomic<int64_t> metric_frame_read_bytes{ 0 };
atomic<int64_t> metric_frame_read_frames{ 0 };

Summary metric_frame_read_time_seconds;

}  // namespace

FrameReader::FrameReader()
{
	call_once(frame_metrics_inited, [] {
		global_metrics.add("frame_opened_files", &metric_frame_opened_files);
		global_metrics.add("frame_closed_files", &metric_frame_closed_files);
		global_metrics.add("frame_read_bytes", &metric_frame_read_bytes);
		global_metrics.add("frame_read_frames", &metric_frame_read_frames);

		vector<double> quantiles{ 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99 };
		metric_frame_read_time_seconds.init(quantiles, 60.0);
		global_metrics.add("frame_read_time_seconds", &metric_frame_read_time_seconds);
	});
}

FrameReader::~FrameReader()
{
	if (fd != -1) {
		close(fd);
		++metric_frame_closed_files;
	}
}

namespace {

string read_string(int fd, size_t size, off_t offset)
{
	string str;
	str.resize(size);
	size_t str_offset = 0;
	while (str_offset < size) {
		int ret = pread(fd, &str[str_offset], size - str_offset, offset + str_offset);
		if (ret <= 0) {
			perror("pread");
			abort();
		}

		str_offset += ret;
	}
	return str;
}

}  // namespace

FrameReader::Frame FrameReader::read_frame(FrameOnDisk frame, bool read_video, bool read_audio)
{
	assert(read_video || read_audio);
	steady_clock::time_point start = steady_clock::now();

	if (int(frame.filename_idx) != last_filename_idx) {
		if (fd != -1) {
			close(fd);  // Ignore errors.
			++metric_frame_closed_files;
		}

		string filename;
		{
			lock_guard<mutex> lock(frame_mu);
			filename = frame_filenames[frame.filename_idx];
		}

		fd = open(filename.c_str(), O_RDONLY);
		if (fd == -1) {
			perror(filename.c_str());
			abort();
		}

		// We want readahead. (Ignore errors.)
		posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

		last_filename_idx = frame.filename_idx;
		++metric_frame_opened_files;
	}

	Frame ret;
	if (read_video) {
		ret.video = read_string(fd, frame.size, frame.offset);
	}
	if (read_audio) {
		ret.audio = read_string(fd, frame.audio_size, frame.offset + frame.size);
	}

	steady_clock::time_point stop = steady_clock::now();
	metric_frame_read_time_seconds.count_event(duration<double>(stop - start).count());

	metric_frame_read_bytes += frame.size;
	++metric_frame_read_frames;

	return ret;
}
