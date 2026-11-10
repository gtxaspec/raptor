# Digital zoom (ISP front crop): `set-zoom` / `get-zoom`

## Summary

Adds runtime digital zoom to `rvd`, exposed over the control socket and the
`raptorctl` CLI:

    raptorctl rvd set-zoom <factor_mille>   # 1000 = off/1x, 2500 = 2.5x, 8000 = 8x
    raptorctl rvd get-zoom                  # {enable,top,left,width,height}

Zoom crops a centered window from the sensor and streams that window. Only the
T31 path is implemented; other SoCs report `RSS_ERR_NOTSUP`-style no-ops, since
the mechanism depends on T31 framesource crop behaviour.

## Why not the obvious ISP crop APIs

Two "obvious" approaches were tried and rejected on hardware (Ingenic T31,
GC2053, 1080p30):

1. `IMP_ISP_Tuning_SetFrontCrop()` — wedges the T31 ISP into a continuous
   `ispcore: irq-status ... err` interrupt loop at ~30/s. The stream dies and
   no subsequent disable recovers it; only a reboot clears it.

2. `IMP_ISP_Tuning_SetAutoZoom(IMPISPAutoZoom)` — the T31 ISP scaler is
   downscale-only, so the scaler output stays at the crop size. Setting a crop
   of 960x540 with `scaler_out = 1920x1080` does not upscale: the 960x540 frame
   is written into 1080p-allocated buffers, producing the classic "two small
   frames + stale band" corruption. There is also no `GetAutoZoom`, so state
   cannot be read back.

## Chosen mechanism

Zoom is implemented with the **framesource channel crop** (`rss_fs_config_t`
`crop`/`scaler`), which rvd already uses to map sensor resolution to stream
resolution. For a zoom factor `f`, the per-stream output is
`min(crop, configured stream size)`, so the pipeline never upscales (the
hardware cannot). The affected streams are rebuilt through the **existing**
`set-resolution` hot-restart path (`do_stream_restart_ex(..., recreate_fs)`),
rather than adding a parallel reconfiguration path.

Because the scalers are downscale-only, **the output stream resolution tracks
the zoom** (e.g. 1080p -> 960x540 at 2x, 240x132 at 8x). This is a hardware
property, not a regression; clients see a smaller frame at higher zoom.

## Lifecycle / why a restart is sometimes needed

- **Zoom in (shrink buffers)** is applied in place by rebuilding the affected
  streams; T31 handles this and frames keep flowing.
- **Zoom out (grow buffers)** cannot grow them back in place: the encoder
  freezes. rvd therefore only records the request; the caller restarts the
  streamer and `rvd_apply_persisted_zoom()` re-applies the target **after** the
  streams are up (a shrink from the fresh full-resolution init).
- **Startup never crops.** Creating a cropped framesource at pipeline init
  crashes rvd, so init always builds at full resolution and zoom is applied
  post-start (~2.5 s, once the streams have settled).
- `/run/raptor-zoom` stores only the **applied** factor, so the caller's
  grow-vs-shrink decision (in-place vs restart) stays correct.
- rvd does **not** restart itself: forking the init script from rvd leaves
  every daemon stopped. The restart is the caller's responsibility (the firmware
  web endpoint does it out-of-process).

## Files

- `rvd/rvd_pipeline.c` — zoom state, crop math, in-place shrink, post-start apply.
- `rvd/rvd_ctrl.c` — `set-zoom` / `get-zoom` handlers; `do_stream_restart_ex`
  exported for the pipeline.
- `rvd/rvd_frame_loop.c` — post-start apply hook.
- `raptorctl/raptorctl_dispatch.c`, `raptorctl/raptorctl_help.c` — CLI surface.
