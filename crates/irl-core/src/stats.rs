//! The stats surface, defined once: the proc-handler declaration, the
//! calldata writer and the websocket vendor's copy loop all iterate
//! [`FIELDS`], so a new stat is a one-line change.

use crate::sei::Timecode;
use crate::sync::SyncStatus;

/// calldata type of a stat.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StatKind {
    /// `long long`.
    Int,
    /// `double`.
    Float,
    /// `bool`.
    Bool,
    /// `string` (NUL-terminated, copied into the calldata).
    String,
}

/// A value of one stat.
#[derive(Debug, Clone, PartialEq)]
pub enum StatValue {
    /// Integer.
    Int(i64),
    /// Float.
    Float(f64),
    /// Bool.
    Bool(bool),
    /// String.
    Str(String),
}

/// The 35 stat fields in proc-declaration order: the 27 of the C plugin, plus
/// the eight the timecode sync branch contributes (`IRL_SYNC_STATS_PROC_DECL`).
pub const FIELDS: &[(&str, StatKind)] = &[
    ("buffer_fill_ms", StatKind::Int),
    ("current_speed", StatKind::Float),
    ("adaptive_latency_control", StatKind::Bool),
    ("reconnecting", StatKind::Bool),
    ("total_audio_frames", StatKind::Int),
    ("total_video_frames", StatKind::Int),
    ("pts_repairs", StatKind::Int),
    ("pts_normalizations", StatKind::Int),
    ("pts_interpolations", StatKind::Int),
    ("pts_resets", StatKind::Int),
    ("pts_last_gap_ms", StatKind::Int),
    ("pts_max_gap_ms", StatKind::Int),
    ("silence_insertions", StatKind::Int),
    ("audio_underruns", StatKind::Int),
    ("audio_resync_skipped_chunks", StatKind::Int),
    ("audio_hidden_trimmed_chunks", StatKind::Int),
    ("audio_quality_events", StatKind::Int),
    ("audio_output_restarts", StatKind::Int),
    ("obs_lead_ms", StatKind::Int),
    ("audio_decoder_flushes", StatKind::Int),
    ("video_corrupt_frames", StatKind::Int),
    ("video_corrupt_held", StatKind::Int),
    ("video_lead_ms", StatKind::Int),
    ("video_lead_excess", StatKind::Int),
    ("stream_delay_ms", StatKind::Int),
    ("low_latency_audio", StatKind::Bool),
    ("reconnect_count", StatKind::Int),
    // ── timecode sync ──
    ("sync_enabled", StatKind::Bool),
    ("sync_status", StatKind::String),
    ("sync_timecode", StatKind::String),
    ("sync_latency_ms", StatKind::Int),
    ("sync_latency_peak_ms", StatKind::Int),
    ("sync_added_ms", StatKind::Int),
    ("sync_error_ms", StatKind::Int),
    ("sync_required_offset_ms", StatKind::Int),
];

/// A snapshot of every stat, in [`FIELDS`] order.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct StatsSnapshot {
    /// Jitter buffer fill.
    pub buffer_fill_ms: i64,
    /// Smoothed playback speed.
    pub current_speed: f64,
    /// Adaptive latency control enabled.
    pub adaptive_latency_control: bool,
    /// Reconnecting.
    pub reconnecting: bool,
    /// Audio frames decoded this connection.
    pub total_audio_frames: i64,
    /// Video frames decoded this connection.
    pub total_video_frames: i64,
    /// PTS repairs.
    pub pts_repairs: i64,
    /// PTS normalizations.
    pub pts_normalizations: i64,
    /// PTS interpolations.
    pub pts_interpolations: i64,
    /// PTS resets.
    pub pts_resets: i64,
    /// Last PTS gap.
    pub pts_last_gap_ms: i64,
    /// Largest PTS gap.
    pub pts_max_gap_ms: i64,
    /// Silence insertions.
    pub silence_insertions: i64,
    /// Underruns.
    pub audio_underruns: i64,
    /// Chunks skipped on resync.
    pub audio_resync_skipped_chunks: i64,
    /// Chunks trimmed silently.
    pub audio_hidden_trimmed_chunks: i64,
    /// Quality events.
    pub audio_quality_events: i64,
    /// Output clock restarts.
    pub audio_output_restarts: i64,
    /// Queued audio lead ahead of wall clock.
    pub obs_lead_ms: i64,
    /// Audio decoder flushes.
    pub audio_decoder_flushes: i64,
    /// Corrupt video frames seen.
    pub video_corrupt_frames: i64,
    /// Corrupt video frames held back.
    pub video_corrupt_held: i64,
    /// Video lead.
    pub video_lead_ms: i64,
    /// Video lead excess events.
    pub video_lead_excess: i64,
    /// Estimated end-to-end delay.
    pub stream_delay_ms: i64,
    /// Low latency audio enabled.
    pub low_latency_audio: bool,
    /// Reconnects.
    pub reconnect_count: i64,
    /// This source is ticked for timecode sync.
    pub sync_enabled: bool,
    /// Sync status (`off`, `no_timecode`, `stale`, `too_slow`, `acquiring`,
    /// `locked`).
    pub sync_status: SyncStatus,
    /// Most recent timecode from the sender; reported as `HH:MM:SS:FF`, empty
    /// when none.
    pub sync_timecode: Option<Timecode>,
    /// How stale the freshest received frame is, from its timecode against NTP.
    pub sync_latency_ms: i64,
    /// Rolling 60s maximum of the above.
    pub sync_latency_peak_ms: i64,
    /// Extra hold currently applied to reach the target presentation time.
    pub sync_added_ms: i64,
    /// How far the actual presentation lands from the target, averaged over
    /// about a second.
    pub sync_error_ms: i64,
    /// Smallest offset at which this source could hold sync.
    pub sync_required_offset_ms: i64,
}

