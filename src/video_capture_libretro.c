/*
 * video-capture-libretro
 * A libretro core that displays a live video capture device (capture card,
 * HDMI grabber, webcam) so RetroArch shaders can be applied to it.
 *
 * The core runs without content. It shows the selected capture device, or an
 * animated test pattern when no device is available or no frame arrives.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "libretro.h"
#include "capture.h"
#include "scale.h"

#ifndef CORE_VERSION
#define CORE_VERSION "0.0.0"
#endif

#define CORE_NAME     "Video Capture Device"
#define LOG_PREFIX    "[Video Capture Device] "

#define PATTERN_WIDTH  640
#define PATTERN_HEIGHT 480
#define DEFAULT_FPS    60.0
#define SAMPLE_RATE    48000.0
#define AUDIO_FRAMES_MAX 4800 /* Silence buffer: enough for frame rates down to 10 fps */

/* Frames without a new capture frame before falling back to the test pattern. */
#define SIGNAL_TIMEOUT_FRAMES 120

#define OPTION_DEVICE     "video_capture_device"
#define OPTION_RESOLUTION "video_capture_resolution"
#define OPTION_ASPECT     "video_capture_aspect"
#define DEVICE_AUTO       "auto"

enum output_resolution
{
   RESOLUTION_NATIVE = 0,
   RESOLUTION_640X480,
   RESOLUTION_320X240
};

enum aspect_mode
{
   ASPECT_SOURCE = 0,  /* Display aspect follows the signal (16:9 for 1080p) */
   ASPECT_4_3_SQUASH,  /* Full frame, displayed as 4:3 */
   ASPECT_4_3_CROP,    /* Central 4:3 area only, pillarbox removed */
   ASPECT_16_9         /* Full frame, displayed as 16:9 */
};

/* Frontend callbacks */
static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

/* Test pattern */
static uint32_t pattern_buffer[PATTERN_WIDTH * PATTERN_HEIGHT];
static int16_t  silence[AUDIO_FRAMES_MAX * 2];
static double   audio_frames_pending; /* Fractional audio frames carried between video frames */
static uint64_t frame_count;

/* Capture state */
static capture_device_t devices[CAPTURE_MAX_DEVICES];
static int              device_count;
static char             active_device_id[sizeof(devices[0].id)];
static uint32_t        *capture_buffer;
static unsigned         capture_width;
static unsigned         capture_height;
static uint64_t         capture_sequence;
static unsigned         frames_without_capture;
static bool             showing_capture;
static bool             can_dupe;

/* Image processing */
static uint32_t              *scaled_buffer;
static enum output_resolution resolution_option = RESOLUTION_NATIVE;
static enum aspect_mode       aspect_option     = ASPECT_SOURCE;

/* Geometry currently reported to the frontend */
static unsigned output_width  = PATTERN_WIDTH;
static unsigned output_height = PATTERN_HEIGHT;
static float    output_aspect = 4.0f / 3.0f;
static double   output_fps    = DEFAULT_FPS;

/* Last frame sent, repeated when the device has no new frame yet */
static const uint32_t *last_output;

/* Core option storage. Device names are used as option values. */
static char option_device_names[CAPTURE_MAX_DEVICES][sizeof(devices[0].name) + 16];
static struct retro_core_option_v2_definition option_definitions[4];
static struct retro_core_option_v2_category   option_categories[1];
static char legacy_device_values[CAPTURE_MAX_DEVICES * (sizeof(devices[0].name) + 17) + 64];
static struct retro_variable legacy_variables[4];

/* Fallback logger used when the frontend does not provide one. */
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

