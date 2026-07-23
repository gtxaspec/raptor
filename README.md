# Raptor Streaming System (RSS)

A modular microservice camera streamer for Ingenic SoCs. Raptor replaces the
traditional monolithic streamer with independent daemons that communicate
through POSIX shared-memory ring buffers and Unix domain control sockets.

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
| RSD  | `rsd`  | RTSP Streaming Daemon. Reads video/audio rings and serves RTSP/RTP streams using the compy library. Supports video+audio, video-only, or audio-only sessions. Supports Digest authentication and RTSPS (TLS via mbedTLS, compile with `TLS=1`). Audio interleaving during IDR delivery prevents large keyframes from starving audio. Optional per-frame MISB ST 0604 UTC timecode SEI (`[rtsp] sei_timecode`) — survives client-side copy recording (NVRs, ffmpeg). |
| RSD-555 | `rsd-555` | Alternative RTSP server using live555 instead of compy. Statically linked — no live555 shared libraries needed on device. Supports H.264/H.265 video, all five audio codecs (PCMU, PCMA, L16, AAC, Opus), Digest auth, per-client refcounted frame queues with fan-out. Reads the same `[rtsp]` config section as RSD (with `[rtsp-555] port` override). Can run alongside or instead of RSD on a different port. |
| RAD  | `rad`  | Raw Audio Daemon. Captures PCM from the ISP audio input, encodes via pluggable codec (G.711 mu-law/A-law, L16, AAC, Opus), and publishes to the `audio` ring. Also handles speaker output via a `speaker` ring. Supports noise suppression, HPF, and AGC when libaudioProcess is available. Codec plugins are modular — adding a new codec requires one source file. |
| ROD  | `rod`  | OSD Rendering Daemon. Renders timestamp, uptime, user text, and logo bitmaps into BGRA SHM double-buffers using libschrift. No HAL dependency -- RVD handles the hardware OSD regions. |
| RHD  | `rhd`  | HTTP Streaming Daemon. Serves JPEG snapshots (`/snap`), MJPEG streams (`/mjpeg`), and audio streams (`/audio`) from SHM rings. Audio is served with proper container framing: WAV for PCM/G.711, ADTS for AAC, Ogg for Opus. Dual-stack IPv4/IPv6, Basic auth. Optional HTTPS via mbedTLS (`[http] https = true`). Optional EXIF capture times in snapshots/MJPEG frames and Ed25519-signed snapshots (verify with `rverify`). |
| RIC  | `ric`  | IR-Cut Controller. Hybrid luma+gain day/night detection: ae_luma for day→night (sensor-independent), gain-ratio for night→day (auto-calibrating, prevents IR flip-flop). Auto-discovers GPIOs from `/etc/thingino.json`. Supports single and dual-GPIO IR-cut filters. |
| RMR  | `rmr`  | Recording/Muxing Daemon. Reads H.264/H.265/MJPEG + audio from rings and writes crash-safe fragmented MP4 segments to SD card. Own fMP4 muxer with zero external dependencies. Optional per-frame MISB ST 0604 UTC timecodes and Ed25519 hash-chain provenance signing (verify with `rverify`). |
| RMD  | `rmd`  | Motion Detection Daemon. Queries RVD for IVS results (hardware motion grid, JZDL YOLOv5 person/object inference on NNA), manages idle/active/cooldown state machine, triggers recording via RMR and GPIO output on detection events. Supports configurable ROI, per-class filtering, and external detection engine push via control socket. |
| RWD  | `rwd`  | WebRTC Daemon. Sends live H.264 + Opus to browsers and go2rtc via WHIP signaling with sub-second latency. ICE-lite, DTLS-SRTP (mbedTLS), SRTP (compy). Two-way audio backchannel (browser mic → camera speaker via Opus decode). HTTPS by default for signaling (enables `getUserMedia` Talk button). Embedded player at `/webrtc`. Optional WebTorrent sharing (`WEBTORRENT=1`) enables external viewing without port forwarding via public tracker signaling + STUN NAT traversal. Requires `TLS=1` and `MBEDTLS_SSL_DTLS_SRTP`. |
| RWC  | `rwc`  | USB Webcam Daemon. Reads JPEG (or H.264) video from rings and raw PCM audio, feeds them to the Linux UVC+UAC gadget via V4L2 and `/dev/uac_mic`. Camera appears as a standard USB webcam with microphone on any connected host. MJPEG + H.264 at 1080p/720p/360p, 16kHz mono mic. Bulk video endpoint (works through USB hubs), isochronous audio. No ALSA dependency — custom minimal UAC1 kernel function. Requires `CONFIG_USB_G_WEBCAM=m` and the thingino kernel webcam patches. |
| RFS  | `rfs`  | File Source Daemon. Reads video+audio from MP4/MOV containers or raw Annex B H.264/H.265 files, publishes to ring buffers at real-time rate. Replaces RVD+RAD on platforms without ISP/encoder hardware (A1, x86 testing). MP4 demuxing via libmov (zero-copy mmap, AVCC→Annex B on-the-fly). B-frame display reorder for raw files. Audio: direct passthrough for AAC/Opus/G.711, MP3 transcode via libhelix, raw PCM encoding via RAD codec plugins (L16/PCMU/PCMA/AAC/Opus). Control socket: status, pause/resume, seek. No HAL dependency. |
| RSP  | `rsp`  | Stream Push Daemon. Reads H.264/H.265 video + audio from SHM rings and pushes to RTMP/RTMPS servers (YouTube Live, Twitch, Facebook Live, custom endpoints). Custom RTMP client with AMF0 encoding, chunk stream framing, and FLV tag construction. H.264 via standard FLV, H.265 via Enhanced RTMP FourCC. Audio transcode: any ring codec (G.711 µ/A-law, L16, Opus) is decoded to PCM and re-encoded to AAC-LC via faac; native AAC is passed through. Zero-copy ring peek in refmode. RTMPS via mbedTLS client-side TLS. Auto-reconnect with configurable backoff. Requires `TLS=1` for RTMPS, `AAC=1` for audio transcode. |
| RSR  | `rsr`  | SRT Listener Daemon. Serves live H.264/H.265 video + audio as MPEG-TS over SRT protocol (ISO 13818-1 compliant muxer). Multi-client with per-client TS state, multi-stream via SRT STREAMID routing (clients select main/sub/sensor streams). AES-128/192/256 encryption via libsrt+mbedTLS. Audio: AAC (with ADTS framing) and Opus (with registration descriptor); G.711/L16 not supported in MPEG-TS — configure RAD to use AAC or Opus for SRT audio. Dual-stack IPv4/IPv6. Compatible with ffplay, VLC, OBS, go2rtc, and any SRT-capable player or NVR. |

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
and the init script to `$DESTDIR/etc/init.d/S31raptor`.

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

