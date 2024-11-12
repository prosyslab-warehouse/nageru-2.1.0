#include "theme.h"

#include <assert.h>
#include <bmusb/bmusb.h>
#include <epoxy/gl.h>
#include <stdarg.h>
#include <lauxlib.h>
#include <lua.hpp>
#include <movit/deinterlace_effect.h>
#include <movit/effect.h>
#include <movit/effect_chain.h>
#include <movit/image_format.h>
#include <movit/input.h>
#include <movit/lift_gamma_gain_effect.h>
#include <movit/mix_effect.h>
#include <movit/multiply_effect.h>
#include <movit/overlay_effect.h>
#include <movit/padding_effect.h>
#include <movit/resample_effect.h>
#include <movit/resize_effect.h>
#include <movit/util.h>
#include <movit/white_balance_effect.h>
#include <movit/ycbcr.h>
#include <movit/ycbcr_input.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

#include "audio_mixer.h"
#include "defs.h"
#ifdef HAVE_CEF
#include "cef_capture.h"
#endif
#include "ffmpeg_capture.h"
#include "flags.h"
#include "image_input.h"
#include "input_state.h"
#include "lua_utils.h"
#include "mainwindow.h"
#include "pbo_frame_allocator.h"
#include "scene.h"

class Mixer;

namespace movit {
class ResourcePool;
}  // namespace movit

using namespace std;
using namespace movit;

extern Mixer *global_mixer;

constexpr unsigned Theme::MenuEntry::CHECKABLE;
constexpr unsigned Theme::MenuEntry::CHECKED;

Theme *get_theme_updata(lua_State* L)
{
	luaL_checktype(L, lua_upvalueindex(1), LUA_TLIGHTUSERDATA);
	return (Theme *)lua_touserdata(L, lua_upvalueindex(1));
}

void print_warning(lua_State* L, const char *format, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	lua_Debug ar;
	lua_getstack(L, 1, &ar);
	lua_getinfo(L, "nSl", &ar);
	fprintf(stderr, "WARNING: %s:%d: %s", ar.source, ar.currentline, buf);
}

int ThemeMenu_set(lua_State *L)
{
	Theme *theme = get_theme_updata(L);
	return theme->set_theme_menu(L);
}

InputStateInfo::InputStateInfo(const InputState &input_state)
{
	for (unsigned signal_num = 0; signal_num < MAX_VIDEO_CARDS; ++signal_num) {
		BufferedFrame frame = input_state.buffered_frames[signal_num][0];
		if (frame.frame == nullptr) {
			last_width[signal_num] = last_height[signal_num] = 0;
			last_interlaced[signal_num] = false;
			last_has_signal[signal_num] = false;
			last_is_connected[signal_num] = false;
			continue;
		}
		const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)frame.frame->userdata;
		last_width[signal_num] = userdata->last_width[frame.field_number];
		last_height[signal_num] = userdata->last_height[frame.field_number];
		last_interlaced[signal_num] = userdata->last_interlaced;
		last_has_signal[signal_num] = userdata->last_has_signal;
		last_is_connected[signal_num] = userdata->last_is_connected;
		last_frame_rate_nom[signal_num] = userdata->last_frame_rate_nom;
		last_frame_rate_den[signal_num] = userdata->last_frame_rate_den;
		last_pixel_format[signal_num] = userdata->pixel_format;
		has_last_subtitle[signal_num] = userdata->has_last_subtitle;
		last_subtitle[signal_num] = userdata->last_subtitle;
	}
}

// An effect that does nothing.
class IdentityEffect : public Effect {
public:
        IdentityEffect() {}
        string effect_type_id() const override { return "IdentityEffect"; }
        string output_fragment_shader() override { return read_file("identity.frag"); }
};

Effect *instantiate_effect(EffectChain *chain, EffectType effect_type)
{
	switch (effect_type) {
	case IDENTITY_EFFECT:
		return new IdentityEffect;
	case WHITE_BALANCE_EFFECT:
	case AUTO_WHITE_BALANCE_EFFECT:
		return new WhiteBalanceEffect;
	case RESAMPLE_EFFECT:
		return new ResampleEffect;
	case PADDING_EFFECT:
		return new PaddingEffect;
	case INTEGRAL_PADDING_EFFECT:
		return new IntegralPaddingEffect;
	case OVERLAY_EFFECT:
		return new OverlayEffect;
	case RESIZE_EFFECT:
		return new ResizeEffect;
	case MULTIPLY_EFFECT:
		return new MultiplyEffect;
	case MIX_EFFECT:
		return new MixEffect;
	case LIFT_GAMMA_GAIN_EFFECT:
		return new LiftGammaGainEffect;
	default:
		fprintf(stderr, "Unhandled effect type %d\n", effect_type);
		abort();
	}
}

namespace {

Effect *get_effect_from_blueprint(EffectChain *chain, lua_State *L, int idx)
{
	EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, idx, "EffectBlueprint");
	if (blueprint->effect != nullptr) {
		luaL_error(L, "An effect can currently only be added to one chain.\n");
	}

	Effect *effect = instantiate_effect(chain, blueprint->effect_type);

	// Set the parameters that were deferred earlier.
	for (const auto &kv : blueprint->int_parameters) {
		if (!effect->set_int(kv.first, kv.second)) {
			luaL_error(L, "Effect refused set_int(\"%s\", %d) (invalid key?)", kv.first.c_str(), kv.second);
		}
	}
	for (const auto &kv : blueprint->float_parameters) {
		if (!effect->set_float(kv.first, kv.second)) {
			luaL_error(L, "Effect refused set_float(\"%s\", %f) (invalid key?)", kv.first.c_str(), kv.second);
		}
	}
	for (const auto &kv : blueprint->vec3_parameters) {
		if (!effect->set_vec3(kv.first, kv.second.data())) {
			luaL_error(L, "Effect refused set_vec3(\"%s\", %f, %f, %f) (invalid key?)", kv.first.c_str(),
				kv.second[0], kv.second[1], kv.second[2]);
		}
	}
	for (const auto &kv : blueprint->vec4_parameters) {
		if (!effect->set_vec4(kv.first, kv.second.data())) {
			luaL_error(L, "Effect refused set_vec4(\"%s\", %f, %f, %f, %f) (invalid key?)", kv.first.c_str(),
				kv.second[0], kv.second[1], kv.second[2], kv.second[3]);
		}
	}
	blueprint->effect = effect;
	return effect;
}

InputStateInfo *get_input_state_info(lua_State *L, int idx)
{
	if (luaL_testudata(L, idx, "InputStateInfo")) {
		return (InputStateInfo *)lua_touserdata(L, idx);
	}
	luaL_error(L, "Error: Index #%d was not InputStateInfo\n", idx);
	return nullptr;
}

}  // namespace

bool checkbool(lua_State* L, int idx)
{
	luaL_checktype(L, idx, LUA_TBOOLEAN);
	return lua_toboolean(L, idx);
}

string checkstdstring(lua_State *L, int index)
{
	size_t len;
	const char* cstr = lua_tolstring(L, index, &len);
	return string(cstr, len);
}

namespace {

int Scene_new(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	Theme *theme = get_theme_updata(L);
	int aspect_w = luaL_checknumber(L, 1);
	int aspect_h = luaL_checknumber(L, 2);

	return wrap_lua_object<Scene>(L, "Scene", theme, aspect_w, aspect_h);
}

int Scene_gc(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	Scene *chain = (Scene *)luaL_checkudata(L, 1, "Scene");
	chain->~Scene();
	return 0;
}

}  // namespace

void add_outputs_and_finalize(EffectChain *chain, bool is_main_chain)
{
	// Add outputs as needed.
	// NOTE: If you change any details about the output format, you will need to
	// also update what's given to the muxer (HTTPD::Mux constructor) and
	// what's put in the H.264 stream (sps_rbsp()).
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_REC_709;

	// Output gamma is tricky. We should output Rec. 709 for TV, except that
	// we expect to run with web players and others that don't really care and
	// just output with no conversion. So that means we'll need to output sRGB,
	// even though H.264 has no setting for that (we use “unspecified”).
	inout_format.gamma_curve = GAMMA_sRGB;

	if (is_main_chain) {
		YCbCrFormat output_ycbcr_format;
		// We actually output 4:2:0 and/or 4:2:2 in the end, but chroma subsampling
		// happens in a pass not run by Movit (see ChromaSubsampler::subsample_chroma()).
		output_ycbcr_format.chroma_subsampling_x = 1;
		output_ycbcr_format.chroma_subsampling_y = 1;

		// This will be overridden if HDMI/SDI output is in force.
		if (global_flags.ycbcr_rec709_coefficients) {
			output_ycbcr_format.luma_coefficients = YCBCR_REC_709;
		} else {
			output_ycbcr_format.luma_coefficients = YCBCR_REC_601;
		}

		output_ycbcr_format.full_range = false;
		output_ycbcr_format.num_levels = 1 << global_flags.x264_bit_depth;

		GLenum type = global_flags.x264_bit_depth > 8 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

		chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, output_ycbcr_format, YCBCR_OUTPUT_SPLIT_Y_AND_CBCR, type);

		// If we're using zerocopy video encoding (so the destination
		// Y texture is owned by VA-API and will be unavailable for
		// display), add a copy, where we'll only be using the Y component.
		if (global_flags.use_zerocopy) {
			chain->add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, output_ycbcr_format, YCBCR_OUTPUT_INTERLEAVED, type);  // Add a copy where we'll only be using the Y component.
		}
		chain->set_dither_bits(global_flags.x264_bit_depth > 8 ? 16 : 8);
		chain->set_output_origin(OUTPUT_ORIGIN_TOP_LEFT);
	} else {
		chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
	}

	chain->finalize();
}

