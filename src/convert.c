/*
 * YUV to RGB conversion with cropping and resampling.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "convert.h"

/* ------------------------------------------------------------------------ */
/* Conversion tables                                                         */
/* ------------------------------------------------------------------------ */

/*
 * RGB from YUV, with Y scaled to 0..1 and U, V centered on 0:
 *   R = Y + kr_v * V
 *   G = Y - kg_u * U - kg_v * V
 *   B = Y + kb_u * U
 * The coefficients come from each standard's luma weights (Kr, Kb):
 *   Rec. 601: Kr = 0.299,  Kb = 0.114
 *   Rec. 709: Kr = 0.2126, Kb = 0.0722
 *   kr_v = 2(1 - Kr), kb_u = 2(1 - Kb),
 *   kg_u = kb_u * Kb / Kg, kg_v = kr_v * Kr / Kg, with Kg = 1 - Kr - Kb.
 *
 * Per-value contributions are precomputed in 16.16 fixed point for the 256
 * possible input values, so a conversion is five table lookups and additions.
 */
static int32_t table_y[256];  /* Y contribution, shared by R, G and B */
static int32_t table_rv[256]; /* V contribution to R */
static int32_t table_gu[256]; /* U contribution to G (negative) */
static int32_t table_gv[256]; /* V contribution to G (negative) */
static int32_t table_bu[256]; /* U contribution to B */
static bool    tables_ready;

/* Clamps channel values to 0..255 with a lookup, which is faster than two
 * comparisons. Values stay within -512..511: the largest Y contribution is
 * about 278 and the largest chroma one about 230. Index = value + 512. */
static uint8_t clamp_table[1024];
static enum color_matrix current_matrix;
static enum color_range  current_range;

void convert_set_colorimetry(enum color_matrix matrix, enum color_range range)
{
   const double kr = matrix == COLOR_MATRIX_REC709 ? 0.2126 : 0.299;
   const double kb = matrix == COLOR_MATRIX_REC709 ? 0.0722 : 0.114;
   const double kg = 1.0 - kr - kb;
   const double kr_v = 2.0 * (1.0 - kr);
   const double kb_u = 2.0 * (1.0 - kb);
   const double kg_u = kb_u * kb / kg;
   const double kg_v = kr_v * kr / kg;

   /* Limited range stretches 16..235 (Y) and 16..240 (U, V) to full scale. */
   const double y_offset = range == COLOR_RANGE_LIMITED ? 16.0 : 0.0;
   const double y_scale  = range == COLOR_RANGE_LIMITED ? 255.0 / 219.0 : 1.0;
   const double c_scale  = range == COLOR_RANGE_LIMITED ? 255.0 / 224.0 : 1.0;
   int i;

   if (tables_ready && matrix == current_matrix && range == current_range)
      return;

   for (i = 0; i < 1024; i++)
      clamp_table[i] = (uint8_t)(i < 512 ? 0 : (i > 512 + 255 ? 255 : i - 512));

   for (i = 0; i < 256; i++)
   {
      const double y = (i - y_offset) * y_scale;
      const double c = (i - 128.0) * c_scale;

      table_y[i]  = (int32_t)(y * 65536.0 + (y >= 0 ? 0.5 : -0.5));
      table_rv[i] = (int32_t)(kr_v * c * 65536.0);
      table_gu[i] = (int32_t)(-kg_u * c * 65536.0);
      table_gv[i] = (int32_t)(-kg_v * c * 65536.0);
      table_bu[i] = (int32_t)(kb_u * c * 65536.0);
   }

   current_matrix = matrix;
   current_range  = range;
   tables_ready   = true;
}

/* Rounds a 16.16 fixed-point channel value and clamps it to 0..255. */
static inline uint32_t clamp_channel(int32_t value)
{
   return clamp_table[((value + 32768) >> 16) + 512];
}

