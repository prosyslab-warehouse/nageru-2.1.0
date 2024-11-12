-- The theme is what decides what's actually shown on screen, what kind of
-- transitions are available (if any), and what kind of inputs there are,
-- if any. In general, it drives the entire display logic by creating Movit
-- chains (called “scenes”), setting their parameters and then deciding which
-- to show when.
--
-- Themes are written in Lua, which reflects a simplified form of the Movit API
-- where all the low-level details (such as texture formats) and alternatives
-- (e.g. turning scaling on or off) are handled by the C++ side and you
-- generally just build scenes.

local state = {
	transition_start = -2.0,
	transition_end = -1.0,
	transition_type = 0,
	transition_src_signal = 0,
	transition_dst_signal = 0,

	live_signal_num = 0,
	preview_signal_num = 1
}

-- Valid values for live_signal_num and preview_signal_num.
local INPUT0_SIGNAL_NUM = 0
local INPUT1_SIGNAL_NUM = 1
local SBS_SIGNAL_NUM = 2
local STATIC_SIGNAL_NUM = 3

-- Valid values for transition_type. (Cuts are done directly, so they need no entry.)
local NO_TRANSITION = 0
local ZOOM_TRANSITION = 1  -- Also for slides.
local FADE_TRANSITION = 2

function make_sbs_input(scene)
	return {
		input = scene:add_input(0),  -- Live inputs only.
		resample_effect = scene:add_effect({ResampleEffect.new(), ResizeEffect.new()}),
		wb_effect = scene:add_white_balance(),
		padding_effect = scene:add_effect(IntegralPaddingEffect.new())
	}
end

-- The main live scene.
function make_sbs_scene()
	local scene = Scene.new(16, 9)

	local input0 = make_sbs_input(scene)
	input0.input:display(0)
	input0.padding_effect:set_vec4("border_color", 0.0, 0.0, 0.0, 1.0)

	local input1 = make_sbs_input(scene)
	input1.input:display(1)
	input1.padding_effect:set_vec4("border_color", 0.0, 0.0, 0.0, 0.0)

	scene:add_effect(OverlayEffect.new(), input0.padding_effect, input1.padding_effect)
	scene:finalize()

	return {
		scene = scene,
		input0 = input0,
		input1 = input1
	}
end
local sbs_scene = make_sbs_scene()

function make_fade_input(scene)
	return {
		input = scene:add_input(),
		resample_effect = scene:add_optional_effect(ResampleEffect.new()),  -- Activated if scaling.
		wb_effect = scene:add_white_balance()  -- Activated for video inputs.
	}
end

-- A scene to fade between two inputs, of which either can be a picture
-- or a live input. Only used live.
function make_fade_scene()
	local scene = Scene.new(16, 9)
	local input0 = make_fade_input(scene)
	local input1 = make_fade_input(scene)
	local mix_effect = scene:add_effect(MixEffect.new(), input0.wb_effect, input1.wb_effect)
	scene:finalize(true)  -- Only used live.

	return {
		scene = scene,
		input0 = input0,
		input1 = input1,
		mix_effect = mix_effect
	}
end
local fade_scene = make_fade_scene()

-- A scene to show a single input on screen.
local scene = Scene.new(16, 9)
local simple_scene = {
	scene = scene,
	input = scene:add_input(),
	resample_effect = scene:add_effect({ResampleEffect.new(), ResizeEffect.new(), IdentityEffect.new()}),
	wb_effect = scene:add_white_balance()
}
scene:finalize()

-- A scene to show a single static picture on screen.
local static_image = ImageInput.new("bg.jpeg")  -- Also used as input to other scenes.
local static_scene = Scene.new(16, 9)
static_scene:add_input(static_image)  -- Note: Locks this input to images only.
static_scene:finalize()

-- Set some global state. Unless marked otherwise, these can only be set once,
-- at the start of the program.
Nageru.set_num_channels(4)

-- Sets, for each channel, which signal it corresponds to (starting from 0).
-- The information is used for whether right-click on the channel should bring up
-- an input selector or not. Only call this for channels that actually correspond
-- directly to a signal (ie., live inputs, not live (0) or preview (1)).
Nageru.set_channel_signal(2, 0)
Nageru.set_channel_signal(3, 1)

-- Set whether a given channel supports setting white balance. (Default is false.)
Nageru.set_supports_wb(2, true)
Nageru.set_supports_wb(3, true)

-- These can be set at any time.
Nageru.set_channel_name(SBS_SIGNAL_NUM + 2, "Side-by-side")
Nageru.set_channel_name(STATIC_SIGNAL_NUM + 2, "Static picture")