namespace {

int EffectChain_new(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	Theme *theme = get_theme_updata(L);
	int aspect_w = luaL_checknumber(L, 1);
	int aspect_h = luaL_checknumber(L, 2);

	return wrap_lua_object<EffectChain>(L, "EffectChain", aspect_w, aspect_h, theme->get_resource_pool());
}

int EffectChain_gc(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	chain->~EffectChain();
	return 0;
}

int EffectChain_add_live_input(lua_State* L)
{
	assert(lua_gettop(L) == 3);
	Theme *theme = get_theme_updata(L);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	bool override_bounce = checkbool(L, 2);
	bool deinterlace = checkbool(L, 3);
	bmusb::PixelFormat pixel_format = global_flags.ten_bit_input ? bmusb::PixelFormat_10BitYCbCr : bmusb::PixelFormat_8BitYCbCr;

	// Needs to be nonowned to match add_video_input (see below).
	return wrap_lua_object_nonowned<LiveInputWrapper>(L, "LiveInputWrapper", theme, chain, pixel_format, override_bounce, deinterlace, /*user_connectable=*/true);
}

int EffectChain_add_video_input(lua_State* L)
{
	assert(lua_gettop(L) == 3);
	Theme *theme = get_theme_updata(L);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	FFmpegCapture **capture = (FFmpegCapture **)luaL_checkudata(L, 2, "VideoInput");
	bool deinterlace = checkbool(L, 3);

	// These need to be nonowned, so that the LiveInputWrapper still exists
	// and can feed frames to the right EffectChain even if the Lua code
	// doesn't care about the object anymore. (If we change this, we'd need
	// to also unregister the signal connection on __gc.)
	int ret = wrap_lua_object_nonowned<LiveInputWrapper>(
		L, "LiveInputWrapper", theme, chain, (*capture)->get_current_pixel_format(),
		/*override_bounce=*/false, deinterlace, /*user_connectable=*/false);
	if (ret == 1) {
		Theme *theme = get_theme_updata(L);
		LiveInputWrapper **live_input = (LiveInputWrapper **)lua_touserdata(L, -1);
		theme->register_video_signal_connection(chain, *live_input, *capture);
	}
	return ret;
}

#ifdef HAVE_CEF
int EffectChain_add_html_input(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	Theme *theme = get_theme_updata(L);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	CEFCapture **capture = (CEFCapture **)luaL_checkudata(L, 2, "HTMLInput");

	// These need to be nonowned, so that the LiveInputWrapper still exists
	// and can feed frames to the right EffectChain even if the Lua code
	// doesn't care about the object anymore. (If we change this, we'd need
	// to also unregister the signal connection on __gc.)
	int ret = wrap_lua_object_nonowned<LiveInputWrapper>(
		L, "LiveInputWrapper", theme, chain, (*capture)->get_current_pixel_format(),
		/*override_bounce=*/false, /*deinterlace=*/false, /*user_connectable=*/false);
	if (ret == 1) {
		Theme *theme = get_theme_updata(L);
		LiveInputWrapper **live_input = (LiveInputWrapper **)lua_touserdata(L, -1);
		theme->register_html_signal_connection(chain, *live_input, *capture);
	}
	return ret;
}
#endif

int EffectChain_add_effect(lua_State* L)
{
	assert(lua_gettop(L) >= 2);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");

	// TODO: Better error reporting.
	Effect *effect;
	if (luaL_testudata(L, 2, "ImageInput")) {
		effect = *(ImageInput **)luaL_checkudata(L, 2, "ImageInput");
	} else {
		effect = get_effect_from_blueprint(chain, L, 2);
	}
	if (lua_gettop(L) == 2) {
		if (effect->num_inputs() == 0) {
			chain->add_input((Input *)effect);
		} else {
			chain->add_effect(effect);
		}
	} else {
		vector<Effect *> inputs;
		for (int idx = 3; idx <= lua_gettop(L); ++idx) {
			if (luaL_testudata(L, idx, "LiveInputWrapper")) {
				LiveInputWrapper **input = (LiveInputWrapper **)lua_touserdata(L, idx);
				inputs.push_back((*input)->get_effect());
			} else if (luaL_testudata(L, idx, "ImageInput")) {
				ImageInput *image = *(ImageInput **)luaL_checkudata(L, idx, "ImageInput");
				inputs.push_back(image);
			} else {
				EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, idx, "EffectBlueprint");
				assert(blueprint->effect != nullptr);  // Parent must be added to the graph.
				inputs.push_back(blueprint->effect);
			}
		}
		chain->add_effect(effect, inputs);
	}

	lua_settop(L, 2);  // Return the effect itself.

	// Make sure Lua doesn't garbage-collect it away.
	lua_pushvalue(L, -1);
	luaL_ref(L, LUA_REGISTRYINDEX);  // TODO: leak?

	return 1;
}

int EffectChain_finalize(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	EffectChain *chain = (EffectChain *)luaL_checkudata(L, 1, "EffectChain");
	bool is_main_chain = checkbool(L, 2);
	add_outputs_and_finalize(chain, is_main_chain);
	return 0;
}

int LiveInputWrapper_connect_signal(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	LiveInputWrapper **input = (LiveInputWrapper **)luaL_checkudata(L, 1, "LiveInputWrapper");
	int signal_num = luaL_checknumber(L, 2);
	bool success = (*input)->connect_signal(signal_num);
	if (!success) {
		print_warning(L, "Calling connect_signal() on a video or HTML input. Ignoring.\n");
	}
	return 0;
}

int ImageInput_new(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	string filename = checkstdstring(L, 1);
	return wrap_lua_object_nonowned<ImageInput>(L, "ImageInput", filename);
}

int VideoInput_new(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	string filename = checkstdstring(L, 1);
	int pixel_format = luaL_checknumber(L, 2);
	if (pixel_format != bmusb::PixelFormat_8BitYCbCrPlanar &&
	    pixel_format != bmusb::PixelFormat_8BitBGRA) {
		print_warning(L, "Invalid enum %d used for video format, choosing Y'CbCr.\n", pixel_format);
		pixel_format = bmusb::PixelFormat_8BitYCbCrPlanar;
	}
	int ret = wrap_lua_object_nonowned<FFmpegCapture>(L, "VideoInput", filename, global_flags.width, global_flags.height);
	if (ret == 1) {
		FFmpegCapture **capture = (FFmpegCapture **)lua_touserdata(L, -1);
		(*capture)->set_pixel_format(bmusb::PixelFormat(pixel_format));

		Theme *theme = get_theme_updata(L);
		theme->register_video_input(*capture);
	}
	return ret;
}

int VideoInput_rewind(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	FFmpegCapture **video_input = (FFmpegCapture **)luaL_checkudata(L, 1, "VideoInput");
	(*video_input)->rewind();
	return 0;
}

int VideoInput_disconnect(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	FFmpegCapture **video_input = (FFmpegCapture **)luaL_checkudata(L, 1, "VideoInput");
	(*video_input)->disconnect();
	return 0;
}

int VideoInput_change_rate(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	FFmpegCapture **video_input = (FFmpegCapture **)luaL_checkudata(L, 1, "VideoInput");
	double new_rate = luaL_checknumber(L, 2);
	(*video_input)->change_rate(new_rate);
	return 0;
}

int VideoInput_get_signal_num(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	FFmpegCapture **video_input = (FFmpegCapture **)luaL_checkudata(L, 1, "VideoInput");
	lua_pushnumber(L, -1 - (*video_input)->get_card_index());
	return 1;
}

int HTMLInput_new(lua_State* L)
{
#ifdef HAVE_CEF
	assert(lua_gettop(L) == 1);
	string url = checkstdstring(L, 1);
	int ret = wrap_lua_object_nonowned<CEFCapture>(L, "HTMLInput", url, global_flags.width, global_flags.height);
	if (ret == 1) {
		CEFCapture **capture = (CEFCapture **)lua_touserdata(L, -1);
		Theme *theme = get_theme_updata(L);
		theme->register_html_input(*capture);
	}
	return ret;
#else
	fprintf(stderr, "This version of Nageru has been compiled without CEF support.\n");
	fprintf(stderr, "HTMLInput is not available.\n");
	abort();
#endif
}

#ifdef HAVE_CEF
int HTMLInput_set_url(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	CEFCapture **video_input = (CEFCapture **)luaL_checkudata(L, 1, "HTMLInput");
	string new_url = checkstdstring(L, 2);
	(*video_input)->set_url(new_url);
	return 0;
}

int HTMLInput_reload(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	CEFCapture **video_input = (CEFCapture **)luaL_checkudata(L, 1, "HTMLInput");
	(*video_input)->reload();
	return 0;
}

