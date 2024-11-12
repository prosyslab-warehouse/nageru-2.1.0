#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

using namespace std;

Flags global_flags;
int flow_initialized_interpolation_quality;

// Long options that have no corresponding short option.
enum LongOption {
	OPTION_HELP = 1000,
	OPTION_SLOW_DOWN_INPUT = 1001,
	OPTION_HTTP_PORT = 1002,
	OPTION_TALLY_URL = 1003,
	OPTION_CUE_IN_POINT_PADDING = 1004,
	OPTION_CUE_OUT_POINT_PADDING = 1005,
	OPTION_MIDI_MAPPING = 1006
};

void usage()
{
	fprintf(stderr, "Usage: futatabi [OPTION]... SOURCE_URL\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "      --help                      print usage information\n");
	fprintf(stderr, "  -w, --width                     output width in pixels (default 1280)\n");
	fprintf(stderr, "  -h, --height                    output height in pixels (default 720)\n");
	fprintf(stderr, "  -r, --frame-rate NUM[/NUM]      output frame rate, as a float or fraction\n");
	fprintf(stderr, "                                    (default 60000/1001 ~= 59.94)\n");
	fprintf(stderr, "      --slow-down-input           slow down input to realtime (default on if no\n");
	fprintf(stderr, "                                    source URL given)\n");
	fprintf(stderr, "  -q, --interpolation-quality N   0 = off\n");
	fprintf(stderr, "                                  1 = fastest\n");
	fprintf(stderr, "                                  2 = default (realtime 720p on fast embedded GPUs)\n");
	fprintf(stderr, "                                  3 = good (realtime 720p on GTX 970 or so)\n");
	fprintf(stderr, "                                  4 = best (not realtime on any current GPU)\n");
	fprintf(stderr, "      --cue-in-point-padding SECS   move cue-in N seconds earlier on set\n");
	fprintf(stderr, "      --cue-out-point-padding SECS  move cue-out N seconds later on set\n");
	fprintf(stderr, "  -d, --working-directory DIR     where to store frames and database\n");
	fprintf(stderr, "      --http-port PORT            which port to listen on for output\n");
	fprintf(stderr, "      --tally-url URL             URL to get tally color from (polled every 100 ms)\n");
	fprintf(stderr, "      --midi-mapping=FILE         start with the given MIDI controller mapping\n");
	fprintf(stderr, "  -l  --source-label NUM:LABEL    label source NUM as LABEL, if visible\n");
}

void parse_flags(int argc, char *const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, OPTION_HELP },
		{ "width", required_argument, 0, 'w' },
		{ "height", required_argument, 0, 'h' },
		{ "frame-rate", required_argument, 0, 'r' },
		{ "slow-down-input", no_argument, 0, OPTION_SLOW_DOWN_INPUT },
		{ "interpolation-quality", required_argument, 0, 'q' },
		{ "working-directory", required_argument, 0, 'd' },
		{ "http-port", required_argument, 0, OPTION_HTTP_PORT },
		{ "tally-url", required_argument, 0, OPTION_TALLY_URL },
		{ "cue-in-point-padding", required_argument, 0, OPTION_CUE_IN_POINT_PADDING },
		{ "cue-out-point-padding", required_argument, 0, OPTION_CUE_OUT_POINT_PADDING },
		{ "midi-mapping", required_argument, 0, OPTION_MIDI_MAPPING },
		{ "source-label", required_argument, 0, 'l' },
		{ 0, 0, 0, 0 }
	};
	for (;;) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "w:h:r:q:d:l:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case 'w':
			global_flags.width = atoi(optarg);
			break;
		case 'h':
			global_flags.height = atoi(optarg);
			break;
		case 'r': {
			double num, den;
			if (sscanf(optarg, "%lf/%lf", &num, &den) == 2) {
				global_flags.output_framerate = num / den;
			} else if (sscanf(optarg, "%lf", &num) == 1) {
				global_flags.output_framerate = num;
			} else {
				fprintf(stderr, "Invalid frame rate given (must be on the form N or N/M)\n");
				exit(1);
			}
			break;
		}
		case OPTION_SLOW_DOWN_INPUT:
			global_flags.slow_down_input = true;
			break;
		case 'q':
			global_flags.interpolation_quality = atoi(optarg);
			global_flags.interpolation_quality_set = true;
			break;
		case 'd':
			global_flags.working_directory = optarg;
			break;
		case 'l': {
			int prefix_len;
			unsigned channel_idx;
			if (sscanf(optarg, "%u:%n", &channel_idx, &prefix_len) == 1) {
				const char *label = optarg + prefix_len;
				global_flags.source_labels[channel_idx] = label;
			} else {
				fprintf(stderr, "Invalid source label format (must be on the form NUM:LABEL)\n");
				exit(1);
			}
			break;
		}
		case OPTION_HTTP_PORT:
			global_flags.http_port = atoi(optarg);
			break;
		case OPTION_TALLY_URL:
			global_flags.tally_url = optarg;
			break;
		case OPTION_CUE_IN_POINT_PADDING:
			global_flags.cue_in_point_padding_seconds = atof(optarg);
			global_flags.cue_in_point_padding_set = true;
			break;
		case OPTION_CUE_OUT_POINT_PADDING:
			global_flags.cue_out_point_padding_seconds = atof(optarg);
			global_flags.cue_out_point_padding_set = true;
			break;
		case OPTION_MIDI_MAPPING:
			global_flags.midi_mapping_filename = optarg;
			break;
		case OPTION_HELP:
			usage();
			exit(0);
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			fprintf(stderr, "\n");
			usage();
			exit(1);
		}
	}

	if (global_flags.interpolation_quality < 0 || global_flags.interpolation_quality > 4) {
		fprintf(stderr, "Interpolation quality must be 0, 1, 2, 3 or 4.\n");
		usage();
		exit(1);
	}
	if (global_flags.cue_in_point_padding_seconds < 0.0 ||
	    global_flags.cue_out_point_padding_seconds < 0.0) {
		fprintf(stderr, "Cue point padding cannot be negative.\n");
		usage();
		exit(1);
	}
}
