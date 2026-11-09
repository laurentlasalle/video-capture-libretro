/*
 * Windows audio capture backend, using WASAPI in shared mode.
 *
 * A capture thread waits for the input's packets (every 10 ms or so), converts
 * them to 16-bit stereo and appends them to a ring buffer. The core reads from
 * the buffer in retro_run(). The input keeps its own sample rate; the core
 * resamples to its output rate.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef NOMINMAX
#define NOMINMAX /* windows.h would otherwise define min() and max() macros */
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"

/* PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), defined here so
 * that no GUID definitions need to be linked. */
static const PROPERTYKEY kFriendlyName =
   { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

/* Ring buffer capacity in stereo frames: about 1.4 s at 48 kHz. When the core
 * does not read (RetroArch paused), the oldest frames are overwritten. */
static const size_t RING_FRAMES = 65536;

static capture_log_t g_logger;

static std::mutex           g_ring_mutex;
static std::vector<int16_t> g_ring(RING_FRAMES * 2);
static size_t               g_ring_read;  /* Index of the oldest frame */
static size_t               g_ring_count; /* Frames stored */

static std::thread       g_thread;
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_running{false};
static std::atomic<unsigned> g_rate{48000};

template <class T> static void safe_release(T **p)
{
   if (*p)
   {
      (*p)->Release();
      *p = nullptr;
   }
}

static void log_message(enum capture_log_level level, const char *format, ...)
{
   char text[512];
   va_list args;

   if (!g_logger)
      return;
   va_start(args, format);
   vsnprintf(text, sizeof(text), format, args);
   va_end(args);
   g_logger(level, text);
}

static std::string to_utf8(const wchar_t *text)
{
   if (!text)
      return std::string();
   const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
   if (size <= 1)
      return std::string();
   std::string out((size_t)size - 1, '\0');
   WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
   return out;
}

static std::wstring to_wide(const char *text)
{
   const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
   if (size <= 1)
      return std::wstring();
   std::wstring out((size_t)size - 1, L'\0');
   MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], size);
   return out;
}

/* Initializes COM on the calling thread for the lifetime of the object, unless
 * it is already initialized in another mode (then it is used as is). */
struct com_scope
{
   bool initialized;
   com_scope() : initialized(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
   ~com_scope()
   {
      if (initialized)
         CoUninitialize();
   }
};

/* ------------------------------------------------------------------------ */
/* Ring buffer                                                               */
/* ------------------------------------------------------------------------ */

static void ring_clear(void)
{
   std::lock_guard<std::mutex> lock(g_ring_mutex);
   g_ring_read  = 0;
   g_ring_count = 0;
}

/* Copies `count` frames between the ring and a linear buffer, starting at ring
 * position `pos`, in at most two blocks (before and after the wrap). */
static void ring_copy_in(size_t pos, const int16_t *src, size_t count)
{
   const size_t first = std::min(count, RING_FRAMES - pos);
   memcpy(&g_ring[pos * 2], src, first * 2 * sizeof(int16_t));
   memcpy(&g_ring[0], src + first * 2, (count - first) * 2 * sizeof(int16_t));
}

static void ring_copy_out(size_t pos, int16_t *dst, size_t count)
{
   const size_t first = std::min(count, RING_FRAMES - pos);
   memcpy(dst, &g_ring[pos * 2], first * 2 * sizeof(int16_t));
   memcpy(dst + first * 2, &g_ring[0], (count - first) * 2 * sizeof(int16_t));
}

/* Appends frames; when the ring is full, the oldest frames are dropped. */
static void ring_write(const int16_t *frames, size_t count)
{
   std::lock_guard<std::mutex> lock(g_ring_mutex);

   if (count > RING_FRAMES)
   {
      frames += (count - RING_FRAMES) * 2;
      count   = RING_FRAMES;
   }
   if (g_ring_count + count > RING_FRAMES)
   {
      const size_t drop = g_ring_count + count - RING_FRAMES;
      g_ring_read   = (g_ring_read + drop) % RING_FRAMES;
      g_ring_count -= drop;
   }

   ring_copy_in((g_ring_read + g_ring_count) % RING_FRAMES, frames, count);
   g_ring_count += count;
}

/* ------------------------------------------------------------------------ */
/* Sample format conversion                                                  */
/* ------------------------------------------------------------------------ */

enum sample_type
{
   SAMPLE_PCM16,
   SAMPLE_PCM24, /* 3 bytes per sample */
   SAMPLE_PCM32, /* 32-bit containers, including 24 bits in 32 */
   SAMPLE_FLOAT32,
   SAMPLE_UNSUPPORTED
};

static enum sample_type sample_type_of(const WAVEFORMATEX *format)
{
   WORD tag = format->wFormatTag;

