/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * receiver-sync.c — absolute-time presentation control.
 *
 * Every synced source targets the same rule: the frame stamped T is presented
 * at NTP time T + offset. Sources do not negotiate; the alignment falls out of
 * both ends sharing a clock. See irl-sync.h for why that is the design.
 *
 * There is exactly one actuator — a delay line of *encoded* packets in front
 * of the decoders — and one loop driving it. Holding compressed packets is not
 * an optimisation but a requirement: the decoded pacing queue is capped at
 * IRL_VIDEO_PACING_MAX_BYTES (192MB, about a second of 1080p60), while the
 * same second of a 6Mbit/s feed is under a megabyte compressed. A five-second
 * offset is unremarkable this way and impossible the other.
 *
 * Nothing in receiver-audio.c changes. Delaying the input shifts the whole
 * PTS-to-OBS mapping with it, so the jitter buffer, the speed controller and
 * the video pacing all keep doing exactly what they did — they simply see the
 * stream arrive later. The slew rate below is chosen so the speed controller
 * never has to correct by more than its inaudible authority.
 *
 * Known limitation: the delay line drains on the receiver thread, which is
 * also the thread that blocks in av_read_frame(). A total delivery gap
 * therefore stalls releases even though the line holds data that is due. That
 * is no worse than the plugin behaves today without a delay line — the gap
 * costs the same concealment either way — but it does mean the buffering here
 * does not add stall resilience. Draining from a separate decode thread would;
 * that is a larger change to the threading model than this feature needs.
 */

#include <stdlib.h>
#include <string.h>

#include "receiver-internal.h"

#include "../include/irl-ntp.h"
#include "../include/irl-sync.h"

/* Presentation error we call aligned, and the wider band we have to leave
 * before admitting we are not. One frame at 60fps is 16.7ms; the tolerance
 * sits just above two of them, and the hysteresis stops a row flickering
 * between Locked and Acquiring on ordinary jitter. */
#define SYNC_LOCK_TOLERANCE_NS 40000000LL
#define SYNC_UNLOCK_TOLERANCE_NS 120000000LL

/* Fraction of real time the hold may move by while locked.
 *
 * Moving the hold moves when packets enter the pipeline, which the jitter
 * buffer absorbs by letting the existing speed controller trim playback.
 * Staying under its -2%/+5% authority is what keeps drift correction
 * inaudible, so this is deliberately below the smaller of the two. */
#define SYNC_SLEW_RATE 0.015

/* Rate at which an unwanted hold is given back — sync switched off, the source
 * unchecked, or the sender stopping timecodes.
 *
 * Zeroing the hold instead would look tidier and would be a bug: every packet
 * in the line becomes due at once, and seconds of audio arriving in one burst
 * overruns a jitter buffer sized at 4x buffer_max_ms while the decoded frames
 * blow past IRL_VIDEO_PACING_MAX_BYTES. Releasing gradually means the stream
 * simply arrives slightly fast for a while, which is the same condition the
 * speed controller already drains a post-stall backlog under. Sits inside its
 * +5% authority, so the cost is mild chipmunk, not a dropout — and unwinding a
 * five-second hold takes about two minutes. */
#define SYNC_RELEASE_RATE 0.04

/* No timecode for this long and the source has stopped being alignable. */
#define SYNC_TC_STALE_NS 3000000000ULL

/* Hysteresis on the "cannot reach the offset" alarm.
 *
 * Bonded cellular spikes constantly, and a feed will cross the threshold for a
 * second or two during a bitrate dip and recover on its own. Alarming on that
 * trains the operator to ignore the warning, which costs more than the missed
 * blip. Transient excursions still show up in the peak column. */
#define SYNC_ALARM_ENTER_NS 4000000000ULL
#define SYNC_ALARM_LEAVE_NS 8000000000ULL

/* Rolling latency peak: 12 buckets of 5s. The peak, not the instantaneous
 * value, is what an offset has to be chosen against. */
#define SYNC_PEAK_BUCKET_NS 5000000000ULL

#define NS_PER_HOUR (3600LL * 1000000000LL)

/* ── Delay line ───────────────────────────────────────────── */

static bool delay_alloc(struct irl_packet_delay *delay)
{
	if (delay->entries)
		return true;

	delay->entries = bzalloc(sizeof(*delay->entries) *
				 IRL_SYNC_DELAY_MAX_PACKETS);
	return delay->entries != NULL;
}

