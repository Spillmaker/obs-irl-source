/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * sync-group.c — global sync settings and the source registry behind them.
 *
 * Nothing here is on the audio or video path. The receiver threads read the
 * two atomics at the top of this file and publish a small snapshot; the
 * registry exists purely so the dock and the websocket vendor have a list to
 * render. Keeping the coordination out of the hot path is a property of the
 * absolute-time design (see irl-sync.h), not an optimisation.
 */

#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "../include/irl-source.h"
#include "../include/irl-ntp.h"
#include "../include/irl-sync.h"
#include "../include/irl-threading.h"

#define SYNC_CONFIG_FILE "sync.json"

/* ── State ────────────────────────────────────────────────── */

struct registry_slot {
	struct irl_source *ctx;
	struct irl_sync_snapshot snap;
	/* When the receiver thread last published. A source whose receiver has
	 * stopped delivering stops publishing, and its last snapshot would
	 * otherwise keep reading as healthy forever. */
	uint64_t updated_ns;
};

/* Published status older than this is reported as stale rather than as
 * whatever it last said. Comfortably longer than the ~5Hz publish interval,
 * so ordinary scheduling never trips it. */
#define SYNC_PUBLISH_STALE_NS 3000000000ULL

static struct {
	bool initialised;

	/* Hot: read per packet by every receiver thread. */
	volatile bool enabled;
	volatile long offset_ms;
	volatile long offset_generation;

	/* Cold: settings-thread and dock only. */
	irl_mutex_t lock;
	char ntp_server[128];
	struct registry_slot slots[IRL_SYNC_MAX_SOURCES];
	size_t slot_count;
} sync = {0};

/* ── Status names ─────────────────────────────────────────── */

const char *irl_sync_status_name(enum irl_sync_status status)
{
	switch (status) {
	case IRL_SYNC_OFF:
		return "off";
	case IRL_SYNC_NO_TIMECODE:
		return "no_timecode";
	case IRL_SYNC_STALE:
		return "stale";
	case IRL_SYNC_TOO_SLOW:
		return "too_slow";
	case IRL_SYNC_ACQUIRING:
		return "acquiring";
	case IRL_SYNC_LOCKED:
		return "locked";
	}
	return "unknown";
}

/* ── Settings ─────────────────────────────────────────────── */

static void sync_config_save(void);

bool irl_sync_is_enabled(void)
{
	return os_atomic_load_bool(&sync.enabled);
}

int irl_sync_offset_ms(void)
{
	return (int)os_atomic_load_long(&sync.offset_ms);
}

uint32_t irl_sync_offset_generation(void)
{
	return (uint32_t)os_atomic_load_long(&sync.offset_generation);
}

void irl_sync_config_get(struct irl_sync_config *out)
{
	out->enabled = irl_sync_is_enabled();
	out->offset_ms = irl_sync_offset_ms();

	irl_mutex_lock(&sync.lock);
	snprintf(out->ntp_server, sizeof(out->ntp_server), "%s",
		 sync.ntp_server);
	irl_mutex_unlock(&sync.lock);
}

void irl_sync_set_enabled(bool enabled)
{
	if (irl_sync_is_enabled() == enabled)
		return;

	os_atomic_store_bool(&sync.enabled, enabled);
	/* Turning sync on is a re-seat for every source, same as an offset
	 * change: their delay lines start from nothing. */
	os_atomic_inc_long(&sync.offset_generation);

	/* The clock reference is only wanted while sync is. Nothing else in
	 * the plugin uses NTP, so leaving it polling would mean sending
	 * packets to a third-party pool on behalf of users who never turned
	 * this on. */
	irl_ntp_set_enabled(enabled);

	blog(LOG_INFO, "[irl-source] Timecode sync %s (offset %dms)",
	     enabled ? "enabled" : "disabled", irl_sync_offset_ms());
	sync_config_save();
}

