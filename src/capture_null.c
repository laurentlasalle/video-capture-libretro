/*
 * Stub capture backend for platforms without a native implementation yet.
 * It reports no devices, so the core shows its No Signal screen.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include "capture.h"

void capture_set_logger(capture_log_t logger)
{
   (void)logger;
}

int capture_list_devices(capture_device_t *out, int max)
{
   (void)out;
   (void)max;
   return 0;
}

bool capture_start(const char *id, bool raw_yuv)
{
   (void)id;
   (void)raw_yuv;
   return false;
}

void capture_stop(void)
{
}

bool capture_is_running(void)
{
   return false;
}

const char *capture_last_error(void)
{
   return "Video capture is not supported on this platform yet.";
}

double capture_get_frame_rate(void)
{
   return 0.0;
}

int64_t capture_time_usec(void)
{
   return 0;
}

uint64_t capture_frames_received(void)
{
   return 0;
}

bool capture_acquire_frame(capture_frame_t *frame)
{
   (void)frame;
   return false;
}
