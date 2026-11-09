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
#include "audio.h"
#include "audio_output.h"
#include "capture.h"
#include "convert.h"
#include "core.h"
#include "font.h"
#include "no_signal.h"
#include "options.h"
#include "scale.h"

/* ------------------------------------------------------------------------ */
/* Constants                                                                 */
/* ------------------------------------------------------------------------ */

/* Seconds without frames before showing the No Signal screen. */
#define NO_FRAMES_TIMEOUT 2.0
/* Seconds of uniformly black frames before showing the No Signal screen. */
#define BLACK_TIMEOUT     1.0
/* Seconds the last image is kept after the device reopens, before showing the
 * No Signal screen. Avoids switching to the No Signal screen's size and back,
 * which makes RetroArch rebuild its shaders twice. */
#define REOPEN_HOLD       1.0

/* Smooth frame pacing (see pick_frame()). A frame arriving within this many
 * milliseconds of retro_run() is in the "collision zone", where timing noise
 * decides whether it is ready in time. */
#define PACING_GUARD_MS   2.5
/* retro_run() calls outside the collision zone before catching up one frame. */
#define PACING_SKIP_RUNS  30

/* Pause detection (see detect_pause()). RetroArch stops calling retro_run()
 * while its menu is open, and briefly while it rebuilds a shader (for example
 * when the output size changes). A gap of STALL_THRESHOLD_USEC or more is
 * excluded from the stats; from PAUSE_THRESHOLD_USEC, it is logged as a pause
 * rather than a stall. */
#define STALL_THRESHOLD_USEC 100000
#define PAUSE_THRESHOLD_USEC 500000

/* Frame rate measurement (see measure_frame_rate()). */
#define RATE_MIN_USEC     10000000 /* Measure over at least 10 seconds */
#define RATE_GAP_USEC     500000   /* A longer interval between two device frames restarts the measurement */
#define RATE_TOLERANCE    0.0004   /* Relative difference accepted between measured and candidate rates */
#define RATE_CONFIRM_USEC 5000000  /* A new rate must still match 5 seconds later */

/* Stats text colors: as expected, slightly off, or frames are being lost. */
#define STATS_COLOR_GOOD    0x40FF40
#define STATS_COLOR_NEUTRAL 0xFFFFFF
#define STATS_COLOR_BAD     0xFF4040
#define STATS_LINES         3

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

/* Frontend callbacks (environ_cb and log_cb are shared through core.h). */
retro_environment_t               environ_cb;
retro_log_printf_t                log_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static struct retro_perf_callback perf_cb;
static unsigned                   message_interface_version;
static bool                       can_dupe; /* The frontend accepts NULL to repeat the last image */

/* Open capture device (the list of devices is in options.c). */
static char        active_device_id[sizeof(devices[0].id)]; /* Empty when no device is open */
static const char *active_device_name = "";
static bool        active_core_convert;   /* Color Conversion the device was opened with */
static unsigned    active_capture_height; /* Capture Resolution the device was opened with */
static double      retry_timer;           /* Seconds until the next reopen attempt */

/* Frames from the device */
static capture_frame_t last_frame;     /* Last frame picked; valid while have_frame */
static bool            have_frame;
static double          seconds_without_frame;
static double          seconds_black;  /* Uniformly black frames for this long */

/* Output to the frontend */
static uint32_t       *rgb_buffer;      /* Converted or scaled image */
static size_t          rgb_buffer_size; /* In pixels */
static const uint32_t *last_output;
static size_t          last_output_pitch;
static bool            showing_capture; /* The last image sent is a captured frame, not the No Signal screen */
static unsigned        output_width  = NO_SIGNAL_WIDTH;
static unsigned        output_height = NO_SIGNAL_HEIGHT;
static float           output_aspect = 4.0f / 3.0f;
static double          output_fps    = DEFAULT_FPS;
static unsigned        crop_size_width;  /* Captured size the crop values apply to, 0 if unknown */
static unsigned        crop_size_height;

/* Frame pacing (see pick_frame()) */
static uint64_t shown_sequence;   /* Sequence of the frame shown last, 0 if none */
static unsigned pacing_safe_runs; /* Consecutive runs behind, outside the collision zone */
static bool     pacing_resync;    /* Show the newest frame next, without counting drops */