-- API ENTRY POINT
-- Called every frame. Returns the color (if any) to paint around the given
-- channel. Returns a CSS color (typically to mark live and preview signals);
-- "transparent" is allowed.
-- Will never be called for live (0) or preview (1).
function channel_color(channel)
	if state.transition_type ~= NO_TRANSITION then
		if channel_involved_in(channel, state.transition_src_signal) or
		   channel_involved_in(channel, state.transition_dst_signal) then
			return "#f00"
		end
	else
		if channel_involved_in(channel, state.live_signal_num) then
			return "#f00"
		end
	end
	if channel_involved_in(channel, state.preview_signal_num) then
		return "#0f0"
	end
	return "transparent"
end

function is_plain_signal(num)
	return num == INPUT0_SIGNAL_NUM or num == INPUT1_SIGNAL_NUM
end

function channel_involved_in(channel, signal_num)
	if is_plain_signal(signal_num) then
		return channel == (signal_num + 2)
	end
	if signal_num == SBS_SIGNAL_NUM then
		return (channel == 2 or channel == 3)
	end
	if signal_num == STATIC_SIGNAL_NUM then
		return (channel == 5)
	end
	return false
end

function finish_transitions(t)
	if state.transition_type ~= NO_TRANSITION and t >= state.transition_end then
		state.live_signal_num = state.transition_dst_signal
		state.transition_type = NO_TRANSITION
	end
end

function in_transition(t)
       return t >= state.transition_start and t <= state.transition_end
end

-- API ENTRY POINT
-- Called every frame.
function get_transitions(t)
	if in_transition(t) then
		-- Transition already in progress, the only thing we can do is really
		-- cut to the preview. (TODO: Make an “abort” and/or “finish”, too?)
		return {"Cut"}
	end

	finish_transitions(t)

	if state.live_signal_num == state.preview_signal_num then
		-- No transitions possible.
		return {}
	end

	if (is_plain_signal(state.live_signal_num) or state.live_signal_num == STATIC_SIGNAL_NUM) and
	   (is_plain_signal(state.preview_signal_num) or state.preview_signal_num == STATIC_SIGNAL_NUM) then
		return {"Cut", "", "Fade"}
	end

	-- Various zooms.
	if state.live_signal_num == SBS_SIGNAL_NUM and is_plain_signal(state.preview_signal_num) then
		return {"Cut", "Zoom in"}
	elseif is_plain_signal(state.live_signal_num) and state.preview_signal_num == SBS_SIGNAL_NUM then
		return {"Cut", "Zoom out"}
	end

	return {"Cut"}
end

function swap_preview_live()
	local temp = state.live_signal_num
	state.live_signal_num = state.preview_signal_num
	state.preview_signal_num = temp
end

function start_transition(type_, t, duration)
	state.transition_start = t
	state.transition_end = t + duration
	state.transition_type = type_
	state.transition_src_signal = state.live_signal_num
	state.transition_dst_signal = state.preview_signal_num
	swap_preview_live()
end

-- API ENTRY POINT
-- Called when the user clicks a transition button.
function transition_clicked(num, t)
	if num == 0 then
		-- Cut.
		if in_transition(t) then
			-- Ongoing transition; finish it immediately before the cut.
			finish_transitions(state.transition_end)
		end

		swap_preview_live()
	elseif num == 1 then
		-- Zoom.
		finish_transitions(t)

		if state.live_signal_num == state.preview_signal_num then
			-- Nothing to do.
			return
		end

		if is_plain_signal(state.live_signal_num) and is_plain_signal(state.preview_signal_num) then
			-- We can't zoom between these. Just make a cut.
			io.write("Cutting from " .. state.live_signal_num .. " to " .. state.live_signal_num .. "\n")
			swap_preview_live()
			return
		end

		if (state.live_signal_num == SBS_SIGNAL_NUM and is_plain_signal(state.preview_signal_num)) or
		   (state.preview_signal_num == SBS_SIGNAL_NUM and is_plain_signal(state.live_signal_num)) then
			start_transition(ZOOM_TRANSITION, t, 1.0)
		end
	elseif num == 2 then
		finish_transitions(t)

		-- Fade.
		if (state.live_signal_num ~= state.preview_signal_num) and
		   (is_plain_signal(state.live_signal_num) or
		    state.live_signal_num == STATIC_SIGNAL_NUM) and
		   (is_plain_signal(state.preview_signal_num) or
		    state.preview_signal_num == STATIC_SIGNAL_NUM) then
			start_transition(FADE_TRANSITION, t, 1.0)
		else
			-- Fades involving SBS are ignored (we have no scene for it).
		end
	end
