/*
 * Stub capture backend for platforms without a native implementation yet.
 * It reports no devices, so the core falls back to its test pattern.
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

bool capture_start(const char *id)
{
   (void)id;
   return false;
}

void capture_stop(void)
{
}

bool capture_is_running(void)
{
   return false;
}

bool capture_fetch_frame(uint32_t *dst, unsigned *width, unsigned *height, uint64_t *sequence)
{
   (void)dst;
   (void)width;
   (void)height;
   (void)sequence;
   return false;
}
