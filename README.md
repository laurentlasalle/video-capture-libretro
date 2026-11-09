# video-capture-libretro

A [libretro](https://www.libretro.com/) core that displays a live video capture device (capture card, HDMI grabber, webcam) inside RetroArch, so its shaders can be applied to external sources: a VCR, original console hardware, or anything else plugged into a capture card.

It works like OBS Studio's *Video Capture Device* source, but the image goes through RetroArch's shader pipeline instead.

> **Status: experimental.** Video and audio capture work on Windows.

## Why

CRT and VHS shaders are designed for low-resolution input. A capture card usually delivers 1080p, often with a 4:3 picture stretched to 16:9 by the HDMI converter. This core rescales the signal to a low resolution (such as 640×480 or 320×240) and restores the 4:3 aspect ratio *before* RetroArch's shaders see it, so scanlines and masks look right.

## Installation (Windows)

1. Download the latest build from the [Actions](../../actions) tab (artifact `video_capture_libretro-windows-x64`).
2. Copy `video_capture_libretro.dll` into RetroArch's `cores` folder.
3. Copy `video_capture_libretro.info` into RetroArch's `info` folder (see **Settings > Directory > Core Info**). If the core still shows as `video_capture_libretro.dll`, close RetroArch and delete `core_info.cache` in that folder.
4. In RetroArch, use **Load Core**, select **Video Capture Device**, then **Start Core**.

Close OBS or any other application using the device first: most capture devices can only be opened by one application at a time.

## Core options

**Capture Device** and **Audio Input** are on the main page of **Quick Menu > Core Options**; the other options are grouped in three submenus: **Video** (Capture Resolution, Output Resolution, Aspect Ratio, Crop Horizontal, Crop Vertical, Frame Pacing), **Color** (Color Conversion, Color Space, Color Range) and **Diagnostics** (Show Capture Stats, Log Capture Stats, No Signal Details).

| Option | Values | Description |
|--------|--------|-------------|
| Capture Device | Auto, detected devices | Device to display. Auto selects the first device found. Devices are detected when the core starts. |
| Audio Input | Off, Auto, detected inputs | Audio played with the picture, for example the audio of the HDMI capture device. *Auto* picks the input that goes with the capture device (same name, or same name and input number, as with "Cam Link Pro HDMI-4" and "Cam Link Pro Audio-4") and shows its choice in a notification. Leave it off if the audio already reaches your speakers another way, for example through a mixer. See [Audio](#audio). |
| Capture Resolution | Auto, 1080p, 720p, 576p, 480p | Resolution requested from the device. *Auto* uses the largest one up to 1080p. Choosing the source's resolution, for example 720p for a 720p console, avoids the device scaling the picture, which keeps pixel-perfect sources sharp. If the device does not offer it, Auto is used. |
| Output Resolution | Native, 640x480, 320x240 | Size of the image passed to the shaders. CRT and VHS shaders expect a low resolution. Downscaling averages the source pixels to avoid aliasing. |
| Aspect Ratio | Source, 4:3 (Squash), 4:3 (Crop Pillarbox), 16:9 | Display aspect ratio. *Squash* keeps the whole frame, for a 4:3 picture stretched to 16:9. *Crop Pillarbox* keeps only the central 4:3 area, for a picture with black bars on the sides. Applies to the area left by the crop options. |
| Crop Horizontal | Off, 4 to 504 px | Pixels removed on the left and on the right of the captured image, for example the black borders around a pixel-perfect picture. Applied before Aspect Ratio and Output Resolution. |
| Crop Vertical | Off, 2 to 252 px | Pixels removed at the top and at the bottom. When the captured size changes, for example after changing Capture Resolution, both crop values are converted to keep the same part of the picture (384 x 36 px at 1080p becomes 256 x 24 px at 720p). |
| Frame Pacing | Smooth, Lowest Latency | *Smooth* (the default) shows every frame in order; when frames arrive right when RetroArch asks for one, it adds up to one frame of latency, then skips a single frame to catch up. Recommended to watch video. *Lowest Latency* always shows the newest frame, but drops and repeats frames at those moments. Recommended to play games through the capture device. See [Latency and frame pacing](#latency-and-frame-pacing). |
| Color Conversion | Core, Media Foundation | Who converts the device's YUV picture to RGB. *Core* converts after downscaling, which is faster at 640x480 and 320x240, and applies the two options below. *Media Foundation* converts the full frame with Windows. |
| Color Space | Default, Rec. 709, Rec. 601 | Shown when Color Conversion is set to Core. Color matrix for the conversion. *Default* uses Rec. 601 up to 576 lines and Rec. 709 above. |
| Color Range | Limited, Full | Shown when Color Conversion is set to Core. *Limited* is standard for video (black at 16, white at 235). Use *Full* if blacks look gray or colors look washed out. |
| Show Capture Stats | Off, On-Screen Notification, In Image | Three frame rates, measured every second: *Capture* (frames received from the device), *RetroArch* (calls to the core) and *Output* (new frames shown). In the image, each line is green when the rate is as expected, white when slightly low, red when frames are being lost. A low RetroArch rate means the frontend or a heavy shader cannot keep up; a normal RetroArch rate with a low Output rate means frames are lost to uneven timing. *In Image* draws the text into the picture, before shaders. |
| Log Capture Stats | Off, On | Shown when Show Capture Stats is on. Writes the stats every second, with the age of each frame when shown, to `video_capture_stats.log` in RetroArch's save directory. |
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

- With **Frame Pacing** set to **Lowest Latency**, each `retro_run()` shows the most recent frame received; there is no frame queue.
- The core reports the device's frame rate to RetroArch, so RetroArch paces the core on the capture device. The rate a device declares is not always the rate of the signal: a device can declare 60 fps for a 59.94 fps source (NTSC video), or the reverse. The core measures the real rate over the first seconds and, if it differs, reports the measured rate instead; it is confirmed by a second measurement 5 seconds later. The change is noted in the stats log. With **Color Conversion** set to **Media Foundation**, the core also turns off the frame rate conversion of Windows' video processor, which would otherwise drop or repeat frames to match the declared rate.

The display's refresh rate decides whether every captured frame is shown. Capture devices usually run at 59.94 fps, which no fixed refresh rate divides evenly: at 120 Hz with **VSync Swap Interval** set to 2, RetroArch shows exactly 60 fps and has to skip a frame regularly to stay in sync; without VSync, RetroArch's timing is uneven and many frames are lost. In order of preference:

1. **Variable refresh rate display (G-Sync, FreeSync)**: enable **Settings > Video > Synchronization > Sync to Exact Content Framerate**. The display follows the capture device exactly and no frame is lost.
2. **Fixed refresh rate display**: set the display to 59.94 Hz or 60 Hz in Windows, and keep **VSync** on in RetroArch with **VSync Swap Interval** at 1. A frame may still be dropped or repeated occasionally, since the display and the capture device have separate clocks.

The moment a frame arrives, relative to the moment RetroArch asks for one, is set when the device opens and drifts slowly if the rates differ slightly. When the two get close, timing noise decides whether each frame is ready in time:

- With **Smooth** (the default), the core shows the frames in order, with at most one frame of extra latency at those moments, then skips a single frame to catch up.
- With **Lowest Latency**, frames are dropped and repeated for as long as the two stay close: a few seconds when the rates differ slightly, possibly a minute or more when they match exactly.

Where frames land relative to RetroArch's calls is set by chance when the device opens, and again when the reported frame rate changes, since RetroArch pauses for a moment then (about 15 seconds after the device opens, if the measured rate differs). With **Lowest Latency**, it can land close to that point and stay there, with frames dropped and repeated every second or so; reopening the device (for example by switching **Color Conversion** back and forth) draws it again. **Smooth** does not have this problem.

Turn on **Show Capture Stats** to check: the *Output* rate should match the *Capture* rate. With **Log Capture Stats**, the rates, the frames dropped and repeated, and the age of each frame when shown are written every second to `video_capture_stats.log`, followed by a session summary when the core closes. The options in use are listed at the top of the file, and each change is noted with the second it happened. Pauses (RetroArch's menu), stalls (RetroArch rebuilding a shader, for example when the output size changes) and the startup delay are noted in the file and excluded from the measurements; frames lost in the partial second before them are written on a separate line, so the lines add up to the session summary. RetroArch 1.16 does not keep core messages in its own log once content is running, hence the separate file.

## Troubleshooting

The core writes the device's formats, the selected format and any errors to RetroArch's log. Enable **Settings > Logging > Log to File**; the list of native formats is shown when **Core Logging Level** is set to **0 (Debug)**.

## Platforms

| Platform | Status |
|----------|--------|
| Windows (Media Foundation) | Supported |
| Linux | Builds; capture not implemented yet (see RetroArch's Video Processor core for V4L2) |
| macOS | Not planned yet |

## Audio

With **Audio Input** set, the core captures that input (WASAPI, shared mode) and sends it to RetroArch with the picture. It keeps about 33 ms of audio buffered (2 frames) and resamples it by up to 0.5%, inaudibly, to follow RetroArch, whose clock differs slightly from the capture device's. After a pause, the core drops the audio received meanwhile, so sound and picture stay in sync.

Windows may also play the same input through **Listen to this device** (Sound settings > the input's properties); turn that off to avoid hearing it twice. Session summaries in the stats log count audio underruns (the buffer ran dry) and resyncs (excess audio dropped, usually after a pause).

## Roadmap

- [x] Video capture with device selection
- [x] Output resolution and aspect ratio options
- [x] Color conversion in the core, Rec. 601/709, limited/full range
- [x] No Signal screen, capture stats, `.vidcap` files
- [x] Audio capture, with dynamic resampling to compensate for clock drift
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
