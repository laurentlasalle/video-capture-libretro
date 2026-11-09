/*
 * Platform-independent interface to an audio capture backend.
 * The Windows backend uses WASAPI (audio_wasapi.cpp); other platforms use a
 * stub (audio_null.c) until a native backend is written.
 *
 * The backend captures from an audio input (for example the audio of an HDMI
 * capture device) into a buffer of 16-bit stereo frames at the device's sample
 * rate. The core reads from that buffer and resamples to its own output rate.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_AUDIO_H
#define VIDEO_CAPTURE_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "capture.h" /* capture_log_t */

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MAX_DEVICES 32

typedef struct audio_device
{
   char name[256]; /* Friendly name, UTF-8 */
   char id[512];   /* Backend-specific identifier (endpoint id on Windows) */
} audio_device_t;

/* Receives the backend's log messages. */
void audio_set_logger(capture_log_t logger);

/* Lists the active audio inputs; returns how many were written to `out`. */
int audio_list_devices(audio_device_t *out, int max);

/* Starts capturing from the input with the given id. Returns false if it
 * could not be opened. */
bool audio_start(const char *id);

/* Stops capturing and empties the buffer. */
void audio_stop(void);

/* True while the input is open and delivering audio. Becomes false if the
 * input disappears (for example when the device is unplugged). */
bool audio_is_running(void);

/* Sample rate of the captured audio, in Hz. */
unsigned audio_sample_rate(void);

/* Number of stereo frames waiting in the buffer. */
size_t audio_available(void);

/* Copies up to `frames` stereo frames (interleaved left and right) to `dst`
 * and removes them from the buffer. Returns the number copied. */
size_t audio_read(int16_t *dst, size_t frames);

/* Removes up to `frames` stereo frames from the buffer, oldest first. */
void audio_discard(size_t frames);

#ifdef __cplusplus
}
#endif

#endif
