/*
 * The No Signal screen (see no_signal.h).
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "capture.h"
#include "font.h"
#include "no_signal.h"

static uint32_t screen_buffer[NO_SIGNAL_WIDTH * NO_SIGNAL_HEIGHT];
static char     screen_key[1024]; /* Content of the screen last drawn */

void no_signal_invalidate(void)
{
   screen_key[0] = '\0';
}

/* 75% SMPTE color bars: white, yellow, cyan, green, magenta, red, blue. */
static const uint32_t bar_colors[7] = {
   0xBFBFBF, 0xBFBF00, 0x00BFBF, 0x00BF00, 0xBF00BF, 0xBF0000, 0x0000BF
};

/* Splits `text` into lines of at most `max_chars` characters, breaking at
 * spaces. Returns the number of lines written to `lines`. */
static int wrap_text(const char *text, unsigned max_chars, char lines[][96], int max_lines)
{
   int count = 0;

   while (*text && count < max_lines)
   {
      size_t len = strlen(text);
      size_t cut = len;

      if (len > max_chars)
      {
         cut = max_chars;
         while (cut > 0 && text[cut] != ' ')
            cut--;
         if (cut == 0)
            cut = max_chars; /* A single word longer than a line */
      }

      if (cut > 95)
         cut = 95;
      memcpy(lines[count], text, cut);
      lines[count][cut] = '\0';
      count++;

      text += cut;
      while (*text == ' ')
         text++;
   }

   return count;
}

/* Draws text centered horizontally at row y. */
static void draw_centered(const char *text, int y, unsigned scale, uint32_t color)
{
   const int x = ((int)NO_SIGNAL_WIDTH - (int)font_text_width(text, scale)) / 2;
   font_draw_text(screen_buffer, NO_SIGNAL_WIDTH * sizeof(uint32_t), NO_SIGNAL_WIDTH, NO_SIGNAL_HEIGHT,
         x, y, text, scale, color);
}

/* Returns the reason text and a troubleshooting hint for a signal state. */
static void describe_signal_state(enum signal_state state, char *reason, size_t reason_size,
      char *hint, size_t hint_size)
{
   switch (state)
   {
      case SIGNAL_NO_DEVICE:
         snprintf(reason, reason_size, "No capture device was found.");
         snprintf(hint, hint_size, "Connect the device, then restart the core. "
               "Devices are detected when the core starts.");
         break;
      case SIGNAL_DEVICE_ERROR:
         snprintf(reason, reason_size, "%s", capture_last_error()[0]
               ? capture_last_error() : "The device could not be opened.");
         snprintf(hint, hint_size, "The core tries to reopen the device every few seconds.");
         break;
      case SIGNAL_BLACK:
         snprintf(reason, reason_size, "The device is connected, but receives no picture from its source.");
         snprintf(hint, hint_size, "Check that the source is turned on, that its cable is plugged into "
               "the input selected in Capture Device, and that its output resolution is supported. "
               "Another application, such as OBS, may also be using the device.");
         break;
      case SIGNAL_NO_FRAMES:
      default:
         snprintf(reason, reason_size, "The device is open, but sends no frames.");
         snprintf(hint, hint_size, "Check the source and its cable. Another application, such as OBS, "
               "may also be using the device. If the problem persists, "
               "try Color Conversion set to Media Foundation, or reconnect the device.");
         break;
   }
}

/* Draws the No Signal screen: color bars with a black panel in the middle
 * showing the title, the device name and, optionally, the details. The screen
 * is redrawn only when its content changes. */
const uint32_t *no_signal_render(enum signal_state state, bool details, const char *device_name)
{
   const char *title = state == SIGNAL_NO_DEVICE ? "NO DEVICE"
                     : state == SIGNAL_DEVICE_ERROR ? "DEVICE ERROR"
                     : "NO SIGNAL";
   char reason[512], hint[512], key[sizeof(screen_key)];
   char lines[12][96];
   const int bars_height = (NO_SIGNAL_HEIGHT * 2) / 3;
   const int panel_x = 40, panel_w = NO_SIGNAL_WIDTH - 80;
   int line_count = 0, panel_h, panel_y, y, x, i;

   describe_signal_state(state, reason, sizeof(reason), hint, sizeof(hint));
   snprintf(key, sizeof(key), "%d|%d|%s|%s", (int)state, (int)details, device_name, reason);
   if (strcmp(key, screen_key) == 0)
      return screen_buffer;
   snprintf(screen_key, sizeof(screen_key), "%s", key);

   /* Background: color bars over a luminance ramp. */
   for (y = 0; y < NO_SIGNAL_HEIGHT; y++)
   {
      uint32_t *row = &screen_buffer[y * NO_SIGNAL_WIDTH];
      for (x = 0; x < NO_SIGNAL_WIDTH; x++)
      {
         uint32_t v = (uint32_t)((x * 255) / (NO_SIGNAL_WIDTH - 1));
         row[x] = y < bars_height ? bar_colors[(x * 7) / NO_SIGNAL_WIDTH] : (v << 16) | (v << 8) | v;
      }
   }

   /* Detail lines, wrapped to the panel width (8 px characters). */
   if (details)
   {
      line_count  = wrap_text(reason, (unsigned)(panel_w - 32) / FONT_GLYPH_SIZE, lines, 6);
      lines[line_count++][0] = '\0'; /* Blank line */
      line_count += wrap_text(hint, (unsigned)(panel_w - 32) / FONT_GLYPH_SIZE, lines + line_count, 5);
   }
   else
   {
      snprintf(lines[0], sizeof(lines[0]), "Set No Signal Details to On in Core Options for help.");
      line_count = 1;
   }

   /* Panel: title (32 px), device name (16 px), then the detail lines (12 px each). */
   panel_h = 24 + 32 + 16 + (device_name[0] ? 24 : 0) + 8 + line_count * 12 + 16;
   panel_y = (NO_SIGNAL_HEIGHT - panel_h) / 2;
   font_fill_rect(screen_buffer, NO_SIGNAL_WIDTH * sizeof(uint32_t), NO_SIGNAL_WIDTH, NO_SIGNAL_HEIGHT,
         panel_x, panel_y, panel_w, panel_h, 0x000000);

   y = panel_y + 24;
   draw_centered(title, y, 4, 0xFFFFFF);
   y += 32 + 16;

   if (device_name[0])
   {
      char name[40];
      snprintf(name, sizeof(name), "%.36s%s", device_name, strlen(device_name) > 36 ? "..." : "");
      draw_centered(name, y, 2, 0xC0C0C0);
      y += 24;
   }

   y += 8;
   for (i = 0; i < line_count; i++, y += 12)
      font_draw_text(screen_buffer, NO_SIGNAL_WIDTH * sizeof(uint32_t), NO_SIGNAL_WIDTH, NO_SIGNAL_HEIGHT,
            panel_x + 16, y, lines[i], 1, 0xA0A0A0);

   return screen_buffer;
}

