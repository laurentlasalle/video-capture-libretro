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

/* Pixel layout of a captured frame. */
enum capture_format
{
   /* 32-bit pixels, 0xXXRRGGBB. Already RGB: converted by the backend or the device. */
   CAPTURE_FORMAT_XRGB8888 = 0,
   /* Planar 4:2:0: a full-size Y plane, then a half-size plane of interleaved
    * U and V bytes. Both planes use the same stride. */
   CAPTURE_FORMAT_NV12,
   /* Packed 4:2:2: Y0 U Y1 V for each pair of pixels. */
   CAPTURE_FORMAT_YUY2
};

/* A captured frame handed over to the core. */
typedef struct capture_frame
{
   uint8_t            *data;       /* First row of the image (the Y plane for NV12) */
   size_t              stride;     /* Row length in bytes */
   enum capture_format format;
   unsigned            width;
   unsigned            height;
   double              frame_rate; /* Frames per second, or 0 if unknown */
   int64_t             arrival_usec; /* When the frame arrived, on the capture_time_usec() clock */
   uint64_t            sequence;     /* Arrival order, starting at 1 */
} capture_frame_t;

/* Sets the function used by the backend to report messages. */
void capture_set_logger(capture_log_t logger);

/* Fills `out` with up to `max` devices. Returns the number of devices found. */
int capture_list_devices(capture_device_t *out, int max);

/* Opens the device with the given id and starts streaming.
 * With `raw_yuv`, NV12 and YUY2 frames are delivered as is, for the core to
 * convert; otherwise the backend delivers XRGB8888. With a nonzero
 * `preferred_height`, a device format of that height is used if the device
 * offers one (for example 720 to capture 1280x720 without the device scaling
 * it); otherwise the largest format up to 1920x1080.
 * Returns false if the device could not be opened; see capture_last_error(). */
bool capture_start(const char *id, bool raw_yuv, unsigned preferred_height);

/* Stops streaming and releases the device. Safe to call when not started. */
void capture_stop(void);

/* True while the device is open and has not reported an error. */
bool capture_is_running(void);

/* Human-readable reason for the last failure (opening or streaming), with a
 * troubleshooting hint when one is known. Empty if there was no failure. */
const char *capture_last_error(void);

/* Frame rate of the current stream in frames per second, or 0 if unknown.
 * Known as soon as capture_start() succeeds, before the first frame. */
double capture_get_frame_rate(void);

/* Current time in microseconds, on the clock used for frame arrival times. */
int64_t capture_time_usec(void);

/* Number of frames received from the device since capture_start(),
 * including frames the core never picked up. Used for statistics. */
uint64_t capture_frames_received(void);

/* Frames are numbered in arrival order, starting at 1. The backend keeps the
 * few most recent ones; older frames are dropped, never queued.
 *
 * Reports the range of frames currently available and when the newest one
 * arrived. Returns false if no frame is available. */
bool capture_peek_frames(uint64_t *oldest, uint64_t *newest, int64_t *newest_arrival_usec);

/* Hands over the frame with the given sequence number, if still available.
 * No copy is made: `frame->data` points into the backend's buffer, which is
 * kept untouched until the next call to this function. The core may draw on
 * it (for the stats overlay). */
bool capture_acquire_frame(uint64_t sequence, capture_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif
