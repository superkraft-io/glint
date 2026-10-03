#pragma once

#include "../../glint_core.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"

#include <chrono>

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrTypes.h"
#  if !defined(GLINT_ENABLE_D3D12) || !GLINT_ENABLE_D3D12
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"
#include "include/gpu/ganesh/gl/win/GrGLMakeWinInterface.h"
#  endif
#  if defined(SK_DIRECT3D)
#    include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#  endif
#endif

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#include "glint_d3d_shader_cache.hpp"
#endif

namespace glint_win32_surface
{
inline void updateDocumentBounds(glint_document& document, int width, int height)
{
  const glint_rect bounds(0.f, 0.f, static_cast<float>(width), static_cast<float>(height));
  document.mCanvas.mRect = bounds;
  document.mCanvas.mPaintRECT = bounds;
  document.mCanvas.mParentW = static_cast<float>(width);
  document.mCanvas.mParentH = static_cast<float>(height);
  document.mLayoutDirty = true;
}

inline void presentBitmapToWindow(HDC deviceContext, const SkBitmap& bitmap, int width, int height)
{
  BITMAPINFO bitmapInfo = {};
  bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmapInfo.bmiHeader.biWidth = width;
  bitmapInfo.bmiHeader.biHeight = -height;
  bitmapInfo.bmiHeader.biPlanes = 1;
  bitmapInfo.bmiHeader.biBitCount = 32;
  bitmapInfo.bmiHeader.biCompression = BI_RGB;

  ::StretchDIBits(
    deviceContext,
    0,
    0,
    width,
    height,
    0,
    0,
    width,
    height,
    bitmap.getPixels(),
    &bitmapInfo,
    DIB_RGB_COLORS,
    SRCCOPY);
}

inline bool paintDocumentCpuOpaque(
  HWND hwnd,
  glint_document& document,
  SkCanvas& canvas,
  const SkColor clearColor,
  const SkBitmap& bitmap,
  int width,
  int height,
  double* drawMs = nullptr,
  double* presentMs = nullptr)
{
  PAINTSTRUCT paintStruct = {};
  HDC deviceContext = ::BeginPaint(hwnd, &paintStruct);
  if (!deviceContext)
    return false;

  const auto drawStart = std::chrono::steady_clock::now();
  canvas.clear(clearColor);
  document.DrawToCanvas(canvas);

  const auto presentStart = std::chrono::steady_clock::now();
  presentBitmapToWindow(deviceContext, bitmap, width, height);
  ::EndPaint(hwnd, &paintStruct);

  if (drawMs)
    *drawMs = std::chrono::duration<double, std::milli>(presentStart - drawStart).count();
  if (presentMs)
    *presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();

  return true;
}

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
#if !defined(GLINT_ENABLE_D3D12) || !GLINT_ENABLE_D3D12
enum class open_gl_init_result
{
  success,
  missing_window,
  get_dc_failed,
  pixel_format_failed,
  legacy_context_failed,
  gr_context_failed
};

// WGL keeps one current context per thread, and Skia issues its GL calls -
// including the ones that free its objects - on whatever context is current.
// Hosts (plugin DAWs, apps embedding a glint view) may render with their own
// GL context on the same thread, so glint makes its context current only
// around its own work and then puts the host's back.
class wgl_context_restorer
{
public:
  wgl_context_restorer()
    : mDC(::wglGetCurrentDC())
    , mRC(::wglGetCurrentContext())
  {
  }

  ~wgl_context_restorer()
  {
    // Nothing was current before: leave ours bound.  Unbinding would only
    // cost a flush, and whoever uses GL next makes their own context current.
    if (!mRC)
      return;
    if (::wglGetCurrentContext() != mRC || ::wglGetCurrentDC() != mDC)
      ::wglMakeCurrent(mDC, mRC);
  }

  wgl_context_restorer(const wgl_context_restorer&) = delete;
  wgl_context_restorer& operator=(const wgl_context_restorer&) = delete;

