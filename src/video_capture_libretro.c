/*
 * video-capture-libretro
 * A libretro core that displays a live video capture device (capture card,
 * HDMI grabber, webcam) so RetroArch shaders can be applied to it.
 *
 * Stage 0: skeleton only. The core runs without content and renders an
 * animated test pattern (SMPTE-style color bars) with silent audio, to
 * validate that RetroArch loads it and calls retro_run() at the expected rate.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "libretro.h"

#ifndef CORE_VERSION
#define CORE_VERSION "0.0.1"
#endif

#define CORE_NAME     "Video Capture"
#define OUT_WIDTH     640
#define OUT_HEIGHT    480
#define OUT_FPS       60.0
#define SAMPLE_RATE   48000.0
#define AUDIO_FRAMES  800 /* SAMPLE_RATE / OUT_FPS stereo frames per video frame */

/* Frontend callbacks */
static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

/* Core state */
static uint32_t framebuffer[OUT_WIDTH * OUT_HEIGHT];
static int16_t  silence[AUDIO_FRAMES * 2];
static uint64_t frame_count;

/* Fallback logger used when the frontend does not provide one. */
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

/* ------------------------------------------------------------------------ */
/* Test pattern                                                              */
/* ------------------------------------------------------------------------ */

/* 75% SMPTE color bars: white, yellow, cyan, green, magenta, red, blue. */
static const uint32_t bar_colors[7] = {
   0xBFBFBF, 0xBFBF00, 0x00BFBF, 0x00BF00, 0xBF00BF, 0xBF0000, 0x0000BF
};

/* Draws the color bars plus a white line that sweeps down the screen once
 * per second, so a frozen image or uneven pacing is easy to spot. */
static void render_test_pattern(void)
{
   const int bars_height = (OUT_HEIGHT * 2) / 3;
   const int sweep_y     = (int)((frame_count % (uint64_t)OUT_FPS) * OUT_HEIGHT / (uint64_t)OUT_FPS);
   int x, y;

   for (y = 0; y < OUT_HEIGHT; y++)
   {
      uint32_t *row = &framebuffer[y * OUT_WIDTH];

      for (x = 0; x < OUT_WIDTH; x++)
      {
         uint32_t color;

         if (y < bars_height)
            color = bar_colors[(x * 7) / OUT_WIDTH];
         else
         {
            /* Bottom third: black-to-white luminance ramp. */
            uint32_t v = (uint32_t)((x * 255) / (OUT_WIDTH - 1));
            color = (v << 16) | (v << 8) | v;
         }

         if (y == sweep_y || y == sweep_y + 1)
            color = 0xFFFFFF;

         row[x] = color;
      }
   }
}

/* ------------------------------------------------------------------------ */
/* libretro API                                                              */
/* ------------------------------------------------------------------------ */

RETRO_API unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback logging;
   bool no_game = true;

   environ_cb = cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log)
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   /* The core is started with "Start Core"; no content file is needed. */
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)         { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)           { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)               { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)             { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
   frame_count = 0;
   memset(silence, 0, sizeof(silence));
}

RETRO_API void retro_deinit(void)
{
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = CORE_NAME;
   info->library_version  = CORE_VERSION;
   info->valid_extensions = "";
   info->need_fullpath    = false;
   info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = OUT_WIDTH;
   info->geometry.base_height  = OUT_HEIGHT;
   info->geometry.max_width    = OUT_WIDTH;
   info->geometry.max_height   = OUT_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = OUT_FPS;
   info->timing.sample_rate    = SAMPLE_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

RETRO_API void retro_reset(void)
{
   frame_count = 0;
}

RETRO_API void retro_run(void)
{
   if (input_poll_cb)
      input_poll_cb();

   render_test_pattern();
   video_cb(framebuffer, OUT_WIDTH, OUT_HEIGHT, OUT_WIDTH * sizeof(uint32_t));

   if (audio_batch_cb)
      audio_batch_cb(silence, AUDIO_FRAMES);

   frame_count++;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   (void)game;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "[Video Capture] XRGB8888 is not supported by the frontend.\n");
      return false;
   }

   log_cb(RETRO_LOG_INFO, "[Video Capture] Core started (version %s, test pattern mode).\n", CORE_VERSION);
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

RETRO_API void retro_unload_game(void)
{
}

RETRO_API unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

/* No save states, memory regions or cheats for a live capture source. */
RETRO_API size_t retro_serialize_size(void)                          { return 0; }
RETRO_API bool   retro_serialize(void *data, size_t size)            { (void)data; (void)size; return false; }
RETRO_API bool   retro_unserialize(const void *data, size_t size)    { (void)data; (void)size; return false; }
RETRO_API void  *retro_get_memory_data(unsigned id)                  { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id)                  { (void)id; return 0; }
RETRO_API void   retro_cheat_reset(void)                             { }
RETRO_API void   retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
