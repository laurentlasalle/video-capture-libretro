/*
 * Media Foundation capture backend (Windows).
 *
 * Opens a video capture device through an asynchronous IMFSourceReader and
 * picks one of its native formats explicitly. Frames are then delivered either
 * as is (NV12 or YUY2, converted to RGB by the core after downscaling) or
 * converted to RGB32 by the source reader's video processor.
 *
 * Each received frame is copied once, into a triple buffer; the core picks up
 * the most recent one in retro_run() by swapping pointers.
 *
 * Copyright (c) 2026 Laurent LaSalle
 * SPDX-License-Identifier: MIT
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "capture.h"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static capture_log_t g_logger = nullptr;

static void log_message(enum capture_log_level level, const char *fmt, ...)
{
   char buffer[1024];
   va_list va;

   if (!g_logger)
      return;

   va_start(va, fmt);
   vsnprintf(buffer, sizeof(buffer), fmt, va);
   va_end(va);
   g_logger(level, buffer);
}

/* Last failure, shown on the No Signal screen (guarded by g_error_mutex). */
static std::mutex  g_error_mutex;
static std::string g_last_error;

static void set_last_error(const char *fmt, ...)
{
   char buffer[512];
   va_list va;

   va_start(va, fmt);
   vsnprintf(buffer, sizeof(buffer), fmt, va);
   va_end(va);

   std::lock_guard<std::mutex> lock(g_error_mutex);
   g_last_error = buffer;
}

template <class T>
static void safe_release(T **pp)
{
   if (*pp)
   {
      (*pp)->Release();
      *pp = nullptr;
   }
}

static std::string wide_to_utf8(const wchar_t *wide)
{
   if (!wide)
      return std::string();

   int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
   if (size <= 1)
      return std::string();

   std::string out((size_t)size - 1, '\0');
   WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], size, nullptr, nullptr);
   return out;
}

static std::wstring utf8_to_wide(const char *utf8)
{
   if (!utf8)
      return std::wstring();

   int size = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
   if (size <= 1)
      return std::wstring();

   std::wstring out((size_t)size - 1, L'\0');
   MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], size);
   return out;
}

static void copy_string(char *dst, size_t dst_size, const std::string &src)
{
   size_t len = src.size() < dst_size - 1 ? src.size() : dst_size - 1;
   memcpy(dst, src.data(), len);
   dst[len] = '\0';
}

static std::string subtype_name(const GUID &subtype)
{
   if (subtype == MFVideoFormat_NV12)   return "NV12";
   if (subtype == MFVideoFormat_YUY2)   return "YUY2";
   if (subtype == MFVideoFormat_UYVY)   return "UYVY";
   if (subtype == MFVideoFormat_RGB32)  return "RGB32";
   if (subtype == MFVideoFormat_ARGB32) return "ARGB32";
   if (subtype == MFVideoFormat_RGB24)  return "RGB24";
   if (subtype == MFVideoFormat_MJPG)   return "MJPG";
   if (subtype == MFVideoFormat_P010)   return "P010";

   /* Video subtypes are FourCC-based: the code is stored in Data1. */
   char fourcc[5];
   fourcc[0] = (char)(subtype.Data1 & 0xFF);
   fourcc[1] = (char)((subtype.Data1 >> 8) & 0xFF);
   fourcc[2] = (char)((subtype.Data1 >> 16) & 0xFF);
   fourcc[3] = (char)((subtype.Data1 >> 24) & 0xFF);
   fourcc[4] = '\0';
   return fourcc;
}

/* Formats the core can convert itself. */
static bool is_raw_yuv(const GUID &subtype)
{
   return subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2;
}

/* Explains common HRESULTs from opening or streaming a device. */
static void describe_device_error(const char *action, HRESULT hr)
{
   bool in_use = hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
#ifdef MF_E_VIDEO_RECORDING_DEVICE_LOCKED
   in_use = in_use || hr == MF_E_VIDEO_RECORDING_DEVICE_LOCKED;
#endif
#ifdef MF_E_HW_MFT_FAILED_START_STREAMING
   in_use = in_use || hr == MF_E_HW_MFT_FAILED_START_STREAMING;
#endif
#ifdef MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
   in_use = in_use || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED;
#endif

   if (hr == E_ACCESSDENIED)
      set_last_error("Windows denied access to the device. Check Settings > Privacy & security > "
            "Camera > Let desktop apps access your camera.");
   else if (in_use)
      set_last_error("The device is in use by another application, such as OBS. "
            "Close it or deactivate its source there.");
   else if (hr == MF_E_SHUTDOWN || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) ||
            hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
      set_last_error("The device was disconnected.");
   else
      set_last_error("%s failed (error 0x%08lX).", action, (unsigned long)hr);

   log_message(CAPTURE_LOG_ERROR, "%s failed (0x%08lX).", action, (unsigned long)hr);
}

