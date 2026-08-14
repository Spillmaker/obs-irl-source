/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * irl-sei.h — SEI timecode extraction from encoded video packets.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One SMPTE-style wall-clock stamp as carried in an HEVC time_code SEI.
 *
 * Only the sub-hour fields are used for alignment (see irl-sync.h): the
 * sender writes local calendar components, so `hours` differs by whole hours
 * between senders in different time zones and is display-only. */
struct irl_timecode {
	uint8_t hours;
	uint8_t minutes;
	uint8_t seconds;
	uint16_t n_frames;
};

/* Scan one Annex B access unit for an HEVC time_code SEI (prefix_sei_nut,
 * payload type 136) and decode the first clock timestamp in it.
 *
 * HEVC only, deliberately. Moblin — the only encoder in the IRL ecosystem
 * currently emitting these — has its H.264 path disabled at the source
 * (`if let timecode, false` in MpegTsWriter.packH264), so there is nothing
 * to read on that codec. H.264's pic_timing SEI is also not parseable in
 * isolation: its leading cpb_removal_delay/dpb_output_delay fields are
 * present only when the SPS VUI says so, meaning a correct reader has to
 * track sequence parameter sets. Adding that for a payload no encoder sends
 * would be untestable code, so this returns false for H.264 and the source
 * reports "no timecode" — which is the honest answer.
 *
 * `data`/`size` are the packet payload as delivered by the MPEG-TS demuxer.
 * Returns false when the packet carries no usable timecode, which is the
 * common case: only the access units the encoder chose to stamp have one.
 */
bool irl_sei_find_timecode(const uint8_t *data, size_t size,
			   struct irl_timecode *out);

#ifdef __cplusplus
}
#endif