int HTMLInput_set_max_fps(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	CEFCapture **video_input = (CEFCapture **)luaL_checkudata(L, 1, "HTMLInput");
	int max_fps = lrint(luaL_checknumber(L, 2));
	(*video_input)->set_max_fps(max_fps);
	return 0;
}

int HTMLInput_execute_javascript_async(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	CEFCapture **video_input = (CEFCapture **)luaL_checkudata(L, 1, "HTMLInput");
	string js = checkstdstring(L, 2);
	(*video_input)->execute_javascript_async(js);
	return 0;
}

int HTMLInput_resize(lua_State* L)
{
	assert(lua_gettop(L) == 3);
	CEFCapture **video_input = (CEFCapture **)luaL_checkudata(L, 1, "HTMLInput");
	unsigned width = lrint(luaL_checknumber(L, 2));
	unsigned height = lrint(luaL_checknumber(L, 3));
	(*video_input)->resize(width, height);
	return 0;
}

int HTMLInput_get_signal_num(lua_State* L)
{
	assert(lua_gettop(L) == 1);
	CEFCapture **video_input = (CEFCapture **)luaL_checkudata(L, 1, "HTMLInput");
	lua_pushnumber(L, -1 - (*video_input)->get_card_index());
	return 1;
}
#endif

int IdentityEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", IDENTITY_EFFECT);
}

int WhiteBalanceEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", WHITE_BALANCE_EFFECT);
}

int ResampleEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", RESAMPLE_EFFECT);
}

int PaddingEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", PADDING_EFFECT);
}

int IntegralPaddingEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", INTEGRAL_PADDING_EFFECT);
}

int OverlayEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", OVERLAY_EFFECT);
}

int ResizeEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", RESIZE_EFFECT);
}

int MultiplyEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", MULTIPLY_EFFECT);
}

int MixEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", MIX_EFFECT);
}

int LiftGammaGainEffect_new(lua_State* L)
{
	assert(lua_gettop(L) == 0);
	return wrap_lua_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", LIFT_GAMMA_GAIN_EFFECT);
}

int InputStateInfo_get_width(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);

	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_width[card_idx]);
	return 1;
}

int InputStateInfo_get_height(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_height[card_idx]);
	return 1;
}

int InputStateInfo_get_frame_height(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	unsigned height = input_state_info->last_height[card_idx];
	if (input_state_info->last_interlaced[card_idx]) {
		height *= 2;
	}
	lua_pushnumber(L, height);
	return 1;
}

int InputStateInfo_get_interlaced(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	lua_pushboolean(L, input_state_info->last_interlaced[card_idx]);
	return 1;
}

int InputStateInfo_get_has_signal(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	lua_pushboolean(L, input_state_info->last_has_signal[card_idx]);
	return 1;
}

int InputStateInfo_get_is_connected(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	lua_pushboolean(L, input_state_info->last_is_connected[card_idx]);
	return 1;
}

int InputStateInfo_get_frame_rate_nom(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_frame_rate_nom[card_idx]);
	return 1;
}

int InputStateInfo_get_frame_rate_den(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	lua_pushnumber(L, input_state_info->last_frame_rate_den[card_idx]);
	return 1;
}

int InputStateInfo_get_last_subtitle(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));
	if (!input_state_info->has_last_subtitle[card_idx]) {
		lua_pushnil(L);
	} else {
		lua_pushstring(L, input_state_info->last_subtitle[card_idx].c_str());
	}
	return 1;
}

namespace {

// Helper function to write e.g. “60” or “59.94”.
string format_frame_rate(int nom, int den)
{
	char buf[256];
	if (nom % den == 0) {
		snprintf(buf, sizeof(buf), "%d", nom / den);
	} else {
		snprintf(buf, sizeof(buf), "%.2f", double(nom) / den);
	}
	return buf;
}

// Helper function to write e.g. “720p60”.
string get_human_readable_resolution(const InputStateInfo *input_state_info, int signal_num)
{
	char buf[256];
	if (input_state_info->last_interlaced[signal_num]) {
		snprintf(buf, sizeof(buf), "%di", input_state_info->last_height[signal_num] * 2);

		// Show field rate instead of frame rate; really for cosmetics only
		// (and actually contrary to EBU recommendations, although in line
		// with typical user expectations).
		return buf + format_frame_rate(input_state_info->last_frame_rate_nom[signal_num] * 2,
			input_state_info->last_frame_rate_den[signal_num]);
	} else {
		snprintf(buf, sizeof(buf), "%dp", input_state_info->last_height[signal_num]);
		return buf + format_frame_rate(input_state_info->last_frame_rate_nom[signal_num],
			input_state_info->last_frame_rate_den[signal_num]);
	}
}

} // namespace

int InputStateInfo_get_human_readable_resolution(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	InputStateInfo *input_state_info = get_input_state_info(L, 1);
	Theme *theme = get_theme_updata(L);
	int card_idx = theme->map_signal_to_card(luaL_checknumber(L, 2));

	string str;
	if (!input_state_info->last_is_connected[card_idx]) {
		str = "disconnected";
	} else if (input_state_info->last_height[card_idx] <= 0) {
		str = "no signal";
	} else if (!input_state_info->last_has_signal[card_idx]) {
		if (input_state_info->last_height[card_idx] == 525) {
			// Special mode for the USB3 cards.
			str = "no signal";
		} else {
			str = get_human_readable_resolution(input_state_info, card_idx) + ", no signal";
		}
	} else {
		str = get_human_readable_resolution(input_state_info, card_idx);
	}

	lua_pushstring(L, str.c_str());
	return 1;
}


int EffectBlueprint_set_int(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, 1, "EffectBlueprint");
	string key = checkstdstring(L, 2);
	float value = luaL_checknumber(L, 3);
	if (blueprint->effect != nullptr) {
		if (!blueprint->effect->set_int(key, value)) {
			luaL_error(L, "Effect refused set_int(\"%s\", %d) (invalid key?)", key.c_str(), int(value));
		}
	} else {
		// TODO: check validity already here, if possible?
		blueprint->int_parameters[key] = value;
	}
	return 0;
}

int EffectBlueprint_set_float(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, 1, "EffectBlueprint");
	string key = checkstdstring(L, 2);
	float value = luaL_checknumber(L, 3);
	if (blueprint->effect != nullptr) {
		if (!blueprint->effect->set_float(key, value)) {
			luaL_error(L, "Effect refused set_float(\"%s\", %d) (invalid key?)", key.c_str(), int(value));
		}
	} else {
		// TODO: check validity already here, if possible?
		blueprint->float_parameters[key] = value;
	}
	return 0;
}

int EffectBlueprint_set_vec3(lua_State *L)
{
	assert(lua_gettop(L) == 5);
	EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, 1, "EffectBlueprint");
	string key = checkstdstring(L, 2);
	array<float, 3> v;
	v[0] = luaL_checknumber(L, 3);
	v[1] = luaL_checknumber(L, 4);
	v[2] = luaL_checknumber(L, 5);

	if (blueprint->effect != nullptr) {
		if (!blueprint->effect->set_vec3(key, v.data())) {
			luaL_error(L, "Effect refused set_vec3(\"%s\", %f, %f, %f) (invalid key?)", key.c_str(),
				v[0], v[1], v[2]);
		}
	} else {
		// TODO: check validity already here, if possible?
		blueprint->vec3_parameters[key] = v;
	}

	return 0;
}

int EffectBlueprint_set_vec4(lua_State *L)
{
	assert(lua_gettop(L) == 6);
	EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, 1, "EffectBlueprint");
	string key = checkstdstring(L, 2);
	array<float, 4> v;
	v[0] = luaL_checknumber(L, 3);
	v[1] = luaL_checknumber(L, 4);
	v[2] = luaL_checknumber(L, 5);
	v[3] = luaL_checknumber(L, 6);
	if (blueprint->effect != nullptr) {
		if (!blueprint->effect->set_vec4(key, v.data())) {
			luaL_error(L, "Effect refused set_vec4(\"%s\", %f, %f, %f, %f) (invalid key?)", key.c_str(),
				v[0], v[1], v[2], v[3]);
		}
	} else {
		// TODO: check validity already here, if possible?
		blueprint->vec4_parameters[key] = v;
	}
	return 0;
}

const luaL_Reg Scene_funcs[] = {
	{ "new", Scene_new },
	{ "__gc", Scene_gc },
	{ "add_input", Scene::add_input },
	{ "add_white_balance", Scene::add_white_balance },
	{ "add_effect", Scene::add_effect },
	{ "add_optional_effect", Scene::add_optional_effect },
	{ "finalize", Scene::finalize },
	{ NULL, NULL }
};

