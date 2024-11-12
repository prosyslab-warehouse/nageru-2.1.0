#ifndef _THEME_H
#define _THEME_H 1

#include <lua.hpp>
#include <movit/effect.h>
#include <movit/flat_input.h>
#include <movit/ycbcr_input.h>
#include <stdbool.h>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "bmusb/bmusb.h"
#include "defs.h"
#include "ref_counted_frame.h"
#include "tweaked_inputs.h"

class Scene;
class CEFCapture;
class FFmpegCapture;
class LiveInputWrapper;
struct InputState;

namespace movit {
class Effect;
class EffectChain;
class ResourcePool;
}  // namespace movit

enum EffectType {
	// LIVE_INPUT_* also covers CEF and video inputs.
	LIVE_INPUT_YCBCR,
	LIVE_INPUT_YCBCR_WITH_DEINTERLACE,
	LIVE_INPUT_YCBCR_PLANAR,
	LIVE_INPUT_BGRA,
	IMAGE_INPUT,

	IDENTITY_EFFECT,
	WHITE_BALANCE_EFFECT,
	AUTO_WHITE_BALANCE_EFFECT,  // Same as WHITE_BALANCE_EFFECT, but sets its value automatically.
	RESAMPLE_EFFECT,
	PADDING_EFFECT,
	INTEGRAL_PADDING_EFFECT,
	OVERLAY_EFFECT,
	RESIZE_EFFECT,
	MULTIPLY_EFFECT,
	MIX_EFFECT,
	LIFT_GAMMA_GAIN_EFFECT,

	NO_EFFECT_TYPE
};

// An EffectBlueprint refers to an Effect before it's being added to the graph.
// It contains enough information to instantiate the effect, including any
// parameters that were set before it was added to the graph. Once it is
// instantiated, it forwards its calls on to the real Effect instead.
struct EffectBlueprint {
	EffectBlueprint(EffectType effect_type) : effect_type(effect_type) {}

	EffectType effect_type;
	std::map<std::string, int> int_parameters;
	std::map<std::string, float> float_parameters;
	std::map<std::string, std::array<float, 3>> vec3_parameters;
	std::map<std::string, std::array<float, 4>> vec4_parameters;

	movit::Effect *effect = nullptr;  // Gets filled out when it's instantiated.
};

// Contains basically the same data as InputState, but does not hold on to
// a reference to the frames. This is important so that we can release them
// without having to wait for Lua's GC.
struct InputStateInfo {
	explicit InputStateInfo(const InputState& input_state);

	unsigned last_width[MAX_VIDEO_CARDS], last_height[MAX_VIDEO_CARDS];
	bool last_interlaced[MAX_VIDEO_CARDS], last_has_signal[MAX_VIDEO_CARDS], last_is_connected[MAX_VIDEO_CARDS];
	unsigned last_frame_rate_nom[MAX_VIDEO_CARDS], last_frame_rate_den[MAX_VIDEO_CARDS];
	bmusb::PixelFormat last_pixel_format[MAX_VIDEO_CARDS];
	bool has_last_subtitle[MAX_VIDEO_CARDS];
	std::string last_subtitle[MAX_VIDEO_CARDS];
};

class Theme {
public:
	Theme(const std::string &filename, const std::vector<std::string> &search_dirs, movit::ResourcePool *resource_pool);
	~Theme();

	struct Chain {
		movit::EffectChain *chain;
		std::function<void()> setup_chain;

		// FRAME_HISTORY frames for each input, in order. Will contain duplicates
		// for non-interlaced inputs.
		std::vector<RefCountedFrame> input_frames;
	};

	Chain get_chain(unsigned num, float t, unsigned width, unsigned height, const InputState &input_state);

	int get_num_channels() const { return num_channels; }
	int map_signal_to_card(int signal_num);
	void set_signal_mapping(int signal_num, int card_idx);
	std::string get_channel_name(unsigned channel);
	int map_channel_to_signal(unsigned channel);
	bool get_supports_set_wb(unsigned channel);
	void set_wb(unsigned channel, float r, float g, float b);
	void set_wb_for_card(int card_idx, float r, float g, float b);
	movit::RGBTriplet get_white_balance_for_card(int card_idx);
	std::string get_channel_color(unsigned channel);

	std::unordered_map<int, movit::RGBTriplet> white_balance_for_card;

	std::vector<std::string> get_transition_names(float t);

	void transition_clicked(int transition_num, float t);
	void channel_clicked(int preview_num);

	movit::ResourcePool *get_resource_pool() const { return resource_pool; }

	// Should be called as part of VideoInput.new() only.
	void register_video_input(FFmpegCapture *capture)
	{
		video_inputs.push_back(capture);
	}

	std::vector<FFmpegCapture *> get_video_inputs() const
	{
		return video_inputs;
	}

#ifdef HAVE_CEF
	// Should be called as part of HTMLInput.new() only.
	void register_html_input(CEFCapture *capture)
	{
		html_inputs.push_back(capture);
	}

	std::vector<CEFCapture *> get_html_inputs() const
	{
		return html_inputs;
	}
#endif

	void register_video_signal_connection(movit::EffectChain *chain, LiveInputWrapper *live_input, FFmpegCapture *capture)
	{
		video_signal_connections[chain].emplace_back(VideoSignalConnection { live_input, capture });
	}

#ifdef HAVE_CEF
	void register_html_signal_connection(movit::EffectChain *chain, LiveInputWrapper *live_input, CEFCapture *capture)
	{
		html_signal_connections[chain].emplace_back(CEFSignalConnection { live_input, capture });
	}
#endif

