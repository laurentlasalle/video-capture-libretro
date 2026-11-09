/*
 * Audio sent to the frontend: the selected audio input (see audio.h), or
 * silence when there is none.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "audio_output.h"
#include "core.h"
#include "options.h"

/* Longest output per video frame: one tenth of a second, for frame rates
 * down to 10 fps. */
#define AUDIO_FRAMES_MAX    4800
/* Buffered input kept ahead of the output: about 33 ms (2 frames) at 48 kHz.
 * WASAPI delivers packets every 10 ms or so, and RetroArch's calls are not
 * perfectly regular either; this leaves room for both. */
#define AUDIO_TARGET_FRAMES 1600
/* Largest resampling correction: +/- 0.5%, inaudible. */
#define AUDIO_MAX_ADJUST    0.005

static retro_audio_sample_batch_t batch_cb;

static char     active_audio_id[sizeof(((audio_device_t *)0)->id)]; /* Empty when no input is selected */
static bool     start_failed;           /* The selected input could not be opened; already reported */
static double   retry_timer;            /* Seconds until the next attempt to reopen a lost input */
static bool     buffering = true;       /* Waiting for AUDIO_TARGET_FRAMES before playing */
static double   frames_pending;         /* Fractional output frames carried between video frames */
static double   in_pending;             /* Fractional input frames carried between video frames */
static int16_t  silence[AUDIO_FRAMES_MAX * 2];
static int16_t  audio_in[AUDIO_FRAMES_MAX * 4 * 2]; /* Room for input rates up to 192 kHz */
static int16_t  audio_out[AUDIO_FRAMES_MAX * 2];
static uint64_t session_underruns;      /* The buffer ran dry */
static uint64_t session_resyncs;        /* Excess input dropped, usually after a pause */
static char     last_capture_name[sizeof(option_device_names[0])]; /* For Auto when retrying */

void audio_output_set_callback(retro_audio_sample_batch_t cb)
{
   batch_cb = cb;
}

/* ------------------------------------------------------------------------ */
/* Input selection                                                           */
/* ------------------------------------------------------------------------ */

/* Copies the letters and digits of `text` to `out`, in lowercase: "Cam Link
 * Pro HDMI-4" becomes "camlinkprohdmi4". */
static void simplify_name(const char *text, char *out, size_t size)
{
   size_t n = 0;

   for (; *text && n + 1 < size; text++)
      if (isalnum((unsigned char)*text))
         out[n++] = (char)tolower((unsigned char)*text);
   out[n] = '\0';
}

/* True if `text` contains the number `number` on its own: "Audio-4" contains
 * 4, "Audio-14" does not. */
static bool contains_number(const char *text, unsigned long number)
{
   while (*text)
   {
      if (isdigit((unsigned char)*text))
      {
         char *end;
         const unsigned long value = strtoul(text, &end, 10);

         if (value == number)
            return true;
         text = end;
      }
      else
         text++;
   }
   return false;
}

/*
 * Returns the index of the audio input that goes with the capture device
 * `video`, or -1. Device makers name them differently, for example:
 *   "Elgato 4K X"         -> "Digital Audio Interface (Elgato 4K X)"
 *   "Cam Link Pro HDMI-4" -> "Cam Link Pro Audio-4 (Cam Link Pro)"
 * First, an input whose name contains the device's name (letters and digits
 * only, ignoring case). Otherwise, if the device's name contains a number (the
 * input number of a multi-input device), an input whose name contains both
 * the text before the word holding that number ("Cam Link Pro") and the same
 * number.
 */