static inline uint32_t yuv_to_xrgb(uint32_t y, uint32_t u, uint32_t v)
{
   const int32_t base = table_y[y];
   const uint32_t r = clamp_channel(base + table_rv[v]);
   const uint32_t g = clamp_channel(base + table_gu[u] + table_gv[v]);
   const uint32_t b = clamp_channel(base + table_bu[u]);
   return (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------------------------ */
/* Native size: one conversion per pixel                                     */
/* ------------------------------------------------------------------------ */

/* Both formats share one U and V pair between two horizontal pixels, so the
 * chroma contributions are computed once per pair. `luma` and `chroma_plane`
 * point to the first row of the crop area; crop_x and crop_width are even. */
static void convert_native(const capture_frame_t *frame, const uint8_t *luma, const uint8_t *chroma_plane,
      unsigned crop_x, unsigned crop_width, unsigned height, uint32_t *dst)
{
   const bool nv12 = frame->format == CAPTURE_FORMAT_NV12;
   unsigned x, y;

   for (y = 0; y < height; y++)
   {
      uint32_t *out = dst + (size_t)y * crop_width;
      const uint8_t *row = luma + frame->stride * y;
      const uint8_t *chroma = nv12 ? chroma_plane + frame->stride * (y / 2) + crop_x : NULL;

      if (!nv12)
         row += crop_x * 2;
      else
         row += crop_x;

      for (x = 0; x < crop_width; x += 2)
      {
         uint32_t y0, y1, u, v;
         int32_t r, g, b, base;

         if (nv12)
         {
            y0 = row[x];
            y1 = row[x + 1];
            u  = chroma[x];
            v  = chroma[x + 1];
         }
         else
         {
            const uint8_t *pair = row + x * 2; /* Y0 U Y1 V */
            y0 = pair[0];
            u  = pair[1];
            y1 = pair[2];
            v  = pair[3];
         }

         r = table_rv[v];
         g = table_gu[u] + table_gv[v];
         b = table_bu[u];

         base = table_y[y0];
         out[x] = (clamp_channel(base + r) << 16) | (clamp_channel(base + g) << 8) | clamp_channel(base + b);
         base = table_y[y1];
         out[x + 1] = (clamp_channel(base + r) << 16) | (clamp_channel(base + g) << 8) | clamp_channel(base + b);
      }
   }
}

/* ------------------------------------------------------------------------ */
/* Resampling                                                                */
/* ------------------------------------------------------------------------ */

/* Source range [start, end) covered by each output column and row, in source
 * pixels relative to the crop area. */
static unsigned col_start[CAPTURE_MAX_WIDTH], col_end[CAPTURE_MAX_WIDTH];
static unsigned row_start[CAPTURE_MAX_HEIGHT], row_end[CAPTURE_MAX_HEIGHT];

/* Vertical sums of the source rows covered by one output row, one entry per
 * byte of the cropped row (YUY2 rows hold 2 bytes per pixel). */
static uint32_t luma_sums[CAPTURE_MAX_WIDTH * 2];
static uint32_t chroma_sums[CAPTURE_MAX_WIDTH]; /* NV12 only: interleaved U and V */

/* 16.16 fixed-point reciprocals, to average with a multiplication instead of
 * a division. Pixel counts per output pixel stay well below this size. */
#define RECIPROCALS 1024
static uint32_t reciprocal[RECIPROCALS];

static inline uint32_t average(uint32_t sum, uint32_t count)
{
   if (count < RECIPROCALS)
      return (sum * reciprocal[count] + 32768) >> 16;
   return (sum + count / 2) / count;
}

/* Splits src_size pixels into dst_size spans. */
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
      {
         e = src_size;
         s = e - 1;
      }

      start[i] = s;
      end[i]   = e;
   }
}

/* Adds rows [first, last) of a plane to `sums`, `bytes` bytes per row starting
 * at `offset`. The inner loop runs over contiguous memory, which compilers
 * vectorize. */
static void sum_rows(uint32_t *sums, const uint8_t *plane, size_t stride, size_t offset,
      unsigned bytes, unsigned first, unsigned last)
{
   unsigned row, i;

   memset(sums, 0, bytes * sizeof(uint32_t));
   for (row = first; row < last; row++)
   {
      const uint8_t *in = plane + stride * row + offset;
      for (i = 0; i < bytes; i++)
         sums[i] += in[i];
   }
}

/*
 * Box-filter resampling in two steps per output row:
 *   1. sum the covered source rows vertically, byte by byte (sum_rows());
 *   2. for each output pixel, add the vertical sums of its columns and divide.
 * Y, U and V are averaged separately, then converted to RGB. In both formats,
 * one U and V pair covers 2 pixels horizontally (and 2 rows in NV12).
 */