/* Forwards backend messages to the frontend log. */
static void capture_logger(enum capture_log_level level, const char *message)
{
   enum retro_log_level retro_level;

   switch (level)
   {
      case CAPTURE_LOG_DEBUG: retro_level = RETRO_LOG_DEBUG; break;
      case CAPTURE_LOG_INFO:  retro_level = RETRO_LOG_INFO;  break;
      case CAPTURE_LOG_WARN:  retro_level = RETRO_LOG_WARN;  break;
      default:                retro_level = RETRO_LOG_ERROR; break;
   }

   if (log_cb)
      log_cb(retro_level, LOG_PREFIX "%s\n", message);
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
   const int bars_height = (PATTERN_HEIGHT * 2) / 3;
   const uint64_t fps    = (uint64_t)(output_fps + 0.5);
   const int sweep_y     = (int)((frame_count % fps) * PATTERN_HEIGHT / fps);
   int x, y;

   for (y = 0; y < PATTERN_HEIGHT; y++)
   {
      uint32_t *row = &pattern_buffer[y * PATTERN_WIDTH];

      for (x = 0; x < PATTERN_WIDTH; x++)
      {
         uint32_t color;

         if (y < bars_height)
            color = bar_colors[(x * 7) / PATTERN_WIDTH];
         else
         {
            /* Bottom third: black-to-white luminance ramp. */
            uint32_t v = (uint32_t)((x * 255) / (PATTERN_WIDTH - 1));
            color = (v << 16) | (v << 8) | v;
         }

         if (y == sweep_y || y == sweep_y + 1)
            color = 0xFFFFFF;

         row[x] = color;
      }
   }
}

/* ------------------------------------------------------------------------ */
/* Core options                                                              */
/* ------------------------------------------------------------------------ */

/* Enumerates devices and gives each a unique name usable as an option value. */
static void refresh_device_list(void)
{
   int i, j;

   device_count = capture_list_devices(devices, CAPTURE_MAX_DEVICES);

   /* Option values must be unique; number identical devices ("Name #2"). */
   for (i = 0; i < device_count; i++)
   {
      int duplicates = 0;

      for (j = 0; j < i; j++)
         if (strcmp(devices[i].name, devices[j].name) == 0)
            duplicates++;

      if (duplicates)
         snprintf(option_device_names[i], sizeof(option_device_names[i]), "%.255s #%d",
               devices[i].name, duplicates + 1);
      else
         snprintf(option_device_names[i], sizeof(option_device_names[i]), "%.255s", devices[i].name);
   }

   if (log_cb)
   {
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "%d capture device(s) found.\n", device_count);
      for (i = 0; i < device_count; i++)
         log_cb(RETRO_LOG_INFO, LOG_PREFIX "  %s\n", option_device_names[i]);
   }
}

