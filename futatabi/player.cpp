#include "player.h"

#include "clip_list.h"
#include "defs.h"
#include "flags.h"
#include "frame_on_disk.h"
#include "jpeg_frame_view.h"
#include "shared/context.h"
#include "shared/ffmpeg_raii.h"
#include "shared/httpd.h"
#include "shared/metrics.h"
#include "shared/mux.h"
#include "shared/timebase.h"
#include "video_stream.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <movit/util.h>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <vector>

using namespace std;
using namespace std::chrono;

extern HTTPD *global_httpd;

void Player::thread_func(AVFormatContext *file_avctx)
{
	pthread_setname_np(pthread_self(), "Player");

	QSurface *surface = create_surface();
	QOpenGLContext *context = create_context(surface);
	if (!make_current(context, surface)) {
		printf("oops\n");
		abort();
	}

	check_error();

	// Create the VideoStream object, now that we have an OpenGL context.
	if (stream_output != NO_STREAM_OUTPUT) {
		video_stream.reset(new VideoStream(file_avctx));
		video_stream->start();
	}

	check_error();

	while (!should_quit) {
		play_playlist_once();
	}
}

namespace {

double calc_progress(const Clip &clip, int64_t pts)
{
	return double(pts - clip.pts_in) / (clip.pts_out - clip.pts_in);
}

void do_splice(const vector<ClipWithID> &new_list, size_t playing_index1, ssize_t playing_index2, vector<ClipWithID> *old_list)
{
	assert(playing_index2 == -1 || size_t(playing_index2) == playing_index1 + 1);

	// First see if we can do the simple thing; find an element in the new
	// list that we are already playing, which will serve as our splice point.
	int splice_start_new_list = -1;
	for (size_t clip_idx = 0; clip_idx < new_list.size(); ++clip_idx) {
		if (new_list[clip_idx].id == (*old_list)[playing_index1].id) {
			splice_start_new_list = clip_idx + 1;
		} else if (playing_index2 != -1 && new_list[clip_idx].id == (*old_list)[playing_index2].id) {
			splice_start_new_list = clip_idx + 1;
		}
	}
	if (splice_start_new_list == -1) {
		// OK, so the playing items are no longer in the new list. Most likely,
		// that means we deleted some range that included them. But the ones
		// before should stay put -- and we don't want to play them. So find
		// the ones that we've already played, and ignore them. Hopefully,
		// they're contiguous; the last one that's not seen will be our cut point.
		//
		// Keeping track of the playlist range explicitly in the UI would remove
		// the need for these heuristics, but it would probably also mean we'd
		// have to lock the playing clip, which sounds annoying.
		unordered_map<uint64_t, size_t> played_ids;
		for (size_t clip_idx = 0; clip_idx < playing_index1; ++old_list) {
			played_ids.emplace((*old_list)[clip_idx].id, clip_idx);
		}
		for (size_t clip_idx = 0; clip_idx < new_list.size(); ++clip_idx) {
			if (played_ids.count(new_list[clip_idx].id)) {
				splice_start_new_list = clip_idx + 1;
			}
		}

		if (splice_start_new_list == -1) {
			// OK, we didn't find any matches; the lists are totally distinct.
			// So probably the entire thing was deleted; leave it alone.
			return;
		}
	}

	size_t splice_start_old_list = ((playing_index2 == -1) ? playing_index1 : playing_index2) + 1;
	old_list->erase(old_list->begin() + splice_start_old_list, old_list->end());
	old_list->insert(old_list->end(), new_list.begin() + splice_start_new_list, new_list.end());
}

// Keeps track of the various timelines (wall clock time, output pts,
// position in the clip we are playing). Generally we keep an origin
// and assume we increase linearly from there; the intention is to
// avoid getting compounded accuracy errors, although with double,
// that is perhaps overkill. (Whenever we break the linear assumption,
// we need to reset said origin.)
class TimelineTracker
{
public:
	struct Instant {
		steady_clock::time_point wallclock_time;
		int64_t in_pts;
		int64_t out_pts;
		int64_t frameno;
	};

	TimelineTracker(double master_speed, int64_t out_pts_origin)
		: master_speed(master_speed), last_out_pts(out_pts_origin) {
		origin.out_pts = out_pts_origin;
		master_speed_ease_target = master_speed;  // Keeps GCC happy.
	}

	void new_clip(steady_clock::time_point wallclock_origin, const Clip *clip, int64_t start_pts_offset)
	{
		this->clip = clip;
		origin.wallclock_time = wallclock_origin;
		origin.in_pts = clip->pts_in + start_pts_offset;
		origin.out_pts = last_out_pts;
		origin.frameno = 0;
	}

