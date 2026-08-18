/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * sync-ntp.c — SNTP (RFC 4330) client for timecode sync.
 *
 * Deliberately small: one UDP round trip, four timestamps, the classic
 * offset/delay pair. No stepping of anything system-wide — the result is an
 * offset from os_gettime_ns() to UTC, kept in this module and used only to
 * place received frames on a shared timeline.
 *
 * The offset is what sync depends on, so it is filtered rather than taken
 * raw. Every poll keeps the sample with the lowest round trip (the least
 * queueing in either direction, and so the least asymmetry error). Cellular
 * and Wi-Fi round trips vary by hundreds of milliseconds; the minimum of a
 * burst is far closer to the truth than any single sample.
 *
 * ── Two policies ──────────────────────────────────────────
 *
 * How hard this client is allowed to poll is a property of the server, not
 * of the plugin, so the server name chooses the policy:
 *
 *   Burst (ntp_burst_hosts below) — servers this project runs, for users of
 *     this plugin. Four packets on acquisition, lowest round trip wins, then
 *     one packet every 1 to 30 seconds at random. Random rather than fixed
 *     because every client that starts with OBS would otherwise poll in step
 *     and turn a steady trickle into a spike. Roughly four packets a minute
 *     per client is nothing to a server that exists for this, and it buys the
 *     sample density the drift estimate needs.
 *
 *   Standard (everything else) — public pools, which are donated capacity.
 *     One packet per poll at the conventional 64 seconds, doubling that on a
 *     kiss-o'-death rate code and stopping altogether on a denial. The old
 *     behaviour here was a four-packet burst every minute to whatever pool
 *     the user had set, which is not a good guest.
 *
 * ── Drift ─────────────────────────────────────────────────
 *
 * os_gettime_ns() counts this machine's crystal, and crystals are wrong: tens
 * of ppm is normal, which is tens of milliseconds per minute of poll interval
 * — the same order as the alignment sync is trying to hold. A rolling window
 * of accepted samples is fitted (Theil–Sen, so one bad sample cannot tilt it)
 * to recover that error as a slope, and under the burst policy the slope is
 * used to extrapolate the offset between polls. Under the standard policy the
 * slope is measured and shown but not applied: one sample a minute is too
 * sparse to trust with the timeline, and the fit is worth showing anyway
 * because an operator reading "+40 ppm" learns something real about the
 * machine.
 *
 * Note what the number is measured against. os_gettime_ns() is
 * QueryPerformanceCounter on Windows, which nothing disciplines, so the fit
 * is the raw crystal error. On Linux it is CLOCK_MONOTONIC, which the system
 * NTP daemon does slew, so a machine already running chrony will report a
 * drift near zero. Both readings are correct: it is the error of the clock
 * this plugin actually schedules against.
 *
 * ── Fallback ──────────────────────────────────────────────
 *
 * A burst server that stops answering is a broken clock for everyone using
 * it, so three failed polls hand over to a public pool at the standard
 * cadence, while the configured server is probed again every 30 to 60 seconds
 * (randomised, so that everyone knocked off by the same outage does not come
 * back in step). Drift correction stops while failed over, because the fit is
 * only as good as the sample rate feeding it.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET irl_socket_t;
#define IRL_INVALID_SOCKET INVALID_SOCKET
#define irl_close_socket closesocket
#define irl_strcasecmp _stricmp
#else
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int irl_socket_t;
#define IRL_INVALID_SOCKET (-1)
#define irl_close_socket close
#define irl_strcasecmp strcasecmp
#endif

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "../../include/sync/irl-ntp.h"
#include "../../include/irl-threading.h"

/* Seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (Unix epoch). */
#define NTP_UNIX_EPOCH_DELTA 2208988800ULL

#define NTP_PORT "123"
#define NTP_PACKET_BYTES 48
#define NTP_RECV_TIMEOUT_MS 1200

/* Samples in an acquisition burst; the lowest-RTT one wins. Four is enough to
 * skip a single queued packet without turning a background task into
 * traffic. */
#define NTP_BURST 4
/* Bounds of the randomised burst-policy poll interval. */
#define NTP_BURST_MIN_INTERVAL_MS 1000
#define NTP_BURST_MAX_INTERVAL_MS 30000
/* Retry gap after a failed burst-policy poll. Short, because three of them
 * are what triggers the failover, and a dead server should not hold the clock
 * hostage for a minute first. */
#define NTP_BURST_RETRY_MS 2000

/* Standard policy: RFC 4330's recommended minimum, and the ceiling the
 * kiss-o'-death backoff doubles toward. */
#define NTP_POLL_INTERVAL_MS 64000
#define NTP_POLL_MAX_INTERVAL_MS 1024000
/* Retry faster until a first sync exists, so a source starting alongside OBS
 * does not sit unsynced for a minute. */
#define NTP_RETRY_INTERVAL_MS 8000

/* Backstop for the idle thread while polling is switched off. The real wakeup
 * is the condvar, which every setter signals; this only bounds the damage if a
 * signal is ever missed. */
