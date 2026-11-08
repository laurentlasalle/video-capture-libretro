/*
 * Image resampling for XRGB8888 frames.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_SCALE_H
#define VIDEO_CAPTURE_SCALE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resamples a region of `src` into `dst` (tightly packed, dst_width x dst_height,
 * at most 1024 pixels on each side).
 *
 * The region starts at (src_x, src_y) and is src_width x src_height pixels;
 * src_pitch is the source row length in pixels. When shrinking, each output
 * pixel is the average of the source pixels it covers (box filter), which
 * avoids the aliasing of plain pixel skipping. When enlarging, the nearest
 * source pixel is used.
 *
 * Returns false, leaving `dst` untouched, if the sizes are out of range or an
 * output pixel would cover more than 256 source pixels. */
bool scale_area(const uint32_t *src, unsigned src_pitch,
      unsigned src_x, unsigned src_y, unsigned src_width, unsigned src_height,
      uint32_t *dst, unsigned dst_width, unsigned dst_height);

#ifdef __cplusplus
}
#endif

#endif
