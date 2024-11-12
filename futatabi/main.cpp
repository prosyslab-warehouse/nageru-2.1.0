#include <arpa/inet.h>
#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <dirent.h>
#include <getopt.h>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "clip_list.h"
#include "defs.h"
#include "flags.h"
#include "frame.pb.h"
#include "frame_on_disk.h"
#include "mainwindow.h"
#include "player.h"
#include "shared/context.h"
#include "shared/disk_space_estimator.h"
#include "shared/ffmpeg_raii.h"
#include "shared/httpd.h"
#include "shared/metrics.h"
#include "shared/post_to_main_thread.h"
#include "shared/ref_counted_gl_sync.h"
#include "shared/timebase.h"
#include "ui_mainwindow.h"
#include "vaapi_jpeg_decoder.h"

#include <QApplication>
#include <QGLFormat>
#include <QProgressDialog>
#include <QSurfaceFormat>
#include <movit/init.h>
#include <movit/util.h>

using namespace std;
using namespace std::chrono;

constexpr char frame_magic[] = "Ftbifrm0";
constexpr size_t frame_magic_len = 8;

mutex RefCountedGLsync::fence_lock;
atomic<bool> should_quit{ false };

int64_t start_pts = -1;

// TODO: Replace by some sort of GUI control, I guess.
int64_t current_pts = 0;

struct FrameFile {
	FILE *fp = nullptr;
	unsigned filename_idx;
	size_t frames_written_so_far = 0;
};
std::map<int, FrameFile> open_frame_files;

mutex frame_mu;
vector<FrameOnDisk> frames[MAX_STREAMS];  // Under frame_mu.
vector<string> frame_filenames;  // Under frame_mu.

atomic<int64_t> metric_received_frames[MAX_STREAMS]{ { 0 } };
Summary metric_received_frame_size_bytes;

namespace {

FrameOnDisk write_frame(int stream_idx, int64_t pts, const uint8_t *data, size_t size, vector<uint32_t> audio, DB *db)
{
	if (open_frame_files.count(stream_idx) == 0) {
		char filename[256];
		snprintf(filename, sizeof(filename), "%s/frames/cam%d-pts%09" PRId64 ".frames",
		         global_flags.working_directory.c_str(), stream_idx, pts);
		FILE *fp = fopen(filename, "wb");
		if (fp == nullptr) {
			perror(filename);
			abort();
		}

		lock_guard<mutex> lock(frame_mu);
		unsigned filename_idx = frame_filenames.size();
		frame_filenames.push_back(filename);
		open_frame_files[stream_idx] = FrameFile{ fp, filename_idx, 0 };
	}

	FrameFile &file = open_frame_files[stream_idx];
	unsigned filename_idx = file.filename_idx;
	string filename;
	{
		lock_guard<mutex> lock(frame_mu);
		filename = frame_filenames[filename_idx];
	}

	FrameHeaderProto hdr;
	hdr.set_stream_idx(stream_idx);
	hdr.set_pts(pts);
	hdr.set_file_size(size);
	hdr.set_audio_size(audio.size() * sizeof(audio[0]));

	string serialized;
	if (!hdr.SerializeToString(&serialized)) {
		fprintf(stderr, "Frame header serialization failed.\n");
		abort();
	}
	uint32_t len = htonl(serialized.size());

	if (fwrite(frame_magic, frame_magic_len, 1, file.fp) != 1) {
		perror("fwrite");
		abort();
	}
	if (fwrite(&len, sizeof(len), 1, file.fp) != 1) {
		perror("fwrite");
		abort();
	}
	if (fwrite(serialized.data(), serialized.size(), 1, file.fp) != 1) {
		perror("fwrite");
		abort();
	}
	off_t offset = ftell(file.fp);
	if (fwrite(data, size, 1, file.fp) != 1) {
		perror("fwrite");
		abort();
	}
	if (audio.size() > 0) {
		if (fwrite(audio.data(), hdr.audio_size(), 1, file.fp) != 1) {
			perror("fwrite");
			exit(1);
		}
	}
	fflush(file.fp);  // No fsync(), though. We can accept losing a few frames.
	global_disk_space_estimator->report_write(filename, 8 + sizeof(len) + serialized.size() + size, pts);

	FrameOnDisk frame;
	frame.pts = pts;
	frame.filename_idx = filename_idx;
	frame.offset = offset;
	frame.size = size;
	frame.audio_size = audio.size() * sizeof(audio[0]);

	{
		lock_guard<mutex> lock(frame_mu);
		assert(stream_idx < MAX_STREAMS);
		frames[stream_idx].push_back(frame);
	}

	if (++file.frames_written_so_far >= FRAMES_PER_FILE) {
		size_t size = ftell(file.fp);

		// Start a new file next time.
		if (fclose(file.fp) != 0) {
			perror("fclose");
			abort();
		}
		open_frame_files.erase(stream_idx);

		// Write information about all frames in the finished file to SQLite.
		// (If we crash before getting to do this, we'll be scanning through
		// the file on next startup, and adding it to the database then.)
		// NOTE: Since we don't fsync(), we could in theory get broken data
		// but with the right size, but it would seem unlikely.
		vector<DB::FrameOnDiskAndStreamIdx> frames_this_file;
		{
			lock_guard<mutex> lock(frame_mu);
			for (size_t stream_idx = 0; stream_idx < MAX_STREAMS; ++stream_idx) {
				for (const FrameOnDisk &frame : frames[stream_idx]) {
					if (frame.filename_idx == filename_idx) {
						frames_this_file.emplace_back(DB::FrameOnDiskAndStreamIdx{ frame, unsigned(stream_idx) });
					}
				}
			}
		}

		const char *basename = filename.c_str();
		while (strchr(basename, '/') != nullptr) {
			basename = strchr(basename, '/') + 1;
		}
		db->store_frame_file(basename, size, frames_this_file);
	}

	return frame;
}

}  // namespace

