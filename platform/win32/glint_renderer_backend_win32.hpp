#pragma once

#include "../../glint_render_backend.h"
#include "../glint_view_base.hpp"
#include "glint_win32_surface_shared.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>
#include <array>
#include <optional>
#include <string>

inline const char* glint_backend_name(glint_backend backend)
{
  switch (backend)
  {
    case glint_backend::Auto:
      return "Auto";
    case glint_backend::CPU:
      return "CPU";
    case glint_backend::OpenGL:
      return "OpenGL";
    case glint_backend::D3D11:
      return "D3D11";
    case glint_backend::D3D12:
      return "D3D12";
    case glint_backend::Vulkan:
      return "Vulkan";
    default:
      return "Unknown";
  }
}

inline glint_backend glint_auto_backend()
{
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
  #if defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
  return glint_backend::D3D12;
  #else
  return glint_backend::OpenGL;
  #endif
#else
  return glint_backend::CPU;
#endif
}

inline glint_backend glint_resolve_backend(glint_backend backend)
{
  return backend == glint_backend::Auto ? glint_auto_backend() : backend;
}

class glint_renderer_backend_win32
{
public:
  virtual ~glint_renderer_backend_win32() = default;

  virtual bool initialize(HWND hwnd) = 0;
  virtual void shutdown() = 0;
  virtual bool resize(int width, int height) = 0;
  virtual SkCanvas* beginFrame() = 0;
  virtual void endFrame() = 0;
  virtual void present() = 0;
  virtual glint_backend backend() const = 0;
  virtual bool isGpu() const = 0;
  virtual const char* diagnostic() const = 0;
  /** True once the GPU device was removed or reset (driver update, TDR, GPU
   *  switch).  The host should recreate the renderer; a fresh device usually
   *  works, unlike the lost one. */
  virtual bool deviceLost() const { return false; }
  /** Live resize: while on, frames still render on the GPU but reach the
   *  window through GDI (read back and drawn into it). Windows shows a GDI
   *  window's new size and its new content in the same frame, while
   *  swapchain frames arrive a frame or more after the moved border, so the
   *  content visibly trailed the edge being dragged. Returns whether the
   *  renderer presents through GDI now (it needs a DirectComposition
   *  swapchain, which can be hidden; renderers without one ignore this). */
  virtual bool setPresentThroughGdi(bool /*on*/) { return false; }
  /** The window went idle: release GPU resources unused for a while. */
  virtual void trimIdleMemory() {}
};

class glint_cpu_renderer_backend_win32 final : public glint_renderer_backend_win32
{
public:
  bool initialize(HWND hwnd) override
  {
    mHWND = hwnd;
    mDiagnostic.clear();
    return true;
  }

  void shutdown() override
  {
    if (mPaintDeviceContext && mHWND)
      ::ReleaseDC(mHWND, mPaintDeviceContext);

    mPaintDeviceContext = nullptr;

    mCpuCanvas.reset();
    mBitmap = SkBitmap();
    mHWND = nullptr;
    mWidth = 0;
    mHeight = 0;
    mDiagnostic.clear();
  }

  bool resize(int width, int height) override
  {
    mWidth = width;
    mHeight = height;

    if (mWidth <= 0 || mHeight <= 0)
    {
      mCpuCanvas.reset();
      mBitmap = SkBitmap();
      mDiagnostic.clear();
      return true;
    }

    mBitmap.allocN32Pixels(mWidth, mHeight);
    mBitmap.eraseColor(SK_ColorBLACK);
    mCpuCanvas = std::make_unique<SkCanvas>(mBitmap);
    mDiagnostic.clear();
    return true;
  }

