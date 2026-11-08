/*
 * Media Foundation capture backend (Windows).
 *
 * Opens a video capture device through an asynchronous IMFSourceReader.
 * The device's native format is chosen explicitly, then the source reader's
 * video processor converts it to RGB32, which matches libretro's XRGB8888.
 * Each received frame is copied into a shared buffer; the core picks up the
 * most recent one in retro_run().
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

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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

static const char *subtype_name(const GUID &subtype, char *fourcc_buf)
{
   if (subtype == MFVideoFormat_NV12)  return "NV12";
   if (subtype == MFVideoFormat_YUY2)  return "YUY2";
   if (subtype == MFVideoFormat_UYVY)  return "UYVY";
   if (subtype == MFVideoFormat_RGB32) return "RGB32";
   if (subtype == MFVideoFormat_ARGB32) return "ARGB32";
   if (subtype == MFVideoFormat_RGB24) return "RGB24";
   if (subtype == MFVideoFormat_MJPG)  return "MJPG";
   if (subtype == MFVideoFormat_P010)  return "P010";

   /* Video subtypes are FourCC-based: the code is stored in Data1. */
   fourcc_buf[0] = (char)(subtype.Data1 & 0xFF);
   fourcc_buf[1] = (char)((subtype.Data1 >> 8) & 0xFF);
   fourcc_buf[2] = (char)((subtype.Data1 >> 16) & 0xFF);
   fourcc_buf[3] = (char)((subtype.Data1 >> 24) & 0xFF);
   fourcc_buf[4] = '\0';
   return fourcc_buf;
}

static bool is_uncompressed(const GUID &subtype)
{
   return subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2 ||
          subtype == MFVideoFormat_UYVY || subtype == MFVideoFormat_RGB32 ||
          subtype == MFVideoFormat_ARGB32 || subtype == MFVideoFormat_RGB24;
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
/* Shared frame state                                                        */
/* ------------------------------------------------------------------------ */

static std::mutex            g_frame_mutex;
static std::vector<uint32_t> g_frame;        /* Latest frame, tightly packed */
static unsigned              g_frame_width;
static unsigned              g_frame_height;
static uint64_t              g_frame_sequence;

/* Current output format of the source reader (guarded by g_frame_mutex). */
static UINT32 g_out_width;
static UINT32 g_out_height;
static LONG   g_out_stride;
static bool   g_warned_oversize;

static std::atomic<bool> g_running(false);

static void update_output_format(IMFSourceReader *reader)
{
   IMFMediaType *type = nullptr;
   UINT32 width = 0, height = 0;
   LONG stride;

   if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)))
      return;

   MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
   stride = (LONG)MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, width * 4);
   safe_release(&type);

   {
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      g_out_width       = width;
      g_out_height      = height;
      g_out_stride      = stride;
      g_warned_oversize = false;
   }

   log_message(CAPTURE_LOG_INFO, "Output format: %ux%u RGB32, stride %ld.",
         (unsigned)width, (unsigned)height, (long)stride);
}

static void copy_rows(const BYTE *scan0, LONG pitch, unsigned width, unsigned height)
{
   /* Called with g_frame_mutex held. */
   g_frame.resize((size_t)width * height);

   for (unsigned y = 0; y < height; y++)
      memcpy(&g_frame[(size_t)y * width], scan0 + (ptrdiff_t)y * pitch, (size_t)width * 4);

   g_frame_width  = width;
   g_frame_height = height;
   g_frame_sequence++;
}

