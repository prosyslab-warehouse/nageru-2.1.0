#include <assert.h>
extern "C" {
#include <lauxlib.h>
#include <lua.hpp>
}

#ifdef HAVE_CEF
#include "cef_capture.h"
#endif
#include "ffmpeg_capture.h"
#include "flags.h"
#include "image_input.h"
#include "input_state.h"
#include "lua_utils.h"
#include "scene.h"
#include "theme.h"

using namespace movit;
using namespace std;

static bool display(Block *block, lua_State *L, int idx);

EffectType current_type(const Block *block)
{
	return block->alternatives[block->currently_chosen_alternative]->effect_type;
}

int find_index_of(const Block *block, EffectType val)
{
	for (size_t idx = 0; idx < block->alternatives.size(); ++idx) {
		if (block->alternatives[idx]->effect_type == val) {
			return idx;
		}
	}
	return -1;
}

string get_declaration_point(lua_State *L)
{
	lua_Debug ar;
	lua_getstack(L, 1, &ar);
	lua_getinfo(L, "nSl", &ar);
	char buf[256];
	snprintf(buf, sizeof(buf), "%s:%d", ar.source, ar.currentline);
	return buf;
}

Scene::Scene(Theme *theme, float aspect_nom, float aspect_denom)
	: theme(theme), aspect_nom(aspect_nom), aspect_denom(aspect_denom), resource_pool(theme->get_resource_pool()) {}

size_t Scene::compute_chain_number(bool is_main_chain) const
{
	assert(chains.size() > 0);
	assert(chains.size() % 2 == 0);
	bitset<256> disabled = find_disabled_blocks(size_t(-1));

	size_t chain_number = compute_chain_number_for_block(blocks.size() - 1, disabled);
	assert(chain_number < chains.size() / 2);
	if (is_main_chain) {
		chain_number += chains.size() / 2;
	}
	return chain_number;
}

size_t Scene::compute_chain_number_for_block(size_t block_idx, const bitset<256> &disabled) const
{
	Block *block = blocks[block_idx];
	size_t chain_number;

	size_t currently_chosen_alternative;
	if (disabled.test(block_idx)) {
		// It doesn't matter, so pick the canonical choice
		// (this is the only one that is actually instantiated).
		currently_chosen_alternative = block->canonical_alternative;
	} else {
		currently_chosen_alternative = block->currently_chosen_alternative;
	}
	assert(currently_chosen_alternative < block->alternatives.size());

	if (block_idx == 0) {
		assert(block->cardinality_base == 1);
		chain_number = currently_chosen_alternative;
	} else {
		chain_number = compute_chain_number_for_block(block_idx - 1, disabled) + block->cardinality_base * currently_chosen_alternative;
	}
	return chain_number;
}

bitset<256> Scene::find_disabled_blocks(size_t chain_idx) const
{
	assert(blocks.size() < 256);

	// The find_disabled_blocks() recursion logic needs only one pass by itself,
	// but the disabler logic is not so smart, so we just run multiple times
	// until it converges.
	bitset<256> prev, ret;
	do {
		find_disabled_blocks(chain_idx, blocks.size() - 1, /*currently_disabled=*/false, &ret);
		prev = ret;

		// Propagate DISABLE_IF_OTHER_DISABLED constraints (we can always do this).
		for (Block *block : blocks) {
			if (ret.test(block->idx)) continue;  // Already disabled.

			EffectType chosen_type = block->alternatives[block->chosen_alternative(chain_idx)]->effect_type;
			if (chosen_type == IDENTITY_EFFECT) {
				ret.set(block->idx);
				continue;
			}

			for (const Block::Disabler &disabler : block->disablers) {
				Block *other = blocks[disabler.block_idx];
				EffectType chosen_type = other->alternatives[other->chosen_alternative(chain_idx)]->effect_type;
				bool other_disabled = ret.test(disabler.block_idx) || chosen_type == IDENTITY_EFFECT;
				if (other_disabled && disabler.condition == Block::Disabler::DISABLE_IF_OTHER_DISABLED) {
					ret.set(block->idx);
					break;
				}
			}
		}

		// We cannot propagate DISABLE_IF_OTHER_ENABLED in all cases;
		// the problem is that if A is disabled if B is enabled,
		// then we cannot disable A unless we actually know for sure
		// that B _is_ enabled. (E.g., imagine that B is disabled
		// if C is enabled -- we couldn't disable A before we knew if
		// C was enabled or not!)
		//
		// We could probably fix a fair amount of these, but the
		// primary use case for DISABLE_IF_OTHER_ENABLED is really
		// mutual exclusion; A must be disabled if B is enabled
		// _and_ vice versa. These loops cannot be automatically
		// resolved; it would depend on what A and B is. Thus,
		// we simply declare this kind of constraint to be a promise
		// from the user, not something that we'll solve for them.
	} while (prev != ret);
	return ret;
}