  SkCanvas* beginFrame() override
  {
    // Re-create the bitmap if the window has a size again (e.g. this
    // renderer was resized to 0 x 0 while the window was collapsed).
    if (mHWND && !mCpuCanvas)
    {
      RECT rc = {};
      ::GetClientRect(mHWND, &rc);
      if (rc.right > 0 && rc.bottom > 0)
        resize(rc.right, rc.bottom);
    }
    if (!mHWND || !mCpuCanvas)
    {
      mDiagnostic = "CPU backend is not ready";
      return nullptr;
    }

    // Validate the WM_PAINT region first, then blit through a fresh window DC.
    // BeginPaint constrains drawing to the invalid region, which causes live
    // resize paints to update only a strip of the inspector window.
    HDC paintContext = ::BeginPaint(mHWND, &mPaintStruct);
    if (!paintContext)
    {
      mDiagnostic = "BeginPaint failed for CPU backend";
      return nullptr;
    }

    ::EndPaint(mHWND, &mPaintStruct);

    mPaintDeviceContext = ::GetDC(mHWND);
    if (!mPaintDeviceContext)
    {
      mDiagnostic = "GetDC failed for CPU backend";
      return nullptr;
    }

    mDiagnostic.clear();
    return mCpuCanvas.get();
  }

  void endFrame() override
  {
  }

  void present() override
  {
    if (!mPaintDeviceContext)
      return;

    glint_win32_surface::presentBitmapToWindow(mPaintDeviceContext, mBitmap, mWidth, mHeight);
    ::ReleaseDC(mHWND, mPaintDeviceContext);
    mPaintDeviceContext = nullptr;
  }

  glint_backend backend() const override
  {
    return glint_backend::CPU;
  }

  bool isGpu() const override
  {
    return false;
  }

  const char* diagnostic() const override
  {
    return mDiagnostic.empty() ? nullptr : mDiagnostic.c_str();
  }

private:
  HWND                      mHWND = nullptr;
  int                       mWidth = 0;
  int                       mHeight = 0;
  SkBitmap                  mBitmap;
  std::unique_ptr<SkCanvas> mCpuCanvas;
  PAINTSTRUCT               mPaintStruct = {};
  HDC                       mPaintDeviceContext = nullptr;
  std::string               mDiagnostic;
};

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && (!defined(GLINT_ENABLE_D3D12) || !GLINT_ENABLE_D3D12)
class glint_opengl_renderer_backend_win32 final : public glint_renderer_backend_win32
{
public:
  bool initialize(HWND hwnd) override
  {
    mHWND = hwnd;
    mLastInitResult = glint_win32_surface::initializeOpenGLContext(
      mHWND,
      mGLDC,
      mGLRC,
      mGrContext,
      mCanvas,
      mGpuSurface);

    if (const char* diagnostic = diagnosticForInitResult(mLastInitResult))
      mDiagnostic = diagnostic;
    else
      mDiagnostic.clear();

    return mLastInitResult == glint_win32_surface::open_gl_init_result::success;
  }

  void shutdown() override
  {
    mHostContext.reset();
    glint_win32_surface::destroyOpenGLContext(mHWND, mGLDC, mGLRC, mGrContext, mGpuSurface, mCanvas);
    mHWND = nullptr;
    mWidth = 0;
    mHeight = 0;
    mDiagnostic.clear();
  }

  bool resize(int width, int height) override
  {
    mWidth = width;
    mHeight = height;

    if (mWidth <= 0 || mHeight <= 0)
    {
      glint_win32_surface::releaseOpenGLSurface(mGLDC, mGLRC, mGpuSurface, mCanvas);
      mDiagnostic.clear();
      return true;
    }

    const bool success = glint_win32_surface::recreateOpenGLSurface(
      mWidth,
      mHeight,
      mGLDC,
      mGLRC,
      mGrContext,
      mGpuSurface,
      mCanvas);

    mDiagnostic = success ? std::string() : std::string("GPU surface creation failed");
    return success;
  }

  SkCanvas* beginFrame() override
  {
    if (!mHWND || !mCanvas || !mGrContext || !mGLDC || !mGLRC)
    {
      mDiagnostic = "OpenGL backend is not ready";
      return nullptr;
    }

    HDC deviceContext = ::BeginPaint(mHWND, &mPaintStruct);
    if (!deviceContext)
    {
      mDiagnostic = "BeginPaint failed for OpenGL backend";
      return nullptr;
    }

    ::EndPaint(mHWND, &mPaintStruct);

    // Draw on our context; present() gives the thread back to the host's.
    mHostContext.emplace();
    if (!glint_win32_surface::makeOpenGLContextCurrent(mGLDC, mGLRC))
    {
      mHostContext.reset();
      mDiagnostic = "wglMakeCurrent failed for OpenGL backend";
      return nullptr;
    }

    mDiagnostic.clear();
    return mCanvas;
  }