	// Returns the current time for said frame.
	Instant advance_to_frame(int64_t frameno);

	int64_t get_in_pts_origin() const { return origin.in_pts; }
	bool playing_at_normal_speed() const {
		if (in_easing) return false;

		const double effective_speed = clip->speed * master_speed;
		return effective_speed >= 0.999 && effective_speed <= 1.001;
	}

	void snap_by(int64_t offset) {
		if (in_easing) {
			// Easing will normally aim for a snap at the very end,
			// so don't disturb it by jittering during the ease.
			return;
		}
		origin.in_pts += offset;
	}

	void change_master_speed(double new_master_speed, Instant now);

	float in_master_speed(float speed) const {
		return (!in_easing && fabs(master_speed - speed) < 1e-6);
	}

	// Instead of changing the speed instantly, change it over the course of
	// about 200 ms. This is a simple linear ramp; I tried various forms of
	// Bézier curves for more elegant/dramatic changing, but it seemed linear
	// looked just as good in practical video.
	void start_easing(double new_master_speed, int64_t length_out_pts, Instant now);

	int64_t find_easing_length(double master_speed_target, int64_t length_out_pts, const vector<FrameOnDisk> &frames, Instant now);

private:
	// Find out how far we are into the easing curve (0..1).
	// We use this to adjust the input pts.
	double find_ease_t(double out_pts) const;
	double easing_out_pts_adjustment(double out_pts) const;

	double master_speed;
	const Clip *clip = nullptr;
	Instant origin;
	int64_t last_out_pts;