#define NTP_IDLE_INTERVAL_MS 3600000

/* Failover. */
#define NTP_FALLBACK_SERVER "pool.ntp.org"
#define NTP_FALLBACK_AFTER_FAILURES 3
#define NTP_FALLBACK_PROBE_MIN_MS 30000
#define NTP_FALLBACK_PROBE_MAX_MS 60000

/* A round trip worse than this says nothing useful about the offset. */
#define NTP_MAX_USABLE_RTT_NS 1000000000LL
/* Weight applied to each accepted sample once a sync exists, on the paths
 * where the drift fit is not driving the offset. Slow enough that one bad
 * measurement cannot move the timeline, fast enough to track real crystal
 * drift between polls. */
#define NTP_OFFSET_SMOOTHING 0.25
/* Past this the estimate is stale enough to be a different problem, so re-seat
 * rather than easing toward the new value. */
#define NTP_RESEAT_THRESHOLD_NS 2000000000LL

/* ── Drift fit tuning ─────────────────────────────────────── */

/* Window size. At the burst policy's ~15s mean interval this is about
 * sixteen minutes of samples, which is what the age limit below keeps. */
#define NTP_DRIFT_CAPACITY 64
#define NTP_DRIFT_MAX_AGE_NS (15LL * 60LL * 1000000000LL)
/* Samples whose round trip is much worse than the best in the window carry a
 * correspondingly worse offset, so they are dropped before fitting. The slack
 * term keeps a server with sub-millisecond round trips from rejecting almost
 * everything on jitter alone. */
#define NTP_DRIFT_RTT_SLACK_NS 200000LL
/* A slope is only fitted from enough samples over a long enough baseline: at
 * 20 ppm, three minutes is 3.6ms of signal, which is above the noise of a
 * decent path and below it for a bad one. */
#define NTP_DRIFT_MIN_SAMPLES 8
#define NTP_DRIFT_MIN_SPAN_NS (180LL * 1000000000LL)
/* Pairs closer together than this are noise divided by a small number, so
 * they are left out of the median rather than allowed to widen it. */
#define NTP_DRIFT_MIN_PAIR_NS (30LL * 1000000000LL)
#define NTP_DRIFT_MIN_PAIRS 4
/* Consumer crystals are specified to ±100 ppm and the bad ones sit inside
 * that. A fit past this is a broken measurement, not a broken clock. */
#define NTP_DRIFT_MAX_PPM 500.0
/* Ceiling on the extrapolation itself. At 100 ppm across the longest burst
 * interval the real correction is 3ms, so this only exists to bound the
 * damage if a fit ever survives every check above and is still wrong. */
#define NTP_DRIFT_MAX_CORRECTION_NS 50000000LL

/* ── Server policy ────────────────────────────────────────── */

/* Servers run for this plugin, where the burst policy is both wanted and
 * welcome. Matched case-insensitively, either exactly or as a parent domain,
 * so an entry of "example.com" also covers "ntp1.example.com".
 *
 * A list rather than one name because more of them may follow, and because
 * saying out loud which hosts get the aggressive treatment is better than
 * burying the same rule in an if. */
static const char *const ntp_burst_hosts[] = {
	"ntp.kringkast.com",
};

static bool host_is_burst(const char *host)
{
	if (!host || !*host)
		return false;

	size_t host_len = strlen(host);

	for (size_t i = 0; i < sizeof(ntp_burst_hosts) / sizeof(*ntp_burst_hosts);
	     i++) {
		const char *entry = ntp_burst_hosts[i];
		size_t entry_len = strlen(entry);

		if (irl_strcasecmp(host, entry) == 0)
			return true;

		/* Subdomain: the tail must match and be preceded by a dot, so
		 * "notkringkast.com" does not match "kringkast.com". */
		if (host_len > entry_len + 1 &&
		    host[host_len - entry_len - 1] == '.' &&
		    irl_strcasecmp(host + host_len - entry_len, entry) == 0)
			return true;
	}

	return false;
}

/* ── State ────────────────────────────────────────────────── */

static struct {
	irl_mutex_t lock;
	irl_cond_t wake;
	irl_thread_t thread;
	bool running;
	bool initialised;

	/* Guarded by lock. */
	char server[128];  /* configured */
	char active[128];  /* being polled, differs while failed over */
	/* Set by any setter to mean "re-read the settings, do not sleep". */
	bool config_dirty;
	bool enabled;
	bool burst_mode;
	bool fallback_active;
	bool synced;
	int64_t offset_ns;
	int64_t rtt_ns;
	uint64_t sampled_at_ns;

	/* Drift, published for display, and applied to the offset only when
	 * `drift_slope` is non-zero (burst policy with a valid fit). The slope
	 * is offset nanoseconds per local nanosecond; `drift_ppm` is the same
	 * number sign-flipped into the way crystal error is normally read,
	 * positive meaning the local clock runs fast. */
	bool drift_valid;
	double drift_ppm;
	double drift_slope;
	uint64_t drift_anchor_ns;
} ntp = {0};