  void endFrame() override
  {
    if (!mGrContext)
      return;

    mGrContext->flush();
    mGrContext->submit(GrSyncCpu::kNo);
  }

  void present() override
  {
    if (mGLDC)
      ::SwapBuffers(mGLDC);
    mHostContext.reset();
  }

  glint_backend backend() const override
  {
    return glint_backend::OpenGL;
  }

  bool isGpu() const override
  {
    return true;
  }

  const char* diagnostic() const override
  {
    return mDiagnostic.empty() ? nullptr : mDiagnostic.c_str();
  }

private:
  static const char* diagnosticForInitResult(glint_win32_surface::open_gl_init_result result)
  {
    switch (result)
    {
      case glint_win32_surface::open_gl_init_result::success:
        return nullptr;

      case glint_win32_surface::open_gl_init_result::missing_window:
        return "GPU init skipped because the view has no HWND";

      case glint_win32_surface::open_gl_init_result::get_dc_failed:
        return "GetDC failed for OpenGL backend";

      case glint_win32_surface::open_gl_init_result::pixel_format_failed:
        return "pixel format setup failed for OpenGL backend";

      case glint_win32_surface::open_gl_init_result::legacy_context_failed:
        return "legacy WGL context creation failed";

      case glint_win32_surface::open_gl_init_result::gr_context_failed:
        return "GrDirectContext creation failed";

      default:
        return "OpenGL backend initialization failed";
    }
  }

  HWND                                      mHWND = nullptr;
  int                                       mWidth = 0;
  int                                       mHeight = 0;
  HDC                                       mGLDC = nullptr;
  HGLRC                                     mGLRC = nullptr;
  sk_sp<GrDirectContext>                    mGrContext;
  sk_sp<SkSurface>                          mGpuSurface;
  SkCanvas*                                 mCanvas = nullptr;
  PAINTSTRUCT                               mPaintStruct = {};
  glint_win32_surface::open_gl_init_result  mLastInitResult = glint_win32_surface::open_gl_init_result::missing_window;
  std::string                               mDiagnostic;
  // The context current before beginFrame(); restored by present().
  std::optional<glint_win32_surface::wgl_context_restorer> mHostContext;
};
#endif // GLINT_RENDER_GPU && !GLINT_ENABLE_D3D12

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
class glint_d3d12_renderer_backend_win32 final : public glint_renderer_backend_win32
{
public:
  bool initialize(HWND hwnd) override
  {
    mHWND = hwnd;
    mLastInitResult = glint_win32_surface::initializeDirect3DContext(
      mHWND,
      mAdapter,
      mDevice,
      mQueue,
      mSwapChain,
      mFence,
      mFenceEvent,
      mGrContext,
      mFenceValues.data(),
      kBufferCount,
      mBufferIndex,
      compositionFor(mHWND));

    if (const char* diagnostic = diagnosticForInitResult(mLastInitResult))
      mDiagnostic = diagnostic;
    else
      mDiagnostic.clear();

    return mLastInitResult == glint_win32_surface::direct3d_init_result::success;
  }

  /** Like initialize(), with the device created beforehand (possibly on
   *  another thread) by glint_win32_surface::createDirect3DDevice(). */
  bool initializeWithDevice(HWND hwnd, glint_win32_surface::direct3d_device&& d)
  {
    mHWND = hwnd;
    mLastInitResult = d.result;
    if (mLastInitResult == glint_win32_surface::direct3d_init_result::success)
    {
      mAdapter   = std::move(d.adapter);
      mDevice    = std::move(d.device);
      mQueue     = std::move(d.queue);
      mGrContext = std::move(d.grContext);
      mLastInitResult = glint_win32_surface::createDirect3DSwapChain(
        mHWND, d.factory.get(), mDevice.get(), mQueue.get(), mSwapChain, mFence, mFenceEvent,
        mFenceValues.data(), kBufferCount, mBufferIndex, compositionFor(mHWND));
    }

    if (const char* diagnostic = diagnosticForInitResult(mLastInitResult))
      mDiagnostic = diagnostic;
    else
      mDiagnostic.clear();

    return mLastInitResult == glint_win32_surface::direct3d_init_result::success;
  }