void Scene::find_disabled_blocks(size_t chain_idx, size_t block_idx, bool currently_disabled, bitset<256> *disabled) const
{
	if (currently_disabled) {
		disabled->set(block_idx);
	}
	Block *block = blocks[block_idx];
	EffectType chosen_type = block->alternatives[block->chosen_alternative(chain_idx)]->effect_type;
	for (size_t input_idx = 0; input_idx < block->inputs.size(); ++input_idx) {
		if (chosen_type == IDENTITY_EFFECT && input_idx > 0) {
			// Multi-input effect that has been replaced by
			// IdentityEffect, so every effect but the first are
			// disabled and will not participate in the chain.
			find_disabled_blocks(chain_idx, block->inputs[input_idx], /*currently_disabled=*/true, disabled);
		} else {
			// Just keep on recursing down.
			find_disabled_blocks(chain_idx, block->inputs[input_idx], currently_disabled, disabled);
		}
	}
}

bool Scene::is_noncanonical_chain(size_t chain_idx) const
{
	bitset<256> disabled = find_disabled_blocks(chain_idx);
	assert(blocks.size() < 256);
	for (size_t block_idx = 0; block_idx < blocks.size(); ++block_idx) {
		Block *block = blocks[block_idx];
		if (disabled.test(block_idx) && block->chosen_alternative(chain_idx) != block->canonical_alternative) {
			return true;
		}

		// Test if we're supposed to be disabled by some other block being enabled;
		// the disabled bit mask does not fully capture this.
		if (!disabled.test(block_idx)) {
			for (const Block::Disabler &disabler : block->disablers) {
				if (disabler.condition == Block::Disabler::DISABLE_IF_OTHER_ENABLED &&
				    !disabled.test(disabler.block_idx)) {
					return true;
				}
			}

			// Auto white balance is always disabled for image inputs.
			if (block->white_balance_controller_block != nullptr) {
				const Block *input = block->white_balance_controller_block;
				if (input->alternatives[input->chosen_alternative(chain_idx)]->effect_type == IMAGE_INPUT) {
					return true;
				}
			}
		}
	}
	return false;
}

int Scene::add_input(lua_State* L)
{
	assert(lua_gettop(L) == 1 || lua_gettop(L) == 2);
	Scene *scene = (Scene *)luaL_checkudata(L, 1, "Scene");

	Block *block = new Block;
	block->declaration_point = get_declaration_point(L);
	block->idx = scene->blocks.size();
	if (lua_gettop(L) == 1) {
		// No parameter given, so a flexible input.
		block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_YCBCR));
		block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_YCBCR_WITH_DEINTERLACE));
		block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_YCBCR_PLANAR));
		block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_BGRA));
		block->alternatives.emplace_back(new EffectBlueprint(IMAGE_INPUT));
	} else {
		// Input of a given type. We'll specialize it here, plus connect the input as given.
		if (lua_isnumber(L, 2)) {
			block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_YCBCR));
			block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_YCBCR_WITH_DEINTERLACE));
#ifdef HAVE_SRT
			if (global_flags.srt_port >= 0) {
				block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_YCBCR_PLANAR));
			}
#endif
#ifdef HAVE_CEF
		} else if (luaL_testudata(L, 2, "HTMLInput")) {
			block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_BGRA));
#endif
		} else if (luaL_testudata(L, 2, "VideoInput")) {
			FFmpegCapture *capture = *(FFmpegCapture **)luaL_checkudata(L, 2, "VideoInput");
			if (capture->get_current_pixel_format() == bmusb::PixelFormat_8BitYCbCrPlanar) {
				block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_YCBCR_PLANAR));
			} else {
				assert(capture->get_current_pixel_format() == bmusb::PixelFormat_8BitBGRA);
				block->alternatives.emplace_back(new EffectBlueprint(LIVE_INPUT_BGRA));
			}
		} else if (luaL_testudata(L, 2, "ImageInput")) {
			block->alternatives.emplace_back(new EffectBlueprint(IMAGE_INPUT));
		} else {
			luaL_error(L, "add_input() called with something that's not a signal (a signal number, a HTML input, or a VideoInput)");
		}
		bool ok = display(block, L, 2);
		assert(ok);
	}
	block->is_input = true;
	scene->blocks.push_back(block);

	return wrap_lua_existing_object_nonowned<Block>(L, "Block", block);
}

Block *Scene::find_block_from_arg(lua_State *L, Scene *scene, int idx)
{
	if (luaL_testudata(L, idx, "Block")) {
		return *(Block **)luaL_checkudata(L, idx, "Block");
	} else {
		EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, idx, "EffectBlueprint");

		// Search through all the blocks to figure out which one contains this effect.
		for (Block *block : scene->blocks) {
			if (find(block->alternatives.begin(), block->alternatives.end(), blueprint) != block->alternatives.end()) {
				return block;
			}
		}
		luaL_error(L, "Input effect in parameter #%d has not been added to this scene", idx - 1);
		return nullptr;  // Dead code.
	}
}