/* Pause detection (see detect_pause()) */
static retro_time_t last_run_usec;     /* Time of the last retro_run() */
static uint64_t     last_run_received; /* Frames received from the device at that time */
static bool         waiting_first_run; /* Device opened, retro_run() not called since */

/* Frame rate measurement (see measure_frame_rate()) */
static uint64_t rate_anchor_sequence; /* First frame of the measurement, 0 if none */
static int64_t  rate_anchor_usec;
static uint64_t rate_last_sequence;
static int64_t  rate_last_usec;
static double   rate_nominal;         /* Rate declared by the device */
static double   rate_reported;        /* Rate reported to RetroArch */
static double   rate_candidate;       /* Rate found by the measurement, not confirmed yet */
static int64_t  rate_candidate_usec;  /* Arrival time of the frame that found it */

/* Stats over the current second (see update_stats()) */
static retro_time_t stats_last_time;     /* Start of the current second, 0 to restart */
static uint64_t     stats_last_received;
static uint64_t     stats_frames_shown;  /* New frames shown */
static uint64_t     stats_runs;          /* retro_run() calls */
static uint64_t     stats_dropped;       /* Frames never shown */
static uint64_t     stats_pacing_skips;  /* Of which skipped on purpose by Smooth pacing */
static uint64_t     stats_repeated;      /* Calls that repeated the previous frame */
static double       frame_age_min_ms;    /* Age of the frames shown: time from arrival to retro_run() */
static double       frame_age_max_ms;
static double       frame_age_sum_ms;
static unsigned     frame_age_count;
static unsigned     stats_seconds;       /* Seconds logged since the stats log was opened */
static double       stats_capture_fps;   /* Results of the last second */
static double       stats_frontend_fps;
static double       stats_output_fps;
static char         stats_lines[STATS_LINES][96];
static uint32_t     stats_colors[STATS_LINES];

/* Session totals since the device was opened, logged when it is closed. */
static retro_time_t session_start_time;
static uint64_t     session_frames_shown;
static uint64_t     session_runs;
static uint64_t     session_dropped;
static uint64_t     session_pacing_skips;
static uint64_t     session_repeated;
static retro_time_t session_paused_usec;   /* Time RetroArch did not run the core (menu, stalls) */
static uint64_t     session_paused_frames; /* Frames received meanwhile */

/* ------------------------------------------------------------------------ */
/* Logging                                                                   */
/* ------------------------------------------------------------------------ */

retro_time_t core_time_usec(void)
{
   return perf_cb.get_time_usec ? perf_cb.get_time_usec() : 0;
}

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

void stats_log_printf(const char *fmt, ...)
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
   options_log(true);
}

/* Shows `text` on screen for `duration_ms`. A STATUS message is meant to be
 * refreshed (the stats); a NOTIFICATION is a one-time message. Frontends
 * older than message interface 1 only know a duration in frames. */
static void send_message(const char *text, unsigned duration_ms, enum retro_message_type type)
{
   if (message_interface_version >= 1)
   {
      struct retro_message_ext message;

      message.msg      = text;
      message.duration = duration_ms;
      message.priority = 1;
      message.level    = RETRO_LOG_INFO;
      message.target   = RETRO_MESSAGE_TARGET_OSD;
      message.type     = type;
      message.progress = -1;
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
   }
   else
   {
      struct retro_message message;

      message.msg    = text;
      message.frames = (unsigned)(output_fps * duration_ms / 1000.0);
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
   }
}

