# Timecode sync

Multiple feeds — a main camera and a chase camera, say — arrive over different
uplinks with different delays, so cutting between them jumps backwards and
forwards in time. Timecode sync fixes that: each feed is held until the moment
it was *captured*, plus a shared offset, so every synced feed shows the same
instant at the same instant.

The alignment is absolute, not relative. Feeds never negotiate with each other
and none of them needs to know the others exist — they simply all present the
frame captured at time T at time T + offset. Two OBS instances anywhere, on the
same NTP reference and the same offset, therefore show the same moment at the
same moment. That is what makes the offset one shared number rather than a
per-source setting, and it is why co-streamers only have to agree on two things:
the NTP pool and the offset.

## What you need

Sync reads SEI timecodes embedded by the sender, so the sender has to put them
there. Today that means **Moblin**:

- Settings → Streams → *(your stream)* → Video → **Timecodes**, with an **NTP
  pool** configured (Moblin will not stamp anything without one).
- **H.265/HEVC** only, over **SRT, SRTLA or RIST**. Moblin's H.264 path is
  disabled in its own source, and its RTMP path never carries the timecode.

Then, in OBS: tick **Sync** on each source and open **View → Docks → IRL
Sync**. Click the server name on the reference clock to set the same NTP pool,
click the offset badge to set the offset, and turn the switch on.

## Choosing an offset

The offset is set in **whole seconds**, because it is a number co-streamers
read to each other and "six" is something two people can agree on over a call.

It must be larger than the feed's real arrival latency, or its frames are
already past their slot when they get here. The dock lists every source with
Sync ticked, and shows:

- **Latency** — how stale the freshest data is. A property of the sender and
  the network; nothing you set changes it.
- **Added** — how much extra hold the plugin is applying to reach the target.
- **Drift** — how far the actual presentation lands from the target. Near zero
  once locked, and a residual rather than a fault: the loop is what corrects
  it. Averaged over about a second, because the per-packet reading
  carries roughly a frame of noise that the alignment underneath it does not.

Changing it is a several-second step for every synced feed at once, so it
happens on OK in the dialog rather than as you type.

A feed that cannot reach the offset turns red and says the value that would fix
it — its rolling 60-second peak latency, rounded up to a whole second. The peak
rather than the instantaneous value, because bonded cellular does not degrade
gently and an offset chosen against the current reading holds right up until
the first bitrate dip. Raise the offset to that, or pass the number to whoever
is holding the phone.

Raising the offset raises latency for everyone, so it is a real cost — sync
means every feed waits for the slowest one.

## What the statuses mean

| Status | Meaning |
|---|---|
| **Locked** | Aligned, within a frame or two |
| **Acquiring** | Converging. Normal at startup and after an offset change |
| **Too slow** | The feed arrives later than the offset. Raise the offset, or improve that uplink |
| **No timecode** | Nothing to align against. The dock says which of three causes it is: *no NTP reference here*, *stream is not H.265*, or *sender is not stamping*. Not a sync failure — the offset will not help |
| **No data** | The feed stopped delivering |
| **Off** | Nothing to align: the master switch is off, this source is not ticked, or no stream is arriving |

"No timecode" is by far the most common reason sync appears not to work, and it
is always a sender-side configuration problem.

## Accuracy, and what it is not

Expect **within one or two frames**. The error budget is NTP discipline at both
ends (a few ms each), the timecode's own one-frame resolution, and OBS's render
tick. That is well inside what anyone notices between camera angles.

Sync holds at the point of composition, not at the viewer. Two OBS instances
will composite the same captured moment at the same time, but their outbound
encode and CDN paths still differ, so a viewer watching two *streams* side by
side sees them offset by that difference. No plugin can control it.

## How it works

Each synced source holds encoded packets in front of its decoder until they are
due. Holding compressed data is what makes a multi-second offset practical:
five seconds of a 6 Mbit/s feed is under 4 MB, while the same five seconds of
decoded 1080p60 would be several gigabytes.

Nothing about the audio pipeline changes. Delaying the input shifts the whole
PTS-to-OBS mapping with it, so the jitter buffer, the adaptive speed controller
and the video pacing keep doing exactly what they did — they just see the stream
arrive later.

The alignment itself is made once, not converged on. When a source's audio
output starts, its clock is anchored where the timecodes say that audio belongs;
nothing is playing yet, so choosing that instant costs only pre-roll. After
that the only thing left is real drift — the sender's clock against ours, and
the network moving — which is slow enough for the existing speed correction to
absorb inaudibly.

The plugin runs its own SNTP client rather than trusting the system clock,
which on Windows defaults to roughly one-second accuracy — two orders of
magnitude too coarse for this. Moblin does the same on its end, for the same
reason.

