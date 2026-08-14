/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * websocket-vendor.c — obs-websocket vendor extension
 *
 * Exposes the per-source stats over obs-websocket so an overlay, a bot or
 * an IRL dashboard can read them from another machine, without the Lua or
 * Python script that the proc_handler path requires.
 *
 * Clients reach these through obs-websocket's own CallVendorRequest:
 *
 *   {"vendorName": "obs-irl-source", "requestType": "GetStats",
 *    "requestData": {"source_name": "IRL Source"}}
 *
 * The stats themselves are not read out of struct irl_source here. This
 * file calls the source's existing "get_stats" proc_handler, the same
 * entry point scripts use, so both transports are guaranteed to report the
 * same numbers, and the audio_state_lock snapshot stays in one place
 * (irl-source.c). obs-websocket runs request callbacks on its own thread,
 * and holding a source reference across the call keeps the source alive
 * for its duration.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>

#include "../include/irl-ntp.h"
#include "../include/irl-source.h"
#include "../include/irl-sync.h"
#include "../third_party/obs-websocket-api.h"

#define IRL_VENDOR_NAME "obs-irl-source"

/* Bumped when a request is added or a response field changes meaning, so a
 * client can feature-detect instead of probing.
 *
 * 2: adds GetSyncStatus and the sync_* fields on GetStats. */
#define IRL_VENDOR_API_VERSION 2

#define IRL_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static obs_websocket_vendor irl_vendor = NULL;

/* ── Stats field table ────────────────────────────────────── */

enum irl_stat_type {
	IRL_STAT_INT,
	IRL_STAT_FLOAT,
	IRL_STAT_BOOL,
	IRL_STAT_STRING,
};

struct irl_stat_field {
	const char *name;
	enum irl_stat_type type;
};

/* calldata is a typed blob that cannot be enumerated, so the fields to copy
 * out have to be named. Keep this in sync with the get_stats declaration in
 * irl_source_create() (irl-source.c) and the stats table in README.md — the
 * names here are the JSON keys clients see. */
static const struct irl_stat_field irl_stat_fields[] = {
	{"buffer_fill_ms", IRL_STAT_INT},
	{"current_speed", IRL_STAT_FLOAT},
	{"adaptive_latency_control", IRL_STAT_BOOL},
	{"reconnecting", IRL_STAT_BOOL},
	{"total_audio_frames", IRL_STAT_INT},
	{"total_video_frames", IRL_STAT_INT},
	{"pts_repairs", IRL_STAT_INT},
	{"pts_normalizations", IRL_STAT_INT},
	{"pts_interpolations", IRL_STAT_INT},
	{"pts_resets", IRL_STAT_INT},
	{"pts_last_gap_ms", IRL_STAT_INT},
	{"pts_max_gap_ms", IRL_STAT_INT},
	{"silence_insertions", IRL_STAT_INT},
	{"audio_underruns", IRL_STAT_INT},
	{"audio_resync_skipped_chunks", IRL_STAT_INT},
	{"audio_hidden_trimmed_chunks", IRL_STAT_INT},
	{"audio_quality_events", IRL_STAT_INT},
	{"audio_output_restarts", IRL_STAT_INT},
	{"obs_lead_ms", IRL_STAT_INT},
	{"audio_decoder_flushes", IRL_STAT_INT},
	{"video_decoder_flushes", IRL_STAT_INT},
	{"video_lead_ms", IRL_STAT_INT},
	{"video_lead_excess", IRL_STAT_INT},
	{"stream_delay_ms", IRL_STAT_INT},
	{"low_latency_audio", IRL_STAT_BOOL},
	{"reconnect_count", IRL_STAT_INT},
	{"sync_enabled", IRL_STAT_BOOL},
	{"sync_status", IRL_STAT_STRING},
	{"sync_timecode", IRL_STAT_STRING},
	{"sync_latency_ms", IRL_STAT_INT},
	{"sync_latency_peak_ms", IRL_STAT_INT},
	{"sync_added_ms", IRL_STAT_INT},
	{"sync_error_ms", IRL_STAT_INT},
	{"sync_required_offset_ms", IRL_STAT_INT},
};