   if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22)
      /* KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT differ only in Data1. */
      tag = (WORD)((const WAVEFORMATEXTENSIBLE *)format)->SubFormat.Data1;

   if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
      return SAMPLE_FLOAT32;
   if (tag == WAVE_FORMAT_PCM)
   {
      switch (format->wBitsPerSample)
      {
         case 16: return SAMPLE_PCM16;
         case 24: return SAMPLE_PCM24;
         case 32: return SAMPLE_PCM32;
      }
   }
   return SAMPLE_UNSUPPORTED;
}

static int16_t read_sample(const BYTE *p, enum sample_type type)
{
   switch (type)
   {
      case SAMPLE_PCM16:
      {
         int16_t v;
         memcpy(&v, p, 2);
         return v;
      }
      case SAMPLE_PCM24:
         return (int16_t)(p[1] | (p[2] << 8));
      case SAMPLE_PCM32:
      {
         int32_t v;
         memcpy(&v, p, 4);
         return (int16_t)(v >> 16);
      }
      case SAMPLE_FLOAT32:
      {
         float v;
         memcpy(&v, p, 4);
         v *= 32767.0f;
         if (v > 32767.0f)  v = 32767.0f;
         if (v < -32768.0f) v = -32768.0f;
         return (int16_t)v;
      }
      default:
         return 0;
   }
}

/* Converts a packet to 16-bit stereo: the first two channels are kept, a mono
 * input is copied to both. */
static void convert_packet(const BYTE *data, UINT32 frames, const WAVEFORMATEX *format,
      enum sample_type type, bool silent, std::vector<int16_t> &out)
{
   const unsigned channels = format->nChannels;
   const unsigned bytes    = format->wBitsPerSample / 8;

   out.resize((size_t)frames * 2);
   if (silent || !data)
   {
      std::fill(out.begin(), out.end(), (int16_t)0);
      return;
   }

   for (UINT32 i = 0; i < frames; i++)
   {
      const BYTE *frame = data + (size_t)i * format->nBlockAlign;
      out[i * 2]     = read_sample(frame, type);
      out[i * 2 + 1] = channels > 1 ? read_sample(frame + bytes, type) : out[i * 2];
   }
}

/* ------------------------------------------------------------------------ */
/* Capture thread                                                            */
/* ------------------------------------------------------------------------ */

static void capture_thread(std::wstring id, std::promise<bool> started)
{
   com_scope com;
   IMMDeviceEnumerator *enumerator = nullptr;
   IMMDevice           *device     = nullptr;
   IAudioClient        *client     = nullptr;
   IAudioCaptureClient *capture    = nullptr;
   WAVEFORMATEX        *format     = nullptr;
   HANDLE               event      = nullptr;
   bool                 reported   = false;
   enum sample_type     type       = SAMPLE_UNSUPPORTED;
   std::vector<int16_t> converted;
   HRESULT hr;

   hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
   if (SUCCEEDED(hr))
      hr = enumerator->GetDevice(id.c_str(), &device);
   if (SUCCEEDED(hr))
      hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
   if (SUCCEEDED(hr))
      hr = client->GetMixFormat(&format);
   if (SUCCEEDED(hr))
   {
      type = sample_type_of(format);
      if (type == SAMPLE_UNSUPPORTED)
         hr = E_FAIL;
   }
   if (SUCCEEDED(hr))
      /* 100 ms buffer, in 100 ns units. */
      hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, format, nullptr);
   if (SUCCEEDED(hr))
   {
      event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      hr = event ? client->SetEventHandle(event) : E_FAIL;
   }
   if (SUCCEEDED(hr))
      hr = client->GetService(IID_PPV_ARGS(&capture));
   if (SUCCEEDED(hr))
      hr = client->Start();

   if (FAILED(hr))
   {
      log_message(CAPTURE_LOG_ERROR, "Could not open the audio input (0x%08lX)%s.", (unsigned long)hr,
            format && type == SAMPLE_UNSUPPORTED ? ": unsupported sample format" : "");
   }
   else
   {
      g_rate    = format->nSamplesPerSec;
      g_running = true;
      log_message(CAPTURE_LOG_INFO, "Audio input format: %lu Hz, %u channel(s), %u-bit %s.",
            (unsigned long)format->nSamplesPerSec, (unsigned)format->nChannels,
            (unsigned)format->wBitsPerSample, type == SAMPLE_FLOAT32 ? "float" : "integer");
   }
   started.set_value(SUCCEEDED(hr));
   reported = true;

   while (SUCCEEDED(hr) && !g_stop)
   {
      UINT32 packet = 0;

      WaitForSingleObject(event, 200);

      while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0)
      {
         BYTE  *data   = nullptr;
         UINT32 frames = 0;
         DWORD  flags  = 0;

         hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
         if (FAILED(hr))
            break;
         convert_packet(data, frames, format, type, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, converted);
         capture->ReleaseBuffer(frames);
         ring_write(converted.data(), frames);
      }
   }

   if (reported && FAILED(hr) && !g_stop)
      log_message(CAPTURE_LOG_WARN, "The audio input stopped (0x%08lX).", (unsigned long)hr);

   g_running = false;
   if (client)
      client->Stop();
   if (event)
      CloseHandle(event);
   if (format)
      CoTaskMemFree(format);
   safe_release(&capture);
   safe_release(&client);
   safe_release(&device);
   safe_release(&enumerator);
}

