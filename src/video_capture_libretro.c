/*
 * video-capture-libretro
 * A libretro core that displays a live video capture device (capture card,
 * HDMI grabber, webcam) so RetroArch shaders can be applied to it.
 *
 * The core runs without content, or with a .vidcap file: an empty file that
 * lets RetroArch save core options and shader presets per "game", for example
 * one file per console or VCR connected to the capture device.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

/* fopen() is used for the capture stats log file. */
#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#include "libretro.h"
#include "capture.h"
#include "convert.h"
#include "font.h"
#include "scale.h"

#ifndef CORE_VERSION
#define CORE_VERSION "0.0.0"
#endif

#define CORE_NAME  "Video Capture Device"
#define LOG_PREFIX "[Video Capture Device] "

#define SCREEN_WIDTH  640 /* No Signal screen size */
#define SCREEN_HEIGHT 480
#define DEFAULT_FPS   60.0
#define SAMPLE_RATE   48000.0
#define AUDIO_FRAMES_MAX 4800 /* Silence buffer: enough for frame rates down to 10 fps */

/* Seconds without frames before showing the No Signal screen. */
#define NO_FRAMES_TIMEOUT 2.0
/* Seconds of uniformly black frames before showing the No Signal screen. */
#define BLACK_TIMEOUT     1.0
/* Seconds between attempts to reopen a device that failed. */
#define RETRY_INTERVAL    3.0

/* Smooth frame pacing (see pick_frame()). A frame arriving within this many
 * milliseconds of retro_run() is in the "collision zone", where timing noise
 * decides whether it is ready in time. */
#define PACING_GUARD_MS   2.5
/* retro_run() calls outside the collision zone before catching up one frame. */
#define PACING_SKIP_RUNS  30

/* ------------------------------------------------------------------------ */
/* Core options                                                              */
/* ------------------------------------------------------------------------ */

#define OPTION_DEVICE      "video_capture_device"
#define OPTION_RESOLUTION  "video_capture_resolution"
#define OPTION_ASPECT      "video_capture_aspect"
#define OPTION_CONVERSION  "video_capture_conversion"
#define OPTION_COLOR_SPACE "video_capture_color_space"
#define OPTION_COLOR_RANGE "video_capture_color_range"
#define OPTION_STATS       "video_capture_stats"
#define OPTION_DETAILS     "video_capture_no_signal_details"
#define OPTION_PACING      "video_capture_frame_pacing"
#define OPTION_STATS_LOG   "video_capture_stats_log"
#define DEVICE_AUTO        "auto"

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

enum color_space_option
{
   COLOR_SPACE_DEFAULT = 0, /* Rec. 601 up to 576 lines, Rec. 709 above */
   COLOR_SPACE_REC709,
   COLOR_SPACE_REC601
};

enum stats_mode
{
   STATS_OFF = 0,
   STATS_NOTIFICATION, /* RetroArch on-screen notification */
   STATS_IMAGE         /* Drawn into the image, before shaders */
};

/* Current option values */
static enum output_resolution  opt_resolution  = RESOLUTION_NATIVE;
static enum aspect_mode        opt_aspect      = ASPECT_SOURCE;
static bool                    opt_core_convert = true;
static enum color_space_option opt_color_space = COLOR_SPACE_DEFAULT;
static enum color_range        opt_color_range = COLOR_RANGE_LIMITED;
static enum stats_mode         opt_stats       = STATS_OFF;
static bool                    opt_details     = false;
static bool                    opt_smooth_pacing = false;
static bool                    opt_stats_log   = false;

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

/* Frontend callbacks */
static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;
static struct retro_perf_callback perf_cb;
static unsigned                   message_interface_version;
static bool                       can_dupe;

/* Devices */
static capture_device_t devices[CAPTURE_MAX_DEVICES];
static int              device_count;
static char             active_device_id[sizeof(devices[0].id)];
static const char      *active_device_name = "";
static bool             active_core_convert; /* Conversion mode the device was opened with */
static double           retry_timer;         /* Seconds until the next reopen attempt */

/* Why the No Signal screen is shown, if it is. */
enum signal_state
{
   SIGNAL_OK = 0,
   SIGNAL_NO_DEVICE,   /* No capture device found */
   SIGNAL_DEVICE_ERROR,/* The device could not be opened, or stopped with an error */
   SIGNAL_NO_FRAMES,   /* The device is open but sends no frames */
   SIGNAL_BLACK        /* The device sends uniformly black frames: no input signal */
};

/* Capture */
static capture_frame_t last_frame;     /* Last frame received; valid while have_frame */
static bool            have_frame;
static double          seconds_without_frame;
static double          seconds_black;

/* Output */
static uint32_t       *rgb_buffer;      /* Converted or scaled image */
static size_t          rgb_buffer_size; /* In pixels */
static uint32_t        screen_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static char            screen_key[1024]; /* Content of the No Signal screen last drawn */
static const uint32_t *last_output;
static size_t          last_output_pitch;
static unsigned        output_width  = SCREEN_WIDTH;
static unsigned        output_height = SCREEN_HEIGHT;
static float           output_aspect = 4.0f / 3.0f;
static double          output_fps    = DEFAULT_FPS;

/* Audio */
static int16_t silence[AUDIO_FRAMES_MAX * 2];
static double  audio_frames_pending; /* Fractional audio frames carried between video frames */

/* Stats */
static retro_time_t stats_last_time;
static uint64_t     stats_last_received;
static uint64_t     stats_frames_shown;  /* New frames shown since stats_last_time */
static uint64_t     stats_runs;          /* retro_run() calls since stats_last_time */
static uint64_t     stats_dropped;       /* Frames skipped (never shown) since stats_last_time */
static uint64_t     stats_repeated;      /* Calls that repeated the previous frame */
static double       stats_capture_fps;   /* Frames received from the device per second */
static double       stats_frontend_fps;  /* retro_run() calls per second */
static double       stats_output_fps;    /* New frames shown per second */

/* Session totals since the device was opened, logged when it is closed. */
static retro_time_t session_start_time;
static uint64_t     session_frames_shown;
static uint64_t     session_runs;
static uint64_t     session_dropped;
static uint64_t     session_pacing_skips;
static uint64_t     session_repeated;
static retro_time_t session_paused_usec;   /* Time RetroArch did not run the core (menu) */
static uint64_t     session_paused_frames; /* Frames received during those pauses */

/* Pause detection: RetroArch stops calling retro_run() while its menu is
 * open, and briefly while it rebuilds a shader (for example when the output
 * size changes). Frames missed meanwhile are not counted as drops. A gap of
 * STALL_THRESHOLD_USEC or more is excluded from the stats; from
 * PAUSE_THRESHOLD_USEC, it is logged as a pause rather than a stall. */
