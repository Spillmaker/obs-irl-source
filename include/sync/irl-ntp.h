/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * irl-ntp.h — module-wide SNTP client.
 *
 * Timecode sync compares a sender's wall clock against this machine's, so
 * both ends have to be disciplined against the same reference. The host
 * system clock is not good enough for that: Windows' w32time defaults to a
 * nine-hour poll and roughly one-second accuracy, which is two orders of
 * magnitude past the frame-level alignment this feature promises. Moblin
 * does not trust iOS's clock either — it runs its own NTP client and stamps
 * frames from that — so the receiving end matches it.
 *
 * One client per module, shared by every source. Thread-safe.
 *
 * Polling runs from module load, not from the master sync switch. It used to
 * be gated on the switch, so as not to send packets to a third-party pool for
 * users who never turn sync on, and the cost was a dock whose clock stayed
 * dead until sync had been enabled once. That is the wrong way round: the
 * clock is what an operator reads to decide whether sync is worth turning on.
 * irl_ntp_set_enabled() remains for that gate to be reinstated.
 *
 * The client has two polling policies, chosen from the server name (see
 * sync-ntp.c):
 *
 *   Burst — for the servers this project runs, which are there to be asked.
 *     An acquisition burst on arrival, then one packet every 1 to 30 seconds
 *     at random, which is dense enough to measure this machine's crystal and
 *     correct for it between polls.
 *
 *   Standard — for public pools, which are not. One packet per poll at the
 *     conventional 64-second cadence, backing off when the server says to.
 *
 * A burst server that stops answering fails over to a public pool and keeps
 * probing for its return, so the clock degrades rather than stops.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct irl_ntp_status {
	bool synced;
	/* utc_ns = os_gettime_ns() + offset_ns. Only meaningful when
	 * `synced`; see irl_ntp_utc_now_ns(). Carries the drift correction
	 * already, so it matches what the sources are being placed on. */
	int64_t offset_ns;
	/* Round-trip time of the sample the current offset came from, and how
	 * long ago that sample was taken. The dock shows both: an offset from
	 * a 400ms round trip is worth less than one from 8ms, and a sync that
	 * stopped refreshing an hour ago is a broken reference that would
	 * otherwise keep ticking plausibly. */
	int64_t rtt_ns;
	uint64_t age_ns;
	/* The server actually being polled, which is the fallback pool rather
	 * than `primary` while `fallback_active`. */
	char server[128];
	char primary[128];

	/* Which policy `server` is being polled under, and whether the
	 * configured server had to be abandoned to reach it. */
	bool burst_mode;
	bool fallback_active;

	/* This machine's measured crystal error, positive when the local
	 * clock runs fast, in parts per million. Measured under both
	 * policies once enough samples exist, but only applied to the
	 * published offset under the burst policy, where the samples are
	 * dense enough for the fit to be worth trusting. Consumer crystals
	 * sit in the tens of ppm, which is tens of milliseconds per poll
	 * interval — the same order as the alignment this feature is trying
	 * to hold. */
	bool drift_valid;
	double drift_ppm;
};

/* Start/stop the polling thread. Both are idempotent and called from
 * obs_module_load / obs_module_unload. */
void irl_ntp_start(void);
void irl_ntp_stop(void);

/* Change the pool/server hostname. Takes effect on the next poll, and forces
 * one immediately. Passing NULL or "" stops polling and drops the sync. */
void irl_ntp_set_server(const char *host);

/* Start or stop polling. Disabling keeps the last offset rather than dropping
 * it — it simply ages, which irl_ntp_status reports — so re-enabling has
 * something usable before the first fresh sample arrives. */
void irl_ntp_set_enabled(bool enabled);

void irl_ntp_get_status(struct irl_ntp_status *out);

/* Current UTC in nanoseconds since the Unix epoch, or false when no sync has
 * been established. */
bool irl_ntp_utc_now_ns(int64_t *utc_ns);

#ifdef __cplusplus
}
#endif
