#ifndef _HTTPD_H
#define _HTTPD_H

// A class dealing with stream output to HTTP.

#include <assert.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <sys/types.h>
#include <map>
#include <unordered_map>
#include <utility>

extern "C" {
#include <libavutil/rational.h>
}

#include <microhttpd.h>

#include "shared/shared_defs.h"

struct MHD_Connection;
struct MHD_Daemon;

class HTTPD {
public:
	// Returns a pair of content and content-type.
	using EndpointCallback = std::function<std::pair<std::string, std::string>()>;

	HTTPD();
	~HTTPD();

	enum StreamType {
		MAIN_STREAM,
		MULTICAM_STREAM,
		SIPHON_STREAM   // The only one that can have stream_index != 0.
	};
	struct StreamID {
		StreamType type;
		unsigned index;

		bool operator< (const StreamID &other) const {
			if (type != other.type)
				return type < other.type;
			return index < other.index;
		}
		bool operator== (const StreamID &other) const {
			return (type == other.type && index == other.index);
		}
	};

	// Should be called before start() (due to threading issues).
	enum CORSPolicy {
		NO_CORS_POLICY,
		ALLOW_ALL_ORIGINS
	};
	void add_endpoint(const std::string &url, const EndpointCallback &callback, CORSPolicy cors_policy)
	{
		endpoints[url] = Endpoint{ callback, cors_policy };
	}

	void start(int port);
	void stop();
	void set_header(StreamID stream_id, const std::string &data);
	void add_data(StreamID stream_id, const char *buf, size_t size, bool keyframe, int64_t time, AVRational timebase);
	int64_t get_num_connected_clients() const
	{
		return metric_num_connected_clients.load();
	}
	int64_t get_num_connected_multicam_clients() const {
		return metric_num_connected_multicam_clients.load();
	}
	int64_t get_num_connected_siphon_clients(unsigned stream_idx) const {
		assert(stream_idx < MAX_VIDEO_CARDS);
		return metric_num_connected_siphon_clients[stream_idx].load();
	}

private:
	// libmicrohttpd 0.9.71 broke the type of MHD_YES/MHD_NO, causing
	// compilation errors for C++ and undefined behavior for C.
#if MHD_VERSION >= 0x00097002
	using MHD_Result = ::MHD_Result;
#else
	using MHD_Result = int;
#endif

	static MHD_Result answer_to_connection_thunk(void *cls, MHD_Connection *connection,
	                                             const char *url, const char *method,
	                                             const char *version, const char *upload_data,
	                                             size_t *upload_data_size, void **con_cls);

	MHD_Result answer_to_connection(MHD_Connection *connection,
	                                const char *url, const char *method,
	                                const char *version, const char *upload_data,
	                                size_t *upload_data_size, void **con_cls);

	static void free_stream(void *cls);

	class Stream {
	public:
		enum Framing {
			FRAMING_RAW,
			FRAMING_METACUBE
		};
		Stream(HTTPD *parent, Framing framing, StreamID stream_id)
			: parent(parent), framing(framing), stream_id(stream_id) {}

		static ssize_t reader_callback_thunk(void *cls, uint64_t pos, char *buf, size_t max);
		ssize_t reader_callback(uint64_t pos, char *buf, size_t max);

		enum DataType {
			DATA_TYPE_HEADER,
			DATA_TYPE_KEYFRAME,
			DATA_TYPE_OTHER
		};
		void add_data(const char *buf, size_t size, DataType data_type, int64_t time, AVRational timebase);
		void stop();
		HTTPD *get_parent() const { return parent; }
		StreamID get_stream_id() const { return stream_id; }

	private:
		HTTPD *parent;
		Framing framing;

		std::mutex buffer_mutex;
		bool should_quit = false;  // Under <buffer_mutex>.
		std::condition_variable has_buffered_data;
		std::deque<std::string> buffered_data;  // Protected by <buffer_mutex>.
		size_t used_of_buffered_data = 0;  // How many bytes of the first element of <buffered_data> that is already used. Protected by <buffer_mutex>.
		size_t buffered_data_bytes = 0;  // The sum of all size() in buffered_data. Protected by <buffer_mutex>.
		size_t seen_keyframe = false;
		StreamID stream_id;
	};

	void add_data_locked(StreamID stream_id, const char *buf, size_t size, Stream::DataType data_type, int64_t time, AVRational timebase);

	MHD_Daemon *mhd = nullptr;
	std::mutex streams_mutex;
	std::set<Stream *> streams;  // Not owned.
	struct Endpoint {
		EndpointCallback callback;
		CORSPolicy cors_policy;
	};
	std::unordered_map<std::string, Endpoint> endpoints;
	std::map<StreamID, std::string> header;

	// Metrics.
	std::atomic<int64_t> metric_num_connected_clients{0};
	std::atomic<int64_t> metric_num_connected_multicam_clients{0};
	std::atomic<int64_t> metric_num_connected_siphon_clients[MAX_VIDEO_CARDS] {{0}};
};

#endif  // !defined(_HTTPD_H)