#define STALL_THRESHOLD_USEC 100000
#define PAUSE_THRESHOLD_USEC 500000
static retro_time_t last_run_usec;
static uint64_t     last_run_received;
static bool         pacing_resync;    /* Show the newest frame next, without counting drops */
static bool         waiting_first_run; /* Device opened, retro_run() not called since */

/* Frame rate measurement (see measure_frame_rate()). */
#define RATE_MIN_USEC   10000000 /* Measure over at least 10 seconds */
#define RATE_GAP_USEC   500000   /* A longer gap between frames restarts the measurement */
#define RATE_TOLERANCE  0.0004   /* Relative difference accepted between measured and reported rates */
static uint64_t rate_anchor_sequence; /* First frame of the measurement, 0 if none */
static int64_t  rate_anchor_usec;
static uint64_t rate_last_sequence;
static int64_t  rate_last_usec;
static double   rate_nominal;         /* Rate declared by the device */
static double   rate_reported;        /* Rate reported to RetroArch */

/* Age of each frame when shown (time between its arrival from the device and
 * retro_run()), over the current stats second. */
static double   frame_age_min_ms;
static double   frame_age_max_ms;
static double   frame_age_sum_ms;
static unsigned frame_age_count;
static unsigned stats_seconds;      /* Seconds since stats started, for the log */
static uint64_t stats_pacing_skips; /* Frames skipped on purpose by smooth pacing */

/* Frame pacing state */
static uint64_t shown_sequence;     /* Sequence of the frame shown last, 0 if none */
static unsigned pacing_safe_runs;   /* Consecutive runs behind, outside the collision zone */

#define STATS_LINES 3
static char         stats_lines[STATS_LINES][96];
static uint32_t     stats_colors[STATS_LINES];

/* Stats colors: as expected, slightly off, or frames are being lost. */
#define STATS_COLOR_GOOD    0x40FF40
#define STATS_COLOR_NEUTRAL 0xFFFFFF
#define STATS_COLOR_BAD     0xFF4040

/* Core option storage. Device names are used as option values. */
static char option_device_names[CAPTURE_MAX_DEVICES][sizeof(devices[0].name) + 16];
static struct retro_core_option_v2_definition option_definitions[11];
static struct retro_core_option_v2_category   option_categories[4];
static char legacy_device_values[CAPTURE_MAX_DEVICES * (sizeof(devices[0].name) + 17) + 64];
static struct retro_variable legacy_variables[11];

/* ------------------------------------------------------------------------ */
/* Logging                                                                   */
/* ------------------------------------------------------------------------ */

/* Fallback logger used when the frontend does not provide one. */
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

/* Capture stats log: one line per second in video_capture_stats.log, in
 * RetroArch's save directory, while Show Capture Stats and Log Capture Stats
 * are on. A separate file is used because RetroArch 1.16 does not keep core
 * messages in its own log once content is running. */
static FILE *stats_log_file;

static void log_options(bool all);

static void stats_log_printf(const char *fmt, ...)
{
   va_list va;

   if (!stats_log_file)
      return;

   va_start(va, fmt);
   vfprintf(stats_log_file, fmt, va);
   va_end(va);
   fputc('\n', stats_log_file);
   fflush(stats_log_file);
}

/* Opens or closes the stats log to match the options. */
static void update_stats_log_file(void)
{
   const bool wanted = opt_stats_log && opt_stats != STATS_OFF;
   const char *dir   = NULL;
   char path[1024];

   if (wanted == (stats_log_file != NULL))
      return;

   if (!wanted)
   {
      fclose(stats_log_file);
      stats_log_file = NULL;
      return;
   }

   if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || !dir)
      return;

   snprintf(path, sizeof(path), "%s%cvideo_capture_stats.log", dir,
#ifdef _WIN32
         '\\'
#else
         '/'
#endif
         );
   stats_log_file = fopen(path, "w");
   stats_seconds  = 0;
   log_cb(stats_log_file ? RETRO_LOG_INFO : RETRO_LOG_WARN, LOG_PREFIX "Capture stats log: %s (%s).\n",
         path, stats_log_file ? "open" : "could not open");
   stats_log_printf("Video Capture Device %s. Options:", CORE_VERSION);
   log_options(true);
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

   log_cb(RETRO_LOG_INFO, LOG_PREFIX "%d capture device(s) found.\n", device_count);
   for (i = 0; i < device_count; i++)
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "  %s\n", option_device_names[i]);
}

/* Fills one option definition. `values` alternates value and label (NULL
 * label: the value is shown) and ends with NULL. */
static void define_option(struct retro_core_option_v2_definition *def, const char *category,
      const char *key, const char *desc, const char *info, const char *default_value,
      const char *const *values)
{
   int i;

   def->category_key  = category;
   def->key           = key;
   def->desc          = desc;
   def->info          = info;
   def->default_value = default_value;
   for (i = 0; values[i * 2]; i++)
   {
      def->values[i].value = values[i * 2];
      def->values[i].label = values[i * 2 + 1];
   }
}

static const char *const resolution_values[] = {
   "native", "Native", "640x480", NULL, "320x240", NULL, NULL
};
static const char *const aspect_values[] = {
   "source", "Source", "4_3_squash", "4:3 (Squash)", "4_3_crop", "4:3 (Crop Pillarbox)",
   "16_9", "16:9", NULL
};
static const char *const conversion_values[] = {
   "core", "Core", "media_foundation", "Media Foundation", NULL
};
static const char *const color_space_values[] = {
   "default", "Default", "rec709", "Rec. 709", "rec601", "Rec. 601", NULL
};
static const char *const color_range_values[] = {
   "limited", "Limited", "full", "Full", NULL
};
static const char *const stats_values[] = {
   "off", "Off", "notification", "On-Screen Notification", "image", "In Image", NULL
};
static const char *const details_values[] = {
   "off", "Off", "on", "On", NULL
};
static const char *const pacing_values[] = {
   "latest", "Lowest Latency", "smooth", "Smooth", NULL
};
static const char *const stats_log_values[] = {
   "off", "Off", "on", "On", NULL
};

static bool update_option_visibility(void);