impl StatKind {
    /// The calldata type name as it appears in a proc declaration.
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Int => "int",
            Self::Float => "float",
            Self::Bool => "bool",
            Self::String => "string",
        }
    }
}

impl StatsSnapshot {
    /// Values in [`FIELDS`] order.
    pub fn values(&self) -> [StatValue; FIELDS.len()] {
        [
            StatValue::Int(self.buffer_fill_ms),
            StatValue::Float(self.current_speed),
            StatValue::Bool(self.adaptive_latency_control),
            StatValue::Bool(self.reconnecting),
            StatValue::Int(self.total_audio_frames),
            StatValue::Int(self.total_video_frames),
            StatValue::Int(self.pts_repairs),
            StatValue::Int(self.pts_normalizations),
            StatValue::Int(self.pts_interpolations),
            StatValue::Int(self.pts_resets),
            StatValue::Int(self.pts_last_gap_ms),
            StatValue::Int(self.pts_max_gap_ms),
            StatValue::Int(self.silence_insertions),
            StatValue::Int(self.audio_underruns),
            StatValue::Int(self.audio_resync_skipped_chunks),
            StatValue::Int(self.audio_hidden_trimmed_chunks),
            StatValue::Int(self.audio_quality_events),
            StatValue::Int(self.audio_output_restarts),
            StatValue::Int(self.obs_lead_ms),
            StatValue::Int(self.audio_decoder_flushes),
            StatValue::Int(self.video_corrupt_frames),
            StatValue::Int(self.video_corrupt_held),
            StatValue::Int(self.video_lead_ms),
            StatValue::Int(self.video_lead_excess),
            StatValue::Int(self.stream_delay_ms),
            StatValue::Bool(self.low_latency_audio),
            StatValue::Int(self.reconnect_count),
            StatValue::Bool(self.sync_enabled),
            StatValue::Str(self.sync_status.name().to_owned()),
            StatValue::Str(
                self.sync_timecode
                    .map(|tc| tc.to_string())
                    .unwrap_or_default(),
            ),
            StatValue::Int(self.sync_latency_ms),
            StatValue::Int(self.sync_latency_peak_ms),
            StatValue::Int(self.sync_added_ms),
            StatValue::Int(self.sync_error_ms),
            StatValue::Int(self.sync_required_offset_ms),
        ]
    }

    /// The named stat, for a caller that wants one field by name (the
    /// websocket vendor's copy loop walks [`FIELDS`] instead).
    pub fn get(&self, name: &str) -> Option<StatValue> {
        let index = FIELDS.iter().position(|(field, _)| *field == name)?;
        Some(self.values()[index].clone())
    }
}