/* COM and Media Foundation must be initialized on the calling thread.
 * Both are reference-counted, so nesting is fine. */
struct mf_scope
{
   bool com_initialized = false;
   bool mf_started      = false;

   bool init()
   {
      HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      /* RPC_E_CHANGED_MODE: the thread is already in an STA. Media Foundation
       * objects are free-threaded, so we can still use them. */
      com_initialized = SUCCEEDED(hr);
      if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
      {
         log_message(CAPTURE_LOG_ERROR, "CoInitializeEx failed (0x%08lX).", (unsigned long)hr);
         return false;
      }

      hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
      if (FAILED(hr))
      {
         log_message(CAPTURE_LOG_ERROR, "MFStartup failed (0x%08lX).", (unsigned long)hr);
         shutdown();
         return false;
      }
      mf_started = true;
      return true;
   }

   void shutdown()
   {
      if (mf_started)
         MFShutdown();
      if (com_initialized)
         CoUninitialize();
      mf_started      = false;
      com_initialized = false;
   }
};

/* ------------------------------------------------------------------------ */
/* Shared frame state (ring of recent frames)                                */
/* ------------------------------------------------------------------------ */

/*
 * A small ring of slots holds the most recent frames, numbered in arrival
 * order (sequence 1, 2, 3...). At any time one slot may be in use by each
 * side, and neither is ever overwritten while in use:
 *   - the writing slot, filled by the Media Foundation callback thread. It is
 *     always the slot with the oldest frame, which is dropped;
 *   - the reading slot, owned by the core until its next acquire.
 * The other slots hold the latest complete frames. The core chooses which one
 * to show (see the Frame Pacing option), so no frame is copied twice.
 *
 * Slots store images tightly packed (stride = width * bytes per pixel), with
 * the NV12 chroma plane directly after the luma plane.
 */
#define RING_SLOTS 5

struct frame_slot
{
   std::vector<uint8_t> data;
   size_t               stride = 0;
   enum capture_format  format = CAPTURE_FORMAT_XRGB8888;
   unsigned             width  = 0;
   unsigned             height = 0;
   int64_t              arrival_usec = 0;
   uint64_t             sequence = 0; /* 0: empty or being written */
};

static frame_slot g_ring[RING_SLOTS];
static int        g_writing = -1;     /* Slot being written, or -1 */
static int        g_reading = -1;     /* Slot owned by the core, or -1 */
static uint64_t   g_next_sequence = 1;
static std::mutex g_slot_mutex;       /* Guards the sequences and slot indexes */

/* Picks the slot to write: an empty one, or the one with the oldest frame,
 * never the slot the core is reading. Its frame is discarded. */
static frame_slot *begin_write(void)
{
   std::lock_guard<std::mutex> lock(g_slot_mutex);
   int best = -1;

   for (int i = 0; i < RING_SLOTS; i++)
      if (i != g_reading && (best < 0 || g_ring[i].sequence < g_ring[best].sequence))
         best = i;

   g_ring[best].sequence = 0;
   g_writing = best;
   return &g_ring[best];
}

/* Publishes the slot being written as the newest frame. */
static void end_write(void)
{
   std::lock_guard<std::mutex> lock(g_slot_mutex);
   g_ring[g_writing].sequence = g_next_sequence++;
   g_writing = -1;
}

/* Current output format of the source reader. Written by update_output_format()
 * and read by the callback thread; guarded by g_format_mutex. */
static std::mutex          g_format_mutex;
static enum capture_format g_out_format;
static UINT32              g_out_width;
static UINT32              g_out_height;
static LONG                g_out_stride; /* Default stride in bytes, negative if bottom-up */
static double              g_out_frame_rate;
static bool                g_warned_oversize;

static std::atomic<bool>     g_running(false);
static std::atomic<uint64_t> g_frames_received(0);

