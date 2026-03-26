# Raptor Streaming System (RSS)

A modular microservice camera streamer for Ingenic T-series SoCs, built as part
of the [thingino](https://github.com/themactep/thingino-firmware) firmware
project. Raptor replaces the monolithic prudynt-t with independent daemons that
communicate through POSIX shared-memory ring buffers and Unix domain control
sockets.

## Architecture

Each daemon runs as a separate process. RVD owns the hardware and publishes
encoded frames to SHM rings; all other daemons are pure consumers or support
services with no direct HAL dependency (except RAD for audio capture and RIC
for ISP exposure queries via RVD's control socket).

```
 sensor
   |
  [RVD] --shm rings--> [RSD] RTSP server (via compy)
   |  \                 [RHD] HTTP snapshots / MJPEG
   |   \                [RMR] fragmented MP4 recording
   |    `--osd shm <--- [ROD] OSD text / logo renderer
   |
  [RAD] --audio ring--> [RSD] (interleaved A/V)
   |                    [RMR] (muxed A/V)
   |
  [RIC] ---- ctrl sock --> [RVD] (exposure queries, ISP mode switch)
```

### Daemons

| Name | Binary | Description |
|------|--------|-------------|
| RVD  | `rvd`  | Raw Video Daemon. Initializes HAL, configures sensor and encoder channels, creates SHM ring buffers (`main`, `sub`, `jpeg0`, `jpeg1`), and runs the frame acquisition loop. Exposes ISP controls and encoder tuning via its control socket. |
| RSD  | `rsd`  | RTSP Streaming Daemon. Reads video/audio rings and serves RTSP/RTP streams using the compy library. Supports Digest authentication. |
| RAD  | `rad`  | Raw Audio Daemon. Captures PCM from the ISP audio input, optionally encodes (G.711 mu-law/A-law, L16, AAC, Opus), and publishes to the `audio` ring. Also handles speaker output via a `speaker` ring. Supports noise suppression, HPF, and AGC when libaudioProcess is available. |
| ROD  | `rod`  | OSD Rendering Daemon. Renders timestamp, uptime, user text, and logo bitmaps into BGRA SHM double-buffers using libschrift. No HAL dependency -- RVD handles the hardware OSD regions. |
| RHD  | `rhd`  | HTTP Streaming Daemon. Serves JPEG snapshots (`/snap.jpg`) and MJPEG streams (`/mjpeg`) from JPEG rings. Dual-stack IPv4/IPv6, Basic auth. |
| RIC  | `ric`  | IR-Cut Controller. Polls ISP exposure via RVD's control socket and switches between day/night modes with configurable hysteresis. Controls IR-cut filter and IR LED GPIOs. |
| RMR  | `rmr`  | Recording/Muxing Daemon. Reads H.264/H.265 + audio from rings and writes crash-safe fragmented MP4 segments to SD card. Own fMP4 muxer with zero external dependencies. |

### Tools

| Name | Binary | Description |
|------|--------|-------------|
| raptorctl  | `raptorctl`  | Management CLI. Query daemon status, read/write config values, and send runtime commands (bitrate, GOP, ISP parameters, OSD text, day/night mode, etc.) over control sockets. |
| ringdump   | `ringdump`   | Ring buffer debugger. Print ring header, follow per-frame metadata, or dump raw Annex B to stdout for piping to ffprobe. |
| rac        | `rac`        | Audio client. Record mic input to file/stdout (PCM16 LE) or play back audio (PCM, MP3, AAC, Opus) to the speaker ring. |

## Related Repositories

| Repository | Description |
|-----------|-------------|
| [raptor-hal](https://github.com/gtxaspec/raptor-hal) | Hardware abstraction layer -- wraps Ingenic IMP SDK calls behind a unified API across SDK generations. |
| [raptor-ipc](https://github.com/gtxaspec/raptor-ipc) | SHM ring buffers, OSD double-buffer SHM, and Unix domain control socket protocol. |
| [raptor-common](https://github.com/gtxaspec/raptor-common) | Config parser, logging, daemonize, signal handling, timestamp utilities. |
| [compy](https://github.com/gtxaspec/compy) | RTSP/RTP server library (used by RSD). Built with CMake; fetches Slice99/Datatype99/Interface99/Metalang99 automatically. |

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

## Build

Raptor cross-compiles with a MIPS toolchain from a thingino Buildroot output.

### Quick build (using build.sh)

```sh
./build.sh t31                    # full distclean + rebuild for T31
./build.sh t31 rvd rsd            # rebuild specific targets
./build.sh t31 clean              # clean only
```

Supported platform arguments: `t20`, `t21`, `t23`, `t30`, `t31`.

`build.sh` expects the thingino firmware tree at
`~/projects/thingino/thingino-firmware/` with a completed Buildroot output for
the target profile.

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
`[rtsp]`, `[http]`, `[osd]`, `[ircut]`, `[recording]`, `[log]`.

See `config/raptor.conf` for the full reference with defaults and comments.

Runtime configuration changes can be made without restart via raptorctl:

```sh
raptorctl rvd set-bitrate 0 3000000   # change main stream bitrate
raptorctl rod set-text "Front Door"   # change OSD text
raptorctl ric mode night              # force night mode
raptorctl config save                 # persist running config to disk
```

## Init Script

The init script `config/S31raptor` (installed as `/etc/init.d/S31raptor`)
manages startup order:

1. **RVD** starts first and creates the SHM ring buffers.
2. The script waits (up to 5 seconds) for `/dev/shm/rss_ring_main` to appear.
3. **RAD**, **ROD**, **RSD**, and **RIC** start as consumers.
4. Shutdown reverses the order: consumers stop first, then RVD.

RHD and RMR are not started by default in the init script; enable them in the
config and add to the init script as needed.

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