const luaL_Reg Block_funcs[] = {
	{ "display", Block_display },
	{ "choose", Block_choose },
	{ "enable", Block_enable },
	{ "enable_if", Block_enable_if },
	{ "disable", Block_disable },
	{ "always_disable_if_disabled", Block_always_disable_if_disabled },
	{ "promise_to_disable_if_enabled", Block_promise_to_disable_if_enabled },
	{ "set_int", Block_set_int },
	{ "set_float", Block_set_float },
	{ "set_vec3", Block_set_vec3 },
	{ "set_vec4", Block_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg EffectBlueprint_funcs[] = {
	// NOTE: No new() function; that's for the individual effects.
	{ "set_int", EffectBlueprint_set_int },
	{ "set_float", EffectBlueprint_set_float },
	{ "set_vec3", EffectBlueprint_set_vec3 },
	{ "set_vec4", EffectBlueprint_set_vec4 },
	{ NULL, NULL }
};

const luaL_Reg EffectChain_funcs[] = {
	{ "new", EffectChain_new },
	{ "__gc", EffectChain_gc },
	{ "add_live_input", EffectChain_add_live_input },
	{ "add_video_input", EffectChain_add_video_input },
#ifdef HAVE_CEF
	{ "add_html_input", EffectChain_add_html_input },
#endif
	{ "add_effect", EffectChain_add_effect },
	{ "finalize", EffectChain_finalize },
	{ NULL, NULL }
};

const luaL_Reg LiveInputWrapper_funcs[] = {
	{ "connect_signal", LiveInputWrapper_connect_signal },
	{ NULL, NULL }
};

const luaL_Reg ImageInput_funcs[] = {
	{ "new", ImageInput_new },
	{ NULL, NULL }
};

const luaL_Reg VideoInput_funcs[] = {
	{ "new", VideoInput_new },
	{ "rewind", VideoInput_rewind },
	{ "disconnect", VideoInput_disconnect },
	{ "change_rate", VideoInput_change_rate },
	{ "get_signal_num", VideoInput_get_signal_num },
	{ NULL, NULL }
};

const luaL_Reg HTMLInput_funcs[] = {
	{ "new", HTMLInput_new },
#ifdef HAVE_CEF
	{ "set_url", HTMLInput_set_url },
	{ "reload", HTMLInput_reload },
	{ "set_max_fps", HTMLInput_set_max_fps },
	{ "execute_javascript_async", HTMLInput_execute_javascript_async },
	{ "resize", HTMLInput_resize },
	{ "get_signal_num", HTMLInput_get_signal_num },
#endif
	{ NULL, NULL }
};

// Effects.
// All of these are solely for new(); the returned metatable will be that of
// EffectBlueprint, and Effect (returned from add_effect()) is its own type.

const luaL_Reg IdentityEffect_funcs[] = {
	{ "new", IdentityEffect_new },
	{ NULL, NULL }
};

const luaL_Reg WhiteBalanceEffect_funcs[] = {
	{ "new", WhiteBalanceEffect_new },
	{ NULL, NULL }
};

const luaL_Reg ResampleEffect_funcs[] = {
	{ "new", ResampleEffect_new },
	{ NULL, NULL }
};

const luaL_Reg PaddingEffect_funcs[] = {
	{ "new", PaddingEffect_new },
	{ NULL, NULL }
};

const luaL_Reg IntegralPaddingEffect_funcs[] = {
	{ "new", IntegralPaddingEffect_new },
	{ NULL, NULL }
};

const luaL_Reg OverlayEffect_funcs[] = {
	{ "new", OverlayEffect_new },
	{ NULL, NULL }
};

const luaL_Reg ResizeEffect_funcs[] = {
	{ "new", ResizeEffect_new },
	{ NULL, NULL }
};

const luaL_Reg MultiplyEffect_funcs[] = {
	{ "new", MultiplyEffect_new },
	{ NULL, NULL }
};

const luaL_Reg MixEffect_funcs[] = {
	{ "new", MixEffect_new },
	{ NULL, NULL }
};

const luaL_Reg LiftGammaGainEffect_funcs[] = {
	{ "new", LiftGammaGainEffect_new },
	{ NULL, NULL }
};

// End of effects.

const luaL_Reg InputStateInfo_funcs[] = {
	{ "get_width", InputStateInfo_get_width },
	{ "get_height", InputStateInfo_get_height },
	{ "get_frame_width", InputStateInfo_get_width },  // Same as get_width().
	{ "get_frame_height", InputStateInfo_get_frame_height },
	{ "get_interlaced", InputStateInfo_get_interlaced },
	{ "get_has_signal", InputStateInfo_get_has_signal },
	{ "get_is_connected", InputStateInfo_get_is_connected },
	{ "get_frame_rate_nom", InputStateInfo_get_frame_rate_nom },
	{ "get_frame_rate_den", InputStateInfo_get_frame_rate_den },
	{ "get_last_subtitle", InputStateInfo_get_last_subtitle },
	{ "get_human_readable_resolution", InputStateInfo_get_human_readable_resolution },
	{ NULL, NULL }
};

const luaL_Reg ThemeMenu_funcs[] = {
	{ "set", ThemeMenu_set },
	{ NULL, NULL }
};

}  // namespace

LiveInputWrapper::LiveInputWrapper(
	Theme *theme,
	EffectChain *chain,
	bmusb::PixelFormat pixel_format,
	bool override_bounce,
	bool deinterlace,
	bool user_connectable)
	: theme(theme),
	  pixel_format(pixel_format),
	  deinterlace(deinterlace),
	  user_connectable(user_connectable)
{
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;

	// Gamma curve depends on the input signal, and we don't really get any
	// indications. A camera would be expected to do Rec. 709, but
	// I haven't checked if any do in practice. However, computers _do_ output
	// in sRGB gamma (ie., they don't convert from sRGB to Rec. 709), and
	// I wouldn't really be surprised if most non-professional cameras do, too.
	// So we pick sRGB as the least evil here.
	inout_format.gamma_curve = GAMMA_sRGB;

	unsigned num_inputs;
	if (deinterlace) {
		deinterlace_effect = new movit::DeinterlaceEffect();

		// As per the comments in deinterlace_effect.h, we turn this off.
		// The most likely interlaced input for us is either a camera
		// (where it's fine to turn it off) or a laptop (where it _should_
		// be turned off).
		CHECK(deinterlace_effect->set_int("enable_spatial_interlacing_check", 0));

		num_inputs = deinterlace_effect->num_inputs();
		assert(num_inputs == FRAME_HISTORY_LENGTH);
	} else {
		num_inputs = 1;
	}

	if (pixel_format == bmusb::PixelFormat_8BitBGRA) {
		for (unsigned i = 0; i < num_inputs; ++i) {
			// We upload our textures ourselves, and Movit swaps
			// R and B in the shader if we specify BGRA, so lie and say RGBA.
			rgba_inputs.push_back(new sRGBSwitchingFlatInput(inout_format, FORMAT_RGBA_POSTMULTIPLIED_ALPHA, GL_UNSIGNED_BYTE, global_flags.width, global_flags.height));
			chain->add_input(rgba_inputs.back());
		}

		if (deinterlace) {
			vector<Effect *> reverse_inputs(rgba_inputs.rbegin(), rgba_inputs.rend());
			chain->add_effect(deinterlace_effect, reverse_inputs);
		}
	} else {
		assert(pixel_format == bmusb::PixelFormat_8BitYCbCr ||
		       pixel_format == bmusb::PixelFormat_10BitYCbCr ||
		       pixel_format == bmusb::PixelFormat_8BitYCbCrPlanar);

		// Most of these settings will be overridden later if using PixelFormat_8BitYCbCrPlanar.
		input_ycbcr_format.chroma_subsampling_x = (pixel_format == bmusb::PixelFormat_10BitYCbCr) ? 1 : 2;
		input_ycbcr_format.chroma_subsampling_y = 1;
		input_ycbcr_format.num_levels = (pixel_format == bmusb::PixelFormat_10BitYCbCr) ? 1024 : 256;
		input_ycbcr_format.cb_x_position = 0.0;
		input_ycbcr_format.cr_x_position = 0.0;
		input_ycbcr_format.cb_y_position = 0.5;
		input_ycbcr_format.cr_y_position = 0.5;
		input_ycbcr_format.luma_coefficients = YCBCR_REC_709;  // Will be overridden later even if not planar.
		input_ycbcr_format.full_range = false;  // Will be overridden later even if not planar.

		for (unsigned i = 0; i < num_inputs; ++i) {
			// When using 10-bit input, we're converting to interleaved through v210Converter.
			YCbCrInputSplitting splitting;
			if (pixel_format == bmusb::PixelFormat_10BitYCbCr) {
				splitting = YCBCR_INPUT_INTERLEAVED;
			} else if (pixel_format == bmusb::PixelFormat_8BitYCbCr) {
				splitting = YCBCR_INPUT_SPLIT_Y_AND_CBCR;
			} else {
				splitting = YCBCR_INPUT_PLANAR;
			}
			if (override_bounce) {
				ycbcr_inputs.push_back(new NonBouncingYCbCrInput(inout_format, input_ycbcr_format, global_flags.width, global_flags.height, splitting));
			} else {
				ycbcr_inputs.push_back(new YCbCrInput(inout_format, input_ycbcr_format, global_flags.width, global_flags.height, splitting));
			}
			chain->add_input(ycbcr_inputs.back());
		}

		if (deinterlace) {
			vector<Effect *> reverse_inputs(ycbcr_inputs.rbegin(), ycbcr_inputs.rend());
			chain->add_effect(deinterlace_effect, reverse_inputs);
		}
	}
}

bool LiveInputWrapper::connect_signal(int signal_num)
{
	if (!user_connectable) {
		return false;
	}

	if (global_mixer == nullptr) {
		// No data yet.
		return true;
	}

	int card_idx = theme->map_signal_to_card(signal_num);
	connect_card(card_idx, *theme->input_state);
	return true;
}

void LiveInputWrapper::connect_card(int card_idx, const InputState &input_state)
{
	BufferedFrame first_frame = input_state.buffered_frames[card_idx][0];
	if (first_frame.frame == nullptr) {
		// No data yet.
		return;
	}
	unsigned width, height;
	{
		const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)first_frame.frame->userdata;
		width = userdata->last_width[first_frame.field_number];
		height = userdata->last_height[first_frame.field_number];
		if (userdata->last_interlaced) {
			height *= 2;
		}
	}

	movit::YCbCrLumaCoefficients ycbcr_coefficients = input_state.ycbcr_coefficients[card_idx];
	bool full_range = input_state.full_range[card_idx];

	if (input_state.ycbcr_coefficients_auto[card_idx]) {
		full_range = false;

		// The Blackmagic driver docs claim that the device outputs Y'CbCr
		// according to Rec. 601, but this seems to indicate the subsampling
		// positions only, as they publish Y'CbCr → RGB formulas that are
		// different for HD and SD (corresponding to Rec. 709 and 601, respectively),
		// and a Lenovo X1 gen 3 I used to test definitely outputs Rec. 709
		// (at least up to rounding error). Other devices seem to use Rec. 601
		// even on HD resolutions. Nevertheless, Rec. 709 _is_ the right choice
		// for HD, so we default to that if the user hasn't set anything.
		if (height >= 720) {
			ycbcr_coefficients = YCBCR_REC_709;
		} else {
			ycbcr_coefficients = YCBCR_REC_601;
		}
	}

	// This is a global, but it doesn't really matter.
	input_ycbcr_format.luma_coefficients = ycbcr_coefficients;
	input_ycbcr_format.full_range = full_range;

	BufferedFrame last_good_frame = first_frame;
	for (unsigned i = 0; i < max(ycbcr_inputs.size(), rgba_inputs.size()); ++i) {
		BufferedFrame frame = input_state.buffered_frames[card_idx][i];
		if (frame.frame == nullptr) {
			// Not enough data; reuse last frame (well, field).
			// This is suboptimal, but we have nothing better.
			frame = last_good_frame;
		}
		const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)frame.frame->userdata;

		unsigned this_width = userdata->last_width[frame.field_number];
		unsigned this_height = userdata->last_height[frame.field_number];
		if (this_width != width || this_height != height) {
			// Resolution changed; reuse last frame/field.
			frame = last_good_frame;
			userdata = (const PBOFrameAllocator::Userdata *)frame.frame->userdata;
		}

		assert(userdata->pixel_format == pixel_format);
		switch (pixel_format) {
		case bmusb::PixelFormat_8BitYCbCr:
			ycbcr_inputs[i]->set_texture_num(0, userdata->tex_y[frame.field_number]);
			ycbcr_inputs[i]->set_texture_num(1, userdata->tex_cbcr[frame.field_number]);
			ycbcr_inputs[i]->change_ycbcr_format(input_ycbcr_format);
			ycbcr_inputs[i]->set_width(width);
			ycbcr_inputs[i]->set_height(height);
			break;
		case bmusb::PixelFormat_8BitYCbCrPlanar:
			ycbcr_inputs[i]->set_texture_num(0, userdata->tex_y[frame.field_number]);
			ycbcr_inputs[i]->set_texture_num(1, userdata->tex_cb[frame.field_number]);
			ycbcr_inputs[i]->set_texture_num(2, userdata->tex_cr[frame.field_number]);
			// YCbCrPlanar is used for video streams, where we can have metadata from the mux.
			// Prefer that if there's no override. (Overrides are only available when using
			// video as SRT cards.)
			if (input_state.ycbcr_coefficients_auto[card_idx]) {
				ycbcr_inputs[i]->change_ycbcr_format(userdata->ycbcr_format);
			} else {
				ycbcr_inputs[i]->change_ycbcr_format(input_ycbcr_format);
			}
			ycbcr_inputs[i]->set_width(width);
			ycbcr_inputs[i]->set_height(height);
			break;
		case bmusb::PixelFormat_10BitYCbCr:
			ycbcr_inputs[i]->set_texture_num(0, userdata->tex_444[frame.field_number]);
			ycbcr_inputs[i]->change_ycbcr_format(input_ycbcr_format);
			ycbcr_inputs[i]->set_width(width);
			ycbcr_inputs[i]->set_height(height);
			break;
		case bmusb::PixelFormat_8BitBGRA:
			rgba_inputs[i]->set_texture_num(userdata->tex_rgba[frame.field_number]);
			rgba_inputs[i]->set_width(width);
			rgba_inputs[i]->set_height(height);
			break;
		default:
			assert(false);
		}

		last_good_frame = frame;
	}

	if (deinterlace) {
		BufferedFrame frame = input_state.buffered_frames[card_idx][0];
		CHECK(deinterlace_effect->set_int("current_field_position", frame.field_number));
	}
}

