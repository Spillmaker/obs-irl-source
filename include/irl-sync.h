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
 * receiver-sync.c sets the bucket duration. */
#define IRL_SYNC_PEAK_BUCKETS 12

/* How long the presentation error is median-filtered over. See
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

/* Completed timecode seconds the frame rate is held over. A rolling maximum,
 * so a second that lost packets cannot drag the estimate down, and a genuine
 * rate change still lands within this many seconds. */
#define IRL_SYNC_FPS_SECONDS 4

/* ── Per-source status ────────────────────────────────────── */

enum irl_sync_status {
	/* Global sync off, or this source is not opted in. */
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
 * loaded by irl_sync_init() and written by each setter below — global rather
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

void irl_sync_init(void);
void irl_sync_shutdown(void);

/* ── Dock (sync-dock.cpp) ─────────────────────────────────── */

/* Defined only when the plugin is built with IRL_ENABLE_DOCK, which needs Qt
 * and obs-frontend-api. Without it everything above still works and the
 * status is reachable over the websocket vendor. */
void irl_sync_dock_register(void);
void irl_sync_dock_unregister(void);

#ifdef __cplusplus
}
#endif