static void declare_core_options(void)
{
   unsigned version = 0;
   int i;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
      struct retro_core_options_v2 options;
      struct retro_core_option_v2_definition *def = &option_definitions[0];
      int max_devices = RETRO_NUM_CORE_OPTION_VALUES_MAX - 2;

      memset(option_definitions, 0, sizeof(option_definitions));
      memset(option_categories, 0, sizeof(option_categories));

      def->key  = OPTION_DEVICE;
      def->desc = "Capture Device";
      def->info = "Video capture device to display. Auto selects the first device found. "
                  "Devices are detected when the core starts.";
      def->values[0].value = DEVICE_AUTO;
      def->values[0].label = "Auto (first device)";
      for (i = 0; i < device_count && i < max_devices; i++)
      {
         def->values[i + 1].value = option_device_names[i];
         def->values[i + 1].label = NULL;
      }
      def->default_value = DEVICE_AUTO;

      def = &option_definitions[1];
      def->key  = OPTION_RESOLUTION;
      def->desc = "Output Resolution";
      def->info = "Resolution of the image passed to RetroArch and its shaders. "
                  "CRT and VHS shaders expect a low resolution such as 640x480 or 320x240. "
                  "Downscaling averages the source pixels to avoid aliasing.";
      def->values[0].value = "native";
      def->values[0].label = "Native";
      def->values[1].value = "640x480";
      def->values[1].label = NULL;
      def->values[2].value = "320x240";
      def->values[2].label = NULL;
      def->default_value = "native";

      def = &option_definitions[2];
      def->key  = OPTION_ASPECT;
      def->desc = "Aspect Ratio";
      def->info = "Display aspect ratio reported to RetroArch. "
                  "4:3 (Squash) keeps the whole frame, for a 4:3 picture stretched to 16:9. "
                  "4:3 (Crop Pillarbox) keeps only the central 4:3 area, for a picture with black bars on the sides. "
                  "Requires Settings > Video > Scaling > Aspect Ratio set to Core Provided; a shader preset may override it.";
      def->values[0].value = "source";
      def->values[0].label = "Source";
      def->values[1].value = "4_3_squash";
      def->values[1].label = "4:3 (Squash)";
      def->values[2].value = "4_3_crop";
      def->values[2].label = "4:3 (Crop Pillarbox)";
      def->values[3].value = "16_9";
      def->values[3].label = "16:9";
      def->default_value = "source";

      options.categories  = option_categories;
      options.definitions = option_definitions;
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
   }
   else
   {
      /* Legacy format: "Description; value1|value2|...". */
      size_t len = (size_t)snprintf(legacy_device_values, sizeof(legacy_device_values),
            "Capture Device; %s", DEVICE_AUTO);

      for (i = 0; i < device_count && len < sizeof(legacy_device_values); i++)
         len += (size_t)snprintf(legacy_device_values + len, sizeof(legacy_device_values) - len,
               "|%s", option_device_names[i]);

      legacy_variables[0].key   = OPTION_DEVICE;
      legacy_variables[0].value = legacy_device_values;
      legacy_variables[1].key   = OPTION_RESOLUTION;
      legacy_variables[1].value = "Output Resolution; native|640x480|320x240";
      legacy_variables[2].key   = OPTION_ASPECT;
      legacy_variables[2].value = "Aspect Ratio; source|4_3_squash|4_3_crop|16_9";
      legacy_variables[3].key   = NULL;
      legacy_variables[3].value = NULL;
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, legacy_variables);
   }
}

/* Returns the id of the device selected in the core options, or NULL. */
static const char *selected_device_id(void)
{
   struct retro_variable var;
   int i;

   if (device_count == 0)
      return NULL;

   var.key   = OPTION_DEVICE;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
       strcmp(var.value, DEVICE_AUTO) != 0)
   {
      for (i = 0; i < device_count; i++)
         if (strcmp(var.value, option_device_names[i]) == 0)
            return devices[i].id;

      if (log_cb)
         log_cb(RETRO_LOG_WARN, LOG_PREFIX "Device \"%s\" not found, using the first device.\n", var.value);
   }

   return devices[0].id;
}

/* Reads the resolution and aspect ratio options. */
static void read_image_options(void)
{
   struct retro_variable var;

   var.key   = OPTION_RESOLUTION;
   var.value = NULL;
   resolution_option = RESOLUTION_NATIVE;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "640x480") == 0)
         resolution_option = RESOLUTION_640X480;
      else if (strcmp(var.value, "320x240") == 0)
         resolution_option = RESOLUTION_320X240;
   }

   var.key   = OPTION_ASPECT;
   var.value = NULL;
   aspect_option = ASPECT_SOURCE;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "4_3_squash") == 0)
         aspect_option = ASPECT_4_3_SQUASH;
      else if (strcmp(var.value, "4_3_crop") == 0)
         aspect_option = ASPECT_4_3_CROP;
      else if (strcmp(var.value, "16_9") == 0)
         aspect_option = ASPECT_16_9;
   }
}

/* Opens the selected device if it differs from the active one. */
static void apply_device_selection(bool force)
{
   const char *id = selected_device_id();

   if (!id)
   {
      if (log_cb)
         log_cb(RETRO_LOG_WARN, LOG_PREFIX "No capture device found, showing the test pattern.\n");
      return;
   }

   if (!force && strcmp(id, active_device_id) == 0)
      return;

   snprintf(active_device_id, sizeof(active_device_id), "%s", id);
   showing_capture        = false;
   frames_without_capture = 0;

   if (capture_start(id))
   {
      if (log_cb)
         log_cb(RETRO_LOG_INFO, LOG_PREFIX "Capture started.\n");
   }
   else if (log_cb)
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "Could not start capture, showing the test pattern.\n");
}

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