static void delay_clear(struct irl_packet_delay *delay)
{
	while (delay->count > 0) {
		AVPacket *pkt = delay->entries[delay->head].pkt;
		delay->entries[delay->head].pkt = NULL;
		delay->head = (delay->head + 1) % IRL_SYNC_DELAY_MAX_PACKETS;
		delay->count--;
		av_packet_free(&pkt);
	}
	delay->bytes = 0;
	delay->last_release_ns = 0;
	/* Re-arms the one-shot overflow warning for the next connection. */
	delay->overflows = 0;
}

static bool delay_full(const struct irl_packet_delay *delay)
{
	return delay->count >= IRL_SYNC_DELAY_MAX_PACKETS ||
	       delay->bytes >= IRL_SYNC_DELAY_MAX_BYTES;
}

static bool delay_push(struct irl_packet_delay *delay, AVPacket *pkt,
		       uint64_t release_ns)
{
	if (!delay_alloc(delay) || delay_full(delay))
		return false;

	AVPacket *clone = av_packet_alloc();
	if (!clone)
		return false;
	if (av_packet_ref(clone, pkt) < 0) {
		av_packet_free(&clone);
		return false;
	}

	/* Releases must not overtake each other: the hold moves while packets
	 * are queued, and a shrinking hold would otherwise make a newer packet
	 * due before an older one. */
	if (release_ns < delay->last_release_ns)
		release_ns = delay->last_release_ns;
	delay->last_release_ns = release_ns;

	int tail = (delay->head + delay->count) % IRL_SYNC_DELAY_MAX_PACKETS;
	delay->entries[tail].pkt = clone;
	delay->entries[tail].release_ns = release_ns;
	delay->count++;
	delay->bytes += (size_t)(clone->size > 0 ? clone->size : 0);

	return true;
}

/* ── Timecode to latency ──────────────────────────────────── */

/* How old the content in this packet is, from its timecode against NTP.
 *
 * The comparison is modulo one hour, reduced to the nearest representative.
 * That is not a shortcut: the sender writes local calendar components (Moblin
 * uses Calendar.current), so two senders in different time zones differ by
 * whole hours, and the timecode carries no date at all, so it wraps at
 * midnight. Working modulo an hour absorbs both, because a real arrival
 * latency is seconds and can never be mistaken for half an hour. The one case
 * it cannot resolve is a sender on a half-hour time zone offset, which lands
 * exactly on the fold. */
static bool timecode_latency_ns(const struct irl_timecode *tc,
				int64_t frame_interval_ns, int64_t *latency_ns)
{
	int64_t utc_now;
	if (!irl_ntp_utc_now_ns(&utc_now))
		return false;

	int64_t now_in_hour = ((utc_now % NS_PER_HOUR) + NS_PER_HOUR) %
			      NS_PER_HOUR;
	int64_t tc_in_hour =
		((int64_t)tc->minutes * 60 + (int64_t)tc->seconds) *
			1000000000LL +
		(int64_t)tc->n_frames * frame_interval_ns;

	int64_t diff = now_in_hour - tc_in_hour;
	diff = ((diff % NS_PER_HOUR) + NS_PER_HOUR) % NS_PER_HOUR;
	if (diff >= NS_PER_HOUR / 2)
		diff -= NS_PER_HOUR;

	*latency_ns = diff;
	return true;
}

/* ── Presentation prediction ──────────────────────────────── */

/* When a packet with this PTS will reach the screen, in the OBS clock.
 *
 * The audio playout offset is the authoritative mapping — it is what
 * irl_video_due_time() puts every frame through — and it already accounts for
 * whatever the delay line is currently holding, because delaying the input
 * shifts the mapping with it. Before audio has primed there is nothing to map
 * against, so a video-only stream (or the first moments of any stream) falls
 * back to the video anchor. */
