/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * irl-sync-state.h — everything timecode sync keeps on one source.
 *
 * Split out so struct irl_source carries a single `sync` member rather than
 * three dozen fields it never reads. Nothing outside src/sync/ looks inside
 * this: the plugin embeds it, sizes it, and zeroes it, and that is all.
 *
 * Deliberately includes nothing from the plugin. irl-source.h includes this,
 * so a dependency the other way would be a cycle — which is also why the state
 * is here and the API that operates on it is in irl-sync.h.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

#include "irl-sei.h"
#include "irl-sync.h"

/* Timecode sync delay line (sync-control.c).
 *
 * Bounds on the encoded packets held in front of the decoders to reach the
 * configured presentation offset. Sized for the worst case the offset ceiling
 * allows: 30s of a high-bitrate feed is tens of megabytes, and the packet
 * count covers 30s of 60fps video interleaved with audio several times over.
 * Hitting either ceiling means something pathological, and the receiver loop
 * applies transport backpressure well before that (see irl_sync_delay_full). */
#define IRL_SYNC_DELAY_MAX_PACKETS 16384
#define IRL_SYNC_DELAY_MAX_BYTES (256u * 1024u * 1024u)

/* One encoded packet waiting for its release into the decoder. */
struct irl_delay_entry {
	AVPacket *pkt;
	uint64_t release_ns;
};

struct irl_packet_delay {
	/* Allocated on first use, so a source that never syncs pays nothing. */
	struct irl_delay_entry *entries;
	int head;
	int count;
	size_t bytes;
	/* Release times are clamped non-decreasing: the hold moves while
	 * packets are queued, and the decoder must never see them reordered. */
	uint64_t last_release_ns;
	/* Packets the line could not take. Should stay zero — the receiver
	 * loop applies backpressure before the ceilings are reached — so a
	 * non-zero count is reported rather than absorbed. */
	uint64_t overflows;
};

/* Per-source sync state.
 *
 * Receiver-thread-owned unless a field says otherwise: the controller runs on
 * the packet path and publishes a snapshot to the registry in sync-config.c
 * for the dock and the websocket vendor to read. The handful the audio thread
 * also touches name their guard in place. */
struct irl_sync_state {
	struct irl_packet_delay delay;
	enum irl_sync_status status;
	enum irl_sync_tc_reason tc_reason;
	bool engaged;
	bool locked;
	bool have_tc;
	struct irl_timecode tc;
	/* How long packets are held in front of the decoders. The single
	 * actuator: everything else about sync is measurement. */
	int64_t hold_ns;
	int64_t latency_ns;
	/* Trimmed mean of the readings in the window, not the last one. See
	 * error_filter_push(). Ring, oldest at error_head; the timestamps
	 * are what bound it by time rather than by sample count. */
	int64_t error_ns;
	int64_t error_window[IRL_SYNC_ERROR_SAMPLES];
	uint64_t error_time[IRL_SYNC_ERROR_SAMPLES];
	int error_head;
	int error_count;
	/* Where content has to play for the stream to land on its offset,
	 * expressed as the audio playout offset it implies: obs_ts - pts. The
	 * audio thread reads it once, when it primes, to anchor its output
	 * clock; the receiver thread keeps it current. Guarded by
	 * audio_state_lock, like the rest of the cross-thread timing state.
	 * See irl_sync_playout_anchor(). */
	int64_t present_bias_ns;
	bool present_bias_valid;
	/* Pre-roll the audio output is waiting out before it starts, handed
	 * back so the delay line absorbs it instead of the jitter buffer. The
	 * flag is atomic so the receiver thread can skip the lock on the
	 * packets where there is nothing to collect, which is nearly all of
	 * them; the value itself is guarded by audio_state_lock. */
	int64_t anchor_defer_ns;
	bool anchor_defer_pending;
	/* Cleared at engage, set when the first correction after it is made,
	 * so how far the seed missed is logged once rather than every packet.
	 * See SYNC_SEED_BIAS_NS. */
	bool seed_reported;
	/* Offset the current hold was computed against, plus the generation
	 * counter that says the user has changed it since. */
	int applied_offset_ms;
	uint32_t offset_generation;
	uint64_t last_tc_ns;
	uint64_t last_adjust_ns;
	/* No further correction until this instant: the error signal lags a
	 * hold behind the hold, so the loop has to wait for its own last
	 * change to land or it winds itself up. See observe_timecode(). */
	uint64_t settle_until_ns;
	uint64_t publish_ns;
	/* Audio priming gate. Written by the receiver thread, read by the audio
	 * thread, so both go through os_atomic_*_bool. See update_prime_gate().
	 * The deadline beside it is receiver-thread only. */
	bool prime_hold;
	/* Set on the delay line's first release: the point the pipeline is
	 * flowing again and the audio output can safely prime. */
	bool released_once;
	uint64_t prime_deadline_ns;
	uint64_t too_slow_since_ns;
	uint64_t in_reach_since_ns;
	/* Frame rate as learned from the timecodes themselves, so converting
	 * n_frames to time never assumes one. See tc_rate_observe(). Zero
	 * until a complete timecode second has been seen. */
	int64_t fps_interval_ns;
	uint16_t fps_recent[IRL_SYNC_FPS_SECONDS];
	int fps_next;
	int fps_count;
	/* The timecode second being accumulated, and the highest frame index
	 * seen in it so far. */
	uint16_t tc_max_frames;
	uint8_t tc_second;
	bool tc_have_second;
	/* Rolling peak of arrival latency, bucketed so it ages out. */
	int64_t peak_buckets[IRL_SYNC_PEAK_BUCKETS];
	int peak_bucket;
	uint64_t peak_bucket_start_ns;
};