HTTPD *global_httpd;

void load_existing_frames();
void record_thread_func();

int main(int argc, char **argv)
{
	parse_flags(argc, argv);
	if (optind == argc) {
		global_flags.stream_source = "multicam.mp4";
		global_flags.slow_down_input = true;
	} else if (optind + 1 == argc) {
		global_flags.stream_source = argv[optind];
	} else {
		usage();
		abort();
	}

	string frame_dir = global_flags.working_directory + "/frames";

	if (mkdir(frame_dir.c_str(), 0777) == 0) {
		fprintf(stderr, "%s does not exist, creating it.\n", frame_dir.c_str());
	} else if (errno != EEXIST) {
		perror(global_flags.working_directory.c_str());
		abort();
	}

	avformat_network_init();
	global_metrics.set_prefix("futatabi");
	global_httpd = new HTTPD;
	global_metrics.remove("num_connected_multicam_clients");

	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);

	QSurfaceFormat fmt;
	fmt.setDepthBufferSize(0);
	fmt.setStencilBufferSize(0);
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setMajorVersion(4);
	fmt.setMinorVersion(5);

	// Turn off vsync, since Qt generally gives us at most frame rate
	// (display frequency) / (number of QGLWidgets active).
	fmt.setSwapInterval(0);

	QSurfaceFormat::setDefaultFormat(fmt);

	QGLFormat::setDefaultFormat(QGLFormat::fromSurfaceFormat(fmt));

	QApplication app(argc, argv);
	global_share_widget = new QGLWidget();
	if (!global_share_widget->isValid()) {
		fprintf(stderr, "Failed to initialize OpenGL. Futatabi needs at least OpenGL 4.5 to function properly.\n");
		abort();
	}

	// Initialize Movit.
	{
		QSurface *surface = create_surface();
		QOpenGLContext *context = create_context(surface);
		if (!make_current(context, surface)) {
			printf("oops\n");
			abort();
		}
		CHECK(movit::init_movit(MOVIT_SHADER_DIR, movit::MOVIT_DEBUG_OFF));
		delete_context(context);
		// TODO: Delete the surface, too.
	}

	load_existing_frames();

	for (int stream_idx = 0; stream_idx < MAX_STREAMS; ++stream_idx) {
		if (!frames[stream_idx].empty()) {
			assert(start_pts > frames[stream_idx].back().pts);
		}
	}

	MainWindow main_window;
	main_window.show();

	global_httpd->add_endpoint("/queue_status", bind(&MainWindow::get_queue_status, &main_window), HTTPD::NO_CORS_POLICY);
	global_httpd->start(global_flags.http_port);

	init_jpeg_vaapi();

	thread record_thread(record_thread_func);

	int ret = app.exec();

	should_quit = true;
	record_thread.join();

	return ret;
}