void Scene::find_inputs_for_block(lua_State *L, Scene *scene, Block *block, int first_input_idx)
{
	if (lua_gettop(L) == first_input_idx - 1) {
		// Implicitly the last added effect.
		assert(!scene->blocks.empty());
		block->inputs.push_back(scene->blocks.size() - 1);
		return;
	}

	for (int idx = first_input_idx; idx <= lua_gettop(L); ++idx) {
		block->inputs.push_back(find_block_from_arg(L, scene, idx)->idx);
	}
}

int Scene::add_effect(lua_State* L)
{
	assert(lua_gettop(L) >= 2);
	Scene *scene = (Scene *)luaL_checkudata(L, 1, "Scene");

	Block *block = new Block;
	block->declaration_point = get_declaration_point(L);
	block->idx = scene->blocks.size();

	if (lua_istable(L, 2)) {
		size_t len = lua_objlen(L, 2);
		for (size_t i = 0; i < len; ++i) {
			lua_rawgeti(L, 2, i + 1);
			EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, -1, "EffectBlueprint");
			block->alternatives.push_back(blueprint);
			lua_settop(L, -2);
		}
	} else {
		EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, 2, "EffectBlueprint");
		block->alternatives.push_back(blueprint);
	}

	int identity_index = find_index_of(block, IDENTITY_EFFECT);
	if (identity_index == -1) {
		block->canonical_alternative = 0;
	} else {
		// Pick the IdentityEffect as the canonical alternative, in case it
		// helps us disable more stuff.
		block->canonical_alternative = identity_index;
	}

	find_inputs_for_block(L, scene, block);
	scene->blocks.push_back(block);

	return wrap_lua_existing_object_nonowned<Block>(L, "Block", block);
}

int Scene::add_optional_effect(lua_State* L)
{
	assert(lua_gettop(L) >= 2);
	Scene *scene = (Scene *)luaL_checkudata(L, 1, "Scene");

	Block *block = new Block;
	block->declaration_point = get_declaration_point(L);
	block->idx = scene->blocks.size();

	EffectBlueprint *blueprint = *(EffectBlueprint **)luaL_checkudata(L, 2, "EffectBlueprint");
	block->alternatives.push_back(blueprint);

	// An IdentityEffect will be the alternative for when the effect is disabled.
	block->alternatives.push_back(new EffectBlueprint(IDENTITY_EFFECT));

	block->canonical_alternative = 1;

	find_inputs_for_block(L, scene, block);
	scene->blocks.push_back(block);

	return wrap_lua_existing_object_nonowned<Block>(L, "Block", block);
}

const Block *Scene::find_root_input_block(lua_State *L, const Block *block)
{
	if (block->is_input) {
		assert(block->inputs.size() == 0);
		return block;
	}

	const Block *ret = nullptr;
	for (size_t input_idx : block->inputs) {
		const Block *parent = find_root_input_block(L, blocks[input_idx]);
		if (parent != nullptr) {
			if (ret != nullptr) {
				luaL_error(L, "add_white_balance() was connected to more than one input");
			}
			ret = parent;
		}
	}
	return ret;
}

int Scene::add_white_balance(lua_State* L)
{
	assert(lua_gettop(L) >= 1);
	Scene *scene = (Scene *)luaL_checkudata(L, 1, "Scene");

	Block *block = new Block;
	block->declaration_point = get_declaration_point(L);
	block->idx = scene->blocks.size();

	block->alternatives.push_back(new EffectBlueprint(WHITE_BALANCE_EFFECT));
	block->alternatives.push_back(new EffectBlueprint(IDENTITY_EFFECT));

	block->canonical_alternative = 1;

	if (lua_gettop(L) == 1) {
		// The last added effect is implicitly both the input and gives the white balance controller.
		assert(!scene->blocks.empty());
		block->inputs.push_back(scene->blocks.size() - 1);
		block->white_balance_controller_block = scene->find_root_input_block(L, block);
	} else if (lua_gettop(L) == 2) {
		// The given effect is both the input and the white balance controller.
		block->inputs.push_back(find_block_from_arg(L, scene, 2)->idx);
		block->white_balance_controller_block = scene->find_root_input_block(L, block);
	} else if (lua_gettop(L) == 3) {
		// We have explicit input and white balance controller.
		block->inputs.push_back(find_block_from_arg(L, scene, 2)->idx);
		block->white_balance_controller_block = find_block_from_arg(L, scene, 3);
	} else {
		luaL_error(L, "add_white_balance([input], [white_balance_controller]) takes zero, one or two arguments");
	}
	if (block->white_balance_controller_block == nullptr || !block->white_balance_controller_block->is_input) {
		luaL_error(L, "add_white_balance() does not get its white balance from an input");
	}

	scene->blocks.push_back(block);

	return wrap_lua_existing_object_nonowned<Block>(L, "Block", block);
}