static int match_audio_input(const char *video)
{
   char video_simple[300], audio_simple[300], base[300], base_simple[300];
   const char *number_start = NULL, *word_start, *p;
   unsigned long number;
   size_t base_len;
   int i;

   simplify_name(video, video_simple, sizeof(video_simple));
   if (!video_simple[0])
      return -1;

   for (i = 0; i < audio_device_count; i++)
   {
      simplify_name(audio_devices[i].name, audio_simple, sizeof(audio_simple));
      if (strstr(audio_simple, video_simple))
         return i;
   }

   /* Last number of the device's name, and the word holding it. */
   for (p = video; *p; p++)
      if (isdigit((unsigned char)*p) && (p == video || !isdigit((unsigned char)p[-1])))
         number_start = p;
   if (!number_start)
      return -1;
   number = strtoul(number_start, NULL, 10);

   word_start = number_start;
   while (word_start > video && word_start[-1] != ' ')
      word_start--;
   base_len = (size_t)(word_start - video);
   if (base_len == 0 || base_len >= sizeof(base))
      return -1;
   memcpy(base, video, base_len);
   base[base_len] = '\0';
   simplify_name(base, base_simple, sizeof(base_simple));
   if (!base_simple[0])
      return -1;

   for (i = 0; i < audio_device_count; i++)
   {
      simplify_name(audio_devices[i].name, audio_simple, sizeof(audio_simple));
      if (strstr(audio_simple, base_simple) && contains_number(audio_devices[i].name, number))
         return i;
   }
   return -1;
}

/* See audio_output.h. Auto uses match_audio_input() and shows its choice in a
 * notification. */
void audio_output_select(bool force, const char *capture_name)
{
   const char *value = options_get(OPTION_AUDIO);
   const bool auto_select = option_is(value, "auto");
   int index = -1, i;

   if (capture_name != last_capture_name)
      snprintf(last_capture_name, sizeof(last_capture_name), "%s", capture_name);

   if (auto_select)
      index = match_audio_input(capture_name);
   else if (value && !option_is(value, "off"))
   {
      for (i = 0; i < audio_device_count && index < 0; i++)
         if (strcmp(option_audio_names[i], value) == 0)
            index = i;
   }

   if (!force && strcmp(index >= 0 ? audio_devices[index].id : "", active_audio_id) == 0)
      return;

   if (strcmp(index >= 0 ? audio_devices[index].id : "", active_audio_id) != 0)
      start_failed = false; /* Another input: report its failure, if any */

   audio_stop();
   snprintf(active_audio_id, sizeof(active_audio_id), "%s", index >= 0 ? audio_devices[index].id : "");
   buffering   = true;
   in_pending  = 0.0;
   retry_timer = RETRY_INTERVAL;

   if (index < 0)
   {
      if (auto_select && capture_name[0])
      {
         log_cb(RETRO_LOG_WARN, LOG_PREFIX "No audio input matches \"%s\"; select one in Audio Input.\n",
               capture_name);
         stats_log_printf("Audio: no input matches \"%s\".", capture_name);
         show_notification("Audio Input: no input matches the capture device, select one manually.", 5000);
      }
      return;
   }

   if (auto_select)
   {
      char text[320];

      snprintf(text, sizeof(text), "Audio Input: %s", option_audio_names[index]);
      show_notification(text, 4000);
   }

   if (audio_start(audio_devices[index].id))
   {
      log_cb(RETRO_LOG_INFO, LOG_PREFIX "Audio started: %s (%u Hz).\n", option_audio_names[index],
            audio_sample_rate());
      stats_log_printf("Audio started: %s (%u Hz).", option_audio_names[index], audio_sample_rate());
      start_failed = false;
   }
   else if (!start_failed)
   {
      /* Logged once: retries every RETRY_INTERVAL would repeat it. */
      log_cb(RETRO_LOG_ERROR, LOG_PREFIX "Could not start audio: %s. Retrying every few seconds.\n",
            option_audio_names[index]);
      stats_log_printf("Could not start audio: %s.", option_audio_names[index]);
      start_failed = true;
   }
}

/* ------------------------------------------------------------------------ */
/* Resampling                                                                */
/* ------------------------------------------------------------------------ */

/* Linear interpolation of `in_frames` stereo frames into `out_frames`. */
static void resample_linear(const int16_t *in, size_t in_frames, int16_t *out, size_t out_frames)
{
   size_t i;

   for (i = 0; i < out_frames; i++)
   {
      const double pos  = (double)i * (double)in_frames / (double)out_frames;
      const size_t a    = (size_t)pos;
      const size_t b    = a + 1 < in_frames ? a + 1 : a;
      const double frac = pos - (double)a;

      out[i * 2]     = (int16_t)(in[a * 2] + (in[b * 2] - in[a * 2]) * frac);
      out[i * 2 + 1] = (int16_t)(in[a * 2 + 1] + (in[b * 2 + 1] - in[a * 2 + 1]) * frac);
   }
}

