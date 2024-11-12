#ifndef _SHARED_DEFS_H
#define _SHARED_DEFS_H 1

#define OUTPUT_FREQUENCY 48000  // Currently needs to be exactly 48000, since bmusb outputs in that.

#define MUX_OPTS { \
	/* Make seekable .mov files, and keep MP4 muxer from using unlimited amounts of memory. */ \
	{ "movflags", "empty_moov+frag_keyframe+default_base_moof+skip_trailer" }, \
	\
	/* Make for somewhat less bursty stream output when using .mov. */ \
	{ "frag_duration", "125000" }, \
	\
	/* Keep nut muxer from using unlimited amounts of memory. */ \
	{ "write_index", "0" } \
}

// In bytes. Beware, if too small, stream clients will start dropping data.
// For mov, you want this at 10MB or so (for the reason mentioned above),
// but for nut, there's no flushing, so such a large mux buffer would cause
// the output to be very uneven.
#define MUX_BUFFER_SIZE 10485760

#define MAX_VIDEO_CARDS 16  // Only really used by Nageru.

#endif  // !defined(_SHARED_DEFS_H)