static void declare_core_options(void)
{
   unsigned version = 0;
   int i;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
      struct retro_core_options_v2 options;
      struct retro_core_option_v2_definition *def = option_definitions;
      const int max_devices = RETRO_NUM_CORE_OPTION_VALUES_MAX - 2;

      memset(option_definitions, 0, sizeof(option_definitions));
      memset(option_categories, 0, sizeof(option_categories));

      /* Capture Device stays on the main page; the other options are grouped
       * in submenus. */
      option_categories[0].key  = "video";
      option_categories[0].desc = "Video";
      option_categories[0].info = "Output resolution, aspect ratio and frame pacing.";
      option_categories[1].key  = "color";
      option_categories[1].desc = "Color";
      option_categories[1].info = "Conversion of the device's YUV picture to RGB.";
      option_categories[2].key  = "diagnostics";
      option_categories[2].desc = "Diagnostics";
      option_categories[2].info = "Capture stats and No Signal screen details.";

      def->key  = OPTION_DEVICE;
      def->desc = "Capture Device";
      def->info = "Video capture device to display. Auto selects the first device found. "
                  "Devices are detected when the core starts.";
      def->values[0].value = DEVICE_AUTO;
      def->values[0].label = "Auto (first device)";
      for (i = 0; i < device_count && i < max_devices; i++)
         def->values[i + 1].value = option_device_names[i];
      def->default_value = DEVICE_AUTO;
      def++;

      define_option(def++, "video", OPTION_RESOLUTION, "Output Resolution",
            "Resolution of the image passed to RetroArch and its shaders. "
            "CRT and VHS shaders expect a low resolution such as 640x480 or 320x240. "
            "Downscaling averages the source pixels to avoid aliasing.",
            "native", resolution_values);

      define_option(def++, "video", OPTION_ASPECT, "Aspect Ratio",
            "Display aspect ratio reported to RetroArch. "
            "4:3 (Squash) keeps the whole frame, for a 4:3 picture stretched to 16:9. "
            "4:3 (Crop Pillarbox) keeps only the central 4:3 area, for a picture with black bars on the sides. "
            "Requires Settings > Video > Scaling > Aspect Ratio set to Core Provided; a shader preset may override it.",
            "source", aspect_values);

      define_option(def++, "video", OPTION_PACING, "Frame Pacing",
            "Lowest Latency always shows the newest frame. The core measures the device's real frame rate "
            "and reports it to RetroArch, so with a variable refresh rate display or Sync to Exact Content "
            "Framerate, frames arrive at a steady moment and none is lost. "
            "Smooth shows every frame in order, at the cost of up to one frame of latency (about 16 ms), "
            "and skips a single frame now and then to catch up. Use it if Show Capture Stats reports "
            "frames dropped and repeated in bursts, for example with a device whose timing is irregular.",
            "latest", pacing_values);

      define_option(def++, "color", OPTION_CONVERSION, "Color Conversion",
            "Converts the device's YUV picture to the RGB format RetroArch needs. "
            "Core converts after downscaling, which is faster at 640x480 and 320x240, "
            "and applies the Color Space and Color Range options. "
            "Media Foundation converts the full frame with Windows; Color Space and Color Range are then ignored. "
            "Changing this option reopens the device.",
            "core", conversion_values);

      define_option(def++, "color", OPTION_COLOR_SPACE, "Color Space",
            "Color matrix used to convert YUV to RGB. Default uses Rec. 601 up to 576 lines and Rec. 709 above. "
            "Applies only when Color Conversion is set to Core.",
            "default", color_space_values);

      define_option(def++, "color", OPTION_COLOR_RANGE, "Color Range",
            "Limited is standard for video: black is at level 16 and white at 235. "
            "Use Full if blacks look gray or colors look washed out. "
            "Applies only when Color Conversion is set to Core.",
            "limited", color_range_values);

      define_option(def++, "diagnostics", OPTION_STATS, "Show Capture Stats",
            "Shows the frame rate received from the device and the frame rate shown, with the image sizes. "
            "If the output frame rate is lower than the capture frame rate, frames are being dropped. "
            "On-Screen Notification uses RetroArch's notifications. "
            "In Image draws the text into the picture, before shaders.",
            "off", stats_values);

      define_option(def++, "diagnostics", OPTION_STATS_LOG, "Log Capture Stats",
            "Writes the capture stats every second, with the age of each frame when shown, to "
            "video_capture_stats.log in RetroArch's save directory. Useful to troubleshoot frame pacing. "
            "Shown only when Show Capture Stats is on.",
            "off", stats_log_values);

      define_option(def++, "diagnostics", OPTION_DETAILS, "No Signal Details",
            "Shows the reason and troubleshooting hints on the No Signal screen.",
            "off", details_values);

      options.categories  = option_categories;
      options.definitions = option_definitions;
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);

      {
         struct retro_core_options_update_display_callback callback;
         callback.callback = update_option_visibility;
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &callback);
      }
   }
   else
   {
      /* Legacy format: "Description; value1|value2|...", first value is the default. */
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
      legacy_variables[3].key   = OPTION_CONVERSION;
      legacy_variables[3].value = "Color Conversion; core|media_foundation";
      legacy_variables[4].key   = OPTION_COLOR_SPACE;
      legacy_variables[4].value = "Color Space; default|rec709|rec601";
      legacy_variables[5].key   = OPTION_COLOR_RANGE;
      legacy_variables[5].value = "Color Range; limited|full";
      legacy_variables[6].key   = OPTION_STATS;
      legacy_variables[6].value = "Show Capture Stats; off|notification|image";
      legacy_variables[7].key   = OPTION_DETAILS;
      legacy_variables[7].value = "No Signal Details; off|on";
      legacy_variables[8].key   = OPTION_PACING;
      legacy_variables[8].value = "Frame Pacing; latest|smooth";
      legacy_variables[9].key   = OPTION_STATS_LOG;
      legacy_variables[9].value = "Log Capture Stats; off|on";
      legacy_variables[10].key   = NULL;
      legacy_variables[10].value = NULL;
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, legacy_variables);
   }
}

/* Returns the value of an option, or NULL if the frontend has none. */
static const char *get_option(const char *key)
{
   struct retro_variable var;

   var.key   = key;
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      return var.value;
   return NULL;
}

static bool option_is(const char *value, const char *expected)
{
   return value && strcmp(value, expected) == 0;
}

/* Shows Log Capture Stats only when Show Capture Stats is on. Called by the
 * frontend when the options menu is displayed or an option changes; returns
 * true if the visibility changed. */
static bool update_option_visibility(void)
{
   static int visible = -1; /* Unknown until the first call */
   const char *stats = get_option(OPTION_STATS);
   const bool show   = stats && !option_is(stats, "off");
   struct retro_core_option_display display;

   if (visible == (int)show)
      return false;

   display.key     = OPTION_STATS_LOG;
   display.visible = show;
   environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
   visible = show;
   return true;
}