## Init Script

A reference init script is included at `config/S31raptor`. RVD must start
first (creates SHM rings); all other daemons start after the ring appears.
Each daemon checks its own `enabled` config flag and exits cleanly if disabled.

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

Most RTSP latency comes from the client's jitter buffer. Recommended
player settings for lowest latency (use UDP transport):

```sh
# ffplay
ffplay -fflags nobuffer -flags low_delay -rtsp_transport udp rtsp://camera/stream0

# mpv
mpv --no-cache --untimed --profile=low-latency rtsp://camera/stream0

# VLC
vlc --network-caching=0 rtsp://camera/stream0
```

WebRTC (via RWD) has inherently low latency (~50ms total) since browsers
use minimal jitter buffering.

### Measuring latency

**On-device** (pipeline only — sensor to ring):
```sh
ringdump main -l           # pipeline latency (per-frame)
ringdump main -l -n 100    # measure 100 frames, show min/avg/max
ringdump audio -l           # audio pipeline latency
```

**End-to-end** (sensor to network client, includes network):
```sh
# Run from any host on the network (requires NTP-synced clocks)
rlatency rtsp://camera/stream0              # continuous measurement
rlatency rtsp://camera/stream0 -n 500       # 500 frames with summary
rlatency rtsp://camera/stream0 -n 200 -v    # verbose per-frame output
```

`rlatency` uses RTCP Sender Report NTP/RTP timestamp correlation
(RFC 3550 §6.4.1) to map each received frame to the camera's wall
clock. Reports min/avg/max/stddev/P50/P95/P99.

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