static void stats_to_obs_data(const calldata_t *cd, obs_data_t *out)
{
	for (size_t i = 0; i < IRL_ARRAY_SIZE(irl_stat_fields); i++) {
		const struct irl_stat_field *f = &irl_stat_fields[i];

		switch (f->type) {
		case IRL_STAT_INT: {
			long long v = 0;
			calldata_get_int(cd, f->name, &v);
			obs_data_set_int(out, f->name, v);
			break;
		}
		case IRL_STAT_FLOAT: {
			double v = 0.0;
			calldata_get_float(cd, f->name, &v);
			obs_data_set_double(out, f->name, v);
			break;
		}
		case IRL_STAT_BOOL: {
			bool v = false;
			calldata_get_bool(cd, f->name, &v);
			obs_data_set_bool(out, f->name, v);
			break;
		}
		case IRL_STAT_STRING: {
			const char *v = NULL;
			calldata_get_string(cd, f->name, &v);
			obs_data_set_string(out, f->name, v ? v : "");
			break;
		}
		}
	}
}

/* ── Response helpers ─────────────────────────────────────── */

/* Vendor requests have no status code of their own: obs-websocket reports
 * RequestStatus::Success as long as the callback ran, and hands the client
 * whatever the callback put in response_data. So the outcome is carried in
 * the payload. Every response has "success"; failures add "error". */
static void respond_error(obs_data_t *response_data, const char *message)
{
	obs_data_set_bool(response_data, "success", false);
	obs_data_set_string(response_data, "error", message);
}

/* ── Source lookup ────────────────────────────────────────── */

static bool source_is_irl(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && strcmp(id, IRL_SOURCE_ID) == 0;
}

struct irl_source_search {
	obs_source_t *first; /* strong reference, released by the caller */
	int count;
};

static bool enum_irl_sources(void *param, obs_source_t *source)
{
	struct irl_source_search *search = param;

	if (!source_is_irl(source))
		return true;

	search->count++;
	if (!search->first)
		/* Not obs_source_get_ref()'s only job here: it returns NULL
		 * for a source already on its way out, which is exactly the
		 * one we must not hand back. */
		search->first = obs_source_get_ref(source);

	return true;
}

/* Resolves which source the request is about. "source_name" names one
 * explicitly; without it, a scene collection holding exactly one IRL source
 * resolves to that source, because that is the common setup and it saves
 * every client a GetSourceList round trip first.
 *
 * Returns a strong reference the caller must release, or NULL after writing
 * the failure into response_data. */
static obs_source_t *resolve_source(obs_data_t *request_data,
				    obs_data_t *response_data)
{
	/* Accept the obs-websocket house style as an alias: core requests all
	 * take "sourceName", so that is what a client reaches for first. */
	const char *name = obs_data_get_string(request_data, "source_name");
	if (!name || !*name)
		name = obs_data_get_string(request_data, "sourceName");

	if (name && *name) {
		obs_source_t *source = obs_get_source_by_name(name);
		if (!source) {
			respond_error(response_data, "No source by that name");
			return NULL;
		}
		if (!source_is_irl(source)) {
			obs_source_release(source);
			respond_error(response_data,
				      "That source is not an IRL Source");
			return NULL;
		}
		return source;
	}

	struct irl_source_search search = {0};
	obs_enum_sources(enum_irl_sources, &search);

	if (search.count == 0) {
		respond_error(response_data, "No IRL Source exists");
		return NULL;
	}
	if (search.count > 1) {
		if (search.first)
			obs_source_release(search.first);
		respond_error(
			response_data,
			"More than one IRL Source exists; pass source_name (see GetSourceList)");
		return NULL;
	}
	if (!search.first) {
		respond_error(response_data, "IRL Source is being destroyed");
		return NULL;
	}

	return search.first;
}

/* ── Requests ─────────────────────────────────────────────── */