Effect *Scene::instantiate_effects(const Block *block, size_t chain_idx, Scene::Instantiation *instantiation)
{
	// Find the chosen alternative for this block in this instance.
	EffectType chosen_type = block->alternatives[block->chosen_alternative(chain_idx)]->effect_type;

	vector<Effect *> inputs;
	for (size_t input_idx : block->inputs) {
		inputs.push_back(instantiate_effects(blocks[input_idx], chain_idx, instantiation));

		// As a special case, we allow IdentityEffect to take only one input
		// even if the other alternative (or alternatives) is multi-input.
		// Thus, even if there are more than one inputs, instantiate only
		// the first one.
		if (chosen_type == IDENTITY_EFFECT) {
			break;
		}
	}

	Effect *effect;
	switch (chosen_type) {
	case LIVE_INPUT_YCBCR:
	case LIVE_INPUT_YCBCR_WITH_DEINTERLACE:
	case LIVE_INPUT_YCBCR_PLANAR:
	case LIVE_INPUT_BGRA: {
		bool deinterlace = (chosen_type == LIVE_INPUT_YCBCR_WITH_DEINTERLACE);
		bool override_bounce = !deinterlace;  // For most chains, this will be fine. Reconsider if we see real problems somewhere; it's better than having the user try to understand it.
		bmusb::PixelFormat pixel_format;
		if (chosen_type == LIVE_INPUT_BGRA) {
			pixel_format = bmusb::PixelFormat_8BitBGRA;
		} else if (chosen_type == LIVE_INPUT_YCBCR_PLANAR) {
			pixel_format = bmusb::PixelFormat_8BitYCbCrPlanar;
		} else if (global_flags.ten_bit_input) {
			pixel_format = bmusb::PixelFormat_10BitYCbCr;
		} else {
			pixel_format = bmusb::PixelFormat_8BitYCbCr;
		}
		LiveInputWrapper *input = new LiveInputWrapper(theme, instantiation->chain.get(), pixel_format, override_bounce, deinterlace, /*user_connectable=*/true);
		effect = input->get_effect();  // Adds itself to the chain, so no need to call add_effect().
		instantiation->inputs.emplace(block->idx, input);
		break;
	}
	case IMAGE_INPUT: {
		ImageInput *input = new ImageInput;
		instantiation->chain->add_input(input);
		instantiation->image_inputs.emplace(block->idx, input);
		effect = input;
		break;
	}
	default:
		effect = instantiate_effect(instantiation->chain.get(), chosen_type);
		instantiation->chain->add_effect(effect, inputs);
		break;
	}
	instantiation->effects.emplace(block->idx, effect);
	return effect;
}

int Scene::finalize(lua_State* L)
{
	bool only_one_mode = false;
	bool chosen_mode = false;
	if (lua_gettop(L) == 2) {
		only_one_mode = true;
		chosen_mode = checkbool(L, 2);
	} else {
		assert(lua_gettop(L) == 1);
	}
	Scene *scene = (Scene *)luaL_checkudata(L, 1, "Scene");
	Theme *theme = get_theme_updata(L);

	size_t base = 1;
	for (Block *block : scene->blocks) {
		block->cardinality_base = base;
		base *= block->alternatives.size();
	}

	const size_t cardinality = base;
	size_t real_cardinality = 0;
	for (size_t chain_idx = 0; chain_idx < cardinality; ++chain_idx) {
		if (!scene->is_noncanonical_chain(chain_idx)) {
			++real_cardinality;
		}
	}
	const size_t total_cardinality = real_cardinality * (only_one_mode ? 1 : 2);
	if (total_cardinality > 200) {
		print_warning(L, "The given Scene will instantiate %zu different versions. This will take a lot of time and RAM to compile; see if you could limit some options by e.g. locking the input type in some cases (by giving a fixed input to add_input()).\n",
			total_cardinality);
	}

	Block *output_block = scene->blocks.back();
	for (bool is_main_chain : { false, true }) {
		for (size_t chain_idx = 0; chain_idx < cardinality; ++chain_idx) {
			if ((only_one_mode && is_main_chain != chosen_mode) ||
			    scene->is_noncanonical_chain(chain_idx)) {
				scene->chains.emplace_back();
				continue;
			}

			Scene::Instantiation instantiation;
			instantiation.chain.reset(new EffectChain(scene->aspect_nom, scene->aspect_denom, theme->get_resource_pool()));
			scene->instantiate_effects(output_block, chain_idx, &instantiation);

			add_outputs_and_finalize(instantiation.chain.get(), is_main_chain);
			scene->chains.emplace_back(move(instantiation));
		}
	}
	return 0;
}

