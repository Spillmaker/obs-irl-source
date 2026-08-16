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

/* Slack added to the hold when working out when a correction will show up.
 * Covers the rest of the pipeline the packet still has to cross once released:
 * decode, the jitter cushion and the audio output lead. */
#define SYNC_SETTLE_MARGIN_NS 750000000LL

/* No timecode for this long and the source has stopped being alignable. */
#define SYNC_TC_STALE_NS 3000000000ULL

/* Deliberate over-hold at engage.
 *
 * The seed can only estimate this machine's pipeline, and it lands early —
 * roughly half a second, most of it the startup backlog trim, which discards
 * audio after sync has already chosen its hold. That is the harmless
 * direction, because irl_sync_playout_anchor() places the output clock exactly
 * when it primes and early simply means the anchor waits. Late it cannot fix:
 * a placement in the past can only be reached by discarding audio.
 *
 * So this is not here to make the seed accurate. It is here to bound the wait,
 * and with it the pre-roll that builds up in the jitter buffer while the
 * anchor is pending. The ceiling below makes room for it rather than clamping
 * it back off in the same call that set it. */
#define SYNC_SEED_BIAS_NS 250000000LL

/* Longest pre-roll irl_sync_playout_anchor() will ask the audio output to wait
 * before it starts. Past this the seed is wrong by more than the jitter buffer
 * should be asked to hold, and starting at "now" and correcting is the lesser
 * evil. */
#define SYNC_ANCHOR_MAX_WAIT_NS 1500000000LL

/* Longest the audio output is kept from priming while sync works out its hold.
 *
 * The seed at engage is a step of whatever the offset needs — seconds, often —
 * and a step that size applied to a pipeline that has already primed starves
 * it: packets stop flowing for the length of the step while the jitter buffer
 * drains at real time. Priming *after* the hold exists costs nothing, because
 * nothing is playing yet to starve. That was always the design; what broke it
 * was engage moving later than priming once the frame rate had to be learned
 * first.
 *
 * The deadline is the backstop for everything that means engage will never
 * come — a sender that stamps nothing, a rate that never resolves — because a
 * source that stays silent is far worse than one that primes unsynced and
 * takes the hold as a hitch. Normal engage is well inside a second of the
 * first video frame, so this is not on the happy path. */
#define SYNC_PRIME_WAIT_NS 3000000000ULL

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

/* Move every queued packet's release time later by the same amount.
 *
 * Only ever later. Shifting earlier would make the whole line due at once,
 * which is the burst SYNC_RELEASE_RATE exists to prevent. Later opens a gap in
 * the line, so it is safe exactly when something downstream is holding enough
 * to cover the gap — which is the one case it is used for. See
 * apply_anchor_defer(). */
