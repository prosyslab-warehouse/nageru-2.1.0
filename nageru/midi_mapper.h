#ifndef _MIDI_MAPPER_H
#define _MIDI_MAPPER_H 1

// MIDIMapper is a class that gets incoming MIDI messages from mixer
// controllers (ie., it is not meant to be used with regular instruments)
// via MIDIDevice, interprets them according to a device-specific, user-defined
// mapping, and calls back into a receiver (typically the MainWindow).
// This way, it is possible to control audio functionality using physical
// pots and faders instead of the mouse.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "defs.h"
#include "shared/midi_device.h"

class MIDIMappingProto;

// Interface for receiving interpreted controller messages.
class ControllerReceiver {
public:
	virtual ~ControllerReceiver() {}

	// All values are [0.0, 1.0].
	virtual void set_locut(float value) = 0;
	virtual void set_limiter_threshold(float value) = 0;
	virtual void set_makeup_gain(float value) = 0;

	virtual void set_stereo_width(unsigned bus_idx, float value) = 0;
	virtual void set_treble(unsigned bus_idx, float value) = 0;
	virtual void set_mid(unsigned bus_idx, float value) = 0;
	virtual void set_bass(unsigned bus_idx, float value) = 0;
	virtual void set_gain(unsigned bus_idx, float value) = 0;
	virtual void set_compressor_threshold(unsigned bus_idx, float value) = 0;
	virtual void set_fader(unsigned bus_idx, float value) = 0;

	virtual void toggle_mute(unsigned bus_idx) = 0;
	virtual void toggle_locut(unsigned bus_idx) = 0;
	virtual void toggle_auto_gain_staging(unsigned bus_idx) = 0;
	virtual void toggle_compressor(unsigned bus_idx) = 0;
	virtual void clear_peak(unsigned bus_idx) = 0;
	virtual void toggle_limiter() = 0;
	virtual void toggle_auto_makeup_gain() = 0;

	// Non-audio events.
	virtual void switch_video_channel(int channel_number) = 0;
	virtual void apply_transition(int transition_number) = 0;
	virtual void prev_audio_view() = 0;
	virtual void next_audio_view() = 0;
	virtual void begin_new_segment() = 0;
	virtual void exit() = 0;

	// Signals to highlight controls to mark them to the user
	// as MIDI-controllable (or not).
	virtual void clear_all_highlights() = 0;

	virtual void highlight_locut(bool highlight) = 0;
	virtual void highlight_limiter_threshold(bool highlight) = 0;
	virtual void highlight_makeup_gain(bool highlight) = 0;

	virtual void highlight_stereo_width(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_treble(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_mid(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_bass(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_gain(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_compressor_threshold(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_fader(unsigned bus_idx, bool highlight) = 0;

	virtual void highlight_mute(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_toggle_locut(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_toggle_auto_gain_staging(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_toggle_compressor(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_clear_peak(unsigned bus_idx, bool highlight) = 0;
	virtual void highlight_toggle_limiter(bool highlight) = 0;
	virtual void highlight_toggle_auto_makeup_gain(bool highlight) = 0;

	// Raw events; used for the editor dialog only.
	virtual void controller_changed(unsigned controller) = 0;
	virtual void note_on(unsigned note) = 0;
};

class MIDIMapper : public MIDIReceiver {
public:
	MIDIMapper(ControllerReceiver *receiver);
	virtual ~MIDIMapper();
	void set_midi_mapping(const MIDIMappingProto &new_mapping);
	void start_thread();
	const MIDIMappingProto &get_current_mapping() const;

	// Overwrites and returns the previous value.
	ControllerReceiver *set_receiver(ControllerReceiver *new_receiver);

	void refresh_highlights();
	void refresh_lights();

	void set_has_peaked(unsigned bus_idx, bool has_peaked)
	{
		this->has_peaked[bus_idx] = has_peaked;
	}

	// MIDIReceiver.
	void controller_received(int controller, int value) override;
	void note_on_received(int note) override;
	void update_num_subscribers(unsigned num_subscribers) override;

private:
	void match_controller(int controller, int field_number, int bank_field_number, float value, std::function<void(unsigned, float)> func);
	void match_button(int note, int field_number, int bank_field_number, std::function<void(unsigned)> func);
	bool has_active_controller(unsigned bus_idx, int field_number, int bank_field_number);  // Also works for buttons.
	bool bank_mismatch(int bank_field_number);

	void update_highlights();

	void update_lights_lock_held();
	void activate_lights_all_buses(int field_number, std::map<MIDIDevice::LightKey, uint8_t> *active_lights);

	std::atomic<bool> should_quit{false};

	std::atomic<bool> has_peaked[MAX_BUSES] {{ false }};

	mutable std::mutex mu;
	ControllerReceiver *receiver;  // Under <mu>.
	std::unique_ptr<MIDIMappingProto> mapping_proto;  // Under <mu>.
	int num_controller_banks;  // Under <mu>.
	std::atomic<int> current_controller_bank{0};
	std::atomic<int> num_subscribed_ports{0};

	MIDIDevice midi_device;
};

bool load_midi_mapping_from_file(const std::string &filename, MIDIMappingProto *new_mapping);
bool save_midi_mapping_to_file(const MIDIMappingProto &mapping_proto, const std::string &filename);

#endif  // !defined(_MIDI_MAPPER_H)