void load_frame_file(const char *filename, const string &basename, unsigned filename_idx, DB *db)
{
	struct stat st;
	if (stat(filename, &st) == -1) {
		perror(filename);
		abort();
	}

	vector<DB::FrameOnDiskAndStreamIdx> all_frames = db->load_frame_file(basename, st.st_size, filename_idx);
	if (!all_frames.empty()) {
		// We already had this cached in the database, so no need to look in the file.
		for (const DB::FrameOnDiskAndStreamIdx &frame : all_frames) {
			if (frame.stream_idx < MAX_STREAMS) {
				frames[frame.stream_idx].push_back(frame.frame);
				start_pts = max(start_pts, frame.frame.pts);
			}
		}
		return;
	}

	FILE *fp = fopen(filename, "rb");
	if (fp == nullptr) {
		perror(filename);
		abort();
	}

	// Find the actual length of the file, since fseek() past the end of the file
	// will succeed without an error.
	if (fseek(fp, 0, SEEK_END) == -1) {
		perror("fseek(SEEK_END)");
		abort();
	}
	off_t file_len = ftell(fp);
	if (fseek(fp, 0, SEEK_SET) == -1) {
		perror("fseek(SEEK_SET)");
		abort();
	}

	size_t magic_offset = 0;
	size_t skipped_bytes = 0;
	while (!feof(fp) && !ferror(fp)) {
		int ch = getc(fp);
		if (ch == -1) {
			break;
		}
		if (ch != frame_magic[magic_offset++]) {
			skipped_bytes += magic_offset;
			magic_offset = 0;
			continue;
		}
		if (magic_offset < frame_magic_len) {
			// Still reading the magic (hopefully).
			continue;
		}

		// OK, found the magic. Try to parse the frame header.
		magic_offset = 0;

		if (skipped_bytes > 0) {
			fprintf(stderr, "WARNING: %s: Skipped %zu garbage bytes in the middle.\n",
			        filename, skipped_bytes);
			skipped_bytes = 0;
		}

		uint32_t len;
		if (fread(&len, sizeof(len), 1, fp) != 1) {
			fprintf(stderr, "WARNING: %s: Short read when getting length.\n", filename);
			break;
		}

		string serialized;
		serialized.resize(ntohl(len));
		if (fread(&serialized[0], serialized.size(), 1, fp) != 1) {
			fprintf(stderr, "WARNING: %s: Short read when reading frame header (%zu bytes).\n", filename, serialized.size());
			break;
		}

		FrameHeaderProto hdr;
		if (!hdr.ParseFromString(serialized)) {
			fprintf(stderr, "WARNING: %s: Corrupted frame header.\n", filename);
			continue;
		}

		FrameOnDisk frame;
		frame.pts = hdr.pts();
		frame.offset = ftell(fp);
		if (frame.offset == -1) {
			fprintf(stderr, "WARNING: %s: ftell() failed (%s).\n", filename, strerror(errno));
			break;
		}
		frame.filename_idx = filename_idx;
		frame.size = hdr.file_size();
		frame.audio_size = hdr.audio_size();

		if (frame.offset + frame.size + frame.audio_size > file_len ||
		    fseek(fp, frame.offset + frame.size + frame.audio_size, SEEK_SET) == -1) {
			fprintf(stderr, "WARNING: %s: Could not seek past frame (probably truncated).\n", filename);
			break;
		}

		if (hdr.stream_idx() >= 0 && hdr.stream_idx() < MAX_STREAMS) {
			frames[hdr.stream_idx()].push_back(frame);
			start_pts = max(start_pts, hdr.pts());
		}
		all_frames.emplace_back(DB::FrameOnDiskAndStreamIdx{ frame, unsigned(hdr.stream_idx()) });
	}

	if (skipped_bytes > 0) {
		fprintf(stderr, "WARNING: %s: Skipped %zu garbage bytes at the end.\n",
		        filename, skipped_bytes);
	}

	off_t size = ftell(fp);
	fclose(fp);

	if (size == -1) {
		fprintf(stderr, "WARNING: %s: ftell() failed (%s).\n", filename, strerror(errno));
		return;
	}

	db->store_frame_file(basename, size, all_frames);
}