It polls from the moment the plugin loads, not from the moment sync is switched
on, so the dock's clock is running before you commit to anything — which is what
you read to decide whether sync is worth turning on at all.

### Which server you set changes how it polls

How hard a client may poll is a property of the server, so the server name
picks the policy.

**A server run for this plugin** — today `ntp.kringkast.com`, which is the
default — is there to be asked. The client sends four packets on arrival and keeps the one with the
lowest round trip, then one packet every 1 to 30 seconds, at random. Random
rather than fixed so that every OBS on the network does not poll in step.

That sample rate is what pays for the second number on the clock line: the
**ppm** figure is this machine's own crystal error, measured by fitting the
last quarter hour of samples. A PC that runs 40 ppm fast loses 2.4ms of
alignment every minute, which is a frame and a half at 60fps, and on this
policy the client extrapolates that away between polls instead of waiting for
the next packet to correct it.

**A public pool** — `pool.ntp.org`, or any other host you type in — is donated
capacity. There
the client sends one packet per poll, every 64 seconds, doubles that interval
if the server answers with a rate-limit kiss-o'-death, and stops polling
altogether if it answers with a denial. The crystal error is still measured
and shown, but it is not applied: one sample a minute is too sparse a fit to
hand the timeline to.

If the configured server stops answering, three failed polls in a row hand over
to `pool.ntp.org` so the clock keeps running, and the clock line says
`fallback` while the server name above it changes to the pool. The original is
probed again every 30 to 60 seconds — randomised again, so that everyone
knocked off by one outage does not all come back at the same instant — and the
client returns to it on the first answer.