/* Last value of each option, to log changes in the stats log. */
static char option_last_values[sizeof(option_definitions) / sizeof(option_definitions[0])][288];

/* Returns the label shown for an option value. */
static const char *option_label(const struct retro_core_option_v2_definition *def, const char *value)
{
   int i;

   for (i = 0; def->values[i].value; i++)
      if (strcmp(def->values[i].value, value) == 0)
         return def->values[i].label ? def->values[i].label : value;
   return value;
}

/* Writes the options to the stats log: all of them with `all`, otherwise only
 * those changed since the previous call. */
static void log_options(bool all)
{
   int i;

   for (i = 0; option_definitions[i].key; i++)
   {
      const struct retro_core_option_v2_definition *def = &option_definitions[i];
      const char *value = get_option(def->key);

      if (!value)
         continue;
      if (all || strcmp(value, option_last_values[i]) != 0)
         stats_log_printf("%s%s: %s.", all ? "  " : "Option changed: ", def->desc, option_label(def, value));
      snprintf(option_last_values[i], sizeof(option_last_values[i]), "%s", value);
   }
}

/* Reads every option except the device, which apply_device_selection() handles. */
static void read_options(void)
{
   const char *value;

   value = get_option(OPTION_RESOLUTION);
   opt_resolution = option_is(value, "640x480") ? RESOLUTION_640X480
                  : option_is(value, "320x240") ? RESOLUTION_320X240
                  : RESOLUTION_NATIVE;

   value = get_option(OPTION_ASPECT);
   opt_aspect = option_is(value, "4_3_squash") ? ASPECT_4_3_SQUASH
              : option_is(value, "4_3_crop")   ? ASPECT_4_3_CROP
              : option_is(value, "16_9")       ? ASPECT_16_9
              : ASPECT_SOURCE;

   opt_core_convert = !option_is(get_option(OPTION_CONVERSION), "media_foundation");

   value = get_option(OPTION_COLOR_SPACE);
   opt_color_space = option_is(value, "rec709") ? COLOR_SPACE_REC709
                   : option_is(value, "rec601") ? COLOR_SPACE_REC601
                   : COLOR_SPACE_DEFAULT;

   opt_color_range = option_is(get_option(OPTION_COLOR_RANGE), "full") ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED;

   value = get_option(OPTION_STATS);
   opt_stats = option_is(value, "notification") ? STATS_NOTIFICATION
             : option_is(value, "image")        ? STATS_IMAGE
             : STATS_OFF;

   opt_details       = option_is(get_option(OPTION_DETAILS), "on");
   opt_smooth_pacing = option_is(get_option(OPTION_PACING), "smooth");
   opt_stats_log     = option_is(get_option(OPTION_STATS_LOG), "on");
}

/* Returns the index of the device selected in the core options, or -1. */
static int selected_device_index(void)
{
   const char *value = get_option(OPTION_DEVICE);
   int i;

   if (device_count == 0)
      return -1;

   if (value && strcmp(value, DEVICE_AUTO) != 0)
   {
      for (i = 0; i < device_count; i++)
         if (strcmp(value, option_device_names[i]) == 0)
            return i;

      log_cb(RETRO_LOG_WARN, LOG_PREFIX "Device \"%s\" not found, using the first device.\n", value);
   }

   return 0;
}

/* Logs a summary of the capture session: average rates and frames lost.
 * Called when the device is closed, so the log keeps a trace of dropped
 * frames without watching the on-screen stats. */
static void log_session_stats(void)
{
   const retro_time_t now = perf_cb.get_time_usec ? perf_cb.get_time_usec() : 0;
   const uint64_t received = capture_frames_received() - session_paused_frames;
   double seconds, paused;

   if (!now || !session_start_time || !received)
      return;

   paused  = (double)session_paused_usec / 1000000.0;
   seconds = (double)(now - session_start_time) / 1000000.0 - paused;
   if (seconds < 1.0)
      return;

   log_cb(RETRO_LOG_INFO, LOG_PREFIX "Session stats over %.0f s (plus %.0f s paused): capture %.2f fps, "
         "RetroArch %.2f fps, output %.2f fps; %llu frames dropped (%llu by pacing), %llu repeated.\n",
         seconds, paused, received / seconds, session_runs / seconds, session_frames_shown / seconds,
         (unsigned long long)session_dropped, (unsigned long long)session_pacing_skips,
         (unsigned long long)session_repeated);
   stats_log_printf("Session over %.0f s (plus %.0f s paused): capture %.2f fps, RetroArch %.2f fps, "
         "output %.2f fps; %llu frames dropped (%llu by pacing), %llu repeated.",
         seconds, paused, received / seconds, session_runs / seconds, session_frames_shown / seconds,
         (unsigned long long)session_dropped, (unsigned long long)session_pacing_skips,
         (unsigned long long)session_repeated);
}

static void reset_session_stats(void)
{
   session_start_time   = perf_cb.get_time_usec ? perf_cb.get_time_usec() : 0;
   session_frames_shown  = 0;
   session_runs          = 0;
   session_dropped       = 0;
   session_pacing_skips  = 0;
   session_repeated      = 0;
   session_paused_usec   = 0;
   session_paused_frames = 0;
}

/* Opens the selected device. Without `force`, does nothing if the device and
 * conversion mode are unchanged. */
static void apply_device_selection(bool force)
{
   const int index = selected_device_index();

   if (index < 0)
   {
      active_device_id[0] = '\0';
      active_device_name  = "";
      return;
   }

   if (!force && strcmp(devices[index].id, active_device_id) == 0 &&
       active_core_convert == opt_core_convert)
      return;

   snprintf(active_device_id, sizeof(active_device_id), "%s", devices[index].id);
   active_device_name    = option_device_names[index];
   active_core_convert   = opt_core_convert;
   have_frame            = false;
   seconds_without_frame = 0.0;
   seconds_black         = 0.0;
   retry_timer           = RETRY_INTERVAL;

   log_session_stats();
   reset_session_stats();
   shown_sequence       = 0;
   pacing_safe_runs     = 0;
   pacing_resync        = false;
   rate_anchor_sequence = 0;
   rate_nominal         = 0.0;

   if (capture_start(devices[index].id, opt_core_convert))
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "Capture started: %s.\n", active_device_name);
   else
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "Could not start capture: %s\n", capture_last_error());

   /* The time until the next retro_run() is treated like a pause: at startup,
    * RetroArch sets up its video driver and shaders after loading the core. */
   last_run_usec     = perf_cb.get_time_usec ? perf_cb.get_time_usec() : 0;
   last_run_received = capture_frames_received();
   waiting_first_run = true;

   stats_last_time     = 0; /* Restart the stats measurement */
   stats_lines[0][0]   = '\0';
}