/* Tells the frontend when the output size or display aspect ratio changes. */
static void set_output_geometry(unsigned width, unsigned height, float aspect)
{
   struct retro_game_geometry geometry;

   if (width == output_width && height == output_height && aspect == output_aspect)
      return;

   output_width  = width;
   output_height = height;
   output_aspect = aspect;

   geometry.base_width   = width;
   geometry.base_height  = height;
   geometry.max_width    = CAPTURE_MAX_WIDTH;
   geometry.max_height   = CAPTURE_MAX_HEIGHT;
   geometry.aspect_ratio = aspect;
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);

   if (log_cb)
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "Output: %ux%u, aspect ratio %.4f.\n", width, height, aspect);
}

/* Returns the device frame rate, or the current one if unknown or implausible. */
static double device_fps(void)
{
   double fps = capture_get_frame_rate();
   return (fps >= 10.0 && fps <= 240.0) ? fps : output_fps;
}

/* Reports the device frame rate to the frontend when it changes, so RetroArch
 * paces retro_run() on the capture device instead of a fixed 60 Hz. */
static void update_frame_rate(void)
{
   struct retro_system_av_info av_info;
   double fps = device_fps();

   if (fps == output_fps)
      return;

   output_fps = fps;

   memset(&av_info, 0, sizeof(av_info));
   av_info.geometry.base_width   = output_width;
   av_info.geometry.base_height  = output_height;
   av_info.geometry.max_width    = CAPTURE_MAX_WIDTH;
   av_info.geometry.max_height   = CAPTURE_MAX_HEIGHT;
   av_info.geometry.aspect_ratio = output_aspect;
   av_info.timing.fps            = output_fps;
   av_info.timing.sample_rate    = SAMPLE_RATE;
   environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);

   if (log_cb)
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "Frame rate: %.3f fps.\n", output_fps);
}

/* Applies the crop, resolution and aspect ratio options to a captured frame,
 * then sends it to the frontend. */
static void output_capture_frame(void)
{
   unsigned crop_x      = 0;
   unsigned crop_width  = capture_width;
   unsigned dst_width   = capture_width;
   unsigned dst_height  = capture_height;
   float aspect;
   const uint32_t *out;

   /* Crop the central 4:3 area when the source is wider than 4:3. */
   if (aspect_option == ASPECT_4_3_CROP && capture_width * 3 > capture_height * 4)
   {
      crop_width = (capture_height * 4 + 1) / 3;
      crop_x     = (capture_width - crop_width) / 2;
   }

   switch (resolution_option)
   {
      case RESOLUTION_640X480: dst_width = 640; dst_height = 480; break;
      case RESOLUTION_320X240: dst_width = 320; dst_height = 240; break;
      default:                 dst_width = crop_width; dst_height = capture_height; break;
   }

   switch (aspect_option)
   {
      case ASPECT_4_3_SQUASH:
      case ASPECT_4_3_CROP: aspect = 4.0f / 3.0f; break;
      case ASPECT_16_9:     aspect = 16.0f / 9.0f; break;
      default:              aspect = (float)crop_width / (float)capture_height; break;
   }

   if (crop_x == 0 && dst_width == capture_width && dst_height == capture_height)
      out = capture_buffer;
   else
   {
      scale_area(capture_buffer, capture_width, crop_x, 0, crop_width, capture_height,
            scaled_buffer, dst_width, dst_height);
      out = scaled_buffer;
   }

   set_output_geometry(dst_width, dst_height, aspect);
   last_output = out;
   video_cb(out, dst_width, dst_height, dst_width * sizeof(uint32_t));
}

