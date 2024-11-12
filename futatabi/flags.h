#ifndef _FLAGS_H
#define _FLAGS_H

#include "defs.h"

#include <string>
#include <unordered_map>

struct Flags {
	int width = 1280, height = 720;
	std::string stream_source;
	std::string working_directory = ".";
	bool slow_down_input = false;
	int interpolation_quality = 2;  // Can be changed in the menus.
	bool interpolation_quality_set = false;
	uint16_t http_port = DEFAULT_HTTPD_PORT;
	double output_framerate = 60000.0 / 1001.0;
	std::string tally_url;
	double cue_in_point_padding_seconds = 0.0;  // Can be changed in the menus.
	bool cue_in_point_padding_set = false;
	double cue_out_point_padding_seconds = 0.0;  // Can be changed in the menus.
	bool cue_out_point_padding_set = false;
	std::string midi_mapping_filename;  // Empty for none.
	std::unordered_map<unsigned, std::string> source_labels;
};
extern Flags global_flags;

// The quality setting that VideoStream was initialized to. The quality cannot
// currently be changed, except turning interpolation completely off, so we compare
// against this to give a warning.
extern int flow_initialized_interpolation_quality;

void usage();
void parse_flags(int argc, char *const argv[]);

#endif  // !defined(_FLAGS_H)