/* ── Wire format ──────────────────────────────────────────── */

static uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* An NTP 64-bit timestamp is seconds since 1900 plus a 32-bit binary
 * fraction. Converted to nanoseconds since the Unix epoch. */
static int64_t ntp_ts_to_unix_ns(const uint8_t *p)
{
	uint64_t seconds = read_be32(p);
	uint64_t fraction = read_be32(p + 4);

	if (seconds < NTP_UNIX_EPOCH_DELTA)
		return 0;

	int64_t ns = (int64_t)(seconds - NTP_UNIX_EPOCH_DELTA) * 1000000000LL;
	/* fraction / 2^32 * 1e9, without overflowing: 1e9 < 2^30, so the
	 * product of a 32-bit fraction and 1e9 fits in 62 bits. */
	ns += (int64_t)((fraction * 1000000000ULL) >> 32);
	return ns;
}

/* ── One sample ───────────────────────────────────────────── */

struct ntp_sample {
	int64_t offset_ns;
	int64_t rtt_ns;
};

/* Why a poll ended the way it did. The two kiss-o'-death codes are the
 * server telling us our polling is unwelcome, which is a different thing from
 * not reaching it and calls for a different answer. */
enum ntp_result {
	NTP_RESULT_OK,
	NTP_RESULT_FAIL,
	NTP_RESULT_RATE, /* slow down */
	NTP_RESULT_DENY, /* go away */
};

static bool set_recv_timeout(irl_socket_t sock)
{
#ifdef _WIN32
	DWORD timeout = NTP_RECV_TIMEOUT_MS;
	return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
			  sizeof(timeout)) == 0;
#else
	struct timeval tv;
	tv.tv_sec = NTP_RECV_TIMEOUT_MS / 1000;
	tv.tv_usec = (NTP_RECV_TIMEOUT_MS % 1000) * 1000;
	return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

/* Exchange one packet with `addr`. The two local timestamps come from
 * os_gettime_ns() rather than the system clock, which is the whole point:
 * the result is an offset onto the same monotonic timebase every other part
 * of the plugin schedules against. */
static enum ntp_result ntp_query_once(const struct addrinfo *addr,
				      struct ntp_sample *out)
{
	irl_socket_t sock = socket(addr->ai_family, addr->ai_socktype,
				   addr->ai_protocol);
	if (sock == IRL_INVALID_SOCKET)
		return NTP_RESULT_FAIL;

	if (!set_recv_timeout(sock)) {
		irl_close_socket(sock);
		return NTP_RESULT_FAIL;
	}

	uint8_t packet[NTP_PACKET_BYTES] = {0};
	/* LI = 0 (no warning), VN = 4, Mode = 3 (client). */
	packet[0] = 0x23;

	uint64_t t1 = os_gettime_ns();
	int sent = (int)sendto(sock, (const char *)packet, sizeof(packet), 0,
			       addr->ai_addr, (int)addr->ai_addrlen);
	if (sent != (int)sizeof(packet)) {
		irl_close_socket(sock);
		return NTP_RESULT_FAIL;
	}

	uint8_t reply[NTP_PACKET_BYTES];
	int got = (int)recv(sock, (char *)reply, sizeof(reply), 0);
	uint64_t t4 = os_gettime_ns();
	irl_close_socket(sock);

	if (got != (int)sizeof(reply))
		return NTP_RESULT_FAIL;

	/* Mode 4 = server. */
	uint8_t mode = reply[0] & 0x07;
	uint8_t stratum = reply[1];
	if (mode != 4)
		return NTP_RESULT_FAIL;

	/* Stratum 0 is never a time source: it is a kiss-o'-death packet, and
	 * the reference identifier says which. RFC 4330 asks for the poll
	 * interval to be increased on RATE and for the server to be dropped
	 * on DENY or RSTR, which is the difference between a pool that is
	 * busy and one that has blocked us. */
	if (stratum == 0) {
		const char *code = (const char *)&reply[12];
		if (memcmp(code, "RATE", 4) == 0)
			return NTP_RESULT_RATE;
		if (memcmp(code, "DENY", 4) == 0 || memcmp(code, "RSTR", 4) == 0)
			return NTP_RESULT_DENY;
		return NTP_RESULT_FAIL;
	}

	if (stratum > 15)
		return NTP_RESULT_FAIL;

	int64_t t2 = ntp_ts_to_unix_ns(&reply[32]); /* server receive */
	int64_t t3 = ntp_ts_to_unix_ns(&reply[40]); /* server transmit */
	if (t2 <= 0 || t3 <= 0)
		return NTP_RESULT_FAIL;

	int64_t rtt = (int64_t)(t4 - t1) - (t3 - t2);
	if (rtt < 0)
		rtt = 0;

	/* Offset from our monotonic clock to UTC, averaging out the path so
	 * long as it is roughly symmetric. */
	out->offset_ns = ((t2 - (int64_t)t1) + (t3 - (int64_t)t4)) / 2;
	out->rtt_ns = rtt;
	return NTP_RESULT_OK;
}

