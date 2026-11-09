/*
 * The No Signal screen: color bars with a panel giving the reason, the device
 * name and, with the No Signal Details option, troubleshooting hints.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_NO_SIGNAL_H
#define VIDEO_CAPTURE_NO_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

#define NO_SIGNAL_WIDTH  640
#define NO_SIGNAL_HEIGHT 480

/* State of the capture, from the core's point of view. Everything but
 * SIGNAL_OK and SIGNAL_HOLD shows the No Signal screen. */
enum signal_state
{
   SIGNAL_OK = 0,
   SIGNAL_NO_DEVICE,   /* No capture device found */
   SIGNAL_DEVICE_ERROR,/* The device could not be opened, or stopped with an error */
   SIGNAL_NO_FRAMES,   /* The device is open but sends no frames */
   SIGNAL_BLACK,       /* The device sends uniformly black frames: no input signal */
   SIGNAL_HOLD         /* The device just reopened: keep the last image until its first frame */
};

/* Draws the screen for `state` and returns it (NO_SIGNAL_WIDTH x
 * NO_SIGNAL_HEIGHT XRGB8888 pixels, tightly packed). It is redrawn only when
 * its content changes. */
const uint32_t *no_signal_render(enum signal_state state, bool details, const char *device_name);

/* Forces the next no_signal_render() to redraw, for example after an option
 * change. */
void no_signal_invalidate(void);

#endif
