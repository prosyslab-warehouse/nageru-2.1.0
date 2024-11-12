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
--
-- This is a much simpler theme than the default theme; it only allows you to
-- switch between inputs and set white balance, no transitions or the likes.
-- Thus, it should be simpler to understand.

local input_neutral_color = {{0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}}

local live_signal_num = 0
local preview_signal_num = 1

local img = ImageInput.new("bg.jpeg")

local scene = Scene.new(16, 9)
local input = scene:add_input()
local wb_effect = scene:add_effect(WhiteBalanceEffect.new())
scene:finalize()

-- Set some global state. Unless marked otherwise, these can only be set once,
-- at the start of the program.
Nageru.set_num_channels(2)

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
Nageru.set_channel_name(2, "First input")
Nageru.set_channel_name(3, "Second input")

-- API ENTRY POINT
-- Called every frame. Returns the color (if any) to paint around the given
-- channel. Returns a CSS color (typically to mark live and preview signals);
-- "transparent" is allowed.
-- Will never be called for live (0) or preview (1).
function channel_color(channel)
	return "transparent"
end

-- API ENTRY POINT
-- Gets called with a new gray point when the white balance is changing.
-- The color is in linear light (not sRGB gamma).
function set_wb(channel, red, green, blue)
	if channel == 2 then
		input_neutral_color[1] = { red, green, blue }
	elseif channel == 3 then
		input_neutral_color[2] = { red, green, blue }
	end
end

-- API ENTRY POINT
-- Called every frame.
function get_transitions(t)
	if live_signal_num == preview_signal_num then
		-- No transitions possible.
		return {}
	else
		return {"Cut"}
	end
end

-- API ENTRY POINT
-- Called when the user clicks a transition button. For our case,
-- we only do cuts, so we ignore the parameters; just switch live and preview.
function transition_clicked(num, t)
	local temp = live_signal_num
	live_signal_num = preview_signal_num
	preview_signal_num = temp
end

-- API ENTRY POINT
function channel_clicked(num)
	preview_signal_num = num
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
-- You should return the scene to use, after having set any parameters you
-- want to set (through set_int() etc.). The parameters will be snapshot
-- at return time and used during rendering.
function get_scene(num, t, width, height, signals)
	local signal_num
	if num == 0 then  -- Live (right pane).
		signal_num = live_signal_num
	elseif num == 1 then  -- Preview (left pane).
		signal_num = preview_signal_num
	else  -- One of the two previews (bottom panes).
		signal_num = num - 2
	end

	if num == 3 then
		input:display(img)
	else
		input:display(signal_num)
	end

	local color = input_neutral_color[signal_num + 1]
	wb_effect:set_vec3("neutral_color", color[1], color[2], color[3])

	return scene
end