static void store_sample(IMFSample *sample)
{
   IMFMediaBuffer *buffer = nullptr;
   IMF2DBuffer *buffer2d  = nullptr;

   if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
      return;

   std::lock_guard<std::mutex> lock(g_frame_mutex);
   const unsigned width  = g_out_width;
   const unsigned height = g_out_height;

   if (width == 0 || height == 0)
   {
      safe_release(&buffer);
      return;
   }

   if (width > CAPTURE_MAX_WIDTH || height > CAPTURE_MAX_HEIGHT)
   {
      if (!g_warned_oversize)
         log_message(CAPTURE_LOG_WARN, "Frame %ux%u exceeds the %ux%u limit, ignoring.",
               width, height, CAPTURE_MAX_WIDTH, CAPTURE_MAX_HEIGHT);
      g_warned_oversize = true;
      safe_release(&buffer);
      return;
   }

   /* Prefer IMF2DBuffer: it gives the real pitch and first-row pointer,
    * including for bottom-up images (negative pitch). */
   if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))))
   {
      BYTE *scan0 = nullptr;
      LONG pitch  = 0;

      if (SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch)))
      {
         copy_rows(scan0, pitch, width, height);
         buffer2d->Unlock2D();
      }
      safe_release(&buffer2d);
   }
   else
   {
      BYTE *data      = nullptr;
      DWORD max_len   = 0;
      DWORD cur_len   = 0;
      LONG pitch      = g_out_stride ? g_out_stride : (LONG)(width * 4);
      LONG abs_pitch  = pitch < 0 ? -pitch : pitch;

      if (SUCCEEDED(buffer->Lock(&data, &max_len, &cur_len)))
      {
         if ((size_t)cur_len >= (size_t)abs_pitch * height && abs_pitch >= (LONG)(width * 4))
         {
            const BYTE *scan0 = pitch < 0 ? data + (size_t)abs_pitch * (height - 1) : data;
            copy_rows(scan0, pitch, width, height);
         }
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
   void set_reader(IMFSourceReader *reader) { reader_ = reader; }
   void set_stopping() { stopping_ = true; }
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
      (void)stream_index;
      (void)timestamp;

      if (stopping_)
         return S_OK;

      if (FAILED(status))
      {
         log_message(CAPTURE_LOG_ERROR, "ReadSample failed (0x%08lX).", (unsigned long)status);
         g_running = false;
         return S_OK;
      }

      if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))
      {
         log_message(CAPTURE_LOG_ERROR, "Capture stream ended (flags 0x%08lX).", (unsigned long)flags);
         g_running = false;
         return S_OK;
      }

      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
         update_output_format(reader_);

      if (sample)
         store_sample(sample);

      /* Request the next frame. */
      if (!stopping_ && reader_)
      {
         HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
               0, nullptr, nullptr, nullptr, nullptr);
         if (FAILED(hr))
         {
            log_message(CAPTURE_LOG_ERROR, "ReadSample request failed (0x%08lX).", (unsigned long)hr);
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

/* Picks the device's native format to request. Preference order:
 * 1. at most 1920x1080 (4K is unnecessary for low-resolution shaders),
 * 2. highest frame rate,
 * 3. largest frame size,
 * 4. uncompressed formats over MJPG and others. */
static IMFMediaType *select_native_type(IMFSourceReader *reader)
{
   IMFMediaType *best = nullptr;
   bool best_fits = false, best_raw = false;
   unsigned best_fps = 0;
   UINT64 best_area = 0;

   for (DWORD i = 0;; i++)
   {
      IMFMediaType *type = nullptr;
      GUID subtype = GUID_NULL;
      UINT32 width = 0, height = 0, rate_num = 0, rate_den = 0;
      char fourcc[5];

      HRESULT hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type);
      if (hr == MF_E_NO_MORE_TYPES)
         break;
      if (FAILED(hr))
         continue;

      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
      MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rate_num, &rate_den);

      const double fps_exact = rate_den ? (double)rate_num / rate_den : 0.0;
      const unsigned fps     = (unsigned)(fps_exact + 0.5);
      const UINT64 area      = (UINT64)width * height;
      const bool fits        = width <= 1920 && height <= 1080;
      const bool raw         = is_uncompressed(subtype);

      log_message(CAPTURE_LOG_DEBUG, "Native format %lu: %ux%u @ %.3f fps, %s.",
            (unsigned long)i, (unsigned)width, (unsigned)height, fps_exact,
            subtype_name(subtype, fourcc));

      bool better = false;
      if (!best)                       better = true;
      else if (fits != best_fits)      better = fits;
      else if (fps != best_fps)        better = fps > best_fps;
      else if (area != best_area)      better = area > best_area;
      else if (raw != best_raw)        better = raw;

      if (better)
      {
         safe_release(&best);
         best      = type;
         best_fits = fits;
         best_fps  = fps;
         best_area = area;
         best_raw  = raw;
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

static void release_session(void)
{
   if (g_callback)
      g_callback->set_stopping();

   if (g_reader && g_callback)
   {
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

extern "C" bool capture_start(const char *id)
{
   IMFAttributes *source_attributes = nullptr;
   IMFAttributes *reader_attributes = nullptr;
   IMFMediaType *native_type        = nullptr;
   IMFMediaType *output_type        = nullptr;
   std::wstring wide_id             = utf8_to_wide(id);
   HRESULT hr;

   capture_stop();

   if (wide_id.empty() || !g_session_scope.init())
      return false;

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
      log_message(CAPTURE_LOG_ERROR, "Could not open the capture device (0x%08lX).", (unsigned long)hr);
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
      log_message(CAPTURE_LOG_ERROR, "Could not create the source reader (0x%08lX).", (unsigned long)hr);
      release_session();
      return false;
   }

   g_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
   g_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

   /* Pick and apply the device's native format. */
   native_type = select_native_type(g_reader);
   if (!native_type)
   {
      log_message(CAPTURE_LOG_ERROR, "The device reports no usable video format.");
      release_session();
      return false;
   }

   {
      GUID subtype = GUID_NULL;
      UINT32 width = 0, height = 0, num = 0, den = 0;
      char fourcc[5];

      native_type->GetGUID(MF_MT_SUBTYPE, &subtype);
      MFGetAttributeSize(native_type, MF_MT_FRAME_SIZE, &width, &height);
      MFGetAttributeRatio(native_type, MF_MT_FRAME_RATE, &num, &den);
      log_message(CAPTURE_LOG_INFO, "Selected native format: %ux%u @ %.3f fps, %s.",
            (unsigned)width, (unsigned)height, den ? (double)num / den : 0.0,
            subtype_name(subtype, fourcc));
   }

   hr = g_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native_type);
   if (FAILED(hr))
      log_message(CAPTURE_LOG_WARN, "Could not set the native format (0x%08lX), using the device default.",
            (unsigned long)hr);

   /* Ask the source reader to convert to RGB32. */
   output_type = make_rgb32_type(native_type);
   hr = output_type
      ? g_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output_type)
      : E_OUTOFMEMORY;
   safe_release(&output_type);
   safe_release(&native_type);
   if (FAILED(hr))
   {
      log_message(CAPTURE_LOG_ERROR, "Could not convert the device format to RGB32 (0x%08lX).",
            (unsigned long)hr);
      release_session();
      return false;
   }

   update_output_format(g_reader);

   /* Start streaming: each OnReadSample requests the next frame. */
   g_callback->set_reader(g_reader);
   g_running = true;

   hr = g_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
   if (FAILED(hr))
   {
      log_message(CAPTURE_LOG_ERROR, "Could not start streaming (0x%08lX).", (unsigned long)hr);
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

extern "C" bool capture_fetch_frame(uint32_t *dst, unsigned *width, unsigned *height, uint64_t *sequence)
{
   std::lock_guard<std::mutex> lock(g_frame_mutex);

   if (g_frame_sequence == *sequence || g_frame.empty())
      return false;

   memcpy(dst, g_frame.data(), g_frame.size() * sizeof(uint32_t));
   *width    = g_frame_width;
   *height   = g_frame_height;
   *sequence = g_frame_sequence;
   return true;
}