int find_card_to_connect(Theme *theme, lua_State *L, const Block *block)
{
	if (block->signal_type_to_connect == Block::CONNECT_SIGNAL) {
		return theme->map_signal_to_card(block->signal_to_connect);
#ifdef HAVE_CEF
	} else if (block->signal_type_to_connect == Block::CONNECT_CEF) {
		return block->cef_to_connect->get_card_index();
#endif
	} else if (block->signal_type_to_connect == Block::CONNECT_VIDEO) {
		return block->video_to_connect->get_card_index();
	} else if (block->signal_type_to_connect == Block::CONNECT_NONE) {
		luaL_error(L, "An input in a scene was not connected to anything (forgot to call display())");
	} else {
		assert(false);
	}
	return -1;
}

std::pair<movit::EffectChain *, std::function<void()>>
Scene::get_chain(Theme *theme, lua_State *L, unsigned num, const InputState &input_state)
{
	// For video inputs, pick the right interlaced/progressive version
	// based on the current state of the signals.
	InputStateInfo info(input_state);
	for (Block *block : blocks) {
		if (block->is_input && block->signal_type_to_connect == Block::CONNECT_SIGNAL) {
			int card_index = theme->map_signal_to_card(block->signal_to_connect);
			if (info.last_interlaced[card_index]) {
				assert(info.last_pixel_format[card_index] == bmusb::PixelFormat_8BitYCbCr ||
				       info.last_pixel_format[card_index] == bmusb::PixelFormat_10BitYCbCr);
				block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_YCBCR_WITH_DEINTERLACE);
			} else if (info.last_pixel_format[card_index] == bmusb::PixelFormat_8BitYCbCrPlanar) {
				block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_YCBCR_PLANAR);
			} else if (info.last_pixel_format[card_index] == bmusb::PixelFormat_8BitBGRA) {
				block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_BGRA);
			} else {
				block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_YCBCR);
			}
			if (block->currently_chosen_alternative == -1) {
				fprintf(stderr, "ERROR: Input connected to a video card pixel format that it was not ready for.\n");
				abort();
			}
		}
	}

	// Find all auto white balance blocks, turn on and off the effect as needed,
	// and fetch the actual white balance set (it is stored in Theme).
	map<Block *, array<float, 3>> white_balance;
	for (size_t block_idx = 0; block_idx < blocks.size(); ++block_idx) {
		Block *block = blocks[block_idx];
		const Block *input = block->white_balance_controller_block;
		if (input == nullptr) {
			continue;  // Not an auto white balance block.
		}

		EffectType chosen_type = current_type(input);
		if (chosen_type == IMAGE_INPUT) {
			// Image inputs never get white balance applied.
			block->currently_chosen_alternative = find_index_of(block, IDENTITY_EFFECT);
			continue;
		}

		assert(chosen_type == LIVE_INPUT_YCBCR ||
		       chosen_type == LIVE_INPUT_YCBCR_WITH_DEINTERLACE ||
		       chosen_type == LIVE_INPUT_YCBCR_PLANAR ||
		       chosen_type == LIVE_INPUT_BGRA);
		int card_idx = find_card_to_connect(theme, L, input);
		RGBTriplet wb = theme->get_white_balance_for_card(card_idx);
		if (fabs(wb.r - 1.0) < 1e-3 && fabs(wb.g - 1.0) < 1e-3 && fabs(wb.b - 1.0) < 1e-3) {
			// Neutral white balance.
			block->currently_chosen_alternative = find_index_of(block, IDENTITY_EFFECT);
		} else {
			block->currently_chosen_alternative = find_index_of(block, WHITE_BALANCE_EFFECT);
			white_balance.emplace(block, array<float, 3>{ wb.r, wb.g, wb.b });
		}
	}

	// Pick out the right chain based on the current selections,
	// and snapshot all the set variables so that we can set them
	// in the prepare function even if they're being changed by
	// the Lua code later.
	bool is_main_chain = (num == 0);
	size_t chain_idx = compute_chain_number(is_main_chain);
	if (is_noncanonical_chain(chain_idx)) {
		// This should be due to promise_to_disable_if_enabled(). Find out what
		// happened, to give the user some help.
		bitset<256> disabled = find_disabled_blocks(chain_idx);
		for (size_t block_idx = 0; block_idx < blocks.size(); ++block_idx) {
			Block *block = blocks[block_idx];
			if (disabled.test(block_idx)) continue;
			for (const Block::Disabler &disabler : block->disablers) {
				if (disabler.condition == Block::Disabler::DISABLE_IF_OTHER_ENABLED &&
				    !disabled.test(disabler.block_idx)) {
					fprintf(stderr, "Promise declared at %s violated.\n", disabler.declaration_point.c_str());
					abort();
				}
			}
		}
		assert(false);  // Something else happened, seemingly.
	}
	const Scene::Instantiation &instantiation = chains[chain_idx];
	EffectChain *effect_chain = instantiation.chain.get();

	map<LiveInputWrapper *, int> cards_to_connect;
	map<ImageInput *, string> images_to_select;
	map<pair<Effect *, string>, int> int_to_set;
	map<pair<Effect *, string>, float> float_to_set;
	map<pair<Effect *, string>, array<float, 3>> vec3_to_set;
	map<pair<Effect *, string>, array<float, 4>> vec4_to_set;
	for (const auto &index_and_input : instantiation.inputs) {
		Block *block = blocks[index_and_input.first];
		EffectType chosen_type = current_type(block);
		if (chosen_type == LIVE_INPUT_YCBCR ||
		    chosen_type == LIVE_INPUT_YCBCR_WITH_DEINTERLACE ||
		    chosen_type == LIVE_INPUT_YCBCR_PLANAR ||
		    chosen_type == LIVE_INPUT_BGRA) {
			LiveInputWrapper *input = index_and_input.second;
			cards_to_connect.emplace(input, find_card_to_connect(theme, L, block));
		}
	}
	for (const auto &index_and_input : instantiation.image_inputs) {
		Block *block = blocks[index_and_input.first];
		ImageInput *input = index_and_input.second;
		if (current_type(block) == IMAGE_INPUT) {
			images_to_select.emplace(input, block->pathname);
		}
	}
	for (const auto &index_and_effect : instantiation.effects) {
		Block *block = blocks[index_and_effect.first];
		Effect *effect = index_and_effect.second;

		bool missing_width = (current_type(block) == RESIZE_EFFECT ||
			current_type(block) == RESAMPLE_EFFECT ||
			current_type(block) == PADDING_EFFECT);
		bool missing_height = missing_width;

		// Get the effects currently set on the block.
		if (current_type(block) != IDENTITY_EFFECT) {  // Ignore settings on optional effects.
			if (block->int_parameters.count("width") && block->int_parameters["width"] > 0) {
				missing_width = false;
			}
			if (block->int_parameters.count("height") && block->int_parameters["height"] > 0) {
				missing_height = false;
			}
			for (const auto &key_and_tuple : block->int_parameters) {
				int_to_set.emplace(make_pair(effect, key_and_tuple.first), key_and_tuple.second);
			}
			for (const auto &key_and_tuple : block->float_parameters) {
				float_to_set.emplace(make_pair(effect, key_and_tuple.first), key_and_tuple.second);
			}
			if (white_balance.count(block)) {
				vec3_to_set.emplace(make_pair(effect, "neutral_color"), white_balance[block]);
			}
			for (const auto &key_and_tuple : block->vec3_parameters) {
				vec3_to_set.emplace(make_pair(effect, key_and_tuple.first), key_and_tuple.second);
			}
			for (const auto &key_and_tuple : block->vec4_parameters) {
				vec4_to_set.emplace(make_pair(effect, key_and_tuple.first), key_and_tuple.second);
			}
		}

		// Parameters set on the blueprint itself override those that are set for the block,
		// so they are set afterwards.
		if (!block->alternatives.empty()) {
			EffectBlueprint *blueprint = block->alternatives[block->currently_chosen_alternative];
			if (blueprint->int_parameters.count("width") && blueprint->int_parameters["width"] > 0) {
				missing_width = false;
			}
			if (blueprint->int_parameters.count("height") && blueprint->int_parameters["height"] > 0) {
				missing_height = false;
			}
			for (const auto &key_and_tuple : blueprint->int_parameters) {
				int_to_set[make_pair(effect, key_and_tuple.first)] = key_and_tuple.second;
			}
			for (const auto &key_and_tuple : blueprint->float_parameters) {
				float_to_set[make_pair(effect, key_and_tuple.first)] = key_and_tuple.second;
			}
			for (const auto &key_and_tuple : blueprint->vec3_parameters) {
				vec3_to_set[make_pair(effect, key_and_tuple.first)] = key_and_tuple.second;
			}
			for (const auto &key_and_tuple : blueprint->vec4_parameters) {
				vec4_to_set[make_pair(effect, key_and_tuple.first)] = key_and_tuple.second;
			}
		}

		if (missing_width || missing_height) {
			fprintf(stderr, "WARNING: Unset or nonpositive width/height for effect declared at %s "
				"when getting scene for signal %u; setting to 1x1 to avoid crash.\n",
				block->declaration_point.c_str(), num);
			int_to_set[make_pair(effect, "width")] = 1;
			int_to_set[make_pair(effect, "height")] = 1;
		}
	}

	lua_pop(L, 1);

	auto setup_chain = [L, theme, cards_to_connect, images_to_select, int_to_set, float_to_set, vec3_to_set, vec4_to_set, input_state]{
		lock_guard<mutex> lock(theme->m);

		// Set up state, including connecting cards.
		for (const auto &input_and_card : cards_to_connect) {
			LiveInputWrapper *input = input_and_card.first;
			input->connect_card(input_and_card.second, input_state);
		}
		for (const auto &input_and_filename : images_to_select) {
			input_and_filename.first->switch_image(input_and_filename.second);
		}
		for (const auto &effect_and_key_and_value : int_to_set) {
			Effect *effect = effect_and_key_and_value.first.first;
			const string &key = effect_and_key_and_value.first.second;
			const int value = effect_and_key_and_value.second;
			if (!effect->set_int(key, value)) {
				luaL_error(L, "Effect refused set_int(\"%s\", %d) (invalid key?)", key.c_str(), value);
			}
		}
		for (const auto &effect_and_key_and_value : float_to_set) {
			Effect *effect = effect_and_key_and_value.first.first;
			const string &key = effect_and_key_and_value.first.second;
			const float value = effect_and_key_and_value.second;
			if (!effect->set_float(key, value)) {
				luaL_error(L, "Effect refused set_float(\"%s\", %f) (invalid key?)", key.c_str(), value);
			}
		}
		for (const auto &effect_and_key_and_value : vec3_to_set) {
			Effect *effect = effect_and_key_and_value.first.first;
			const string &key = effect_and_key_and_value.first.second;
			const float *value = effect_and_key_and_value.second.data();
			if (!effect->set_vec3(key, value)) {
				luaL_error(L, "Effect refused set_vec3(\"%s\", %f, %f, %f) (invalid key?)", key.c_str(),
						value[0], value[1], value[2]);
			}
		}
		for (const auto &effect_and_key_and_value : vec4_to_set) {
			Effect *effect = effect_and_key_and_value.first.first;
			const string &key = effect_and_key_and_value.first.second;
			const float *value = effect_and_key_and_value.second.data();
			if (!effect->set_vec4(key, value)) {
				luaL_error(L, "Effect refused set_vec4(\"%s\", %f, %f, %f, %f) (invalid key?)", key.c_str(),
						value[0], value[1], value[2], value[3]);
			}
		}
	};
	return make_pair(effect_chain, move(setup_chain));
}