	// If easing between new and old master speeds.
	bool in_easing = false;
	int64_t ease_started_pts = 0;
	double master_speed_ease_target;
	int64_t ease_length_out_pts = 0;
};

TimelineTracker::Instant TimelineTracker::advance_to_frame(int64_t frameno)
{
	Instant ret;
	double in_pts_double = origin.in_pts + TIMEBASE * clip->speed * (frameno - origin.frameno) * master_speed / global_flags.output_framerate;
	double out_pts_double = origin.out_pts + TIMEBASE * (frameno - origin.frameno) / global_flags.output_framerate;

	if (in_easing) {
		double in_pts_adjustment = easing_out_pts_adjustment(out_pts_double) * clip->speed;
		in_pts_double += in_pts_adjustment;
	}

	ret.in_pts = lrint(in_pts_double);
	ret.out_pts = lrint(out_pts_double);
	ret.wallclock_time = origin.wallclock_time + microseconds(lrint((out_pts_double - origin.out_pts) * 1e6 / TIMEBASE));
	ret.frameno = frameno;

	last_out_pts = ret.out_pts;

	if (in_easing && ret.out_pts >= ease_started_pts + ease_length_out_pts) {
		// We have ended easing. Add what we need for the entire easing period,
		// then _actually_ change the speed as we go back into normal mode.
		origin.out_pts += easing_out_pts_adjustment(out_pts_double);
		change_master_speed(master_speed_ease_target, ret);
		in_easing = false;
	}

	return ret;
}

void TimelineTracker::change_master_speed(double new_master_speed, Instant now)
{
	master_speed = new_master_speed;

	// Reset the origins, since the calculations depend on linear interpolation
	// based on the master speed.
	origin = now;
}

void TimelineTracker::start_easing(double new_master_speed, int64_t length_out_pts, Instant now)
{
	if (in_easing) {
		// Apply whatever we managed to complete of the previous easing.
		origin.out_pts += easing_out_pts_adjustment(now.out_pts);
		double reached_speed = master_speed + (master_speed_ease_target - master_speed) * find_ease_t(now.out_pts);
		change_master_speed(reached_speed, now);
	}
	in_easing = true;
	ease_started_pts = now.out_pts;
	master_speed_ease_target = new_master_speed;
	ease_length_out_pts = length_out_pts;
}

double TimelineTracker::find_ease_t(double out_pts) const
{
	return (out_pts - ease_started_pts) / double(ease_length_out_pts);
}

double TimelineTracker::easing_out_pts_adjustment(double out_pts) const
{
	double t = find_ease_t(out_pts);
	double area_factor = (master_speed_ease_target - master_speed) * ease_length_out_pts;
	double val = 0.5 * min(t, 1.0) * min(t, 1.0) * area_factor;
	if (t > 1.0) {
		val += area_factor * (t - 1.0);
	}
	return val;
}

int64_t TimelineTracker::find_easing_length(double master_speed_target, int64_t desired_length_out_pts, const vector<FrameOnDisk> &frames, Instant now)
{
	// Find out what frame we would have hit (approximately) with the given ease length.
	double in_pts_length = 0.5 * (master_speed_target + master_speed) * desired_length_out_pts * clip->speed;
	const int input_frame_num = distance(
		frames.begin(),
		find_first_frame_at_or_after(frames, lrint(now.in_pts + in_pts_length)));

	// Round length_out_pts to the nearest amount of whole frames.
	const double frame_length = TIMEBASE / global_flags.output_framerate;
	const int length_out_frames = lrint(desired_length_out_pts / frame_length);

	// Time the easing so that we aim at 200 ms (or whatever length_out_pts
	// was), but adjust it so that we hit exactly on a frame. Unless we are
	// somehow unlucky and run in the middle of a bad fade, this should
	// lock us nicely into a cadence where we hit original frames (of course
	// assuming the new speed is a reasonable ratio).
	//
	// Assume for a moment that we are easing into a slowdown, and that
	// we're slightly too late to hit the frame we want to. This means that
	// we can shorten the ease a bit; this chops some of the total integrated
	// velocity and arrive at the frame a bit sooner. Solve for the time
	// we want to shorten the ease by (let's call it x, where the original
	// length of the ease is called len) such that we hit exactly the in
	// pts at the right time:
	//
	//   0.5 * (mst + ms) * (len - x) * cs + mst * x * cs = desired_len_in_pts
	//
	// gives
	//
	//   x = (2 * desired_len_in_pts / cs - (mst + ms) * len) / (mst - ms)
	//
	// Conveniently, this holds even if we are too early; a negative x
	// (surprisingly!) gives a lenghtening such that we don't hit the desired
	// frame, but hit one slightly later. (x larger than len means that
	// it's impossible to hit the desired frame, even if we dropped the ease
	// altogether and just changed speeds instantly.) We also have sign invariance,
	// so that these properties hold even if we are speeding up, not slowing
	// down. Together, these two properties mean that we can cast a fairly
	// wide net, trying various input and output frames and seeing which ones
	// can be matched up with a minimal change to easing time. (This lets us
	// e.g. end the ease close to the midpoint between two endpoint frames
	// even if we don't know the frame rate, or deal fairly robustly with
	// dropped input frames.) Many of these will give us the same answer,
	// but that's fine, because the ease length is the only output.
	int64_t best_length_out_pts = TIMEBASE * 10;  // Infinite.
	for (int output_frame_offset = -2; output_frame_offset <= 2; ++output_frame_offset) {
		int64_t aim_length_out_pts = lrint((length_out_frames + output_frame_offset) * frame_length);
		if (aim_length_out_pts < 0) {
			continue;
		}

		for (int input_frame_offset = -2; input_frame_offset <= 2; ++input_frame_offset) {
			if (input_frame_num + input_frame_offset < 0 ||
			    input_frame_num + input_frame_offset >= int(frames.size())) {
				continue;
			}
			const int64_t in_pts = frames[input_frame_num + input_frame_offset].pts;
			double shorten_by_out_pts = (2.0 * (in_pts - now.in_pts) / clip->speed - (master_speed_target + master_speed) * aim_length_out_pts) / (master_speed_target - master_speed);
			int64_t length_out_pts = lrint(aim_length_out_pts - shorten_by_out_pts);

			if (length_out_pts >= 0 &&
			    abs(length_out_pts - desired_length_out_pts) < abs(best_length_out_pts - desired_length_out_pts)) {
				best_length_out_pts = length_out_pts;
			}
		}
	}

	// If we need more than two seconds of easing, we give up --
	// this can happen if we're e.g. going from 101% to 100%.
	// If so, it would be better to let other mechanisms, such as the switch
	// to the next clip, deal with getting us back into sync.
	if (best_length_out_pts > TIMEBASE * 2) {
		return desired_length_out_pts;
	} else {
		return best_length_out_pts;
	}
}

}  // namespace

