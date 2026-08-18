/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * plugin.c — OBS module registration
 */

#include <obs-module.h>
#include "../include/irl-source.h"
/* IRLSync */
#include "../include/sync/irl-sync.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-irl-source", "en-US")

static struct obs_source_info irl_source_info = {
	.id = IRL_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = irl_source_get_name,
	.create = irl_source_create,
	.destroy = irl_source_destroy,
	.update = irl_source_update,
	.activate = irl_source_activate,
	.deactivate = irl_source_deactivate,
	.show = irl_source_show,
	.hide = irl_source_hide,
	.get_defaults = irl_source_get_defaults,
	.get_properties = irl_source_get_properties,
	.video_tick = irl_source_tick,
	.media_play_pause = irl_source_media_play_pause,
	.media_restart = irl_source_media_restart,
	.media_stop = irl_source_media_stop,
	.media_get_state = irl_source_media_get_state,
};

bool obs_module_load(void)
{
	obs_register_source(&irl_source_info);
	/* IRLSync */
	irl_sync_module_load();
	return true;
}

/* Runs after every module's obs_module_load(), which is the only point at
 * which obs-websocket is guaranteed to have published its API. See
 * websocket-vendor.c. We also load the IRLSync Dock here since this is the
 * step where we know UI exists. */
void obs_module_post_load(void)
{
	irl_websocket_vendor_register();
	/* IRLSync */
	irl_sync_module_post_load();
}

const char *obs_module_description(void)
{
	return "IRL Source by irlserver.com — live streaming source with "
	       "jitter buffering, PTS repair, and adaptive latency control";
}

const char *obs_module_author(void)
{
	return "Thomas Lekanger";
}

void obs_module_unload(void)
{
	/* IRLSync */
	irl_sync_module_unload();
}