  /** Call before deleting `rc`: a deleted context cannot be made current. */
  void forget(HGLRC rc)
  {
    if (rc && mRC == rc)
    {
      mDC = nullptr;
      mRC = nullptr;
    }
  }

private:
  HDC   mDC;
  HGLRC mRC;
};

inline bool makeOpenGLContextCurrent(HDC glDC, HGLRC glRC)
{
  if (!glDC || !glRC)
    return false;
  if (::wglGetCurrentContext() == glRC && ::wglGetCurrentDC() == glDC)
    return true;
  return ::wglMakeCurrent(glDC, glRC) != FALSE;
}

inline void destroyOpenGLContext(
  HWND hwnd,
  HDC& glDC,
  HGLRC& glRC,
  sk_sp<GrDirectContext>& grContext,
  sk_sp<SkSurface>& gpuSurface,
  SkCanvas*& canvas)
{
  canvas = nullptr;

  {
    wgl_context_restorer restoreHostContext;
    // Skia frees its GL objects through the current context, so make ours
    // current first.  If that fails the objects die with the context:
    // abandon them rather than issue GL calls into someone else's context.
    const bool current = makeOpenGLContextCurrent(glDC, glRC);
    if (grContext && !current)
      grContext->abandonContext();
    gpuSurface.reset();
    grContext.reset();

    if (glRC)
    {
      restoreHostContext.forget(glRC);
      ::wglDeleteContext(glRC);   // also makes it not current
      glRC = nullptr;
    }
  }

  if (glDC && hwnd)
  {
    ::ReleaseDC(hwnd, glDC);
    glDC = nullptr;
  }
}

inline open_gl_init_result initializeOpenGLContext(
  HWND hwnd,
  HDC& glDC,
  HGLRC& glRC,
  sk_sp<GrDirectContext>& grContext,
  SkCanvas*& canvas,
  sk_sp<SkSurface>& gpuSurface)
{
  if (!hwnd)
    return open_gl_init_result::missing_window;

  // Set up our context and Skia on it, then hand the thread back to
  // whatever context the host had current.
  wgl_context_restorer restoreHostContext;

  glDC = ::GetDC(hwnd);
  if (!glDC)
    return open_gl_init_result::get_dc_failed;

  PIXELFORMATDESCRIPTOR pixelFormat = {};
  pixelFormat.nSize = sizeof(pixelFormat);
  pixelFormat.nVersion = 1;
  pixelFormat.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pixelFormat.iPixelType = PFD_TYPE_RGBA;
  pixelFormat.cColorBits = 32;
  pixelFormat.cDepthBits = 0;
  pixelFormat.cStencilBits = 8;   // Skia's stencil clips and path fills
  pixelFormat.iLayerType = PFD_MAIN_PLANE;

  const int format = ::ChoosePixelFormat(glDC, &pixelFormat);
  if (format == 0 || !::SetPixelFormat(glDC, format, &pixelFormat))
  {
    destroyOpenGLContext(hwnd, glDC, glRC, grContext, gpuSurface, canvas);
    return open_gl_init_result::pixel_format_failed;
  }

  HGLRC legacyContext = ::wglCreateContext(glDC);
  if (!legacyContext)
  {
    destroyOpenGLContext(hwnd, glDC, glRC, grContext, gpuSurface, canvas);
    return open_gl_init_result::legacy_context_failed;
  }

  ::wglMakeCurrent(glDC, legacyContext);

  typedef HGLRC (WINAPI* wglCreateContextAttribsARBProc)(HDC, HGLRC, const int*);
  auto createModernContext = reinterpret_cast<wglCreateContextAttribsARBProc>(
    ::wglGetProcAddress("wglCreateContextAttribsARB"));

  if (createModernContext)
  {
    static const int attribs[] = {
      0x2091, 3,
      0x2092, 3,
      0x9126, 0x00000002,
      0
    };

    HGLRC modernContext = createModernContext(glDC, nullptr, attribs);
    if (modernContext)
    {
      ::wglMakeCurrent(nullptr, nullptr);
      ::wglDeleteContext(legacyContext);
      glRC = modernContext;
      ::wglMakeCurrent(glDC, glRC);
    }
    else
    {
      glRC = legacyContext;
    }
  }
  else
  {
    glRC = legacyContext;
  }

  auto glInterface = GrGLInterfaces::MakeWin();
  grContext = GrDirectContexts::MakeGL(std::move(glInterface));
  if (!grContext)
  {
    destroyOpenGLContext(hwnd, glDC, glRC, grContext, gpuSurface, canvas);
    return open_gl_init_result::gr_context_failed;
  }

  return open_gl_init_result::success;
}

/** Drops the window surface, releasing it through our own context. */
inline void releaseOpenGLSurface(
  HDC glDC,
  HGLRC glRC,
  sk_sp<SkSurface>& gpuSurface,
  SkCanvas*& canvas)
{
  canvas = nullptr;
  if (!gpuSurface)
    return;

  wgl_context_restorer restoreHostContext;
  makeOpenGLContextCurrent(glDC, glRC);
  gpuSurface.reset();
}

inline bool recreateOpenGLSurface(
  int width,
  int height,
  HDC glDC,
  HGLRC glRC,
  sk_sp<GrDirectContext>& grContext,
  sk_sp<SkSurface>& gpuSurface,
  SkCanvas*& canvas)
{
  canvas = nullptr;

  // Release the old surface and flush pending work on our own context (not
  // whatever happens to be current), then restore the host's context.
  wgl_context_restorer restoreHostContext;
  const bool current = makeOpenGLContextCurrent(glDC, glRC);
  gpuSurface.reset();

  if (!current || !grContext || width <= 0 || height <= 0)
    return false;

  grContext->flushAndSubmit();

  static constexpr GrGLenum kGL_RGBA8 = 0x8058;

  GrGLFramebufferInfo framebufferInfo = {};
  framebufferInfo.fFBOID = 0;
  framebufferInfo.fFormat = kGL_RGBA8;

  // Tell Skia what the window's pixel format really has: a stencil buffer
  // Skia believes in but that does not exist breaks clips and path fills.
  int stencilBits = 0;
  PIXELFORMATDESCRIPTOR pixelFormat = {};
  const int format = ::GetPixelFormat(glDC);
  if (format > 0 && ::DescribePixelFormat(glDC, format, sizeof(pixelFormat), &pixelFormat))
    stencilBits = pixelFormat.cStencilBits;

  GrBackendRenderTarget renderTarget =
    GrBackendRenderTargets::MakeGL(width, height, 0, stencilBits, framebufferInfo);

  gpuSurface = SkSurfaces::WrapBackendRenderTarget(
    grContext.get(),
    renderTarget,
    kBottomLeft_GrSurfaceOrigin,
    kRGBA_8888_SkColorType,
    nullptr,
    nullptr);

  if (!gpuSurface)
    return false;

  canvas = gpuSurface->getCanvas();
  return true;
}

inline bool paintDocumentGpu(
  HWND hwnd,
  HDC glDC,
  HGLRC glRC,
  GrDirectContext& grContext,
  SkCanvas& canvas,
  glint_document& document,
  const SkColor clearColor,
  double* drawMs = nullptr,
  double* presentMs = nullptr)
{
  PAINTSTRUCT paintStruct = {};
  HDC deviceContext = ::BeginPaint(hwnd, &paintStruct);
  if (!deviceContext)
    return false;

  ::EndPaint(hwnd, &paintStruct);

  wgl_context_restorer restoreHostContext;
  if (!makeOpenGLContextCurrent(glDC, glRC))
    return false;

  const auto drawStart = std::chrono::steady_clock::now();
  canvas.clear(clearColor);
  document.DrawToCanvas(canvas);

  grContext.flush();
  grContext.submit(GrSyncCpu::kNo);

  const auto presentStart = std::chrono::steady_clock::now();
  ::SwapBuffers(glDC);

  if (drawMs)
    *drawMs = std::chrono::duration<double, std::milli>(presentStart - drawStart).count();
  if (presentMs)
    *presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();

  return true;
}

#endif // !GLINT_ENABLE_D3D12
#if defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
enum class direct3d_init_result
{
  success,
  missing_window,
  factory_failed,
  adapter_failed,
  device_failed,
  queue_failed,
  context_failed,
  swapchain_failed,
  fence_failed,
  fence_event_failed
};

inline void destroyDirect3DResources(
  HANDLE& fenceEvent,
  gr_cp<ID3D12Fence>& fence,
  gr_cp<IDXGISwapChain3>& swapChain,
  gr_cp<ID3D12CommandQueue>& queue,
  gr_cp<ID3D12Device>& device,
  gr_cp<IDXGIAdapter1>& adapter,
  sk_sp<GrDirectContext>& grContext,
  sk_sp<SkSurface>* surfaces,
  gr_cp<ID3D12Resource>* buffers,
  const int bufferCount)
{
  for (int index = 0; index < bufferCount; ++index)
  {
    surfaces[index].reset();
    buffers[index].reset(nullptr);
  }

  grContext.reset();
  swapChain.reset(nullptr);
  queue.reset(nullptr);
  device.reset(nullptr);
  adapter.reset(nullptr);
  fence.reset(nullptr);

  if (fenceEvent)
  {
    ::CloseHandle(fenceEvent);
    fenceEvent = nullptr;
  }
}

inline bool waitForDirect3DFence(HANDLE fenceEvent, ID3D12Fence* fence, uint64_t value)
{
  if (!fence || !fenceEvent)
    return false;

  if (fence->GetCompletedValue() >= value)
    return true;

  if (FAILED(fence->SetEventOnCompletion(value, fenceEvent)))
    return false;

  return WAIT_OBJECT_0 == ::WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
}

// Picks the first hardware adapter that supports D3D12 and creates its
// device. The device is created in the same call that tests support:
// creating a throwaway device first (pDevice = nullptr) loaded and set up the
// driver twice, ~150 ms of extra startup.
inline bool chooseHardwareAdapter(IDXGIFactory4* factory, gr_cp<IDXGIAdapter1>& adapter,
                                  gr_cp<ID3D12Device>& device)
{
  if (!factory)
    return false;

  adapter.reset(nullptr);
  device.reset(nullptr);

  for (UINT index = 0;; ++index)
  {
    gr_cp<IDXGIAdapter1> candidate;
    if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
      break;

    DXGI_ADAPTER_DESC1 description = {};
    if (FAILED(candidate->GetDesc1(&description)))
      continue;

    if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      continue;

    if (SUCCEEDED(::D3D12CreateDevice(candidate.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
      adapter = std::move(candidate);
      return true;
    }
  }

  return false;
}

// The window-independent part of the D3D12 setup: DXGI factory, adapter,
// device, command queue and Skia context. This is the slow part (the driver
// loads and initialises the device: ~250 ms), and it needs no window, so a
// host can run it on another thread while it shows its first frames with the
// CPU renderer (see glint_window_win32). createDirect3DSwapChain() finishes
// the setup for a window.
struct direct3d_device
{
  gr_cp<IDXGIFactory4>      factory;
  gr_cp<IDXGIAdapter1>      adapter;
  gr_cp<ID3D12Device>       device;
  gr_cp<ID3D12CommandQueue> queue;
  sk_sp<GrDirectContext>    grContext;
  direct3d_init_result      result = direct3d_init_result::factory_failed;
};

inline direct3d_device createDirect3DDevice()
{
  direct3d_device d;
  HRESULT factoryResult = E_FAIL;
#if defined(_DEBUG)
  // The DXGI debug layer comes with the optional "Graphics Tools" Windows
  // feature; without it the debug factory fails with
  // DXGI_ERROR_SDK_COMPONENT_MISSING.  Retry without the flag rather than
  // drop debug builds to the CPU renderer.
  factoryResult = ::CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&d.factory));
  if (FAILED(factoryResult))
    d.factory.reset(nullptr);
#endif
  if (FAILED(factoryResult))
    factoryResult = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory));
  if (FAILED(factoryResult))
    return d;

  if (!chooseHardwareAdapter(d.factory.get(), d.adapter, d.device))
  {
    d.result = direct3d_init_result::adapter_failed;
    return d;
  }

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  if (FAILED(d.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d.queue))))
  {
    d.result = direct3d_init_result::queue_failed;
    return d;
  }

  // Compiled shaders from the binary / disk instead of D3DCompile per shader.
  glint_d3d_shader_cache::install();

  GrD3DBackendContext backendContext{};
  backendContext.fAdapter = d.adapter;
  backendContext.fDevice = d.device;
  backendContext.fQueue = d.queue;
  // The context may be created on a worker thread: Ganesh's D3D backend has no
  // thread affinity, it only must not be used from two threads at once.
  d.grContext = GrDirectContext::MakeDirect3D(backendContext);
  d.result = d.grContext ? direct3d_init_result::success : direct3d_init_result::context_failed;
  return d;
}