static bool predict_presentation_ns(struct irl_source *ctx, int64_t pts_ns,
				    int64_t *presentation_ns)
{
	irl_mutex_lock(&ctx->audio_state_lock);
	uint64_t audio_obs_end = ctx->latest_audio_obs_end_ts_ns;
	int64_t audio_pts_end = ctx->latest_audio_buffered_end_pts_ns;
	irl_mutex_unlock(&ctx->audio_state_lock);

	if (audio_obs_end != 0 && audio_pts_end > 0) {
		*presentation_ns = pts_ns + ((int64_t)audio_obs_end -
					     audio_pts_end);
		return true;
	}

	if (ctx->video_ts_init) {
		*presentation_ns = (int64_t)ctx->video_sys_base +
				   (pts_ns - ctx->video_pts_base);
		return true;
	}

	return false;
}

/* ── Rolling latency peak ─────────────────────────────────── */

static void peak_record(struct irl_source *ctx, int64_t latency_ns,
			uint64_t now_ns)
{
	if (ctx->sync_peak_bucket_start_ns == 0)
		ctx->sync_peak_bucket_start_ns = now_ns;

	while (now_ns - ctx->sync_peak_bucket_start_ns >= SYNC_PEAK_BUCKET_NS) {
		ctx->sync_peak_bucket =
			(ctx->sync_peak_bucket + 1) % IRL_SYNC_PEAK_BUCKETS;
		ctx->sync_peak_buckets[ctx->sync_peak_bucket] = INT64_MIN;
		ctx->sync_peak_bucket_start_ns += SYNC_PEAK_BUCKET_NS;
	}

	if (latency_ns > ctx->sync_peak_buckets[ctx->sync_peak_bucket])
		ctx->sync_peak_buckets[ctx->sync_peak_bucket] = latency_ns;
}

static int64_t peak_value(const struct irl_source *ctx)
{
	int64_t peak = INT64_MIN;

	for (int i = 0; i < IRL_SYNC_PEAK_BUCKETS; i++) {
		if (ctx->sync_peak_buckets[i] > peak)
			peak = ctx->sync_peak_buckets[i];
	}

	return peak == INT64_MIN ? 0 : peak;
}

/* ── Status publication ───────────────────────────────────── */

static void publish(struct irl_source *ctx)
{
	struct irl_sync_snapshot snap = {0};

	snap.status = ctx->sync_status;
	snap.have_timecode = ctx->sync_have_tc;
	snap.tc = ctx->sync_tc;
	snap.latency_ms = ctx->sync_latency_ns / 1000000LL;
	snap.added_ms = ctx->sync_hold_ns / 1000000LL;
	snap.error_ms = ctx->sync_error_ns / 1000000LL;

	int64_t peak_ns = peak_value(ctx);
	snap.latency_peak_ms = peak_ns / 1000000LL;
	snap.required_offset_ms =
		snap.latency_peak_ms + IRL_SYNC_OFFSET_MARGIN_MS;

	irl_sync_publish(ctx, &snap);
}

/* ── Reset ────────────────────────────────────────────────── */

void irl_sync_reset(struct irl_source *ctx)
{
	delay_clear(&ctx->sync_delay);

	ctx->sync_engaged = false;
	ctx->sync_status = IRL_SYNC_OFF;
	ctx->sync_have_tc = false;
	ctx->sync_hold_ns = 0;
	ctx->sync_latency_ns = 0;
	ctx->sync_error_ns = 0;
	ctx->sync_last_tc_ns = 0;
	ctx->sync_last_adjust_ns = 0;
	ctx->sync_too_slow_since_ns = 0;
	ctx->sync_in_reach_since_ns = 0;
	ctx->sync_applied_offset_ms = 0;
	ctx->sync_offset_generation = irl_sync_offset_generation();
	ctx->sync_peak_bucket = 0;
	ctx->sync_peak_bucket_start_ns = 0;
	for (int i = 0; i < IRL_SYNC_PEAK_BUCKETS; i++)
		ctx->sync_peak_buckets[i] = INT64_MIN;

	publish(ctx);
}

void irl_sync_free(struct irl_source *ctx)
{
	delay_clear(&ctx->sync_delay);
	bfree(ctx->sync_delay.entries);
	ctx->sync_delay.entries = NULL;
}

/* ── Control loop ─────────────────────────────────────────── */

static bool sync_wanted(const struct irl_source *ctx)
{
	return irl_sync_is_enabled() &&
	       os_atomic_load_bool(&ctx->config.sync_enabled);
}

/* Give back hold the source is no longer entitled to, at a rate the pipeline
 * downstream can absorb. See SYNC_RELEASE_RATE. */