/// The `proc_handler_add` declaration string,
/// `void get_stats(out int buffer_fill_ms, out float current_speed, ...)`.
pub fn proc_declaration() -> String {
    let mut decl = String::from("void get_stats(");
    for (i, (name, kind)) in FIELDS.iter().enumerate() {
        if i > 0 {
            decl.push_str(", ");
        }
        decl.push_str("out ");
        decl.push_str(kind.as_str());
        decl.push(' ');
        decl.push_str(name);
    }
    decl.push(')');
    decl
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The declaration `irl_source_create` passed to `proc_handler_add`
    /// (`src/irl-source.c`), with `out int video_decoder_flushes` removed —
    /// that stat was always zero and is not ported — and the timecode sync
    /// branch's `IRL_SYNC_STATS_PROC_DECL` appended, as its `irl-source.c`
    /// did.
    const C_DECLARATION: &str = "void get_stats(out int buffer_fill_ms, \
out float current_speed, out bool adaptive_latency_control, \
out bool reconnecting, \
out int total_audio_frames, out int total_video_frames, \
out int pts_repairs, out int pts_normalizations, \
out int pts_interpolations, out int pts_resets, \
out int pts_last_gap_ms, out int pts_max_gap_ms, \
out int silence_insertions, out int audio_underruns, \
out int audio_resync_skipped_chunks, \
out int audio_hidden_trimmed_chunks, \
out int audio_quality_events, \
out int audio_output_restarts, out int obs_lead_ms, \
out int audio_decoder_flushes, \
out int video_corrupt_frames, out int video_corrupt_held, \
out int video_lead_ms, out int video_lead_excess, \
out int stream_delay_ms, out bool low_latency_audio, \
out int reconnect_count, \
out bool sync_enabled, \
out string sync_status, out string sync_timecode, \
out int sync_latency_ms, out int sync_latency_peak_ms, \
out int sync_added_ms, out int sync_error_ms, \
out int sync_required_offset_ms)";

    #[test]
    fn there_are_thirty_five_fields() {
        assert_eq!(FIELDS.len(), 35);
        // video_decoder_flushes was removed (it was always 0 in C).
        assert!(
            !FIELDS
                .iter()
                .any(|(name, _)| *name == "video_decoder_flushes")
        );
    }

    #[test]
    fn field_names_are_unique() {
        let mut names: Vec<&str> = FIELDS.iter().map(|(name, _)| *name).collect();
        names.sort_unstable();
        let count = names.len();
        names.dedup();
        assert_eq!(names.len(), count);
    }

    #[test]
    fn declaration_matches_the_c_literal() {
        assert_eq!(proc_declaration(), C_DECLARATION);
    }

    #[test]
    fn every_field_is_written_and_typed() {
        // A snapshot where every field carries a distinct value, so a copy
        // that reads the wrong member shows up.
        let snap = StatsSnapshot {
            buffer_fill_ms: 1,
            current_speed: 1.05,
            adaptive_latency_control: true,
            reconnecting: true,
            total_audio_frames: 2,
            total_video_frames: 3,
            pts_repairs: 4,
            pts_normalizations: 5,
            pts_interpolations: 6,
            pts_resets: 7,
            pts_last_gap_ms: 8,
            pts_max_gap_ms: 9,
            silence_insertions: 10,
            audio_underruns: 11,
            audio_resync_skipped_chunks: 12,
            audio_hidden_trimmed_chunks: 13,
            audio_quality_events: 14,
            audio_output_restarts: 15,
            obs_lead_ms: 16,
            audio_decoder_flushes: 17,
            video_corrupt_frames: 18,
            video_corrupt_held: 19,
            video_lead_ms: 20,
            video_lead_excess: 21,
            stream_delay_ms: 22,
            low_latency_audio: true,
            reconnect_count: 23,
            sync_enabled: true,
            sync_status: SyncStatus::Locked,
            sync_timecode: Some(Timecode {
                hours: 1,
                minutes: 2,
                seconds: 3,
                n_frames: 4,
            }),
            sync_latency_ms: 24,
            sync_latency_peak_ms: 25,
            sync_added_ms: 26,
            sync_error_ms: 27,
            sync_required_offset_ms: 28,
        };

        let values = snap.values();
        assert_eq!(values.len(), FIELDS.len());

        // Types line up with the declaration ...
        for ((name, kind), value) in FIELDS.iter().zip(values.iter()) {
            let matches = matches!(
                (kind, value),
                (StatKind::Int, StatValue::Int(_))
                    | (StatKind::Float, StatValue::Float(_))
                    | (StatKind::Bool, StatValue::Bool(_))
                    | (StatKind::String, StatValue::Str(_))
            );
            assert!(matches, "{name} has the wrong value kind: {value:?}");
        }

        // ... every integer field carries its own distinct value ...
        let ints: Vec<i64> = values
            .iter()
            .filter_map(|v| match v {
                StatValue::Int(i) => Some(*i),
                _ => None,
            })
            .collect();
        let mut sorted = ints.clone();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(sorted.len(), ints.len(), "a field was written twice");
        assert!(!ints.contains(&0), "a field was not written");

        // ... and the non-integer fields are what the snapshot holds.
        assert_eq!(values[1], StatValue::Float(1.05));
        assert_eq!(values[2], StatValue::Bool(true));
        assert_eq!(values[3], StatValue::Bool(true));
        assert_eq!(values[25], StatValue::Bool(true));
        assert_eq!(values[27], StatValue::Bool(true));
        assert_eq!(values[28], StatValue::Str("locked".to_owned()));
        assert_eq!(values[29], StatValue::Str("01:02:03:04".to_owned()));

        // Spot-check the by-name accessor against the same snapshot.
        assert_eq!(snap.get("buffer_fill_ms"), Some(StatValue::Int(1)));
        assert_eq!(snap.get("reconnect_count"), Some(StatValue::Int(23)));
        assert_eq!(snap.get("low_latency_audio"), Some(StatValue::Bool(true)));
        assert_eq!(
            snap.get("sync_required_offset_ms"),
            Some(StatValue::Int(28))
        );
        assert_eq!(snap.get("video_decoder_flushes"), None);
    }

    #[test]
    fn a_default_snapshot_is_all_zero() {
        let values = StatsSnapshot::default().values();
        for (i, value) in values.iter().enumerate() {
            let zero = match value {
                StatValue::Int(v) => *v == 0,
                StatValue::Float(v) => *v == 0.0,
                StatValue::Bool(v) => !*v,
                // No timecode reads as the empty string; the status reads as
                // "off", which is the C's zero-valued enum.
                StatValue::Str(s) => s.is_empty() || s == "off",
            };
            assert!(zero, "{} defaulted to {value:?}", FIELDS[i].0);
        }
        assert_eq!(values[28], StatValue::Str("off".to_owned()));
        assert_eq!(values[29], StatValue::Str(String::new()));
    }
}
