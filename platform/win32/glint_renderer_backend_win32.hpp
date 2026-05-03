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
      mCanvas = nullptr;
      mGpuSurface.reset();
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
    ::wglMakeCurrent(mGLDC, mGLRC);
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
};

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
      mBufferIndex);

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
    mBufferIndex = 0;
    mFenceValues.fill(0);
    mDiagnostic.clear();
  }

  bool resize(int width, int height) override
  {
    mWidth = width;
    mHeight = height;

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

  SkCanvas* beginFrame() override
  {
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
    if (!mSwapChain || !mQueue || !mFence)
      return;

    if (FAILED(mSwapChain->Present(1, 0)))
      mDiagnostic = "D3D12 Present failed";
    else if (FAILED(mQueue->Signal(mFence.get(), mFenceValues[mBufferIndex])))
      mDiagnostic = "D3D12 queue signal failed";
    else
      mDiagnostic.clear();
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
  int                               mWidth = 0;
  int                               mHeight = 0;
  PAINTSTRUCT                       mPaintStruct = {};
  gr_cp<IDXGIAdapter1>              mAdapter;
  gr_cp<ID3D12Device>               mDevice;
  gr_cp<ID3D12CommandQueue>         mQueue;
  gr_cp<IDXGISwapChain3>            mSwapChain;
  gr_cp<ID3D12Fence>                mFence;
  HANDLE                            mFenceEvent = nullptr;
  sk_sp<GrDirectContext>            mGrContext;
  std::array<gr_cp<ID3D12Resource>, kBufferCount> mBuffers;
  std::array<sk_sp<SkSurface>, kBufferCount>      mSurfaces;
  std::array<uint64_t, kBufferCount>              mFenceValues = {};
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
      return std::make_unique<glint_opengl_renderer_backend_win32>();

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
