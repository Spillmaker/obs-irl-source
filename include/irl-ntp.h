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
 * Polling is gated on irl_ntp_set_enabled(), which sync-group.c ties to the
 * master sync switch. A plugin that quietly sent packets to a third-party pool
 * every minute on behalf of users who never turned sync on would be making a
 * network request nobody asked for; the cost of gating is a few seconds before
 * the first sync lands after enabling.
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
	 * `synced`; see irl_ntp_utc_now_ns(). */
	int64_t offset_ns;
	/* Round-trip time of the sample the current offset came from, and how
	 * long ago that sample was taken. The dock shows both: an offset from
	 * a 400ms round trip is worth less than one from 8ms, and a sync that
	 * stopped refreshing an hour ago is a broken reference that would
	 * otherwise keep ticking plausibly. */
	int64_t rtt_ns;
	uint64_t age_ns;
	char server[128];
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