namespace {

int call_num_channels(lua_State *L)
{
	lua_getglobal(L, "num_channels");

	if (lua_pcall(L, 0, 1, 0) != 0) {
		fprintf(stderr, "error running function `num_channels': %s\n", lua_tostring(L, -1));
		fprintf(stderr, "Try Nageru.set_num_channels(...) at the start of the script instead.\n");
		abort();
	}

	int num_channels = luaL_checknumber(L, 1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return num_channels;
}

}  // namespace

int Nageru_set_channel_name(lua_State *L)
{
	// NOTE: m is already locked.
	Theme *theme = get_theme_updata(L);
	unsigned channel = luaL_checknumber(L, 1);
	const string text = checkstdstring(L, 2);
	theme->channel_names[channel] = text;
	lua_pop(L, 2);
	return 0;
}

int Nageru_set_num_channels(lua_State *L)
{
	// NOTE: m is already locked.
	Theme *theme = get_theme_updata(L);
	if (theme->startup_finished) {
		luaL_error(L, "set_num_channels() can only be called at startup.");
	}
	theme->num_channels = luaL_checknumber(L, 1);
	lua_pop(L, 1);
	return 0;
}

int Nageru_set_channel_signal(lua_State *L)
{
	// NOTE: m is already locked.
	Theme *theme = get_theme_updata(L);
	if (theme->startup_finished) {
		luaL_error(L, "set_channel_signal() can only be called at startup.");
	}
	unsigned channel = luaL_checknumber(L, 1);
	int signal = luaL_checknumber(L, 2);
	theme->channel_signals[channel] = signal;
	lua_pop(L, 2);
	return 0;
}

int Nageru_set_supports_wb(lua_State *L)
{
	// NOTE: m is already locked.
	Theme *theme = get_theme_updata(L);
	if (theme->startup_finished) {
		luaL_error(L, "set_supports_wb() can only be called at startup.");
	}
	unsigned channel = luaL_checknumber(L, 1);
	bool supports_wb = checkbool(L, 2);
	theme->channel_supports_wb[channel] = supports_wb;
	lua_pop(L, 2);
	return 0;
}

// NOTE: There's a race condition in all of the audio functions; if the mapping
// is changed by the user underway, you might not be manipulating the bus you
// expect. (You should not get crashes, though.) There's not all that much we
// can do about it, short of locking the entire mixer while anything from the
// theme runs.

int Nageru_get_num_audio_buses(lua_State *L)
{
	if (global_audio_mixer == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}
	lua_pushinteger(L, global_audio_mixer->num_buses());
	return 1;
}

int Nageru_get_audio_bus_name(lua_State *L)
{
	if (global_audio_mixer == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}
	int bus_index = luaL_checknumber(L, 1);
	InputMapping input_mapping = global_audio_mixer->get_input_mapping();
	if (bus_index < 0 || size_t(bus_index) >= input_mapping.buses.size()) {
		// Doesn't fix the race, but fixes other out-of-bounds.
		print_warning(L, "Theme called get_audio_bus_name() on nonexistent bus %d; returning nil.\n", bus_index);
		lua_pushnil(L);
	} else {
		lua_pushstring(L, input_mapping.buses[bus_index].name.c_str());
	}
	return 1;
}

int Nageru_get_audio_bus_fader_level_db(lua_State *L)
{
	if (global_audio_mixer == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}

	int bus_index = luaL_checknumber(L, 1);
	if (bus_index < 0 || size_t(bus_index) >= global_audio_mixer->num_buses()) {
		// Doesn't fix the race, but fixes other out-of-bounds.
		print_warning(L, "Theme called get_audio_bus_fader_level_db() on nonexistent bus %d; returning 0.0.\n", bus_index);
		lua_pushnumber(L, 0.0);
	} else {
		lua_pushnumber(L, global_audio_mixer->get_fader_volume(bus_index));
	}
	return 1;
}

int Nageru_set_audio_bus_fader_level_db(lua_State *L)
{
	if (global_audio_mixer == nullptr || global_mainwindow == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}

	int bus_index = luaL_checknumber(L, 1);
	if (bus_index < 0 || size_t(bus_index) >= global_audio_mixer->num_buses()) {
		// Doesn't fix the race, but fixes other out-of-bounds.
		print_warning(L, "Theme called set_audio_bus_fader_level_db() on nonexistent bus %d; ignoring.\n", bus_index);
		return 0;
	}
	double level_db = luaL_checknumber(L, 2);

	// Go through the UI, so that it gets updated.
	global_mainwindow->set_fader_absolute(bus_index, level_db);
	return 0;
}

int Nageru_get_audio_bus_mute(lua_State *L)
{
	if (global_audio_mixer == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}

	int bus_index = luaL_checknumber(L, 1);
	if (bus_index < 0 || size_t(bus_index) >= global_audio_mixer->num_buses()) {
		// Doesn't fix the race, but fixes other out-of-bounds.
		print_warning(L, "Theme called get_audio_bus_mute() on nonexistent bus %d; returning false.\n", bus_index);
		lua_pushboolean(L, false);
	} else {
		lua_pushboolean(L, global_audio_mixer->get_mute(bus_index));
	}
	return 1;
}

int Nageru_set_audio_bus_mute(lua_State *L)
{
	if (global_audio_mixer == nullptr || global_mainwindow == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}

	int bus_index = luaL_checknumber(L, 1);
	if (bus_index < 0 || size_t(bus_index) >= global_audio_mixer->num_buses()) {
		// Doesn't fix the race, but fixes other out-of-bounds.
		print_warning(L, "Theme called set_audio_bus_mute() on nonexistent bus %d; ignoring.\n", bus_index);
		return 0;
	}
	bool mute = checkbool(L, 2);

	// Go through the UI, so that it gets updated.
	if (mute != global_audio_mixer->get_mute(bus_index)) {
		global_mainwindow->toggle_mute(bus_index);
	}
	return 0;
}

int Nageru_get_audio_bus_eq_level_db(lua_State *L)
{
	if (global_audio_mixer == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}

	int bus_index = luaL_checknumber(L, 1);
	int band = luaL_checknumber(L, 2);
	if (bus_index < 0 || size_t(bus_index) >= global_audio_mixer->num_buses()) {
		// Doesn't fix the race, but fixes other out-of-bounds.
		print_warning(L, "Theme called get_audio_bus_eq_level_db() on nonexistent bus %d; returning 0.0.\n", bus_index);
		lua_pushnumber(L, 0.0);
	} else if (band != EQ_BAND_BASS && band != EQ_BAND_MID && band != EQ_BAND_TREBLE) {
		print_warning(L, "Theme called get_audio_bus_eq_level_db() on nonexistent band; returning 0.0.\n", bus_index);
		lua_pushnumber(L, 0.0);
	} else {
		lua_pushnumber(L, global_audio_mixer->get_eq(bus_index, EQBand(band)));
	}
	return 1;
}

int Nageru_set_audio_bus_eq_level_db(lua_State *L)
{
	if (global_audio_mixer == nullptr || global_mainwindow == nullptr) {
		// The audio mixer isn't set up until we know how many FFmpeg inputs we have.
		luaL_error(L, "Audio functions can not be called before the theme is done initializing.");
	}

	int bus_index = luaL_checknumber(L, 1);
	int band = luaL_checknumber(L, 2);
	if (bus_index < 0 || size_t(bus_index) >= global_audio_mixer->num_buses()) {
		// Doesn't fix the race, but fixes other out-of-bounds.
		print_warning(L, "Theme called set_audio_bus_eq_level_db() on nonexistent bus %d; ignoring.\n", bus_index);
		return 0;
	} else if (band != EQ_BAND_BASS && band != EQ_BAND_MID && band != EQ_BAND_TREBLE) {
		print_warning(L, "Theme called set_audio_bus_eq_level_db() on nonexistent band; returning 0.0.\n", bus_index);
		return 0;
	}
	double level_db = luaL_checknumber(L, 3);

	// Go through the UI, so that it gets updated.
	global_mainwindow->set_eq_absolute(bus_index, EQBand(band), level_db);
	return 0;
}

Theme::Theme(const string &filename, const vector<string> &search_dirs, ResourcePool *resource_pool)
	: resource_pool(resource_pool), signal_to_card_mapping(global_flags.default_stream_mapping)
{
	// Defaults.
	channel_names[0] = "Live";
	channel_names[1] = "Preview";

	L = luaL_newstate();
        luaL_openlibs(L);

	// Search through all directories until we find a file that will load
	// (as in, does not return LUA_ERRFILE); then run it. We store load errors
	// from all the attempts, and show them once we know we can't find any of them.
	lua_settop(L, 0);
	vector<string> errors;
	bool success = false;

	vector<string> real_search_dirs;
	if (!filename.empty() && filename[0] == '/') {
		real_search_dirs.push_back("");
	} else {
		real_search_dirs = search_dirs;
	}

	string path;
	int theme_code_ref;
	for (const string &dir : real_search_dirs) {
		if (dir.empty()) {
			path = filename;
		} else {
			path = dir + "/" + filename;
		}
		int err = luaL_loadfile(L, path.c_str());
		if (err == 0) {
			// Save the theme for when we're actually going to run it
			// (we need to set up the right environment below first,
			// and we couldn't do that before, because we didn't know the
			// path to put in Nageru.THEME_PATH).
			theme_code_ref = luaL_ref(L, LUA_REGISTRYINDEX);
			assert(lua_gettop(L) == 0);

			success = true;
			break;
		}
		errors.push_back(lua_tostring(L, -1));
		lua_pop(L, 1);
		if (err != LUA_ERRFILE) {
			// The file actually loaded, but failed to parse somehow. Abort; don't try the next one.
			break;
		}
	}

	if (!success) {
		for (const string &error : errors) {
			fprintf(stderr, "%s\n", error.c_str());
		}
		abort();
	}
	assert(lua_gettop(L) == 0);

	// Make sure the path exposed to the theme (as Nageru.THEME_PATH;
	// can be useful for locating files when talking to CEF) is absolute.
	// In a sense, it would be nice if realpath() had a mode not to
	// resolve symlinks, but it doesn't, so we only call it if we don't
	// already have an absolute path (which may leave ../ elements etc.).
	if (path[0] == '/') {
		theme_path = path;
	} else {
		char *absolute_theme_path = realpath(path.c_str(), nullptr);
		theme_path = absolute_theme_path;
		free(absolute_theme_path);
	}

	// Set up the API we provide.
	register_globals();
	register_class("Scene", Scene_funcs);
	register_class("Block", Block_funcs);
	register_class("EffectBlueprint", EffectBlueprint_funcs);
	register_class("EffectChain", EffectChain_funcs);
	register_class("LiveInputWrapper", LiveInputWrapper_funcs);
	register_class("ImageInput", ImageInput_funcs);
	register_class("VideoInput", VideoInput_funcs);
	register_class("HTMLInput", HTMLInput_funcs);
	register_class("IdentityEffect", IdentityEffect_funcs, IDENTITY_EFFECT);
	register_class("WhiteBalanceEffect", WhiteBalanceEffect_funcs, WHITE_BALANCE_EFFECT);
	register_class("ResampleEffect", ResampleEffect_funcs, RESAMPLE_EFFECT);
	register_class("PaddingEffect", PaddingEffect_funcs, PADDING_EFFECT);
	register_class("IntegralPaddingEffect", IntegralPaddingEffect_funcs, INTEGRAL_PADDING_EFFECT);
	register_class("OverlayEffect", OverlayEffect_funcs, OVERLAY_EFFECT);
	register_class("ResizeEffect", ResizeEffect_funcs, RESIZE_EFFECT);
	register_class("MultiplyEffect", MultiplyEffect_funcs, MULTIPLY_EFFECT);
	register_class("MixEffect", MixEffect_funcs, MIX_EFFECT);
	register_class("LiftGammaGainEffect", LiftGammaGainEffect_funcs, LIFT_GAMMA_GAIN_EFFECT);
	register_class("InputStateInfo", InputStateInfo_funcs);
	register_class("ThemeMenu", ThemeMenu_funcs);

	// Now actually run the theme to get everything set up.
	lua_rawgeti(L, LUA_REGISTRYINDEX, theme_code_ref);
	luaL_unref(L, LUA_REGISTRYINDEX, theme_code_ref);
	if (lua_pcall(L, 0, 0, 0)) {
		fprintf(stderr, "Error when running %s: %s\n", path.c_str(), lua_tostring(L, -1));
		abort();
	}
	assert(lua_gettop(L) == 0);

	if (num_channels == -1) {
		// Ask it for the number of channels.
		num_channels = call_num_channels(L);
	}
	startup_finished = true;
}

Theme::~Theme()
{
	theme_menu.reset();
	lua_close(L);
}

void Theme::register_globals()
{
	// Set Nageru.VIDEO_FORMAT_BGRA = bmusb::PixelFormat_8BitBGRA, etc.
	const vector<pair<string, int>> num_constants = {
		{ "VIDEO_FORMAT_BGRA", bmusb::PixelFormat_8BitBGRA },
		{ "VIDEO_FORMAT_YCBCR", bmusb::PixelFormat_8BitYCbCrPlanar },
		{ "CHECKABLE", MenuEntry::CHECKABLE },
		{ "CHECKED", MenuEntry::CHECKED },
		{ "EQ_BAND_BASS", EQ_BAND_BASS },
		{ "EQ_BAND_MID", EQ_BAND_MID },
		{ "EQ_BAND_TREBLE", EQ_BAND_TREBLE },
	};
	const vector<pair<string, string>> str_constants = {
		{ "THEME_PATH", theme_path },
	};

	lua_newtable(L);  // t = {}

	for (const pair<string, int> &constant : num_constants) {
		lua_pushstring(L, constant.first.c_str());
		lua_pushinteger(L, constant.second);
		lua_settable(L, 1);  // t[key] = value
	}
	for (const pair<string, string> &constant : str_constants) {
		lua_pushstring(L, constant.first.c_str());
		lua_pushstring(L, constant.second.c_str());
		lua_settable(L, 1);  // t[key] = value
	}

	const luaL_Reg Nageru_funcs[] = {
		// Channel information.
		{ "set_channel_name", Nageru_set_channel_name },
		{ "set_num_channels", Nageru_set_num_channels },
		{ "set_channel_signal", Nageru_set_channel_signal },
		{ "set_supports_wb", Nageru_set_supports_wb },

		// Audio.
		{ "get_num_audio_buses", Nageru_get_num_audio_buses },
		{ "get_audio_bus_name", Nageru_get_audio_bus_name },
		{ "get_audio_bus_fader_level_db", Nageru_get_audio_bus_fader_level_db },
		{ "set_audio_bus_fader_level_db", Nageru_set_audio_bus_fader_level_db },
		{ "get_audio_bus_eq_level_db", Nageru_get_audio_bus_eq_level_db },
		{ "set_audio_bus_eq_level_db", Nageru_set_audio_bus_eq_level_db },
		{ "get_audio_bus_mute", Nageru_get_audio_bus_mute },
		{ "set_audio_bus_mute", Nageru_set_audio_bus_mute },

		{ nullptr, nullptr }
	};
	lua_pushlightuserdata(L, this);
	luaL_setfuncs(L, Nageru_funcs, 1);        // for (name,f in funcs) { mt[name] = f, with upvalue {theme} }

	lua_setglobal(L, "Nageru");  // Nageru = t
	assert(lua_gettop(L) == 0);
}

void Theme::register_class(const char *class_name, const luaL_Reg *funcs, EffectType effect_type)
{
	assert(lua_gettop(L) == 0);
	luaL_newmetatable(L, class_name);  // mt = {}
	lua_pushlightuserdata(L, this);
	luaL_setfuncs(L, funcs, 1);        // for (name,f in funcs) { mt[name] = f, with upvalue {theme} }
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");    // mt.__index = mt
	if (effect_type != NO_EFFECT_TYPE) {
		lua_pushnumber(L, effect_type);
		lua_setfield(L, -2, "__effect_type_id");  // mt.__effect_type_id = effect_type
	}
	lua_setglobal(L, class_name);      // ClassName = mt
	assert(lua_gettop(L) == 0);
}

Theme::Chain Theme::get_chain_from_effect_chain(EffectChain *effect_chain, unsigned num, const InputState &input_state)
{
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "Argument #-1 should be a function\n");
		abort();
	}
	lua_pushvalue(L, -1);
	shared_ptr<LuaRefWithDeleter> funcref(new LuaRefWithDeleter(&m, L, luaL_ref(L, LUA_REGISTRYINDEX)));
	lua_pop(L, 2);

	Chain chain;
	chain.chain = effect_chain;
	chain.setup_chain = [this, funcref, input_state, effect_chain]{
		lock_guard<mutex> lock(m);

		assert(this->input_state == nullptr);
		this->input_state = &input_state;

		// Set up state, including connecting signals.
		lua_rawgeti(L, LUA_REGISTRYINDEX, funcref->get());
		if (lua_pcall(L, 0, 0, 0) != 0) {
			fprintf(stderr, "error running chain setup callback: %s\n", lua_tostring(L, -1));
			abort();
		}
		assert(lua_gettop(L) == 0);

		// The theme can't (or at least shouldn't!) call connect_signal() on
		// each FFmpeg or CEF input, so we'll do it here.
		if (video_signal_connections.count(effect_chain)) {
			for (const VideoSignalConnection &conn : video_signal_connections[effect_chain]) {
				conn.wrapper->connect_card(conn.source->get_card_index(), input_state);
			}
		}
#ifdef HAVE_CEF
		if (html_signal_connections.count(effect_chain)) {
			for (const CEFSignalConnection &conn : html_signal_connections[effect_chain]) {
				conn.wrapper->connect_card(conn.source->get_card_index(), input_state);
			}
		}
#endif

		this->input_state = nullptr;
	};
	return chain;
}

