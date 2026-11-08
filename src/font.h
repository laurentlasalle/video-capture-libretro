/*
 * Minimal text drawing on XRGB8888 images, used for the No Signal screen and
 * the capture stats overlay.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_FONT_H
#define VIDEO_CAPTURE_FONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_GLYPH_SIZE 8 /* Glyphs are 8x8 pixels before scaling */

/* Width in pixels of `text` drawn at the given integer scale. */
unsigned font_text_width(const char *text, unsigned scale);

/* Draws `text` with its top-left corner at (x, y). Pixels outside the image
 * are clipped. `pitch` is the image row length in bytes. Only the glyph
 * pixels are written; the background is left untouched. */
void font_draw_text(uint32_t *dst, size_t pitch, unsigned width, unsigned height,
      int x, int y, const char *text, unsigned scale, uint32_t color);

/* Fills a rectangle, clipped to the image. */
void font_fill_rect(uint32_t *dst, size_t pitch, unsigned width, unsigned height,
      int x, int y, int w, int h, uint32_t color);

#ifdef __cplusplus
}
#endif

#endif