// A DirectComposition visual showing a swapchain over a window's GDI content.
//
// The swapchain can be shown or hidden at any time, and the window's own GDI
// content (its redirection surface) shows when it is hidden. That lets a
// window switch between presenting through the swapchain and through GDI:
// the CPU renderer's first frames at startup, and live resizes, where
// Windows shows a GDI window's new size and content in the same frame while
// swapchain frames trail the border (see setPresentThroughGdi()).
struct direct3d_composition
{
  gr_cp<IDCompositionDesktopDevice> device;
  gr_cp<IDCompositionTarget>        target;
  gr_cp<IDCompositionVisual2>       visual;
  bool                              shown = false;

  bool create(HWND hwnd, IDXGISwapChain1* swapChain)
  {
    if (FAILED(::DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&device)))) return false;
    if (FAILED(device->CreateTargetForHwnd(hwnd, TRUE, &target)))           return false;
    if (FAILED(device->CreateVisual(&visual)))                               return false;
    if (FAILED(visual->SetContent(swapChain)))                               return false;
    shown = false;
    return true;
  }

  // Show the swapchain over the window. Call after a Present(), so what it
  // shows is a current frame.
  void show()
  {
    if (shown || !device) return;
    shown = SUCCEEDED(target->SetRoot(visual.get())) && SUCCEEDED(device->Commit());
  }

  // Stop showing it: the window's GDI content shows. Call after that content
  // was drawn.
  void hide()
  {
    if (!shown || !device) return;
    target->SetRoot(nullptr);
    device->Commit();
    shown = false;
  }

  void reset()
  {
    hide();
    visual.reset(nullptr);
    target.reset(nullptr);
    device.reset(nullptr);
  }
};

