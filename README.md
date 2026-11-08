# video-capture-libretro

A [libretro](https://www.libretro.com/) core that displays a live video capture device (capture card, HDMI grabber, webcam) inside RetroArch, so its shaders can be applied to external sources: a VCR, original console hardware, or anything else plugged into a capture card.

It works like OBS Studio's *Video Capture Device* source, but the image goes through RetroArch's shader pipeline instead.

> **Status: experimental.** Video capture works on Windows. Audio is not captured yet.

## Why

CRT and VHS shaders are designed for low-resolution input. A capture card usually delivers 1080p, often with a 4:3 picture stretched to 16:9 by the HDMI converter. This core rescales the signal to a low resolution (such as 640×480 or 320×240) and restores the 4:3 aspect ratio *before* RetroArch's shaders see it, so scanlines and masks look right.

## Installation (Windows)

1. Download the latest build from the [Actions](../../actions) tab (artifact `video_capture_libretro-windows-x64`).
2. Copy `video_capture_libretro.dll` into RetroArch's `cores` folder.
3. Copy `video_capture_libretro.info` into RetroArch's `info` folder (see **Settings > Directory > Core Info**). If the core still shows as `video_capture_libretro.dll`, close RetroArch and delete `core_info.cache` in that folder.
4. In RetroArch, use **Load Core**, select **Video Capture Device**, then **Start Core**.

Close OBS or any other application using the device first: most capture devices can only be opened by one application at a time.

## Core options

| Option | Values | Description |
|--------|--------|-------------|
| Capture Device | Auto, detected devices | Device to display. Auto selects the first device found. Devices are detected when the core starts. |
| Output Resolution | Native, 640x480, 320x240 | Size of the image passed to the shaders. CRT and VHS shaders expect a low resolution. Downscaling averages the source pixels to avoid aliasing. |
| Aspect Ratio | Source, 4:3 (Squash), 4:3 (Crop Pillarbox), 16:9 | Display aspect ratio. *Squash* keeps the whole frame, for a 4:3 picture stretched to 16:9. *Crop Pillarbox* keeps only the central 4:3 area, for a picture with black bars on the sides. |
| Color Conversion | Core, Media Foundation | Who converts the device's YUV picture to RGB. *Core* converts after downscaling, which is faster at 640x480 and 320x240, and applies the two options below. *Media Foundation* converts the full frame with Windows. |
| Color Space | Default, Rec. 709, Rec. 601 | Color matrix for the conversion. *Default* uses Rec. 601 up to 576 lines and Rec. 709 above. |
| Color Range | Limited, Full | *Limited* is standard for video (black at 16, white at 235). Use *Full* if blacks look gray or colors look washed out. |
| Show Capture Stats | Off, On-Screen Notification, In Image | Three frame rates, measured every second: *Capture* (frames received from the device), *RetroArch* (calls to the core) and *Output* (new frames shown). In the image, each line is green when the rate is as expected, white when slightly low, red when frames are being lost. A low RetroArch rate means the frontend or a heavy shader cannot keep up; a normal RetroArch rate with a low Output rate means frames are lost to uneven timing. *In Image* draws the text into the picture, before shaders. |
| No Signal Details | Off, On | Shows the reason and troubleshooting hints on the No Signal screen. |

Resolution and aspect ratio are independent: 640x480 with 16:9 gives an anamorphic image, like a DVD. For the core's aspect ratio to be used, set **Settings > Video > Scaling > Aspect Ratio** to **Core Provided**. A shader preset may still force its own aspect ratio.

## No Signal screen

The core shows color bars with a message instead of a black screen when:

- **NO DEVICE**: no capture device was found;
- **DEVICE ERROR**: the device could not be opened, for example because another application uses it. The core retries every 3 seconds;
- **NO SIGNAL**: the device sends no frames for 2 seconds, or sends perfectly black frames for 1 second. Capture devices without input signal often send digitally generated black; analog video, even on a black scene, always has some noise, so it is not mistaken for a missing signal.

Set **No Signal Details** to **On** for the exact reason and hints.

## Saving settings per source (.vidcap files)

The core can also be started with a `.vidcap` file, which RetroArch treats like a game. The file can be empty: it only gives RetroArch a name to save settings under. This makes it easy to keep one set of settings per source, for example one per console or VCR:

1. Create an empty text file and rename it, for example `VCR JVC.vidcap`.
2. In RetroArch, use **Load Content**, select the file and pick the **Video Capture Device** core.
3. Set the core options (device, resolution, aspect ratio), then use **Quick Menu > Core Options > Manage Core Options > Save Game Options**.
4. Optionally, save a shader preset for this file only: **Quick Menu > Shaders > Save > Save Game Preset**.

Next time the file is loaded, its options and shader preset come back automatically. `.vidcap` files can also be added to playlists.

## Latency and frame pacing

- The core keeps no frame queue: each `retro_run()` shows the most recent frame received, and older frames are dropped.
- The core reports the device's real frame rate (for example 59.94 fps) to RetroArch, so RetroArch paces the core on the capture device.

The display's refresh rate decides whether every captured frame is shown. Capture devices usually run at 59.94 fps, which no fixed refresh rate divides evenly: at 120 Hz with **VSync Swap Interval** set to 2, RetroArch shows exactly 60 fps and has to skip a frame regularly to stay in sync; without VSync, RetroArch's timing is uneven and many frames are lost. In order of preference:

1. **Variable refresh rate display (G-Sync, FreeSync)**: enable **Settings > Video > Synchronization > Sync to Exact Content Framerate**. The display follows the capture device exactly and no frame is lost.
2. **Fixed refresh rate display**: set the display to 59.94 Hz or 60 Hz in Windows, and keep **VSync** on in RetroArch with **VSync Swap Interval** at 1. A frame may still be dropped or repeated occasionally, since the display and the capture device have separate clocks.

Turn on **Show Capture Stats** to check: the *Output* rate should match the *Capture* rate. When the core closes, a summary of the session (average rates and frames not shown) is written to the log.

## Troubleshooting

The core writes the device's formats, the selected format and any errors to RetroArch's log. Enable **Settings > Logging > Log to File**; the list of native formats is shown when **Core Logging Level** is set to **0 (Debug)**.

## Platforms

| Platform | Status |
|----------|--------|
| Windows (Media Foundation) | Supported |
| Linux | Builds; capture not implemented yet (see RetroArch's Video Processor core for V4L2) |
| macOS | Not planned yet |

## Roadmap

- [x] Video capture with device selection
- [x] Output resolution and aspect ratio options
- [x] Color conversion in the core, Rec. 601/709, limited/full range
- [x] No Signal screen, capture stats, `.vidcap` files
- [ ] Audio capture, with dynamic resampling to compensate for clock drift
- [ ] First release

## Building

Requires CMake 3.16 or later and a C99/C++17 compiler (MSVC, MinGW, GCC or Clang).

```sh
cmake -S . -B build
cmake --build build --config Release
```

## License

MIT, see [LICENSE](LICENSE).

- `include/libretro.h` is © The RetroArch team, under the MIT license.
- The bitmap font in `src/font.c` comes from [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper, in the public domain.