static void update_output_format(IMFSourceReader *reader)
{
   IMFMediaType *type = nullptr;
   GUID subtype = GUID_NULL;
   UINT32 width = 0, height = 0, rate_num = 0, rate_den = 0;
   enum capture_format format;
   LONG stride;
   double frame_rate;

   if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)))
      return;

   type->GetGUID(MF_MT_SUBTYPE, &subtype);
   MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
   MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rate_num, &rate_den);

   if (subtype == MFVideoFormat_NV12)
      format = CAPTURE_FORMAT_NV12;
   else if (subtype == MFVideoFormat_YUY2)
      format = CAPTURE_FORMAT_YUY2;
   else
      format = CAPTURE_FORMAT_XRGB8888;

   {
      const UINT32 bytes_per_pixel = format == CAPTURE_FORMAT_XRGB8888 ? 4 : (format == CAPTURE_FORMAT_YUY2 ? 2 : 1);
      stride = (LONG)MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, width * bytes_per_pixel);
   }
   frame_rate = rate_den ? (double)rate_num / rate_den : 0.0;
   safe_release(&type);

   {
      std::lock_guard<std::mutex> lock(g_format_mutex);
      g_out_format      = format;
      g_out_width       = width;
      g_out_height      = height;
      g_out_stride      = stride;
      g_out_frame_rate  = frame_rate;
      g_warned_oversize = false;
   }

   log_message(CAPTURE_LOG_INFO, "Output format: %ux%u %s @ %.3f fps (%u/%u), stride %ld.",
         (unsigned)width, (unsigned)height, subtype_name(subtype).c_str(), frame_rate,
         (unsigned)rate_num, (unsigned)rate_den, (long)stride);
}

/* Copies `rows` rows of `row_bytes` bytes. `pitch` may be negative for
 * bottom-up images; src always points to the top row. */
static void copy_plane(uint8_t *dst, const BYTE *src, LONG pitch, size_t row_bytes, unsigned rows)
{
   if (pitch == (LONG)row_bytes)
      memcpy(dst, src, row_bytes * rows);
   else
      for (unsigned y = 0; y < rows; y++)
         memcpy(dst + y * row_bytes, src + (ptrdiff_t)y * pitch, row_bytes);
}

/* Copies a locked image into the write slot, then publishes it.
 * `buffer_len` is the size of the locked buffer, used to find the NV12 chroma
 * plane when the driver pads the luma plane. */
