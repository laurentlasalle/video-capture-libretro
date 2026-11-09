/*
 * Audio sent to the frontend with each video frame: the audio input selected
 * in the core options, resampled to SAMPLE_RATE, or silence.
 *
 * The input runs on the capture device's clock and RetroArch on the
 * computer's, so the amount of buffered input slowly drifts. The module keeps
 * a small, steady amount buffered by resampling very slightly (see
 * output_audio() in audio_output.c).
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_AUDIO_OUTPUT_H
#define VIDEO_CAPTURE_AUDIO_OUTPUT_H

#include <stdbool.h>

#include "libretro.h"

void audio_output_set_callback(retro_audio_sample_batch_t cb);

/* Opens the audio input selected in the Audio Input option. With Auto, the
 * input that belongs to the capture device named `capture_name`. Without
 * `force`, nothing happens if that input is already open. */
void audio_output_select(bool force, const char *capture_name);

/* Sends one video frame's worth of audio (SAMPLE_RATE / fps frames) to the
 * frontend, and retries a lost input every RETRY_INTERVAL seconds. Called
 * once per retro_run(). */
void audio_output_run(double fps);

/* Closes the audio input. */
void audio_output_close(void);

/* Session counters (underruns and resyncs), logged with the session summary. */
void audio_output_reset_session(void);
void audio_output_log_session(void);

#endif