static void release_hold(struct irl_source *ctx, uint64_t now_ns)
{
	if (ctx->sync_hold_ns <= 0) {
		ctx->sync_hold_ns = 0;
		ctx->sync_last_adjust_ns = now_ns;
		return;
	}

	int64_t elapsed_ns = ctx->sync_last_adjust_ns
				     ? (int64_t)(now_ns - ctx->sync_last_adjust_ns)
				     : 0;
	ctx->sync_last_adjust_ns = now_ns;

	int64_t step_ns = (int64_t)((double)elapsed_ns * SYNC_RELEASE_RATE);
	ctx->sync_hold_ns -= step_ns;
	if (ctx->sync_hold_ns < 0)
		ctx->sync_hold_ns = 0;
}

/* Classify, with the alarm hysteresis applied. */
static void update_status(struct irl_source *ctx, uint64_t now_ns,
			  bool reachable)
{
	if (reachable) {
		ctx->sync_too_slow_since_ns = 0;
		if (ctx->sync_in_reach_since_ns == 0)
			ctx->sync_in_reach_since_ns = now_ns;
	} else {
		ctx->sync_in_reach_since_ns = 0;
		if (ctx->sync_too_slow_since_ns == 0)
			ctx->sync_too_slow_since_ns = now_ns;
	}

	bool alarmed = ctx->sync_status == IRL_SYNC_TOO_SLOW;

	if (!alarmed && !reachable &&
	    now_ns - ctx->sync_too_slow_since_ns >= SYNC_ALARM_ENTER_NS) {
		alarmed = true;
		blog(LOG_WARNING,
		     "[irl-source] Sync: feed arrives %lldms late, past the %dms offset; raise the offset to at least %lldms",
		     (long long)(ctx->sync_latency_ns / 1000000LL),
		     irl_sync_offset_ms(),
		     (long long)(peak_value(ctx) / 1000000LL +
				 IRL_SYNC_OFFSET_MARGIN_MS));
	} else if (alarmed && reachable &&
		   now_ns - ctx->sync_in_reach_since_ns >= SYNC_ALARM_LEAVE_NS) {
		alarmed = false;
		blog(LOG_INFO,
		     "[irl-source] Sync: feed back within the offset");
	}

	if (alarmed) {
		ctx->sync_status = IRL_SYNC_TOO_SLOW;
		return;
	}

	int64_t err = llabs(ctx->sync_error_ns);
	if (ctx->sync_locked) {
		if (err > SYNC_UNLOCK_TOLERANCE_NS)
			ctx->sync_locked = false;
	} else if (err <= SYNC_LOCK_TOLERANCE_NS) {
		ctx->sync_locked = true;
	}

	ctx->sync_status = ctx->sync_locked ? IRL_SYNC_LOCKED
					    : IRL_SYNC_ACQUIRING;
}

