#ifndef _QUEUE_LENGTH_POLICY_H
#define _QUEUE_LENGTH_POLICY_H 1

#include <atomic>
#include <string>
#include <utility>
#include <vector>

// A class to estimate the future jitter. Used in QueueLengthPolicy (see below).
//
// There are many ways to estimate jitter; I've tested a few ones (and also
// some algorithms that don't explicitly model jitter) with different
// parameters on some real-life data in experiments/queue_drop_policy.cpp.
// This is one based on simple order statistics where I've added some margin in
// the number of starvation events; I believe that about one every hour would
// probably be acceptable, but this one typically goes lower than that, at the
// cost of 2–3 ms extra latency. (If the queue is hard-limited to one frame, it's
// possible to get ~10 ms further down, but this would mean framedrops every
// second or so.) The general strategy is: Take the 99.9-percentile jitter over
// last 5000 frames, multiply by two, and that's our worst-case jitter
// estimate. The fact that we're not using the max value means that we could
// actually even throw away very late frames immediately, which means we only
// get one user-visible event instead of seeing something both when the frame
// arrives late (duplicate frame) and then again when we drop.
class JitterHistory {
private:
	static constexpr size_t history_length = 5000;
	static constexpr double percentile = 0.999;
	static constexpr double multiplier = 2.0;

public:
	void register_metrics(const std::vector<std::pair<std::string, std::string>> &labels);
	void unregister_metrics(const std::vector<std::pair<std::string, std::string>> &labels);

	void clear() {
		history.clear();
		orders.clear();
		expected_timestamp = std::chrono::steady_clock::time_point::min();
	}
	void frame_arrived(std::chrono::steady_clock::time_point now, int64_t frame_duration, size_t dropped_frames);
	std::chrono::steady_clock::time_point get_expected_next_frame() const { return expected_timestamp; }
	double estimate_max_jitter() const;

private:
	// A simple O(k) based algorithm for getting the k-th largest or
	// smallest element from our window; we simply keep the multiset
	// ordered (insertions and deletions are O(n) as always) and then
	// iterate from one of the sides. If we had larger values of k,
	// we could go for a more complicated setup with two sets or heaps
	// (one increasing and one decreasing) that we keep balanced around
	// the point, or it is possible to reimplement std::set with
	// counts in each node. However, since k=5, we don't need this.
	std::multiset<double> orders;
	std::deque<std::multiset<double>::iterator> history;

	std::chrono::steady_clock::time_point expected_timestamp = std::chrono::steady_clock::time_point::min();
	int64_t last_duration = 0;

	// Metrics. There are no direct summaries for jitter, since we already have latency summaries.
	std::atomic<int64_t> metric_input_underestimated_jitter_frames{0};
	std::atomic<double> metric_input_estimated_max_jitter_seconds{0.0 / 0.0};
};

// For any card that's not the master (where we pick out the frames as they
// come, as fast as we can process), there's going to be a queue. The question
// is when we should drop frames from that queue (apart from the obvious
// dropping if the 16-frame queue should become full), especially given that
// the frame rate could be lower or higher than the master (either subtly or
// dramatically). We have two (conflicting) demands:
//
//   1. We want to avoid starving the queue.
//   2. We don't want to add more delay than is needed.
//
// Our general strategy is to drop as many frames as we can (helping for #2)
// that we think is safe for #1 given jitter. To this end, we measure the
// deviation from the expected arrival time for all cards, and use that for
// continuous jitter estimation.
//
// We then drop everything from the queue that we're sure we won't need to
// serve the output in the time before the next frame arrives. Typically,
// this means the queue will contain 0 or 1 frames, although more is also
// possible if the jitter is very high.
class QueueLengthPolicy {
public:
	QueueLengthPolicy() {}

	void register_metrics(const std::vector<std::pair<std::string, std::string>> &labels);
	void unregister_metrics(const std::vector<std::pair<std::string, std::string>> &labels);

	// Call after picking out a frame, so 0 means starvation.
	// Note that the policy has no memory; everything is given in as parameters.
	void update_policy(std::chrono::steady_clock::time_point now,
	                   std::chrono::steady_clock::time_point expected_next_input_frame,
			   int64_t input_frame_duration,
	                   int64_t master_frame_duration,
	                   double max_input_card_jitter_seconds,
	                   double max_master_card_jitter_seconds);
	unsigned get_safe_queue_length() const { return safe_queue_length; }

private:
	unsigned safe_queue_length = 0;  // Can never go below zero.

	// Metrics.
	std::atomic<int64_t> metric_input_queue_safe_length_frames{1};
};

#endif  // !defined(_QUEUE_LENGTH_POLICY_H)