static void vendor_get_stats(obs_data_t *request_data,
			     obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);

	obs_source_t *source = resolve_source(request_data, response_data);
	if (!source)
		return;

	calldata_t cd;
	calldata_init(&cd);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	bool called = ph && proc_handler_call(ph, "get_stats", &cd);

	if (called) {
		obs_data_set_string(response_data, "source_name",
				    obs_source_get_name(source));
		stats_to_obs_data(&cd, response_data);
		obs_data_set_bool(response_data, "success", true);
	} else {
		respond_error(response_data, "Source did not answer get_stats");
	}

	calldata_free(&cd);
	obs_source_release(source);
}

static bool enum_source_list(void *param, obs_source_t *source)
{
	obs_data_array_t *array = param;

	if (!source_is_irl(source))
		return true;

	obs_data_t *entry = obs_data_create();
	obs_data_set_string(entry, "source_name", obs_source_get_name(source));
	/* Deliberately no URL: it can carry an SRT passphrase or a stream key,
	 * and this list is readable by every connected websocket client. */
	obs_data_set_bool(entry, "active", obs_source_active(source));
	obs_data_set_bool(entry, "showing", obs_source_showing(source));
	obs_data_array_push_back(array, entry);
	obs_data_release(entry);

	return true;
}

static void vendor_get_source_list(obs_data_t *request_data,
				   obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	obs_data_array_t *array = obs_data_array_create();
	obs_enum_sources(enum_source_list, array);

	obs_data_set_array(response_data, "sources", array);
	obs_data_array_release(array);
	obs_data_set_bool(response_data, "success", true);
}

/* The whole sync picture in one call: what the group is configured to do,
 * whether this machine has a clock worth trusting, and every source's status.
 *
 * This is the alerting path. A dock only helps when someone is looking at it,
 * and during a live IRL show the operator may be nowhere near the machine —
 * whereas the person who can actually fix an out-of-sync feed is the one
 * holding the phone, and chat is how you reach them. A bot polling this can
 * say "chase cam out of sync, needs 7.4s" where it will be seen. */
