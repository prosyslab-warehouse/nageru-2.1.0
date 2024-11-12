#include "ycbcr_converter.h"

#include "exif_parser.h"
#include "flags.h"
#include "jpeg_frame.h"

#include <array>
#include <movit/mix_effect.h>
#include <movit/white_balance_effect.h>
#include <movit/ycbcr_input.h>

using namespace movit;
using namespace std;

namespace {

void setup_outputs(YCbCrConverter::OutputMode output_mode, const ImageFormat &output_format, const YCbCrFormat &ycbcr_output_format, EffectChain *chain)
{
	if (output_mode == YCbCrConverter::OUTPUT_TO_RGBA) {
		chain->add_output(output_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
		chain->set_output_origin(OUTPUT_ORIGIN_BOTTOM_LEFT);
	} else if (output_mode == YCbCrConverter::OUTPUT_TO_SEMIPLANAR) {
		chain->add_ycbcr_output(output_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format, YCBCR_OUTPUT_SPLIT_Y_AND_CBCR);
		chain->set_output_origin(OUTPUT_ORIGIN_TOP_LEFT);
	} else {
		assert(output_mode == YCbCrConverter::OUTPUT_TO_DUAL_YCBCR);

		// One full Y'CbCr texture (for interpolation), one that's just Y (throwing away the
		// Cb and Cr channels). The second copy is sort of redundant, but it's the easiest way
		// of getting the gray data into a layered texture.
		chain->add_ycbcr_output(output_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
		chain->add_ycbcr_output(output_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_output_format);
		chain->set_output_origin(OUTPUT_ORIGIN_TOP_LEFT);
	}
}

}  // namespace

YCbCrConverter::YCbCrConverter(YCbCrConverter::OutputMode output_mode, ResourcePool *resource_pool)
{
	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	ycbcr_format.luma_coefficients = YCBCR_REC_709;
	ycbcr_format.num_levels = 256;
	ycbcr_format.chroma_subsampling_x = 2;
	ycbcr_format.chroma_subsampling_y = 1;
	ycbcr_format.cb_x_position = 0.0f;  // H.264 -- _not_ JPEG, even though our input is MJPEG-encoded
	ycbcr_format.cb_y_position = 0.5f;  // Irrelevant.
	ycbcr_format.cr_x_position = 0.0f;
	ycbcr_format.cr_y_position = 0.5f;

	// This is a hack. Even though we're sending MJPEG around, which is
	// full-range, it's mostly transporting signals from limited-range
	// sources with no conversion, so we ought to have had false here.
	// However, in the off chance that we're actually getting real MJPEG,
	// we don't want to crush its blacks (or whites) by clamping. All of
	// our processing is fades or other linear operations, so if we're in
	// limited-range input, we'll stay in limited-range output. (Fading
	// between limited-range and full-range sources will be broken,
	// of course.) There will be some slight confusion in the parts of the
	// algorithms dealing with RGB, but they're small and we'll manage.
	ycbcr_format.full_range = false;

	YCbCrFormat ycbcr_output_format = ycbcr_format;
	ycbcr_output_format.chroma_subsampling_x = 1;

	// Planar Y'CbCr decoding chain.
	planar_chain.reset(new EffectChain(global_flags.width, global_flags.height, resource_pool));
	ycbcr_planar_input = (YCbCrInput *)planar_chain->add_input(new YCbCrInput(inout_format, ycbcr_format, global_flags.width, global_flags.height, YCBCR_INPUT_PLANAR));
	setup_outputs(output_mode, inout_format, ycbcr_output_format, planar_chain.get());
	planar_chain->set_dither_bits(8);
	planar_chain->finalize();

	// Semiplanar Y'CbCr decoding chain (for images coming from VA-API).
	semiplanar_chain.reset(new EffectChain(global_flags.width, global_flags.height, resource_pool));
	ycbcr_semiplanar_input = (YCbCrInput *)semiplanar_chain->add_input(new YCbCrInput(inout_format, ycbcr_format, global_flags.width, global_flags.height, YCBCR_INPUT_SPLIT_Y_AND_CBCR));
	setup_outputs(output_mode, inout_format, ycbcr_output_format, semiplanar_chain.get());
	semiplanar_chain->set_dither_bits(8);
	semiplanar_chain->finalize();

	// Fade chains. These include white balance adjustments.
	for (bool first_input_is_semiplanar : { false, true }) {
		for (bool second_input_is_semiplanar : { false, true }) {
			FadeChain &fade_chain = fade_chains[first_input_is_semiplanar][second_input_is_semiplanar];
			fade_chain.chain.reset(new EffectChain(global_flags.width, global_flags.height, resource_pool));
			fade_chain.input[0] = (YCbCrInput *)fade_chain.chain->add_input(
				new YCbCrInput(inout_format, ycbcr_format, global_flags.width, global_flags.height,
				               first_input_is_semiplanar ? YCBCR_INPUT_SPLIT_Y_AND_CBCR : YCBCR_INPUT_PLANAR));
			fade_chain.input[1] = (YCbCrInput *)fade_chain.chain->add_input(
				new YCbCrInput(inout_format, ycbcr_format, global_flags.width, global_flags.height,
				               second_input_is_semiplanar ? YCBCR_INPUT_SPLIT_Y_AND_CBCR : YCBCR_INPUT_PLANAR));
			fade_chain.wb_effect[0] = (WhiteBalanceEffect *)fade_chain.chain->add_effect(
				new WhiteBalanceEffect, fade_chain.input[0]);
			fade_chain.wb_effect[1] = (WhiteBalanceEffect *)fade_chain.chain->add_effect(
				new WhiteBalanceEffect, fade_chain.input[1]);
			fade_chain.mix_effect = (MixEffect *)fade_chain.chain->add_effect(
				new MixEffect, fade_chain.wb_effect[0], fade_chain.wb_effect[1]);
			setup_outputs(output_mode, inout_format, ycbcr_output_format, fade_chain.chain.get());
			fade_chain.chain->set_dither_bits(8);
			fade_chain.chain->finalize();
		}
	}

	// Fade from interleaved chain (ie., first input is interleaved, since it comes
	// directly from the GPU anyway).
	for (bool second_input_is_semiplanar : { false, true }) {
		FadeChain &fade_chain = interleaved_fade_chains[second_input_is_semiplanar];
		fade_chain.chain.reset(new EffectChain(global_flags.width, global_flags.height, resource_pool));

		ycbcr_format.chroma_subsampling_x = 1;
		fade_chain.input[0] = (YCbCrInput *)fade_chain.chain->add_input(
			new YCbCrInput(inout_format, ycbcr_format, global_flags.width, global_flags.height,
			               YCBCR_INPUT_INTERLEAVED));

		ycbcr_format.chroma_subsampling_x = 2;
		fade_chain.input[1] = (YCbCrInput *)fade_chain.chain->add_input(
			new YCbCrInput(inout_format, ycbcr_format, global_flags.width, global_flags.height,
			               second_input_is_semiplanar ? YCBCR_INPUT_SPLIT_Y_AND_CBCR : YCBCR_INPUT_PLANAR));

		fade_chain.wb_effect[0] = (WhiteBalanceEffect *)fade_chain.chain->add_effect(
			new WhiteBalanceEffect, fade_chain.input[0]);
		fade_chain.wb_effect[1] = (WhiteBalanceEffect *)fade_chain.chain->add_effect(
			new WhiteBalanceEffect, fade_chain.input[1]);
		fade_chain.mix_effect = (MixEffect *)fade_chain.chain->add_effect(
			new MixEffect, fade_chain.wb_effect[0], fade_chain.wb_effect[1]);
		setup_outputs(output_mode, inout_format, ycbcr_output_format, fade_chain.chain.get());
		fade_chain.chain->set_dither_bits(8);
		fade_chain.chain->finalize();
	}
}

EffectChain *YCbCrConverter::prepare_chain_for_conversion(shared_ptr<Frame> frame)
{
	if (frame->is_semiplanar) {
		setup_input_for_frame(frame, ycbcr_format, ycbcr_semiplanar_input);
		return semiplanar_chain.get();
	} else {
		setup_input_for_frame(frame, ycbcr_format, ycbcr_planar_input);
		return planar_chain.get();
	}
}

EffectChain *YCbCrConverter::prepare_chain_for_fade(shared_ptr<Frame> frame, shared_ptr<Frame> secondary_frame, float fade_alpha)
{
	const FadeChain &fade_chain = fade_chains[frame->is_semiplanar][secondary_frame->is_semiplanar];
	setup_input_for_frame(frame, ycbcr_format, fade_chain.input[0]);
	setup_input_for_frame(secondary_frame, ycbcr_format, fade_chain.input[1]);
	bool ok = fade_chain.mix_effect->set_float("strength_first", 1.0f - fade_alpha);
	ok |= fade_chain.mix_effect->set_float("strength_second", fade_alpha);
	RGBTriplet neutral_color0 = get_neutral_color(frame->exif_data);
	RGBTriplet neutral_color1 = get_neutral_color(secondary_frame->exif_data);
	ok |= fade_chain.wb_effect[0]->set_vec3("neutral_color", (float *)&neutral_color0);
	ok |= fade_chain.wb_effect[1]->set_vec3("neutral_color", (float *)&neutral_color1);
	assert(ok);
	return fade_chain.chain.get();
}

EffectChain *YCbCrConverter::prepare_chain_for_fade_from_texture(GLuint tex, RGBTriplet tex_neutral_color, unsigned width, unsigned height, std::shared_ptr<Frame> secondary_frame, float fade_alpha)
{
	const FadeChain &fade_chain = interleaved_fade_chains[secondary_frame->is_semiplanar];
	{
		YCbCrFormat format_copy = ycbcr_format;
		format_copy.chroma_subsampling_x = 1;
		format_copy.chroma_subsampling_y = 1;
		fade_chain.input[0]->change_ycbcr_format(format_copy);

		fade_chain.input[0]->set_width(width);  // Doesn't really matter.
		fade_chain.input[0]->set_height(height);
		fade_chain.input[0]->set_texture_num(0, tex);

		glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	}
	setup_input_for_frame(secondary_frame, ycbcr_format, fade_chain.input[1]);
	bool ok = fade_chain.mix_effect->set_float("strength_first", 1.0f - fade_alpha);
	ok |= fade_chain.mix_effect->set_float("strength_second", fade_alpha);
	RGBTriplet neutral_color1 = get_neutral_color(secondary_frame->exif_data);
	ok |= fade_chain.wb_effect[0]->set_vec3("neutral_color", (float *)&tex_neutral_color);
	ok |= fade_chain.wb_effect[1]->set_vec3("neutral_color", (float *)&neutral_color1);
	assert(ok);
	return fade_chain.chain.get();
}

void setup_input_for_frame(shared_ptr<Frame> frame, const YCbCrFormat &ycbcr_format, YCbCrInput *input)
{
	YCbCrFormat format_copy = ycbcr_format;
	format_copy.chroma_subsampling_x = frame->chroma_subsampling_x;
	format_copy.chroma_subsampling_y = frame->chroma_subsampling_y;
	input->change_ycbcr_format(format_copy);

	input->set_width(frame->width);
	input->set_height(frame->height);
	input->set_texture_num(0, *frame->y);
	if (frame->is_semiplanar) {
		input->set_texture_num(1, *frame->cbcr);
	} else {
		input->set_texture_num(1, *frame->cb);
		input->set_texture_num(2, *frame->cr);
	}
}
