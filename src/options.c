/*
 * Core options.
 *
 * All options are described once, in option_specs[] (plus the capture device
 * and audio input options, whose values are the devices found at startup).
 * From that description, options_declare() builds both formats a frontend may
 * ask for: core options v2 (with categories, labels and descriptions) and the
 * legacy "Description; value1|value2" format.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "options.h"

#define OPTION_RESOLUTION  "video_capture_resolution"
#define OPTION_ASPECT      "video_capture_aspect"
#define OPTION_CAPTURE_RES "video_capture_capture_resolution"
#define OPTION_CONVERSION  "video_capture_conversion"
#define OPTION_COLOR_SPACE "video_capture_color_space"
#define OPTION_COLOR_RANGE "video_capture_color_range"
#define OPTION_STATS       "video_capture_stats"
#define OPTION_STATS_LOG   "video_capture_stats_log"
#define OPTION_DETAILS     "video_capture_no_signal_details"
#define OPTION_PACING      "video_capture_frame_pacing"

/* Current values (see options.h). */
enum output_resolution  opt_resolution     = RESOLUTION_NATIVE;
enum aspect_mode        opt_aspect         = ASPECT_SOURCE;
unsigned                opt_capture_height = 0;
unsigned                opt_crop_h         = 0;
unsigned                opt_crop_v         = 0;
bool                    opt_core_convert   = true;
enum color_space_option opt_color_space    = COLOR_SPACE_DEFAULT;
enum color_range        opt_color_range    = COLOR_RANGE_LIMITED;
enum stats_mode         opt_stats          = STATS_OFF;
bool                    opt_stats_log      = false;
bool                    opt_details        = false;
bool                    opt_smooth_pacing  = true;

capture_device_t devices[CAPTURE_MAX_DEVICES];
int              device_count;
char             option_device_names[CAPTURE_MAX_DEVICES][sizeof(((capture_device_t *)0)->name) + 16];
audio_device_t   audio_devices[AUDIO_MAX_DEVICES];
int              audio_device_count;
char             option_audio_names[AUDIO_MAX_DEVICES][sizeof(((audio_device_t *)0)->name) + 16];

/* ------------------------------------------------------------------------ */
/* Option values                                                             */
/* ------------------------------------------------------------------------ */

/* Each list alternates values and labels (a NULL label shows the value) and
 * ends with NULL. The first value is the default. */