/* Microseconds from the high-resolution performance counter. */
static int64_t qpc_usec(void)
{
   static LARGE_INTEGER frequency;
   LARGE_INTEGER now;

   if (!frequency.QuadPart)
      QueryPerformanceFrequency(&frequency);
   QueryPerformanceCounter(&now);
   return (int64_t)(now.QuadPart / frequency.QuadPart * 1000000 +
         now.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}

static void copy_to_write_slot(const BYTE *scan0, LONG pitch, size_t buffer_len,
      enum capture_format format, unsigned width, unsigned height, int64_t arrival_usec)
{
   frame_slot *slot = begin_write(); /* Only this thread writes to the slot */
   const size_t bytes_per_pixel = format == CAPTURE_FORMAT_XRGB8888 ? 4 : (format == CAPTURE_FORMAT_YUY2 ? 2 : 1);
   const size_t row_bytes = (size_t)width * bytes_per_pixel;
   const size_t luma_size = row_bytes * height;

   if (format == CAPTURE_FORMAT_NV12)
   {
      const size_t abs_pitch = (size_t)(pitch < 0 ? -pitch : pitch);
      const unsigned chroma_rows = (height + 1) / 2;
      size_t chroma_offset = abs_pitch * height;

      /* Some drivers pad the luma plane to a multiple of 16 rows. The buffer
       * then holds 1.5 padded planes: the chroma plane starts at 2/3. */
      if (pitch > 0 && buffer_len > abs_pitch * (height + chroma_rows))
         chroma_offset = (buffer_len / (3 * abs_pitch)) * 2 * abs_pitch;

      slot->data.resize(luma_size + row_bytes * chroma_rows); /* Allocates only when the size grows */
      copy_plane(slot->data.data(), scan0, pitch, row_bytes, height);
      copy_plane(slot->data.data() + luma_size, scan0 + chroma_offset, pitch, row_bytes, chroma_rows);
   }
   else
   {
      slot->data.resize(luma_size);
      copy_plane(slot->data.data(), scan0, pitch, row_bytes, height);
   }

   slot->stride       = row_bytes;
   slot->arrival_usec = arrival_usec;
   slot->format       = format;
   slot->width        = width;
   slot->height       = height;

   end_write();
}

static void store_sample(IMFSample *sample, int64_t arrival_usec)
{
   IMFMediaBuffer *buffer = nullptr;
   IMF2DBuffer *buffer2d  = nullptr;
   enum capture_format format;
   unsigned width, height;
   LONG default_stride;
   BYTE *data      = nullptr;
   DWORD max_len   = 0;
   DWORD cur_len   = 0;

   {
      std::lock_guard<std::mutex> lock(g_format_mutex);
      format         = g_out_format;
      width          = g_out_width;
      height         = g_out_height;
      default_stride = g_out_stride;

      if (width == 0 || height == 0)
         return;

      if (width > CAPTURE_MAX_WIDTH || height > CAPTURE_MAX_HEIGHT)
      {
         if (!g_warned_oversize)
            log_message(CAPTURE_LOG_WARN, "Frame %ux%u exceeds the %ux%u limit, ignoring.",
                  width, height, CAPTURE_MAX_WIDTH, CAPTURE_MAX_HEIGHT);
         g_warned_oversize = true;
         return;
      }
   }

   g_frames_received++;

   if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
      return;

   {
      const size_t bytes_per_pixel = format == CAPTURE_FORMAT_XRGB8888 ? 4 : (format == CAPTURE_FORMAT_YUY2 ? 2 : 1);
      const unsigned rows = format == CAPTURE_FORMAT_NV12 ? height + height / 2 : height;

      /* Prefer IMF2DBuffer: it gives the real pitch and first-row pointer,
       * including for bottom-up images (negative pitch). Otherwise fall back
       * to a plain lock and the media type's default stride. A buffer must
       * not be locked both ways at once. */
      if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))))
      {
         BYTE *scan0 = nullptr;
         LONG pitch  = 0;
         DWORD len   = 0;

         buffer2d->GetContiguousLength(&len);
         if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch)))
         {
            const size_t abs_pitch = (size_t)(pitch < 0 ? -pitch : pitch);
            if (abs_pitch >= width * bytes_per_pixel)
               copy_to_write_slot(scan0, pitch, len ? len : abs_pitch * rows, format, width, height, arrival_usec);
            buffer2d->Unlock2D();
         }
         safe_release(&buffer2d);
      }
      else if (SUCCEEDED(buffer->Lock(&data, &max_len, &cur_len)))
      {
         const LONG pitch       = default_stride ? default_stride : (LONG)(width * bytes_per_pixel);
         const size_t abs_pitch = (size_t)(pitch < 0 ? -pitch : pitch);
         const BYTE *scan0      = pitch < 0 ? data + abs_pitch * (height - 1) : data;

         if (abs_pitch >= width * bytes_per_pixel && (size_t)cur_len >= abs_pitch * rows)
            copy_to_write_slot(scan0, pitch, cur_len, format, width, height, arrival_usec);
         buffer->Unlock();
      }
   }

   safe_release(&buffer);
}

/* ------------------------------------------------------------------------ */
/* Source reader callback                                                    */
/* ------------------------------------------------------------------------ */

class reader_callback : public IMFSourceReaderCallback
{
public:
   reader_callback()
      : ref_count_(1), reader_(nullptr), stopping_(false)
   {
      flush_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
   }

   virtual ~reader_callback()
   {
      if (flush_event_)
         CloseHandle(flush_event_);
   }

   /* Non-owning: the reader holds a reference to us, not the reverse. */
   void set_reader(IMFSourceReader *reader)
   {
      std::lock_guard<std::mutex> lock(request_mutex_);
      reader_ = reader;
   }

   /* After this returns, the callback never requests another frame, so the
    * reader can be flushed and released safely. */
   void set_stopping()
   {
      std::lock_guard<std::mutex> lock(request_mutex_);
      stopping_ = true;
      reader_   = nullptr;
   }

   HANDLE flush_event() const { return flush_event_; }