Theme::Chain Theme::get_chain(unsigned num, float t, unsigned width, unsigned height, const InputState &input_state)
{
	const char *func_name = "get_scene";  // For error reporting.
	Chain chain;

	lock_guard<mutex> lock(m);
	assert(lua_gettop(L) == 0);
	lua_getglobal(L, "get_scene");  /* function to be called */
	if (lua_isnil(L, -1)) {
		// Try the pre-1.9.0 name for compatibility.
		lua_pop(L, 1);
		lua_getglobal(L, "get_chain");
		func_name = "get_chain";
	}
	lua_pushnumber(L, num);
	lua_pushnumber(L, t);
	lua_pushnumber(L, width);
	lua_pushnumber(L, height);
	wrap_lua_object<InputStateInfo>(L, "InputStateInfo", input_state);

	if (lua_pcall(L, 5, LUA_MULTRET, 0) != 0) {
		fprintf(stderr, "error running function “%s”: %s\n", func_name, lua_tostring(L, -1));
		abort();
	}

	if (luaL_testudata(L, -1, "Scene") != nullptr) {
		if (lua_gettop(L) != 1) {
			luaL_error(L, "%s() for chain number %d returned an Scene, but also other items", func_name);
		}
		Scene *auto_effect_chain = (Scene *)luaL_testudata(L, -1, "Scene");
		auto chain_and_setup = auto_effect_chain->get_chain(this, L, num, input_state);
		chain.chain = chain_and_setup.first;
		chain.setup_chain = move(chain_and_setup.second);
	} else if (luaL_testudata(L, -2, "EffectChain") != nullptr) {
		// Old-style (pre-Nageru 1.9.0) return of a single chain and prepare function.
		if (lua_gettop(L) != 2) {
			luaL_error(L, "%s() for chain number %d returned an EffectChain, but needs to also return a prepare function (or use Scene)", func_name);
		}
		EffectChain *effect_chain = (EffectChain *)luaL_testudata(L, -2, "EffectChain");
		chain = get_chain_from_effect_chain(effect_chain, num, input_state);
	} else {
		luaL_error(L, "%s() for chain number %d did not return an EffectChain or Scene\n", func_name, num);
	}
	assert(lua_gettop(L) == 0);

	// TODO: Can we do better, e.g. by running setup_chain() and seeing what it references?
	// Actually, setup_chain does maybe hold all the references we need now anyway?
	chain.input_frames.reserve(MAX_VIDEO_CARDS * FRAME_HISTORY_LENGTH);
	for (unsigned card_index = 0; card_index < MAX_VIDEO_CARDS; ++card_index) {
		for (unsigned frame_num = 0; frame_num < FRAME_HISTORY_LENGTH; ++frame_num) {
			chain.input_frames.push_back(input_state.buffered_frames[card_index][frame_num].frame);
		}
	}

	return chain;
}