bool display(Block *block, lua_State *L, int idx)
{
	if (lua_isnumber(L, idx)) {
		int signal_idx = luaL_checknumber(L, idx);
		block->signal_type_to_connect = Block::CONNECT_SIGNAL;
		block->signal_to_connect = signal_idx;
		block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_YCBCR);  // Will be changed to deinterlaced at get_chain() time if needed.
		return true;
#ifdef HAVE_CEF
	} else if (luaL_testudata(L, idx, "HTMLInput")) {
		CEFCapture *capture = *(CEFCapture **)luaL_checkudata(L, idx, "HTMLInput");
		block->signal_type_to_connect = Block::CONNECT_CEF;
		block->cef_to_connect = capture;
		block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_BGRA);
		assert(capture->get_current_pixel_format() == bmusb::PixelFormat_8BitBGRA);
		return true;
#endif
	} else if (luaL_testudata(L, idx, "VideoInput")) {
		FFmpegCapture *capture = *(FFmpegCapture **)luaL_checkudata(L, idx, "VideoInput");
		block->signal_type_to_connect = Block::CONNECT_VIDEO;
		block->video_to_connect = capture;
		if (capture->get_current_pixel_format() == bmusb::PixelFormat_8BitYCbCrPlanar) {
			block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_YCBCR_PLANAR);
		} else {
			assert(capture->get_current_pixel_format() == bmusb::PixelFormat_8BitBGRA);
			block->currently_chosen_alternative = find_index_of(block, LIVE_INPUT_BGRA);
		}
		return true;
	} else if (luaL_testudata(L, idx, "ImageInput")) {
		ImageInput *image = *(ImageInput **)luaL_checkudata(L, idx, "ImageInput");
		block->signal_type_to_connect = Block::CONNECT_NONE;
		block->currently_chosen_alternative = find_index_of(block, IMAGE_INPUT);
		block->pathname = image->get_pathname();
		return true;
	} else {
		return false;
	}
}