/* ── Drift window ─────────────────────────────────────────── */

/* Owned by the poll thread alone: it is filled and fitted between the
 * publishes, and only the fit result crosses the lock. Nothing else may touch
 * it. */
static struct {
	struct {
		uint64_t t_ns;
		int64_t offset_ns;
		int64_t rtt_ns;
	} sample[NTP_DRIFT_CAPACITY];
	size_t count;

	/* Scratch for the fit. Static rather than automatic because the pair
	 * list is 2016 doubles at capacity and this runs on a thread whose
	 * stack size is not ours to assume. */
	double slopes[NTP_DRIFT_CAPACITY * (NTP_DRIFT_CAPACITY - 1) / 2];
	double residuals[NTP_DRIFT_CAPACITY];
	size_t keep[NTP_DRIFT_CAPACITY];
} drift;

struct drift_fit {
	bool valid;
	double slope;      /* offset ns per local ns */
	int64_t offset_ns; /* fitted offset at anchor_ns */
	uint64_t anchor_ns;
	size_t used;
};

/* Offsets are a property of the path as much as of the clock — two servers
 * disagree by their own asymmetry, which is the same order as the drift
 * signal itself — so the window cannot span a change of server. */
static void drift_window_reset(void)
{
	drift.count = 0;
}

static void drift_window_add(const struct ntp_sample *s, uint64_t now_ns)
{
	if (drift.count == NTP_DRIFT_CAPACITY) {
		memmove(&drift.sample[0], &drift.sample[1],
			sizeof(drift.sample[0]) * (NTP_DRIFT_CAPACITY - 1));
		drift.count--;
	}

	drift.sample[drift.count].t_ns = now_ns;
	drift.sample[drift.count].offset_ns = s->offset_ns;
	drift.sample[drift.count].rtt_ns = s->rtt_ns;
	drift.count++;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;
	return (x > y) - (x < y);
}