/*
 * Sends `frames` stereo frames at SAMPLE_RATE to the frontend: captured audio
 * if an input is open, silence otherwise.
 *
 * The input runs on the capture device's clock and RetroArch on the computer's,
 * so the amount of buffered input slowly drifts. The core keeps about
 * AUDIO_TARGET_FRAMES buffered: it reads slightly more or less input than it
 * outputs (up to AUDIO_MAX_ADJUST), and resamples it to the output length.
 * After a pause, when the buffer holds far too much, the excess is dropped;
 * when it runs dry, the core waits for the target again, outputting silence.
 */
static void output_audio(size_t frames)
{
   const double base   = (double)audio_sample_rate() / SAMPLE_RATE;
   const double target = AUDIO_TARGET_FRAMES * base;
   size_t level, in_frames, got;
   double adjust;

   if (!active_audio_id[0] || !audio_is_running())
   {
      buffering = true;
      batch_cb(silence, frames);
      return;
   }

   level = audio_available();

   if (buffering)
   {
      if ((double)level < target)
      {
         batch_cb(silence, frames);
         return;
      }
      buffering = false;
   }

   /* Far too much buffered (after a pause): drop the excess. */
   if ((double)level > target * 3.0 + frames * base)
   {
      audio_discard(level - (size_t)target);
      level = (size_t)target;
      session_resyncs++;
   }

   adjust = AUDIO_MAX_ADJUST * ((double)level - target) / target;
   if (adjust > AUDIO_MAX_ADJUST)  adjust = AUDIO_MAX_ADJUST;
   if (adjust < -AUDIO_MAX_ADJUST) adjust = -AUDIO_MAX_ADJUST;

   in_pending += (double)frames * base * (1.0 + adjust);
   in_frames = (size_t)in_pending;
   if (in_frames > sizeof(audio_in) / sizeof(audio_in[0]) / 2)
      in_frames = sizeof(audio_in) / sizeof(audio_in[0]) / 2;
   if (in_frames == 0)
      in_frames = 1;
   in_pending -= (double)in_frames;

   got = audio_read(audio_in, in_frames);
   if (got < in_frames)
   {
      /* Ran dry: pad with silence and wait for the target again. */
      memset(audio_in + got * 2, 0, (in_frames - got) * 2 * sizeof(int16_t));
      buffering = true;
      session_underruns++;
   }

   resample_linear(audio_in, in_frames, audio_out, frames);
   batch_cb(audio_out, frames);
}


/* ------------------------------------------------------------------------ */
/* Per-frame output                                                          */
/* ------------------------------------------------------------------------ */

void audio_output_run(double fps)
{
   size_t frames;

   if (!batch_cb)
      return;

   /* Audio input lost (device unplugged): retry periodically. */
   if (active_audio_id[0] && !audio_is_running())
   {
      retry_timer -= 1.0 / fps;
      if (retry_timer <= 0.0)
         audio_output_select(true, last_capture_name);
   }

   /* Output length matching the frame rate: 800.8 frames per video frame at
    * 59.94 fps, so the fraction is carried over to the next call. */
   frames_pending += SAMPLE_RATE / fps;
   frames = (size_t)frames_pending;
   if (frames > AUDIO_FRAMES_MAX)
      frames = AUDIO_FRAMES_MAX;
   frames_pending -= (double)frames;
   if (frames_pending > AUDIO_FRAMES_MAX)
      frames_pending = 0.0;

   output_audio(frames);
}

void audio_output_close(void)
{
   audio_stop();
   active_audio_id[0] = '\0';
   start_failed       = false;
}

void audio_output_reset_session(void)
{
   session_underruns = 0;
   session_resyncs   = 0;
}

void audio_output_log_session(void)
{
   if (active_audio_id[0])
      stats_log_printf("Audio: %llu underrun(s), %llu resync(s).",
            (unsigned long long)session_underruns, (unsigned long long)session_resyncs);
}
