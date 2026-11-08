/*
 * Image resampling for XRGB8888 frames.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "scale.h"

/* Largest output size supported; the core outputs at most 640x480 when scaling. */
#define MAX_DST 1024

/* Source range [start, end) covered by each output column and row. */
static unsigned col_start[MAX_DST], col_end[MAX_DST];
static unsigned row_start[MAX_DST], row_end[MAX_DST];

/* Channel sums for the output row being built. Red and blue are summed
 * together in one 32-bit value (see scale_area()). */
static uint32_t sum_rb[MAX_DST];
static uint32_t sum_g[MAX_DST];

/* Splits src_size pixels into dst_size spans. Returns the widest span. */
static unsigned compute_spans(unsigned *start, unsigned *end, unsigned src_size, unsigned dst_size)
{
   unsigned i, widest = 0;

   for (i = 0; i < dst_size; i++)
   {
      unsigned s = (unsigned)(((uint64_t)i * src_size) / dst_size);
      unsigned e = (unsigned)(((uint64_t)(i + 1) * src_size) / dst_size);

      /* Enlarging: each output pixel still needs one source pixel. */
      if (e <= s)
         e = s + 1;
      if (e > src_size)
      {
         e = src_size;
         s = e - 1;
      }

      start[i] = s;
      end[i]   = e;
      if (e - s > widest)
         widest = e - s;
   }

   return widest;
}

bool scale_area(const uint32_t *src, unsigned src_pitch,
      unsigned src_x, unsigned src_y, unsigned src_width, unsigned src_height,
      uint32_t *dst, unsigned dst_width, unsigned dst_height)
{
   unsigned x, y;

   if (!src_width || !src_height || !dst_width || !dst_height ||
       dst_width > MAX_DST || dst_height > MAX_DST)
      return false;

   /*
    * Red and blue are summed in parallel: masking a pixel with 0x00FF00FF
    * leaves red in bits 16-23 and blue in bits 0-7, so adding masked pixels
    * accumulates both channels in separate 16-bit lanes. Green is summed on its
    * own (mask 0x0000FF00). A lane holds up to 65535, i.e. 257 pixels of value
    * 255, so each output pixel may cover at most 256 source pixels. With a
    * 3840x2160 source and a 320x240 output, a pixel covers 12x9 = 108.
    */
   if (compute_spans(col_start, col_end, src_width, dst_width) *
       compute_spans(row_start, row_end, src_height, dst_height) > 256)
      return false;

   for (y = 0; y < dst_height; y++)
   {
      const unsigned rows = row_end[y] - row_start[y];
      uint32_t *out = &dst[(size_t)y * dst_width];
      unsigned sy;

      memset(sum_rb, 0, dst_width * sizeof(uint32_t));
      memset(sum_g, 0, dst_width * sizeof(uint32_t));

      /* Accumulate every source row covered by this output row. Each source
       * pixel is read exactly once. */
      for (sy = row_start[y]; sy < row_end[y]; sy++)
      {
         const uint32_t *in = &src[(size_t)(src_y + sy) * src_pitch + src_x];

         for (x = 0; x < dst_width; x++)
         {
            uint32_t rb = 0, g = 0;
            unsigned sx;

            for (sx = col_start[x]; sx < col_end[x]; sx++)
            {
               rb += in[sx] & 0x00FF00FF;
               g  += in[sx] & 0x0000FF00;
            }

            sum_rb[x] += rb;
            sum_g[x]  += g;
         }
      }

      /* Divide by the pixel count. Multiplying by a 16.16 fixed-point
       * reciprocal is much faster than three integer divisions per pixel;
       * the rounding error stays below one level out of 255. */
      for (x = 0; x < dst_width; x++)
      {
         const uint32_t count = rows * (col_end[x] - col_start[x]);
         const uint32_t recip = (65536 + count / 2) / count;
         const uint32_t r     = ((sum_rb[x] >> 16)     * recip + 32768) >> 16;
         const uint32_t b     = ((sum_rb[x] & 0xFFFF)  * recip + 32768) >> 16;
         const uint32_t g     = ((sum_g[x] >> 8)       * recip + 32768) >> 16;

         out[x] = (r << 16) | (g << 8) | b;
      }
   }

   return true;
}