void load_existing_frames()
{
	QProgressDialog progress("Scanning frame directory...", "Abort", 0, 1);
	progress.setWindowTitle("Futatabi");
	progress.setWindowModality(Qt::WindowModal);
	progress.setMinimumDuration(1000);
	progress.setMaximum(1);
	progress.setValue(0);

	string frame_dir = global_flags.working_directory + "/frames";
	DIR *dir = opendir(frame_dir.c_str());
	if (dir == nullptr) {
		perror("frames/");
		start_pts = 0;
		return;
	}

	vector<string> frame_basenames;
	for (;;) {
		errno = 0;
		dirent *de = readdir(dir);
		if (de == nullptr) {
			if (errno != 0) {
				perror("readdir");
				abort();
			}
			break;
		}

		if (de->d_type == DT_REG || de->d_type == DT_LNK) {
			string filename = frame_dir + "/" + de->d_name;
			frame_filenames.push_back(filename);
			frame_basenames.push_back(de->d_name);
		}

		if (progress.wasCanceled()) {
			abort();
		}
	}
	closedir(dir);

	progress.setMaximum(frame_filenames.size() + 2);
	progress.setValue(1);

	progress.setLabelText("Opening database...");
	DB db(global_flags.working_directory + "/futatabi.db");

	progress.setLabelText("Reading frame files...");
	progress.setValue(2);

	for (size_t i = 0; i < frame_filenames.size(); ++i) {
		load_frame_file(frame_filenames[i].c_str(), frame_basenames[i], i, &db);
		progress.setValue(i + 3);
		if (progress.wasCanceled()) {
			abort();
		}
	}

	if (start_pts == -1) {
		start_pts = 0;
	} else {
		// Add a gap of one second from the old frames to the new ones.
		start_pts += TIMEBASE;
	}
	current_pts = start_pts;

	for (int stream_idx = 0; stream_idx < MAX_STREAMS; ++stream_idx) {
		sort(frames[stream_idx].begin(), frames[stream_idx].end(),
		     [](const auto &a, const auto &b) { return a.pts < b.pts; });
	}

	db.clean_unused_frame_files(frame_basenames);
}

