/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * sei-timecode.c — HEVC time_code SEI extraction (ITU-T H.265 D.2.27).
 *
 * Parsed here rather than taken from FFmpeg's AV_FRAME_DATA_S12M_TIMECODE
 * side data, which the HEVC decoder does produce for this payload. That side
 * data is packed through av_timecode_get_smpte(avctx->framerate, ...), which
 * squeezes the wire format's 9-bit n_frames into SMPTE 12M's two-digit frame
 * field (`ff % 40`) and only handles rates above 30fps when avctx->framerate
 * is populated. Live MPEG-TS routinely leaves the frame rate unset — the same
 * reason receiver-video.c measures the frame interval itself — so a 60fps
 * sender's frame 45 would silently arrive as 5. Reading the SEI directly
 * keeps the full field and needs no frame rate at all.
 */

#include <string.h>

#include "../include/irl-sei.h"

#define HEVC_NAL_PREFIX_SEI_NUT 39
#define HEVC_SEI_TYPE_TIME_CODE 136

/* Cap on the unescaped SEI payload we are willing to buffer. Time_code is a
 * handful of bytes; anything larger is another payload type entirely (HDR
 * metadata, user data) and is skipped without copying. */
#define SEI_SCRATCH_BYTES 256

/* ── Bit reader ───────────────────────────────────────────── */

struct bitreader {
	const uint8_t *data;
	size_t size;
	size_t bit_pos;
};

static bool br_read(struct bitreader *br, int count, uint32_t *out)
{
	uint32_t value = 0;

	if (count <= 0 || count > 32)
		return false;
	if (br->bit_pos + (size_t)count > br->size * 8)
		return false;

	for (int i = 0; i < count; i++) {
		size_t byte = (br->bit_pos + (size_t)i) / 8;
		int bit = 7 - (int)((br->bit_pos + (size_t)i) % 8);
		value = (value << 1) | ((br->data[byte] >> bit) & 1u);
	}
	br->bit_pos += (size_t)count;
	*out = value;
	return true;
}

/* ── Annex B / RBSP helpers ───────────────────────────────── */

/* Strip emulation prevention bytes (0x00 0x00 0x03 → 0x00 0x00) from an
 * escaped NAL payload, stopping once `dst` is full.
 *
 * Truncating rather than failing is deliberate: an SEI NAL may hold several
 * messages, and a time_code that fits within the scratch is still readable
 * even if something large follows it. A message that ends up straddling the
 * cut is rejected by the payload-size check in scan_sei_rbsp(). */
static size_t rbsp_unescape(const uint8_t *src, size_t src_size, uint8_t *dst,
			    size_t dst_size)
{
	size_t out = 0;
	size_t zeros = 0;

	for (size_t i = 0; i < src_size && out < dst_size; i++) {
		uint8_t b = src[i];

		if (zeros >= 2 && b == 0x03) {
			/* The escape byte itself is dropped, and the zero run
			 * restarts: 00 00 03 00 00 03 is two escapes. */
			zeros = 0;
			continue;
		}

		dst[out++] = b;
		zeros = (b == 0x00) ? zeros + 1 : 0;
	}

	return out;
}

/* Locate the next Annex B start code at or after `from`. Sets `*nal_start` to
 * the first byte after it. Returns false when no further start code exists. */
static bool next_start_code(const uint8_t *data, size_t size, size_t from,
			    size_t *nal_start)
{
	for (size_t i = from; i + 3 <= size; i++) {
		if (data[i] != 0 || data[i + 1] != 0)
			continue;
		if (data[i + 2] == 1) {
			*nal_start = i + 3;
			return true;
		}
		if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
			*nal_start = i + 4;
			return true;
		}
	}
	return false;
}

/* ── time_code payload ────────────────────────────────────── */

/* H.265 D.2.27 time_code( payloadSize ). Only the first clock timestamp is
 * decoded: the spec allows up to three, and a sender that means "now" writes
 * one (Moblin hardcodes num_clock_ts = 1). */
static bool parse_time_code(const uint8_t *payload, size_t size,
			    struct irl_timecode *out)
{
	struct bitreader br = {payload, size, 0};
	uint32_t v;