/* Fold one timecoded video packet into the controller. */
static void observe_timecode(struct irl_source *ctx, const AVPacket *pkt,
			     const struct irl_timecode *tc, uint64_t now_ns)
{
	irl_mutex_lock(&ctx->audio_state_lock);
	int64_t frame_interval_ns = ctx->video_frame_interval_ns;
	irl_mutex_unlock(&ctx->audio_state_lock);
	if (frame_interval_ns < IRL_VIDEO_INTERVAL_MIN_NS ||
	    frame_interval_ns > IRL_VIDEO_INTERVAL_MAX_NS)
		frame_interval_ns = IRL_VIDEO_INTERVAL_DEFAULT_NS;

	int64_t latency_ns;
	if (!timecode_latency_ns(tc, frame_interval_ns, &latency_ns)) {
		/* Timecodes are arriving but there is no NTP reference to
		 * compare them against, so nothing can be aligned. */
		ctx->sync_status = IRL_SYNC_NO_TIMECODE;
		return;
	}

	ctx->sync_tc = *tc;
	ctx->sync_have_tc = true;
	ctx->sync_last_tc_ns = now_ns;
	ctx->sync_latency_ns = latency_ns;
	peak_record(ctx, latency_ns, now_ns);

	const int64_t prev_hold_ns = ctx->sync_hold_ns;
	const int64_t elapsed_ns =
		ctx->sync_last_adjust_ns
			? (int64_t)(now_ns - ctx->sync_last_adjust_ns)
			: 0;

	int64_t offset_ns = (int64_t)irl_sync_offset_ms() * 1000000LL;
	int64_t needed_ns = offset_ns - latency_ns;
	bool reachable = needed_ns >= 0;

	/* Engage: seed the hold so the pipeline primes already delayed,
	 * instead of priming undelayed and then taking a multi-second step
	 * that would starve the decoder. The nominal pipeline delay is the
	 * jitter cushion plus the output lead — the rest of the error is what
	 * the loop below is for. */
	if (!ctx->sync_engaged) {
		int64_t nominal_pipeline_ns =
			os_atomic_load_long(&ctx->config.buffer_target_ms) *
			1000000LL;

		ctx->sync_hold_ns = needed_ns - nominal_pipeline_ns;
		if (ctx->sync_hold_ns < 0)
			ctx->sync_hold_ns = 0;
		ctx->sync_engaged = true;
		ctx->sync_locked = false;
		ctx->sync_applied_offset_ms = irl_sync_offset_ms();
		ctx->sync_offset_generation = irl_sync_offset_generation();

		blog(LOG_INFO,
		     "[irl-source] Sync engaged: feed latency %lldms, offset %dms, holding %lldms",
		     (long long)(latency_ns / 1000000LL), irl_sync_offset_ms(),
		     (long long)(ctx->sync_hold_ns / 1000000LL));
	}

	/* The user turned the offset knob. Step by the delta rather than
	 * slewing: they asked for the change and expect a hitch, and slewing
	 * several seconds at the drift rate would take a minute. */
	uint32_t generation = irl_sync_offset_generation();
	if (generation != ctx->sync_offset_generation) {
		int64_t delta_ns = ((int64_t)irl_sync_offset_ms() -
				    ctx->sync_applied_offset_ms) *
				   1000000LL;
		ctx->sync_hold_ns += delta_ns;
		ctx->sync_applied_offset_ms = irl_sync_offset_ms();
		ctx->sync_offset_generation = generation;
		ctx->sync_locked = false;
	}

	int64_t pts_ns = 0;
	if (pkt->pts != AV_NOPTS_VALUE && ctx->fmt_ctx &&
	    ctx->video_stream_idx >= 0) {
		AVStream *vs = ctx->fmt_ctx->streams[ctx->video_stream_idx];
		pts_ns = av_rescale_q(pkt->pts, vs->time_base,
				      (AVRational){1, 1000000000});
	}

	int64_t presentation_ns;
	if (pkt->pts != AV_NOPTS_VALUE &&
	    predict_presentation_ns(ctx, pts_ns, &presentation_ns)) {
		/* Where this content should land, and where it will. */
		int64_t target_ns = (int64_t)now_ns + needed_ns;
		ctx->sync_error_ns = presentation_ns - target_ns;

		/* Positive error means we are late, so hold less. While
		 * unlocked the correction is applied outright — nothing is
		 * aligned yet, so there is no smoothness to protect. Once
		 * locked it is rate limited, which is what keeps the resulting
		 * playback trim inside the speed controller's inaudible
		 * band. */
		int64_t step_ns = -ctx->sync_error_ns;
		if (ctx->sync_locked) {
			int64_t limit_ns =
				(int64_t)((double)elapsed_ns * SYNC_SLEW_RATE);
			if (step_ns > limit_ns)
				step_ns = limit_ns;
			if (step_ns < -limit_ns)
				step_ns = -limit_ns;
		}

		ctx->sync_hold_ns += step_ns;
	}

	ctx->sync_last_adjust_ns = now_ns;

	/* The one invariant that keeps every path above safe: the hold may
	 * grow as fast as it likes — that only makes packets wait longer — but
	 * it may never shrink faster than the pipeline can absorb. Shrinking it
	 * makes queued packets due sooner, and a large enough cut (the user
	 * lowering the offset by seconds, say) would make the whole line due at
	 * once and overrun the buffers downstream. Capping the decrease turns
	 * that into an ordinary drain instead. */
	int64_t floor_ns = prev_hold_ns -
			   (int64_t)((double)elapsed_ns * SYNC_RELEASE_RATE);
	if (ctx->sync_hold_ns < floor_ns)
		ctx->sync_hold_ns = floor_ns;

	int64_t max_hold_ns = (int64_t)IRL_SYNC_MAX_OFFSET_MS * 1000000LL;
	if (ctx->sync_hold_ns < 0)
		ctx->sync_hold_ns = 0;
	if (ctx->sync_hold_ns > max_hold_ns)
		ctx->sync_hold_ns = max_hold_ns;

	update_status(ctx, now_ns, reachable);
}

