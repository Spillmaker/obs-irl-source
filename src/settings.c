/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * settings.c — OBS properties UI for the IRL source
 */

#include "../include/irl-source.h"

/* ── Defaults ─────────────────────────────────────────────── */

void irl_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_int(settings, "reconnect_delay",
				 IRL_DEFAULT_RECONNECT_DELAY);
	obs_data_set_default_int(settings, "network_buffer_mb",
				 IRL_DEFAULT_NETWORK_BUFFER_MB);

	obs_data_set_default_int(settings, "buffer_target_ms",
				 IRL_DEFAULT_BUFFER_TARGET_MS);
	obs_data_set_default_bool(settings, "adaptive_speed",
				  IRL_DEFAULT_ADAPTIVE_SPEED);

	obs_data_set_default_string(settings, "ffmpeg_options", "");
	obs_data_set_default_int(settings, "hw_decode", IRL_DEFAULT_HW_DECODE);
	obs_data_set_default_bool(settings, "wait_for_keyframe",
				  IRL_DEFAULT_WAIT_KEYFRAME);
	obs_data_set_default_bool(settings, "low_latency_audio",
				  IRL_DEFAULT_LOW_LATENCY_AUDIO);
	obs_data_set_default_bool(settings, "close_when_inactive",
				  IRL_DEFAULT_CLOSE_WHEN_INACTIVE);
	obs_data_set_default_bool(settings, "clear_on_disconnect",
				  IRL_DEFAULT_CLEAR_ON_DISCONNECT);
	obs_data_set_default_bool(settings, "sync_enabled",
				  IRL_DEFAULT_SYNC_ENABLED);
}

/* ── Properties ───────────────────────────────────────────── */

obs_properties_t *irl_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	/* Without this, the dialog calls update() on every keystroke, so
	 * typing a URL reopens the stream once per character. */
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	/* ── General ───────────────────────────────────────── */

	obs_properties_add_text(props, "url",
				obs_module_text("URL"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "reconnect_delay",
			       obs_module_text("Reconnect Delay (s)"), 1, 60,
			       1);

	/* ── Audio Buffer ──────────────────────────────────── */

	obs_properties_add_int(props, "buffer_target_ms",
			       obs_module_text("Target Buffer (ms)"), 20, 500,
			       10);
	obs_properties_add_bool(props, "adaptive_speed",
				obs_module_text("Adaptive Latency Control"));
	obs_properties_add_text(
		props, "audio_buffer_help",
		obs_module_text(
			"Target Buffer is the jitter cushion and the main "
			"latency knob. Adaptive Latency Control keeps audio "
			"near native rate with bounded speed correction; "
			"backlog from a stall is played back slightly sped up "
			"instead of skipped. Lower values reduce delay but "
			"make silence or hitches more likely on bad signal."),
		OBS_TEXT_INFO);

	/* ── Timecode Sync ─────────────────────────────────── */

	obs_properties_add_bool(props, "sync_enabled",
				obs_module_text("Sync"));
	obs_properties_add_text(
		props, "sync_help",
		obs_module_text(
			"Sync keeps this feed aligned with every other synced "
			"feed by presenting each frame at the wall-clock time it "
			"was captured, plus a shared offset. The master switch, "
			"the offset and the NTP server live in the IRL Sync dock "
			"(View > Docks), because they are one setting that every "
			"source and every OBS instance has to agree on.\n\n"
			"Requires the sender to embed SEI timecodes: in Moblin "
			"that is Settings > Streams > (stream) > Video > "
			"Timecodes, with an NTP pool set, and it only reaches "
			"the wire on H.265/HEVC over SRT, SRTLA or RIST. The "
			"dock reports \"No timecode\" when nothing arrives."),
		OBS_TEXT_INFO);

	/* ── Advanced ──────────────────────────────────────── */

	obs_properties_add_text(props, "ffmpeg_options",
				obs_module_text("FFmpeg Options"),
				OBS_TEXT_DEFAULT);

	obs_property_t *hw = obs_properties_add_list(
		props, "hw_decode", obs_module_text("Hardware Decode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(hw, obs_module_text("Auto"), 0);
	obs_property_list_add_int(hw, obs_module_text("Off"), 1);

	obs_properties_add_bool(
		props, "wait_for_keyframe",
		obs_module_text("Wait for Keyframe on Start/Reconnect"));
	obs_properties_add_bool(
		props, "low_latency_audio",
		obs_module_text("Use OBS Low-Latency Async Audio Mode"));
	obs_properties_add_bool(
		props, "clear_on_disconnect",
		obs_module_text("Show Nothing When the Stream Ends"));
	obs_properties_add_bool(props, "close_when_inactive",
				obs_module_text("Close Stream When Inactive"));
	obs_properties_add_text(
		props, "advanced_help",
		obs_module_text(
			"Wait for Keyframe avoids corrupt startup/reconnect frames, "
			"but it can add up to one keyframe interval of delay. The "
			"low-latency option enables OBS async unbuffered audio mode "
			"for this source and makes the plugin drain immediately "
			"instead of building the normal jitter cushion, so it is "
			"faster but less tolerant of jitter. Show Nothing When "
			"the Stream Ends blanks the source as soon as the stream "
			"drops, instead of leaving the last frame frozen on screen "
			"until it reconnects. Close Stream "
			"When Inactive stops receiving when the source is not "
			"active, clearing the frame if the option above is on. "
			"FFmpeg Options can "
			"override any demuxer option, for example buffer_size or "
			"the SRT latency."),
		OBS_TEXT_INFO);

	/* ── About ─────────────────────────────────────────── */

	obs_properties_add_text(
		props, "about_info",
		obs_module_text(
			"IRL Source v" OBS_IRL_SOURCE_VERSION
			" by Thomas Lekanger\n"
			"https://irlserver.com\n\n"
			"Codec/protocol-agnostic live source with audio jitter "
			"buffering, PTS discontinuity repair, adaptive latency "
			"control, and first-keyframe gating.\n\n"
			"Licensed under AGPL-3.0-or-later"),
		OBS_TEXT_INFO);

	return props;
}