static void vendor_get_sync_status(obs_data_t *request_data,
				   obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	struct irl_sync_config cfg;
	irl_sync_config_get(&cfg);

	obs_data_set_bool(response_data, "sync_enabled", cfg.enabled);
	obs_data_set_int(response_data, "offset_ms", cfg.offset_ms);

	struct irl_ntp_status ntp;
	irl_ntp_get_status(&ntp);

	obs_data_t *clock = obs_data_create();
	obs_data_set_bool(clock, "synced", ntp.synced);
	obs_data_set_string(clock, "server", ntp.server);
	obs_data_set_int(clock, "offset_ms", ntp.offset_ns / 1000000LL);
	obs_data_set_int(clock, "rtt_ms", ntp.rtt_ns / 1000000LL);
	obs_data_set_int(clock, "age_ms", (long long)(ntp.age_ns / 1000000ULL));

	int64_t utc_ns = 0;
	if (irl_ntp_utc_now_ns(&utc_ns))
		obs_data_set_int(clock, "utc_ms", utc_ns / 1000000LL);
	obs_data_set_obj(response_data, "clock", clock);
	obs_data_release(clock);

	struct irl_sync_entry entries[IRL_SYNC_MAX_SOURCES];
	size_t count = irl_sync_collect(entries, IRL_ARRAY_SIZE(entries));

	obs_data_array_t *array = obs_data_array_create();
	int64_t worst_required_ms = 0;
	int out_of_sync = 0;

	for (size_t i = 0; i < count; i++) {
		const struct irl_sync_entry *e = &entries[i];
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, "source_name", e->source_name);
		obs_data_set_bool(item, "sync_enabled", e->sync_enabled);
		obs_data_set_string(item, "status",
				    irl_sync_status_name(e->snap.status));
		/* Which of the three causes of "no timecode" this is, so a bot
		 * can say something actionable instead of just "not synced". */
		obs_data_set_string(item, "timecode_reason",
				    irl_sync_tc_reason_name(
					    e->snap.tc_reason));
		obs_data_set_int(item, "latency_ms", e->snap.latency_ms);
		obs_data_set_int(item, "latency_peak_ms",
				 e->snap.latency_peak_ms);
		obs_data_set_int(item, "added_ms", e->snap.added_ms);
		obs_data_set_int(item, "error_ms", e->snap.error_ms);
		obs_data_set_int(item, "required_offset_ms",
				 e->snap.required_offset_ms);

		if (e->snap.have_timecode) {
			char tc[24];
			snprintf(tc, sizeof(tc), "%02u:%02u:%02u:%02u",
				 e->snap.tc.hours, e->snap.tc.minutes,
				 e->snap.tc.seconds,
				 (unsigned)e->snap.tc.n_frames);
			obs_data_set_string(item, "timecode", tc);
		} else {
			obs_data_set_string(item, "timecode", "");
		}

		if (e->sync_enabled &&
		    e->snap.required_offset_ms > worst_required_ms)
			worst_required_ms = e->snap.required_offset_ms;
		if (e->snap.status == IRL_SYNC_TOO_SLOW ||
		    e->snap.status == IRL_SYNC_STALE)
			out_of_sync++;

		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	obs_data_set_array(response_data, "sources", array);
	obs_data_array_release(array);

	/* Pre-computed so a bot does not have to reimplement the policy: the
	 * count worth alerting on, and the offset that would clear it. */
	obs_data_set_int(response_data, "out_of_sync_count", out_of_sync);
	obs_data_set_int(response_data, "recommended_offset_ms",
			 worst_required_ms);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_version(obs_data_t *request_data,
			       obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	obs_data_set_string(response_data, "plugin_version",
			    OBS_IRL_SOURCE_VERSION);
	obs_data_set_int(response_data, "vendor_api_version",
			 IRL_VENDOR_API_VERSION);
	obs_data_set_int(response_data, "obs_websocket_api_version",
			 (long long)obs_websocket_get_api_version());
	obs_data_set_bool(response_data, "success", true);
}

/* ── Registration ─────────────────────────────────────────── */

static const struct {
	const char *type;
	obs_websocket_request_callback_function callback;
} irl_vendor_requests[] = {
	{"GetStats", vendor_get_stats},
	{"GetSourceList", vendor_get_source_list},
	{"GetSyncStatus", vendor_get_sync_status},
	{"GetVersion", vendor_get_version},
};

/*
 * Must run from obs_module_post_load(): obs-websocket publishes the global
 * proc this goes through from its own obs_module_load(), and module load
 * order between plugins is not defined. Every module's load has finished by
 * the time any post_load runs.
 *
 * There is no matching teardown, by design. The API has no vendor
 * unregister call — a registration is meant to last for the life of the
 * process — and the request-unregister proc would have to be called during
 * module unload at shutdown, where obs-websocket may already have destroyed
 * the proc handler and the vendor object this holds.
 */
void irl_websocket_vendor_register(void)
{
	if (irl_vendor)
		return;

	irl_vendor = obs_websocket_register_vendor(IRL_VENDOR_NAME);
	if (!irl_vendor) {
		/* The normal case on an OBS without obs-websocket enabled.
		 * Nothing else in the plugin depends on it. */
		blog(LOG_INFO,
		     "[irl-source] obs-websocket not available; vendor requests disabled");
		return;
	}

	for (size_t i = 0; i < IRL_ARRAY_SIZE(irl_vendor_requests); i++) {
		if (!obs_websocket_vendor_register_request(
			    irl_vendor, irl_vendor_requests[i].type,
			    irl_vendor_requests[i].callback, NULL)) {
			blog(LOG_WARNING,
			     "[irl-source] Failed to register obs-websocket vendor request '%s'",
			     irl_vendor_requests[i].type);
		}
	}

	blog(LOG_INFO,
	     "[irl-source] Registered obs-websocket vendor '%s' (obs-websocket API v%u)",
	     IRL_VENDOR_NAME, obs_websocket_get_api_version());
}