// Creates the swapchain, fence and fence event for `hwnd` on a device from
// createDirect3DDevice(). With `composition`, the swapchain is shown through
// DirectComposition (see direct3d_composition); otherwise it is an HWND
// swapchain.
inline direct3d_init_result createDirect3DSwapChain(
  HWND hwnd,
  IDXGIFactory4* factory,
  ID3D12Device* device,
  ID3D12CommandQueue* queue,
  gr_cp<IDXGISwapChain3>& swapChain,
  gr_cp<ID3D12Fence>& fence,
  HANDLE& fenceEvent,
  uint64_t* fenceValues,
  const int bufferCount,
  unsigned int& bufferIndex,
  direct3d_composition* composition = nullptr)
{
  if (!hwnd)
    return direct3d_init_result::missing_window;

  RECT windowRect = {};
  ::GetClientRect(hwnd, &windowRect);
  const UINT width = static_cast<UINT>(std::max<LONG>(windowRect.right - windowRect.left, 1));
  const UINT height = static_cast<UINT>(std::max<LONG>(windowRect.bottom - windowRect.top, 1));

  DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
  swapChainDesc.BufferCount = static_cast<UINT>(bufferCount);
  swapChainDesc.Width = width;
  swapChainDesc.Height = height;
  swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapChainDesc.SampleDesc.Count = 1;
  // Waitable swapchain with a one-frame queue (set below): each frame is drawn
  // only once the previous one was taken by the compositor, so what is shown
  // is at most one frame old. With the default queue of 3, a live resize
  // showed frames several window sizes behind the window border.
  swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  // While a live resize outruns rendering, DWM shows the last frame at its
  // own size instead of stretching it to the new window size (stretching
  // made the whole UI wobble when dragging the right / bottom edge).
  swapChainDesc.Scaling = DXGI_SCALING_NONE;

  gr_cp<IDXGISwapChain1> swapChain1;
  bool composed = false;
  if (composition)
  {
    // Composition swapchains require stretch scaling; the visual shows the
    // buffer 1:1 at its own size (clipped to the window), so nothing stretches.
    DXGI_SWAP_CHAIN_DESC1 compDesc = swapChainDesc;
    compDesc.Scaling   = DXGI_SCALING_STRETCH;
    compDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(factory->CreateSwapChainForComposition(queue, &compDesc, nullptr, &swapChain1))
        && composition->create(hwnd, swapChain1.get()))
      composed = true;
    else
    {
      composition->reset();
      swapChain1.reset(nullptr);
    }
  }
  if (!composed && FAILED(factory->CreateSwapChainForHwnd(
        queue,
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1)))
  {
    return direct3d_init_result::swapchain_failed;
  }

  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  if (FAILED(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain))))
    return direct3d_init_result::swapchain_failed;
  swapChain->SetMaximumFrameLatency(1);

  bufferIndex = swapChain->GetCurrentBackBufferIndex();

  for (int index = 0; index < bufferCount; ++index)
    fenceValues[index] = 0;

  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    return direct3d_init_result::fence_failed;

  fenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!fenceEvent)
    return direct3d_init_result::fence_event_failed;

  return direct3d_init_result::success;
}

