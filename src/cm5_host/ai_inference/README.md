# ai_inference — Hailo-8 / YOLOv8 vision pipeline

**Project:** HYDRA-UMC
**Status:** 🚧 pipeline design + starter config only — no Hailo SDK output
(`.hef` model) is vendored here, and nothing in this folder has been run
against real Hailo-8 hardware yet.

Drives the M.2 Hailo-8 AI accelerator (README.md section 3, 26 TOPS,
`docs/datasheets/Hailo-8*.pdf`) to run multi-stream YOLOv8/YOLO11 object
detection across the 8 cameras `video_streamer/` captures — pick-and-place
fiducial alignment, defect inspection (README.md section 1).

Note: the board now also carries a second M.2 AI accelerator, a Hailo-10
(README.md section 3), reachable behind the same onboard PCIe Gen3 switch.
It is used for a different job — local cognitive reasoning / GenAI (LLM/VLA
models), not vision — so it is out of scope for this folder; nothing here
targets it. If a Hailo-10 counterpart to this pipeline is ever needed, it
belongs in its own folder/module, not bolted onto this Hailo-8/YOLOv8 one.

## Toolchain (not installed on this development machine — Linux/x86_64 or
the Hailo Dataflow Compiler's own supported host only)

1. **Hailo Dataflow Compiler** (`hailo_dataflow_compiler`, requires a
   (free) Hailo Developer Zone account) — converts a trained YOLOv8 `.onnx`
   export into a Hailo-8-native `.hef` model, with INT8 quantization
   calibrated against representative images.
2. **HailoRT** (`hailort`, Python + C++ runtime + PCIe driver) — loads a
   `.hef` and runs inference on the actual M.2 card. This is what actually
   needs to be present on the CM5 target device, not the Dataflow Compiler
   above (that's a one-time, offline model-conversion step, typically run
   on a dev machine, not the CM5 itself).
3. **TAPPAS** (Hailo's own GStreamer-based application framework) — the
   `hailonet`/`hailofilter` GStreamer elements this pipeline builds on, per
   README.md section 1's own "GStreamer pipelines and OpenCV" mention.

## Layout

- `models/` — where a converted `.hef` model lands once one exists. Empty
  today (`.gitkeep` only) — a real model is a build/training artifact, not
  hand-written source, and shouldn't be committed here until this project
  has an actual trained YOLOv8 checkpoint to convert.
- `pipelines/` — GStreamer pipeline definitions. `pipelines/yolov8_8cam.txt`
  is a STARTING POINT `gst-launch-1.0`-style pipeline string (not yet
  runnable — references a `.hef` that doesn't exist yet), showing the
  intended shape: 8x camera source → `hailonet` (batched multi-stream
  inference) → `hailofilter` (post-processing/NMS) → per-stream overlay →
  handoff to `video_streamer/`.

## What's still needed

- An actual trained/fine-tuned YOLOv8 model for this project's own
  detection targets (PCB panels, ICs, fiducials — see the demo detection
  labels already used in HYDRA-UMC-STUDIO's own mock camera state,
  `src/store.tsx`'s `createDefaultCameras()`, as a hint at what's expected,
  not a spec)
- Actually running the Dataflow Compiler conversion once that model exists
- Verifying `pipelines/yolov8_8cam.txt` against real Hailo-8 hardware and a
  real 8-camera rig — nothing here has been hardware-tested
