/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ntp-client.c — SNTP (RFC 4330) client for timecode sync.
 *
 * Deliberately small: one UDP round trip, four timestamps, the classic
 * offset/delay pair. No stepping of anything system-wide — the result is an
 * offset from os_gettime_ns() to UTC, kept in this module and used only to
 * place received frames on a shared timeline.
 *
 * The offset is what sync depends on, so it is filtered rather than taken
 * raw. Each poll sends a short burst and keeps the sample with the lowest
 * round trip (the least queueing in either direction, and so the least
 * asymmetry error), then eases the published offset toward it. Cellular and
 * Wi-Fi round trips vary by hundreds of milliseconds; the minimum of a burst
 * is far closer to the truth than any single sample.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET irl_socket_t;
#define IRL_INVALID_SOCKET INVALID_SOCKET
#define irl_close_socket closesocket
#else
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int irl_socket_t;
#define IRL_INVALID_SOCKET (-1)
#define irl_close_socket close
#endif

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "../include/irl-ntp.h"
#include "../include/irl-threading.h"

/* Seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (Unix epoch). */
#define NTP_UNIX_EPOCH_DELTA 2208988800ULL

#define NTP_PORT "123"
#define NTP_PACKET_BYTES 48
#define NTP_RECV_TIMEOUT_MS 1200
/* Samples per poll; the lowest-RTT one wins. Four is enough to skip a single
 * queued packet without turning a background task into traffic. */
#define NTP_BURST 4
#define NTP_POLL_INTERVAL_MS 64000
/* Retry faster until a first sync exists, so a source starting alongside OBS
 * does not sit unsynced for a minute. */
#define NTP_RETRY_INTERVAL_MS 8000
/* Backstop for the idle thread while polling is switched off. The real wakeup
 * is the condvar, which every setter signals; this only bounds the damage if a
 * signal is ever missed. */
#define NTP_IDLE_INTERVAL_MS 3600000
/* A round trip worse than this says nothing useful about the offset. */
#define NTP_MAX_USABLE_RTT_NS 1000000000LL
/* Weight applied to each accepted sample once a sync exists. Slow enough
 * that one bad measurement cannot move the timeline, fast enough to track
 * real crystal drift (tens of ppm) between polls. */
#define NTP_OFFSET_SMOOTHING 0.25
/* Past this the estimate is stale enough to be a different problem, so re-seat
 * rather than easing toward the new value. */
#define NTP_RESEAT_THRESHOLD_NS 2000000000LL

/* ── State ────────────────────────────────────────────────── */

static struct {
	irl_mutex_t lock;
	irl_cond_t wake;
	irl_thread_t thread;
	bool running;
	bool initialised;

