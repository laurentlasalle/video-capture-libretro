/*
 * Stub audio backend for platforms without a native implementation yet.
 * It reports no inputs, so the core outputs silence.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include "audio.h"

void audio_set_logger(capture_log_t logger)
{
   (void)logger;
}

int audio_list_devices(audio_device_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

bool audio_start(const char *id)
{
   (void)id;
   return false;
}

void audio_stop(void)
{
}

bool audio_is_running(void)
{
   return false;
}

unsigned audio_sample_rate(void)
{
   return 48000;
}

size_t audio_available(void)
{
   return 0;
}

size_t audio_read(int16_t *dst, size_t frames)
{
   (void)dst;
   (void)frames;
   return 0;
}

void audio_discard(size_t frames)
{
   (void)frames;
}