void irl_sync_set_offset_ms(int offset_ms)
{
	if (offset_ms < IRL_SYNC_MIN_OFFSET_MS)
		offset_ms = IRL_SYNC_MIN_OFFSET_MS;
	if (offset_ms > IRL_SYNC_MAX_OFFSET_MS)
		offset_ms = IRL_SYNC_MAX_OFFSET_MS;

	if (irl_sync_offset_ms() == offset_ms)
		return;

	os_atomic_set_long(&sync.offset_ms, offset_ms);
	/* A user turning this knob is asking every synced source to shift at
	 * once and expects a hitch. Bumping the generation makes the receivers
	 * step rather than slew, which converges in one frame instead of the
	 * minute a drift-rate correction would take. */
	os_atomic_inc_long(&sync.offset_generation);

	blog(LOG_INFO, "[irl-source] Timecode sync offset set to %dms",
	     offset_ms);
	sync_config_save();
}

void irl_sync_set_ntp_server(const char *host)
{
	char applied[sizeof(sync.ntp_server)];

	irl_mutex_lock(&sync.lock);
	snprintf(sync.ntp_server, sizeof(sync.ntp_server), "%s",
		 host ? host : "");
	snprintf(applied, sizeof(applied), "%s", sync.ntp_server);
	irl_mutex_unlock(&sync.lock);

	irl_ntp_set_server(applied);
	sync_config_save();
}

/* ── Persistence ──────────────────────────────────────────── */

static char *sync_config_path(void)
{
	char *dir = obs_module_get_config_path(obs_current_module(), "");
	if (!dir)
		return NULL;

	/* Created on demand: OBS only makes a module's config directory when
	 * a module asks for it. */
	os_mkdirs(dir);
	bfree(dir);

	return obs_module_get_config_path(obs_current_module(),
					  SYNC_CONFIG_FILE);
}

static void sync_config_load(void)
{
	/* Defaults first, so a missing or unreadable file is simply "not
	 * configured yet" rather than a failure to start. */
	os_atomic_store_bool(&sync.enabled, false);
	os_atomic_set_long(&sync.offset_ms, IRL_SYNC_DEFAULT_OFFSET_MS);
	irl_mutex_lock(&sync.lock);
	snprintf(sync.ntp_server, sizeof(sync.ntp_server), "%s",
		 IRL_SYNC_DEFAULT_NTP_SERVER);
	irl_mutex_unlock(&sync.lock);

	char *path = sync_config_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file(path);
	bfree(path);
	if (!data)
		return;

	obs_data_set_default_bool(data, "enabled", false);
	obs_data_set_default_int(data, "offset_ms", IRL_SYNC_DEFAULT_OFFSET_MS);
	obs_data_set_default_string(data, "ntp_server",
				    IRL_SYNC_DEFAULT_NTP_SERVER);

	int offset = (int)obs_data_get_int(data, "offset_ms");
	if (offset < IRL_SYNC_MIN_OFFSET_MS)
		offset = IRL_SYNC_MIN_OFFSET_MS;
	if (offset > IRL_SYNC_MAX_OFFSET_MS)
		offset = IRL_SYNC_MAX_OFFSET_MS;

	os_atomic_store_bool(&sync.enabled, obs_data_get_bool(data, "enabled"));
	os_atomic_set_long(&sync.offset_ms, offset);

	irl_mutex_lock(&sync.lock);
	snprintf(sync.ntp_server, sizeof(sync.ntp_server), "%s",
		 obs_data_get_string(data, "ntp_server"));
	irl_mutex_unlock(&sync.lock);

	obs_data_release(data);
}

static void sync_config_save(void)
{
	char *path = sync_config_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", irl_sync_is_enabled());
	obs_data_set_int(data, "offset_ms", irl_sync_offset_ms());

	irl_mutex_lock(&sync.lock);
	obs_data_set_string(data, "ntp_server", sync.ntp_server);
	irl_mutex_unlock(&sync.lock);

	if (!obs_data_save_json_safe(data, path, "tmp", "bak"))
		blog(LOG_WARNING,
		     "[irl-source] Could not save sync settings to %s", path);

	obs_data_release(data);
	bfree(path);
}

/* ── Registry ─────────────────────────────────────────────── */