int Block_display(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	if (!block->is_input) {
		luaL_error(L, "display() called on something that isn't an input");
	}

	bool ok = display(block, L, 2);
	if (!ok) {
		luaL_error(L, "display() called with something that's not a signal (a signal number, a HTML input, or a VideoInput)");
	}

	if (block->currently_chosen_alternative == -1) {
		luaL_error(L, "display() called on an input whose type was fixed at construction time, with a signal of different type");
	}

	return 0;
}

int Block_choose(lua_State* L)
{
	assert(lua_gettop(L) == 2);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	int alternative_idx = -1;
	if (lua_isnumber(L, 2)) {
		alternative_idx = luaL_checknumber(L, 2);
	} else if (lua_istable(L, 2)) {
		// See if it's an Effect metatable (e.g. foo:choose(ResampleEffect))
		lua_getfield(L, 2, "__effect_type_id");
		if (lua_isnumber(L, -1)) {
			EffectType effect_type = EffectType(luaL_checknumber(L, -1));
			alternative_idx = find_index_of(block, effect_type);
		}
		lua_pop(L, 1);
	}

	if (alternative_idx == -1) {
		luaL_error(L, "choose() called with something that was not an index or an effect type (e.g. ResampleEffect) that was part of the alternatives");
	}

	assert(alternative_idx >= 0);
	assert(size_t(alternative_idx) < block->alternatives.size());
	block->currently_chosen_alternative = alternative_idx;

	return wrap_lua_existing_object_nonowned<EffectBlueprint>(L, "EffectBlueprint", block->alternatives[alternative_idx]);
}