  void shutdown() override
  {
    if (mGrContext)
    {
      mGrContext->flush();
      mGrContext->submit(GrSyncCpu::kYes);
    }

    mCurrentSurface = nullptr;
    mCanvas = nullptr;
    mComposition.reset();
    if (mFrameLatencyWaitable)
    {
      ::CloseHandle(mFrameLatencyWaitable);
      mFrameLatencyWaitable = nullptr;
    }
    glint_win32_surface::destroyDirect3DResources(
      mFenceEvent,
      mFence,
      mSwapChain,
      mQueue,
      mDevice,
      mAdapter,
      mGrContext,
      mSurfaces.data(),
      mBuffers.data(),
      kBufferCount);
    mHWND = nullptr;
    mWidth = 0;
    mHeight = 0;
    mGdiPresent = false;
    mGdiSurface.reset();
    mGdiBitmap.reset();
    mBufferIndex = 0;
    mFenceValues.fill(0);
    mDiagnostic.clear();
  }

  bool resize(int width, int height) override
  {
    mWidth = width;
    mHeight = height;

    // Presenting through GDI (live resize): the swapchain is resized once,
    // when that ends (setPresentThroughGdi(false)).
    if (mGdiPresent && width > 0 && height > 0)
    {
      mCurrentSurface = nullptr;
      mDiagnostic.clear();
      return true;
    }

    const bool success = glint_win32_surface::recreateDirect3DSurfaces(
      mWidth,
      mHeight,
      mSwapChain,
      mFence,
      mFenceEvent,
      mGrContext,
      mSurfaces.data(),
      mBuffers.data(),
      mFenceValues.data(),
      kBufferCount,
      mBufferIndex,
      mCanvas);

    mCurrentSurface = nullptr;
    mDiagnostic = success ? std::string() : std::string("D3D12 swapchain surface creation failed");
    return success;
  }

  bool setPresentThroughGdi(bool on) override
  {
    if (on == mGdiPresent) return mGdiPresent;
    if (on)
    {
      // GDI content can't show over an HWND swapchain, only over a hidden
      // composition one.
      if (!mComposition.device) return false;
      mGdiPresent = true;
      return true;
    }
    mGdiPresent = false;
    mGdiSurface.reset();
    mGdiBitmap.reset();
    resize(mWidth, mHeight);   // the swapchain catches up with the final size
    return false;
  }

  SkCanvas* beginFrame() override
  {
    if (mDeviceLost)
    {
      mDiagnostic = "D3D12 device lost";
      return nullptr;
    }

    if (!mHWND || !mGrContext || !mSwapChain || mWidth <= 0 || mHeight <= 0)
    {
      mDiagnostic = "D3D12 backend is not ready";
      return nullptr;
    }

    HDC deviceContext = ::BeginPaint(mHWND, &mPaintStruct);
    if (!deviceContext)
    {
      mDiagnostic = "BeginPaint failed for D3D12 backend";
      return nullptr;
    }

    ::EndPaint(mHWND, &mPaintStruct);

    if (mGdiPresent)
    {
      // Live resize: draw into an offscreen GPU surface; present() reads it
      // back and draws it into the window through GDI.
      if (!mGdiSurface || mGdiSurface->width() != mWidth || mGdiSurface->height() != mHeight)
        mGdiSurface = SkSurfaces::RenderTarget(mGrContext.get(), skgpu::Budgeted::kYes,
                                               SkImageInfo::MakeN32Premul(mWidth, mHeight));
      if (!mGdiSurface)
      {
        mDiagnostic = "D3D12 offscreen surface creation failed";
        return nullptr;
      }
      mCurrentSurface = nullptr;
      mDiagnostic.clear();
      return mGdiSurface->getCanvas();
    }

    // One-frame queue: wait until the compositor took the previous frame, so
    // this one is drawn for the newest state (window size) and shown next.
    if (!mFrameLatencyWaitable && mSwapChain)
      mFrameLatencyWaitable = mSwapChain->GetFrameLatencyWaitableObject();
    if (mFrameLatencyWaitable)
      ::WaitForSingleObjectEx(mFrameLatencyWaitable, 100, TRUE);

    mCurrentSurface = glint_win32_surface::acquireDirect3DBackbufferSurface(
      mSwapChain,
      mFence,
      mFenceEvent,
      mSurfaces.data(),
      mFenceValues.data(),
      kBufferCount,
      mBufferIndex,
      mCanvas);
    if (!mCurrentSurface || !mCanvas)
    {
      mDiagnostic = "failed to acquire D3D12 swapchain backbuffer";
      return nullptr;
    }

    mDiagnostic.clear();
    return mCanvas;
  }