The master switch, the offset and the NTP server are stored per machine (in the
plugin's own config), not in the scene collection. A scene collection copied to
another machine brings its sources but not its offset, which is intended: the
offset describes an agreement between people, not a layout.

## Building the dock

The Linux build needs one extra package for it:

```bash
sudo apt install qt6-base-dev
```

`qt6-base-dev` builds the IRL Sync dock, and it is the only thing in the plugin
that needs Qt. Without it the build still succeeds and configure says so — sync
itself runs headless and its status stays readable over the obs-websocket
vendor, you just get no dock. `-DIRL_ENABLE_DOCK=OFF` skips it deliberately.
On Windows and macOS, Qt6 comes from the `*-deps-qt6-*` archives in the same
obs-deps release used for libobs; point `CMAKE_PREFIX_PATH` at it.

## Reading status over obs-websocket

The plugin's vendor (`obs-irl-source`, see the root
[README](../../README.md#reading-stats-over-obs-websocket)) adds one request:

| Request | Params | Returns |
|---|---|---|
| `GetSyncStatus` | none | `sync_enabled`, `offset_ms`, `clock`, `sources`, `out_of_sync_count`, `recommended_offset_ms` |

`GetSyncStatus` is the alerting path for timecode sync. A dock
only helps when somebody is looking at it, and during a live show the person who
can actually fix an out-of-sync feed is the one out in the field — so a bot
polling this can put "chase cam out of sync, needs 7.4s" in chat, where it will
be seen. `out_of_sync_count` and `recommended_offset_ms` are pre-computed so a
bot does not have to reimplement the policy. `clock` reports the NTP reference
as `{synced, server, primary_server, offset_ms, rtt_ms, age_ms, utc_ms,
burst_mode, fallback_active, drift_valid, drift_ppm}`; a large `age_ms` means
the reference has gone stale and everything downstream of it is suspect, and
`fallback_active` means `server` is the pool the client failed over to rather
than the `primary_server` it was asked for. Each
entry in `sources` carries `source_name`, `sync_enabled`, `status`, `timecode`,
`latency_ms`, `latency_peak_ms`, `added_ms`, `error_ms`, `required_offset_ms`
and `timecode_reason` — `ok`, `no_clock`, `codec` or `absent`, which says which
of the three causes of a `no_timecode` status applies.

Each source's `get_stats` proc also carries these fields:

| Field | Type | Meaning |
|---|---|---|
| `sync_enabled` | bool | Whether this source is ticked for timecode sync |
| `sync_status` | string | `off`, `no_timecode`, `stale`, `too_slow`, `acquiring` or `locked` |
| `sync_timecode` | string | Most recent timecode from the sender, `HH:MM:SS:FF`, empty when none |
| `sync_latency_ms` | int | How stale the freshest received frame is, from its timecode against NTP |
| `sync_latency_peak_ms` | int | Rolling 60s maximum of the above. Pick an offset against this, not the instantaneous value |
| `sync_added_ms` | int | Extra hold currently applied to reach the target presentation time |
| `sync_error_ms` | int | How far the actual presentation lands from the target, averaged over about a second. Near zero when locked. Shown in the dock as **Drift** |
| `sync_required_offset_ms` | int | Smallest offset at which this source could hold sync: its peak latency rounded up to a whole second |

---

# Working on this code

This half is for someone changing the feature rather than using it.

## Files

| file | what it owns |
|---|---|
| `sync-control.c` | the controller: SEI to latency, the delay line, the drift loop, the playout anchor |
| `sync-config.c` | global settings and their store, the source registry, the stats calldata, the websocket handler, module lifecycle |
| `sync-ntp.c` | the module's own SNTP client |
| `sync-sei.c` | Annex B scan and `time_code` decode |
| `sync-dock.cpp` | the Qt dock — the only C++ in the plugin |
| `../../include/sync/irl-sync.h` | the entire public API, in one header |
| `../../include/sync/irl-sync-state.h` | `struct irl_sync_state`, embedded in `struct irl_source` |

## How it attaches to the plugin

Outside this folder the feature is nineteen lines, each one call, each marked
`/* -- timecode sync -- */` so the whole set greps out at once:

```
grep -rn "timecode sync" src --include=*.c
```

| file | calls |
|---|---|
| `plugin.c` | `irl_sync_module_load` / `_post_load` / `_unload` |
| `receiver.c` | `irl_sync_wait_for_room`, `irl_sync_intercept`, `irl_sync_drain` |
| `irl-source.c` | register, unregister, reset, free, `irl_sync_stats`, `IRL_SYNC_STATS_PROC_DECL` |
| `receiver-stream.c` | `irl_sync_reset` on disconnect and on a new connection |
| `receiver-audio.c` | `irl_sync_prime_held`, `irl_sync_playout_anchor` |
| `settings.c` | `irl_sync_source_defaults`, `irl_sync_source_properties` |
| `websocket-vendor.c` | `IRL_SYNC_STAT_FIELDS`, `irl_sync_vendor_status` |

`struct irl_source` carries one member, `sync`. Nothing outside this folder
looks inside it — the plugin embeds it, sizes it and zeroes it, and that is all.

The coupling that remains, deliberately, is the other way: this code includes
`irl-source.h`, because a controller over the pipeline has to see the pipeline.
Hiding that behind an abstraction would cost more than it buys.

## Control model

Two mechanisms, and it matters which does what.

**The anchor places the stream. Once.** When the audio output primes, its clock
is anchored. That is the only moment the stream can be positioned exactly and
for free, because nothing is playing yet and starting later costs only pre-roll.
`irl_sync_playout_anchor()` returns where the audio in hand belongs, and the
output starts there. Every restart of the clock line goes through the same door
— prime, the concealment re-anchor, the stalled-clock restart — so an outage
costs the audio it costs but not the alignment.

**The loop only holds it there.** After placement the residual is real drift:
the sender's clock against ours, and the network moving. Tens of milliseconds
over minutes, which the existing adaptive speed controller absorbs inaudibly.

The delay line is *storage*, not steering. Moving the hold does not move the
presentation time — it moves content between the delay line and the jitter
buffer, and the total delay is unchanged. Only the speed controller draining or
building that buffer actually presents anything earlier or later, at +5% and
-2%. Every rate in `sync-control.c` is set under those two numbers for that
reason, and no amount of tuning here can beat them.

## Things that will bite

- **The frame is the resolution floor.** `n_frames` is a 9-bit index within its
  timecode second, so capture time is known to one frame — 33 ms at 30 fps.
  Frame-accurate alignment is reachable; phase-coherent audio mixing is not.
- **The frame rate is never assumed.** It is learned from the timecodes by
  agreement between recent seconds, not by taking the maximum: a corrupt SEI
  once made a 30 fps sender look like 41 fps and put a sawtooth into every
  latency reading.
- **Latency arithmetic is modulo one hour.** Senders write local calendar
  components and the timecode carries no date, so whole-hour zone differences
  and midnight both fold out. A sender on a half-hour zone lands on the fold.
- **`now` cancels out of the target.** `target = now + offset - (now - capture)`
  is `capture + offset`, so arrival jitter is not in the error signal. Filtering
  the latency would put it back in.
- **Priming is gated.** Engage seeds a multi-second hold, and a hold applied to
  an already-playing pipeline starves it. `irl_sync_prime_held()` keeps the
  audio output from priming until the delay line is flowing.
- **A hold change takes a hold to arrive.** The delay line is a queue holding
  seconds of content, so the loop waits one settle interval between corrections
  or it winds itself up. The pre-roll takeover at prime is the exception: it
  shifts the queued packets too, so it lands immediately.
- **Growing the hold starves; shrinking it floods.** Both are rate-limited, and
  the shift helper only ever moves release times later.