end

-- API ENTRY POINT
function channel_clicked(num)
	state.preview_signal_num = num
end

function setup_fade_input(state, input, signals, signal_num, width, height)
	if signal_num == STATIC_SIGNAL_NUM then
		input.input:display(static_image)
		input.wb_effect:disable()

		-- We assume this is already correctly scaled at load time.
		input.resample_effect:disable()
	else
		input.input:display(signal_num)
		input.wb_effect:enable()

		if (signals:get_width(signal_num) ~= width or signals:get_height(signal_num) ~= height) then
			input.resample_effect:enable()
			input.resample_effect:set_int("width", width)
			input.resample_effect:set_int("height", height)
		else
			input.resample_effect:disable()
		end
	end
end

function needs_scale(signals, signal_num, width, height)
	if signal_num == STATIC_SIGNAL_NUM then
		-- We assume this is already correctly scaled at load time.
		return false
	end
	assert(is_plain_signal(signal_num))
	return (signals:get_width(signal_num) ~= width or signals:get_height(signal_num) ~= height)
end

function setup_simple_input(state, signals, signal_num, width, height, hq)
	simple_scene.input:display(signal_num)
	if needs_scale(signals, signal_num, width, height) then
		if hq then
			simple_scene.resample_effect:choose(ResampleEffect)  -- High-quality resampling.
		else
			simple_scene.resample_effect:choose(ResizeEffect)  -- Low-quality resampling.
		end
		simple_scene.resample_effect:set_int("width", width)
		simple_scene.resample_effect:set_int("height", height)
	else
		simple_scene.resample_effect:disable()  -- No scaling.
	end
end