void irl_sync_register_source(struct irl_source *ctx)
{
	irl_mutex_lock(&sync.lock);

	if (sync.slot_count < IRL_SYNC_MAX_SOURCES) {
		struct registry_slot *slot = &sync.slots[sync.slot_count++];
		memset(slot, 0, sizeof(*slot));
		slot->ctx = ctx;
	} else {
		blog(LOG_WARNING,
		     "[irl-source] More than %d IRL sources; the newest will not appear in the sync dock",
		     IRL_SYNC_MAX_SOURCES);
	}

	irl_mutex_unlock(&sync.lock);
}

void irl_sync_unregister_source(struct irl_source *ctx)
{
	irl_mutex_lock(&sync.lock);

	for (size_t i = 0; i < sync.slot_count; i++) {
		if (sync.slots[i].ctx != ctx)
			continue;

		sync.slots[i] = sync.slots[sync.slot_count - 1];
		sync.slot_count--;
		break;
	}

	irl_mutex_unlock(&sync.lock);
}

void irl_sync_publish(struct irl_source *ctx,
		      const struct irl_sync_snapshot *snap)
{
	irl_mutex_lock(&sync.lock);

	for (size_t i = 0; i < sync.slot_count; i++) {
		if (sync.slots[i].ctx == ctx) {
			sync.slots[i].snap = *snap;
			sync.slots[i].updated_ns = os_gettime_ns();
			break;
		}
	}

	irl_mutex_unlock(&sync.lock);
}

/* A source that has stopped publishing is stale, whatever it last said —
 * unless it last said it was not participating, which stays true. */
static void apply_staleness(struct irl_sync_snapshot *snap, uint64_t updated_ns,
			    uint64_t now_ns)
{
	if (snap->status == IRL_SYNC_OFF)
		return;
	if (updated_ns != 0 && now_ns - updated_ns < SYNC_PUBLISH_STALE_NS)
		return;

	snap->status = IRL_SYNC_STALE;
	snap->error_ms = 0;
}

size_t irl_sync_collect(struct irl_sync_entry *entries, size_t max)
{
	size_t written = 0;
	uint64_t now_ns = os_gettime_ns();

	/* The lock is held across obs_source_get_name() so a source cannot be
	 * unregistered — which destroy() does before tearing anything down —
	 * while its name is being read. */
	irl_mutex_lock(&sync.lock);

	for (size_t i = 0; i < sync.slot_count && written < max; i++) {
		const struct registry_slot *slot = &sync.slots[i];
		struct irl_sync_entry *entry = &entries[written++];

		memset(entry, 0, sizeof(*entry));
		const char *name = obs_source_get_name(slot->ctx->source);
		snprintf(entry->source_name, sizeof(entry->source_name), "%s",
			 name ? name : "");
		entry->sync_enabled =
			os_atomic_load_bool(&slot->ctx->config.sync_enabled);
		entry->snap = slot->snap;
		apply_staleness(&entry->snap, slot->updated_ns, now_ns);
	}

	irl_mutex_unlock(&sync.lock);

	return written;
}

bool irl_sync_get_snapshot(struct irl_source *ctx,
			   struct irl_sync_snapshot *out)
{
	bool found = false;
	uint64_t now_ns = os_gettime_ns();

	irl_mutex_lock(&sync.lock);

	for (size_t i = 0; i < sync.slot_count; i++) {
		if (sync.slots[i].ctx != ctx)
			continue;

		*out = sync.slots[i].snap;
		apply_staleness(out, sync.slots[i].updated_ns, now_ns);
		found = true;
		break;
	}

	irl_mutex_unlock(&sync.lock);

	return found;
}

/* ── Lifecycle ────────────────────────────────────────────── */

void irl_sync_init(void)
{
	if (sync.initialised)
		return;

	irl_mutex_init(&sync.lock);
	sync.initialised = true;

	sync_config_load();

	/* Apply the loaded settings to the clock. Order matters only in that
	 * the server has to be known before polling is allowed to start. */
	struct irl_sync_config cfg;
	irl_sync_config_get(&cfg);
	irl_ntp_set_server(cfg.ntp_server);
	irl_ntp_set_enabled(cfg.enabled);
}

void irl_sync_shutdown(void)
{
	if (!sync.initialised)
		return;

	irl_mutex_destroy(&sync.lock);
	sync.initialised = false;
}