   STDMETHODIMP QueryInterface(REFIID iid, void **ppv) override
   {
      if (!ppv)
         return E_POINTER;

      if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback))
      {
         *ppv = static_cast<IMFSourceReaderCallback *>(this);
         AddRef();
         return S_OK;
      }

      *ppv = nullptr;
      return E_NOINTERFACE;
   }

   STDMETHODIMP_(ULONG) AddRef() override
   {
      return (ULONG)InterlockedIncrement(&ref_count_);
   }

   STDMETHODIMP_(ULONG) Release() override
   {
      LONG count = InterlockedDecrement(&ref_count_);
      if (count == 0)
         delete this;
      return (ULONG)count;
   }

   STDMETHODIMP OnReadSample(HRESULT status, DWORD stream_index, DWORD flags,
         LONGLONG timestamp, IMFSample *sample) override
   {
      const int64_t arrival_usec = qpc_usec(); /* Before any processing */

      (void)stream_index;
      (void)timestamp;

      if (stopping_)
         return S_OK;

      if (FAILED(status))
      {
         describe_device_error("Reading from the device", status);
         g_running = false;
         return S_OK;
      }

      if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))
      {
         set_last_error("The device stopped streaming. It may have been disconnected.");
         log_message(CAPTURE_LOG_ERROR, "Capture stream ended (flags 0x%08lX).", (unsigned long)flags);
         g_running = false;
         return S_OK;
      }

      /* The sample delivered with this flag already uses the new format. */
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
      {
         std::lock_guard<std::mutex> lock(request_mutex_);
         if (stopping_ || !reader_)
            return S_OK;
         update_output_format(reader_);
      }

      if (sample)
         store_sample(sample, arrival_usec);

      /* Request the next frame, unless capture_stop() is in progress. The lock
       * keeps the reader alive for the duration of the request. */
      std::lock_guard<std::mutex> lock(request_mutex_);
      if (!stopping_ && reader_)
      {
         HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
               0, nullptr, nullptr, nullptr, nullptr);
         if (FAILED(hr))
         {
            describe_device_error("Requesting a frame", hr);
            g_running = false;
         }
      }

      return S_OK;
   }

   STDMETHODIMP OnFlush(DWORD stream_index) override
   {
      (void)stream_index;
      SetEvent(flush_event_);
      return S_OK;
   }

   STDMETHODIMP OnEvent(DWORD stream_index, IMFMediaEvent *event) override
   {
      (void)stream_index;
      (void)event;
      return S_OK;
   }

private:
   volatile LONG     ref_count_;
   std::mutex        request_mutex_; /* Guards reader_ and stopping_ during requests */
   IMFSourceReader  *reader_;
   std::atomic<bool> stopping_;
   HANDLE            flush_event_;
};

/* ------------------------------------------------------------------------ */
/* Session                                                                   */
/* ------------------------------------------------------------------------ */

static mf_scope         g_session_scope;
static IMFMediaSource  *g_source   = nullptr;
static IMFSourceReader *g_reader   = nullptr;
static reader_callback *g_callback = nullptr;

/* Ranks a subtype for the selected conversion mode (higher is better). */
static int format_rank(const GUID &subtype, bool raw_yuv)
{
   if (raw_yuv)
   {
      /* NV12 is the smallest to copy (12 bits per pixel), then YUY2 (16). */
      if (subtype == MFVideoFormat_NV12) return 3;
      if (subtype == MFVideoFormat_YUY2) return 2;
   }
   else if (subtype == MFVideoFormat_RGB32)
      return 3; /* Delivered as RGB by the device: no conversion at all */

   if (subtype == MFVideoFormat_MJPG)
      return 0; /* Needs a decoder */
   return 1;
}

/* Logs the device's native formats grouped by type and size, with the list of
 * frame rates, instead of one line per format (devices may list hundreds). */
static void log_native_formats(IMFSourceReader *reader)
{
   struct group
   {
      std::string         label;
      std::vector<double> rates;
   };
   std::vector<group> groups;

   for (DWORD i = 0;; i++)
   {
      IMFMediaType *type = nullptr;
      GUID subtype = GUID_NULL;
      UINT32 width = 0, height = 0, num = 0, den = 0;
      char label[64];

      HRESULT hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
      if (hr == MF_E_NO_MORE_TYPES)
         break;
      if (FAILED(hr))
         continue;

      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
      MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
      safe_release(&type);

      snprintf(label, sizeof(label), "%s %ux%u", subtype_name(subtype).c_str(), (unsigned)width, (unsigned)height);

      auto it = std::find_if(groups.begin(), groups.end(), [&](const group &g) { return g.label == label; });
      if (it == groups.end())
      {
         groups.push_back(group{ label, {} });
         it = groups.end() - 1;
      }

      const double fps = den ? std::round((double)num / den * 100.0) / 100.0 : 0.0;
      if (std::find(it->rates.begin(), it->rates.end(), fps) == it->rates.end())
         it->rates.push_back(fps);
   }

   for (group &g : groups)
   {
      std::string line = "Native format: " + g.label + " @ ";
      char rate[16];

      std::sort(g.rates.begin(), g.rates.end(), std::greater<double>());
      for (size_t i = 0; i < g.rates.size(); i++)
      {
         snprintf(rate, sizeof(rate), "%s%g", i ? ", " : "", g.rates[i]);
         line += rate;
      }
      log_message(CAPTURE_LOG_DEBUG, "%s fps.", line.c_str());
   }
}