	if (!br_read(&br, 2, &v) || v < 1)
		return false;

	/* clock_timestamp_flag[0] */
	if (!br_read(&br, 1, &v) || v == 0)
		return false;

	/* units_field_based_flag(1) + counting_type(5) */
	if (!br_read(&br, 6, &v))
		return false;

	uint32_t full_timestamp_flag;
	if (!br_read(&br, 1, &full_timestamp_flag))
		return false;

	/* discontinuity_flag(1) + cnt_dropped_flag(1) */
	if (!br_read(&br, 2, &v))
		return false;

	uint32_t n_frames;
	if (!br_read(&br, 9, &n_frames))
		return false;

	/* Without full_timestamp_flag the fields are individually optional and
	 * a sender may omit hours or minutes entirely, which cannot be aligned
	 * against a wall clock. Treat it as no timecode rather than guessing. */
	if (!full_timestamp_flag)
		return false;

	uint32_t seconds, minutes, hours;
	if (!br_read(&br, 6, &seconds) || !br_read(&br, 6, &minutes) ||
	    !br_read(&br, 5, &hours))
		return false;

	if (seconds > 59 || minutes > 59 || hours > 23)
		return false;

	out->hours = (uint8_t)hours;
	out->minutes = (uint8_t)minutes;
	out->seconds = (uint8_t)seconds;
	out->n_frames = (uint16_t)n_frames;
	return true;
}

/* ── SEI message walk ─────────────────────────────────────── */

/* Walk the sei_message() list in one unescaped prefix SEI RBSP. */
static bool scan_sei_rbsp(const uint8_t *rbsp, size_t size,
			  struct irl_timecode *out)
{
	size_t pos = 0;

	while (pos < size) {
		size_t payload_type = 0;
		size_t payload_size = 0;

		/* ff_byte runs, both fields. A 0xFF with nothing after it is a
		 * truncated message, not a valid extension. */
		while (pos < size && rbsp[pos] == 0xFF) {
			payload_type += 255;
			pos++;
		}
		if (pos >= size)
			return false;
		payload_type += rbsp[pos++];

		while (pos < size && rbsp[pos] == 0xFF) {
			payload_size += 255;
			pos++;
		}
		if (pos >= size)
			return false;
		payload_size += rbsp[pos++];

		if (payload_size > size - pos)
			return false;

		if (payload_type == HEVC_SEI_TYPE_TIME_CODE &&
		    parse_time_code(&rbsp[pos], payload_size, out))
			return true;

		pos += payload_size;
	}

	return false;
}

/* ── Entry point ──────────────────────────────────────────── */

bool irl_sei_find_timecode(const uint8_t *data, size_t size,
			   struct irl_timecode *out)
{
	if (!data || !out || size < 4)
		return false;

	size_t nal_start;
	if (!next_start_code(data, size, 0, &nal_start))
		return false;

	while (nal_start < size) {
		size_t next_nal;
		bool has_next = next_start_code(data, size, nal_start, &next_nal);
		/* The start code the scan found is preceded by its own two or
		 * three leading zero bytes, which are not part of this NAL.
		 * Trimming them is unnecessary here: parsing stops at the
		 * declared payload size long before reaching them. */
		size_t nal_end = has_next ? next_nal : size;

		if (nal_end > nal_start + 2) {
			uint8_t type = (data[nal_start] >> 1) & 0x3F;

			if (type == HEVC_NAL_PREFIX_SEI_NUT) {
				/* Skip the two-byte HEVC NAL header. */
				const uint8_t *payload = &data[nal_start + 2];
				size_t payload_size = nal_end - nal_start - 2;
				uint8_t rbsp[SEI_SCRATCH_BYTES];

				size_t rbsp_size = rbsp_unescape(
					payload, payload_size, rbsp,
					sizeof(rbsp));
				if (rbsp_size &&
				    scan_sei_rbsp(rbsp, rbsp_size, out))
					return true;
			}
		}

		if (!has_next)
			break;
		nal_start = next_nal;
	}

	return false;
}