  void endFrame() override
  {
    if (!mGrContext || !mCurrentSurface)
      return;

    GrFlushInfo flushInfo = {};
    mGrContext->flush(mCurrentSurface, SkSurfaces::BackendSurfaceAccess::kPresent, flushInfo);
    mGrContext->submit();
  }

  void present() override
  {
    if (mGdiPresent)
    {
      presentThroughGdi();
      return;
    }

    if (!mSwapChain || !mQueue || !mFence)
      return;

    const HRESULT presentResult = mSwapChain->Present(1, 0);
    if (SUCCEEDED(presentResult)) mComposition.show();
    // Signal even when Present failed: beginFrame already advanced this
    // buffer's fence value and the next acquire / resize waits for it, so
    // skipping the signal hangs the window thread.  After device removal
    // fences read as complete, so this cannot hang either.
    const HRESULT signalResult = mQueue->Signal(mFence.get(), mFenceValues[mBufferIndex]);

    if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET)
    {
      mDeviceLost = true;
      mDiagnostic = "D3D12 device lost";
    }
    else if (FAILED(presentResult))
      mDiagnostic = "D3D12 Present failed";
    else if (FAILED(signalResult))
      mDiagnostic = "D3D12 queue signal failed";
    else
      mDiagnostic.clear();
  }

  bool deviceLost() const override { return mDeviceLost; }

  void trimIdleMemory() override
  {
    // Cached textures / layers not used in the last few seconds: pages left
    // behind keep nothing alive in the cache.
    if (mGrContext) mGrContext->performDeferredCleanup(std::chrono::seconds(3));
  }

  glint_backend backend() const override
  {
    return glint_backend::D3D12;
  }

  bool isGpu() const override
  {
    return true;
  }

  const char* diagnostic() const override
  {
    return mDiagnostic.empty() ? nullptr : mDiagnostic.c_str();
  }