void record_thread_func()
{
	for (unsigned i = 0; i < MAX_STREAMS; ++i) {
		global_metrics.add("received_frames", { { "stream", to_string(i) } }, &metric_received_frames[i]);
	}
	global_metrics.add("received_frame_size_bytes", &metric_received_frame_size_bytes);

	if (global_flags.stream_source.empty() || global_flags.stream_source == "/dev/null") {
		// Save the user from some repetitive messages.
		return;
	}

	pthread_setname_np(pthread_self(), "ReceiveFrames");

	int64_t pts_offset = 0;  // Needs to be initialized due to a spurious GCC warning.
	DB db(global_flags.working_directory + "/futatabi.db");

	while (!should_quit.load()) {
		auto format_ctx = avformat_open_input_unique(global_flags.stream_source.c_str(), nullptr, nullptr);
		if (format_ctx == nullptr) {
			fprintf(stderr, "%s: Error opening file. Waiting one second and trying again...\n", global_flags.stream_source.c_str());
			sleep(1);
			continue;
		}

		// Match any audio streams to video streams, sequentially.
		vector<int> video_stream_idx, audio_stream_idx;
		for (unsigned i = 0; i < format_ctx->nb_streams; ++i) {
			if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				video_stream_idx.push_back(i);
			} else if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				audio_stream_idx.push_back(i);
			}
		}
		unordered_map<int, int> audio_stream_to_video_stream_idx;
		for (size_t i = 0; i < min(video_stream_idx.size(), audio_stream_idx.size()); ++i) {
			audio_stream_to_video_stream_idx[audio_stream_idx[i]] = video_stream_idx[i];
		}

		vector<uint32_t> pending_audio[MAX_STREAMS];
		int64_t last_pts = -1;
		while (!should_quit.load()) {
			AVPacket pkt;
			unique_ptr<AVPacket, decltype(av_packet_unref) *> pkt_cleanup(
				&pkt, av_packet_unref);
			av_init_packet(&pkt);
			pkt.data = nullptr;
			pkt.size = 0;

			// TODO: Make it possible to abort av_read_frame() (use an interrupt callback);
			// right now, should_quit will be ignored if it's hung on I/O.
			if (av_read_frame(format_ctx.get(), &pkt) != 0) {
				break;
			}

			AVStream *stream = format_ctx->streams[pkt.stream_index];
			if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
			    audio_stream_to_video_stream_idx.count(pkt.stream_index)) {
				if ((pkt.size % (sizeof(uint32_t) * 2)) != 0) {
					fprintf(stderr, "Audio stream %u had a packet of strange length %d, ignoring.\n",
						pkt.stream_index, pkt.size);
				} else {
					// TODO: Endianness?
					const uint32_t *begin = (const uint32_t *)pkt.data;
					const uint32_t *end = (const uint32_t *)(pkt.data + pkt.size);
					pending_audio[audio_stream_to_video_stream_idx[pkt.stream_index]].assign(begin, end);
				}
			}

			if (pkt.stream_index >= MAX_STREAMS ||
			    stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
				continue;
			}

			++metric_received_frames[pkt.stream_index];
			metric_received_frame_size_bytes.count_event(pkt.size);

			// Convert pts to our own timebase.
			AVRational stream_timebase = stream->time_base;
			int64_t pts = av_rescale_q(pkt.pts, stream_timebase, AVRational{ 1, TIMEBASE });

			// Translate offset into our stream.
			if (last_pts == -1) {
				pts_offset = start_pts - pts;
			}
			pts = std::max(pts + pts_offset, start_pts);

			//fprintf(stderr, "Got a frame from camera %d, pts = %ld, size = %d\n",
			//      pkt.stream_index, pts, pkt.size);
			FrameOnDisk frame = write_frame(pkt.stream_index, pts, pkt.data, pkt.size, move(pending_audio[pkt.stream_index]), &db);

			post_to_main_thread([pkt, frame] {
				global_mainwindow->display_frame(pkt.stream_index, frame);
			});

			if (last_pts != -1 && global_flags.slow_down_input) {
				this_thread::sleep_for(microseconds((pts - last_pts) * 1000000 / TIMEBASE));
			}
			last_pts = pts;
			current_pts = pts;
		}

		if (!should_quit.load()) {
			fprintf(stderr, "%s: Hit EOF. Waiting one second and trying again...\n", global_flags.stream_source.c_str());
			sleep(1);
		}

		start_pts = last_pts + TIMEBASE;
	}
}
