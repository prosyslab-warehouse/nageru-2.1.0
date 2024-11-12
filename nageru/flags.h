#ifndef _FLAGS_H
#define _FLAGS_H

#include <math.h>

#include <map>
#include <string>
#include <vector>

#include "defs.h"
#include "ycbcr_interpretation.h"

struct Flags {
	int width = 1280, height = 720;
	int min_num_cards = 2;
	int max_num_cards = MAX_VIDEO_CARDS;
	std::string va_display;
	bool fake_cards_audio = false;
	bool uncompressed_video_to_http = false;
	bool x264_video_to_http = false;
	bool x264_video_to_disk = false;  // Disables Quick Sync entirely. Implies x264_video_to_http == true.
	bool x264_separate_disk_encode = false;  // Disables Quick Sync entirely. Implies x264_video_to_disk == true.
	std::vector<std::string> theme_dirs { ".", PREFIX "/share/nageru" };
	std::string recording_dir = ".";
	std::string theme_filename = "theme.lua";
	bool locut_enabled = true;
	bool gain_staging_auto = true;
	float initial_gain_staging_db = 0.0f;
	bool compressor_enabled = true;
	bool limiter_enabled = true;
	bool final_makeup_gain_auto = true;
	bool flush_pbos = true;
	std::string stream_mux_name = DEFAULT_STREAM_MUX_NAME;
	bool stream_coarse_timebase = false;
	std::string stream_audio_codec_name;  // Blank = use the same as for the recording.
	int stream_audio_codec_bitrate = DEFAULT_AUDIO_OUTPUT_BIT_RATE;  // Ignored if stream_audio_codec_name is blank.
	std::string x264_preset;  // Empty will be overridden by X264_DEFAULT_PRESET, unless speedcontrol is set.
	std::string x264_tune = X264_DEFAULT_TUNE;
	bool x264_speedcontrol = false;
	bool x264_speedcontrol_verbose = false;
	int x264_bitrate = -1;  // In kilobit/sec. -1 = not set = DEFAULT_X264_OUTPUT_BIT_RATE.
	float x264_crf = HUGE_VAL;  // From 51 - QP_MAX_SPEC to 51. HUGE_VAL = not set = use x264_bitrate instead.
	int x264_vbv_max_bitrate = -1;  // In kilobits. 0 = no limit, -1 = same as <x264_bitrate> (CBR).
	int x264_vbv_buffer_size = -1;  // In kilobits. 0 = one-frame VBV, -1 = same as <x264_bitrate> (one-second VBV).
	std::vector<std::string> x264_extra_param;  // In “key[,value]” format.

	std::string x264_separate_disk_preset;  // Empty will be overridden by X264_DEFAULT_PRESET, unless speedcontrol is set.
	std::string x264_separate_disk_tune = X264_DEFAULT_TUNE;
	int x264_separate_disk_bitrate = -1;
	float x264_separate_disk_crf = HUGE_VAL;
	std::vector<std::string> x264_separate_disk_extra_param;  // In “key[,value]” format.

	std::string v4l_output_device;  // Empty if none.
	bool enable_alsa_output = true;
	std::map<int, int> default_stream_mapping;
	bool multichannel_mapping_mode = false;  // Implicitly true if input_mapping_filename is nonempty.
	std::string input_mapping_filename;  // Empty for none.
	std::string midi_mapping_filename;  // Empty for none.
	bool default_hdmi_input = false;
	bool print_video_latency = false;
	double audio_queue_length_ms = 100.0;
	bool ycbcr_rec709_coefficients = false;  // Will be overridden by HDMI/SDI output if ycbcr_auto_coefficients == true.
	bool ycbcr_auto_coefficients = true;
	int output_card = -1;
	double output_buffer_frames = 6.0;
	double output_slop_frames = 0.5;
	bool output_card_is_master = true;
	int max_input_queue_frames = 6;
	int http_port = DEFAULT_HTTPD_PORT;
	int srt_port = DEFAULT_SRT_PORT;  // -1 for none.
	bool enable_srt = true;  // UI toggle; not settable from the command line. See also srt_port.
	bool display_timecode_in_stream = false;
	bool display_timecode_on_stdout = false;
	bool enable_quick_cut_keys = false;
	bool ten_bit_input = false;
	bool ten_bit_output = false;  // Implies x264_video_to_disk == true and x264_bit_depth == 10.
	YCbCrInterpretation ycbcr_interpretation[MAX_VIDEO_CARDS];
	bool transcode_video = true;  // Kaeru only.
	bool transcode_audio = true;  // Kaeru only.
	bool enable_audio = true;  // Kaeru only. If false, then transcode_audio is also false.
	int x264_bit_depth = 8;  // Not user-settable.
	bool use_zerocopy = false;  // Not user-settable.
	bool fullscreen = false;
	std::map<unsigned, unsigned> card_to_mjpeg_stream_export;  // If a card is not in the map, it is not exported.
};
extern Flags global_flags;

enum Program {
	PROGRAM_NAGERU,
	PROGRAM_KAERU
};
void usage(Program program);
void parse_flags(Program program, int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
