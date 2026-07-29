# Raptor Streaming System (RSS)

A modular microservice camera streamer for Ingenic SoCs. Raptor replaces the
traditional monolithic streamer with independent daemons that communicate
through POSIX shared-memory ring buffers and Unix domain control sockets.

## Why Raptor

A camera streamer sounds like one job. It is really six unrelated ones
fused together: driving a closed vendor SDK, parsing hostile network
input, writing to flash, signing media, flipping GPIOs, keeping A/V
clocks honest. The incumbents put all of that in one process, on a
device with as little as 32 MB of RAM, no swap, no operator, and an
uptime measured in months.

Raptor's position: **fault domains should match feature domains, and
the only boundary the kernel enforces is the process.** The vendor SDK
is closed, stateful, and not thread-safe, and it misbehaves in ways
nobody can patch. In a monolith, an SDK wedge takes RTSP, recording,
and motion down with it. In Raptor, exactly two daemons link the HAL;
nothing that parses network input shares an address space with the
hardware, and nothing touching the hardware has a socket open to the
world.

Nor is this "microservices" with serialization taxes: daemons share
lock-free SHM rings, zero-copy in reference mode, so a consumer reads
the same physical frame the encoder produced. A slow client overflows
only its own reader; the encode path never blocks on anyone. Process
isolation at roughly the IPC cost of threads.

The consequences are operational: a WebRTC crash cannot stop a
recording, one daemon can be swapped on a live camera, leaks are
per-daemon numbers, unused features never load, and every seam is
inspectable (`ringdump`, control sockets, per-daemon logs). Because
each daemon is a small program with a narrow contract, the whole
system builds on x86 with a mock HAL and runs under ASAN/TSAN,
fuzzers, and an external conformance battery. A monolith with SDK
calls threaded through it cannot even compile off the camera.

Why not something else? Monoliths are shared fate: one deadlock is a
dead camera. Threads share a heap, so they share corruption, and the
SDK forces serialization anyway. Desktop media frameworks are sized
for desktops, not 32 KB MIPS caches, and are still one failure
domain. Generic restreamers (go2rtc, MediaMTX) are relays, not
sources: no ISP, no encoder. Raptor feeds them; it does not compete
with them.

None of this is novel. It is the Unix process model, the oldest
reliability mechanism there is, applied to a domain that habitually
skips it.

## Architecture

Each daemon runs as a separate process. Producers (RVD, RAD, or RFS) publish
encoded frames to SHM rings; all other daemons are pure consumers or support
services. On camera platforms, RVD owns the ISP/encoder hardware. On platforms
without ISP (A1, x86 testing), RFS replaces RVD+RAD by reading from files.

Raptor is fully *modular* -- **install and run only the daemons your application requires**.
A headless recorder might run just RVD and RMR. A cloud-connected doorbell
might run RVD, RSD, RAD, RIC, and ROD. A minimal RTSP-only camera needs
just RVD and RSD. On an A1 media processor, RFS + RSD streams video from
MP4 files. Each daemon starts independently and discovers available ring
buffers at runtime, gracefully skipping any that don't exist.

```
 sensor                              .mp4 / .h264 file
   |                                        |
  [RVD] --shm rings--+              [RFS] --shm rings--+
   |  \               |                |               |
   |   \              +--> [RSD] RTSP/RTSPS server (via compy)
   |   \              +--> [RSD-555] RTSP server (via live555)
   |    \             +--> [RHD] HTTP snapshots / MJPEG / audio
   |     \            +--> [RMR] fragmented MP4 recording
   |      \           +--> [RWD] WebRTC/WHIP server (DTLS-SRTP)
   |       \          +--> [RWC] USB webcam (UVC + UAC1)
   |       \          +--> [RSP] RTMP/RTMPS push (YouTube, Twitch)
   |       \          +--> [RSR] SRT listener (MPEG-TS)
   |        `--osd shm <-- [ROD] OSD text / logo renderer
   |        `--ivs ------> [RMD] motion detection → triggers RMR
   |
  [RAD] --audio ring--+--> (all consumers above)
   |
  [RIC] ---- ctrl sock --> [RVD] (exposure queries, ISP mode switch)
```

### Daemons