void show_notification(const char *text, unsigned duration_ms)
{
   send_message(text, duration_ms, RETRO_MESSAGE_TYPE_NOTIFICATION);
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


/* Logs a summary of the capture session: average rates and frames lost.
 * Called when the device is closed, so the log keeps a trace of dropped
 * frames without watching the on-screen stats. */
static void log_session_stats(void)
{
   /* The session ends at the last retro_run(): time spent in RetroArch's menu
    * before closing the content is not counted. */
   const bool at_last_run  = last_run_usec > session_start_time;
   const retro_time_t now  = at_last_run ? last_run_usec : core_time_usec();
   const uint64_t received = (at_last_run ? last_run_received : capture_frames_received()) - session_paused_frames;
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
   audio_output_log_session();
}

static void reset_session_stats(void)
{
   session_start_time   = core_time_usec();
   session_frames_shown  = 0;
   session_runs          = 0;
   session_dropped       = 0;
   session_pacing_skips  = 0;
   session_repeated      = 0;
   session_paused_usec   = 0;
   session_paused_frames = 0;
   audio_output_reset_session();
}

static void restart_stats_second(retro_time_t end);

/* ------------------------------------------------------------------------ */
/* Device selection                                                          */
/* ------------------------------------------------------------------------ */

/* Opens the device selected in the core options. Without `force`, does
 * nothing if the device and the options that require reopening it (Color
 * Conversion, Capture Resolution) are unchanged. With `force`, reopens it
 * anyway: this is how a device that failed is retried. */
static void apply_device_selection(bool force)
{
   static bool start_failed; /* The device could not be opened; already reported */
   const int index = options_device_index();

   if (index < 0)
   {
      active_device_id[0] = '\0';
      active_device_name  = "";
      return;
   }

   if (!force && strcmp(devices[index].id, active_device_id) == 0 &&
       active_core_convert == opt_core_convert && active_capture_height == opt_capture_height)
      return;

   if (strcmp(devices[index].id, active_device_id) != 0)
      start_failed = false; /* Another device: report its failure, if any */

   snprintf(active_device_id, sizeof(active_device_id), "%s", devices[index].id);
   active_device_name    = option_device_names[index];
   active_core_convert   = opt_core_convert;
   active_capture_height = opt_capture_height;
   have_frame            = false;
   seconds_without_frame = 0.0;
   seconds_black         = 0.0;
   retry_timer           = RETRY_INTERVAL;

   restart_stats_second(core_time_usec());
   log_session_stats();
   reset_session_stats();
   shown_sequence       = 0;
   pacing_safe_runs     = 0;
   pacing_resync        = false;
   rate_anchor_sequence = 0;
   rate_nominal         = 0.0;
   rate_candidate       = 0.0;

   if (capture_start(devices[index].id, opt_core_convert, opt_capture_height))
   {
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "Capture started: %s.\n", active_device_name);
      start_failed = false;
   }
   else if (!start_failed)
   {
      /* Logged once: the retries every RETRY_INTERVAL would repeat it. */
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "Could not start capture: %s Retrying every few seconds.\n",
            capture_last_error());
      start_failed = true;
   }

   /* The time until the next retro_run() is treated like a pause: at startup,
    * RetroArch sets up its video driver and shaders after loading the core. */
   last_run_usec     = core_time_usec();
   last_run_received = capture_frames_received();
   waiting_first_run = true;

   stats_lines[0][0] = '\0';
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
 * RetroArch pauses for a moment when the reported rate changes, so a new rate
 * must be found twice, RATE_CONFIRM_USEC apart, before it is reported.
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

   /* First frame, or the device stopped sending frames for a while (signal
    * lost): restart the measurement, keeping the rate reported so far. Frames
    * received while RetroArch was paused do not count as a gap: the interval
    * is divided by the number of frames the device sent meanwhile. */
   if (!rate_anchor_sequence || frame->sequence <= rate_last_sequence ||
       (frame->arrival_usec - rate_last_usec) / (int64_t)(frame->sequence - rate_last_sequence) > RATE_GAP_USEC)
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

   for (i = 0; i < 3; i++)
      if (fabs(measured / candidates[i] - 1.0) <= RATE_TOLERANCE)
         break;

   /* No match, or the rate already reported: nothing to change. */
   if (i == 3 || candidates[i] == rate_reported)
   {
      rate_candidate = 0.0;
      return rate_reported;
   }

   /* A new rate must be found again RATE_CONFIRM_USEC later, over a longer
    * measurement, before it is reported: each change pauses RetroArch. */
   if (candidates[i] != rate_candidate)
   {
      rate_candidate      = candidates[i];
      rate_candidate_usec = frame->arrival_usec;
      return rate_reported;
   }
   if (frame->arrival_usec - rate_candidate_usec < RATE_CONFIRM_USEC)
      return rate_reported;

   rate_reported  = candidates[i];
   rate_candidate = 0.0;
   log_cb(RETRO_LOG_INFO, LOG_PREFIX "Measured frame rate: %.3f fps (device declares %.3f).\n",
         measured, rate_nominal);
   stats_log_printf("Measured frame rate: %.3f fps (device declares %.3f), reporting %.3f fps to RetroArch.",
         measured, rate_nominal, rate_reported);
   return rate_reported;
}

