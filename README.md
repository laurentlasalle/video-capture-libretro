# video-capture-libretro

A [libretro](https://www.libretro.com/) core that displays a live video capture device (capture card, HDMI grabber, webcam) inside RetroArch, so its shaders can be applied to external sources: a VCR, original console hardware, or anything else plugged into a capture card.

It works like OBS Studio's *Video Capture Device* source, but the image goes through RetroArch's shader pipeline instead.

> **Status: experimental, stage 0.** The core currently renders an animated test pattern only. Device capture is not implemented yet.

## Why

CRT and VHS shaders are designed for low-resolution input. A capture card usually delivers 1080p, often with a 4:3 picture stretched to 16:9 by the HDMI converter. This core will rescale the signal to a low resolution (such as 640×480 or 320×240) and restore the 4:3 aspect ratio *before* RetroArch's shaders see it, so scanlines and masks look right.

## Platforms

| Platform | Status |
|----------|--------|
| Windows (Media Foundation) | Primary target, in progress |
| Linux | Builds, capture not planned yet (see RetroArch's Video Processor core for V4L2) |
| macOS | Not planned yet |

## Roadmap

- [x] **Stage 0: Skeleton.** Core loads without content, renders a test pattern, CI builds the Windows DLL.
- [ ] **Stage 1: Video capture.** Media Foundation Source Reader, device selection, NV12/YUY2 to XRGB8888 conversion.
- [ ] **Stage 2: Core options.**
  - *Output Resolution*: Native, 640x480, 320x240
  - *Aspect Correction*: Off, Squash 16:9 to 4:3, Crop pillarbox
  - *Audio Capture*: On, Off
  - *Frame Buffering*: 0 to 3 frames
- [ ] **Stage 3: Robustness.** Signal resolution changes, "No Signal" screen, device reconnection.
- [ ] **Stage 4: Audio.** Capture device audio with dynamic resampling to compensate for clock drift.
- [ ] **Stage 5: Polish.** Documentation and first release.

## Installation (Windows)

1. Download the latest build from the [Actions](../../actions) tab (artifact `video_capture_libretro-windows-x64`).
2. Copy `video_capture_libretro.dll` into RetroArch's `cores` folder.
3. Copy `video_capture_libretro.info` into RetroArch's `info` folder.
4. In RetroArch, use **Load Core**, select **Video Capture**, then **Start Core**.

## Building

Requires CMake 3.16 or later and a C99 compiler (MSVC, MinGW, GCC or Clang).

```sh
cmake -S . -B build
cmake --build build --config Release
```

## License

MIT, see [LICENSE](LICENSE). `include/libretro.h` is © The RetroArch team, also under the MIT license.
