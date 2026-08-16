/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * irl-sync.h — timecode synchronisation: global settings and source registry.
 *
 * The model is absolute, not relative. Every synced source independently
 * presents the frame stamped T at NTP time T + offset; sources never
 * negotiate with each other, and none of them needs to know the others
 * exist. Two OBS instances anywhere, on the same NTP reference and the same
 * offset, therefore composite the same captured moment at the same instant —
 * which is the point, and is why the offset is one global number rather than
 * a per-source setting.
 *
 * That choice also keeps the hot path free of cross-source coordination: the
 * receiver thread reads two atomics and its own timecodes. What lives here is
 * only the settings those atomics come from, plus a registry that exists so
 * the dock and the websocket vendor have something to enumerate.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <obs-module.h>
#include <libavcodec/avcodec.h>

#include "irl-sei.h"

#ifdef __cplusplus
extern "C" {
#endif

struct irl_source;

/* ── Limits ───────────────────────────────────────────────── */

#define IRL_SYNC_DEFAULT_OFFSET_MS 5000
#define IRL_SYNC_MIN_OFFSET_MS 0
/* Held as compressed packets, so the cost is bitrate x offset rather than
 * decoded frames: 30s of a 6Mbit/s feed is about 22MB. The ceiling is here to
 * bound a typo, not because the buffering is expensive. */
#define IRL_SYNC_MAX_OFFSET_MS 30000
#define IRL_SYNC_DEFAULT_NTP_SERVER "pool.ntp.org"

/* The offset is set, and reported, in whole seconds. It is a number
 * co-streamers read to each other to land on the same target, and "six" is a
 * thing two people can agree on over a call in a way that 6123ms is not. The
 * stored value stays in milliseconds because everything downstream computes in
 * time, not in UI steps. */
#define IRL_SYNC_OFFSET_STEP_MS 1000

/* Rolling window over which arrival latency is peak-held, as a bucket count;
 * sync-control.c sets the bucket duration. */
#define IRL_SYNC_PEAK_BUCKETS 12

/* How long the presentation error is averaged over. See
 * error_filter_push().
 *
 * A duration, not a sample count, because sources run at anything from 25 to
 * 240fps: a fixed number of samples would be a different filter on every feed,
 * and at the low end would put seconds of lag into the control loop. The
 * capacity is what a duration costs at the fastest rate the interval bounds
 * accept (IRL_VIDEO_INTERVAL_MIN_NS, 250fps), so the window is time-bounded on
 * every source rather than sample-bounded on the fast ones. */
#define IRL_SYNC_ERROR_WINDOW_NS 1000000000ULL
#define IRL_SYNC_ERROR_SAMPLES 256

/* Completed timecode seconds the frame rate is decided from. Enough for two
 * of them to agree while a damaged one is outvoted, and small enough that a
 * genuine rate change still lands within this many seconds. */
#define IRL_SYNC_FPS_SECONDS 4

/* ── Per-source status ────────────────────────────────────── */

enum irl_sync_status {
	/* Nothing to align: sync switched off globally, this source not opted
	 * in, or — the common one, since the dock only lists sources that are
	 * opted in — no stream arriving. Deliberately not an alarm state. A
	 * feed that is not connected is the normal condition before someone
	 * goes live, and blinking at the operator for it is how a warning gets
	 * trained away. */
	IRL_SYNC_OFF,
	/* Opted in, but nothing to align against: no SEI timecode in the
	 * stream (H.264, RTMP, or timecodes not enabled on the sender), or
	 * no NTP reference on this machine. By far the most common reason
	 * sync "does not work", and not a failure of the sync itself — which
	 * is why it is its own state and not an error. */
	IRL_SYNC_NO_TIMECODE,
	/* Timecodes were arriving and stopped. */
	IRL_SYNC_STALE,
	/* The feed arrives later than the target offset, so its frames are
	 * already past their slot when they get here. Nothing but a larger
	 * offset (or a better uplink) fixes this. */
	IRL_SYNC_TOO_SLOW,
	/* Converging. Expected at startup and after an offset change. */
	IRL_SYNC_ACQUIRING,
	IRL_SYNC_LOCKED,
};

const char *irl_sync_status_name(enum irl_sync_status status);

/* Why a source that wants to sync has no timecode to align against.
 *
 * "No timecode" on its own sends people hunting in the wrong place — the cause
 * is almost always one of three specific things, and each has a different fix.
 * Reporting which one turns a dead end into an instruction. */
enum irl_sync_tc_reason {
	IRL_SYNC_TC_OK,
	/* This machine has no NTP reference, so nothing can be compared. */
	IRL_SYNC_TC_NO_CLOCK,
	/* The stream is not H.265. Moblin only writes the SEI on HEVC (its
	 * H.264 path is disabled at the source), and the timecode SEI this
	 * reads is HEVC-specific, so H.264 can never carry one. */
	IRL_SYNC_TC_CODEC,
	/* HEVC, but no time_code SEI is arriving: Timecodes off on the sender,
	 * no NTP pool set there, or a transport that drops the SEI. */
	IRL_SYNC_TC_ABSENT,
};

const char *irl_sync_tc_reason_name(enum irl_sync_tc_reason reason);

/* What one source publishes for the dock and the websocket vendor.
 *
 * The three latency figures answer different questions and are deliberately
 * not collapsed into one: `latency_ms` is how stale the freshest data is (a
 * property of the sender and the network, unaffected by any setting),
 * `added_ms` is how much extra hold this plugin is applying to reach the
 * target, and `error_ms` is how far the actual presentation lands from it. */
struct irl_sync_snapshot {
	enum irl_sync_status status;
	/* Only meaningful while status is IRL_SYNC_NO_TIMECODE. */
	enum irl_sync_tc_reason tc_reason;
	bool have_timecode;
	struct irl_timecode tc;
	int64_t latency_ms;
	/* Rolling maximum. Instantaneous latency reads fine right up until a
	 * bitrate dip, so this is what an offset should be chosen against. */
	int64_t latency_peak_ms;
	int64_t added_ms;
	int64_t error_ms;
	/* The smallest settable offset at which this source could hold sync:
	 * latency_peak_ms rounded up to a whole second. Shown so a red row says
	 * what to do about it.
	 *
	 * Deliberately not padded with headroom. A source locks whenever the
	 * offset is at or above its arrival latency, so quoting a padded figure
	 * as the requirement reads as a bug the first time someone locks fine
	 * below it. Rounding up to the step the control offers gives half a
	 * second of slack on average and stays honest. */
	int64_t required_offset_ms;
};

/* ── Global settings ──────────────────────────────────────── */

struct irl_sync_config {
	bool enabled;
	int offset_ms;
	char ntp_server[128];
};

/* Read the current settings. They persist in the module config directory,
 * loaded at module load and written by each setter below — global rather
 * than per-source, so they live outside the scene collection: the offset is a
 * number a group of streamers agrees on, not a property of one machine's
 * layout. A scene collection copied to another machine therefore arrives
 * without it, which is the intended behaviour. */
void irl_sync_config_get(struct irl_sync_config *out);
void irl_sync_set_enabled(bool enabled);
void irl_sync_set_offset_ms(int offset_ms);
void irl_sync_set_ntp_server(const char *host);

/* Hot-path readers. Backed by atomics; safe from any thread. */
bool irl_sync_is_enabled(void);
int irl_sync_offset_ms(void);
/* Bumped whenever the offset changes, so a receiver can notice and re-seat
 * instead of slewing several seconds at the drift rate. */
uint32_t irl_sync_offset_generation(void);

/* ── Source registry ──────────────────────────────────────── */

#define IRL_SYNC_MAX_SOURCES 64

struct irl_sync_entry {
	char source_name[256];
	bool sync_enabled;
	struct irl_sync_snapshot snap;
};

void irl_sync_register_source(struct irl_source *ctx);
void irl_sync_unregister_source(struct irl_source *ctx);

/* Publish this source's latest status. Called from the receiver thread. */
void irl_sync_publish(struct irl_source *ctx,
		      const struct irl_sync_snapshot *snap);

/* Snapshot every registered source. Returns how many entries were written. */
size_t irl_sync_collect(struct irl_sync_entry *entries, size_t max);

/* One source's published status, for its own get_stats proc. False when the
 * source is not registered. */
bool irl_sync_get_snapshot(struct irl_source *ctx,
			   struct irl_sync_snapshot *out);

/* ── Receiver hooks ───────────────────────────────────────── */

/* Everything the media path calls, and the whole of it. Three of these are in
 * the read loop, two in the audio pump and two in the source lifecycle; each
 * is one line at its call site, and each call site carries a "timecode sync"
 * marker comment so the whole set greps out in one go.
 *
 * They take struct irl_source because a controller over the pipeline has to
 * see the pipeline. That is the one direction of coupling this split does not
 * remove, and hiding it behind an abstraction would cost more than it buys. */
/* Fold a freshly read packet into the controller — extract its timecode if it
 * has one, measure, move the hold — and then decide whether the delay line
 * keeps it. True means the line took it and the caller must not dispatch it.
 *
 * The two halves are one call because the read loop has no use for them apart:
 * measuring without holding would be a stats feature, and holding without
 * measuring cannot know for how long. */
bool irl_sync_intercept(struct irl_source *ctx, AVPacket *pkt);
/* Release everything whose moment has come. */
void irl_sync_drain(struct irl_source *ctx, AVFrame *frame);
/* Block while the delay line is at its ceiling, draining as room appears, so
 * the excess is held by the transport rather than by this process. False when
 * the thread was asked to stop while waiting — the caller must break. */
bool irl_sync_wait_for_room(struct irl_source *ctx, AVFrame *frame);
/* True while the audio output must not prime yet: sync is about to seed a hold,
 * and a hold applied to an already-running pipeline starves it. Read from the
 * audio thread. */
bool irl_sync_prime_held(struct irl_source *ctx);
/* Where the audio holding this PTS has to start playing, in the OBS clock, for
 * the stream to land on its configured offset. False when sync has nothing to
 * say — no timecodes, no NTP reference, sync off — or when the placement is
 * already in the past, which anchoring cannot fix. Called from the audio thread
 * at prime. */
bool irl_sync_playout_anchor(struct irl_source *ctx, int64_t pts_ns,
			     uint64_t now_ns, uint64_t *anchor_ns);
void irl_sync_reset(struct irl_source *ctx);
void irl_sync_free(struct irl_source *ctx);

/* ── Statistics ───────────────────────────────────────────── */

/* The fields sync contributes to a source's get_stats proc, as the declaration
 * fragment and the code that fills it. Both live here so they cannot drift
 * apart, and so irl-source.c carries one line of each instead of twenty. The
 * third place a new field has to appear is irl_stat_fields[] in
 * websocket-vendor.c; see the table in README.md. */
#define IRL_SYNC_STATS_PROC_DECL \
	"out bool sync_enabled, " \
	"out string sync_status, out string sync_timecode, " \
	"out int sync_latency_ms, out int sync_latency_peak_ms, " \
	"out int sync_added_ms, out int sync_error_ms, " \
	"out int sync_required_offset_ms"

void irl_sync_stats(struct irl_source *ctx, calldata_t *cd);

/* The same fields as rows for irl_stat_fields[] in websocket-vendor.c, so the
 * two lists cannot fall out of step. */
#define IRL_SYNC_STAT_FIELDS \
	{"sync_enabled", IRL_STAT_BOOL}, \
	{"sync_status", IRL_STAT_STRING}, \
	{"sync_timecode", IRL_STAT_STRING}, \
	{"sync_latency_ms", IRL_STAT_INT}, \
	{"sync_latency_peak_ms", IRL_STAT_INT}, \
	{"sync_added_ms", IRL_STAT_INT}, \
	{"sync_error_ms", IRL_STAT_INT}, \
	{"sync_required_offset_ms", IRL_STAT_INT},

/* The vendor's GetSyncStatus request, in the shape obs-websocket wants. The
 * whole handler lives in src/sync/ so websocket-vendor.c carries one table row
 * and one dispatch entry. */
void irl_sync_vendor_status(obs_data_t *request_data,
			    obs_data_t *response_data, void *priv_data);

/* ── Per-source settings ──────────────────────────────────── */

/* The "Sync" checkbox and its help text, added to the source's own properties.
 * Called from settings.c; the wording, the key and the default all live in
 * src/sync/ so a change to any of them touches nothing else. */
void irl_sync_source_defaults(obs_data_t *settings);
void irl_sync_source_properties(obs_properties_t *props);

/* ── Module lifecycle ─────────────────────────────────────── */

/* The whole feature's hooks into the plugin's own lifecycle, one call each, so
 * plugin.c never has to know what sync is made of. Everything they start —
 * the NTP client, the settings store, the source registry, the dock — is
 * private to src/sync/.
 *
 * _post_load exists because two of those pieces cannot run any earlier: the
 * dock needs the frontend, which is only guaranteed once module loading has
 * finished. */
void irl_sync_module_load(void);
void irl_sync_module_post_load(void);
void irl_sync_module_unload(void);

/* ── Dock (sync-dock.cpp) ─────────────────────────────────── */

/* Defined only when the plugin is built with IRL_ENABLE_DOCK, which needs Qt
 * and obs-frontend-api. Without it everything above still works and the
 * status is reachable over the websocket vendor. Called from
 * irl_sync_module_post_load(); nothing outside src/sync/ uses these. */
void irl_sync_dock_register(void);
void irl_sync_dock_unregister(void);

#ifdef __cplusplus
}
#endif