/* Rounds `value` to the nearest multiple of `step`, at most `max`. */
static unsigned round_to_step(double value, unsigned step, unsigned max)
{
   const unsigned rounded = (unsigned)(value / step + 0.5) * step;
   return rounded > max ? max : rounded;
}

/*
 * Keeps the crop on the same part of the picture when the captured size
 * changes, for example from 1920x1080 to 1280x720 after changing Capture
 * Resolution: the crop values are pixels of the captured image, so they are
 * scaled by the same ratio and rounded to the options' steps (384 x 36 at
 * 1080p becomes 256 x 24 at 720p). The new values are written to the core
 * options, so the menu shows them. If the frontend cannot change option
 * values, the values to enter are written to the logs instead.
 */
static void convert_crop_to_size(unsigned width, unsigned height)
{
   const unsigned old_width = crop_size_width, old_height = crop_size_height;
   unsigned crop_h, crop_v;
   char value_h[16], value_v[16];
   struct retro_variable var;
   bool set = true;

   crop_size_width  = width;
   crop_size_height = height;

   if (!old_width || !old_height || (old_width == width && old_height == height) ||
       (!opt_crop_h && !opt_crop_v))
      return;

   crop_h = round_to_step((double)opt_crop_h * width / old_width, CROP_H_STEP, CROP_H_STEP * (CROP_VALUES - 1));
   crop_v = round_to_step((double)opt_crop_v * height / old_height, CROP_V_STEP, CROP_V_STEP * (CROP_VALUES - 1));
   if (crop_h == opt_crop_h && crop_v == opt_crop_v)
      return;

   snprintf(value_h, sizeof(value_h), "%u", crop_h);
   snprintf(value_v, sizeof(value_v), "%u", crop_v);

   var.key   = OPTION_CROP_H;
   var.value = value_h;
   set = environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &var);
   var.key   = OPTION_CROP_V;
   var.value = value_v;
   set = environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &var) && set;

   if (set)
   {
      opt_crop_h = crop_h;
      opt_crop_v = crop_v;
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "Captured size changed from %ux%u to %ux%u: crop set to %u x %u px.\n",
            old_width, old_height, width, height, crop_h, crop_v);
      stats_log_printf("Captured size changed from %ux%u to %ux%u: crop set to %u x %u px.",
            old_width, old_height, width, height, crop_h, crop_v);
   }
   else
   {
      log_cb(RETRO_LOG_WARN, LOG_PREFIX "Captured size changed from %ux%u to %ux%u: set Crop Horizontal to "
            "%u px and Crop Vertical to %u px to keep the same crop.\n",
            old_width, old_height, width, height, crop_h, crop_v);
      stats_log_printf("Captured size changed from %ux%u to %ux%u: set Crop Horizontal to %u px and "
            "Crop Vertical to %u px to keep the same crop.", old_width, old_height, width, height, crop_h, crop_v);
   }
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

/* Ends the current stats second early, before a pause or a device restart.
 * Frames dropped or repeated in that partial second are written to the stats
 * log, so the per-second lines add up to the session summary. `end` is the
 * time the partial second ends. */
