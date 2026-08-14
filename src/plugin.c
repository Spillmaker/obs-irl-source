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
#include "../include/irl-ntp.h"
#include "../include/irl-source.h"
#include "../include/irl-sync.h"

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

	/* Ordering: the NTP client has to exist before the sync settings are
	 * loaded, because loading them applies the configured server to it. */
	irl_ntp_start();
	irl_sync_init();
	return true;
}

/* Runs after every module's obs_module_load(), which is the only point at
 * which obs-websocket is guaranteed to have published its API. See
 * websocket-vendor.c. The dock goes here for a different reason: the frontend
 * is only guaranteed to exist once module loading has finished. */
void obs_module_post_load(void)
{
	irl_websocket_vendor_register();
#ifdef IRL_ENABLE_DOCK
	irl_sync_dock_register();
#endif
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
#ifdef IRL_ENABLE_DOCK
	irl_sync_dock_unregister();
#endif
	irl_ntp_stop();
	irl_sync_shutdown();
}