-- API ENTRY POINT
-- Called every frame. Get the scene for displaying at input <num>,
-- where 0 is live, 1 is preview, 2 is the first channel to display
-- in the bottom bar, and so on up to num_channels()+1. t is the
-- current time in seconds. width and height are the dimensions of
-- the output, although you can ignore them if you don't need them
-- (they're useful if you want to e.g. know what to resample by).
--
-- <signals> is basically an exposed InputState, which you can use to
-- query for information about the signals at the point of the current
-- frame. In particular, you can call get_frame_width() and get_frame_height()
-- for any signal number, and use that to e.g. assist in scene selection.
-- (You can also use get_width() and get_height(), which return the
-- _field_ size. This has half the height for interlaced signals.)
--
-- You should return scene to use, after having set any parameters you
-- want to set (through set_int() etc.). The parameters will be snapshot
-- at return time and used during rendering.
function get_scene(num, t, width, height, signals)
	local input_resolution = {}
	for signal_num=0,1 do
		local res = {
			width = signals:get_frame_width(signal_num),
			height = signals:get_frame_height(signal_num),
		}
		input_resolution[signal_num] = res

		local text_res = signals:get_human_readable_resolution(signal_num)
		Nageru.set_channel_name(signal_num + 2, "Input " .. (signal_num + 1) .. " (" .. text_res .. ")")
	end

	if num == 0 then  -- Live.
		finish_transitions(t)
		if state.transition_type == ZOOM_TRANSITION then
			-- Transition in or out of SBS.
			prepare_sbs_scene(state, calc_zoom_progress(state, t), state.transition_type, state.transition_src_signal, state.transition_dst_signal, width, height, input_resolution, true)
			return sbs_scene.scene
		elseif state.transition_type == NO_TRANSITION and state.live_signal_num == SBS_SIGNAL_NUM then
			-- Static SBS view.
			prepare_sbs_scene(state, 0.0, NO_TRANSITION, 0, SBS_SIGNAL_NUM, width, height, input_resolution, true)
			return sbs_scene.scene
		elseif state.transition_type == FADE_TRANSITION then
			setup_fade_input(state, fade_scene.input0, signals, state.transition_src_signal, width, height)
			setup_fade_input(state, fade_scene.input1, signals, state.transition_dst_signal, width, height)

			local tt = calc_fade_progress(t, state.transition_start, state.transition_end)
			fade_scene.mix_effect:set_float("strength_first", 1.0 - tt)
			fade_scene.mix_effect:set_float("strength_second", tt)

			return fade_scene.scene
		elseif is_plain_signal(state.live_signal_num) then
			setup_simple_input(state, signals, state.live_signal_num, width, height, true)
			return simple_scene.scene
		elseif state.live_signal_num == STATIC_SIGNAL_NUM then  -- Static picture.
			return static_scene
		else
			assert(false)
		end
	end
	if num == 1 then  -- Preview.
		num = state.preview_signal_num + 2
	end

	-- Individual preview inputs.
	if is_plain_signal(num - 2) then
		setup_simple_input(state, signals, num - 2, width, height, false)
		return simple_scene.scene
	end
	if num == SBS_SIGNAL_NUM + 2 then
		prepare_sbs_scene(state, 0.0, NO_TRANSITION, 0, SBS_SIGNAL_NUM, width, height, input_resolution, false)
		return sbs_scene.scene
	end
	if num == STATIC_SIGNAL_NUM + 2 then
		return static_scene
	end
end

function place_rectangle(input, x0, y0, x1, y1, screen_width, screen_height, input_width, input_height, hq)
	input.padding_effect:set_int("width", screen_width)
	input.padding_effect:set_int("height", screen_height)

	-- Cull.
	if x0 > screen_width or x1 < 0.0 or y0 > screen_height or y1 < 0.0 then
		input.resample_effect:choose(ResizeEffect)  -- Low-quality resizing.
		input.resample_effect:set_int("width", 1)
		input.resample_effect:set_int("height", 1)
		input.padding_effect:set_int("left", screen_width + 100)
		input.padding_effect:set_int("top", screen_height + 100)
		return
	end

	local srcx0 = 0.0
	local srcx1 = 1.0
	local srcy0 = 0.0
	local srcy1 = 1.0

	-- Clip.
	if x0 < 0 then
		srcx0 = -x0 / (x1 - x0)
		x0 = 0
	end
	if y0 < 0 then
		srcy0 = -y0 / (y1 - y0)
		y0 = 0
	end
	if x1 > screen_width then
		srcx1 = (screen_width - x0) / (x1 - x0)
		x1 = screen_width
	end
	if y1 > screen_height then
		srcy1 = (screen_height - y0) / (y1 - y0)
		y1 = screen_height
	end

	if hq then
		-- High-quality resampling. Go for the actual effect (returned by choose())
		-- since we want to set zoom_*, which will give an error if set on ResizeEffect.
		local resample_effect = input.resample_effect:choose(ResampleEffect)

		local x_subpixel_offset = x0 - math.floor(x0)
		local y_subpixel_offset = y0 - math.floor(y0)

		-- Resampling must be to an integral number of pixels. Round up,
		-- and then add an extra pixel so we have some leeway for the border.
		local width = math.ceil(x1 - x0) + 1
		local height = math.ceil(y1 - y0) + 1
		resample_effect:set_int("width", width)
		resample_effect:set_int("height", height)

		-- Correct the discrepancy with zoom. (This will leave a small
		-- excess edge of pixels and subpixels, which we'll correct for soon.)
		local zoom_x = (x1 - x0) / (width * (srcx1 - srcx0))
		local zoom_y = (y1 - y0) / (height * (srcy1 - srcy0))
		resample_effect:set_float("zoom_x", zoom_x)
		resample_effect:set_float("zoom_y", zoom_y)
		resample_effect:set_float("zoom_center_x", 0.0)
		resample_effect:set_float("zoom_center_y", 0.0)

		-- Padding must also be to a whole-pixel offset.
		input.padding_effect:set_int("left", math.floor(x0))
		input.padding_effect:set_int("top", math.floor(y0))

		-- Correct _that_ discrepancy by subpixel offset in the resampling.
		resample_effect:set_float("left", srcx0 * input_width - x_subpixel_offset / zoom_x)
		resample_effect:set_float("top", srcy0 * input_height - y_subpixel_offset / zoom_y)

		-- Finally, adjust the border so it is exactly where we want it.
		input.padding_effect:set_float("border_offset_left", x_subpixel_offset)
		input.padding_effect:set_float("border_offset_right", x1 - (math.floor(x0) + width))
		input.padding_effect:set_float("border_offset_top", y_subpixel_offset)
		input.padding_effect:set_float("border_offset_bottom", y1 - (math.floor(y0) + height))
	else
		-- Lower-quality simple resizing.
		input.resample_effect:choose(ResizeEffect)

		local width = round(x1 - x0)
		local height = round(y1 - y0)
		input.resample_effect:set_int("width", width)
		input.resample_effect:set_int("height", height)

		-- Padding must also be to a whole-pixel offset.
		input.padding_effect:set_int("left", math.floor(x0))
		input.padding_effect:set_int("top", math.floor(y0))

		-- No subpixel stuff.
		input.padding_effect:set_float("border_offset_left", 0.0)
		input.padding_effect:set_float("border_offset_right", 0.0)
		input.padding_effect:set_float("border_offset_top", 0.0)
		input.padding_effect:set_float("border_offset_bottom", 0.0)
	end
end

-- This is broken, of course (even for positive numbers), but Lua doesn't give us access to real rounding.
function round(x)
	return math.floor(x + 0.5)
end

function lerp(a, b, t)
	return a + (b - a) * t
end

function lerp_pos(a, b, t)
	return {
		x0 = lerp(a.x0, b.x0, t),
		y0 = lerp(a.y0, b.y0, t),
		x1 = lerp(a.x1, b.x1, t),
		y1 = lerp(a.y1, b.y1, t)
	}
end

function pos_from_top_left(x, y, width, height, screen_width, screen_height)
	local xs = screen_width / 1280.0
	local ys = screen_height / 720.0
	return {
		x0 = round(xs * x),
		y0 = round(ys * y),
		x1 = round(xs * (x + width)),
		y1 = round(ys * (y + height))
	}
end

function prepare_sbs_scene(state, t, transition_type, src_signal, dst_signal, screen_width, screen_height, input_resolution, hq)
	-- First input is positioned (16,48) from top-left.
	-- Second input is positioned (16,48) from the bottom-right.
	local pos0 = pos_from_top_left(16, 48, 848, 477, screen_width, screen_height)
	local pos1 = pos_from_top_left(1280 - 384 - 16, 720 - 216 - 48, 384, 216, screen_width, screen_height)

	local pos_fs = { x0 = 0, y0 = 0, x1 = screen_width, y1 = screen_height }
	local affine_param
	if transition_type == NO_TRANSITION then
		-- Static SBS view.
		affine_param = { sx = 1.0, sy = 1.0, tx = 0.0, ty = 0.0 }   -- Identity.
	else
		-- Zooming to/from SBS view into or out of a single view.
		assert(transition_type == ZOOM_TRANSITION)
		local signal, real_t
		if src_signal == SBS_SIGNAL_NUM then
			signal = dst_signal
			real_t = t
		else
			assert(dst_signal == SBS_SIGNAL_NUM)
			signal = src_signal
			real_t = 1.0 - t
		end

		if signal == INPUT0_SIGNAL_NUM then
			affine_param = find_affine_param(pos0, lerp_pos(pos0, pos_fs, real_t))
		elseif signal == INPUT1_SIGNAL_NUM then
			affine_param = find_affine_param(pos1, lerp_pos(pos1, pos_fs, real_t))
		end
	end

	-- NOTE: input_resolution is not 1-indexed, unlike usual Lua arrays.
	place_rectangle_with_affine(sbs_scene.input0, pos0, affine_param, screen_width, screen_height, input_resolution[0].width, input_resolution[0].height, hq)
	place_rectangle_with_affine(sbs_scene.input1, pos1, affine_param, screen_width, screen_height, input_resolution[1].width, input_resolution[1].height, hq)
end

-- Find the transformation that changes the first rectangle to the second one.
function find_affine_param(a, b)
	local sx = (b.x1 - b.x0) / (a.x1 - a.x0)
	local sy = (b.y1 - b.y0) / (a.y1 - a.y0)
	return {
		sx = sx,
		sy = sy,
		tx = b.x0 - a.x0 * sx,
		ty = b.y0 - a.y0 * sy
	}
end

function place_rectangle_with_affine(input, pos, aff, screen_width, screen_height, input_width, input_height, hq)
	local x0 = pos.x0 * aff.sx + aff.tx
	local x1 = pos.x1 * aff.sx + aff.tx
	local y0 = pos.y0 * aff.sy + aff.ty
	local y1 = pos.y1 * aff.sy + aff.ty

	place_rectangle(input, x0, y0, x1, y1, screen_width, screen_height, input_width, input_height, hq)
end

function calc_zoom_progress(state, t)
	if t < state.transition_start then
		return 0.0
	elseif t > state.transition_end then
		return 1.0
	else
		local tt = (t - state.transition_start) / (state.transition_end - state.transition_start)
		-- Smooth it a bit.
		return math.sin(tt * 3.14159265358 * 0.5)
	end
end

function calc_fade_progress(t, transition_start, transition_end)
	local tt = (t - transition_start) / (transition_end - transition_start)
	if tt < 0.0 then
		return 0.0
	elseif tt > 1.0 then
		return 1.0
	end

	-- Make the fade look maybe a tad more natural, by pumping it
	-- through a sigmoid function.
	tt = 10.0 * tt - 5.0
	tt = 1.0 / (1.0 + math.exp(-tt))

	return tt
end