/* Picks the device's native format to request. Preference order:
 * 1. at most 1920x1080 (4K is unnecessary for low-resolution shaders),
 * 2. highest frame rate,
 * 3. largest frame size,
 * 4. best format for the conversion mode (see format_rank()). */
static IMFMediaType *select_native_type(IMFSourceReader *reader, bool raw_yuv)
{
   IMFMediaType *best = nullptr;
   bool best_fits = false;
   unsigned best_fps = 0;
   UINT64 best_area = 0;
   int best_rank = -1;

   for (DWORD i = 0;; i++)
   {
      IMFMediaType *type = nullptr;
      GUID subtype = GUID_NULL;
      UINT32 width = 0, height = 0, rate_num = 0, rate_den = 0;

      HRESULT hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
      if (hr == MF_E_NO_MORE_TYPES)
         break;
      if (FAILED(hr))
         continue;

      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
      MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rate_num, &rate_den);

      const unsigned fps = rate_den ? (unsigned)((double)rate_num / rate_den + 0.5) : 0;
      const UINT64 area  = (UINT64)width * height;
      const bool fits    = width <= 1920 && height <= 1080;
      const int rank     = format_rank(subtype, raw_yuv);

      bool better;
      if (!best)                  better = true;
      else if (fits != best_fits) better = fits;
      else if (fps != best_fps)   better = fps > best_fps;
      else if (area != best_area) better = area > best_area;
      else                        better = rank > best_rank;

      if (better)
      {
         safe_release(&best);
         best      = type;
         best_fits = fits;
         best_fps  = fps;
         best_area = area;
         best_rank = rank;
      }
      else
         safe_release(&type);
   }

   return best;
}

/* Builds an RGB32 output type with the same frame size and rate as `native`. */
static IMFMediaType *make_rgb32_type(IMFMediaType *native)
{
   IMFMediaType *out = nullptr;
   UINT32 width = 0, height = 0, num = 0, den = 0;

   if (FAILED(MFCreateMediaType(&out)))
      return nullptr;

   out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
   out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
   out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

   if (SUCCEEDED(MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &width, &height)))
      MFSetAttributeSize(out, MF_MT_FRAME_SIZE, width, height);
   if (SUCCEEDED(MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &num, &den)))
      MFSetAttributeRatio(out, MF_MT_FRAME_RATE, num, den);
   if (SUCCEEDED(MFGetAttributeRatio(native, MF_MT_PIXEL_ASPECT_RATIO, &num, &den)))
      MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, num, den);

   return out;
}

/* MF_XVP_DISABLE_FRC (mfidl.h, Windows 8 and later), defined here so that
 * older SDK headers also build. */
static const GUID kXvpDisableFrc =
   { 0x2c0afa19, 0x7a97, 0x4d5a, { 0x9e, 0xe8, 0x16, 0xd4, 0xfc, 0x51, 0x8d, 0x8c } };

/*
 * Turns off frame rate conversion in the video processor that the source
 * reader inserts to convert to RGB32. Devices often declare a rate that differs
 * from the signal (59.94 fps for a 60 fps source); the processor then drops or
 * repeats frames to match the declared rate. Without conversion, frames are
 * passed on as they arrive, and the core measures their real rate.
 */