static void convert_resampled(const capture_frame_t *frame, const uint8_t *luma, const uint8_t *chroma_plane,
      unsigned crop_x, unsigned crop_width, unsigned height, uint32_t *dst, unsigned dst_width, unsigned dst_height)
{
   const bool nv12 = frame->format == CAPTURE_FORMAT_NV12;
   unsigned x, y, i;

   if (!reciprocal[1])
      for (i = 1; i < RECIPROCALS; i++)
         reciprocal[i] = (65536 + i / 2) / i;

   compute_spans(col_start, col_end, crop_width, dst_width);
   compute_spans(row_start, row_end, height, dst_height);

   for (y = 0; y < dst_height; y++)
   {
      const unsigned rows = row_end[y] - row_start[y];
      uint32_t *out = dst + (size_t)y * dst_width;

      if (nv12)
      {
         /* Chroma rows covering luma rows [start, end): [start / 2, ceil(end / 2)). */
         const unsigned chroma_first = row_start[y] / 2;
         const unsigned chroma_last  = (row_end[y] + 1) / 2;
         const unsigned chroma_rows  = chroma_last - chroma_first;

         sum_rows(luma_sums, luma, frame->stride, crop_x, crop_width, row_start[y], row_end[y]);
         sum_rows(chroma_sums, chroma_plane, frame->stride, crop_x, crop_width, chroma_first, chroma_last);

         for (x = 0; x < dst_width; x++)
         {
            /* Chroma pairs covering columns [start, end), as byte offsets. */
            const unsigned pair_first = col_start[x] & ~1u;
            const unsigned pair_last  = (col_end[x] + 1) & ~1u;
            uint32_t sy = 0, su = 0, sv = 0;
            unsigned sx;

            for (sx = col_start[x]; sx < col_end[x]; sx++)
               sy += luma_sums[sx];
            for (sx = pair_first; sx < pair_last; sx += 2)
            {
               su += chroma_sums[sx];
               sv += chroma_sums[sx + 1];
            }

            out[x] = yuv_to_xrgb(average(sy, rows * (col_end[x] - col_start[x])),
                  average(su, chroma_rows * (pair_last - pair_first) / 2),
                  average(sv, chroma_rows * (pair_last - pair_first) / 2));
         }
      }
      else /* YUY2: Y0 U Y1 V, so Y at even bytes, U at 4n + 1, V at 4n + 3 */
      {
         sum_rows(luma_sums, luma, frame->stride, (size_t)crop_x * 2, crop_width * 2,
               row_start[y], row_end[y]);

         for (x = 0; x < dst_width; x++)
         {
            const unsigned pair_first = col_start[x] & ~1u;
            const unsigned pair_last  = (col_end[x] + 1) & ~1u;
            uint32_t sy = 0, su = 0, sv = 0;
            unsigned sx;

            for (sx = col_start[x]; sx < col_end[x]; sx++)
               sy += luma_sums[sx * 2];
            for (sx = pair_first; sx < pair_last; sx += 2)
            {
               su += luma_sums[sx * 2 + 1];
               sv += luma_sums[sx * 2 + 3];
            }

            out[x] = yuv_to_xrgb(average(sy, rows * (col_end[x] - col_start[x])),
                  average(su, rows * (pair_last - pair_first) / 2),
                  average(sv, rows * (pair_last - pair_first) / 2));
         }
      }
   }
}

bool convert_yuv_frame(const capture_frame_t *frame, unsigned crop_x, unsigned crop_y,
      unsigned crop_width, unsigned crop_height, uint32_t *dst, unsigned dst_width, unsigned dst_height)
{
   const uint8_t *luma, *chroma_plane;

   if ((frame->format != CAPTURE_FORMAT_NV12 && frame->format != CAPTURE_FORMAT_YUY2) ||
       !crop_width || !crop_height || (crop_x | crop_y | crop_width) & 1 ||
       crop_x + crop_width > frame->width || crop_y + crop_height > frame->height ||
       !dst_width || !dst_height || dst_width > CAPTURE_MAX_WIDTH || dst_height > CAPTURE_MAX_HEIGHT)
      return false;

   if (!tables_ready)
      convert_set_colorimetry(COLOR_MATRIX_REC709, COLOR_RANGE_LIMITED);

   /* First row of the crop area. NV12 chroma rows cover 2 luma rows each,
    * hence the even crop_y. */
   luma         = frame->data + frame->stride * crop_y;
   chroma_plane = frame->data + frame->stride * frame->height + frame->stride * (crop_y / 2);

   if (dst_width == crop_width && dst_height == crop_height)
      convert_native(frame, luma, chroma_plane, crop_x, crop_width, crop_height, dst);
   else
      convert_resampled(frame, luma, chroma_plane, crop_x, crop_width, crop_height, dst, dst_width, dst_height);

   return true;
}

/* ------------------------------------------------------------------------ */
/* Black frame detection                                                     */
/* ------------------------------------------------------------------------ */

/* True if every byte in [p, p + count) at the given step equals `value`. */
static bool all_bytes_equal(const uint8_t *p, size_t count, size_t step, uint8_t value)
{
   size_t i;
   for (i = 0; i < count; i += step)
      if (p[i] != value)
         return false;
   return true;
}

bool convert_frame_is_black(const capture_frame_t *frame)
{
   const size_t luma_size = frame->stride * frame->height;
   unsigned y, x;

   if (!frame->width || !frame->height)
      return false;

   switch (frame->format)
   {
      case CAPTURE_FORMAT_NV12:
         /* Black is Y at 16 (limited) or 0 (full), with neutral chroma (128). */
         return frame->data[0] <= 16 &&
                all_bytes_equal(frame->data, luma_size, 1, frame->data[0]) &&
                all_bytes_equal(frame->data + luma_size, frame->stride * ((frame->height + 1) / 2), 1, 128);

      case CAPTURE_FORMAT_YUY2:
         /* Even bytes are Y, odd bytes are U and V. */
         return frame->data[0] <= 16 &&
                all_bytes_equal(frame->data, luma_size, 2, frame->data[0]) &&
                all_bytes_equal(frame->data + 1, luma_size - 1, 2, 128);

      case CAPTURE_FORMAT_XRGB8888:
      default:
         for (y = 0; y < frame->height; y++)
         {
            const uint32_t *row = (const uint32_t *)(frame->data + frame->stride * y);
            for (x = 0; x < frame->width; x++)
               if (row[x] & 0x00FFFFFF)
                  return false;
         }
         return true;
   }
}
