#ifndef _MIDI_DEVICE_H
#define _MIDI_DEVICE_H 1

// MIDIDevice is a class that pools incoming MIDI messages from
// all MIDI devices in the system, decodes them and sends them on.

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

typedef struct snd_seq_addr snd_seq_addr_t;
typedef struct snd_seq_event snd_seq_event_t;
typedef struct _snd_seq snd_seq_t;

class MIDIReceiver {
public:
	// Pitch bend events are received as a virtual controller with
	// range -8192..8191 instead of 0..127 (but see the comment
	// in map_controller_to_float() in midi_mapper.cpp).
	static constexpr int PITCH_BEND_CONTROLLER = 128;

	virtual ~MIDIReceiver() {}
	virtual void controller_received(int controller, int value) = 0;
	virtual void note_on_received(int note) = 0;
	virtual void update_num_subscribers(unsigned num_subscribers) = 0;
};

class MIDIDevice {
public:
	struct LightKey {
		enum { NOTE, CONTROLLER } type;
		unsigned number;

		bool operator< (const LightKey& other) const
		{
			if (type != other.type) {
				return type < other.type;
			}
			return number < other.number;
		}
	};

	MIDIDevice(MIDIReceiver *receiver);
	~MIDIDevice();
	void start_thread();

	void update_lights(const std::map<LightKey, uint8_t> &active_lights)
	{
		std::lock_guard<std::recursive_mutex> lock(mu);
		update_lights_lock_held(active_lights);
	}

private:
	void thread_func();
	void handle_event(snd_seq_t *seq, snd_seq_event_t *event);
	void subscribe_to_port_lock_held(snd_seq_t *seq, const snd_seq_addr_t &addr);
	void update_lights_lock_held(const std::map<LightKey, uint8_t> &active_lights);

	std::atomic<bool> should_quit{false};
	int should_quit_fd;

	// Recursive because the MIDI receiver may update_lights() back while we are sending it stuff.
	// TODO: Do we need this anymore after receiver is not under the lock?
	mutable std::recursive_mutex mu;
	MIDIReceiver *receiver;  // _Not_ under <mu>; everything on it should be thread-safe.

	std::thread midi_thread;
	std::map<LightKey, uint8_t> current_light_status;  // Keyed by note number. Under <mu>.
	snd_seq_t *alsa_seq{nullptr};  // Under <mu>.
	int alsa_queue_id{-1};  // Under <mu>.
	std::atomic<int> num_subscribed_ports{0};
};

#endif  // !defined(_MIDI_DEVICE_H)