| Name | Binary | Description |
|------|--------|-------------|
| RVD  | `rvd`  | Raw Video Daemon. Initializes HAL, configures sensor and encoder channels, creates SHM ring buffers (`main`, `sub`, `jpeg0`, `jpeg1`), and runs the frame acquisition loop. Exposes ISP controls and encoder tuning via its control socket. |
| RSD  | `rsd`  | RTSP Streaming Daemon. Serves RTSP/RTSPS (Digest auth, TLS via mbedTLS) from the video/audio rings using compy; video+audio, video-only, or audio-only sessions, with audio interleaved during IDR delivery so large keyframes never starve it. Optional per-frame MISB ST 0604 UTC timecode SEI, MJPEG endpoints (`/jpeg`, `/jpeg_sub`), and ONVIF Profile T audio backchannel into the `speaker` ring. |
| RSD-555 | `rsd-555` | Alternative RTSP server built on live555, statically linked. Same `[rtsp]` config (port override via `[rtsp-555]`), H.264/H.265 plus all five audio codecs, per-client refcounted fan-out. Runs alongside or instead of RSD. |
| RAD  | `rad`  | Raw Audio Daemon. Captures PCM, encodes through pluggable codecs (G.711 mu/A-law, L16, AAC, Opus) into the `audio` ring, and drives speaker output from the `speaker` ring. Optional noise suppression, HPF, AGC. A new codec is one source file. |
| ROD  | `rod`  | OSD Rendering Daemon. Renders timestamp, uptime, user text, and logo bitmaps into BGRA SHM double-buffers using libschrift. No HAL dependency -- RVD handles the hardware OSD regions. |
| RHD  | `rhd`  | HTTP Streaming Daemon. JPEG snapshots (`/snap`), MJPEG (`/mjpeg`), and audio (`/audio`) straight from the rings, with proper container framing (WAV, ADTS, Ogg). Dual-stack IPv4/IPv6, Basic auth, optional HTTPS. Optional EXIF capture times and Ed25519-signed snapshots. |
| RIC  | `ric`  | IR-Cut Controller. Hybrid luma+gain day/night detection: ae_luma for day-to-night, auto-calibrating gain-ratio for night-to-day (prevents IR flip-flop). Pins from raptor.conf or auto-discovered from `/etc/thingino.json`; single and dual-GPIO filters. |
| RMR  | `rmr`  | Recording/Muxing Daemon. Writes crash-safe fragmented MP4 segments (H.264/H.265/MJPEG + audio) to SD with its own dependency-free fMP4 muxer. Optional MISB ST 0604 UTC timecodes and Ed25519 hash-chain provenance signing. |
| RMD  | `rmd`  | Motion Detection Daemon. Consumes RVD's IVS results (hardware motion grid, JZDL YOLOv5 inference on NNA), runs the idle/active/cooldown state machine, and triggers RMR plus GPIO outputs. Configurable ROI, per-class filtering, external detection push via control socket. |
| RWD  | `rwd`  | WebRTC Daemon. Live H.264 + Opus to browsers and go2rtc via WHIP with sub-second latency: ICE-lite, DTLS-SRTP, two-way audio (browser mic to camera speaker). Embedded player at `/webrtc`; optional WebTorrent sharing for external viewing without port forwarding. |
| RWC  | `rwc`  | USB Webcam Daemon. Presents the camera as a standard UVC + UAC1 USB webcam (MJPEG or H.264 at 1080p/720p/360p, 16kHz mono mic) through the kernel gadget. Bulk video endpoint, so it works through USB hubs. |
| RFS  | `rfs`  | File Source Daemon. Plays MP4/MOV or raw Annex B H.264/H.265 files into the rings at real-time rate, replacing RVD+RAD where there is no ISP (A1, x86 testing). Zero-copy MP4 demux, B-frame reorder, audio passthrough or transcode. Pause/resume/seek via control socket. |
| RSP  | `rsp`  | Stream Push Daemon. Pushes video + audio to RTMP/RTMPS endpoints (YouTube Live, Twitch, custom) with its own RTMP client; H.264 via standard FLV, H.265 via Enhanced RTMP FourCC. Ring audio is transcoded to AAC-LC as needed, native AAC passes through. Auto-reconnect with backoff. |
| RSR  | `rsr`  | SRT Listener Daemon. Live video + audio as MPEG-TS over SRT: multi-client, stream selection via STREAMID, AES encryption. Audio must be AAC or Opus (G.711/L16 do not exist in MPEG-TS). Works with ffplay, VLC, OBS, go2rtc, and SRT-capable NVRs. |