static void restart_stats_second(retro_time_t end)
{
   if (stats_last_time && end > stats_last_time &&
       (stats_dropped || stats_repeated || stats_pacing_skips))
      stats_log_printf("Partial second (%.2f s): dropped %llu (pacing skips %llu), repeated %llu",
            (double)(end - stats_last_time) / 1000000.0, (unsigned long long)stats_dropped,
            (unsigned long long)stats_pacing_skips, (unsigned long long)stats_repeated);

   stats_dropped      = 0;
   stats_repeated     = 0;
   stats_pacing_skips = 0;
   stats_last_time    = 0;
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
   const retro_time_t now = core_time_usec();
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

   stats_capture_fps  = (double)(received - stats_last_received) / seconds;
   stats_frontend_fps = (double)stats_runs / seconds;
   stats_output_fps   = (double)stats_frames_shown / seconds;

   /* Stats log: one line per second. */
   stats_seconds++;
   stats_log_printf("t=%us capture %.2f RetroArch %.2f output %.2f fps | dropped %llu "
         "(pacing skips %llu), repeated %llu | frame age min %.1f avg %.1f max %.1f ms",
         stats_seconds, stats_capture_fps, stats_frontend_fps, stats_output_fps,
         (unsigned long long)stats_dropped,
         (unsigned long long)stats_pacing_skips,
         (unsigned long long)stats_repeated,
         frame_age_count ? frame_age_min_ms : 0.0,
         frame_age_count ? frame_age_sum_ms / frame_age_count : 0.0,
         frame_age_count ? frame_age_max_ms : 0.0);

   /* Start the next second. */
   stats_last_time     = now;
   stats_last_received = received;
   stats_frames_shown  = 0;
   stats_runs          = 0;
   stats_dropped       = 0;
   stats_pacing_skips  = 0;
   stats_repeated      = 0;
   frame_age_sum_ms    = 0.0;
   frame_age_count     = 0;

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

      /* Shown for 2 seconds and refreshed every second, so it stays up. */
      send_message(text, 2000, RETRO_MESSAGE_TYPE_STATUS);
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
   unsigned crop_x      = 0;
   unsigned crop_y      = 0;
   unsigned crop_width  = frame->width;
   unsigned crop_height = frame->height;
   unsigned dst_width, dst_height;
   uint32_t *out;
   size_t pitch;
   float aspect;

   /* Crop options, ignored if they would leave less than 16 pixels. All
    * values are even: YUV pixels come in pairs. */
   if (opt_crop_h && frame->width > opt_crop_h * 2 + 16)
   {
      crop_x     = opt_crop_h;
      crop_width = frame->width - opt_crop_h * 2;
   }
   if (opt_crop_v && frame->height > opt_crop_v * 2 + 16)
   {
      crop_y      = opt_crop_v;
      crop_height = frame->height - opt_crop_v * 2;
   }

   /* Keep the central 4:3 area when the remaining area is wider than 4:3. */
   if (opt_aspect == ASPECT_4_3_CROP && crop_width * 3 > crop_height * 4)
   {
      const unsigned width = ((crop_height * 4 + 1) / 3) & ~1u;

      crop_x    += ((crop_width - width) / 2) & ~1u;
      crop_width = width;
   }

   switch (opt_aspect)
   {
      case ASPECT_4_3_SQUASH:
      case ASPECT_4_3_CROP: aspect = 4.0f / 3.0f; break;
      case ASPECT_16_9:     aspect = 16.0f / 9.0f; break;
      default:              aspect = (float)crop_width / (float)crop_height; break;
   }

   if (opt_resolution == RESOLUTION_NATIVE)
   {
      dst_width  = crop_width;
      dst_height = crop_height;
   }
   else
   {
      dst_width  = opt_resolution == RESOLUTION_320X240 ? 320 : 640;
      dst_height = opt_resolution == RESOLUTION_320X240 ? 240 : 480;
   }

   if (frame->format == CAPTURE_FORMAT_XRGB8888 && opt_resolution == RESOLUTION_NATIVE)
   {
      /* No copy: point into the captured frame. The pitch skips the cropped
       * columns, so the frontend reads only the crop area. */
      out   = (uint32_t *)(frame->data + frame->stride * crop_y) + crop_x;
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
         if (!scale_area((const uint32_t *)frame->data, (unsigned)(frame->stride / 4), crop_x, crop_y,
               crop_width, crop_height, out, dst_width, dst_height))
            return; /* Cannot happen with CAPTURE_MAX_* sizes; keep the previous frame */
      }
      else
      {
         convert_set_colorimetry(frame_matrix(frame), opt_color_range);
         if (!convert_yuv_frame(frame, crop_x, crop_y, crop_width, crop_height, out, dst_width, dst_height))
            return;
      }
   }

   set_output_geometry(dst_width, dst_height, aspect);

   if (opt_stats == STATS_IMAGE)
      draw_stats_overlay(out, pitch, dst_width, dst_height);

   last_output       = out;
   last_output_pitch = pitch;
   showing_capture   = true;
   video_cb(out, dst_width, dst_height, pitch);
}