static const char *const capture_res_values[] = {
   "auto", "Auto (up to 1080p)", "1080", "1080p", "720", "720p", "576", "576p", "480", "480p", NULL
};
static const char *const resolution_values[] = {
   "native", "Native", "640x480", NULL, "320x240", NULL, NULL
};
static const char *const aspect_values[] = {
   "source", "Source", "4_3_squash", "4:3 (Squash)", "4_3_crop", "4:3 (Crop Pillarbox)",
   "16_9", "16:9", NULL
};
static const char *const pacing_values[] = {
   "smooth", "Smooth", "latest", "Lowest Latency", NULL
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
static const char *const off_on_values[] = {
   "off", "Off", "on", "On", NULL
};

/* Crop values, built at startup: "0" / "Off", "4" / "4 px", "8" / "8 px"... */
static char        crop_text[2][CROP_VALUES][2][12];
static const char *crop_values[2][CROP_VALUES * 2 + 1];

static void build_crop_values(int axis, unsigned step)
{
   int i;

   for (i = 0; i < CROP_VALUES; i++)
   {
      snprintf(crop_text[axis][i][0], sizeof(crop_text[axis][i][0]), "%u", i * step);
      if (i == 0)
         snprintf(crop_text[axis][i][1], sizeof(crop_text[axis][i][1]), "Off");
      else
         snprintf(crop_text[axis][i][1], sizeof(crop_text[axis][i][1]), "%u px", i * step);
      crop_values[axis][i * 2]     = crop_text[axis][i][0];
      crop_values[axis][i * 2 + 1] = crop_text[axis][i][1];
   }
   crop_values[axis][CROP_VALUES * 2] = NULL;
}

/* ------------------------------------------------------------------------ */
/* Option descriptions                                                       */
/* ------------------------------------------------------------------------ */

struct option_spec
{
   const char        *category; /* Submenu key, NULL for the main page */
   const char        *key;
   const char        *desc;
   const char        *info;
   const char *const *values;
};

/* Every option except the capture device and audio input, in menu order. */
static const struct option_spec option_specs[] = {
   { "video", OPTION_CAPTURE_RES, "Capture Resolution",
     "Resolution requested from the device. Choose the source's resolution, if the device offers it, "
     "so that the device does not scale the picture. Changing it reopens the device.",
     capture_res_values },
   { "video", OPTION_RESOLUTION, "Output Resolution",
     "Size of the picture sent to RetroArch and its shaders. CRT and VHS shaders expect a low "
     "resolution, such as 640x480 or 320x240.",
     resolution_values },
   { "video", OPTION_ASPECT, "Aspect Ratio",
     "Shape of the picture. 4:3 (Squash) stretches the whole picture to 4:3. 4:3 (Crop Pillarbox) "
     "keeps its central 4:3 part. RetroArch's Aspect Ratio setting must be Core Provided.",
     aspect_values },
   { "video", OPTION_CROP_H, "Crop Horizontal",
     "Pixels removed on the left and on the right, for example black borders. "
     "Converted automatically when the captured size changes.",
     crop_values[0] },
   { "video", OPTION_CROP_V, "Crop Vertical",
     "Pixels removed at the top and at the bottom. "
     "Converted automatically when the captured size changes.",
     crop_values[1] },
   { "video", OPTION_PACING, "Frame Pacing",
     "Smooth shows every frame, at the cost of up to one frame of latency at times. "
     "Lowest Latency always shows the newest frame, but can drop and repeat frames.",
     pacing_values },
   { "color", OPTION_CONVERSION, "Color Conversion",
     "Core converts the colors after downscaling, which is faster, and uses Color Space and "
     "Color Range. Media Foundation lets Windows convert them. Changing it reopens the device.",
     conversion_values },
   { "color", OPTION_COLOR_SPACE, "Color Space",
     "Color matrix of the source. Default uses Rec. 601 up to 576 lines and Rec. 709 above.",
     color_space_values },
   { "color", OPTION_COLOR_RANGE, "Color Range",
     "Limited is standard for video. Use Full if blacks look gray and colors look washed out.",
     color_range_values },
   { "diagnostics", OPTION_STATS, "Show Capture Stats",
     "Frame rates of the device, of RetroArch and of the picture shown. In Image draws them into "
     "the picture, before shaders.",
     stats_values },
   { "diagnostics", OPTION_STATS_LOG, "Log Capture Stats",
     "Writes the stats every second to video_capture_stats.log, in RetroArch's save directory.",
     off_on_values },
   { "diagnostics", OPTION_DETAILS, "No Signal Details",
     "Shows the cause and troubleshooting hints on the No Signal screen.",
     off_on_values },
};

#define OPTION_SPEC_COUNT (sizeof(option_specs) / sizeof(option_specs[0]))
#define OPTION_COUNT      (OPTION_SPEC_COUNT + 2) /* Plus the capture device and audio input */

static struct retro_core_option_v2_category categories[] = {
   { "video", "Video", "Capture and output resolution, aspect ratio, crop and frame pacing." },
   { "color", "Color", "Conversion of the device's YUV picture to RGB." },
   { "diagnostics", "Diagnostics", "Capture stats and No Signal screen details." },
   { NULL, NULL, NULL },
};

/* Built by options_declare(); the extra entry terminates the list. */
static struct retro_core_option_v2_definition definitions[OPTION_COUNT + 1];

/* Last value of each option, to log changes. */
static char last_values[OPTION_COUNT][sizeof(option_device_names[0])];

/* ------------------------------------------------------------------------ */
/* Devices                                                                   */
/* ------------------------------------------------------------------------ */

/* Gives each name in `names` (`stride` bytes apart) a unique option value in
 * `out`: identical names are numbered ("Name #2"). */
static void make_unique_names(const char *names, size_t stride, int count, char *out, size_t out_size)
{
   int i, j;

   for (i = 0; i < count; i++)
   {
      const char *name = names + (size_t)i * stride;
      int duplicates = 0;

      for (j = 0; j < i; j++)
         if (strcmp(name, names + (size_t)j * stride) == 0)
            duplicates++;

      if (duplicates)
         snprintf(out + (size_t)i * out_size, out_size, "%.255s #%d", name, duplicates + 1);
      else
         snprintf(out + (size_t)i * out_size, out_size, "%.255s", name);
   }
}

void options_find_devices(void)
{
   int i;

   device_count = capture_list_devices(devices, CAPTURE_MAX_DEVICES);
   make_unique_names(devices[0].name, sizeof(devices[0]), device_count,
         option_device_names[0], sizeof(option_device_names[0]));

   log_cb(RETRO_LOG_INFO, LOG_PREFIX "%d capture device(s) found.\n", device_count);
   for (i = 0; i < device_count; i++)
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "  %s\n", option_device_names[i]);

   audio_device_count = audio_list_devices(audio_devices, AUDIO_MAX_DEVICES);
   make_unique_names(audio_devices[0].name, sizeof(audio_devices[0]), audio_device_count,
         option_audio_names[0], sizeof(option_audio_names[0]));

   log_cb(RETRO_LOG_INFO, LOG_PREFIX "%d audio input(s) found.\n", audio_device_count);
   for (i = 0; i < audio_device_count; i++)
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "  %s\n", option_audio_names[i]);
}

