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
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"
#include "include/gpu/ganesh/gl/win/GrGLMakeWinInterface.h"
#  if defined(SK_DIRECT3D)
#    include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#  endif
#endif

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
#include <d3d12.h>
#include <dxgi1_4.h>
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
enum class open_gl_init_result
{
  success,
  missing_window,
  get_dc_failed,
  pixel_format_failed,
  legacy_context_failed,
  gr_context_failed
};

inline void destroyOpenGLContext(
  HWND hwnd,
  HDC& glDC,
  HGLRC& glRC,
  sk_sp<GrDirectContext>& grContext,
  sk_sp<SkSurface>& gpuSurface,
  SkCanvas*& canvas)
{
  canvas = nullptr;
  gpuSurface.reset();
  grContext.reset();

  if (glRC)
  {
    ::wglMakeCurrent(nullptr, nullptr);
    ::wglDeleteContext(glRC);
    glRC = nullptr;
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
  gpuSurface.reset();

  if (!grContext || !glDC || !glRC || width <= 0 || height <= 0)
    return false;

  grContext->flushAndSubmit();
  ::wglMakeCurrent(glDC, glRC);

  static constexpr GrGLenum kGL_RGBA8 = 0x8058;

  GrGLFramebufferInfo framebufferInfo = {};
  framebufferInfo.fFBOID = 0;
  framebufferInfo.fFormat = kGL_RGBA8;

  GrBackendRenderTarget renderTarget =
    GrBackendRenderTargets::MakeGL(width, height, 0, 8, framebufferInfo);

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

  ::wglMakeCurrent(glDC, glRC);

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

inline bool chooseHardwareAdapter(IDXGIFactory4* factory, gr_cp<IDXGIAdapter1>& adapter)
{
  if (!factory)
    return false;

  adapter.reset(nullptr);

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

    if (SUCCEEDED(::D3D12CreateDevice(candidate.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
      adapter = std::move(candidate);
      return true;
    }
  }

  return false;
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
  unsigned int& bufferIndex)
{
  if (!hwnd)
    return direct3d_init_result::missing_window;

  UINT factoryFlags = 0;
#if defined(_DEBUG)
  factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

  gr_cp<IDXGIFactory4> factory;
  if (FAILED(::CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory))))
    return direct3d_init_result::factory_failed;

  if (!chooseHardwareAdapter(factory.get(), adapter))
    return direct3d_init_result::adapter_failed;

  if (FAILED(::D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    return direct3d_init_result::device_failed;

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
    return direct3d_init_result::queue_failed;

  GrD3DBackendContext backendContext{};
  backendContext.fAdapter = adapter;
  backendContext.fDevice = device;
  backendContext.fQueue = queue;
  grContext = GrDirectContext::MakeDirect3D(backendContext);
  if (!grContext)
    return direct3d_init_result::context_failed;

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

  gr_cp<IDXGISwapChain1> swapChain1;
  if (FAILED(factory->CreateSwapChainForHwnd(
        queue.get(),
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

  if (FAILED(swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_R8G8B8A8_UNORM, 0)))
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

  if (FAILED(swapChain->Present(1, 0)))
    return false;

  return SUCCEEDED(queue->Signal(fence.get(), fenceValue));
}
#endif
#endif
}