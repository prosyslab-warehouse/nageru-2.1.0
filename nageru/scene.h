#ifndef _SCENE_H
#define _SCENE_H 1

// A Scene is an equivalent of an EffectChain, but each part is not a single
// Effect. (The name itself does not carry any specific meaning above that
// of what an EffectChain is; it was just chosen as a more intuitive name than
// an EffectChain when we had to change anyway.) Instead, it is a “block”,
// which can hold one or more effect alternatives, e.g., one block could hold
// ResizeEffect or IdentityEffect (effectively doing nothing), or many
// different input types. On finalization, every different combination of
// block alternatives are tried, and one EffectChain is generated for each.
// This also goes for whether the scene is destined for preview outputs
// (directly to screen, RGBA) or live (Y'CbCr output).

#include <stddef.h>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class CEFCapture;
struct EffectBlueprint;
class FFmpegCapture;
class ImageInput;
struct InputState;
class LiveInputWrapper;
class Theme;
struct lua_State;

namespace movit {
class Effect;
class EffectChain;
class ResourcePool;
}  // namespace movit

struct Block {
	// Index into the parent Scene's list of blocks.
	using Index = size_t;
	Index idx = 0;

	// Each instantiation is indexed by the chosen alternative for each block.
	// These are combined into one big variable-base number, ranging from 0
	// to (B_0*B_1*B_2*...*B_n)-1, where B_i is the number of alternatives for
	// block number i and n is the index of the last block.
	//
	// The actual index, given alternatives A_0, A_1, A_2, ..., is given as
	//
	//   A_0 + B_0 * (A_1 + B_1 * (A_2 + B_2 * (...)))
	//
	// where each A_i can of course range from 0 to B_i-1. In other words,
	// the first block gets the lowest “bits” (or trits, or quats...) of the
	// index number, the second block gets the ones immediately above,
	// and so on. Thus, there are no holes in the sequence.
	//
	// Expanding the formula above gives the equivalent index
	//
	//   A_0 + A_1 * B_0 + A_2 * B_0 * B_1 + A_3 * ...
	//
	// or
	//
	//   A_0 * C_0 + A_1 * C_1 + A_2 * C_2 + A_3 * ...
	//
	// where C_0 = 0 and C_(i+1) = C_i * B_i. In other words, C_i is
	// the product of the cardinalities of each previous effect; if we
	// are e.g. at the third index and there have been C_2 = 3 * 5 = 15
	// different alternatives for constructing the scene so far
	// (with possible indexes 0..14), it is only logical that if we
	// want three new options (B_2 = 3), we must add 0, 15 or 30 to
	// the index. (Then the local possible indexes become 0..44 and
	// C_3 = 45, of course.) Given an index number k, we can then get our
	// own local “bits” of the index, giving the alternative for this
	// block, by doing (k / 15) % 3.
	//
	// This specific member contains the value of C_i for this block.
	// (B_i is alternatives.size().) Not set before finalize() has run.
	size_t cardinality_base = 0;

	// Find the chosen alternative for this block in a given instance.
	int chosen_alternative(size_t chain_idx) const {
		if (chain_idx == size_t(-1)) {
			return currently_chosen_alternative;
		} else {
			return (chain_idx / cardinality_base) % alternatives.size();
		}
	}

	std::vector<EffectBlueprint *> alternatives;  // Must all have the same amount of inputs. Pointers to make things easier for Lua.
	std::vector<Index> inputs;  // One for each input of alternatives[0] (ie., typically 0 or 1, occasionally 2).

	// If any of these effects are disabled (IdentityEffect chosen)
	// or enabled (not chosen) as determined by <condition>, so should this one.
	struct Disabler {
		Index block_idx;
		enum {
			DISABLE_IF_OTHER_DISABLED,

			// This a promise from the user; ie., we don't disable automatically
			// (see comments in find_disabled_blocks()).
			DISABLE_IF_OTHER_ENABLED
		} condition;
		std::string declaration_point;  // For error messages.
	};
	std::vector<Disabler> disablers;
	int currently_chosen_alternative = 0;
	// What alternative to use if the block is disabled.
	// Points to an alternative with IDENTITY_EFFECT if it exists
	// (to disable as much as possible), otherwise 0.
	int canonical_alternative = 0;
	bool is_input = false;