/* ------------------------------------------------------------------------ */
/* Declaration                                                               */
/* ------------------------------------------------------------------------ */

static void fill_definition(struct retro_core_option_v2_definition *def, const char *category,
      const char *key, const char *desc, const char *info, const char *const *values)
{
   int i;

   def->category_key  = category;
   def->key           = key;
   def->desc          = desc;
   def->info          = info;
   def->default_value = values[0];
   for (i = 0; values[i * 2] && i < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1; i++)
   {
      def->values[i].value = values[i * 2];
      def->values[i].label = values[i * 2 + 1];
   }
}

/* Fills definitions[]: the capture device and audio input first, on the main
 * page, then option_specs[]. */
static void build_definitions(void)
{
   struct retro_core_option_v2_definition *def = definitions;
   size_t i;
   int n;

   memset(definitions, 0, sizeof(definitions));

   def->key  = OPTION_DEVICE;
   def->desc = "Capture Device";
   def->info = "Video capture device to show. Auto selects the first device found. "
               "Devices are detected when the core starts.";
   def->values[0].value = "auto";
   def->values[0].label = "Auto (first device)";
   for (n = 0; n < device_count && n < RETRO_NUM_CORE_OPTION_VALUES_MAX - 2; n++)
      def->values[n + 1].value = option_device_names[n];
   def->default_value = "auto";
   def++;

   def->key  = OPTION_AUDIO;
   def->desc = "Audio Input";
   def->info = "Audio played with the picture. Auto selects the input that belongs to the capture "
               "device. Leave it off if the sound already reaches your speakers another way.";
   def->values[0].value = "off";
   def->values[0].label = "Off";
   def->values[1].value = "auto";
   def->values[1].label = "Auto (match capture device)";
   for (n = 0; n < audio_device_count && n < RETRO_NUM_CORE_OPTION_VALUES_MAX - 3; n++)
      def->values[n + 2].value = option_audio_names[n];
   def->default_value = "off";
   def++;

   for (i = 0; i < OPTION_SPEC_COUNT; i++, def++)
      fill_definition(def, option_specs[i].category, option_specs[i].key, option_specs[i].desc,
            option_specs[i].info, option_specs[i].values);
}

static bool update_option_visibility(void);

/* Declares definitions[] in the legacy format: "Description; default|other|...". */
static void declare_legacy_options(void)
{
   static struct retro_variable variables[OPTION_COUNT + 1];
   static char text[16384];
   size_t used = 0, i;

   for (i = 0; i < OPTION_COUNT; i++)
   {
      const struct retro_core_option_v2_definition *def = &definitions[i];
      char *start = text + used;
      int v;

      used += (size_t)snprintf(start, sizeof(text) - used, "%s; ", def->desc);
      for (v = 0; def->values[v].value && used < sizeof(text); v++)
         used += (size_t)snprintf(text + used, sizeof(text) - used, v ? "|%s" : "%s", def->values[v].value);
      if (used >= sizeof(text) - 1)
         break; /* Out of room: declare the options built so far */
      used++; /* Keep the terminating NUL */

      variables[i].key   = def->key;
      variables[i].value = start;
   }
   variables[i].key   = NULL;
   variables[i].value = NULL;
   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

void options_declare(void)
{
   unsigned version = 0;

   build_crop_values(0, CROP_H_STEP);
   build_crop_values(1, CROP_V_STEP);
   build_definitions();

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
      struct retro_core_options_v2 options;
      struct retro_core_options_update_display_callback callback;

      options.categories  = categories;
      options.definitions = definitions;
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);

      callback.callback = update_option_visibility;
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &callback);
   }
   else
      declare_legacy_options();
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */
/* ------------------------------------------------------------------------ */

const char *options_get(const char *key)
{
   struct retro_variable var;

   var.key   = key;
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      return var.value;
   return NULL;
}

bool option_is(const char *value, const char *expected)
{
   return value && strcmp(value, expected) == 0;
}