void Player::play_playlist_once()
{
	vector<ClipWithID> clip_list;
	bool clip_ready;
	steady_clock::time_point before_sleep = steady_clock::now();
	string pause_status;

	// Wait until we're supposed to play something.
	{
		unique_lock<mutex> lock(queue_state_mu);
		playing = false;
		clip_ready = new_clip_changed.wait_for(lock, milliseconds(100), [this] {
			return should_quit || new_clip_ready;
		});
		if (should_quit) {
			return;
		}
		if (clip_ready) {
			new_clip_ready = false;
			playing = true;
			clip_list = move(queued_clip_list);
			queued_clip_list.clear();
			assert(!clip_list.empty());
			assert(!splice_ready);  // This corner case should have been handled in splice_play().
		} else {
			pause_status = this->pause_status;
		}
	}

	steady_clock::duration time_slept = steady_clock::now() - before_sleep;
	int64_t slept_pts = duration_cast<duration<size_t, TimebaseRatio>>(time_slept).count();
	if (slept_pts > 0) {
		if (video_stream != nullptr) {
			// Add silence for the time we're waiting.
			video_stream->schedule_silence(steady_clock::now(), pts, slept_pts, QueueSpotHolder());
		}
		pts += slept_pts;
	}

	if (!clip_ready) {
		if (video_stream != nullptr) {
			++metric_refresh_frame;
			string subtitle = "Futatabi " NAGERU_VERSION ";PAUSED;0.000;" + pause_status;
			video_stream->schedule_refresh_frame(steady_clock::now(), pts, /*display_func=*/nullptr, QueueSpotHolder(),
				subtitle);
		}
		return;
	}

	should_skip_to_next = false;  // To make sure we don't have a lingering click from before play.
	steady_clock::time_point origin = steady_clock::now();  // TODO: Add a 100 ms buffer for ramp-up?
	TimelineTracker timeline(start_master_speed, pts);
	timeline.new_clip(origin, &clip_list[0].clip, /*pts_offset=*/0);
	for (size_t clip_idx = 0; clip_idx < clip_list.size(); ++clip_idx) {
		const Clip *clip = &clip_list[clip_idx].clip;
		const Clip *next_clip = (clip_idx + 1 < clip_list.size()) ? &clip_list[clip_idx + 1].clip : nullptr;

		double next_clip_fade_time = -1.0;
		if (next_clip != nullptr) {
			double duration_this_clip = double(clip->pts_out - timeline.get_in_pts_origin()) / TIMEBASE / clip->speed;
			double duration_next_clip = double(next_clip->pts_out - next_clip->pts_in) / TIMEBASE / clip->speed;
			next_clip_fade_time = min(min(duration_this_clip, duration_next_clip), clip->fade_time_seconds);
		}

		int stream_idx = clip->stream_idx;

		// Start playing exactly at a frame.
		// TODO: Snap secondary (fade-to) clips in the same fashion
		// so that we don't get jank here).
		{
			lock_guard<mutex> lock(frame_mu);

			// Find the first frame such that frame.pts <= in_pts.
			auto it = find_last_frame_before(frames[stream_idx], timeline.get_in_pts_origin());
			if (it != frames[stream_idx].end()) {
				timeline.snap_by(it->pts - timeline.get_in_pts_origin());
			}
		}

		steady_clock::time_point next_frame_start;
		for (int64_t frameno = 0; !should_quit; ++frameno) {  // Ends when the clip ends.
			TimelineTracker::Instant instant = timeline.advance_to_frame(frameno);
			int64_t in_pts = instant.in_pts;
			pts = instant.out_pts;
			next_frame_start = instant.wallclock_time;

			float new_master_speed = change_master_speed.exchange(0.0f / 0.0f);
			if (!std::isnan(new_master_speed) && !timeline.in_master_speed(new_master_speed)) {
				int64_t ease_length_out_pts = TIMEBASE / 5;  // 200 ms.
				int64_t recommended_pts_length = timeline.find_easing_length(new_master_speed, ease_length_out_pts, frames[clip->stream_idx], instant);
				timeline.start_easing(new_master_speed, recommended_pts_length, instant);
			}

			if (should_skip_to_next.exchange(false)) {  // Test and clear.
				Clip *clip = &clip_list[clip_idx].clip;  // Get a non-const pointer.
				clip->pts_out = std::min<int64_t>(clip->pts_out, llrint(in_pts + clip->fade_time_seconds * clip->speed * TIMEBASE));
			}

			if (in_pts >= clip->pts_out) {
				break;
			}

			// Only play audio if we're within 0.1% of normal speed. We could do
			// stretching or pitch shift later if it becomes needed.
			const bool play_audio = timeline.playing_at_normal_speed();

			{
				lock_guard<mutex> lock(queue_state_mu);
				if (splice_ready) {
					if (next_clip == nullptr) {
						do_splice(to_splice_clip_list, clip_idx, -1, &clip_list);
					} else {
						do_splice(to_splice_clip_list, clip_idx, clip_idx + 1, &clip_list);
					}
					to_splice_clip_list.clear();
					splice_ready = false;

					// Refresh the clip pointer, since the clip list may have been reallocated.
					clip = &clip_list[clip_idx].clip;

					// Recompute next_clip and any needed fade times, since the next clip may have changed
					// (or we may have gone from no new clip to having one, or the other way).
					next_clip = (clip_idx + 1 < clip_list.size()) ? &clip_list[clip_idx + 1].clip : nullptr;
					if (next_clip != nullptr) {
						double duration_this_clip = double(clip->pts_out - timeline.get_in_pts_origin()) / TIMEBASE / clip->speed;
						double duration_next_clip = double(next_clip->pts_out - next_clip->pts_in) / TIMEBASE / clip->speed;
						next_clip_fade_time = min(min(duration_this_clip, duration_next_clip), clip->fade_time_seconds);
					}
				}
			}

			steady_clock::duration time_behind = steady_clock::now() - next_frame_start;
			metric_player_ahead_seconds.count_event(-duration<double>(time_behind).count());
			if (stream_output != FILE_STREAM_OUTPUT && time_behind >= milliseconds(200)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping a frame (no matter the type).\n",
				        lrint(1e3 * duration<double>(time_behind).count()));
				++metric_dropped_unconditional_frame;
				continue;
			}

			// pts not affected by the swapping below.
			int64_t in_pts_for_progress = in_pts, in_pts_secondary_for_progress = -1;

			int primary_stream_idx = stream_idx;
			FrameOnDisk secondary_frame;
			int secondary_stream_idx = -1;
			float fade_alpha = 0.0f;
			double time_left_this_clip = double(clip->pts_out - in_pts) / TIMEBASE / clip->speed;
			if (next_clip != nullptr && time_left_this_clip <= next_clip_fade_time) {
				// We're in a fade to the next clip->
				secondary_stream_idx = next_clip->stream_idx;
				int64_t in_pts_secondary = lrint(next_clip->pts_in + (next_clip_fade_time - time_left_this_clip) * TIMEBASE * clip->speed);
				in_pts_secondary_for_progress = in_pts_secondary;
				fade_alpha = 1.0f - time_left_this_clip / next_clip_fade_time;

				// If more than half-way through the fade, interpolate the next clip
				// instead of the current one, since it's more visible.
				if (fade_alpha >= 0.5f) {
					swap(primary_stream_idx, secondary_stream_idx);
					swap(in_pts, in_pts_secondary);
					fade_alpha = 1.0f - fade_alpha;
				}

				FrameOnDisk frame_lower, frame_upper;
				bool ok = find_surrounding_frames(in_pts_secondary, secondary_stream_idx, &frame_lower, &frame_upper);

				if (ok) {
					secondary_frame = frame_lower;
				} else {
					secondary_stream_idx = -1;
				}
			}

			// NOTE: None of this will take into account any snapping done below.
			double clip_progress = calc_progress(*clip, in_pts_for_progress);
			map<uint64_t, double> progress{ { clip_list[clip_idx].id, clip_progress } };
			TimeRemaining time_remaining;
			if (next_clip != nullptr && time_left_this_clip <= next_clip_fade_time) {
				double next_clip_progress = calc_progress(*next_clip, in_pts_secondary_for_progress);
				progress[clip_list[clip_idx + 1].id] = next_clip_progress;
				time_remaining = compute_time_left(clip_list, clip_idx + 1, next_clip_progress);
			} else {
				time_remaining = compute_time_left(clip_list, clip_idx, clip_progress);
			}
			if (progress_callback != nullptr) {
				progress_callback(progress, time_remaining);
			}

			FrameOnDisk frame_lower, frame_upper;
			bool ok = find_surrounding_frames(in_pts, primary_stream_idx, &frame_lower, &frame_upper);
			if (!ok) {
				break;
			}

			// Wait until we should, or (given buffering) can, output the frame.
			{
				unique_lock<mutex> lock(queue_state_mu);
				if (video_stream == nullptr) {
					// No queue, just wait until the right time and then show the frame.
					new_clip_changed.wait_until(lock, next_frame_start, [this] {
						return should_quit || new_clip_ready || override_stream_idx != -1;
					});
					if (should_quit) {
						return;
					}
				} else {
					// If the queue is full (which is really the state we'd like to be in),
					// wait until there's room for one more frame (ie., one was output from
					// VideoStream), or until or until there's a new clip we're supposed to play.
					//
					// In this case, we don't sleep until next_frame_start; the displaying is
					// done by the queue.
					new_clip_changed.wait(lock, [this] {
						if (num_queued_frames < max_queued_frames) {
							return true;
						}
						return should_quit || new_clip_ready || override_stream_idx != -1;
					});
				}
				if (should_quit) {
					return;
				}
				if (new_clip_ready) {
					if (video_stream != nullptr) {
						lock.unlock();  // Urg.
						video_stream->clear_queue();
						lock.lock();
					}
					return;
				}
				// Honor if we got an override request for the camera.
				if (override_stream_idx != -1) {
					stream_idx = override_stream_idx;
					override_stream_idx = -1;
					continue;
				}
			}

			string subtitle;
			{
				stringstream ss;
				ss.imbue(locale("C"));
				ss.precision(3);
				ss << "Futatabi " NAGERU_VERSION ";PLAYING;";
				ss << fixed << (time_remaining.num_infinite * 86400.0 + time_remaining.t);
				ss << ";" << format_duration(time_remaining) << " left";
				subtitle = ss.str();
			}

			// Snap to input frame: If we can do so with less than 1% jitter
			// (ie., move less than 1% of an _output_ frame), do so.
			// TODO: Snap secondary (fade-to) clips in the same fashion.
			double pts_snap_tolerance = 0.01 * double(TIMEBASE) * clip->speed / global_flags.output_framerate;
			bool snapped = false;
			for (FrameOnDisk snap_frame : { frame_lower, frame_upper }) {
				if (fabs(snap_frame.pts - in_pts) < pts_snap_tolerance) {
					display_single_frame(primary_stream_idx, snap_frame, secondary_stream_idx,
					                     secondary_frame, fade_alpha, next_frame_start, /*snapped=*/true,
					                     subtitle, play_audio);
					timeline.snap_by(snap_frame.pts - in_pts);
					snapped = true;
					break;
				}
			}
			if (snapped) {
				continue;
			}

			// If there's nothing to interpolate between, or if interpolation is turned off,
			// or we're a preview, then just display the frame.
			if (frame_lower.pts == frame_upper.pts || global_flags.interpolation_quality == 0 || video_stream == nullptr) {
				display_single_frame(primary_stream_idx, frame_lower, secondary_stream_idx,
				                     secondary_frame, fade_alpha, next_frame_start, /*snapped=*/false,
				                     subtitle, play_audio);
				continue;
			}

			// The snapping above makes us lock to the input framerate, even in the presence
			// of pts drift, for most typical cases where it's needed, like converting 60 → 2x60
			// or 60 → 2x59.94. However, there are some corner cases like 25 → 2x59.94, where we'd
			// get a snap very rarely (in the given case, once every 24 output frames), and by
			// that time, we'd have drifted out. We could have solved this by changing the overall
			// speed ever so slightly, but it requires that we know the actual frame rate (which
			// is difficult in the presence of jitter and missed frames), or at least do some kind
			// of matching/clustering. Instead, we take the opportunity to lock to in-between rational
			// points if we can. E.g., if we are converting 60 → 2x60, we would not only snap to
			// an original frame every other frame; we would also snap to exactly alpha=0.5 every
			// in-between frame. Of course, we will still need to interpolate, but we get a lot
			// closer when we actually get close to an original frame. In other words: Snap more
			// often, but snap less each time. Unless the input and output frame rates are completely
			// decorrelated with no common factor, of course (e.g. 12.345 → 34.567, which we should
			// really never see in practice).
			for (double fraction : { 1.0 / 2.0, 1.0 / 3.0, 2.0 / 3.0, 1.0 / 4.0, 3.0 / 4.0,
			                         1.0 / 5.0, 2.0 / 5.0, 3.0 / 5.0, 4.0 / 5.0 }) {
				double subsnap_pts = frame_lower.pts + fraction * (frame_upper.pts - frame_lower.pts);
				if (fabs(subsnap_pts - in_pts) < pts_snap_tolerance) {
					timeline.snap_by(lrint(subsnap_pts) - in_pts);
					in_pts = lrint(subsnap_pts);
					break;
				}
			}

			if (stream_output != FILE_STREAM_OUTPUT && time_behind >= milliseconds(100)) {
				fprintf(stderr, "WARNING: %ld ms behind, dropping an interpolated frame.\n",
				        lrint(1e3 * duration<double>(time_behind).count()));
				++metric_dropped_interpolated_frame;
				continue;
			}

			double alpha = double(in_pts - frame_lower.pts) / (frame_upper.pts - frame_lower.pts);
			auto display_func = [this](shared_ptr<Frame> frame) {
				if (destination != nullptr) {
					destination->setFrame(frame);
				}
			};
			if (secondary_stream_idx == -1) {
				++metric_interpolated_frame;
			} else {
				++metric_interpolated_faded_frame;
			}
			video_stream->schedule_interpolated_frame(
				next_frame_start, pts, display_func, QueueSpotHolder(this),
				frame_lower, frame_upper, alpha,
				secondary_frame, fade_alpha, subtitle, play_audio);
			last_pts_played = in_pts;  // Not really needed; only previews use last_pts_played.
		}

		// The clip ended.
		if (should_quit) {
			return;
		}

		// Start the next clip from the point where the fade went out.
		if (next_clip != nullptr) {
			timeline.new_clip(next_frame_start, next_clip, /*pts_start_offset=*/lrint(next_clip_fade_time * TIMEBASE * clip->speed));
		}
	}

	if (done_callback != nullptr) {
		done_callback();
	}
}

