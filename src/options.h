/*
 * Core options: declaration to the frontend, current values, and the lists of
 * capture devices and audio inputs offered in the options.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_CAPTURE_OPTIONS_H
#define VIDEO_CAPTURE_OPTIONS_H

#include <stdbool.h>

#include "audio.h"
#include "capture.h"
#include "convert.h"

/* Option keys used outside options.c. */
#define OPTION_DEVICE  "video_capture_device"
#define OPTION_AUDIO   "video_capture_audio_device"
#define OPTION_CROP_H  "video_capture_crop_horizontal"
#define OPTION_CROP_V  "video_capture_crop_vertical"

/* Crop options: pixels removed on each side. Horizontal steps are larger to
 * fit RetroArch's limit of 128 values per option (127 plus the terminator). */
#define CROP_VALUES   127
#define CROP_H_STEP   4   /* 0 to 504 */
#define CROP_V_STEP   2   /* 0 to 252; even, as NV12 rows come in pairs */

enum output_resolution
{
   RESOLUTION_NATIVE = 0,
   RESOLUTION_640X480,
   RESOLUTION_320X240
};

enum aspect_mode
{
   ASPECT_SOURCE = 0,  /* Display aspect follows the crop area (16:9 for an uncropped 1080p frame) */
   ASPECT_4_3_SQUASH,  /* Full crop area, displayed as 4:3 */
   ASPECT_4_3_CROP,    /* Central 4:3 part of the crop area, pillarbox removed */
   ASPECT_16_9         /* Full crop area, displayed as 16:9 */
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

/* Current option values, updated by options_read(). The capture device and
 * audio input are read by their own modules (options_device_index(),
 * options_get()). */
extern enum output_resolution  opt_resolution;
extern enum aspect_mode        opt_aspect;
extern unsigned                opt_capture_height; /* Preferred device format height, 0 for Auto */
extern unsigned                opt_crop_h;         /* Pixels removed on the left and on the right */
extern unsigned                opt_crop_v;         /* Pixels removed at the top and at the bottom */
extern bool                    opt_core_convert;   /* Color Conversion: Core (true) or Media Foundation */
extern enum color_space_option opt_color_space;
extern enum color_range        opt_color_range;
extern enum stats_mode         opt_stats;
extern bool                    opt_stats_log;
extern bool                    opt_details;
extern bool                    opt_smooth_pacing;

/* Devices and audio inputs found at startup. The option values are their
 * names, numbered when several are identical ("Name #2"). */
extern capture_device_t devices[CAPTURE_MAX_DEVICES];
extern int              device_count;
extern char             option_device_names[CAPTURE_MAX_DEVICES][sizeof(((capture_device_t *)0)->name) + 16];
extern audio_device_t   audio_devices[AUDIO_MAX_DEVICES];
extern int              audio_device_count;
extern char             option_audio_names[AUDIO_MAX_DEVICES][sizeof(((audio_device_t *)0)->name) + 16];

/* Lists the capture devices and audio inputs. Called once, before
 * options_declare(), since they become option values. */
void options_find_devices(void);

/* Declares the options to the frontend (core options v2, or the legacy
 * format for older frontends). */
void options_declare(void);

/* Reads the current option values into the opt_* variables. */
void options_read(void);

/* Returns an option's current value, or NULL. */
const char *options_get(const char *key);

/* True if `value` is not NULL and equals `expected`. */
bool option_is(const char *value, const char *expected);

/* Index in devices[] of the selected capture device, or -1 if there is none. */
int options_device_index(void);

/* Writes the options to the stats log: all of them with `all`, otherwise
 * only those changed since the previous call. */
void options_log(bool all);

#endif