static void disable_frame_rate_conversion(IMFSourceReader *reader)
{
   IMFSourceReaderEx *reader_ex = nullptr;
   unsigned disabled = 0;

   if (FAILED(reader->QueryInterface(IID_PPV_ARGS(&reader_ex))))
      return;

   for (DWORD index = 0;; index++)
   {
      IMFTransform  *transform  = nullptr;
      IMFAttributes *attributes = nullptr;
      GUID category;

      if (FAILED(reader_ex->GetTransformForStream((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, index,
            &category, &transform)))
         break;
      if (SUCCEEDED(transform->GetAttributes(&attributes)) &&
          SUCCEEDED(attributes->SetUINT32(kXvpDisableFrc, TRUE)))
         disabled++;
      safe_release(&attributes);
      safe_release(&transform);
   }
   safe_release(&reader_ex);

   if (disabled)
      log_message(CAPTURE_LOG_DEBUG, "Frame rate conversion disabled in %u video transform(s).", disabled);
}

static void release_session(void)
{
   if (g_callback)
      g_callback->set_stopping();

   if (g_reader && g_callback)
   {
      /* Cancel the pending request; OnFlush() signals when it is done. */
      ResetEvent(g_callback->flush_event());
      if (SUCCEEDED(g_reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS)))
      {
         if (WaitForSingleObject(g_callback->flush_event(), 2000) != WAIT_OBJECT_0)
            log_message(CAPTURE_LOG_WARN, "Timed out waiting for the source reader to flush.");
      }
   }

   if (g_source)
      g_source->Shutdown();

   safe_release(&g_reader);
   safe_release(&g_source);
   safe_release(&g_callback);

   g_running = false;
   g_session_scope.shutdown();
}

/* ------------------------------------------------------------------------ */
/* Public interface                                                          */
/* ------------------------------------------------------------------------ */

extern "C" void capture_set_logger(capture_log_t logger)
{
   g_logger = logger;
}

extern "C" int capture_list_devices(capture_device_t *out, int max)
{
   mf_scope scope;
   IMFAttributes *attributes = nullptr;
   IMFActivate **devices     = nullptr;
   UINT32 count = 0;
   int found    = 0;

   if (!scope.init())
      return 0;

   if (SUCCEEDED(MFCreateAttributes(&attributes, 1)) &&
       SUCCEEDED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) &&
       SUCCEEDED(MFEnumDeviceSources(attributes, &devices, &count)))
   {
      for (UINT32 i = 0; i < count; i++)
      {
         WCHAR *name = nullptr, *link = nullptr;
         UINT32 len  = 0;

         if (found < max &&
             SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) &&
             SUCCEEDED(devices[i]->GetAllocatedString(
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)))
         {
            copy_string(out[found].name, sizeof(out[found].name), wide_to_utf8(name));
            copy_string(out[found].id, sizeof(out[found].id), wide_to_utf8(link));
            found++;
         }

         CoTaskMemFree(name);
         CoTaskMemFree(link);
         devices[i]->Release();
      }
      CoTaskMemFree(devices);
   }

   safe_release(&attributes);
   scope.shutdown();
   return found;
}

extern "C" bool capture_start(const char *id, bool raw_yuv)
{
   IMFAttributes *source_attributes = nullptr;
   IMFAttributes *reader_attributes = nullptr;
   IMFMediaType *native_type        = nullptr;
   std::wstring wide_id             = utf8_to_wide(id);
   GUID native_subtype              = GUID_NULL;
   HRESULT hr;

   capture_stop();
   set_last_error("%s", "");
   g_frames_received = 0;

   if (wide_id.empty() || !g_session_scope.init())
   {
      set_last_error("Could not initialize Media Foundation.");
      return false;
   }

   /* Create the media source for the device. */
   hr = MFCreateAttributes(&source_attributes, 2);
   if (SUCCEEDED(hr))
      hr = source_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
   if (SUCCEEDED(hr))
      hr = source_attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            wide_id.c_str());
   if (SUCCEEDED(hr))
      hr = MFCreateDeviceSource(source_attributes, &g_source);
   safe_release(&source_attributes);
   if (FAILED(hr))
   {
      describe_device_error("Opening the device", hr);
      release_session();
      return false;
   }

   /* Create an asynchronous source reader with format conversion enabled. */
   g_callback = new reader_callback();

   hr = MFCreateAttributes(&reader_attributes, 3);
   if (SUCCEEDED(hr))
      hr = reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, g_callback);
   if (SUCCEEDED(hr))
      hr = reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
   if (SUCCEEDED(hr))
      hr = reader_attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
   if (SUCCEEDED(hr))
      hr = MFCreateSourceReaderFromMediaSource(g_source, reader_attributes, &g_reader);
   safe_release(&reader_attributes);
   if (FAILED(hr))
   {
      describe_device_error("Creating the source reader", hr);
      release_session();
      return false;
   }

   g_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
   g_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

   /* Pick and apply the device's native format. */
   log_native_formats(g_reader);
   native_type = select_native_type(g_reader, raw_yuv);
   if (!native_type)
   {
      set_last_error("The device reports no usable video format.");
      log_message(CAPTURE_LOG_ERROR, "The device reports no usable video format.");
      release_session();
      return false;
   }

   {
      UINT32 width = 0, height = 0, num = 0, den = 0;

      native_type->GetGUID(MF_MT_SUBTYPE, &native_subtype);
      MFGetAttributeSize(native_type, MF_MT_FRAME_SIZE, &width, &height);
      MFGetAttributeRatio(native_type, MF_MT_FRAME_RATE, &num, &den);
      log_message(CAPTURE_LOG_INFO, "Selected native format: %ux%u @ %.3f fps, %s.",
            (unsigned)width, (unsigned)height, den ? (double)num / den : 0.0,
            subtype_name(native_subtype).c_str());
   }

   hr = g_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native_type);
   if (FAILED(hr))
      log_message(CAPTURE_LOG_WARN, "Could not set the native format (0x%08lX), using the device default.",
            (unsigned long)hr);

   /* NV12 and YUY2 are delivered as is when the core converts them; anything
    * else is converted to RGB32 by the source reader. */
   if (!(raw_yuv && SUCCEEDED(hr) && is_raw_yuv(native_subtype)))
   {
      IMFMediaType *output_type = make_rgb32_type(native_type);

      hr = output_type
         ? g_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output_type)
         : E_OUTOFMEMORY;
      safe_release(&output_type);
      if (FAILED(hr))
      {
         safe_release(&native_type);
         describe_device_error("Converting the device format to RGB32", hr);
         release_session();
         return false;
      }
      disable_frame_rate_conversion(g_reader);
   }
   safe_release(&native_type);

   update_output_format(g_reader);

   /* Forget any frame left by a previous device. */
   {
      std::lock_guard<std::mutex> lock(g_slot_mutex);
      for (int i = 0; i < RING_SLOTS; i++)
         g_ring[i].sequence = 0;
      g_reading = -1;
   }

   /* Start streaming: each OnReadSample requests the next frame. */
   g_callback->set_reader(g_reader);
   g_running = true;

   hr = g_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
   if (FAILED(hr))
   {
      describe_device_error("Starting the stream", hr);
      release_session();
      return false;
   }

   return true;
}