inline direct3d_init_result initializeDirect3DContext(
  HWND hwnd,
  gr_cp<IDXGIAdapter1>& adapter,
  gr_cp<ID3D12Device>& device,
  gr_cp<ID3D12CommandQueue>& queue,
  gr_cp<IDXGISwapChain3>& swapChain,
  gr_cp<ID3D12Fence>& fence,
  HANDLE& fenceEvent,
  sk_sp<GrDirectContext>& grContext,
  uint64_t* fenceValues,
  const int bufferCount,
  unsigned int& bufferIndex,
  direct3d_composition* composition = nullptr)
{
  if (!hwnd)
    return direct3d_init_result::missing_window;

  direct3d_device d = createDirect3DDevice();
  if (d.result != direct3d_init_result::success)
    return d.result;

  adapter   = std::move(d.adapter);
  device    = std::move(d.device);
  queue     = std::move(d.queue);
  grContext = std::move(d.grContext);
  return createDirect3DSwapChain(hwnd, d.factory.get(), device.get(), queue.get(), swapChain, fence, fenceEvent,
                                 fenceValues, bufferCount, bufferIndex, composition);
}

inline bool recreateDirect3DSurfaces(
  int width,
  int height,
  gr_cp<IDXGISwapChain3>& swapChain,
  gr_cp<ID3D12Fence>& fence,
  HANDLE fenceEvent,
  sk_sp<GrDirectContext>& grContext,
  sk_sp<SkSurface>* surfaces,
  gr_cp<ID3D12Resource>* buffers,
  uint64_t* fenceValues,
  const int bufferCount,
  unsigned int& bufferIndex,
  SkCanvas*& canvas)
{
  canvas = nullptr;

  if (!swapChain || !fence || !grContext || width <= 0 || height <= 0)
  {
    for (int index = 0; index < bufferCount; ++index)
    {
      surfaces[index].reset();
      buffers[index].reset(nullptr);
    }
    return width <= 0 || height <= 0;
  }

  grContext->flush();
  grContext->submit(GrSyncCpu::kYes);

  for (int index = 0; index < bufferCount; ++index)
  {
    if (!waitForDirect3DFence(fenceEvent, fence.get(), fenceValues[index]))
      return false;

    surfaces[index].reset();
    buffers[index].reset(nullptr);
  }

  DXGI_SWAP_CHAIN_DESC1 currentDesc = {};
  swapChain->GetDesc1(&currentDesc);
  if (FAILED(swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_R8G8B8A8_UNORM,
                                      currentDesc.Flags)))
    return false;

  GrD3DTextureResourceInfo info(
    nullptr,
    nullptr,
    D3D12_RESOURCE_STATE_PRESENT,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    1,
    1,
    0);

  for (int index = 0; index < bufferCount; ++index)
  {
    if (FAILED(swapChain->GetBuffer(static_cast<UINT>(index), IID_PPV_ARGS(&buffers[index]))))
      return false;

    info.fResource = buffers[index];
    GrBackendRenderTarget backendRenderTarget(width, height, info);
    surfaces[index] = SkSurfaces::WrapBackendRenderTarget(
      grContext.get(),
      backendRenderTarget,
      kTopLeft_GrSurfaceOrigin,
      kRGBA_8888_SkColorType,
      nullptr,
      nullptr);

    if (!surfaces[index])
      return false;
  }

  bufferIndex = swapChain->GetCurrentBackBufferIndex();
  if (bufferIndex >= static_cast<unsigned int>(bufferCount))
    return false;

  canvas = surfaces[bufferIndex] ? surfaces[bufferIndex]->getCanvas() : nullptr;
  return canvas != nullptr;
}