void Player::display_single_frame(int primary_stream_idx, const FrameOnDisk &primary_frame, int secondary_stream_idx, const FrameOnDisk &secondary_frame, double fade_alpha, steady_clock::time_point frame_start, bool snapped, const std::string &subtitle, bool play_audio)
{
	auto display_func = [this, primary_stream_idx, primary_frame, secondary_frame, fade_alpha] {
		if (destination != nullptr) {
			destination->setFrame(primary_stream_idx, primary_frame, secondary_frame, fade_alpha);
		}
	};
	if (video_stream == nullptr) {
		display_func();
	} else {
		if (secondary_stream_idx == -1) {
			// NOTE: We could be increasing unused metrics for previews, but that's harmless.
			if (snapped) {
				++metric_original_snapped_frame;
			} else {
				++metric_original_frame;
			}
			video_stream->schedule_original_frame(
				frame_start, pts, display_func, QueueSpotHolder(this),
				primary_frame, subtitle, play_audio);
		} else {
			assert(secondary_frame.pts != -1);
			// NOTE: We could be increasing unused metrics for previews, but that's harmless.
			if (snapped) {
				++metric_faded_snapped_frame;
			} else {
				++metric_faded_frame;
			}
			video_stream->schedule_faded_frame(frame_start, pts, display_func,
			                                   QueueSpotHolder(this), primary_frame,
			                                   secondary_frame, fade_alpha, subtitle);
		}
	}
	last_pts_played = primary_frame.pts;
}