static void delay_shift_later(struct irl_packet_delay *delay, int64_t shift_ns)
{
	for (int i = 0; i < delay->count; i++) {
		const int idx = (delay->head + i) % IRL_SYNC_DELAY_MAX_PACKETS;
		delay->entries[idx].release_ns += (uint64_t)shift_ns;
	}
	delay->last_release_ns += (uint64_t)shift_ns;
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
 * shifts the mapping with it.
 *
 * The video anchor is a fallback for streams that have no audio at all, never
 * for the moments before audio primes. It describes video that has not been
 * through the delay line yet, so on a stream that does have audio it reports a
 * presentation time several seconds out — one such reading went straight into
 * the loop as a -5747ms correction, and the hold ceiling was the only thing
 * that stopped it. A source with an audio track measures nothing until that
 * audio is playing. */
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

	if (ctx->audio_stream_idx >= 0)
		return false;

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

/* Work out why nothing is arriving to align against.
 *
 * Ordered by what the operator can act on soonest: a missing clock is local,
 * a wrong codec is one setting on the sender, and "absent" is everything else
 * about how the sender is configured. */
static enum irl_sync_tc_reason diagnose_missing_timecode(struct irl_source *ctx)
{
	int64_t utc_ns;

	if (!irl_ntp_utc_now_ns(&utc_ns))
		return IRL_SYNC_TC_NO_CLOCK;

	if (ctx->fmt_ctx && ctx->video_stream_idx >= 0 &&
	    ctx->fmt_ctx->streams[ctx->video_stream_idx]->codecpar->codec_id !=
		    AV_CODEC_ID_HEVC)
		return IRL_SYNC_TC_CODEC;

	return IRL_SYNC_TC_ABSENT;
}

/* Said once per transition, so a stream that simply never carries timecodes
 * explains itself in the log without repeating every packet. */
static void set_tc_reason(struct irl_source *ctx, enum irl_sync_tc_reason reason)
{
	if (ctx->sync_tc_reason == reason)
		return;
	ctx->sync_tc_reason = reason;

	switch (reason) {
	case IRL_SYNC_TC_NO_CLOCK:
		blog(LOG_WARNING,
		     "[irl-source] Sync: no NTP reference on this machine yet; check the server in the IRL Sync dock");
		break;
	case IRL_SYNC_TC_CODEC:
		blog(LOG_WARNING,
		     "[irl-source] Sync: stream is %s, but SEI timecodes only exist on H.265/HEVC; switch the sender's codec",
		     ctx->fmt_ctx && ctx->video_stream_idx >= 0
			     ? avcodec_get_name(
				       ctx->fmt_ctx
					       ->streams[ctx->video_stream_idx]
					       ->codecpar->codec_id)
			     : "not H.265");
		break;
	case IRL_SYNC_TC_ABSENT:
		blog(LOG_WARNING,
		     "[irl-source] Sync: H.265 stream carries no time_code SEI; enable Timecodes and set an NTP pool on the sender (Moblin: Settings > Streams > Video > Timecodes)");
		break;
	case IRL_SYNC_TC_OK:
		blog(LOG_INFO, "[irl-source] Sync: timecodes detected");
		break;
	}
}

/* ── Frame rate ───────────────────────────────────────────── */

/* The frame rate is a property of the stream, and every source can be running
 * a different one — 25, 30, 50, 60, 240 — so it is learned, never assumed.
 *
 * It is learned from the timecodes rather than from the decoder, for two
 * reasons. The signal is the same one being converted: n_frames is an index
 * within its timecode second (H.265 D.2.27), so the highest index a second
 * carries is that second's frame count, and using it to convert n_frames back
 * to time is self-consistent by construction. And it is available on the
 * receiver thread, at the first complete second, where the decoder's own
 * measurement is both cross-thread and only written once frames are coming out
 * the far side of the keyframe gate.
 *
 * Taken from agreement between seconds rather than from any single one, so
 * neither a lossy second nor a corrupt SEI can push a wrong figure into every
 * latency measurement afterwards. See tc_rate_record().
 */
static void tc_rate_record(struct irl_source *ctx, uint16_t max_frames)
{
	ctx->sync_fps_recent[ctx->sync_fps_next] = max_frames;
	ctx->sync_fps_next = (ctx->sync_fps_next + 1) % IRL_SYNC_FPS_SECONDS;
	if (ctx->sync_fps_count < IRL_SYNC_FPS_SECONDS)
		ctx->sync_fps_count++;

	/* The count the recent seconds agree on — not the highest of them.
	 *
	 * Taking the maximum is the obvious choice and is wrong. It is robust
	 * to loss, which can only ever shorten a second, and that was the
	 * reasoning. But a corrupt SEI yields a garbage n_frames that is just
	 * as likely to be high as low, and a maximum adopts it outright. Not
	 * hypothetical: a 30fps sender read as "41 frames per second" across a
	 * reconnect, which put a quarter-second sawtooth into every latency
	 * measurement until it aged out.
	 *
	 * Requiring two of the recent seconds to agree rejects both failure
	 * modes at once, because loss and corruption both produce one-off
	 * values that nothing else matches. Ties go to the larger count, since
	 * between two plausible readings the short one is the damaged one. */
	uint16_t agreed = 0;
	int best_votes = 0;
	for (int i = 0; i < ctx->sync_fps_count; i++) {
		const uint16_t candidate = ctx->sync_fps_recent[i];
		int votes = 0;

		for (int j = 0; j < ctx->sync_fps_count; j++) {
			if (ctx->sync_fps_recent[j] == candidate)
				votes++;
		}

		if (votes > best_votes ||
		    (votes == best_votes && candidate > agreed)) {
			best_votes = votes;
			agreed = candidate;
		}
	}

	if (best_votes < 2)
		return;

	/* Indices are zero-based, so the count is one more than the highest. */
	int64_t interval_ns = 1000000000LL / ((int64_t)agreed + 1);

	/* Outside what any real stream runs at: a corrupt SEI, or a sender
	 * counting something other than frames. Keep whatever was already
	 * learned rather than adopting a figure that would bias every
	 * conversion after it. */
	if (interval_ns < IRL_VIDEO_INTERVAL_MIN_NS ||
	    interval_ns > IRL_VIDEO_INTERVAL_MAX_NS)
		return;

	if (ctx->sync_fps_interval_ns != interval_ns) {
		ctx->sync_fps_interval_ns = interval_ns;
		blog(LOG_INFO,
		     "[irl-source] Sync: sender is stamping %d frames per second",
		     (int)agreed + 1);
	}
}

static void tc_rate_observe(struct irl_source *ctx,
			    const struct irl_timecode *tc)
{
	if (ctx->sync_tc_have_second && tc->seconds == ctx->sync_tc_second) {
		if (tc->n_frames > ctx->sync_tc_max_frames)
			ctx->sync_tc_max_frames = tc->n_frames;
		return;
	}

	/* A second just ended, so its highest index is one full frame count.
	 *
	 * The second the stream was joined partway into counts the same as any
	 * other: frame indices ascend within a second, so joining late still
	 * observes that second's tail, which is where its maximum is. Only
	 * lost packets can shorten a second, and the rolling maximum over
	 * IRL_SYNC_FPS_SECONDS is what covers that. */
	if (ctx->sync_tc_have_second)
		tc_rate_record(ctx, ctx->sync_tc_max_frames);

	ctx->sync_tc_second = tc->seconds;
	ctx->sync_tc_max_frames = tc->n_frames;
	ctx->sync_tc_have_second = true;
}

/* The interval to convert n_frames with, or false if nothing has measured one
 * yet. There is deliberately no default: an assumed rate would put
 * n_frames x (assumed - actual) of error into every latency reading, which at
 * the top of the range is seconds, and would then be seeded into the hold and
 * held in the latency peak long after the real rate was known. */
static bool tc_frame_interval_ns(struct irl_source *ctx, int64_t *interval_ns,
				 const char **origin)
{
	if (ctx->sync_fps_interval_ns > 0) {
		*interval_ns = ctx->sync_fps_interval_ns;
		*origin = "timecodes";
		return true;
	}

	/* Before the first complete second, the decoder's cadence will do if
	 * it has one — also measured, just from the other side of the pipe. */
	irl_mutex_lock(&ctx->audio_state_lock);
	int64_t decoded_ns = ctx->video_frame_interval_ns;
	irl_mutex_unlock(&ctx->audio_state_lock);

	if (decoded_ns >= IRL_VIDEO_INTERVAL_MIN_NS &&
	    decoded_ns <= IRL_VIDEO_INTERVAL_MAX_NS) {
		*interval_ns = decoded_ns;
		*origin = "decoder";
		return true;
	}

	return false;
}

/* ── Error filter ─────────────────────────────────────────── */

static int cmp_int64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a;
	int64_t y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

static void error_filter_reset(struct irl_source *ctx)
{
	ctx->sync_error_head = 0;
	ctx->sync_error_count = 0;
}

/* Fold one presentation-error reading in and return the filtered window value.
 *
 * The per-packet reading carries about a frame of noise: it is the *predicted*
 * presentation time of one access unit, taken from an audio playout mapping
 * that steps as audio is submitted, against a capture timecode the sender can
 * only express to frame resolution. That noise is zero-mean and the true error
 * moves far slower, so filtering costs nothing real and buys two things.
 *
 * The loop corrects once per settle interval, on whatever single reading
 * happened to be current when the gate opened — so an unfiltered sample put a
 * frame of dither straight into the actuator, every time. And the number the
 * operator reads jittered by a frame while the alignment underneath it was
 * steady, which is not what a sync readout is for.
 *
 * A trimmed mean rather than a median. Both reject the one wildly wrong sample
 * a corrupt or mis-stamped timecode produces, which is why this is not a plain
 * mean — but a median *returns* one of the samples, so its output can only ever
 * be a value the quantisation already permits. The correction is then quantised
 * too, and the loop parks on the nearest step instead of on zero: a residual
 * that sits still at a fraction of a frame and never walks off it. Averaging
 * what is left after the tails are cut lands between the steps, so the loop can
 * aim at zero.
 *
 * The window is bounded by time, not by sample count, so the filter behaves
 * identically on a 25fps feed and a 240fps one. See IRL_SYNC_ERROR_WINDOW_NS. */
static int64_t error_filter_push(struct irl_source *ctx, int64_t error_ns,
				 uint64_t now_ns)
{
	if (ctx->sync_error_count == IRL_SYNC_ERROR_SAMPLES) {
		/* Full before the window expired: a rate above what the
		 * interval bounds accept, or timecodes on every field of an
		 * interlaced feed. Dropping the oldest keeps the window to the
		 * most recent second's worth either way. */
		ctx->sync_error_head =
			(ctx->sync_error_head + 1) % IRL_SYNC_ERROR_SAMPLES;
		ctx->sync_error_count--;
	}

	int tail = (ctx->sync_error_head + ctx->sync_error_count) %
		   IRL_SYNC_ERROR_SAMPLES;
	ctx->sync_error_window[tail] = error_ns;
	ctx->sync_error_time[tail] = now_ns;
	ctx->sync_error_count++;

	/* Age out anything past the window. Never empties: the sample just
	 * pushed is by definition current. */
	while (ctx->sync_error_count > 1 &&
	       now_ns - ctx->sync_error_time[ctx->sync_error_head] >
		       IRL_SYNC_ERROR_WINDOW_NS) {
		ctx->sync_error_head =
			(ctx->sync_error_head + 1) % IRL_SYNC_ERROR_SAMPLES;
		ctx->sync_error_count--;
	}

	int64_t sorted[IRL_SYNC_ERROR_SAMPLES];
	for (int i = 0; i < ctx->sync_error_count; i++) {
		sorted[i] = ctx->sync_error_window[(ctx->sync_error_head + i) %
						   IRL_SYNC_ERROR_SAMPLES];
	}
	qsort(sorted, (size_t)ctx->sync_error_count, sizeof(sorted[0]),
	      cmp_int64);

	/* A tenth off each end, which is enough to lose a corrupt timecode
	 * without losing the shape of the distribution. Below ten samples a
	 * tenth rounds to nothing, so trim one instead: a window that small is
	 * a feed that has only just started, and one bad sample in three would
	 * otherwise pass straight through. The count kept is never less than
	 * one — at three samples this is exactly the median again. */
	int trim = ctx->sync_error_count / 10;
	if (trim == 0 && ctx->sync_error_count >= 3)
		trim = 1;

	const int kept = ctx->sync_error_count - 2 * trim;
	int64_t sum = 0;
	for (int i = trim; i < trim + kept; i++)
		sum += sorted[i];

	return sum / kept;
}

/* The smallest offset the control can be set to that still clears `peak_ms`.
 * See irl_sync_snapshot.required_offset_ms. */
static int64_t required_offset_ms(int64_t peak_ms)
{
	if (peak_ms <= 0)
		return 0;
	return ((peak_ms + IRL_SYNC_OFFSET_STEP_MS - 1) /
		IRL_SYNC_OFFSET_STEP_MS) *
	       IRL_SYNC_OFFSET_STEP_MS;
}

static void publish(struct irl_source *ctx)
{
	struct irl_sync_snapshot snap = {0};

	snap.status = ctx->sync_status;
	snap.tc_reason = ctx->sync_tc_reason;
	snap.have_timecode = ctx->sync_have_tc;
	snap.tc = ctx->sync_tc;
	snap.latency_ms = ctx->sync_latency_ns / 1000000LL;
	snap.added_ms = ctx->sync_hold_ns / 1000000LL;
	snap.error_ms = ctx->sync_error_ns / 1000000LL;

	int64_t peak_ns = peak_value(ctx);
	snap.latency_peak_ms = peak_ns / 1000000LL;
	snap.required_offset_ms = required_offset_ms(snap.latency_peak_ms);

	irl_sync_publish(ctx, &snap);
}

/* ── Reset ────────────────────────────────────────────────── */

void irl_sync_reset(struct irl_source *ctx)
{
	delay_clear(&ctx->sync_delay);

	irl_mutex_lock(&ctx->audio_state_lock);
	ctx->sync_present_bias_ns = 0;
	ctx->sync_present_bias_valid = false;
	ctx->sync_anchor_defer_ns = 0;
	irl_mutex_unlock(&ctx->audio_state_lock);
	os_atomic_set_bool(&ctx->sync_anchor_defer_pending, false);

	ctx->sync_engaged = false;
	ctx->sync_seed_reported = false;
	ctx->sync_status = IRL_SYNC_OFF;
	ctx->sync_tc_reason = IRL_SYNC_TC_OK;
	ctx->sync_have_tc = false;
	ctx->sync_hold_ns = 0;
	ctx->sync_latency_ns = 0;
	ctx->sync_error_ns = 0;
	error_filter_reset(ctx);
	os_atomic_set_bool(&ctx->sync_prime_hold, false);
	ctx->sync_released_once = false;
	ctx->sync_prime_deadline_ns = 0;
	ctx->sync_fps_interval_ns = 0;
	ctx->sync_fps_next = 0;
	ctx->sync_fps_count = 0;
	ctx->sync_tc_max_frames = 0;
	ctx->sync_tc_second = 0;
	ctx->sync_tc_have_second = false;
	ctx->sync_last_tc_ns = 0;
	ctx->sync_last_adjust_ns = 0;
	ctx->sync_settle_until_ns = 0;
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
		     (long long)required_offset_ms(peak_value(ctx) / 1000000LL));
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

	/* Nothing measured yet is not the same as measured zero. Between engage
	 * and the delay line's first release there is no reading at all, and
	 * sync_error_ns is still the 0 it was reset to — which would otherwise
	 * be inside the lock tolerance and report Locked for the several
	 * seconds the line is filling. */
	if (ctx->sync_error_count == 0) {
		ctx->sync_locked = false;
		ctx->sync_status = IRL_SYNC_ACQUIRING;
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

/* Fold one timecoded video packet in.
 *
 * With `control` clear this measures and reports but changes nothing: the
 * stamp, the arrival latency and the peak are all still worth having while
 * sync is switched off, because they are what the operator looks at to decide
 * whether to switch it on — is this sender stamping at all, and what offset
 * would it need. The controller stays out of it, and the status stays Off. */
static void observe_timecode(struct irl_source *ctx, const AVPacket *pkt,
			     const struct irl_timecode *tc, uint64_t now_ns,
			     bool control)
{
	tc_rate_observe(ctx, tc);

	int64_t frame_interval_ns;
	const char *interval_origin = "";
	if (!tc_frame_interval_ns(ctx, &frame_interval_ns, &interval_origin)) {
		/* Timecodes are arriving, but nothing has established the frame
		 * rate yet, so n_frames cannot be turned into time. Normally
		 * one timecode second. Hold the stamp for the dock and keep the
		 * staleness clock fed, but measure nothing: a guess here would
		 * seed the hold wrong and sit in the latency peak for a minute
		 * afterwards. */
		ctx->sync_tc = *tc;
		ctx->sync_have_tc = true;
		ctx->sync_last_tc_ns = now_ns;
		if (!control)
			return;
		set_tc_reason(ctx, IRL_SYNC_TC_OK);
		ctx->sync_status = IRL_SYNC_ACQUIRING;
		return;
	}

	int64_t latency_ns;
	if (!timecode_latency_ns(tc, frame_interval_ns, &latency_ns)) {
		/* Timecodes are arriving but there is no NTP reference to
		 * compare them against, so nothing can be aligned. */
		if (!control)
			return;
		ctx->sync_status = IRL_SYNC_NO_TIMECODE;
		set_tc_reason(ctx, IRL_SYNC_TC_NO_CLOCK);
		return;
	}

	ctx->sync_tc = *tc;
	ctx->sync_have_tc = true;
	ctx->sync_last_tc_ns = now_ns;
	ctx->sync_latency_ns = latency_ns;
	peak_record(ctx, latency_ns, now_ns);

	if (!control)
		return;

	set_tc_reason(ctx, IRL_SYNC_TC_OK);

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
	 * jitter cushion; the rest of the error is what the loop below is for,
	 * biased onto the cheap side of zero. See SYNC_SEED_BIAS_NS. */
	if (!ctx->sync_engaged) {
		int64_t nominal_pipeline_ns =
			os_atomic_load_long(&ctx->config.buffer_target_ms) *
			1000000LL;

		ctx->sync_hold_ns = needed_ns - nominal_pipeline_ns +
				    SYNC_SEED_BIAS_NS;
		if (ctx->sync_hold_ns < 0)
			ctx->sync_hold_ns = 0;
		ctx->sync_seed_reported = false;

		/* Hold off measuring until the line has filled and the first
		 * packets have come out the far end. Engage takes the delay
		 * line from empty to a hold's worth of content, so nothing
		 * reaches the decoder — let alone the audio output — for that
		 * long, and every reading before then describes a pipeline
		 * that is not running yet. Without this the loop corrected in
		 * the same call that engaged. */
		ctx->sync_settle_until_ns = now_ns +
					    (uint64_t)ctx->sync_hold_ns +
					    SYNC_SETTLE_MARGIN_NS;
		ctx->sync_engaged = true;
		ctx->sync_locked = false;
		ctx->sync_applied_offset_ms = irl_sync_offset_ms();
		ctx->sync_offset_generation = irl_sync_offset_generation();
		error_filter_reset(ctx);

		/* Now that the hold is known, the priming gate can be given the
		 * time it actually needs: the line goes quiet for the length of
		 * the hold before its first release. The initial deadline only
		 * ever covered "will this source engage at all". */
		ctx->sync_prime_deadline_ns = now_ns +
					      (uint64_t)ctx->sync_hold_ns +
					      SYNC_PRIME_WAIT_NS;

		blog(LOG_INFO,
		     "[irl-source] Sync engaged: %.2ffps (from %s), feed latency %lldms, offset %dms, holding %lldms (%lldms of that is seed bias)",
		     1000000000.0 / (double)frame_interval_ns, interval_origin,
		     (long long)(latency_ns / 1000000LL), irl_sync_offset_ms(),
		     (long long)(ctx->sync_hold_ns / 1000000LL),
		     (long long)(SYNC_SEED_BIAS_NS / 1000000LL));
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
		/* The target just moved by seconds. Every sample in the window
		 * describes the old one, and an average of stale readings would
		 * hold the loop back from a change the user asked for. */
		error_filter_reset(ctx);
	}

	int64_t pts_ns = 0;
	bool have_pts = false;
	if (pkt->pts != AV_NOPTS_VALUE && ctx->fmt_ctx &&
	    ctx->video_stream_idx >= 0) {
		AVStream *vs = ctx->fmt_ctx->streams[ctx->video_stream_idx];
		pts_ns = av_rescale_q(pkt->pts, vs->time_base,
				      (AVRational){1, 1000000000});
		have_pts = true;
	}

	/* Where this content should land, in the OBS clock. `now_ns` cancels
	 * out of it — target = now + offset - (now - capture) = capture +
	 * offset — so this carries no arrival jitter, only the frame the
	 * timecode was quantised to. */
	const int64_t target_ns = (int64_t)now_ns + needed_ns;

	/* Publish it as the playout offset it implies, for the audio output to
	 * anchor on when it primes. Smoothed, because a single reading inherits
	 * the timecode's one-frame grid — 33ms at 30fps, most of the lock
	 * tolerance — and the anchor is a one-shot placement with no second
	 * chance. Eight samples is a third of a second at 30fps and averages
	 * that grid away without lagging an offset change the loop below would
	 * have to unpick. */
	if (have_pts) {
		const int64_t raw_ns = target_ns - pts_ns;
		irl_mutex_lock(&ctx->audio_state_lock);
		ctx->sync_present_bias_ns =
			ctx->sync_present_bias_valid
				? ctx->sync_present_bias_ns +
					  (raw_ns - ctx->sync_present_bias_ns) / 8
				: raw_ns;
		ctx->sync_present_bias_valid = true;
		irl_mutex_unlock(&ctx->audio_state_lock);
	}

	int64_t presentation_ns;
	if (have_pts &&
	    predict_presentation_ns(ctx, pts_ns, &presentation_ns)) {
		ctx->sync_error_ns = error_filter_push(
			ctx, presentation_ns - target_ns, now_ns);

		/* Wait for the last correction to reach the measurement before
		 * making another one.
		 *
		 * This loop's dead time is its own actuator: the error is
		 * derived from the audio playout offset, which describes audio
		 * that was *released* a hold ago, so a change to the hold does
		 * not show up in the error until roughly a hold later.
		 * Correcting per packet against a stale reading is positive
		 * feedback dressed as negative — at 60 timecodes a second it
		 * re-applied the same full correction until the hold hit its
		 * ceiling within a fraction of a second, leaving the stream
		 * tens of seconds behind. One correction per settling
		 * interval converges geometrically instead. */
		if (now_ns >= ctx->sync_settle_until_ns) {
			/* Positive error means we are late, so hold less.
			 * Unlocked the correction is applied outright —
			 * nothing is aligned yet, so there is no smoothness to
			 * protect, and the gate above is what makes a full
			 * step safe. Once locked it is rate limited, which
			 * keeps the resulting playback trim inside the speed
			 * controller's inaudible band. */
			int64_t step_ns = -ctx->sync_error_ns;
			if (ctx->sync_locked) {
				int64_t limit_ns = (int64_t)(
					(double)elapsed_ns * SYNC_SLEW_RATE);
				if (step_ns > limit_ns)
					step_ns = limit_ns;
				if (step_ns < -limit_ns)
					step_ns = -limit_ns;
			}

			ctx->sync_hold_ns += step_ns;
			ctx->sync_last_adjust_ns = now_ns;

			/* The first reading after engage is how far the seed
			 * actually missed, and its sign says whether the bias
			 * is doing its job: positive means the source started
			 * late, which is the direction that corrects at +5%.
			 * Said once per engage, so it costs nothing. */
			if (!ctx->sync_seed_reported) {
				ctx->sync_seed_reported = true;
				blog(LOG_INFO,
				     "[irl-source] Sync seed missed by %lldms (%s); correcting",
				     (long long)(ctx->sync_error_ns / 1000000LL),
				     ctx->sync_error_ns >= 0 ? "late, fast to fix"
							     : "early, slow to fix");
			}
		}
	}

	/* Ceiling from first principles rather than the configured maximum.
	 *
	 * hold = (offset - latency) - pipeline, and the pipeline delay is never
	 * negative, so the hold can never legitimately exceed what is still
	 * needed. Bounding it here means a misbehaving loop overshoots by the
	 * pipeline delay rather than running to IRL_SYNC_MAX_OFFSET_MS and
	 * parking the stream half a minute behind.
	 *
	 * Plus headroom, because two of the overshoots above it are deliberate
	 * and one of them is not even physically bounded by zero: the startup
	 * backlog trim discards audio after sync has chosen its hold, so the
	 * pipeline it is subtracting can genuinely come out negative. The
	 * allowance is exactly what the two can add — the seed bias, and the
	 * largest pre-roll the anchor is allowed to hand back. */
	int64_t max_hold_ns = needed_ns > 0
				      ? needed_ns + SYNC_SEED_BIAS_NS +
						SYNC_ANCHOR_MAX_WAIT_NS
				      : 0;
	if (ctx->sync_hold_ns > max_hold_ns)
		ctx->sync_hold_ns = max_hold_ns;
	if (ctx->sync_hold_ns < 0)
		ctx->sync_hold_ns = 0;

	/* The one invariant that keeps every path above safe, and deliberately
	 * applied last so the ceiling cannot bypass it: the hold may grow as
	 * fast as it likes — that only makes packets wait longer — but it may
	 * never shrink faster than the pipeline can absorb. Shrinking it makes
	 * queued packets due sooner, and a large enough cut (the user lowering
	 * the offset by seconds, or a momentary latency spike pulling the
	 * ceiling down) would make the whole line due at once and overrun the
	 * buffers downstream. Capping the decrease turns that into an ordinary
	 * drain instead. */
	int64_t floor_ns = prev_hold_ns -
			   (int64_t)((double)elapsed_ns * SYNC_RELEASE_RATE);
	if (ctx->sync_hold_ns < floor_ns)
		ctx->sync_hold_ns = floor_ns;
	if (ctx->sync_hold_ns < 0)
		ctx->sync_hold_ns = 0;

	/* Arm the gate whenever the hold actually moved. A change takes the
	 * larger of the old and new holds to work through the line — packets
	 * already queued drain at their existing release times — plus the rest
	 * of the pipeline. */
	if (ctx->sync_hold_ns != prev_hold_ns) {
		int64_t propagation_ns = ctx->sync_hold_ns > prev_hold_ns
						 ? ctx->sync_hold_ns
						 : prev_hold_ns;
		ctx->sync_settle_until_ns =
			now_ns + (uint64_t)propagation_ns +
			SYNC_SETTLE_MARGIN_NS;
	}

	update_status(ctx, now_ns, reachable);
}

/* ── Receiver-thread entry points ─────────────────────────── */

/* Whether the audio thread should still be holding off priming. Recomputed on
 * every packet, from the receiver thread that owns all of this state.
 *
 * Released as soon as any of the reasons to wait stops applying: the hold is
 * seeded, the source turns out to have nothing to align against, sync is off,
 * or the deadline passes. */
static void update_prime_gate(struct irl_source *ctx, uint64_t now_ns)
{
	/* Engaging is not the finish line: seeding the hold stops the line for
	 * the length of the hold, so priming the instant it engages starves the
	 * pipeline exactly as priming before it did. What the gate is waiting
	 * for is the line flowing again — the first release. */
	/* A source already arriving late enough for the offset needs no hold,
	 * so the line never queues anything and there is no first release to
	 * wait for. It is flowing the moment it engages. */
	bool flowing = ctx->sync_released_once ||
		       (ctx->sync_engaged && ctx->sync_hold_ns <= 0);

	bool hold = sync_wanted(ctx) && !flowing &&
		    ctx->sync_status != IRL_SYNC_NO_TIMECODE &&
		    now_ns < ctx->sync_prime_deadline_ns;

	if (hold != os_atomic_load_bool(&ctx->sync_prime_hold))
		os_atomic_set_bool(&ctx->sync_prime_hold, hold);
}

bool irl_sync_prime_held(struct irl_source *ctx)
{
	return os_atomic_load_bool(&ctx->sync_prime_hold);
}

bool irl_sync_playout_anchor(struct irl_source *ctx, int64_t pts_ns,
			     uint64_t now_ns, uint64_t *anchor_ns)
{
	if (pts_ns <= 0)
		return false;

	irl_mutex_lock(&ctx->audio_state_lock);
	const bool valid = ctx->sync_present_bias_valid;
	const int64_t bias_ns = ctx->sync_present_bias_ns;
	irl_mutex_unlock(&ctx->audio_state_lock);

	if (!valid)
		return false;

	/* The playout offset the loop spends its life steering toward, applied
	 * here in one step because nothing is playing yet to disturb. */
	const int64_t placed_ns = pts_ns + bias_ns;
	const int64_t wait_ns = placed_ns - (int64_t)now_ns;
	if (wait_ns <= 0 || wait_ns > SYNC_ANCHOR_MAX_WAIT_NS)
		return false;

	/* Hand the wait back so the delay line can absorb it. */
	irl_mutex_lock(&ctx->audio_state_lock);
	ctx->sync_anchor_defer_ns = wait_ns;
	irl_mutex_unlock(&ctx->audio_state_lock);
	os_atomic_set_bool(&ctx->sync_anchor_defer_pending, true);

	*anchor_ns = (uint64_t)placed_ns;
	return true;
}

/* Take over the pre-roll the audio output is sitting out before it starts.
 *
 * The anchor places the output clock exactly, and whatever wait that implies
 * has to be absorbed somewhere. Left alone it lands in the jitter buffer, which
 * then starts playback several hundred milliseconds above target — and the
 * speed controller drains that excess at +5%, which pulls the playout earlier
 * and unwinds the placement it was there to make.
 *
 * So the delay line takes it instead. Pausing the input for the same window the
 * output is waiting leaves the buffer exactly where it was, with nothing for
 * the speed controller to do. Safe here specifically because nothing is playing
 * yet: the gap this opens is one the output is already waiting through. */
static void apply_anchor_defer(struct irl_source *ctx, uint64_t now_ns)
{
	if (!os_atomic_load_bool(&ctx->sync_anchor_defer_pending))
		return;

	irl_mutex_lock(&ctx->audio_state_lock);
	const int64_t defer_ns = ctx->sync_anchor_defer_ns;
	ctx->sync_anchor_defer_ns = 0;
	irl_mutex_unlock(&ctx->audio_state_lock);
	os_atomic_set_bool(&ctx->sync_anchor_defer_pending, false);

	if (defer_ns <= 0)
		return;

	ctx->sync_hold_ns += defer_ns;
	delay_shift_later(&ctx->sync_delay, defer_ns);

	/* The queued packets moved with the hold, so this change is already at
	 * the line's output rather than a hold away from it. The gate only has
	 * to cover the rest of the pipeline. */
	ctx->sync_settle_until_ns = now_ns + SYNC_SETTLE_MARGIN_NS;

	blog(LOG_INFO,
	     "[irl-source] Sync: delay line took over the %lldms pre-roll; holding %lldms",
	     (long long)(defer_ns / 1000000LL),
	     (long long)(ctx->sync_hold_ns / 1000000LL));
}

void irl_sync_observe(struct irl_source *ctx, const AVPacket *pkt)
{
	uint64_t now_ns = os_gettime_ns();

	apply_anchor_defer(ctx, now_ns);

	/* First packet of the stream: start the clock the gate runs against.
	 * Here rather than in irl_sync_reset because reset has no `now`, and
	 * nothing can prime before a packet has been read anyway. */
	if (ctx->sync_prime_deadline_ns == 0)
		ctx->sync_prime_deadline_ns = now_ns + SYNC_PRIME_WAIT_NS;

	/* Whether the controller may act. Reading the timecodes does not
	 * depend on it: a source that is switched off still reports what it is
	 * receiving, which is the only way to tell a sender that is not
	 * stamping from one that is and simply has not been turned on yet. */
	const bool control = sync_wanted(ctx);

	if (!control && ctx->sync_status != IRL_SYNC_OFF) {
		ctx->sync_status = IRL_SYNC_OFF;
		ctx->sync_tc_reason = IRL_SYNC_TC_OK;
		ctx->sync_error_ns = 0;
		ctx->sync_locked = false;
	}

	if (pkt->stream_index == ctx->video_stream_idx && pkt->data &&
	    pkt->size > 0) {
		struct irl_timecode tc;

		if (irl_sei_find_timecode(pkt->data, (size_t)pkt->size, &tc))
			observe_timecode(ctx, pkt, &tc, now_ns, control);
	}

	/* Whatever hold was built up is handed back gradually rather than
	 * dropped in one go. Stays engaged=false so re-enabling re-seeds from
	 * scratch. */
	bool give_back_hold = !control;

	/* No timecode for a while: either the sender never sends them or it
	 * stopped. Both leave the source unalignable, and the delay line has
	 * to let go rather than hold a stale amount forever. */
	if (ctx->sync_last_tc_ns == 0 ||
	    now_ns - ctx->sync_last_tc_ns > SYNC_TC_STALE_NS) {
		if (control && ctx->sync_status != IRL_SYNC_NO_TIMECODE) {
			ctx->sync_status = IRL_SYNC_NO_TIMECODE;
			ctx->sync_error_ns = 0;
			ctx->sync_locked = false;
		}
		if (control)
			set_tc_reason(ctx, diagnose_missing_timecode(ctx));
		/* Nothing left to display either, so the stamp goes with it —
		 * a frozen timecode is worse than none. */
		ctx->sync_have_tc = false;
		give_back_hold = true;
	}

	if (give_back_hold) {
		ctx->sync_engaged = false;
		release_hold(ctx, now_ns);
	}

	update_prime_gate(ctx, now_ns);

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

		/* The line has started flowing again, which is the condition
		 * the priming gate is really waiting on. */
		ctx->sync_released_once = true;
	}
}
