/*
 * Image cropping and resampling for XRGB8888 frames.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "scale.h"

#define MAX_SPANS 3840

/* Source range [start, end) covered by each output column or row. */
static unsigned span_start[MAX_SPANS];
static unsigned span_end[MAX_SPANS];
static unsigned row_start[MAX_SPANS];
static unsigned row_end[MAX_SPANS];

/* Per-channel sums for one output row. */
static uint32_t sum_r[MAX_SPANS];
static uint32_t sum_g[MAX_SPANS];
static uint32_t sum_b[MAX_SPANS];

static void compute_spans(unsigned *start, unsigned *end, unsigned src_size, unsigned dst_size)
{
   unsigned i;

   for (i = 0; i < dst_size; i++)
   {
      unsigned s = (unsigned)(((uint64_t)i * src_size) / dst_size);
      unsigned e = (unsigned)(((uint64_t)(i + 1) * src_size) / dst_size);

      /* Enlarging: each output pixel still needs one source pixel. */
      if (e <= s)
         e = s + 1;
      if (e > src_size)
         e = src_size;
      if (s >= e)
         s = e - 1;

      start[i] = s;
      end[i]   = e;
   }
}

void scale_area(const uint32_t *src, unsigned src_pitch,
      unsigned src_x, unsigned src_y, unsigned src_width, unsigned src_height,
      uint32_t *dst, unsigned dst_width, unsigned dst_height)
{
   unsigned x, y;

   if (!src_width || !src_height || !dst_width || !dst_height ||
       dst_width > MAX_SPANS || dst_height > MAX_SPANS)
      return;

   /* Same size: plain copy of the region. */
   if (src_width == dst_width && src_height == dst_height)
   {
      for (y = 0; y < dst_height; y++)
         memcpy(&dst[(size_t)y * dst_width],
               &src[(size_t)(src_y + y) * src_pitch + src_x],
               (size_t)dst_width * sizeof(uint32_t));
      return;
   }

   compute_spans(span_start, span_end, src_width, dst_width);
   compute_spans(row_start, row_end, src_height, dst_height);

   for (y = 0; y < dst_height; y++)
   {
      const unsigned rows = row_end[y] - row_start[y];
      unsigned sy;
      uint32_t *out = &dst[(size_t)y * dst_width];

      memset(sum_r, 0, dst_width * sizeof(uint32_t));
      memset(sum_g, 0, dst_width * sizeof(uint32_t));
      memset(sum_b, 0, dst_width * sizeof(uint32_t));

      /* Accumulate every source row covered by this output row.
       * Sums fit in 32 bits: at most 255 * 3840 * 2160 per channel. */
      for (sy = row_start[y]; sy < row_end[y]; sy++)
      {
         const uint32_t *in = &src[(size_t)(src_y + sy) * src_pitch + src_x];

         for (x = 0; x < dst_width; x++)
         {
            uint32_t r = 0, g = 0, b = 0;
            unsigned sx;

            for (sx = span_start[x]; sx < span_end[x]; sx++)
            {
               const uint32_t p = in[sx];
               r += (p >> 16) & 0xFF;
               g += (p >> 8) & 0xFF;
               b += p & 0xFF;
            }

            sum_r[x] += r;
            sum_g[x] += g;
            sum_b[x] += b;
         }
      }

      for (x = 0; x < dst_width; x++)
      {
         const uint32_t count = rows * (span_end[x] - span_start[x]);
         const uint32_t half  = count / 2; /* Round to nearest */
         const uint32_t r     = (sum_r[x] + half) / count;
         const uint32_t g     = (sum_g[x] + half) / count;
         const uint32_t b     = (sum_b[x] + half) / count;

         out[x] = (r << 16) | (g << 8) | b;
      }
   }
}