/* ------------------------------------------------------------------------ */
/* Frontend geometry and timing                                              */
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

   log_cb(RETRO_LOG_INFO, LOG_PREFIX "Output: %ux%u, aspect ratio %.4f.\n", width, height, aspect);
   stats_log_printf("Output: %ux%u, aspect ratio %.4f.", width, height, aspect);
}

/* Returns `fps` if plausible, otherwise the current output rate. */
static double valid_fps(double fps)
{
   return (fps >= 10.0 && fps <= 240.0) ? fps : output_fps;
}

/* Reports the device frame rate to the frontend when it changes, so RetroArch
 * paces retro_run() on the capture device instead of a fixed 60 Hz. */
static void update_frame_rate(double device_rate)
{
   struct retro_system_av_info av_info;
   const double fps = valid_fps(device_rate);

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

   log_cb(RETRO_LOG_INFO, LOG_PREFIX "Frame rate: %.3f fps.\n", output_fps);
}

/*
 * Works out the frame rate to report to RetroArch from the frames received.
 *
 * Capture devices declare a nominal rate, often 60 fps, even when the source
 * runs at 59.94 fps (NTSC video). RetroArch then calls retro_run() at 60 Hz
 * while frames arrive at 59.94 Hz, and a frame is lost every 17 seconds or so.
 * The rate is measured from the frame sequence numbers and arrival times over
 * at least RATE_MIN_USEC; when it matches the nominal rate or its NTSC
 * variant (x1000/1001 or x1001/1000) better than the rate reported so far, that
 * rate is reported instead. With a variable refresh rate display, RetroArch
 * then follows the source exactly.
 *
 * Once the rates match, the moment frames arrive relative to retro_run()
 * stays where it was when the rate changed. The change is therefore made only
 * when frames arrive halfway between two calls, far from the collision zone
 * described in pick_frame().
 */
static double measure_frame_rate(const capture_frame_t *frame)
{
   double candidates[3], measured;
   int i;

   if (!frame->arrival_usec)
      return frame->frame_rate;

   /* New device format: start over from the declared rate. */
   if (frame->frame_rate != rate_nominal)
   {
      rate_nominal         = frame->frame_rate;
      rate_reported        = frame->frame_rate;
      rate_anchor_sequence = 0;
   }

   /* First frame, or frames stopped for a while (signal lost): restart the
    * measurement, keeping the rate reported so far. */
   if (!rate_anchor_sequence || frame->sequence <= rate_last_sequence ||
       frame->arrival_usec - rate_last_usec > RATE_GAP_USEC)
   {
      rate_anchor_sequence = frame->sequence;
      rate_anchor_usec     = frame->arrival_usec;
   }

   rate_last_sequence = frame->sequence;
   rate_last_usec     = frame->arrival_usec;

   if (frame->arrival_usec - rate_anchor_usec < RATE_MIN_USEC)
      return rate_reported;

   measured = (double)(frame->sequence - rate_anchor_sequence) * 1000000.0 /
              (double)(frame->arrival_usec - rate_anchor_usec);

   candidates[0] = rate_nominal;
   candidates[1] = rate_nominal * 1000.0 / 1001.0;
   candidates[2] = rate_nominal * 1001.0 / 1000.0;

   /* Wait for frames to arrive halfway between two calls. */
   {
      const double age_ms    = (double)(capture_time_usec() - frame->arrival_usec) / 1000.0;
      const double period_ms = 1000.0 / output_fps;

      if (age_ms < period_ms * 0.3 || age_ms > period_ms * 0.7)
         return rate_reported;
   }

   for (i = 0; i < 3; i++)
   {
      if (candidates[i] != rate_reported && fabs(measured / candidates[i] - 1.0) <= RATE_TOLERANCE)
      {
         rate_reported = candidates[i];
         log_cb(RETRO_LOG_INFO, LOG_PREFIX "Measured frame rate: %.3f fps (device declares %.3f).\n",
               measured, rate_nominal);
         stats_log_printf("Measured frame rate: %.3f fps (device declares %.3f), reporting %.3f fps to RetroArch.",
               measured, rate_nominal, rate_reported);
         break;
      }
   }

   return rate_reported;
}

/* Makes sure rgb_buffer holds at least `pixels` pixels. */
static bool reserve_rgb_buffer(size_t pixels)
{
   uint32_t *buffer;

   if (pixels <= rgb_buffer_size)
      return true;

   buffer = (uint32_t *)realloc(rgb_buffer, pixels * sizeof(uint32_t));
   if (!buffer)
   {
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "Out of memory.\n");
      return false;
   }

   rgb_buffer      = buffer;
   rgb_buffer_size = pixels;
   return true;
}

/* ------------------------------------------------------------------------ */
/* Stats                                                                     */
/* ------------------------------------------------------------------------ */

static const char *format_name(enum capture_format format)
{
   switch (format)
   {
      case CAPTURE_FORMAT_NV12: return "NV12";
      case CAPTURE_FORMAT_YUY2: return "YUY2";
      default:                  return "RGB";
   }
}

/* Color for a measured rate compared with the rate it should reach. Rates are
 * measured over about one second, so one frame more or less (1 fps) is normal
 * timing noise; two is borderline; more means frames are being lost. */
static uint32_t rate_color(double measured, double expected)
{
   const double difference = expected - measured;

   if (expected <= 0.0)
      return STATS_COLOR_NEUTRAL;
   if (difference <= 1.0)
      return STATS_COLOR_GOOD;
   if (difference <= 2.0)
      return STATS_COLOR_NEUTRAL;
   return STATS_COLOR_BAD;
}

/* Measures three rates about once per second and updates the stats text:
 *   Capture:   frames received from the device, expected at its nominal rate;
 *   RetroArch: retro_run() calls, expected at the rate the core reported;
 *   Output:    new frames shown, expected to match the capture rate.
 * If RetroArch is low, the frontend (or a heavy shader) cannot keep up. If
 * RetroArch is fine but Output is low, frames are lost to uneven timing
 * between the device and the frontend. */
