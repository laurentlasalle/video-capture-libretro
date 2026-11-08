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

/* A captured frame handed over to the core. */
typedef struct capture_frame
{
   const uint32_t *pixels;     /* XRGB8888, tightly packed (pitch = width) */
   unsigned        width;
   unsigned        height;
   double          frame_rate; /* Frames per second, or 0 if unknown */
} capture_frame_t;

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

/* Frame rate of the current stream in frames per second, or 0 if unknown.
 * Known as soon as capture_start() succeeds, before the first frame. */
double capture_get_frame_rate(void);

/* Hands over the most recent frame if one arrived since the previous call.
 * Returns false when there is no new frame.
 *
 * No copy is made: the backend keeps three buffers (one being written by the
 * device, one holding the latest complete frame, one owned by the core) and
 * swaps pointers. Older frames are dropped, never queued, so latency stays
 * minimal. `frame->pixels` stays valid until the next call to this function. */
bool capture_acquire_frame(capture_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif
