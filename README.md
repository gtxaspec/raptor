# Raptor Streaming System (RSS)

A modular microservice camera streamer for Ingenic T-series SoCs. Raptor
replaces the traditional monolithic streamer with independent daemons that
communicate through POSIX shared-memory ring buffers and Unix domain control
sockets.

## Architecture

Each daemon runs as a separate process. RVD owns the hardware and publishes
encoded frames to SHM rings; all other daemons are pure consumers or support
services with no direct HAL dependency (except RAD for audio capture and RIC
for ISP exposure queries via RVD's control socket).

Raptor is fully modular -- run only the daemons your application requires.
A headless recorder might run just RVD and RMR. A cloud-connected doorbell
might run RVD, RSD, RAD, RIC, and ROD. A minimal RTSP-only camera needs
just RVD and RSD. Each daemon starts independently and discovers available
ring buffers at runtime, gracefully skipping any that don't exist.

```
 sensor
   |
  [RVD] --shm rings--> [RSD] RTSP/RTSPS server (via compy)
   |  \                 [RHD] HTTP snapshots / MJPEG
   |   \                [RMR] fragmented MP4 recording
   |    \               [RWD] WebRTC/WHIP server (DTLS-SRTP via mbedTLS + compy)
   |     \              [RWC] USB webcam (UVC bulk + UAC1 mic)
   |      `--osd shm <-- [ROD] OSD text / logo renderer
   |      `--ivs ------> [RMD] motion detection → triggers RMR
   |
  [RAD] --audio ring--> [RSD] (interleaved A/V)
   |                    [RMR] (muxed A/V)
   |                    [RWD] (WebRTC A/V)
   |                    [RWC] (USB mic via /dev/uac_mic)
   |
  [RIC] ---- ctrl sock --> [RVD] (exposure queries, ISP mode switch)
```

### Daemons

| Name | Binary | Description |
|------|--------|-------------|
| RVD  | `rvd`  | Raw Video Daemon. Initializes HAL, configures sensor and encoder channels, creates SHM ring buffers (`main`, `sub`, `jpeg0`, `jpeg1`), and runs the frame acquisition loop. Exposes ISP controls and encoder tuning via its control socket. |
| RSD  | `rsd`  | RTSP Streaming Daemon. Reads video/audio rings and serves RTSP/RTP streams using the compy library. Supports video+audio, video-only, or audio-only sessions. Supports Digest authentication and RTSPS (TLS via mbedTLS, compile with `TLS=1`). |
| RAD  | `rad`  | Raw Audio Daemon. Captures PCM from the ISP audio input, encodes via pluggable codec (G.711 mu-law/A-law, L16, AAC, Opus), and publishes to the `audio` ring. Also handles speaker output via a `speaker` ring. Supports noise suppression, HPF, and AGC when libaudioProcess is available. Codec plugins are modular — adding a new codec requires one source file. |
| ROD  | `rod`  | OSD Rendering Daemon. Renders timestamp, uptime, user text, and logo bitmaps into BGRA SHM double-buffers using libschrift. No HAL dependency -- RVD handles the hardware OSD regions. |
| RHD  | `rhd`  | HTTP Streaming Daemon. Serves JPEG snapshots (`/snap`), MJPEG streams (`/mjpeg`), and audio streams (`/audio`) from SHM rings. Audio is served with proper container framing: WAV for PCM/G.711, ADTS for AAC, Ogg for Opus. Dual-stack IPv4/IPv6, Basic auth. Optional HTTPS via mbedTLS (`[http] https = true`). |
| RIC  | `ric`  | IR-Cut Controller. Hybrid luma+gain day/night detection: ae_luma for day→night (sensor-independent), gain-ratio for night→day (auto-calibrating, prevents IR flip-flop). Auto-discovers GPIOs from `/etc/thingino.json`. Supports single and dual-GPIO IR-cut filters. |
| RMR  | `rmr`  | Recording/Muxing Daemon. Reads H.264/H.265 + audio from rings and writes crash-safe fragmented MP4 segments to SD card. Own fMP4 muxer with zero external dependencies. |
| RMD  | `rmd`  | Motion Detection Daemon. Queries RVD for IVS hardware motion results (configurable grid ROI), manages idle/active/cooldown state machine, triggers recording via RMR and GPIO output on motion events. |
| RWD  | `rwd`  | WebRTC Daemon. Sends live H.264 + Opus to browsers and go2rtc via WHIP signaling with sub-second latency. ICE-lite, DTLS-SRTP (mbedTLS), SRTP (compy). Two-way audio backchannel (browser mic → camera speaker via Opus decode). HTTPS by default for signaling (enables `getUserMedia` Talk button). Embedded player at `/webrtc`. Optional WebTorrent sharing (`WEBTORRENT=1`) enables external viewing without port forwarding via public tracker signaling + STUN NAT traversal. Requires `TLS=1` and `MBEDTLS_SSL_DTLS_SRTP`. |
| RWC  | `rwc`  | USB Webcam Daemon. Reads JPEG (or H.264) video from rings and raw PCM audio, feeds them to the Linux UVC+UAC gadget via V4L2 and `/dev/uac_mic`. Camera appears as a standard USB webcam with microphone on any connected host. MJPEG + H.264 at 1080p/720p/360p, 16kHz mono mic. Bulk video endpoint (works through USB hubs), isochronous audio. No ALSA dependency — custom minimal UAC1 kernel function. Requires `CONFIG_USB_G_WEBCAM=m` and the thingino kernel webcam patches. |

### Tools

| Name | Binary | Description |
|------|--------|-------------|
| raptorctl  | `raptorctl`  | Management CLI. Query daemon status, read/write config values, and send runtime commands (bitrate, GOP, ISP parameters, OSD text, day/night mode, etc.) over control sockets. |
| ringdump   | `ringdump`   | Ring buffer debugger. Print ring header, follow per-frame metadata, dump raw Annex B to stdout, or measure pipeline latency (`-l`). |
| rac        | `rac`        | Audio client. Record mic input to file/stdout (PCM16 LE) or play back audio (PCM, MP3, AAC, Opus) to the speaker ring. |
| rlatency   | `rlatency`   | RTSP end-to-end latency measurement. Uses RTCP Sender Report NTP/RTP correlation (RFC 3550) to compute per-frame latency with percentile stats. Runs on host, not camera. |

## Related Repositories

| Repository | Description |
|-----------|-------------|
| [raptor-hal](https://github.com/gtxaspec/raptor-hal) | Hardware abstraction layer -- wraps Ingenic IMP SDK calls behind a unified API across SDK generations. |
| [raptor-ipc](https://github.com/gtxaspec/raptor-ipc) | SHM ring buffers, OSD double-buffer SHM, and Unix domain control socket protocol. |
| [raptor-common](https://github.com/gtxaspec/raptor-common) | Config parser, logging, daemonize, signal handling, timestamp utilities. |
| [compy](https://github.com/gtxaspec/compy) | RTSP/RTP server library (used by RSD). |
| [raptor-docs](https://github.com/gtxaspec/raptor-docs) | Architecture docs, design notes, and API reference. |

## Dependencies

Raptor is built against the above libraries and the Ingenic vendor SDK.

Runtime shared libraries from the Ingenic SDK / Buildroot sysroot:

- `libimp` -- Ingenic multimedia platform
- `libalog` -- Ingenic logging
- `libsysutils` -- Ingenic system utilities
- `libschrift` -- TrueType font rasterizer (ROD)
- `libfaac` -- AAC encoder (optional, `AAC=1`)
- `libhelix-aac` / `libhelix-mp3` -- AAC/MP3 decoders (optional, for rac playback)
- `libopus` -- Opus codec (optional, `OPUS=1`)
- `libaudioProcess` -- Ingenic audio effects (optional, `AUDIO_EFFECTS=1`)
- `libmbedtls` / `libmbedcrypto` / `libmbedx509` -- TLS/DTLS (optional, `TLS=1`, required for RTSPS and WebRTC)

## Build

Raptor cross-compiles with a MIPS toolchain from a thingino Buildroot output.

### Quick build (using build.sh)

```sh
./build.sh t31 /path/to/buildroot/output              # full distclean + rebuild
./build.sh t31 /path/to/buildroot/output rvd rsd       # rebuild specific targets
./build.sh t31 /path/to/buildroot/output clean          # clean only
```

Supported platforms: `t20`, `t21`, `t23`, `t30`, `t31`, `t32`, `t40`, `t41`.

`build.sh` takes the Buildroot output directory as the second argument. It
auto-detects the sysroot tuple and TLS support (mbedTLS).

### Manual build

```sh
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- SYSROOT=/path/to/sysroot
```

Required variables:

- `PLATFORM` -- target SoC: `T20`, `T21`, `T23`, `T30`, `T31`, `T32`, `T40`, `T41`
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

Build individual targets:

```sh
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- rvd ringdump raptorctl
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- build   # collect binaries into build/
```

Install to a staging directory:

```sh
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- DESTDIR=/tmp/raptor install
```

This installs binaries to `$DESTDIR/usr/bin/`, the config to `$DESTDIR/etc/raptor.conf`,
and the init script to `$DESTDIR/etc/init.d/S31raptor`.

## Configuration

All daemons share a single INI-style config file: `/etc/raptor.conf`.

Sections: `[sensor]`, `[stream0]`, `[stream1]`, `[jpeg]`, `[ring]`, `[audio]`,
`[rtsp]`, `[http]`, `[osd]`, `[ircut]`, `[recording]`, `[webrtc]`, `[webtorrent]`, `[motion]`, `[log]`.

See `config/raptor.conf` for the full reference with defaults and comments.

Runtime configuration changes can be made without restart via raptorctl:

```sh
raptorctl status                      # show which daemons are running
raptorctl memory                      # per-daemon memory usage
raptorctl cpu                         # per-daemon CPU usage (1s sample)
raptorctl rvd                         # show RVD commands (works for any daemon)

# Encoder (basic)
raptorctl rvd set-bitrate 0 3000000   # change main stream bitrate
raptorctl rvd set-gop 0 50            # change GOP length
raptorctl rvd set-fps 0 25            # change frame rate
raptorctl rvd set-rc-mode 0 vbr       # rate control: fixqp/cbr/vbr/smart/capped_vbr/capped_quality
raptorctl rvd set-qp-bounds 0 15 45   # QP range
raptorctl rvd request-idr             # force keyframe

# Encoder (advanced — 40+ commands, SoC-dependent)
raptorctl rvd get-enc-caps            # show which features this SoC supports
raptorctl rvd set-gop-mode 0 2        # GOP mode (0=default 1=pyramidal 2=smartP)
raptorctl rvd set-roi 0 0 1 100 100 200 200 30  # ROI region
raptorctl rvd set-color2grey 0 1      # greyscale mode
raptorctl rvd get-bitrate 0           # query target + average bitrate

# ISP
raptorctl rvd set-brightness 128      # ISP brightness (0-255)
raptorctl rvd set-wb daylight         # white balance: auto/manual/daylight/cloudy/incandescent/
                                      #   flourescent/twilight/shade/warm_flourescent/custom
raptorctl rvd set-wb manual 200 300   # manual WB with r_gain/b_gain
raptorctl rvd get-isp                 # show all ISP settings
raptorctl rvd get-wb                  # show white balance

# OSD
raptorctl rod privacy on              # privacy mode (all streams)
raptorctl rod privacy on 0            # privacy mode (stream 0 only)
raptorctl rod set-text "Front Door"   # change OSD text
raptorctl rod set-font-color 0xFFFF0000    # text color (0xAARRGGBB)
raptorctl rod set-stroke-color 0xFF000000  # stroke color
raptorctl rod set-stroke-size 2            # stroke width (0-5)

# Audio
raptorctl rad set-volume 80           # input volume
raptorctl rad set-gain 25             # input gain
raptorctl rad set-alc-gain 5          # ALC PGA gain 0-7 (T21/T31 only)
raptorctl rad set-ns 1 moderate       # noise suppression (low/moderate/high/veryhigh)
raptorctl rad set-hpf 1               # high-pass filter
raptorctl rad set-agc 1 10 0          # AGC (target_level, compression)
raptorctl rad ao-set-volume 80        # output (speaker) volume
raptorctl rad ao-set-gain 25          # output (speaker) gain

# Other
raptorctl ric mode night              # force night mode
raptorctl rsd clients                 # list RTSP clients
raptorctl rhd clients                 # list HTTP clients
raptorctl rwd clients                 # list WebRTC clients
raptorctl rwd share                   # show WebTorrent share URL
raptorctl config get audio             # show all [audio] config keys
raptorctl config get rtsp port        # read single config value
raptorctl config set audio codec pcmu # change config value
raptorctl config save                 # persist running config to disk
```

## ISP OSD

On T23/T32/T40/T41, OSD can optionally use the ISP hardware overlay instead
of the IPU bind chain. ISP OSD works with IVDC (`direct_mode=2`) and has
zero DDR bandwidth overhead. Enable in config:

```ini
[osd]
isp_osd = true
```

ISP OSD applies to main streams only (T23 hardware limitation per Ingenic
docs). Sub streams do not receive ISP OSD. Each sensor supports up to 8
picture regions. Per-stream OSD can be disabled:

```ini
[stream1]
osd_enabled = false
```

IPU OSD remains the default and works on all platforms including
T20/T21/T30/T31 which do not have ISP OSD.

### Per-Stream JPEG

JPEG snapshots are enabled per-stream with per-stream quality and fps
overrides. Global defaults are in `[jpeg]`, per-stream settings in `[streamN]`:

```ini
[jpeg]
quality = 75
fps = 1

[stream0]
jpeg = true
jpeg_quality = 90         # higher quality for main

[stream1]
jpeg = false              # no JPEG for sub stream
```

### IVDC + JPEG

On T23 with IVDC enabled, full-resolution JPEG snapshots are not available
(SDK rejects JPEG+IVDC in the same encoder group). Sub-stream JPEG works
normally for lower-resolution snapshots. JPEG indices are preserved so
`jpeg0` (main) is simply absent while `jpeg1` (sub) remains accessible.

## Ring Reconnection

All consumer daemons (RSD, RWD, RMR, RHD) automatically reconnect to both
video and audio ring buffers after RVD or RAD restarts. If a ring producer
stops writing for ~2 seconds, consumers close the stale ring and retry until
the new ring appears. No manual daemon restart required.

## Init Script

The init script `config/S31raptor` (installed as `/etc/init.d/S31raptor`)
manages startup order:

1. **RVD** starts first and creates the SHM ring buffers.
2. The script waits (up to 5 seconds) for `/dev/shm/rss_ring_main` to appear.
3. **RAD**, **ROD**, **RSD**, **RHD**, **RMR**, **RIC**, **RMD**, and **RWD** start.
   Each daemon checks its own `enabled` flag in the config and exits cleanly
   if disabled, so all can be started unconditionally.
4. Shutdown reverses the order: consumers stop first, then RVD.

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
[stream0]
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
| T20 | Old (IMP v1)  | Supported |
| T21 | Old (IMP v1)  | Supported |
| T23 | Old (IMP v1)  | Supported |
| T30 | Old (IMP v1)  | Supported |
| T31 | New (IMP v2)  | Primary target |
| T32 | New (IMP v2)  | Supported |
| T40 | IMPVI         | Supported |
| T41 | IMPVI         | Supported |

Platform differences are handled by raptor-hal, which abstracts the three SDK
generations behind a common API. The `PLATFORM` build variable selects the
correct HAL backend at compile time.

## License

Licensed under the GNU General Public License v3.0.