// Find the frame immediately before and after this point.
// If we have an exact match, return it immediately.
bool Player::find_surrounding_frames(int64_t pts, int stream_idx, FrameOnDisk *frame_lower, FrameOnDisk *frame_upper)
{
	lock_guard<mutex> lock(frame_mu);

	// Find the first frame such that frame.pts >= pts.
	auto it = find_last_frame_before(frames[stream_idx], pts);
	if (it == frames[stream_idx].end()) {
		return false;
	}
	*frame_upper = *it;

	// If we have an exact match, return it immediately.
	if (frame_upper->pts == pts) {
		*frame_lower = *it;
		return true;
	}

	// Find the last frame such that in_pts <= frame.pts (if any).
	if (it == frames[stream_idx].begin()) {
		*frame_lower = *it;
	} else {
		*frame_lower = *(it - 1);
	}
	assert(pts >= frame_lower->pts);
	assert(pts <= frame_upper->pts);
	return true;
}

Player::Player(JPEGFrameView *destination, Player::StreamOutput stream_output, AVFormatContext *file_avctx)
	: destination(destination), stream_output(stream_output)
{
	player_thread = thread(&Player::thread_func, this, file_avctx);

	if (stream_output == HTTPD_STREAM_OUTPUT) {
		global_metrics.add("http_output_frames", { { "type", "original" }, { "reason", "edge_frame_or_no_interpolation" } }, &metric_original_frame);
		global_metrics.add("http_output_frames", { { "type", "faded" }, { "reason", "edge_frame_or_no_interpolation" } }, &metric_faded_frame);
		global_metrics.add("http_output_frames", { { "type", "original" }, { "reason", "snapped" } }, &metric_original_snapped_frame);
		global_metrics.add("http_output_frames", { { "type", "faded" }, { "reason", "snapped" } }, &metric_faded_snapped_frame);
		global_metrics.add("http_output_frames", { { "type", "interpolated" } }, &metric_interpolated_frame);
		global_metrics.add("http_output_frames", { { "type", "interpolated_faded" } }, &metric_interpolated_faded_frame);
		global_metrics.add("http_output_frames", { { "type", "refresh" } }, &metric_refresh_frame);
		global_metrics.add("http_dropped_frames", { { "type", "interpolated" } }, &metric_dropped_interpolated_frame);
		global_metrics.add("http_dropped_frames", { { "type", "unconditional" } }, &metric_dropped_unconditional_frame);

		vector<double> quantiles{ 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99 };
		metric_player_ahead_seconds.init(quantiles, 60.0);
		global_metrics.add("player_ahead_seconds", &metric_player_ahead_seconds);
	}
}

