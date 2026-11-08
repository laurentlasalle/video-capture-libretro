/*
 * Platform-independent interface to a video capture backend.
 * The Windows backend uses Media Foundation (capture_mf.cpp); other platforms
 * use a stub (capture_null.c) until a native backend is written.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_CAPTURE_H
#define VIDEO_CAPTURE_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest frame the core accepts from a device. */
#define CAPTURE_MAX_WIDTH  3840
#define CAPTURE_MAX_HEIGHT 2160

#define CAPTURE_MAX_DEVICES 32

typedef struct capture_device
{
   char name[256]; /* Friendly name, UTF-8 */
   char id[1024];  /* Backend-specific identifier (symbolic link on Windows) */
} capture_device_t;

enum capture_log_level
{
   CAPTURE_LOG_DEBUG = 0,
   CAPTURE_LOG_INFO,
   CAPTURE_LOG_WARN,
   CAPTURE_LOG_ERROR
};

typedef void (*capture_log_t)(enum capture_log_level level, const char *message);

/* Sets the function used by the backend to report messages. */
void capture_set_logger(capture_log_t logger);

/* Fills `out` with up to `max` devices. Returns the number of devices found. */
int capture_list_devices(capture_device_t *out, int max);

/* Opens the device with the given id and starts streaming.
 * Returns false if the device could not be opened. */
bool capture_start(const char *id);

/* Stops streaming and releases the device. Safe to call when not started. */
void capture_stop(void);

/* True while the device is open and has not reported an error. */
bool capture_is_running(void);

/* Copies the most recent frame into `dst` (XRGB8888, tightly packed, at most
 * CAPTURE_MAX_WIDTH x CAPTURE_MAX_HEIGHT) if it is newer than `*sequence`.
 * On success, updates `*width`, `*height` and `*sequence` and returns true. */
bool capture_fetch_frame(uint32_t *dst, unsigned *width, unsigned *height, uint64_t *sequence);

#ifdef __cplusplus
}
#endif

#endif