static void update_stats(void)
{
   const retro_time_t now = perf_cb.get_time_usec ? perf_cb.get_time_usec() : 0;
   const uint64_t received = capture_frames_received();
   double seconds;

   stats_runs++;

   if (!now)
      return;

   if (!stats_last_time)
   {
      stats_last_time     = now;
      stats_last_received = received;
      stats_frames_shown  = 0;
      stats_runs          = 0;
      stats_dropped       = 0;
      stats_repeated      = 0;
      stats_pacing_skips  = 0;
      frame_age_count     = 0;
      frame_age_sum_ms    = 0.0;
      return;
   }

   seconds = (double)(now - stats_last_time) / 1000000.0;
   if (seconds < 1.0)
      return;

   /* Stats log: one line per second. */
   {
      const uint64_t received_delta = received - stats_last_received;

      stats_seconds++;
      stats_log_printf("t=%us capture %.2f RetroArch %.2f output %.2f fps | dropped %llu "
            "(pacing skips %llu), repeated %llu | frame age min %.1f avg %.1f max %.1f ms",
            stats_seconds, received_delta / seconds, stats_runs / seconds, stats_frames_shown / seconds,
            (unsigned long long)stats_dropped,
            (unsigned long long)stats_pacing_skips,
            (unsigned long long)stats_repeated,
            frame_age_count ? frame_age_min_ms : 0.0,
            frame_age_count ? frame_age_sum_ms / frame_age_count : 0.0,
            frame_age_count ? frame_age_max_ms : 0.0);
      frame_age_sum_ms   = 0.0;
      frame_age_count    = 0;
      stats_pacing_skips = 0;
      stats_dropped      = 0;
      stats_repeated     = 0;
   }

   stats_capture_fps   = (double)(received - stats_last_received) / seconds;
   stats_frontend_fps  = (double)stats_runs / seconds;
   stats_output_fps    = (double)stats_frames_shown / seconds;
   stats_last_time     = now;
   stats_last_received = received;
   stats_frames_shown  = 0;
   stats_runs          = 0;

   snprintf(stats_lines[0], sizeof(stats_lines[0]), "Capture:   %6.2f fps  %ux%u %s",
         stats_capture_fps, last_frame.width, last_frame.height, format_name(last_frame.format));
   snprintf(stats_lines[1], sizeof(stats_lines[1]), "RetroArch: %6.2f fps",
         stats_frontend_fps);
   snprintf(stats_lines[2], sizeof(stats_lines[2]), "Output:    %6.2f fps  %ux%u",
         stats_output_fps, output_width, output_height);

   stats_colors[0] = rate_color(stats_capture_fps, last_frame.frame_rate);
   stats_colors[1] = rate_color(stats_frontend_fps, output_fps);
   stats_colors[2] = rate_color(stats_output_fps, stats_capture_fps);

   if (opt_stats == STATS_NOTIFICATION)
   {
      char text[240];

      snprintf(text, sizeof(text), "Capture %.2f fps (%ux%u %s) | RetroArch %.2f fps | Output %.2f fps (%ux%u)",
            stats_capture_fps, last_frame.width, last_frame.height, format_name(last_frame.format),
            stats_frontend_fps, stats_output_fps, output_width, output_height);

      if (message_interface_version >= 1)
      {
         struct retro_message_ext message;

         message.msg      = text;
         message.duration = 2000; /* Milliseconds; refreshed every second */
         message.priority = 1;
         message.level    = RETRO_LOG_INFO;
         message.target   = RETRO_MESSAGE_TARGET_OSD;
         message.type     = RETRO_MESSAGE_TYPE_STATUS;
         message.progress = -1;
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
      }
      else
      {
         struct retro_message message;

         message.msg    = text;
         message.frames = (unsigned)(output_fps * 2.0);
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
      }
   }
}

/* Draws the stats in the top-left corner of an output image, on a dark box
 * so the text stays readable over any picture. Each line is colored by
 * rate_color(). */
static void draw_stats_overlay(uint32_t *image, size_t pitch, unsigned width, unsigned height)
{
   /* Text height about 1/30 of the image: 8 px at 240 lines, 32 px at 1080. */
   const unsigned scale = height >= 240 ? height / 240 : 1;
   const unsigned line_height = (FONT_GLYPH_SIZE + 2) * scale;
   const unsigned margin = 2 * scale;
   unsigned box_width = 0;
   int i;

   if (!stats_lines[0][0])
      return;

   for (i = 0; i < STATS_LINES; i++)
   {
      const unsigned w = font_text_width(stats_lines[i], scale);
      if (w > box_width)
         box_width = w;
   }

   font_fill_rect(image, pitch, width, height, 0, 0,
         (int)(box_width + 2 * margin), (int)(STATS_LINES * line_height + 2 * margin), 0x101010);
   for (i = 0; i < STATS_LINES; i++)
      font_draw_text(image, pitch, width, height, (int)margin, (int)(margin + i * line_height),
            stats_lines[i], scale, stats_colors[i]);
}