	struct MenuEntry {
		MenuEntry(const std::string &text, lua_State *L, int lua_ref, unsigned flags)
			: text(text), is_submenu(false), entry{L, lua_ref, flags} {}
		MenuEntry(const std::string &text, std::vector<std::unique_ptr<MenuEntry>> submenu)
			: text(text), is_submenu(true), submenu(std::move(submenu)) {}
		~MenuEntry();

		static constexpr unsigned CHECKABLE = 1;
		static constexpr unsigned CHECKED = 2;

		std::string text;
		bool is_submenu;

		union {
			// is_submenu = false.
			struct {
				lua_State *L;
				int lua_ref;
				unsigned flags;
			} entry;

			// is_submenu = true.
			std::vector<std::unique_ptr<MenuEntry>> submenu;
		};
	};
	MenuEntry *get_theme_menu() { return theme_menu.get(); }  // Can be empty for no menu.
	void theme_menu_entry_clicked(int lua_ref);

	// Will be invoked every time the theme sets a new menu.
	// Is not invoked for a menu that exists at the time of the callback.
	void set_theme_menu_callback(std::function<void()> callback)
	{
		theme_menu_callback = callback;
	}

	std::string format_status_line(const std::string &disk_space_left_text, double file_length_seconds);

	// Signal that the given card is going away and will not be replaced
	// with a fake capture card, so remove all connections to it so that
	// they don't automatically come back on the next frame.
	void remove_card(unsigned card_index);

private:
	void register_globals();
	void register_class(const char *class_name, const luaL_Reg *funcs, EffectType effect_type = NO_EFFECT_TYPE);
	int set_theme_menu(lua_State *L);
	Chain get_chain_from_effect_chain(movit::EffectChain *effect_chain, unsigned num, const InputState &input_state);
	void call_lua_wb_callback(unsigned channel, float r, float g, float b);

	std::string theme_path;

	std::mutex m;
	lua_State *L;  // Protected by <m>.
	const InputState *input_state = nullptr;  // Protected by <m>. Only set temporarily, during chain setup.
	movit::ResourcePool *resource_pool;
	int num_channels = -1;
	bool startup_finished = false;

	std::mutex map_m;
	std::map<int, int> signal_to_card_mapping;  // Protected by <map_m>.

	std::vector<FFmpegCapture *> video_inputs;
	struct VideoSignalConnection {
		LiveInputWrapper *wrapper;
		FFmpegCapture *source;
	};
	std::unordered_map<movit::EffectChain *, std::vector<VideoSignalConnection>>
		 video_signal_connections;
#ifdef HAVE_CEF
	std::vector<CEFCapture *> html_inputs;
	struct CEFSignalConnection {
		LiveInputWrapper *wrapper;
		CEFCapture *source;
	};
	std::unordered_map<movit::EffectChain *, std::vector<CEFSignalConnection>>
		html_signal_connections;
#endif

	std::unique_ptr<MenuEntry> theme_menu;
	std::function<void()> theme_menu_callback;

	std::map<unsigned, std::string> channel_names;  // Set using Nageru.set_channel_name(). Protected by <m>.
	std::map<unsigned, int> channel_signals;  // Set using Nageru.set_channel_signal(). Protected by <m>.
	std::map<unsigned, bool> channel_supports_wb;  // Set using Nageru.set_supports_wb(). Protected by <m>.

	friend class LiveInputWrapper;
	friend class Scene;
	friend int ThemeMenu_set(lua_State *L);
	friend int Nageru_set_channel_name(lua_State *L);
	friend int Nageru_set_num_channels(lua_State *L);
	friend int Nageru_set_channel_signal(lua_State *L);
	friend int Nageru_set_supports_wb(lua_State *L);
};

// LiveInputWrapper is a facade on top of an YCbCrInput, exposed to
// the Lua code. It contains a function (connect_signal()) intended
// to be called during chain setup, that picks out the current frame
// (in the form of a set of textures) from the input state given by
// the mixer, and communicates that state over to the actual YCbCrInput.
class LiveInputWrapper {
public:
	// Note: <override_bounce> is irrelevant for PixelFormat_8BitBGRA.
	LiveInputWrapper(Theme *theme, movit::EffectChain *chain, bmusb::PixelFormat pixel_format, bool override_bounce, bool deinterlace, bool user_connectable);

	bool connect_signal(int signal_num);  // Must be called with the theme's <m> lock held, since it accesses theme->input_state. Returns false on error.
	void connect_card(int signal_num, const InputState &input_state);
	movit::Effect *get_effect() const
	{
		if (deinterlace) {
			return deinterlace_effect;
		} else if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
			return rgba_inputs[0];
		} else {
			return ycbcr_inputs[0];
		}
	}

private:
	Theme *theme;  // Not owned by us.
	bmusb::PixelFormat pixel_format;
	movit::YCbCrFormat input_ycbcr_format;
	std::vector<movit::YCbCrInput *> ycbcr_inputs;  // Multiple ones if deinterlacing. Owned by the chain.
	std::vector<movit::FlatInput *> rgba_inputs;  // Multiple ones if deinterlacing. Owned by the chain.
	movit::Effect *deinterlace_effect = nullptr;  // Owned by the chain.
	bool deinterlace;
	bool user_connectable;
};

// Utility functions used by Scene.
void add_outputs_and_finalize(movit::EffectChain *chain, bool is_main_chain);
Theme *get_theme_updata(lua_State* L);
bool checkbool(lua_State* L, int idx);
std::string checkstdstring(lua_State *L, int index);
movit::Effect *instantiate_effect(movit::EffectChain *chain, EffectType effect_type);
void print_warning(lua_State* L, const char *format, ...);

#endif  // !defined(_THEME_H)