static double median_sorted(const double *v, size_t n)
{
	return (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* Theil–Sen: the median of the slopes of every pair. A least-squares line
 * through NTP samples follows whichever sample had the worst queueing;
 * the median of pairs ignores it, which matters on the paths this plugin
 * runs over. */
static void drift_fit_current(struct drift_fit *out)
{
	memset(out, 0, sizeof(*out));

	if (drift.count < NTP_DRIFT_MIN_SAMPLES)
		return;

	const uint64_t newest = drift.sample[drift.count - 1].t_ns;

	/* Age first, so the round-trip floor below is the best of what is
	 * still relevant rather than of a sample from an hour ago. */
	size_t k = 0;
	int64_t min_rtt = INT64_MAX;
	for (size_t i = 0; i < drift.count; i++) {
		if ((int64_t)(newest - drift.sample[i].t_ns) >
		    NTP_DRIFT_MAX_AGE_NS)
			continue;
		if (drift.sample[i].rtt_ns < min_rtt)
			min_rtt = drift.sample[i].rtt_ns;
		drift.keep[k++] = i;
	}
	if (k < NTP_DRIFT_MIN_SAMPLES)
		return;

	const int64_t rtt_limit = min_rtt + min_rtt / 2 + NTP_DRIFT_RTT_SLACK_NS;
	size_t f = 0;
	for (size_t j = 0; j < k; j++) {
		if (drift.sample[drift.keep[j]].rtt_ns <= rtt_limit)
			drift.keep[f++] = drift.keep[j];
	}
	if (f < NTP_DRIFT_MIN_SAMPLES)
		return;

	const uint64_t first = drift.sample[drift.keep[0]].t_ns;
	const uint64_t last = drift.sample[drift.keep[f - 1]].t_ns;
	if ((int64_t)(last - first) < NTP_DRIFT_MIN_SPAN_NS)
		return;

	size_t pairs = 0;
	for (size_t a = 0; a < f; a++) {
		const uint64_t ta = drift.sample[drift.keep[a]].t_ns;
		const int64_t oa = drift.sample[drift.keep[a]].offset_ns;
		for (size_t b = a + 1; b < f; b++) {
			const int64_t dt =
				(int64_t)(drift.sample[drift.keep[b]].t_ns - ta);
			if (dt < NTP_DRIFT_MIN_PAIR_NS)
				continue;
			const int64_t doff =
				drift.sample[drift.keep[b]].offset_ns - oa;
			drift.slopes[pairs++] = (double)doff / (double)dt;
		}
	}
	if (pairs < NTP_DRIFT_MIN_PAIRS)
		return;

	qsort(drift.slopes, pairs, sizeof(drift.slopes[0]), cmp_double);
	const double slope = median_sorted(drift.slopes, pairs);

	if (!isfinite(slope) || fabs(slope) * 1e6 > NTP_DRIFT_MAX_PPM)
		return;

	/* The intercept gets the same treatment: the median residual against
	 * the newest sample's time, so the published offset is a filtered
	 * value rather than whatever the last packet happened to measure. */
	for (size_t j = 0; j < f; j++) {
		const int64_t dt =
			(int64_t)(drift.sample[drift.keep[j]].t_ns - last);
		drift.residuals[j] =
			(double)drift.sample[drift.keep[j]].offset_ns -
			slope * (double)dt;
	}
	qsort(drift.residuals, f, sizeof(drift.residuals[0]), cmp_double);

	out->valid = true;
	out->slope = slope;
	out->offset_ns = (int64_t)median_sorted(drift.residuals, f);
	out->anchor_ns = last;
	out->used = f;
}

/* ── Publishing ───────────────────────────────────────────── */

/* Offset as of `now_ns`, including the drift extrapolation when one is
 * running. Call with the lock held. */
static int64_t offset_at_locked(uint64_t now_ns)
{
	int64_t offset = ntp.offset_ns;

	if (ntp.drift_slope != 0.0 && ntp.drift_anchor_ns &&
	    now_ns > ntp.drift_anchor_ns) {
		double correction =
			ntp.drift_slope *
			(double)(int64_t)(now_ns - ntp.drift_anchor_ns);
		if (correction > (double)NTP_DRIFT_MAX_CORRECTION_NS)
			correction = (double)NTP_DRIFT_MAX_CORRECTION_NS;
		else if (correction < -(double)NTP_DRIFT_MAX_CORRECTION_NS)
			correction = -(double)NTP_DRIFT_MAX_CORRECTION_NS;
		offset += (int64_t)correction;
	}

	return offset;
}

static void publish_sample(const struct ntp_sample *best, bool burst_mode)
{
	const uint64_t now = os_gettime_ns();

	drift_window_add(best, now);

	struct drift_fit fit;
	drift_fit_current(&fit);

	irl_mutex_lock(&ntp.lock);

	if (burst_mode && fit.valid) {
		/* The fit is already a filtered estimate over minutes of
		 * samples, so it replaces the exponential average rather than
		 * feeding it — smoothing a smoothed value only adds lag. */
		ntp.offset_ns = fit.offset_ns;
		ntp.drift_slope = fit.slope;
		ntp.drift_anchor_ns = fit.anchor_ns;
	} else {
		if (!ntp.synced || llabs(best->offset_ns - ntp.offset_ns) >
					   NTP_RESEAT_THRESHOLD_NS) {
			ntp.offset_ns = best->offset_ns;
		} else {
			double delta =
				(double)(best->offset_ns - ntp.offset_ns);
			ntp.offset_ns += (int64_t)(delta * NTP_OFFSET_SMOOTHING);
		}
		/* Measured but not applied: no extrapolation on this path. */
		ntp.drift_slope = 0.0;
		ntp.drift_anchor_ns = 0;
	}

	ntp.drift_valid = fit.valid;
	/* Sign flip: a local clock that runs slow makes the offset to UTC
	 * grow, and reads to everyone else as a negative ppm error. */
	ntp.drift_ppm = fit.valid ? -fit.slope * 1e6 : 0.0;

	bool first = !ntp.synced;
	ntp.synced = true;
	ntp.rtt_ns = best->rtt_ns;
	ntp.sampled_at_ns = now;
	int64_t published = ntp.offset_ns;
	irl_mutex_unlock(&ntp.lock);

	if (first) {
		blog(LOG_INFO,
		     "[irl-source] NTP synced: offset %+lldms, round trip %lldms",
		     (long long)(published / 1000000LL),
		     (long long)(best->rtt_ns / 1000000LL));
	}
}

static bool should_stop(void)
{
	irl_mutex_lock(&ntp.lock);
	bool stop = !ntp.running;
	irl_mutex_unlock(&ntp.lock);
	return stop;
}

static bool is_synced(void)
{
	irl_mutex_lock(&ntp.lock);
	bool synced = ntp.synced;
	irl_mutex_unlock(&ntp.lock);
	return synced;
}

/* Announce which server is being polled and under which policy. The drift
 * estimate belongs to the reference it was measured against, so it goes with
 * it. */
static void set_active(const char *host, bool burst_mode, bool fallback)
{
	irl_mutex_lock(&ntp.lock);
	snprintf(ntp.active, sizeof(ntp.active), "%s", host ? host : "");
	ntp.burst_mode = burst_mode;
	ntp.fallback_active = fallback;
	ntp.drift_valid = false;
	ntp.drift_ppm = 0.0;
	ntp.drift_slope = 0.0;
	ntp.drift_anchor_ns = 0;
	irl_mutex_unlock(&ntp.lock);
}

/* ── Poll ─────────────────────────────────────────────────── */

/* `samples` packets to `host`, keeping the one with the lowest round trip. */
static enum ntp_result ntp_poll(const char *host, int samples,
				struct ntp_sample *out)
{
	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	struct addrinfo *res = NULL;
	if (getaddrinfo(host, NTP_PORT, &hints, &res) != 0 || !res)
		return NTP_RESULT_FAIL;

	struct ntp_sample best = {0};
	bool have_best = false;
	enum ntp_result worst = NTP_RESULT_FAIL;

	for (int i = 0; i < samples; i++) {
		struct ntp_sample sample;

		/* Checked between samples so shutdown waits out at most one
		 * receive timeout rather than the whole burst. Without it,
		 * quitting OBS mid-poll would block the join for seconds. */
		if (should_stop())
			break;

		enum ntp_result r = ntp_query_once(res, &sample);
		if (r != NTP_RESULT_OK) {
			/* A kiss-o'-death answer is information; keep it to
			 * report if no sample survives the burst. */
			if (r != NTP_RESULT_FAIL)
				worst = r;
			continue;
		}
		if (sample.rtt_ns > NTP_MAX_USABLE_RTT_NS)
			continue;
		if (!have_best || sample.rtt_ns < best.rtt_ns) {
			best = sample;
			have_best = true;
		}
	}

	freeaddrinfo(res);

	if (have_best) {
		*out = best;
		return NTP_RESULT_OK;
	}
	return worst;
}

/* ── Poll loop ────────────────────────────────────────────── */

/* Thread-private scheduling state. */
struct poll_state {
	char primary[sizeof(ntp.server)];
	bool burst;
	bool fallback;
	bool acquire; /* next poll is the acquisition burst */
	bool denied;  /* server told us to stop, so we did */
	bool probe_primary; /* failed over, and the primary is worth retrying */
	int consec_fail;
	uint32_t pool_interval_ms;
	uint64_t pool_due_ns;
	uint64_t probe_due_ns;
};

/* xorshift32, seeded off the clock and this process's own address space, so
 * two OBS instances started by the same script do not draw the same
 * intervals. Only the poll thread calls it. */
static uint32_t ntp_rand_range(uint32_t lo, uint32_t hi)
{
	static uint32_t state;

	if (!state) {
		state = (uint32_t)(os_gettime_ns() >> 8) ^
			(uint32_t)(uintptr_t)&state;
		if (!state)
			state = 0x9e3779b9u;
	}

	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;

	return lo + state % (hi - lo + 1);
}

static uint32_t burst_interval_ms(void)
{
	return ntp_rand_range(NTP_BURST_MIN_INTERVAL_MS,
			      NTP_BURST_MAX_INTERVAL_MS);
}

/* `probe` is false when the primary refused service rather than went missing:
 * the clock still moves to the pool, but a server that said go away is not
 * poked every half minute to see whether it meant it. */
static void begin_fallback(struct poll_state *st, bool probe)
{
	st->fallback = true;
	st->probe_primary = probe;
	st->pool_interval_ms = NTP_POLL_INTERVAL_MS;
	/* The pool is polled at once, to get the clock back on something
	 * live; the server that just failed is left alone until the first
	 * probe is due. */
	st->pool_due_ns = 0;
	st->probe_due_ns =
		probe ? os_gettime_ns() +
				(uint64_t)ntp_rand_range(
					NTP_FALLBACK_PROBE_MIN_MS,
					NTP_FALLBACK_PROBE_MAX_MS) *
					1000000ULL
		      : UINT64_MAX;
	drift_window_reset();
	/* The offset itself is kept: it is a few milliseconds stale at worst,
	 * and dropping the sync would stall every source over an outage the
	 * pool is about to paper over. The pool's own samples ease it across. */
	set_active(NTP_FALLBACK_SERVER, false, true);
}

static void end_fallback(struct poll_state *st)
{
	blog(LOG_INFO, "[irl-source] NTP server '%s' is back; resuming",
	     st->primary);

	st->fallback = false;
	st->consec_fail = 0;
	st->acquire = true;
	drift_window_reset();
	set_active(st->primary, st->burst, false);
}

/* Returns how long to wait before the next iteration. */
static uint32_t poll_primary(struct poll_state *st)
{
	const int samples = (st->acquire && st->burst) ? NTP_BURST : 1;
	struct ntp_sample best;
	enum ntp_result r = ntp_poll(st->primary, samples, &best);

	if (r == NTP_RESULT_OK) {
		publish_sample(&best, st->burst);
		st->acquire = false;
		st->consec_fail = 0;
		return st->burst ? burst_interval_ms() : st->pool_interval_ms;
	}

	if (r == NTP_RESULT_RATE) {
		/* Being asked to slow down is not a failure, so it does not
		 * count toward the failover — the server is answering. */
		if (st->pool_interval_ms < NTP_POLL_MAX_INTERVAL_MS)
			st->pool_interval_ms *= 2;
		blog(LOG_WARNING,
		     "[irl-source] NTP server '%s' asked for a slower poll; backing off to %us",
		     st->primary, st->pool_interval_ms / 1000);
		return st->pool_interval_ms;
	}

	if (r == NTP_RESULT_DENY) {
		/* Ignoring this is how a client gets an address blocked, so
		 * this server is not asked again. If there is somewhere else
		 * to go, go there; otherwise the last offset stays and ages,
		 * which the dock reports. */
		if (irl_strcasecmp(st->primary, NTP_FALLBACK_SERVER) != 0) {
			blog(LOG_WARNING,
			     "[irl-source] NTP server '%s' refused service; using '%s' instead",
			     st->primary, NTP_FALLBACK_SERVER);
			begin_fallback(st, false);
			return 0;
		}
		st->denied = true;
		blog(LOG_WARNING,
		     "[irl-source] NTP server '%s' refused service; polling stopped. Set a different server in the IRL Sync dock",
		     st->primary);
		return NTP_IDLE_INTERVAL_MS;
	}

	st->consec_fail++;

	if (st->burst && st->consec_fail >= NTP_FALLBACK_AFTER_FAILURES &&
	    irl_strcasecmp(st->primary, NTP_FALLBACK_SERVER) != 0) {
		blog(LOG_WARNING,
		     "[irl-source] NTP server '%s' unreachable; falling back to '%s'",
		     st->primary, NTP_FALLBACK_SERVER);
		begin_fallback(st, true);
		return 0; /* poll the pool immediately */
	}

	/* Once per outage rather than once per attempt: at the burst policy's
	 * retry gap this line would otherwise fill the log. */
	if (st->consec_fail == 1) {
		blog(LOG_WARNING,
		     "[irl-source] NTP poll of '%s' failed; timecode sync stays unsynced until it succeeds",
		     st->primary);
	}

	if (st->burst)
		return NTP_BURST_RETRY_MS;
	return is_synced() ? st->pool_interval_ms : NTP_RETRY_INTERVAL_MS;
}

/* Failed over: probe the configured server on its own schedule, and keep the
 * clock alive off the pool on its. */
static uint32_t poll_fallback(struct poll_state *st)
{
	uint64_t now = os_gettime_ns();
	struct ntp_sample best;

	if (st->probe_primary && now >= st->probe_due_ns) {
		if (ntp_poll(st->primary, 1, &best) == NTP_RESULT_OK) {
			end_fallback(st);
			/* Seeds the window the reset above emptied; the
			 * acquisition burst follows a second later. */
			publish_sample(&best, st->burst);
			return NTP_BURST_MIN_INTERVAL_MS;
		}
		st->probe_due_ns =
			os_gettime_ns() +
			(uint64_t)ntp_rand_range(NTP_FALLBACK_PROBE_MIN_MS,
						 NTP_FALLBACK_PROBE_MAX_MS) *
				1000000ULL;
	}

	now = os_gettime_ns();
	if (now >= st->pool_due_ns) {
		enum ntp_result r = ntp_poll(NTP_FALLBACK_SERVER, 1, &best);
		uint32_t next;

		if (r == NTP_RESULT_OK) {
			publish_sample(&best, false);
			next = st->pool_interval_ms;
		} else if (r == NTP_RESULT_RATE) {
			if (st->pool_interval_ms < NTP_POLL_MAX_INTERVAL_MS)
				st->pool_interval_ms *= 2;
			next = st->pool_interval_ms;
		} else if (r == NTP_RESULT_DENY) {
			/* Both servers are refusing. Nothing left to poll, so
			 * stop asking; the probe still runs the primary. */
			next = NTP_POLL_MAX_INTERVAL_MS;
		} else {
			next = is_synced() ? st->pool_interval_ms
					   : NTP_RETRY_INTERVAL_MS;
		}

		st->pool_due_ns = os_gettime_ns() + (uint64_t)next * 1000000ULL;
	}

	/* Wake for whichever of the two is due first. */
	now = os_gettime_ns();
	uint64_t next_ns = st->probe_due_ns < st->pool_due_ns ? st->probe_due_ns
							      : st->pool_due_ns;
	if (next_ns <= now)
		return 1;
	uint64_t wait_ms = (next_ns - now) / 1000000ULL;
	if (wait_ms < 1)
		return 1;
	return wait_ms > NTP_IDLE_INTERVAL_MS ? NTP_IDLE_INTERVAL_MS
					      : (uint32_t)wait_ms;
}

static void *ntp_thread(void *unused)
{
	UNUSED_PARAMETER(unused);

	struct poll_state st;
	memset(&st, 0, sizeof(st));
	st.pool_interval_ms = NTP_POLL_INTERVAL_MS;

	for (;;) {
		char configured[sizeof(ntp.server)];
		bool enabled;

		irl_mutex_lock(&ntp.lock);
		if (!ntp.running) {
			irl_mutex_unlock(&ntp.lock);
			break;
		}
		ntp.config_dirty = false;
		enabled = ntp.enabled;
		memcpy(configured, ntp.server, sizeof(configured));
		irl_mutex_unlock(&ntp.lock);

		if (strcmp(configured, st.primary) != 0) {
			memcpy(st.primary, configured, sizeof(st.primary));
			st.burst = host_is_burst(st.primary);
			st.fallback = false;
			st.probe_primary = false;
			st.acquire = true;
			st.denied = false;
			st.consec_fail = 0;
			st.pool_interval_ms = NTP_POLL_INTERVAL_MS;
			st.pool_due_ns = 0;
			st.probe_due_ns = 0;
			drift_window_reset();
			set_active(st.primary, st.burst, false);

			if (st.primary[0]) {
				blog(LOG_INFO,
				     "[irl-source] NTP server '%s': %s polling",
				     st.primary,
				     st.burst ? "burst" : "standard");
			}
		}

		uint32_t wait_ms;

		if (!enabled || !st.primary[0] || st.denied) {
			/* Nothing to do: sync is off, no server is set, or the
			 * server has refused us. Sit on the condvar until a
			 * setter says otherwise. No packets leave the machine
			 * in this state. */
			wait_ms = NTP_IDLE_INTERVAL_MS;
		} else if (st.fallback) {
			wait_ms = poll_fallback(&st);
		} else {
			wait_ms = poll_primary(&st);
		}

		/* Woken early by a settings change or by shutdown. The
		 * predicate re-check handles both, and a spurious wakeup just
		 * re-reads the settings. */
		irl_mutex_lock(&ntp.lock);
		if (ntp.running && !ntp.config_dirty && wait_ms)
			irl_cond_timedwait(&ntp.wake, &ntp.lock, wait_ms);
		irl_mutex_unlock(&ntp.lock);
	}

	return NULL;
}

/* ── Public API ───────────────────────────────────────────── */

void irl_ntp_start(void)
{
	if (ntp.running)
		return;

	if (!ntp.initialised) {
		irl_mutex_init(&ntp.lock);
		irl_cond_init(&ntp.wake);
		ntp.initialised = true;
	}

#ifdef _WIN32
	/* libobs and libsrt both start Winsock already, but the count is
	 * per-caller and this module owns its own sockets. */
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	ntp.running = true;
	if (irl_thread_create(&ntp.thread, ntp_thread, NULL) != 0) {
		ntp.running = false;
		blog(LOG_ERROR,
		     "[irl-source] Failed to start NTP thread; timecode sync unavailable");
	}
}

void irl_ntp_stop(void)
{
	if (!ntp.running)
		return;

	irl_mutex_lock(&ntp.lock);
	ntp.running = false;
	irl_cond_broadcast(&ntp.wake);
	irl_mutex_unlock(&ntp.lock);

	irl_thread_join(&ntp.thread);

#ifdef _WIN32
	WSACleanup();
#endif
}

void irl_ntp_set_server(const char *host)
{
	if (!ntp.initialised)
		return;

	irl_mutex_lock(&ntp.lock);
	if (host && *host) {
		if (strcmp(ntp.server, host) != 0) {
			snprintf(ntp.server, sizeof(ntp.server), "%s", host);
			/* The old offset belongs to the old reference. */
			ntp.synced = false;
			ntp.config_dirty = true;
			irl_cond_broadcast(&ntp.wake);
		}
	} else if (ntp.server[0]) {
		ntp.server[0] = '\0';
		ntp.synced = false;
		ntp.config_dirty = true;
		irl_cond_broadcast(&ntp.wake);
	}
	irl_mutex_unlock(&ntp.lock);
}

void irl_ntp_set_enabled(bool enabled)
{
	if (!ntp.initialised)
		return;

	irl_mutex_lock(&ntp.lock);
	bool changed = ntp.enabled != enabled;
	if (changed) {
		ntp.enabled = enabled;
		ntp.config_dirty = true;
		irl_cond_broadcast(&ntp.wake);
	}
	irl_mutex_unlock(&ntp.lock);

	if (changed) {
		blog(LOG_INFO, "[irl-source] NTP polling %s",
		     enabled ? "started" : "stopped");
	}
}

void irl_ntp_get_status(struct irl_ntp_status *out)
{
	memset(out, 0, sizeof(*out));
	if (!ntp.initialised)
		return;

	const uint64_t now = os_gettime_ns();

	irl_mutex_lock(&ntp.lock);
	out->synced = ntp.synced;
	out->offset_ns = offset_at_locked(now);
	out->rtt_ns = ntp.rtt_ns;
	out->age_ns = ntp.sampled_at_ns ? now - ntp.sampled_at_ns : 0;
	out->burst_mode = ntp.burst_mode;
	out->fallback_active = ntp.fallback_active;
	out->drift_valid = ntp.drift_valid;
	out->drift_ppm = ntp.drift_ppm;
	snprintf(out->server, sizeof(out->server), "%s",
		 ntp.active[0] ? ntp.active : ntp.server);
	snprintf(out->primary, sizeof(out->primary), "%s", ntp.server);
	irl_mutex_unlock(&ntp.lock);
}

bool irl_ntp_utc_now_ns(int64_t *utc_ns)
{
	if (!ntp.initialised)
		return false;

	const uint64_t now = os_gettime_ns();

	irl_mutex_lock(&ntp.lock);
	bool synced = ntp.synced;
	int64_t offset = offset_at_locked(now);
	irl_mutex_unlock(&ntp.lock);

	if (!synced)
		return false;

	*utc_ns = (int64_t)now + offset;
	return true;
}
