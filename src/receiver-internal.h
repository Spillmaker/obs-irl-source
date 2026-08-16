#pragma once

#include "../include/irl-source.h"

uint64_t irl_audio_output_claim(struct irl_source *ctx, int frames,
				int out_rate);
void irl_reset_stream_timing_state(struct irl_source *ctx);
void irl_reset_audio_timing_state(struct irl_source *ctx);
void irl_mark_audio_recovery(struct irl_source *ctx, uint64_t duration_us);
bool irl_audio_recovery_active(const struct irl_source *ctx);
bool irl_open_stream(struct irl_source *ctx);
void irl_close_ffmpeg(struct irl_source *ctx);
void irl_prepare_new_connection(struct irl_source *ctx);
bool irl_wait_for_reconnect(struct irl_source *ctx);
void irl_handle_stream_read_error(struct irl_source *ctx, int read_ret);
bool irl_pump_audio_once(struct irl_source *ctx);
void irl_handle_audio_packet(struct irl_source *ctx, AVPacket *pkt,
			     AVFrame *frame);
void irl_handle_video_packet(struct irl_source *ctx, AVPacket *pkt,
			     AVFrame *frame);
void irl_handle_audio_frame(struct irl_source *ctx, AVFrame *frame);
void irl_handle_video_frame(struct irl_source *ctx, AVFrame *frame);
void irl_video_queue_push(struct irl_source *ctx, AVFrame *frame,
			  int64_t pts_ns);
void irl_video_request_clear(struct irl_source *ctx);
void *irl_video_thread(void *data);
void irl_log_receiver_stats(struct irl_source *ctx);

/* Route one packet to its decoder. Shared by the read loop and the sync
 * delay line, which releases held packets down the same path. */
void irl_dispatch_packet(struct irl_source *ctx, AVPacket *pkt, AVFrame *frame);

/* ── Timecode sync (receiver-sync.c) ──────────────────────── */

/* Fold a freshly read packet into the sync controller: extract its timecode
 * if it has one, measure, and move the hold. */
void irl_sync_observe(struct irl_source *ctx, const AVPacket *pkt);
/* True when the packet was taken by the delay line and must not be dispatched
 * by the caller. */
bool irl_sync_hold(struct irl_source *ctx, AVPacket *pkt);
/* Release everything whose moment has come. */
void irl_sync_drain(struct irl_source *ctx, AVFrame *frame);
bool irl_sync_delay_full(const struct irl_source *ctx);
/* True while the audio output must not prime yet: sync is about to seed a hold,
 * and a hold applied to an already-running pipeline starves it. Read from the
 * audio thread. */
bool irl_sync_prime_held(struct irl_source *ctx);
/* Where the audio holding this PTS has to start playing, in the OBS clock, for
 * the stream to land on its configured offset. False when sync has nothing to
 * say — no timecodes, no NTP reference, sync off — or when the placement is
 * already in the past, which anchoring cannot fix. Called from the audio thread
 * at prime. */
bool irl_sync_playout_anchor(struct irl_source *ctx, int64_t pts_ns,
			     uint64_t now_ns, uint64_t *anchor_ns);
void irl_sync_reset(struct irl_source *ctx);
void irl_sync_free(struct irl_source *ctx);