static void output_no_signal_screen(enum signal_state state)
{
   const uint32_t *screen = no_signal_render(state, opt_details, active_device_name);

   set_output_geometry(NO_SIGNAL_WIDTH, NO_SIGNAL_HEIGHT, 4.0f / 3.0f);
   last_output       = screen;
   last_output_pitch = NO_SIGNAL_WIDTH * sizeof(uint32_t);
   showing_capture   = false;
   video_cb(screen, NO_SIGNAL_WIDTH, NO_SIGNAL_HEIGHT, last_output_pitch);
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
 * alternates dropped and repeated frames for as long as the two stay close:
 * a few seconds if the rates differ slightly, a minute or more if they match.
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

      /* Two frames behind (for example after a stall), or one frame behind for
       * a while outside the collision zone: jump to the newest frame. */
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
      /* First frame since the device opened: the session starts here, so the
       * time the device took to send it is not counted. */
      if (!have_frame)
      {
         reset_session_stats();
         session_paused_frames = capture_frames_received() - 1;
      }

      convert_crop_to_size(frame.width, frame.height);
      last_frame            = frame;
      have_frame            = true;
      seconds_without_frame = 0.0;
      seconds_black         = convert_frame_is_black(&frame) ? seconds_black + frame_time : 0.0;
      stats_frames_shown++;
      session_frames_shown++;

      *new_frame = true;

      if (frame.arrival_usec)
      {
         const double age_ms = (double)(capture_time_usec() - frame.arrival_usec) / 1000.0;

         if (!frame_age_count || age_ms < frame_age_min_ms) frame_age_min_ms = age_ms;
         if (!frame_age_count || age_ms > frame_age_max_ms) frame_age_max_ms = age_ms;
         frame_age_sum_ms += age_ms;
         frame_age_count++;

         update_frame_rate(measure_frame_rate(&frame));
      }
      else
         update_frame_rate(frame.frame_rate);
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

   if (!have_frame && showing_capture && can_dupe && seconds_without_frame < REOPEN_HOLD)
      return SIGNAL_HOLD;
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

   if (state == SIGNAL_HOLD)
   {
      video_cb(NULL, output_width, output_height, last_output_pitch); /* Repeat the last image */
      return;
   }

   if (state != SIGNAL_OK)
   {
      output_no_signal_screen(state);
      return;
   }

   no_signal_invalidate(); /* Redraw the No Signal screen next time it appears */

   if (opt_stats != STATS_OFF)
      update_stats();

   /* Show the new settings right away, even without a new frame. */
   if (new_frame || options_changed || !showing_capture)
      output_capture_frame();
   else if (can_dupe)
      video_cb(NULL, output_width, output_height, last_output_pitch);
   else
      video_cb(last_output, output_width, output_height, last_output_pitch);
}

/* ------------------------------------------------------------------------ */
/* Pause detection                                                           */
/* ------------------------------------------------------------------------ */

/* Detects pauses (RetroArch's menu, shader setup) from the time since the
 * previous call. A pause restarts the current stats second, excludes the
 * pause from the session summary, and makes the core resume on the newest
 * frame. */
static void detect_pause(void)
{
   const retro_time_t now  = core_time_usec();
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
      restart_stats_second(last_run_usec);
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
   audio_set_logger(capture_logger);

   /* The core starts without content ("Start Core") or with a .vidcap file. */
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

   options_find_devices();
   options_declare();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)           { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)             { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_output_set_callback(cb); }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)                 { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)               { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
   no_signal_invalidate();
}

RETRO_API void retro_deinit(void)
{
   capture_stop();
   audio_output_close();
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
   audio_output_select(true, active_device_name);
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
      options_read();
      options_log(false);
      update_stats_log_file();
      apply_device_selection(false);
      audio_output_select(false, active_device_name);
      no_signal_invalidate();
   }

   present_frame(updated);

   audio_output_run(output_fps);
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

   options_read();
   update_stats_log_file();
   active_device_id[0] = '\0';
   apply_device_selection(true);
   audio_output_close();
   audio_output_select(true, active_device_name);
   crop_size_width  = 0;
   crop_size_height = 0;

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
   restart_stats_second(core_time_usec());
   log_session_stats();
   capture_stop();
   audio_output_close();
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