private:
  static constexpr int kBufferCount = 2;


  // Reads the offscreen frame back and draws it into the window through GDI
  // (setPresentThroughGdi()). Once it is there, the swapchain is hidden.
  void presentThroughGdi()
  {
    if (!mGdiSurface || mWidth <= 0 || mHeight <= 0) return;
    if (mGdiBitmap.width() != mWidth || mGdiBitmap.height() != mHeight)
      mGdiBitmap.allocN32Pixels(mWidth, mHeight);
    if (!mGdiSurface->readPixels(mGdiBitmap, 0, 0)) return;
    if (HDC dc = ::GetDC(mHWND))
    {
      glint_win32_surface::presentBitmapToWindow(dc, mGdiBitmap, mWidth, mHeight);
      ::ReleaseDC(mHWND, dc);
    }
    mComposition.hide();
  }

  // Top-level windows show the swapchain through DirectComposition, which can
  // be hidden for presenting through GDI (live resize); child windows
  // (embedded views) keep an HWND swapchain.
  // GLINT_D3D_COMPOSITION=0 turns it off (HWND swapchains everywhere).
  glint_win32_surface::direct3d_composition* compositionFor(HWND hwnd)
  {
    static const bool disabled = [] {
      char value[8] = {};
      const DWORD n = ::GetEnvironmentVariableA("GLINT_D3D_COMPOSITION", value, sizeof(value));
      return n > 0 && value[0] == '0';
    }();
    if (disabled || (::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD)) return nullptr;
    return &mComposition;
  }

  static const char* diagnosticForInitResult(glint_win32_surface::direct3d_init_result result)
  {
    switch (result)
    {
      case glint_win32_surface::direct3d_init_result::success:
        return nullptr;

      case glint_win32_surface::direct3d_init_result::missing_window:
        return "GPU init skipped because the view has no HWND";

      case glint_win32_surface::direct3d_init_result::factory_failed:
        return "DXGI factory creation failed";

      case glint_win32_surface::direct3d_init_result::adapter_failed:
        return "no suitable Direct3D adapter was found";

      case glint_win32_surface::direct3d_init_result::device_failed:
        return "D3D12 device creation failed";

      case glint_win32_surface::direct3d_init_result::queue_failed:
        return "D3D12 command queue creation failed";

      case glint_win32_surface::direct3d_init_result::context_failed:
        return "GrDirectContext creation failed";

      case glint_win32_surface::direct3d_init_result::swapchain_failed:
        return "DXGI swapchain creation failed";

      case glint_win32_surface::direct3d_init_result::fence_failed:
        return "D3D12 fence creation failed";

      case glint_win32_surface::direct3d_init_result::fence_event_failed:
        return "D3D12 fence event creation failed";

      default:
        return "D3D12 backend initialization failed";
    }
  }

  HWND                              mHWND = nullptr;
  int                               mWidth = 0;    // window client size
  int                               mHeight = 0;
  bool                              mGdiPresent = false;   // setPresentThroughGdi()
  sk_sp<SkSurface>                  mGdiSurface;           // offscreen frame while presenting through GDI
  SkBitmap                          mGdiBitmap;            // its read-back pixels
  PAINTSTRUCT                       mPaintStruct = {};
  gr_cp<IDXGIAdapter1>              mAdapter;
  gr_cp<ID3D12Device>               mDevice;
  gr_cp<ID3D12CommandQueue>         mQueue;
  gr_cp<IDXGISwapChain3>            mSwapChain;
  gr_cp<ID3D12Fence>                mFence;
  HANDLE                            mFenceEvent = nullptr;
  sk_sp<GrDirectContext>            mGrContext;
  std::array<gr_cp<ID3D12Resource>, kBufferCount> mBuffers;
  glint_win32_surface::direct3d_composition       mComposition;
  HANDLE                                           mFrameLatencyWaitable = nullptr;
  std::array<sk_sp<SkSurface>, kBufferCount>      mSurfaces;
  std::array<uint64_t, kBufferCount>              mFenceValues = {};
  bool                                            mDeviceLost  = false;
  unsigned int                      mBufferIndex = 0;
  SkCanvas*                         mCanvas = nullptr;
  SkSurface*                        mCurrentSurface = nullptr;
  glint_win32_surface::direct3d_init_result mLastInitResult = glint_win32_surface::direct3d_init_result::missing_window;
  std::string                       mDiagnostic;
};
#endif

inline std::unique_ptr<glint_renderer_backend_win32> create_glint_renderer_backend_win32(glint_backend backend)
{
  switch (glint_resolve_backend(backend))
  {
    case glint_backend::CPU:
      return std::make_unique<glint_cpu_renderer_backend_win32>();

    case glint_backend::OpenGL:
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && (!defined(GLINT_ENABLE_D3D12) || !GLINT_ENABLE_D3D12)
      return std::make_unique<glint_opengl_renderer_backend_win32>();
#else
      return nullptr;
#endif

    case glint_backend::D3D12:
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
      return std::make_unique<glint_d3d12_renderer_backend_win32>();
#else
      return nullptr;
#endif

    default:
      return nullptr;
  }
}