int Block_enable(lua_State *L)
{
	assert(lua_gettop(L) == 1);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");

	if (block->alternatives.size() != 2 ||
	    block->alternatives[1]->effect_type != IDENTITY_EFFECT) {
		luaL_error(L, "enable() called on something that wasn't added with add_optional_effect()");
	}
	block->currently_chosen_alternative = 0;  // The actual effect.
	return 0;
}

int Block_enable_if(lua_State *L)
{
	assert(lua_gettop(L) == 2);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");

	if (block->alternatives.size() != 2 ||
	    block->alternatives[1]->effect_type != IDENTITY_EFFECT) {
		luaL_error(L, "enable_if() called on something that wasn't added with add_optional_effect()");
	}
	bool enabled = checkbool(L, 2);
	block->currently_chosen_alternative = enabled ? 0 : 1;
	return 0;
}

int Block_disable(lua_State *L)
{
	assert(lua_gettop(L) == 1);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");

	block->currently_chosen_alternative = find_index_of(block, IDENTITY_EFFECT);
	if (block->currently_chosen_alternative == -1) {
		luaL_error(L, "disable() called on something that didn't have an IdentityEffect fallback (try add_optional_effect())");
	}
	assert(block->currently_chosen_alternative != -1);
	return 0;
}

int Block_always_disable_if_disabled(lua_State *L)
{
	assert(lua_gettop(L) == 2);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	Block *disabler_block = *(Block **)luaL_checkudata(L, 2, "Block");

	int my_alternative = find_index_of(block, IDENTITY_EFFECT);
	int their_alternative = find_index_of(disabler_block, IDENTITY_EFFECT);
	if (my_alternative == -1) {
		luaL_error(L, "always_disable_if_disabled() called on something that didn't have an IdentityEffect fallback (try add_optional_effect())");
	}
	if (their_alternative == -1) {
		luaL_error(L, "always_disable_if_disabled() with an argument that didn't have an IdentityEffect fallback (try add_optional_effect())");
	}

	// The declaration point isn't actually used, but it's nice for completeness.
	block->disablers.push_back(Block::Disabler{ disabler_block->idx, Block::Disabler::DISABLE_IF_OTHER_DISABLED, get_declaration_point(L) });

	lua_pop(L, 2);
	return 0;
}

int Block_promise_to_disable_if_enabled(lua_State *L)
{
	assert(lua_gettop(L) == 2);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	Block *disabler_block = *(Block **)luaL_checkudata(L, 2, "Block");

	int my_alternative = find_index_of(block, IDENTITY_EFFECT);
	int their_alternative = find_index_of(disabler_block, IDENTITY_EFFECT);
	if (my_alternative == -1) {
		luaL_error(L, "promise_to_disable_if_enabled() called on something that didn't have an IdentityEffect fallback (try add_optional_effect())");
	}
	if (their_alternative == -1) {
		luaL_error(L, "promise_to_disable_if_enabled() with an argument that didn't have an IdentityEffect fallback (try add_optional_effect())");
	}

	block->disablers.push_back(Block::Disabler{ disabler_block->idx, Block::Disabler::DISABLE_IF_OTHER_ENABLED, get_declaration_point(L) });

	lua_pop(L, 2);
	return 0;
}

int Block_set_int(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	string key = checkstdstring(L, 2);
	float value = luaL_checknumber(L, 3);

	// TODO: check validity already here, if possible?
	block->int_parameters[key] = value;

	return 0;
}

int Block_set_float(lua_State *L)
{
	assert(lua_gettop(L) == 3);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	string key = checkstdstring(L, 2);
	float value = luaL_checknumber(L, 3);

	// TODO: check validity already here, if possible?
	block->float_parameters[key] = value;

	return 0;
}

int Block_set_vec3(lua_State *L)
{
	assert(lua_gettop(L) == 5);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	string key = checkstdstring(L, 2);
	array<float, 3> v;
	v[0] = luaL_checknumber(L, 3);
	v[1] = luaL_checknumber(L, 4);
	v[2] = luaL_checknumber(L, 5);

	// TODO: check validity already here, if possible?
	block->vec3_parameters[key] = v;

	return 0;
}

int Block_set_vec4(lua_State *L)
{
	assert(lua_gettop(L) == 6);
	Block *block = *(Block **)luaL_checkudata(L, 1, "Block");
	string key = checkstdstring(L, 2);
	array<float, 4> v;
	v[0] = luaL_checknumber(L, 3);
	v[1] = luaL_checknumber(L, 4);
	v[2] = luaL_checknumber(L, 5);
	v[3] = luaL_checknumber(L, 6);

	// TODO: check validity already here, if possible?
	block->vec4_parameters[key] = v;

	return 0;
}