void options_read(void)
{
   const char *value;

   value = options_get(OPTION_CAPTURE_RES);
   opt_capture_height = value && !option_is(value, "auto") ? (unsigned)strtoul(value, NULL, 10) : 0;

   value = options_get(OPTION_RESOLUTION);
   opt_resolution = option_is(value, "640x480") ? RESOLUTION_640X480
                  : option_is(value, "320x240") ? RESOLUTION_320X240
                  : RESOLUTION_NATIVE;

   value = options_get(OPTION_ASPECT);
   opt_aspect = option_is(value, "4_3_squash") ? ASPECT_4_3_SQUASH
              : option_is(value, "4_3_crop")   ? ASPECT_4_3_CROP
              : option_is(value, "16_9")       ? ASPECT_16_9
              : ASPECT_SOURCE;

   /* Crop values are even: YUV pixels come in pairs. */
   value = options_get(OPTION_CROP_H);
   opt_crop_h = value ? (unsigned)strtoul(value, NULL, 10) & ~1u : 0;
   value = options_get(OPTION_CROP_V);
   opt_crop_v = value ? (unsigned)strtoul(value, NULL, 10) & ~1u : 0;

   opt_smooth_pacing = !option_is(options_get(OPTION_PACING), "latest");
   opt_core_convert  = !option_is(options_get(OPTION_CONVERSION), "media_foundation");

   value = options_get(OPTION_COLOR_SPACE);
   opt_color_space = option_is(value, "rec709") ? COLOR_SPACE_REC709
                   : option_is(value, "rec601") ? COLOR_SPACE_REC601
                   : COLOR_SPACE_DEFAULT;

   opt_color_range = option_is(options_get(OPTION_COLOR_RANGE), "full") ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED;

   value = options_get(OPTION_STATS);
   opt_stats = option_is(value, "notification") ? STATS_NOTIFICATION
             : option_is(value, "image")        ? STATS_IMAGE
             : STATS_OFF;

   opt_stats_log = option_is(options_get(OPTION_STATS_LOG), "on");
   opt_details   = option_is(options_get(OPTION_DETAILS), "on");
}

int options_device_index(void)
{
   const char *value = options_get(OPTION_DEVICE);
   int i;

   if (device_count == 0)
      return -1;

   if (value && strcmp(value, "auto") != 0)
   {
      for (i = 0; i < device_count; i++)
         if (strcmp(value, option_device_names[i]) == 0)
            return i;

      log_cb(RETRO_LOG_WARN, LOG_PREFIX "Device \"%s\" not found, using the first device.\n", value);
   }

   return 0;
}

/* ------------------------------------------------------------------------ */
/* Visibility and logging                                                    */
/* ------------------------------------------------------------------------ */

/* Shows or hides an option. `state` remembers the last visibility (-1 when
 * unknown); returns true if it changed. */
static bool set_option_visible(const char *key, bool visible, int *state)
{
   struct retro_core_option_display display;

   if (*state == (int)visible)
      return false;

   display.key     = key;
   display.visible = visible;
   environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
   *state = visible;
   return true;
}

/* Hides options that have no effect: Log Capture Stats while Show Capture
 * Stats is off, and Color Space and Color Range while Color Conversion is set
 * to Media Foundation, which ignores them. The frontend calls this when the
 * options menu is shown and after each change; it returns true if a
 * visibility changed, so the menu is redrawn. */
static bool update_option_visibility(void)
{
   static int stats_log_state = -1, color_space_state = -1, color_range_state = -1;
   const char *stats      = options_get(OPTION_STATS);
   const bool show_log    = stats && !option_is(stats, "off");
   const bool show_colors = !option_is(options_get(OPTION_CONVERSION), "media_foundation");
   bool changed = false;

   changed |= set_option_visible(OPTION_STATS_LOG, show_log, &stats_log_state);
   changed |= set_option_visible(OPTION_COLOR_SPACE, show_colors, &color_space_state);
   changed |= set_option_visible(OPTION_COLOR_RANGE, show_colors, &color_range_state);
   return changed;
}

/* Returns the label shown in the menu for an option value. */
static const char *option_label(const struct retro_core_option_v2_definition *def, const char *value)
{
   int i;

   for (i = 0; def->values[i].value; i++)
      if (strcmp(def->values[i].value, value) == 0)
         return def->values[i].label ? def->values[i].label : value;
   return value;
}

void options_log(bool all)
{
   size_t i;

   for (i = 0; i < OPTION_COUNT && definitions[i].key; i++)
   {
      const struct retro_core_option_v2_definition *def = &definitions[i];
      const char *value = options_get(def->key);

      if (!value)
         continue;
      if (all || strcmp(value, last_values[i]) != 0)
         stats_log_printf("%s%s: %s.", all ? "  " : "Option changed: ", def->desc, option_label(def, value));
      snprintf(last_values[i], sizeof(last_values[i]), "%s", value);
   }
}