	// For LIVE_INPUT* only. We can't just always populate signal_to_connect,
	// since when we set this, CEF and video signals may not have numbers yet.
	// FIXME: Perhaps it would be simpler if they just did?
	enum { CONNECT_NONE, CONNECT_SIGNAL, CONNECT_CEF, CONNECT_VIDEO } signal_type_to_connect = CONNECT_NONE;
	int signal_to_connect = 0;  // For CONNECT_SIGNAL.
#ifdef HAVE_CEF
	CEFCapture *cef_to_connect = nullptr;  // For CONNECT_CEF.
#endif
	FFmpegCapture *video_to_connect = nullptr;  // For CONNECT_VIDEO.

	std::string pathname;  // For IMAGE_INPUT only.

	// Parameters to set on the effect prior to render.
	// Will be set _before_ the ones from the EffectBlueprint, so that
	// the latter takes priority.
	std::map<std::string, int> int_parameters;
	std::map<std::string, float> float_parameters;
	std::map<std::string, std::array<float, 3>> vec3_parameters;
	std::map<std::string, std::array<float, 4>> vec4_parameters;

	std::string declaration_point;  // For error messages.

	// Only for AUTO_WHITE_BALANCE_EFFECT. Points to the parent block with is_input = true,
	// so that we know which signal to get the white balance from.
	const Block *white_balance_controller_block = nullptr;
};

int Block_display(lua_State* L);
int Block_choose(lua_State* L);
int Block_enable(lua_State *L);
int Block_enable_if(lua_State *L);
int Block_disable(lua_State *L);
int Block_always_disable_if_disabled(lua_State *L);
int Block_promise_to_disable_if_enabled(lua_State *L);
int Block_set_int(lua_State *L);
int Block_set_float(lua_State *L);
int Block_set_vec3(lua_State *L);
int Block_set_vec4(lua_State *L);

class Scene {
private:
	std::vector<Block *> blocks;  // The last one represents the output node (after finalization). Pointers to make things easier for Lua.
	struct Instantiation {
		std::unique_ptr<movit::EffectChain> chain;
		std::map<Block::Index, movit::Effect *> effects;  // So that we can set parameters.
		std::map<Block::Index, LiveInputWrapper *> inputs;  // So that we can connect signals.
		std::map<Block::Index, ImageInput *> image_inputs;  // So that we can connect signals.
	};
	std::vector<Instantiation> chains;  // Indexed by combination of each block's chosen alternative. See Block for information.

	Theme *theme;
	float aspect_nom, aspect_denom;
	movit::ResourcePool *resource_pool;

	movit::Effect *instantiate_effects(const Block *block, size_t chain_idx, Instantiation *instantiation);
	size_t compute_chain_number_for_block(size_t block_idx, const std::bitset<256> &disabled) const;
	static void find_inputs_for_block(lua_State *L, Scene *scene, Block *block, int first_input_idx = 3);
	static Block *find_block_from_arg(lua_State *L, Scene *scene, int idx);

	// Find out which blocks (indexed by position in the “blocks” array),
	// if any, are disabled in a given instantiation. A disabled block is
	// one that will not be instantiated at all, because it is a secondary
	// (ie., not the first) input of some multi-input effect that was replaced
	// with IdentityEffect in the given instantiation.
	//
	// Set chain_idx to size_t(-1) to use whatever is in each block's
	// currently_chosen_alternative.
	std::bitset<256> find_disabled_blocks(size_t chain_idx) const;
	void find_disabled_blocks(size_t chain_idx, size_t block_idx, bool currently_disabled, std::bitset<256> *disabled) const;

	// If a block is disabled, it should always have canonical_alternative chosen,
	// so that we don't instantiate a bunch of irrelevant duplicates that
	// differ only in disabled blocks. You can check this property with
	// is_noncanonical_chain() and then avoid instantiating the ones where
	// it returns true.
	bool is_noncanonical_chain(size_t chain_idx) const;

	// For a given block, find any parents it may have that are inputs.
	// If there is more than one, throws an error. If there are zero,
	// returns nullptr (should probably also be an error).
	const Block *find_root_input_block(lua_State *L, const Block *block);

public:
	Scene(Theme *theme, float aspect_nom, float aspect_denom);
	size_t compute_chain_number(bool is_main_chain) const;

	std::pair<movit::EffectChain *, std::function<void()>>
	get_chain(Theme *theme, lua_State *L, unsigned num, const InputState &input_state);

	static int add_input(lua_State *L);
	static int add_effect(lua_State *L);
	static int add_optional_effect(lua_State *L);
	static int add_white_balance(lua_State *L);
	static int finalize(lua_State *L);
};

#endif   // !defined(_SCENE_H)