Feature-to-build-flag mapping (`TLS=1`, `AAC=1`, `OPUS=1`, `WEBTORRENT=1`) is
under [Build](#build). RWC additionally needs the thingino kernel's webcam
gadget patches (`CONFIG_USB_G_WEBCAM=m`).

### Tools

| Name | Binary | Description |
|------|--------|-------------|
| raptorctl  | `raptorctl`  | Management CLI. Query daemon status, read/write config values, and send runtime commands (bitrate, GOP, ISP parameters, OSD text, day/night mode, etc.) over control sockets. |
| ringdump   | `ringdump`   | Ring buffer debugger. Print ring header, follow per-frame metadata, dump raw Annex B to stdout, or measure pipeline latency (`-l`). |
| rac        | `rac`        | Audio client. Record mic input to file/stdout (PCM16 LE) or play back audio (PCM, MP3, AAC, Opus) to the speaker ring. |
| rlatency   | `rlatency`   | RTSP end-to-end latency measurement. Uses RTCP Sender Report NTP/RTP correlation (RFC 3550) to compute per-frame latency with percentile stats. Runs on host, not camera. |
| rverify    | `rverify`    | Signed-recording verifier. Checks the Ed25519 hash chain in RMR recordings against a device public key (`-k`), reports tamper location, clean close, and power-loss prefixes; `-t`/`-T` summarize or dump the embedded MISB ST 0604 UTC timecodes. Also verifies RHD-signed JPEG snapshots and reports their EXIF capture time. Builds for host and camera. |

## Related Repositories

| Repository | Description |
|-----------|-------------|
| [raptor-docs](https://github.com/gtxaspec/raptor-docs) | Architecture docs, design notes, and API reference. |
| [raptor-hal](https://github.com/gtxaspec/raptor-hal) | Hardware abstraction layer -- wraps Ingenic IMP SDK calls behind a unified API across SDK generations. |
| [raptor-ipc](https://github.com/gtxaspec/raptor-ipc) | SHM ring buffers, OSD double-buffer SHM, and Unix domain control socket protocol. |
| [raptor-common](https://github.com/gtxaspec/raptor-common) | Config parser, logging, daemonize, signal handling, timestamp utilities. |
| [compy](https://github.com/gtxaspec/compy) | RTSP/RTP server library (used by RSD). |
| [raptor-test](https://github.com/gtxaspec/raptor-test) | Black-box conformance battery: point it at a live camera and it exercises RTSP/RTP, codecs, timestamps, auth, and provenance. |
| [thingino-verify](https://github.com/thingino/thingino-verify) | Browser verifier for signed recordings and snapshots (rverify compiled to WebAssembly), live at [verify.thingino.com](https://verify.thingino.com). |
| [live555](http://www.live555.com/) | RTSP/RTP server library (used by RSD-555, statically linked). |

## Dependencies

Raptor is built against the above libraries and the Ingenic vendor SDK.

Runtime shared libraries from the Ingenic SDK / Buildroot sysroot:

- `libimp` -- Ingenic multimedia platform
- `libalog` -- Ingenic logging
- `libschrift` -- TrueType font rasterizer (ROD)
- `libfaac` -- AAC encoder (optional, `AAC=1`)
- `libhelix-aac` / `libhelix-mp3` -- AAC/MP3 decoders (optional, for rac playback)
- `libopus` -- Opus codec (optional, `OPUS=1`)
- `libaudioProcess` -- Ingenic audio effects (optional, `AUDIO_EFFECTS=1`)
- `libmbedtls` / `libmbedcrypto` / `libmbedx509` -- TLS/DTLS (optional, `TLS=1`, required for RTSPS and WebRTC)
- `libsrt` -- SRT protocol (optional, statically linked into RSR, built from source by `build-standalone.sh`)
- `libliveMedia` / `libgroupsock` / `libUsageEnvironment` / `libBasicUsageEnvironment` -- live555 RTSP (optional, statically linked by RSD-555)
- `libmov` -- MP4 demuxer (statically compiled into RFS from [ireader/media-server](https://github.com/ireader/media-server), MIT license)

## Build

### Standalone build (no Buildroot)

```sh
./build-standalone.sh t31                # downloads toolchain + deps, builds everything
./build-standalone.sh t31 --local        # use sibling repo checkouts instead of cloning
./build-standalone.sh t31 --static       # static binaries
./build-standalone.sh t31 --clean        # clean all build artifacts
```

Supported platforms: `t10`, `t20`, `t21`, `t23`, `t30`, `t31`, `t32`, `t33`, `t40`, `t41`, `a1`.

First run downloads the toolchain and all dependencies automatically.
Output binaries go to `build/`. Options: `--no-tls`, `--no-aac`,
`--no-opus`, `--no-mp3`, `--no-audio-effects`, `--alt` (jz-crypto HW
accel), `--libc=musl|uclibc|glibc`.

### Manual build

```sh
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- SYSROOT=/path/to/sysroot
```

Required variables:

- `PLATFORM` -- target SoC: `T10`, `T20`, `T21`, `T23`, `T30`, `T31`, `T32`, `T33`, `T40`, `T41`, `A1`
- `CROSS_COMPILE` -- toolchain prefix (e.g., `mipsel-linux-`)

Optional variables:

- `SYSROOT` -- path to Buildroot sysroot for library linking
- `DEBUG=1` -- build with `-O0 -g` instead of `-Os`
- `AAC=1` -- enable AAC encode/decode support
- `MP3=1` -- enable MP3 decode support (rac playback)
- `OPUS=1` -- enable Opus encode/decode support
- `AUDIO_EFFECTS=1` -- enable noise suppression, HPF, AGC
- `TLS=1` -- enable RTSPS (TLS-encrypted RTSP via mbedTLS) and WebRTC (RWD)
- `WEBTORRENT=1` -- enable WebTorrent external sharing in RWD (requires `TLS=1`)
- `V=1` -- verbose build output

Install to a staging directory:

```sh
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- DESTDIR=/tmp/raptor install
```

This installs binaries to `$DESTDIR/usr/bin/`, the config to `$DESTDIR/etc/raptor.conf`,
and the init script to `$DESTDIR/etc/init.d/S31raptor`. The script starts RVD
first (it creates the rings), then the rest; each daemon checks its own
`enabled` config flag and exits cleanly if disabled.

## Configuration

All daemons share a single INI-style config file: `/etc/raptor.conf`.
See [raptor-docs/23-rss-config.md](https://github.com/gtxaspec/raptor-docs/blob/main/23-rss-config.md)
for the complete reference (all sections, keys, types, defaults).

Runtime changes via `raptorctl`:

```sh
raptorctl status                      # show running daemons
raptorctl rvd set-bitrate 0 2000000   # change encoder bitrate at runtime
raptorctl rvd save jpeg /tmp/snap.jpg # one-shot snapshot (also: raw = NV12, bayer = sensor RAW)
raptorctl config save                 # persist running config to disk
raptorctl rvd                         # show all RVD commands
```

## Ring Reconnection

All consumer daemons (RSD, RWD, RMR, RHD, RSP) automatically reconnect to both
video and audio ring buffers after RVD or RAD restarts. If a ring producer
stops writing for ~2 seconds, consumers close the stale ring and retry until
the new ring appears. No manual daemon restart required.

## Ring Reference Mode (Zero-Copy)

When `refmode` is enabled, video ring buffers become metadata-only (~9KB per
stream instead of ~1.5MB). The encoder writes compressed output directly to a
shared backing store that consumers mmap — eliminating the per-frame memcpy
from encoder DMA buffer into the ring data region.

Two backing store paths are selected automatically based on the encoder IP:

| SoCs | Method | Backing store |
|------|--------|---------------|
| T10-T30, T32, T33 | POSIX SHM injection | Named SHM (`/rss_enc_<stream>`) |
| T31, T40, T41 | rmem zero-copy | `/dev/rmem` mmap |

Enable in config:

```ini
[ring]
refmode = true
```

Consumers detect refmode transparently via ring header flags — no consumer
code changes required. JPEG snapshots stay embedded (not affected by refmode).

## Stream Timecodes and Signed Recordings

Optional provenance features (all ship disabled in the sample config):

- **Per-frame UTC timecodes** — every H.264/H.265 frame carries a MISB
  ST 0604 Precision Time Stamp SEI: absolute capture time in microseconds
  plus an NTP-lock status byte. Enable with `[rtsp] sei_timecode = true`
  (live streams; survives NVR-side copy recording) and/or
  `[recording] sei_timecode = true` (RMR files, including pre-buffered
  motion footage with its true capture time). ~36 bytes/frame; standard
  players skip it, forensic tools parse it.

- **Signed recordings** — `[recording] sign = true` makes RMR sign every
  fMP4 with a per-device Ed25519 key: a hash chain of `uuid` boxes covering
  the init segment, every fragment, and a final clean-close marker. Any
  edit, removed frame, splice, or appended footage breaks the chain;
  power-cut files verify up to the last complete fragment. The key is
  generated on first use (`[recording] sign_key`, default
  `/etc/raptor/sign_ed25519.key`); export the public key with
  `raptorctl rmr export-pubkey` and verify clips anywhere with
  `rverify -k <pubkey> file.mp4` — offline, no cloud. This is signing,
  not encryption: recordings stay ordinary playable MP4s.

- **JPEG capture times and signed snapshots** — the same two ideas for
  stills. `[http] exif_timestamp = true` embeds the frame capture time
  (UTC, microsecond precision) as standard EXIF in `/snap.jpg` and every
  MJPEG stream frame. `[http] sign_snapshots = true` appends an Ed25519
  signature to `/snap.jpg` using the same device key as RMR; verify with
  `rverify snapshot.jpg -k <pubkey>`. Snapshots stay ordinary JPEGs.

Timecodes cost no measurable CPU; signing costs roughly 2% CPU per Mbps
recorded on the slowest supported SoC. Details: `20-rss-architecture.md`
and `23-rss-config.md` in raptor-docs.


## Latency

Server-side pipeline latency (sensor capture → ring availability) is ~2ms
average, measured with `ringdump main -l`. The full end-to-end breakdown:

| Stage | Latency |
|-------|---------|
| Sensor → ISP → Encoder → Ring | ~2ms (measured) |
| Ring → RTP packetization | <1ms |
| Network (wired LAN) | ~2ms |
| **Server total** | **~5ms** |
| WebRTC client (browser) | ~50ms |
| RTSP client jitter buffer | 100-500ms (client-dependent) |

### Low-latency mode

Enable in config for encoder immediate frame output:

```ini
[sensor]
low_latency = true
```

### RTSP client tuning

Most RTSP latency is the client's jitter buffer; use UDP transport:

```sh
ffplay -fflags nobuffer -flags low_delay -rtsp_transport udp rtsp://camera/stream0
mpv --no-cache --untimed --profile=low-latency rtsp://camera/stream0
vlc --network-caching=0 rtsp://camera/stream0
```

WebRTC (via RWD) needs none of this: browsers buffer minimally (~50ms total).

### Measuring

```sh
ringdump main -l -n 100        # on-device: sensor-to-ring, per-frame min/avg/max
rlatency rtsp://cam/stream0    # any host: end-to-end via RTCP SR correlation
```

`rlatency` maps each received frame to the camera's wall clock
(RFC 3550 Section 6.4.1) and reports min/avg/max/stddev/P50/P95/P99;
clocks must be NTP-synced.

## Supported Platforms

| SoC | SDK Generation | Status |
|-----|---------------|--------|
| T10 | Old (IMP v1)  | Supported (uses T20 SDK) |
| T20 | Old (IMP v1)  | Supported |
| T21 | Old (IMP v1)  | Supported |
| T23 | Old (IMP v1)  | Supported |
| T30 | Old (IMP v1)  | Supported |
| T31 | New (IMP v2)  | Primary target |
| T32 | New (IMP v2)  | Supported |
| T33 | New (IMP v2)  | Supported (T32-compatible) |
| T40 | IMPVI         | Supported |
| T41 | IMPVI         | Supported |
| A1  | VDEC/VENC     | RFS only (no ISP HAL) |

Platform differences are handled by raptor-hal, which abstracts the three SDK
generations behind a common API. The `PLATFORM` build variable selects the
correct HAL backend at compile time. On A1, HAL is not built — RFS serves as
the video/audio producer instead of RVD+RAD.

## License

Licensed under the GNU General Public License v3.0.
