/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * receiver.c — FFmpeg open/read thread (protocol-agnostic)
 *
 * Opens any FFmpeg-supported URL, decodes audio+video, and feeds
 * decoded frames to the jitter buffer / video handler.
 */

#include "../include/irl-source.h"
#include "receiver-internal.h"

/* ── Main read loop ───────────────────────────────────────── */

void *irl_audio_thread(void *data)
{
	struct irl_source *ctx = data;

	while (os_atomic_load_bool(&ctx->thread_active)) {
		if (os_atomic_load_bool(&ctx->reconnecting)) {
			os_sleep_ms(1);
			continue;
		}

		bool pumped = false;
		for (int i = 0; i < 16 && os_atomic_load_bool(&ctx->thread_active);
		     i++) {
			irl_mutex_lock(&ctx->audio_state_lock);
			bool ok = irl_pump_audio_once(ctx);
			irl_mutex_unlock(&ctx->audio_state_lock);
			if (!ok)
				break;
			pumped = true;
		}

		if (!pumped)
			os_sleep_ms(1);
	}

	return NULL;
}

void irl_dispatch_packet(struct irl_source *ctx, AVPacket *pkt, AVFrame *frame)
{
	if (pkt->stream_index == ctx->audio_stream_idx && ctx->audio_dec_ctx)
		irl_handle_audio_packet(ctx, pkt, frame);
	else if (pkt->stream_index == ctx->video_stream_idx &&
		 ctx->video_dec_ctx)
		irl_handle_video_packet(ctx, pkt, frame);
}

void *irl_receiver_thread(void *data)
{
	struct irl_source *ctx = data;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	if (!pkt || !frame) {
		blog(LOG_ERROR,
		     "[irl-source] Failed to allocate packet/frame, receiver exiting");
		av_packet_free(&pkt);
		av_frame_free(&frame);
		os_atomic_store_bool(&ctx->thread_active, false);
		return NULL;
	}

	blog(LOG_INFO, "[irl-source] Receiver thread started for: %s",
	     ctx->config.url ? ctx->config.url : "(null)");

	while (os_atomic_load_bool(&ctx->thread_active)) {
		if (!ctx->fmt_ctx) {
			if (!irl_open_stream(ctx)) {
				if (!irl_wait_for_reconnect(ctx))
					break;
				continue;
			}
			irl_prepare_new_connection(ctx);
		}

		/* Backlog backpressure: above the fill ceiling, stop
		 * reading so the transport holds the excess and playback
		 * bleeds it off via speed. Bounded by buffer capacity
		 * so a burst between checks can never force the ring
		 * buffer to drop audible data. */
		if (ctx->audio_stream_idx >= 0 &&
		    !ctx->config.low_latency_audio) {
			int pace_ms =
				(int)os_atomic_load_long(
					&ctx->config.buffer_max_ms) *
				3;
			if (pace_ms > IRL_BLEED_PACE_FILL_MS)
				pace_ms = IRL_BLEED_PACE_FILL_MS;
			while (os_atomic_load_bool(&ctx->thread_active) &&
			       audio_buffer_fill_ms_locked(&ctx->audio_buf) >
				       pace_ms) {
				os_sleep_ms(5);
			}
			if (!os_atomic_load_bool(&ctx->thread_active))
				break;
		}

		/* Sync delay line at its ceiling: stop reading and let the
		 * transport hold the excess, exactly as the audio bleed pace
		 * above does. Draining inside the wait is what makes room, so
		 * this cannot deadlock while packets are becoming due. */
		while (os_atomic_load_bool(&ctx->thread_active) &&
		       irl_sync_delay_full(ctx)) {
			irl_sync_drain(ctx, frame);
			os_sleep_ms(2);
		}
		if (!os_atomic_load_bool(&ctx->thread_active))
			break;

		ctx->io_start_us = (uint64_t)av_gettime();
		int ret = av_read_frame(ctx->fmt_ctx, pkt);
		if (ret < 0) {
			irl_handle_stream_read_error(ctx, ret);
			continue;
		}

		irl_sync_observe(ctx, pkt);
		if (!irl_sync_hold(ctx, pkt))
			irl_dispatch_packet(ctx, pkt, frame);

		av_packet_unref(pkt);
		irl_sync_drain(ctx, frame);
		irl_log_receiver_stats(ctx);
	}

	irl_close_ffmpeg(ctx);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	return NULL;
}

void irl_receiver_stop(struct irl_source *ctx)
{
	if (!os_atomic_load_bool(&ctx->thread_active))
		return;

	os_atomic_store_bool(&ctx->thread_active, false);
	irl_mutex_lock(&ctx->video_queue_lock);
	irl_cond_broadcast(&ctx->video_queue_cond);
	irl_mutex_unlock(&ctx->video_queue_lock);
	irl_thread_join(&ctx->video_thread);
	irl_thread_join(&ctx->audio_thread);
	irl_thread_join(&ctx->receiver_thread);
}