Player::~Player()
{
	should_quit = true;
	new_clip_changed.notify_all();
	player_thread.join();

	if (video_stream != nullptr) {
		video_stream->stop();
	}
}

void Player::play(const vector<ClipWithID> &clips)
{
	lock_guard<mutex> lock(queue_state_mu);
	new_clip_ready = true;
	queued_clip_list = clips;
	splice_ready = false;
	override_stream_idx = -1;
	new_clip_changed.notify_all();
}

void Player::splice_play(const vector<ClipWithID> &clips)
{
	lock_guard<mutex> lock(queue_state_mu);
	if (new_clip_ready) {
		queued_clip_list = clips;
		assert(!splice_ready);
		return;
	}

	splice_ready = true;
	to_splice_clip_list = clips;  // Overwrite any queued but not executed splice.
}

void Player::override_angle(unsigned stream_idx)
{
	int64_t last_pts;

	// Corner case: If a new clip is waiting to be played, change its stream and then we're done.
	{
		lock_guard<mutex> lock(queue_state_mu);
		if (new_clip_ready) {
			assert(queued_clip_list.size() == 1);
			queued_clip_list[0].clip.stream_idx = stream_idx;
			return;
		}

		// If we are playing a clip, set override_stream_idx, and the player thread will
		// pick it up and change its internal index.
		if (playing) {
			override_stream_idx = stream_idx;
			new_clip_changed.notify_all();
			return;
		}

		// OK, so we're standing still, presumably at the end of a clip.
		// Look at the last frame played (if it exists), and show the closest
		// thing we've got.
		if (last_pts_played < 0) {
			return;
		}
		last_pts = last_pts_played;
	}

	lock_guard<mutex> lock(frame_mu);
	auto it = find_first_frame_at_or_after(frames[stream_idx], last_pts);
	if (it == frames[stream_idx].end()) {
		return;
	}
	destination->setFrame(stream_idx, *it);
}

