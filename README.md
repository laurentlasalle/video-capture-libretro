# video-capture-libretro

A [libretro](https://www.libretro.com/) core that displays a live video capture device (capture card, HDMI grabber, webcam) inside RetroArch, so its shaders can be applied to external sources: a VCR, original console hardware, or anything else plugged into a capture card.

It works like OBS Studio's *Video Capture Device* source, but the image goes through RetroArch's shader pipeline instead.

> **Status: experimental, stage 2.** The core displays the selected capture device (Windows only), with optional downscaling and aspect ratio correction. Audio is not captured yet.

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
- [x] **Stage 1: Video capture.** Media Foundation Source Reader, *Capture Device* core option, conversion to XRGB8888, test pattern when no frames arrive.
- [x] **Stage 2: Image options.** *Output Resolution* and *Aspect Ratio* core options.
- [ ] **Stage 3: Robustness.** Signal resolution changes, "No Signal" screen, device reconnection.
- [ ] **Stage 4: Audio.** Capture device audio with dynamic resampling to compensate for clock drift, *Audio Capture* core option (On/Off).
- [ ] **Stage 5: Polish.** Documentation and first release.

## Installation (Windows)

1. Download the latest build from the [Actions](../../actions) tab (artifact `video_capture_libretro-windows-x64`).
2. Copy `video_capture_libretro.dll` into RetroArch's `cores` folder.
3. Copy `video_capture_libretro.info` into RetroArch's `info` folder.
4. In RetroArch, use **Load Core**, select **Video Capture Device**, then **Start Core**.

## Usage

### Core options

| Option | Values | Description |
|--------|--------|-------------|
| Capture Device | Auto, detected devices | Device to display. Auto selects the first device found. |
| Output Resolution | Native, 640x480, 320x240 | Size of the image passed to the shaders. CRT and VHS shaders expect a low resolution. Downscaling averages the source pixels to avoid aliasing. |
| Aspect Ratio | Source, 4:3 (Squash), 4:3 (Crop Pillarbox), 16:9 | Display aspect ratio. *Squash* keeps the whole frame, for a 4:3 picture stretched to 16:9. *Crop Pillarbox* keeps only the central 4:3 area, for a picture with black bars on the sides. |

Resolution and aspect ratio are independent: 640x480 with 16:9 gives an anamorphic image, like a DVD. For the core's aspect ratio to be used, set **Settings > Video > Scaling > Aspect Ratio** to **Core Provided**. A shader preset may still force its own aspect ratio.

### Notes

- The core picks the first capture device it finds. To choose another one, open **Quick Menu > Core Options > Capture Device**. Devices are detected when the core starts.
- When no device is found, or no frame arrives for two seconds, the core shows a test pattern (color bars with a white line sweeping down once per second).
- The selected device format and any errors are written to RetroArch's log (**Settings > Logging**).
- Close OBS or any other application using the device first: most capture devices can only be opened by one application at a time.

## Building

Requires CMake 3.16 or later and a C99 compiler (MSVC, MinGW, GCC or Clang).

```sh
cmake -S . -B build
cmake --build build --config Release
```

## License

MIT, see [LICENSE](LICENSE). `include/libretro.h` is © The RetroArch team, also under the MIT license.
