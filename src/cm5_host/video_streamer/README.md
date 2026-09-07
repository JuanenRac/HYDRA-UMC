# video_streamer — Multi-camera RTSP/WebRTC server

**Project:** HYDRA-UMC
**Status:** 🚧 skeleton config only, not yet wired to `ai_inference/` or a
real camera rig.

Serves the 8 USB camera feeds (README.md section 4, GL3523 hub-fed) as
RTSP (and eventually WebRTC, for lower-latency browser viewing — matching
HYDRA-UMC-STUDIO's own "Octal Vision Matrix" dashboard view, which today
only shows mock camera state, `src/store.tsx`'s `createDefaultCameras()`)
streams, optionally with `ai_inference/`'s own YOLOv8 overlay burned in.

## Approach

`rtsp-simple-server` (a.k.a. **MediaMTX**) is the leading candidate — a
single static Go binary, no GStreamer pipeline hand-rolling needed for the
common case, WebRTC support built in already. `src/mediamtx.yml` is a
STARTING config (8 camera paths, `/dev/video0`-`/dev/video7`) — not yet
verified against real hardware (camera device enumeration on a real GL3523
hub setup may not land on those exact device paths in that exact order).

## What's still needed

- Verify actual `/dev/videoN` enumeration on real hardware (8 cameras
  across 2 GL3523 hubs) — likely needs udev rules for stable
  by-path/by-id naming instead of relying on enumeration order
- Wire in `ai_inference/`'s own overlay output as an alternate/additional
  source per camera, once that pipeline is real
- Decide the actual auth/access story before this is reachable outside
  localhost (MediaMTX supports this; not configured in the starter file)