void Player::take_queue_spot()
{
	lock_guard<mutex> lock(queue_state_mu);
	++num_queued_frames;
}

void Player::release_queue_spot()
{
	lock_guard<mutex> lock(queue_state_mu);
	assert(num_queued_frames > 0);
	--num_queued_frames;
	new_clip_changed.notify_all();
}

TimeRemaining compute_time_left(const vector<ClipWithID> &clips, size_t currently_playing_idx, double progress_currently_playing)
{
	// Look at the last clip and then start counting from there.
	TimeRemaining remaining { 0, 0.0 };
	double last_fade_time_seconds = 0.0;
	for (size_t row = currently_playing_idx; row < clips.size(); ++row) {
		const Clip &clip = clips[row].clip;
		double clip_length = double(clip.pts_out - clip.pts_in) / TIMEBASE / clip.speed;
		if (clip_length >= 86400.0 || clip.pts_out == -1) {  // More than one day.
			++remaining.num_infinite;
		} else {
			if (row == currently_playing_idx) {
				// A clip we're playing: Subtract the part we've already played.
				remaining.t = clip_length * (1.0 - progress_currently_playing);
			} else {
				// A clip we haven't played yet: Subtract the part that's overlapping
				// with a previous clip (due to fade).
				remaining.t += max(clip_length - last_fade_time_seconds, 0.0);
			}
		}
		last_fade_time_seconds = min(clip_length, clip.fade_time_seconds);
	}
	return remaining;
}

string format_duration(TimeRemaining t)
{
	int t_ms = lrint(t.t * 1e3);

	int ms = t_ms % 1000;
	t_ms /= 1000;
	int s = t_ms % 60;
	t_ms /= 60;
	int m = t_ms;

	char buf[256];
	if (t.num_infinite > 1 && t.t > 0.0) {
		snprintf(buf, sizeof(buf), "%zu clips + %d:%02d.%03d", t.num_infinite, m, s, ms);
	} else if (t.num_infinite > 1) {
		snprintf(buf, sizeof(buf), "%zu clips", t.num_infinite);
	} else if (t.num_infinite == 1 && t.t > 0.0) {
		snprintf(buf, sizeof(buf), "%zu clip + %d:%02d.%03d", t.num_infinite, m, s, ms);
	} else if (t.num_infinite == 1) {
		snprintf(buf, sizeof(buf), "%zu clip", t.num_infinite);
	} else {
		snprintf(buf, sizeof(buf), "%d:%02d.%03d", m, s, ms);
	}
	return buf;
}