/* ── Receiver-thread entry points ─────────────────────────── */

void irl_sync_observe(struct irl_source *ctx, const AVPacket *pkt)
{
	uint64_t now_ns = os_gettime_ns();

	if (!sync_wanted(ctx)) {
		if (ctx->sync_status != IRL_SYNC_OFF) {
			ctx->sync_status = IRL_SYNC_OFF;
			ctx->sync_have_tc = false;
			ctx->sync_error_ns = 0;
			ctx->sync_locked = false;
			publish(ctx);
		}
		/* Whatever hold was built up is handed back gradually rather
		 * than dropped in one go. Stays engaged=false so re-enabling
		 * re-seeds from scratch. */
		ctx->sync_engaged = false;
		release_hold(ctx, now_ns);
		return;
	}

	if (pkt->stream_index == ctx->video_stream_idx && pkt->data &&
	    pkt->size > 0) {
		struct irl_timecode tc;

		if (irl_sei_find_timecode(pkt->data, (size_t)pkt->size, &tc))
			observe_timecode(ctx, pkt, &tc, now_ns);
	}

	/* No timecode for a while: either the sender never sends them or it
	 * stopped. Both leave the source unalignable, and the delay line has
	 * to let go rather than hold a stale amount forever. */
	if (ctx->sync_last_tc_ns == 0 ||
	    now_ns - ctx->sync_last_tc_ns > SYNC_TC_STALE_NS) {
		if (ctx->sync_status != IRL_SYNC_NO_TIMECODE) {
			ctx->sync_status = IRL_SYNC_NO_TIMECODE;
			ctx->sync_have_tc = false;
			ctx->sync_error_ns = 0;
			ctx->sync_locked = false;
		}
		ctx->sync_engaged = false;
		release_hold(ctx, now_ns);
	}

	/* ~5Hz is plenty for a dock and keeps this off the per-packet path. */
	if (now_ns - ctx->sync_publish_ns >= 200000000ULL) {
		ctx->sync_publish_ns = now_ns;
		publish(ctx);
	}
}

bool irl_sync_hold(struct irl_source *ctx, AVPacket *pkt)
{
	if (ctx->sync_hold_ns <= 0 && ctx->sync_delay.count == 0)
		return false;

	/* Once anything is queued, everything must queue behind it or the
	 * decoder would see packets out of order. */
	uint64_t release_ns = os_gettime_ns() + (uint64_t)ctx->sync_hold_ns;

	if (delay_push(&ctx->sync_delay, pkt, release_ns))
		return true;

	/* A ceiling reached despite the receiver loop's backpressure, or an
	 * allocation failure. Passing the packet straight through keeps decode
	 * going at the cost of it arriving out of order once; dropping it would
	 * cost reference frames until the next keyframe. Reported rather than
	 * absorbed silently, because it means sync is no longer holding what it
	 * claims to. */
	if (ctx->sync_delay.overflows++ == 0) {
		blog(LOG_WARNING,
		     "[irl-source] Sync delay line could not take a packet (%d queued, %zu bytes); alignment will drift until it recovers",
		     ctx->sync_delay.count, ctx->sync_delay.bytes);
	}
	return false;
}

bool irl_sync_delay_full(const struct irl_source *ctx)
{
	return delay_full(&ctx->sync_delay);
}

void irl_sync_drain(struct irl_source *ctx, AVFrame *frame)
{
	struct irl_packet_delay *delay = &ctx->sync_delay;
	uint64_t now_ns = os_gettime_ns();

	while (delay->count > 0) {
		struct irl_delay_entry *entry = &delay->entries[delay->head];

		if (entry->release_ns > now_ns)
			break;

		AVPacket *pkt = entry->pkt;
		entry->pkt = NULL;
		delay->head = (delay->head + 1) % IRL_SYNC_DELAY_MAX_PACKETS;
		delay->count--;
		delay->bytes -= (size_t)(pkt->size > 0 ? pkt->size : 0);

		irl_dispatch_packet(ctx, pkt, frame);
		av_packet_free(&pkt);
	}
}