/* ------------------------------------------------------------------------ */
/* No Signal screen                                                          */
/* ------------------------------------------------------------------------ */

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
   const int x = ((int)SCREEN_WIDTH - (int)font_text_width(text, scale)) / 2;
   font_draw_text(screen_buffer, SCREEN_WIDTH * sizeof(uint32_t), SCREEN_WIDTH, SCREEN_HEIGHT,
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
static void render_no_signal_screen(enum signal_state state)
{
   const char *title = state == SIGNAL_NO_DEVICE ? "NO DEVICE"
                     : state == SIGNAL_DEVICE_ERROR ? "DEVICE ERROR"
                     : "NO SIGNAL";
   char reason[512], hint[512], key[sizeof(screen_key)];
   char lines[12][96];
   const int bars_height = (SCREEN_HEIGHT * 2) / 3;
   const int panel_x = 40, panel_w = SCREEN_WIDTH - 80;
   int line_count = 0, panel_h, panel_y, y, x, i;

   describe_signal_state(state, reason, sizeof(reason), hint, sizeof(hint));
   snprintf(key, sizeof(key), "%d|%d|%s|%s", (int)state, (int)opt_details, active_device_name, reason);
   if (strcmp(key, screen_key) == 0)
      return;
   snprintf(screen_key, sizeof(screen_key), "%s", key);

   /* Background: color bars over a luminance ramp. */
   for (y = 0; y < SCREEN_HEIGHT; y++)
   {
      uint32_t *row = &screen_buffer[y * SCREEN_WIDTH];
      for (x = 0; x < SCREEN_WIDTH; x++)
      {
         uint32_t v = (uint32_t)((x * 255) / (SCREEN_WIDTH - 1));
         row[x] = y < bars_height ? bar_colors[(x * 7) / SCREEN_WIDTH] : (v << 16) | (v << 8) | v;
      }
   }

   /* Detail lines, wrapped to the panel width (8 px characters). */
   if (opt_details)
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
   panel_h = 24 + 32 + 16 + (active_device_name[0] ? 24 : 0) + 8 + line_count * 12 + 16;
   panel_y = (SCREEN_HEIGHT - panel_h) / 2;
   font_fill_rect(screen_buffer, SCREEN_WIDTH * sizeof(uint32_t), SCREEN_WIDTH, SCREEN_HEIGHT,
         panel_x, panel_y, panel_w, panel_h, 0x000000);

   y = panel_y + 24;
   draw_centered(title, y, 4, 0xFFFFFF);
   y += 32 + 16;

   if (active_device_name[0])
   {
      char name[40];
      snprintf(name, sizeof(name), "%.36s%s", active_device_name, strlen(active_device_name) > 36 ? "..." : "");
      draw_centered(name, y, 2, 0xC0C0C0);
      y += 24;
   }

   y += 8;
   for (i = 0; i < line_count; i++, y += 12)
      font_draw_text(screen_buffer, SCREEN_WIDTH * sizeof(uint32_t), SCREEN_WIDTH, SCREEN_HEIGHT,
            panel_x + 16, y, lines[i], 1, 0xA0A0A0);
}

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

/* Picks the color matrix for the current frame. */
static enum color_matrix frame_matrix(const capture_frame_t *frame)
{
   switch (opt_color_space)
   {
      case COLOR_SPACE_REC709: return COLOR_MATRIX_REC709;
      case COLOR_SPACE_REC601: return COLOR_MATRIX_REC601;
      default: return frame->height <= 576 ? COLOR_MATRIX_REC601 : COLOR_MATRIX_REC709;
   }
}

/* Applies crop, resolution, aspect ratio and color conversion to the last
 * captured frame, then sends it to the frontend. */
static void output_capture_frame(void)
{
   capture_frame_t *frame = &last_frame;
   unsigned crop_x     = 0;
   unsigned crop_width = frame->width;
   unsigned dst_width, dst_height;
   uint32_t *out;
   size_t pitch;
   float aspect;

   /* Crop the central 4:3 area when the source is wider than 4:3. */
   if (opt_aspect == ASPECT_4_3_CROP && frame->width * 3 > frame->height * 4)
   {
      crop_width = (frame->height * 4 + 1) / 3;
      crop_x     = ((frame->width - crop_width) / 2) & ~1u; /* Even: YUV pixels come in pairs */
   }

   switch (opt_aspect)
   {
      case ASPECT_4_3_SQUASH:
      case ASPECT_4_3_CROP: aspect = 4.0f / 3.0f; break;
      case ASPECT_16_9:     aspect = 16.0f / 9.0f; break;
      default:              aspect = (float)crop_width / (float)frame->height; break;
   }

   if (opt_resolution == RESOLUTION_NATIVE)
   {
      dst_width  = crop_width;
      dst_height = frame->height;
   }
   else
   {
      dst_width  = opt_resolution == RESOLUTION_320X240 ? 320 : 640;
      dst_height = opt_resolution == RESOLUTION_320X240 ? 240 : 480;
   }

   if (frame->format == CAPTURE_FORMAT_XRGB8888 && opt_resolution == RESOLUTION_NATIVE)
   {
      /* No copy: point into the captured frame. The pitch skips the cropped
       * columns, so the frontend reads only the central area. */
      out   = (uint32_t *)frame->data + crop_x;
      pitch = frame->stride;
   }
   else
   {
      if (!reserve_rgb_buffer((size_t)dst_width * dst_height))
         return;
      out   = rgb_buffer;
      pitch = dst_width * sizeof(uint32_t);

      if (frame->format == CAPTURE_FORMAT_XRGB8888)
      {
         if (!scale_area((const uint32_t *)frame->data, (unsigned)(frame->stride / 4), crop_x, 0,
               crop_width, frame->height, out, dst_width, dst_height))
            return; /* Cannot happen with CAPTURE_MAX_* sizes; keep the previous frame */
      }
      else
      {
         convert_set_colorimetry(frame_matrix(frame), opt_color_range);
         if (!convert_yuv_frame(frame, crop_x, crop_width, out, dst_width, dst_height))
            return;
      }
   }

   set_output_geometry(dst_width, dst_height, aspect);

   if (opt_stats == STATS_IMAGE)
      draw_stats_overlay(out, pitch, dst_width, dst_height);

   last_output       = out;
   last_output_pitch = pitch;
   video_cb(out, dst_width, dst_height, pitch);
}

static void output_no_signal_screen(enum signal_state state)
{
   render_no_signal_screen(state);
   set_output_geometry(SCREEN_WIDTH, SCREEN_HEIGHT, 4.0f / 3.0f);
   last_output       = screen_buffer;
   last_output_pitch = SCREEN_WIDTH * sizeof(uint32_t);
   video_cb(screen_buffer, SCREEN_WIDTH, SCREEN_HEIGHT, last_output_pitch);
}

/*
 * Chooses which captured frame to show in this retro_run() call.
 *
 * The device and RetroArch run on separate clocks, so the moment a frame
 * arrives slowly drifts relative to the moment retro_run() is called. When
 * the two get close (the "collision zone"), timing noise makes two frames
 * arrive before one call and none before the next.
 *
 * Lowest Latency always shows the newest frame. In the collision zone, this
 * alternates dropped and repeated frames for a few seconds.
 *
 * Smooth advances one frame per call: when two frames are ready, it shows the
 * older one instead of dropping it, which costs one frame of latency. Once
 * frames arrive away from the collision zone again (for PACING_SKIP_RUNS
 * calls in a row), it skips one frame to catch up. The clock difference then
 * costs a single skipped frame per drift cycle instead of a burst of drops
 * and repeats.
 *
 * Returns true and fills `frame` when a frame newer than the last one shown
 * is available.
 */
static bool pick_frame(capture_frame_t *frame)
{
   uint64_t oldest, newest, pick;
   int64_t newest_arrival_usec;

   if (!capture_peek_frames(&oldest, &newest, &newest_arrival_usec) || newest <= shown_sequence)
      return false;

   pick = newest;

   if (opt_smooth_pacing && shown_sequence && !pacing_resync)
   {
      const uint64_t backlog   = newest - shown_sequence; /* New frames available */
      const double period_ms   = 1000.0 / output_fps;
      const double newest_age  = (double)(capture_time_usec() - newest_arrival_usec) / 1000.0;
      const bool   safe_phase  = newest_age >= PACING_GUARD_MS && newest_age <= period_ms - PACING_GUARD_MS;

      /* Behind by one frame, away from the collision zone: count down to a catch-up. */
      pacing_safe_runs = (backlog >= 2 && safe_phase) ? pacing_safe_runs + 1 : 0;

      if (backlog >= 3 || pacing_safe_runs >= PACING_SKIP_RUNS)
      {
         stats_pacing_skips   += backlog - 1;
         session_pacing_skips += backlog - 1;
         pacing_safe_runs      = 0;
      }
      else
      {
         pick = shown_sequence + 1;
         if (pick < oldest)
            pick = oldest; /* The next frame is gone (for example after a pause) */
      }
   }

   if (!capture_acquire_frame(pick, frame))
      return false;

   /* Frames between the last one shown and this one are never shown. After a
    * device start or a pause, missed frames are not drops. */
   if (shown_sequence && !pacing_resync && pick > shown_sequence + 1)
   {
      stats_dropped   += pick - shown_sequence - 1;
      session_dropped += pick - shown_sequence - 1;
   }

   shown_sequence = pick;
   pacing_resync  = false;
   return true;
}

/* Fetches the latest frame and works out whether there is a usable picture. */
static enum signal_state poll_capture(bool *new_frame)
{
   const double frame_time = 1.0 / output_fps;
   capture_frame_t frame;

   *new_frame = false;

   if (!active_device_id[0])
      return SIGNAL_NO_DEVICE;

   if (!capture_is_running())
   {
      /* Opening failed or the stream stopped: retry periodically, for example
       * once another application releases the device. */
      retry_timer -= frame_time;
      if (retry_timer <= 0.0)
         apply_device_selection(true);
      return capture_is_running() ? SIGNAL_NO_FRAMES : SIGNAL_DEVICE_ERROR;
   }

   if (pick_frame(&frame))
   {
      last_frame            = frame;
      have_frame            = true;
      seconds_without_frame = 0.0;
      seconds_black         = convert_frame_is_black(&frame) ? seconds_black + frame_time : 0.0;
      stats_frames_shown++;
      session_frames_shown++;

      if (frame.arrival_usec)
      {
         const double age_ms = (double)(capture_time_usec() - frame.arrival_usec) / 1000.0;
         if (!frame_age_count || age_ms < frame_age_min_ms) frame_age_min_ms = age_ms;
         if (!frame_age_count || age_ms > frame_age_max_ms) frame_age_max_ms = age_ms;
         frame_age_sum_ms += age_ms;
         frame_age_count++;
      }
      *new_frame = true;
      update_frame_rate(measure_frame_rate(&frame));
   }
   else
   {
      seconds_without_frame += frame_time;
      if (have_frame)
      {
         stats_repeated++;
         session_repeated++;
      }
   }

   if (!have_frame || seconds_without_frame >= NO_FRAMES_TIMEOUT)
      return SIGNAL_NO_FRAMES;
   if (seconds_black >= BLACK_TIMEOUT)
      return SIGNAL_BLACK;
   return SIGNAL_OK;
}

static void present_frame(bool options_changed)
{
   bool new_frame;
   const enum signal_state state = poll_capture(&new_frame);

   if (state != SIGNAL_OK)
   {
      output_no_signal_screen(state);
      return;
   }

   screen_key[0] = '\0'; /* Redraw the No Signal screen next time it appears */

   if (opt_stats != STATS_OFF)
      update_stats();

   /* Show the new settings right away, even without a new frame. */
   if (new_frame || options_changed || last_output == screen_buffer)
      output_capture_frame();
   else if (can_dupe)
      video_cb(NULL, output_width, output_height, last_output_pitch);
   else
      video_cb(last_output, output_width, output_height, last_output_pitch);
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

   /* The core starts without content ("Start Core") or with a .vidcap file. */
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
   memset(silence, 0, sizeof(silence));
   screen_key[0] = '\0';
}

RETRO_API void retro_deinit(void)
{
   capture_stop();
   if (stats_log_file)
   {
      fclose(stats_log_file);
      stats_log_file = NULL;
   }
   free(rgb_buffer);
   rgb_buffer      = NULL;
   rgb_buffer_size = 0;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = CORE_NAME;
   info->library_version  = CORE_VERSION;
   info->valid_extensions = "vidcap";
   info->need_fullpath    = true; /* The file's content is not used */
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
   apply_device_selection(true);
}

/* Detects pauses (RetroArch's menu, shader setup) from the time since the
 * previous call. A pause restarts the current stats second, excludes the
 * pause from the session summary, and makes the core resume on the newest
 * frame. */
static void detect_pause(void)
{
   const retro_time_t now  = perf_cb.get_time_usec ? perf_cb.get_time_usec() : 0;
   const uint64_t received = capture_frames_received();
   const bool first_run    = waiting_first_run;

   waiting_first_run = false;
   if (!now)
      return;

   if (last_run_usec && now - last_run_usec >= STALL_THRESHOLD_USEC)
   {
      const retro_time_t gap = now - last_run_usec;

      session_paused_usec   += gap;
      session_paused_frames += received >= last_run_received ? received - last_run_received : 0;
      stats_last_time        = 0; /* Restart the stats second */
      pacing_resync          = true;

      if (first_run)
         stats_log_printf("RetroArch started running the core %.1f s after the device opened.",
               (double)gap / 1000000.0);
      else if (gap >= PAUSE_THRESHOLD_USEC)
         stats_log_printf("Paused for %.1f s.", (double)gap / 1000000.0);
      else
         stats_log_printf("RetroArch stalled for %.0f ms.", (double)gap / 1000.0);
   }

   last_run_usec     = now;
   last_run_received = received;
}

RETRO_API void retro_run(void)
{
   bool updated = false;

   detect_pause();
   session_runs++;

   if (input_poll_cb)
      input_poll_cb();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      read_options();
      log_options(false);
      update_stats_log_file();
      apply_device_selection(false);
      screen_key[0] = '\0';
   }

   present_frame(updated);

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
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "XRGB8888 is not supported by the frontend.\n");
      return false;
   }

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe))
      can_dupe = false;
   if (!environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      memset(&perf_cb, 0, sizeof(perf_cb));
   if (!environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &message_interface_version))
      message_interface_version = 0;


   log_cb(RETRO_LOG_INFO, LOG_PREFIX "Core started (version %s).\n", CORE_VERSION);
   if (game && game->path)
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "Content: %s\n", game->path);

   read_options();
   update_stats_log_file();
   active_device_id[0] = '\0';
   apply_device_selection(true);

   /* retro_get_system_av_info() is called after this: report the device rate directly. */
   output_fps = valid_fps(capture_get_frame_rate());
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
   log_session_stats();
   capture_stop();
   active_device_id[0] = '\0';
   have_frame          = false;
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