string Theme::get_channel_name(unsigned channel)
{
	lock_guard<mutex> lock(m);

	// We never ask the legacy channel_name() about live and preview.
	// The defaults are set in our constructor.
	if (channel == 0 || channel == 1) {
		return channel_names[channel];
	}

	lua_getglobal(L, "channel_name");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		if (channel_names.count(channel)) {
			return channel_names[channel];
		} else {
			return "(no title)";
		}
	}

	lua_pushnumber(L, channel);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `channel_name': %s\n", lua_tostring(L, -1));
		abort();
	}
	const char *ret = lua_tostring(L, -1);
	if (ret == nullptr) {
		fprintf(stderr, "function `channel_name' returned nil for channel %d\n", channel);
		fprintf(stderr, "Try Nageru.set_channel_name(channel, name) at the start of the script instead.\n");
		abort();
	}

	string retstr = ret;
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return retstr;
}

int Theme::map_channel_to_signal(unsigned channel)
{
	lock_guard<mutex> lock(m);
	lua_getglobal(L, "channel_signal");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		if (channel_signals.count(channel)) {
			return channel_signals[channel];
		} else {
			return -1;
		}
	}

	lua_pushnumber(L, channel);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `channel_signal': %s\n", lua_tostring(L, -1));
		fprintf(stderr, "Try Nageru.set_channel_signal(channel, signal) at the start of the script instead.\n");
		abort();
	}

	int ret = luaL_checknumber(L, 1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return ret;
}

std::string Theme::get_channel_color(unsigned channel)
{
	lock_guard<mutex> lock(m);
	lua_getglobal(L, "channel_color");
	lua_pushnumber(L, channel);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `channel_color': %s\n", lua_tostring(L, -1));
		abort();
	}

	const char *ret = lua_tostring(L, -1);
	if (ret == nullptr) {
		fprintf(stderr, "function `channel_color' returned nil for channel %d\n", channel);
		abort();
	}

	string retstr = ret;
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return retstr;
}

