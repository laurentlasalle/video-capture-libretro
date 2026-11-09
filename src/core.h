/*
 * Declarations shared by the core's modules: frontend callbacks, logging and
 * a few constants. The libretro entry points are in video_capture_libretro.c.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_CORE_H
#define VIDEO_CAPTURE_CORE_H

#include <stdbool.h>
#include <stddef.h>

#include "libretro.h"

#ifndef CORE_VERSION
#define CORE_VERSION "0.0.0"
#endif

#define CORE_NAME  "Video Capture Device"
#define LOG_PREFIX "[Video Capture Device] "

#define DEFAULT_FPS    60.0
#define SAMPLE_RATE    48000.0 /* Audio rate reported to the frontend */
#define RETRY_INTERVAL 3.0     /* Seconds between attempts to reopen a device or audio input */

/* Frontend callbacks, set in video_capture_libretro.c. */
extern retro_environment_t environ_cb;
extern retro_log_printf_t  log_cb;

/* Microseconds from the frontend's clock, or 0 if it has none. */
retro_time_t core_time_usec(void);

/* Appends a line to the capture stats log, if it is open (see stats_log_printf()
 * in video_capture_libretro.c). */
void stats_log_printf(const char *fmt, ...);

/* Shows a message in RetroArch's notifications for `duration_ms`. */
void show_notification(const char *text, unsigned duration_ms);

#endif