static void present_frame(void)
{
   bool new_frame = false;

   if (capture_buffer && capture_is_running())
      new_frame = capture_fetch_frame(capture_buffer, &capture_width, &capture_height, &capture_sequence);

   if (new_frame)
   {
      showing_capture        = true;
      frames_without_capture = 0;
      update_frame_rate();
      output_capture_frame();
      return;
   }

   if (showing_capture && ++frames_without_capture < SIGNAL_TIMEOUT_FRAMES)
   {
      /* The device runs at its own pace: repeat the previous frame. */
      if (can_dupe)
         video_cb(NULL, output_width, output_height, output_width * sizeof(uint32_t));
      else
         video_cb(last_output, output_width, output_height, output_width * sizeof(uint32_t));
      return;
   }

   if (showing_capture && log_cb)
      log_cb(RETRO_LOG_WARN, LOG_PREFIX "No frames received, showing the test pattern.\n");
   showing_capture = false;

   render_test_pattern();
   set_output_geometry(PATTERN_WIDTH, PATTERN_HEIGHT, 4.0f / 3.0f);
   last_output = pattern_buffer;
   video_cb(pattern_buffer, PATTERN_WIDTH, PATTERN_HEIGHT, PATTERN_WIDTH * sizeof(uint32_t));
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

   capture_set_logger(capture_logger);

   /* The core is started with "Start Core"; no content file is needed. */
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

   refresh_device_list();
   declare_core_options();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)           { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)             { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)                 { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)               { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
   frame_count = 0;
   memset(silence, 0, sizeof(silence));
}

RETRO_API void retro_deinit(void)
{
   capture_stop();
   free(capture_buffer);
   free(scaled_buffer);
   capture_buffer = NULL;
   scaled_buffer  = NULL;
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
   info->geometry.base_width   = output_width;
   info->geometry.base_height  = output_height;
   info->geometry.max_width    = CAPTURE_MAX_WIDTH;
   info->geometry.max_height   = CAPTURE_MAX_HEIGHT;
   info->geometry.aspect_ratio = output_aspect;
   info->timing.fps            = output_fps;
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
   apply_device_selection(true);
}

RETRO_API void retro_run(void)
{
   bool updated = false;

   if (input_poll_cb)
      input_poll_cb();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      read_image_options();
      apply_device_selection(false);

      /* Show the new settings right away, even before the next frame. */
      if (showing_capture)
         output_capture_frame();
      else
         present_frame();
   }
   else
      present_frame();

   /* Silent audio matching the frame rate (800.8 frames per video frame at 59.94 fps). */
   if (audio_batch_cb)
   {
      size_t frames;

      audio_frames_pending += SAMPLE_RATE / output_fps;
      frames = (size_t)audio_frames_pending;
      if (frames > AUDIO_FRAMES_MAX)
         frames = AUDIO_FRAMES_MAX;
      audio_frames_pending -= (double)frames;
      if (audio_frames_pending > AUDIO_FRAMES_MAX)
         audio_frames_pending = 0.0;

      audio_batch_cb(silence, frames);
   }

   frame_count++;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   (void)game;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "XRGB8888 is not supported by the frontend.\n");
      return false;
   }

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe))
      can_dupe = false;

   if (!capture_buffer)
      capture_buffer = (uint32_t *)malloc((size_t)CAPTURE_MAX_WIDTH * CAPTURE_MAX_HEIGHT * sizeof(uint32_t));
   if (!scaled_buffer)
      scaled_buffer = (uint32_t *)malloc((size_t)CAPTURE_MAX_WIDTH * CAPTURE_MAX_HEIGHT * sizeof(uint32_t));
   if (!capture_buffer || !scaled_buffer)
   {
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "Out of memory.\n");
      return false;
   }

   log_cb(RETRO_LOG_INFO, LOG_PREFIX "Core started (version %s).\n", CORE_VERSION);

   active_device_id[0] = '\0';
   capture_sequence    = 0;
   read_image_options();
   apply_device_selection(true);

   /* retro_get_system_av_info() is called after this: report the device rate directly. */
   output_fps = device_fps();
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
   capture_stop();
   active_device_id[0] = '\0';
   showing_capture     = false;
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