/* ------------------------------------------------------------------------ */
/* Public interface                                                          */
/* ------------------------------------------------------------------------ */

extern "C" void audio_set_logger(capture_log_t logger)
{
   g_logger = logger;
}

extern "C" int audio_list_devices(audio_device_t *out, int max)
{
   com_scope com;
   IMMDeviceEnumerator *enumerator = nullptr;
   IMMDeviceCollection *collection = nullptr;
   UINT count = 0;
   int found  = 0;

   if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
      return 0;

   if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) &&
       SUCCEEDED(collection->GetCount(&count)))
   {
      for (UINT i = 0; i < count && found < max; i++)
      {
         IMMDevice      *device = nullptr;
         IPropertyStore *props  = nullptr;
         LPWSTR          id     = nullptr;
         PROPVARIANT     name;

         PropVariantInit(&name);
         if (SUCCEEDED(collection->Item(i, &device)) && SUCCEEDED(device->GetId(&id)) &&
             SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
             SUCCEEDED(props->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR)
         {
            snprintf(out[found].name, sizeof(out[found].name), "%s", to_utf8(name.pwszVal).c_str());
            snprintf(out[found].id, sizeof(out[found].id), "%s", to_utf8(id).c_str());
            found++;
         }
         PropVariantClear(&name);
         if (id)
            CoTaskMemFree(id);
         safe_release(&props);
         safe_release(&device);
      }
   }

   safe_release(&collection);
   safe_release(&enumerator);
   return found;
}

extern "C" bool audio_start(const char *id)
{
   std::promise<bool> started;
   std::future<bool>  result = started.get_future();

   audio_stop();
   g_stop = false;
   g_thread = std::thread(capture_thread, to_wide(id), std::move(started));

   if (!result.get())
   {
      g_thread.join();
      return false;
   }
   return true;
}

extern "C" void audio_stop(void)
{
   g_stop = true;
   if (g_thread.joinable())
      g_thread.join();
   g_running = false;
   ring_clear();
}

extern "C" bool audio_is_running(void)
{
   return g_running;
}

extern "C" unsigned audio_sample_rate(void)
{
   return g_rate;
}

extern "C" size_t audio_available(void)
{
   std::lock_guard<std::mutex> lock(g_ring_mutex);
   return g_ring_count;
}

extern "C" size_t audio_read(int16_t *dst, size_t frames)
{
   std::lock_guard<std::mutex> lock(g_ring_mutex);
   const size_t count = frames < g_ring_count ? frames : g_ring_count;

   ring_copy_out(g_ring_read, dst, count);
   g_ring_read   = (g_ring_read + count) % RING_FRAMES;
   g_ring_count -= count;
   return count;
}

extern "C" void audio_discard(size_t frames)
{
   std::lock_guard<std::mutex> lock(g_ring_mutex);
   const size_t count = frames < g_ring_count ? frames : g_ring_count;

   g_ring_read   = (g_ring_read + count) % RING_FRAMES;
   g_ring_count -= count;
}