bool Theme::get_supports_set_wb(unsigned channel)
{
	lock_guard<mutex> lock(m);
	lua_getglobal(L, "supports_set_wb");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		if (channel_supports_wb.count(channel)) {
			return channel_supports_wb[channel];
		} else {
			return false;
		}
	}

	lua_pushnumber(L, channel);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `supports_set_wb': %s\n", lua_tostring(L, -1));
		fprintf(stderr, "Try Nageru.set_supports_wb(channel, bool) at the start of the script instead.\n");
		abort();
	}

	bool ret = checkbool(L, -1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return ret;
}

void Theme::set_wb(unsigned channel, float r, float g, float b)
{
	int signal = map_channel_to_signal(channel);

	lock_guard<mutex> lock(m);
	if (signal != -1) {
		int card_idx = map_signal_to_card(signal);
		white_balance_for_card[card_idx] = RGBTriplet{ r, g, b };
	}

	call_lua_wb_callback(channel, r, g, b);
}

void Theme::set_wb_for_card(int card_idx, float r, float g, float b)
{
	lock_guard<mutex> lock(m);
	white_balance_for_card[card_idx] = RGBTriplet{ r, g, b };

	for (const auto &channel_and_signal : channel_signals) {
		if (map_signal_to_card(channel_and_signal.second) == card_idx) {
			call_lua_wb_callback(channel_and_signal.first, r, g, b);
		}
	}
}

void Theme::call_lua_wb_callback(unsigned channel, float r, float g, float b)
{
	lua_getglobal(L, "set_wb");
	if (lua_isnil(L, -1)) {
		// The function doesn't exist, to just ignore. We've stored the white balance,
		// and most likely, it will be picked up by auto white balance instead.
		lua_pop(L, 1);
		return;
	}
	lua_pushnumber(L, channel);
	lua_pushnumber(L, r);
	lua_pushnumber(L, g);
	lua_pushnumber(L, b);
	if (lua_pcall(L, 4, 0, 0) != 0) {
		fprintf(stderr, "error running function `set_wb': %s\n", lua_tostring(L, -1));
		abort();
	}

	assert(lua_gettop(L) == 0);
}

RGBTriplet Theme::get_white_balance_for_card(int card_idx)
{
	if (white_balance_for_card.count(card_idx)) {
		return white_balance_for_card[card_idx];
	} else {
		return RGBTriplet{ 1.0, 1.0, 1.0 };
	}
}

vector<string> Theme::get_transition_names(float t)
{
	lock_guard<mutex> lock(m);
	lua_getglobal(L, "get_transitions");
	lua_pushnumber(L, t);
	if (lua_pcall(L, 1, 1, 0) != 0) {
		fprintf(stderr, "error running function `get_transitions': %s\n", lua_tostring(L, -1));
		abort();
	}

	vector<string> ret;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		ret.push_back(lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return ret;
}

int Theme::map_signal_to_card(int signal_num)
{
	// Negative numbers map to raw signals.
	if (signal_num < 0) {
		return -1 - signal_num;
	}

	lock_guard<mutex> lock(map_m);
	if (signal_to_card_mapping.count(signal_num)) {
		return signal_to_card_mapping[signal_num];
	}

	int card_index;
	if (global_flags.output_card != -1) {
		// Try to exclude the output card from the default card_index.
		card_index = signal_num % (global_flags.max_num_cards - 1);
		if (card_index >= global_flags.output_card) {
			 ++card_index;
		}
		if (signal_num >= int(global_flags.max_num_cards - 1)) {
			print_warning(L, "Theme asked for input %d, but we only have %u input card(s) (card %d is busy with output).\n",
				signal_num, global_flags.max_num_cards - 1, global_flags.output_card);
			fprintf(stderr, "Mapping to card %d instead.\n", card_index);
		}
	} else {
		card_index = signal_num % global_flags.max_num_cards;
		if (signal_num >= int(global_flags.max_num_cards)) {
			print_warning(L, "Theme asked for input %d, but we only have %u card(s).\n", signal_num, global_flags.max_num_cards);
			fprintf(stderr, "Mapping to card %d instead.\n", card_index);
		}
	}
	global_mixer->force_card_active(card_index);
	signal_to_card_mapping[signal_num] = card_index;
	return card_index;
}

void Theme::set_signal_mapping(int signal_num, int card_idx)
{
	lock_guard<mutex> lock(map_m);
	assert(card_idx < MAX_VIDEO_CARDS);
	signal_to_card_mapping[signal_num] = card_idx;
}

void Theme::transition_clicked(int transition_num, float t)
{
	lock_guard<mutex> lock(m);
	lua_getglobal(L, "transition_clicked");
	lua_pushnumber(L, transition_num);
	lua_pushnumber(L, t);

	if (lua_pcall(L, 2, 0, 0) != 0) {
		fprintf(stderr, "error running function `transition_clicked': %s\n", lua_tostring(L, -1));
		abort();
	}
	assert(lua_gettop(L) == 0);
}

void Theme::channel_clicked(int preview_num)
{
	lock_guard<mutex> lock(m);
	lua_getglobal(L, "channel_clicked");
	lua_pushnumber(L, preview_num);

	if (lua_pcall(L, 1, 0, 0) != 0) {
		fprintf(stderr, "error running function `channel_clicked': %s\n", lua_tostring(L, -1));
		abort();
	}
	assert(lua_gettop(L) == 0);
}

template <class T>
void destroy(T &ref)
{
	ref.~T();
}

Theme::MenuEntry::~MenuEntry()
{
	if (is_submenu) {
		destroy(submenu);
	} else {
		luaL_unref(entry.L, LUA_REGISTRYINDEX, entry.lua_ref);
	}
}

namespace {

vector<unique_ptr<Theme::MenuEntry>> create_recursive_theme_menu(lua_State *L);

unique_ptr<Theme::MenuEntry> create_theme_menu_entry(lua_State *L, int index)
{
	unique_ptr<Theme::MenuEntry> entry;

	lua_rawgeti(L, index, 1);
	const string text = checkstdstring(L, -1);
	lua_pop(L, 1);

	unsigned flags = 0;
	if (lua_objlen(L, -1) > 2) {
		lua_rawgeti(L, -1, 3);
		flags = luaL_checknumber(L, -1);
		lua_pop(L, 1);
	}

	lua_rawgeti(L, index, 2);
	if (lua_istable(L, -1)) {
		vector<unique_ptr<Theme::MenuEntry>> submenu = create_recursive_theme_menu(L);
		entry.reset(new Theme::MenuEntry{ text, move(submenu) });
		lua_pop(L, 1);
	} else {
		luaL_checktype(L, -1, LUA_TFUNCTION);
		int ref = luaL_ref(L, LUA_REGISTRYINDEX);
		entry.reset(new Theme::MenuEntry{ text, L, ref, flags });
	}
	return entry;
}

vector<unique_ptr<Theme::MenuEntry>> create_recursive_theme_menu(lua_State *L)
{
	vector<unique_ptr<Theme::MenuEntry>> menu;
	size_t num_elements = lua_objlen(L, -1);
	for (size_t i = 1; i <= num_elements; ++i) {
		lua_rawgeti(L, -1, i);
		menu.emplace_back(create_theme_menu_entry(L, -1));
		lua_pop(L, 1);
	}
	return menu;
}

}  // namespace

int Theme::set_theme_menu(lua_State *L)
{
	theme_menu.reset();

	vector<unique_ptr<MenuEntry>> root_menu;
	int num_elements = lua_gettop(L);
	for (int i = 1; i <= num_elements; ++i) {
		root_menu.emplace_back(create_theme_menu_entry(L, i));
	}
	theme_menu.reset(new MenuEntry("", move(root_menu)));

	lua_pop(L, num_elements);
	assert(lua_gettop(L) == 0);

	if (theme_menu_callback != nullptr) {
		theme_menu_callback();
	}

	return 0;
}

void Theme::theme_menu_entry_clicked(int lua_ref)
{
	lock_guard<mutex> lock(m);
	lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
	if (lua_pcall(L, 0, 0, 0) != 0) {
		fprintf(stderr, "error running menu callback: %s\n", lua_tostring(L, -1));
		abort();
	}
}

string Theme::format_status_line(const string &disk_space_left_text, double file_length_seconds)
{
	lock_guard<mutex> lock(m);
	lua_getglobal(L, "format_status_line");
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return disk_space_left_text;
	}

	lua_pushstring(L, disk_space_left_text.c_str());
	lua_pushnumber(L, file_length_seconds);
	if (lua_pcall(L, 2, 1, 0) != 0) {
		fprintf(stderr, "error running function format_status_line(): %s\n", lua_tostring(L, -1));
		abort();
	}
	string text = checkstdstring(L, 1);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);
	return text;
}

void Theme::remove_card(unsigned card_index)
{
	lock_guard<mutex> lock(map_m);
	for (auto it = signal_to_card_mapping.begin(); it != signal_to_card_mapping.end(); ) {
		if (it->second == int(card_index)) {
			it = signal_to_card_mapping.erase(it);
		} else {
			++it;
		}
	}
}