inline SkSurface* acquireDirect3DBackbufferSurface(
  gr_cp<IDXGISwapChain3>& swapChain,
  gr_cp<ID3D12Fence>& fence,
  HANDLE fenceEvent,
  sk_sp<SkSurface>* surfaces,
  uint64_t* fenceValues,
  const int bufferCount,
  unsigned int& bufferIndex,
  SkCanvas*& canvas)
{
  canvas = nullptr;

  if (!swapChain || !fence)
    return nullptr;

  const uint64_t currentFenceValue = fenceValues[bufferIndex];
  bufferIndex = swapChain->GetCurrentBackBufferIndex();
  if (bufferIndex >= static_cast<unsigned int>(bufferCount))
    return nullptr;

  if (!waitForDirect3DFence(fenceEvent, fence.get(), fenceValues[bufferIndex]))
    return nullptr;

  fenceValues[bufferIndex] = currentFenceValue + 1;
  if (!surfaces[bufferIndex])
    return nullptr;

  canvas = surfaces[bufferIndex]->getCanvas();
  return surfaces[bufferIndex].get();
}

inline bool flushAndPresentDirect3D(
  sk_sp<GrDirectContext>& grContext,
  SkSurface* surface,
  gr_cp<IDXGISwapChain3>& swapChain,
  gr_cp<ID3D12CommandQueue>& queue,
  gr_cp<ID3D12Fence>& fence,
  const uint64_t fenceValue)
{
  if (!grContext || !surface || !swapChain || !queue || !fence)
    return false;

  GrFlushInfo flushInfo = {};
  grContext->flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, flushInfo);
  grContext->submit();

  // Signal even if Present failed so waits on fenceValue cannot hang.
  const bool presented = SUCCEEDED(swapChain->Present(1, 0));
  const bool signaled  = SUCCEEDED(queue->Signal(fence.get(), fenceValue));
  return presented && signaled;
}
#endif
#endif
}