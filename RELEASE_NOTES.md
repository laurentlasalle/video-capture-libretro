First release of **Video Capture Device**, a libretro core that shows a live video capture device (capture card, HDMI grabber, webcam) in RetroArch, so its shaders can be applied to a VCR, original console hardware, or anything else plugged into a capture card.

## Features

- Video capture through Media Foundation, with the device and its resolution picked in the core options.
- Audio capture from the device's audio input, kept in sync with the picture.
- Output at native resolution, 640x480 or 320x240, downscaled before the shaders; aspect ratio options (Source, 4:3 Squash, 4:3 Crop Pillarbox, 16:9) and crop options, for example for pixel-perfect sources.
- YUV to RGB conversion in the core, with Rec. 601/709 and limited/full range, or by Windows.
- Frame pacing tuned for capture devices: the real frame rate of the source is measured and reported to RetroArch, with Smooth (every frame shown) and Lowest Latency modes.
- A No Signal screen explaining what is wrong (no device, device in use, no picture) and how to fix it.
- Optional capture stats, on screen or in a log file.
- `.vidcap` files: empty files that let RetroArch save core options and shader presets per source.

## Installation

1. Copy `video_capture_libretro.dll` into RetroArch's `cores` folder and `video_capture_libretro.info` into its `info` folder.
2. In RetroArch, use **Load Core** > **Video Capture Device**, then **Start Core**.

See the [README](https://github.com/laurentlasalle/video-capture-libretro#readme) for the options, frame pacing advice and troubleshooting.

## Requirements and limitations

- Windows 10 or later, 64-bit. Linux and macOS are not supported yet.
- Most capture devices can be used by one application at a time: close OBS or any other application using the device first.
- Tested with an Elgato Cam Link Pro (with a Blackmagic Pocket Cinema Camera 4K and a SNES Classic Mini as sources) and RetroArch 1.16, with Vulkan and G-Sync.

`SHA256SUMS.txt` lists the SHA-256 checksums of the files.