extern "C" void capture_stop(void)
{
   if (g_source || g_reader || g_callback)
      release_session();
}

extern "C" bool capture_is_running(void)
{
   return g_running;
}

extern "C" const char *capture_last_error(void)
{
   /* Copied so the pointer stays valid after the lock is released. Only the
    * core thread calls this, so one static copy is enough. */
   static std::string copy;
   std::lock_guard<std::mutex> lock(g_error_mutex);
   copy = g_last_error;
   return copy.c_str();
}

extern "C" double capture_get_frame_rate(void)
{
   std::lock_guard<std::mutex> lock(g_format_mutex);
   return g_out_frame_rate;
}

extern "C" int64_t capture_time_usec(void)
{
   return qpc_usec();
}

extern "C" uint64_t capture_frames_received(void)
{
   return g_frames_received;
}

extern "C" bool capture_peek_frames(uint64_t *oldest, uint64_t *newest, int64_t *newest_arrival_usec)
{
   std::lock_guard<std::mutex> lock(g_slot_mutex);
   int newest_slot = -1;

   *oldest = 0;
   *newest = 0;
   for (int i = 0; i < RING_SLOTS; i++)
   {
      const uint64_t sequence = g_ring[i].sequence;
      if (!sequence)
         continue;
      if (!*oldest || sequence < *oldest)
         *oldest = sequence;
      if (sequence > *newest)
      {
         *newest     = sequence;
         newest_slot = i;
      }
   }

   if (newest_slot < 0)
      return false;
   *newest_arrival_usec = g_ring[newest_slot].arrival_usec;
   return true;
}

extern "C" bool capture_acquire_frame(uint64_t sequence, capture_frame_t *frame)
{
   frame_slot *slot = nullptr;

   {
      std::lock_guard<std::mutex> lock(g_slot_mutex);
      for (int i = 0; i < RING_SLOTS; i++)
         if (g_ring[i].sequence == sequence && sequence)
         {
            g_reading = i; /* Protected from the writer until the next acquire */
            slot = &g_ring[i];
            break;
         }
   }

   if (!slot)
      return false;

   frame->data         = slot->data.data();
   frame->stride       = slot->stride;
   frame->format       = slot->format;
   frame->width        = slot->width;
   frame->height       = slot->height;
   frame->frame_rate   = capture_get_frame_rate();
   frame->arrival_usec = slot->arrival_usec;
   frame->sequence     = sequence;
   return true;
}