	/* Guarded by lock. */
	char server[128];
	/* Set by any setter to mean "re-read the settings, do not sleep". */
	bool config_dirty;
	bool enabled;
	bool synced;
	int64_t offset_ns;
	int64_t rtt_ns;
	uint64_t sampled_at_ns;
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
static bool ntp_query_once(const struct addrinfo *addr, struct ntp_sample *out)
{
	irl_socket_t sock = socket(addr->ai_family, addr->ai_socktype,
				   addr->ai_protocol);
	if (sock == IRL_INVALID_SOCKET)
		return false;

	if (!set_recv_timeout(sock)) {
		irl_close_socket(sock);
		return false;
	}

	uint8_t packet[NTP_PACKET_BYTES] = {0};
	/* LI = 0 (no warning), VN = 4, Mode = 3 (client). */
	packet[0] = 0x23;

	uint64_t t1 = os_gettime_ns();
	int sent = (int)sendto(sock, (const char *)packet, sizeof(packet), 0,
			       addr->ai_addr, (int)addr->ai_addrlen);
	if (sent != (int)sizeof(packet)) {
		irl_close_socket(sock);
		return false;
	}

	uint8_t reply[NTP_PACKET_BYTES];
	int got = (int)recv(sock, (char *)reply, sizeof(reply), 0);
	uint64_t t4 = os_gettime_ns();
	irl_close_socket(sock);

	if (got != (int)sizeof(reply))
		return false;

	/* Mode 4 = server. Stratum 0 is a kiss-o'-death packet, never a time
	 * source. */
	uint8_t mode = reply[0] & 0x07;
	uint8_t stratum = reply[1];
	if (mode != 4 || stratum == 0 || stratum > 15)
		return false;

	int64_t t2 = ntp_ts_to_unix_ns(&reply[32]); /* server receive */
	int64_t t3 = ntp_ts_to_unix_ns(&reply[40]); /* server transmit */
	if (t2 <= 0 || t3 <= 0)
		return false;

	int64_t rtt = (int64_t)(t4 - t1) - (t3 - t2);
	if (rtt < 0)
		rtt = 0;

	/* Offset from our monotonic clock to UTC, averaging out the path so
	 * long as it is roughly symmetric. */
	out->offset_ns = ((t2 - (int64_t)t1) + (t3 - (int64_t)t4)) / 2;
	out->rtt_ns = rtt;
	return true;
}

/* ── Poll ─────────────────────────────────────────────────── */

static void publish_sample(const struct ntp_sample *best)
{
	irl_mutex_lock(&ntp.lock);

	if (!ntp.synced ||
	    llabs(best->offset_ns - ntp.offset_ns) > NTP_RESEAT_THRESHOLD_NS) {
		ntp.offset_ns = best->offset_ns;
	} else {
		double delta = (double)(best->offset_ns - ntp.offset_ns);
		ntp.offset_ns += (int64_t)(delta * NTP_OFFSET_SMOOTHING);
	}

	bool first = !ntp.synced;
	ntp.synced = true;
	ntp.rtt_ns = best->rtt_ns;
	ntp.sampled_at_ns = os_gettime_ns();
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

/* Returns true when a usable sample was published. */
static bool ntp_poll(const char *host)
{
	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	struct addrinfo *res = NULL;
	if (getaddrinfo(host, NTP_PORT, &hints, &res) != 0 || !res)
		return false;

	struct ntp_sample best = {0};
	bool have_best = false;

	for (int i = 0; i < NTP_BURST; i++) {
		struct ntp_sample sample;

		/* Checked between samples so shutdown waits out at most one
		 * receive timeout rather than the whole burst. Without it,
		 * quitting OBS mid-poll would block the join for seconds. */
		if (should_stop())
			break;

		if (!ntp_query_once(res, &sample))
			continue;
		if (sample.rtt_ns > NTP_MAX_USABLE_RTT_NS)
			continue;
		if (!have_best || sample.rtt_ns < best.rtt_ns) {
			best = sample;
			have_best = true;
		}
	}

	freeaddrinfo(res);

	if (have_best)
		publish_sample(&best);
	return have_best;
}

static void *ntp_thread(void *unused)
{
	UNUSED_PARAMETER(unused);

	for (;;) {
		char host[sizeof(ntp.server)];
		bool enabled;

		irl_mutex_lock(&ntp.lock);
		if (!ntp.running) {
			irl_mutex_unlock(&ntp.lock);
			break;
		}
		ntp.config_dirty = false;
		enabled = ntp.enabled;
		memcpy(host, ntp.server, sizeof(host));
		irl_mutex_unlock(&ntp.lock);

		uint32_t wait_ms;

		if (!enabled || !host[0]) {
			/* Nothing to do: sync is off, or no server is set. Sit
			 * on the condvar until a setter says otherwise. No
			 * packets leave the machine in this state. */
			wait_ms = NTP_IDLE_INTERVAL_MS;
		} else {
			bool ok = ntp_poll(host);
			if (!ok)
				blog(LOG_WARNING,
				     "[irl-source] NTP poll of '%s' failed; timecode sync stays unsynced until it succeeds",
				     host);
			wait_ms = ok ? NTP_POLL_INTERVAL_MS
				     : NTP_RETRY_INTERVAL_MS;
		}

		/* Woken early by a settings change or by shutdown. The
		 * predicate re-check handles both, and a spurious wakeup just
		 * re-reads the settings. */
		irl_mutex_lock(&ntp.lock);
		if (ntp.running && !ntp.config_dirty)
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

	irl_mutex_lock(&ntp.lock);
	out->synced = ntp.synced;
	out->offset_ns = ntp.offset_ns;
	out->rtt_ns = ntp.rtt_ns;
	out->age_ns = ntp.sampled_at_ns ? os_gettime_ns() - ntp.sampled_at_ns
					: 0;
	snprintf(out->server, sizeof(out->server), "%s", ntp.server);
	irl_mutex_unlock(&ntp.lock);
}

bool irl_ntp_utc_now_ns(int64_t *utc_ns)
{
	if (!ntp.initialised)
		return false;

	irl_mutex_lock(&ntp.lock);
	bool synced = ntp.synced;
	int64_t offset = ntp.offset_ns;
	irl_mutex_unlock(&ntp.lock);

	if (!synced)
		return false;

	*utc_ns = (int64_t)os_gettime_ns() + offset;
	return true;
}
