#pragma once

/**
 * glint_view_win32.hpp
 * Win32 embedded view host for glint_document.
 *
 * glint_view_win32 creates a child HWND inside an existing native parent and
 * owns the document, input routing, redraw invalidation, and CPU/GPU render
 * surface lifecycle for that embedded view.
 */

#include "../glint_view_base.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include "glint_renderer_backend_win32.hpp"
#include "glint_win32_host_shared.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

class glint_view_win32 final : public glint_view_base
{
public:
  static std::unique_ptr<glint_view_win32> create(const glint_view_options& options = {})
  {
    auto view = std::unique_ptr<glint_view_win32>(new glint_view_win32(options));
    if (!view->open())
      return nullptr;

    return view;
  }

  ~glint_view_win32() override
  {
    close();
  }

  void* nativeHandle() const override
  {
    return mHWND;
  }

  HWND hwnd() const
  {
    return mHWND;
  }

  bool isOpen() const
  {
    return mHWND != nullptr;
  }

  void resize(int width, int height) override
  {
    if (!mHWND)
      return;

    // width/height are LOGICAL (CSS) pixels. Refresh DPR from the current
    // monitor before converting: the window may have moved to a different
    // display or the system scaling may have changed since we last set mDpr.
    {
      const float freshDpr = deviceScaleForWindow(mHWND);
      if (freshDpr > 0.f)
        mDpr = freshDpr;
    }
    const float dpr = mDpr > 0.f ? mDpr : 1.f;
    const int physW = static_cast<int>(std::lround(static_cast<float>(width) * dpr));
    const int physH = static_cast<int>(std::lround(static_cast<float>(height) * dpr));

    ::SetWindowPos(
      mHWND,
      nullptr,
      0,
      0,
      physW,
      physH,
      SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
  }

  void requestRedraw() override
  {
    requestRedrawInternal();
  }

private:
  explicit glint_view_win32(const glint_view_options& options)
    : glint_view_base(options)
    , mParent(static_cast<HWND>(options.parent))
  {
  }

  static constexpr UINT kAnimTimer = 1;

  // Query the effective DPI for an HWND with a runtime fallback for pre-Win10
  // systems. Returns the device pixel ratio (e.g. 1.5 at 150% scale).
  //
  // Note: on Per-Monitor V2, a child HWND's DPI context is only updated by
  // the OS immediately before it posts WM_DPICHANGED_AFTERPARENT to the
  // child. If the top-level ancestor's WM_DPICHANGED handler resizes the
  // child synchronously (via SetWindowPos), the child's WM_SIZE fires
  // *before* its own DPI context has been refreshed, so GetDpiForWindow(child)
  // still returns the OLD DPI. We therefore walk up to the top-level ancestor
  // (which was just updated during its WM_DPICHANGED) and query that instead.
  // This keeps DPR in sync during live display-scaling changes.
  static float deviceScaleForWindow(HWND hwnd)
  {
    typedef UINT (WINAPI* GetDpiForWindowProc)(HWND);
    static GetDpiForWindowProc getDpiForWindow = [] {
      HMODULE h = ::GetModuleHandleW(L"user32.dll");
      if (!h)
        h = ::LoadLibraryW(L"user32.dll");
      return h ? reinterpret_cast<GetDpiForWindowProc>(::GetProcAddress(h, "GetDpiForWindow"))
               : nullptr;
    }();

    UINT dpi = 96;
    if (getDpiForWindow && hwnd)
    {
      HWND queryTarget = ::GetAncestor(hwnd, GA_ROOT);
      if (!queryTarget)
        queryTarget = hwnd;
      const UINT queried = getDpiForWindow(queryTarget);
      if (queried > 0)
        dpi = queried;
    }
    else
    {
      // Legacy fallback: use the primary screen's logical DPI.
      HDC hdc = ::GetDC(nullptr);
      if (hdc)
      {
        const int logicalDpi = ::GetDeviceCaps(hdc, LOGPIXELSX);
        if (logicalDpi > 0)
          dpi = static_cast<UINT>(logicalDpi);
        ::ReleaseDC(nullptr, hdc);
      }
    }

    return static_cast<float>(dpi) / 96.f;
  }

  static bool runtimeLoggingEnabled()
  {
#if defined(_DEBUG)
    return true;
#else
    static const bool enabled = [] {
      char value[8] = {};
      const DWORD length = ::GetEnvironmentVariableA("GLINT_ENABLE_RUNTIME_LOG", value, sizeof(value));
      return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
#endif
  }

  static bool telemetryEnabled()
  {
    static const bool enabled = [] {
      char value[8] = {};
      const DWORD length = ::GetEnvironmentVariableA("GLINT_ENABLE_TELEMETRY", value, sizeof(value));
      return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
  }

  static void appendRuntimeLogLine(const char* message)
  {
    char tempPath[MAX_PATH] = {};
    const DWORD tempPathLength = ::GetTempPathA(MAX_PATH, tempPath);
    if (tempPathLength == 0 || tempPathLength >= MAX_PATH)
      return;

    std::string logPath(tempPath);
    logPath += "glint_runtime.log";
    FILE* logFile = std::fopen(logPath.c_str(), "a");
    if (logFile)
    {
      std::fputs(message, logFile);
      std::fputc('\n', logFile);
      std::fclose(logFile);
    }
  }

  static void logRuntimeMessage(const char* message)
  {
    if (!runtimeLoggingEnabled())
      return;

    ::OutputDebugStringA(message);
    ::OutputDebugStringA("\n");

    appendRuntimeLogLine(message);
  }

  static void logTelemetryMessage(const char* message)
  {
    if (!telemetryEnabled())
      return;

    ::OutputDebugStringA(message);
    ::OutputDebugStringA("\n");

    if (runtimeLoggingEnabled())
      appendRuntimeLogLine(message);
  }

  void requestRedrawInternal()
  {
    mRedrawRequested = true;
    if (telemetryEnabled())
      ++mRedrawRequestCount;
    if (mHWND)
      glint_win32_host::invalidateWindow(mHWND);
  }

  void acknowledgePendingPaint() const
  {
    if (!mHWND)
      return;

    RECT updateRect = {};
    if (!::GetUpdateRect(mHWND, &updateRect, FALSE))
      return;

    PAINTSTRUCT paintStruct = {};
    HDC paintDeviceContext = ::BeginPaint(mHWND, &paintStruct);
    if (paintDeviceContext)
      ::EndPaint(mHWND, &paintStruct);
  }

  void updateTelemetry()
  {
    if (!telemetryEnabled() || !mDocument)
      return;

    const auto now = std::chrono::steady_clock::now();
    if (mLastTelemetryLogTime.time_since_epoch().count() == 0)
    {
      mLastTelemetryLogTime = now;
      mLastPaintCountSample = mPaintCount;
      mLastRedrawRequestCountSample = mRedrawRequestCount;
      mLastTimerWakeCountSample = mTimerWakeCount;
      mLastPaintDrawMsSample = mPaintDrawMsTotal;
      mLastPaintPresentMsSample = mPaintPresentMsTotal;
      return;
    }

    const double elapsedMs = std::chrono::duration<double, std::milli>(now - mLastTelemetryLogTime).count();
    if (elapsedMs < 500.0)
      return;

    const double elapsedSeconds = elapsedMs / 1000.0;
    const uint64_t paintDelta = mPaintCount - mLastPaintCountSample;
    const uint64_t redrawDelta = mRedrawRequestCount - mLastRedrawRequestCountSample;
    const uint64_t timerDelta = mTimerWakeCount - mLastTimerWakeCountSample;
    const double drawMsDelta = mPaintDrawMsTotal - mLastPaintDrawMsSample;
    const double presentMsDelta = mPaintPresentMsTotal - mLastPaintPresentMsSample;
    const double paintsPerSecond = elapsedSeconds > 0.0 ? static_cast<double>(paintDelta) / elapsedSeconds : 0.0;
    const double redrawsPerSecond = elapsedSeconds > 0.0 ? static_cast<double>(redrawDelta) / elapsedSeconds : 0.0;
    const double timerPerSecond = elapsedSeconds > 0.0 ? static_cast<double>(timerDelta) / elapsedSeconds : 0.0;
    const double avgDrawMs = paintDelta > 0 ? drawMsDelta / static_cast<double>(paintDelta) : 0.0;
    const double avgPresentMs = paintDelta > 0 ? presentMsDelta / static_cast<double>(paintDelta) : 0.0;

    char message[256] = {};
    std::snprintf(
      message,
      sizeof(message),
      "GLINT VIEW TELEMETRY: fps=%.1f frame_ms=%.2f draw=%.2f present=%.2f paint/s=%.1f req/s=%.1f timer/s=%.1f pending=%d",
      static_cast<double>(mDocument->getFPS()),
      static_cast<double>(mDocument->getFrameTimeMs()),
      avgDrawMs,
      avgPresentMs,
      paintsPerSecond,
      redrawsPerSecond,
      timerPerSecond,
      mRedrawRequested ? 1 : 0);
    logTelemetryMessage(message);

    mLastTelemetryLogTime = now;
    mLastPaintCountSample = mPaintCount;
    mLastRedrawRequestCountSample = mRedrawRequestCount;
    mLastTimerWakeCountSample = mTimerWakeCount;
    mLastPaintDrawMsSample = mPaintDrawMsTotal;
    mLastPaintPresentMsSample = mPaintPresentMsTotal;
  }

  bool open()
  {
    if (!mParent)
      return false;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS | CS_OWNDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"glint_view_win32";
    ::RegisterClassExW(&windowClass);

    // Compute DPI-derived physical size before window creation so WM_CREATE /
    // WM_SIZE see correct backing-store dimensions from the first frame.
    mDpr = deviceScaleForWindow(mParent);
    if (mDpr <= 0.f)
      mDpr = 1.f;

    // mW/mH hold the caller-supplied LOGICAL size. If the caller passed zero,
    // derive a logical fallback from the parent's client rect (which is in
    // physical px when the process is per-monitor-aware).
    if (mW <= 0 || mH <= 0)
    {
      RECT clientRect = {};
      ::GetClientRect(mParent, &clientRect);
      const int parentPhysW = static_cast<int>(clientRect.right - clientRect.left);
      const int parentPhysH = static_cast<int>(clientRect.bottom - clientRect.top);
      if (mW <= 0)
        mW = static_cast<int>(std::lround(static_cast<float>(parentPhysW) / mDpr));
      if (mH <= 0)
        mH = static_cast<int>(std::lround(static_cast<float>(parentPhysH) / mDpr));
    }

    mWpx = static_cast<int>(std::lround(static_cast<float>(mW) * mDpr));
    mHpx = static_cast<int>(std::lround(static_cast<float>(mH) * mDpr));

    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP;
    if (mOptions.visible)
      style |= WS_VISIBLE;

    mHWND = ::CreateWindowExW(
      0,
      windowClass.lpszClassName,
      L"",
      style,
      0,
      0,
      mWpx,
      mHpx,
      mParent,
      nullptr,
      ::GetModuleHandleW(nullptr),
      this);

    return mHWND != nullptr && mDocument != nullptr;
  }

  void close()
  {
    if (mHWND)
    {
      ::DestroyWindow(mHWND);
      mHWND = nullptr;
    }
  }

  void initDocument()
  {
    createDocument(
      [this]() {
        requestRedrawInternal();
      });

    mDocument->hwnd = mHWND;
    mDocument->devicePixelRatio = mDpr;
    mDocument->mCanvas.style.display = "flex";
    mDocument->mCanvas.style.flexDirection = "column";
    updateDocumentBounds();

    if (mOptions.onDocumentCreated)
      mOptions.onDocumentCreated(*mDocument);
  }

  void logRequestedBackend() const
  {
    char message[96] = {};
    std::snprintf(message, sizeof(message), "GLINT VIEW: requested backend = %s", glint_backend_name(requestedBackend()));
    logRuntimeMessage(message);
  }

  glint_backend requestedBackend() const
  {
    if (!shouldUseGpu())
      return glint_backend::CPU;

    return glint_resolve_backend(preferredBackend());
  }

  void logActiveBackend() const
  {
    char message[128] = {};
    std::snprintf(
      message,
      sizeof(message),
      "GLINT VIEW: active backend = %s (%s)",
      glint_backend_name(mActiveBackend),
      mRenderer && mRenderer->isGpu() ? "GPU" : "CPU");
    logRuntimeMessage(message);
  }

  bool activateFallbackRenderer(glint_backend failedBackend)
  {
    if (failedBackend == glint_backend::D3D11 || failedBackend == glint_backend::D3D12)
    {
      logRuntimeMessage("GLINT VIEW: falling back to OpenGL backend");
      if (activateRenderer(glint_backend::OpenGL))
        return true;
    }

    if (failedBackend != glint_backend::CPU)
      logRuntimeMessage("GLINT VIEW: falling back to CPU backend");

    if (!activateRenderer(glint_backend::CPU))
    {
      logRuntimeMessage("GLINT VIEW: CPU backend init failed");
      return false;
    }

    return true;
  }

  void logUnavailableBackendCompileGate(glint_backend requestedBackend) const
  {
    if (requestedBackend != glint_backend::D3D12)
      return;

#if !defined(GLINT_RENDER_GPU) || !GLINT_RENDER_GPU
    logRuntimeMessage("GLINT VIEW: D3D12 factory unavailable because GLINT_RENDER_GPU is disabled");
#elif !defined(GLINT_ENABLE_D3D12) || !GLINT_ENABLE_D3D12
    logRuntimeMessage("GLINT VIEW: D3D12 factory unavailable because GLINT_ENABLE_D3D12 is disabled");
#elif !defined(SK_DIRECT3D)
    logRuntimeMessage("GLINT VIEW: D3D12 factory unavailable because SK_DIRECT3D is not defined after Skia headers");
#endif
  }

  bool activateRenderer(glint_backend requestedBackend)
  {
    std::unique_ptr<glint_renderer_backend_win32> renderer = create_glint_renderer_backend_win32(requestedBackend);
    if (!renderer)
    {
      logUnavailableBackendCompileGate(requestedBackend);
      char message[128] = {};
      std::snprintf(
        message,
        sizeof(message),
        "GLINT VIEW: %s backend is not implemented for Win32 view host",
        glint_backend_name(requestedBackend));
      logRuntimeMessage(message);
      return false;
    }

    if (!renderer->initialize(mHWND))
    {
      const char* diagnostic = renderer->diagnostic();
      char message[192] = {};
      std::snprintf(
        message,
        sizeof(message),
        "GLINT VIEW: %s backend init failed%s%s",
        glint_backend_name(requestedBackend),
        diagnostic ? ": " : "",
        diagnostic ? diagnostic : "");
      logRuntimeMessage(message);
      return false;
    }

    if (!renderer->resize(mWpx, mHpx))
    {
      const char* diagnostic = renderer->diagnostic();
      char message[192] = {};
      std::snprintf(
        message,
        sizeof(message),
        "GLINT VIEW: %s backend resize failed%s%s",
        glint_backend_name(requestedBackend),
        diagnostic ? ": " : "",
        diagnostic ? diagnostic : "");
      logRuntimeMessage(message);
      renderer->shutdown();
      return false;
    }

    mRenderer = std::move(renderer);
    mActiveBackend = mRenderer->backend();

    if (mActiveBackend == glint_backend::OpenGL)
    {
      logRuntimeMessage("GLINT VIEW: GrDirectContext created");
      logRuntimeMessage("GLINT VIEW: GPU surface created (OpenGL)");
    }
    else if (mActiveBackend == glint_backend::D3D12)
    {
      logRuntimeMessage("GLINT VIEW: GrDirectContext created");
      logRuntimeMessage("GLINT VIEW: GPU surface created (D3D12)");
    }

    logActiveBackend();
    return true;
  }

  void initializeRenderer()
  {
    logRequestedBackend();

    if (activateRenderer(requestedBackend()))
      return;

    activateFallbackRenderer(requestedBackend());
  }

  void destroyRenderer()
  {
    if (mRenderer)
      mRenderer->shutdown();

    mRenderer.reset();
    mActiveBackend = glint_backend::CPU;
  }

  void recreateRendererSurface()
  {
    if (!mRenderer)
    {
      initializeRenderer();
      return;
    }

    if (mRenderer->resize(mWpx, mHpx))
    {
      mActiveBackend = mRenderer->backend();
      if (mActiveBackend == glint_backend::OpenGL)
        logRuntimeMessage("GLINT VIEW: GPU surface created (OpenGL)");
      else if (mActiveBackend == glint_backend::D3D12)
        logRuntimeMessage("GLINT VIEW: GPU surface created (D3D12)");
      return;
    }

    const glint_backend failedBackend = mRenderer->backend();
    const char* diagnostic = mRenderer->diagnostic();
    char message[192] = {};
    std::snprintf(
      message,
      sizeof(message),
      "GLINT VIEW: %s backend resize failed%s%s",
      glint_backend_name(failedBackend),
      diagnostic ? ": " : "",
      diagnostic ? diagnostic : "");
    logRuntimeMessage(message);

    if (failedBackend != glint_backend::CPU)
    {
      destroyRenderer();
      activateFallbackRenderer(failedBackend);
    }
  }

  void paint()
  {
    if (!mRenderer || !mDocument || !mHWND || mWpx <= 0 || mHpx <= 0)
    {
      acknowledgePendingPaint();
      return;
    }

    double drawMs = 0.0;
    double presentMs = 0.0;
    if (telemetryEnabled())
      ++mPaintCount;

    SkCanvas* canvas = mRenderer->beginFrame();
    if (!canvas)
    {
      const glint_backend failedBackend = mRenderer->backend();
      const char* diagnostic = mRenderer->diagnostic();
      char message[192] = {};
      std::snprintf(
        message,
        sizeof(message),
        "GLINT VIEW: %s beginFrame failed%s%s",
        glint_backend_name(failedBackend),
        diagnostic ? ": " : "",
        diagnostic ? diagnostic : "");
      logRuntimeMessage(message);

      if (failedBackend != glint_backend::CPU)
      {
        destroyRenderer();
        if (!activateFallbackRenderer(failedBackend))
        {
          acknowledgePendingPaint();
          return;
        }

        if (!mRenderer)
        {
          acknowledgePendingPaint();
          return;
        }

        canvas = mRenderer->beginFrame();
        if (!canvas)
        {
          acknowledgePendingPaint();
          return;
        }
      }
      else
      {
        acknowledgePendingPaint();
        return;
      }
    }

    const auto drawStart = std::chrono::steady_clock::now();
    mRedrawRequested = false;
    canvas->clear(clearColor());
    if (mDpr > 0.f && mDpr != 1.f)
    {
      // Document is authored in LOGICAL (CSS) pixels. Scale by the device
      // pixel ratio so drawing fills the physical backing store.
      canvas->save();
      canvas->scale(mDpr, mDpr);
      mDocument->DrawToCanvas(*canvas);
      canvas->restore();
    }
    else
    {
      mDocument->DrawToCanvas(*canvas);
    }
    mRenderer->endFrame();
    const auto presentStart = std::chrono::steady_clock::now();
    mRenderer->present();

    drawMs = std::chrono::duration<double, std::milli>(presentStart - drawStart).count();
    presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();

    if (telemetryEnabled())
    {
      mPaintDrawMsTotal += drawMs;
      mPaintPresentMsTotal += presentMs;
      updateTelemetry();
    }
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    auto* self = reinterpret_cast<glint_view_win32*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
      auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lp);
      self = static_cast<glint_view_win32*>(createStruct->lpCreateParams);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      self->mHWND = hwnd;
    }

    if (!self)
      return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
      case WM_CREATE:
        self->initDocument();
        self->initializeRenderer();
        ::SetTimer(hwnd, kAnimTimer, 16, nullptr);
        return 0;

      case WM_SIZE:
        // lp carries the client size in PHYSICAL pixels (we are per-monitor DPI aware).
        self->mWpx = LOWORD(lp);
        self->mHpx = HIWORD(lp);
        {
          // Refresh DPR defensively: WM_SIZE may arrive before
          // WM_DPICHANGED_AFTERPARENT in some host sequences (e.g. when the
          // host resizes our HWND immediately after its own WM_DPICHANGED).
          const float freshDpr = deviceScaleForWindow(hwnd);
          if (freshDpr > 0.f)
            self->mDpr = freshDpr;
          const float dpr = self->mDpr > 0.f ? self->mDpr : 1.f;
          self->mW = static_cast<int>(std::lround(static_cast<float>(self->mWpx) / dpr));
          self->mH = static_cast<int>(std::lround(static_cast<float>(self->mHpx) / dpr));
        }
        if (self->mDocument) self->mDocument->devicePixelRatio = self->mDpr;
        self->updateDocumentBounds();
        self->recreateRendererSurface();
        self->mRedrawRequested = true;
        glint_win32_host::invalidateWindow(hwnd);
        return 0;

      // A child window receives WM_DPICHANGED_AFTERPARENT when the effective
      // DPI of its monitor changes. (Top-level windows would get WM_DPICHANGED.)
      //
      // For WM_DPICHANGED_AFTERPARENT: the parent has already resized us to
      // the correct new physical client rect during its own WM_DPICHANGED,
      // so do NOT resize the HWND here (that would over/undershoot). Just
      // refresh mDpr and recompute the logical dimensions from the current
      // physical size. WM_SIZE already tried to refresh DPR from the root
      // ancestor, but we do it again defensively here in case message
      // ordering differed on some host.
      case WM_DPICHANGED_AFTERPARENT:
      {
        const float newDpr = deviceScaleForWindow(hwnd);
        if (newDpr > 0.f && std::fabs(newDpr - self->mDpr) > 1e-4f)
        {
          self->mDpr = newDpr;
          const float dpr = newDpr;
          self->mW = static_cast<int>(std::lround(static_cast<float>(self->mWpx) / dpr));
          self->mH = static_cast<int>(std::lround(static_cast<float>(self->mHpx) / dpr));
          if (self->mDocument) self->mDocument->devicePixelRatio = self->mDpr;
          self->updateDocumentBounds();
          self->recreateRendererSurface();
          self->mRedrawRequested = true;
          glint_win32_host::invalidateWindow(hwnd);
        }
        return 0;
      }

      // Reached only when this HWND is itself a top-level window (e.g. via
      // the standalone top-level host). Keep the logical size constant and
      // resize the HWND to the new physical size.
      case WM_DPICHANGED:
      {
        const float newDpr = deviceScaleForWindow(hwnd);
        if (newDpr > 0.f && std::fabs(newDpr - self->mDpr) > 1e-4f)
        {
          self->mDpr = newDpr;
          const int physW = static_cast<int>(std::lround(static_cast<float>(self->mW) * newDpr));
          const int physH = static_cast<int>(std::lround(static_cast<float>(self->mH) * newDpr));
          if (self->mDocument) self->mDocument->devicePixelRatio = self->mDpr;
          ::SetWindowPos(
            hwnd,
            nullptr,
            0,
            0,
            physW,
            physH,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_NOMOVE);
        }
        return 0;
      }

      case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTALLKEYS;

      case WM_PAINT:
        self->paint();
        return 0;

      case WM_ERASEBKGND:
        return 1;

      case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;

      case WM_DESTROY:
        ::KillTimer(hwnd, kAnimTimer);
        self->destroyRenderer();
        self->mDocument.reset();
        self->mHWND = nullptr;
        return 0;

      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK:
      {
        ::SetFocus(hwnd);
        ::SetCapture(hwnd);
        glint_win32_host::routeLeftButtonDown(self->mDocument.get(), self->mPrevX, self->mPrevY, wp, lp, self->mDpr);
        glint_win32_host::invalidateWindow(hwnd);
        return 0;
      }

      case WM_LBUTTONUP:
      {
        ::ReleaseCapture();
        glint_win32_host::routeLeftButtonUp(self->mDocument.get(), wp, lp, self->mDpr);
        glint_win32_host::invalidateWindow(hwnd);
        return 0;
      }

      case WM_RBUTTONDOWN:
      {
        glint_win32_host::routeRightButtonDown(self->mDocument.get(), wp, lp, self->mDpr);
        return 0;
      }

      case WM_RBUTTONUP:
      {
        glint_win32_host::routeRightButtonUp(self->mDocument.get(), wp, lp, self->mDpr);
        return 0;
      }

      case WM_MOUSEMOVE:
      {
        glint_win32_host::routeMouseMove(hwnd, self->mDocument.get(), self->mPrevX, self->mPrevY, wp, lp, self->mDpr);
        glint_win32_host::invalidateWindow(hwnd);
        return 0;
      }

      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
          return glint_win32_host::routeSetCursor(self->mDocument.get(), self->mPrevX, self->mPrevY);
        return ::DefWindowProcW(hwnd, msg, wp, lp);

      case WM_MOUSELEAVE:
        glint_win32_host::routeMouseLeave(self->mDocument.get());
        glint_win32_host::invalidateWindow(hwnd);
        return 0;

      case WM_MOUSEWHEEL:
      {
        glint_win32_host::routeMouseWheel(hwnd, self->mDocument.get(), wp, lp, self->mDpr);
        glint_win32_host::invalidateWindow(hwnd);
        return 0;
      }

      case WM_CHAR:
      {
        glint_win32_host::routeChar(self->mDocument.get(), wp);
        return 0;
      }

      case WM_KEYDOWN:
      {
        if (self->mDocument)
        {
          const glint_key_press keyPress = glint_win32_host::virtualKeyPress(wp);
          self->mDocument->OnKeyDown(keyPress);
        }
        glint_win32_host::invalidateWindow(hwnd);
        return 0;
      }

      case WM_KEYUP:
      {
        if (self->mDocument)
        {
          const glint_key_press keyPress = glint_win32_host::virtualKeyPress(wp);
          self->mDocument->OnKeyUp(keyPress);
        }
        return 0;
      }

      case WM_TIMER:
        if (wp == kAnimTimer)
        {
          if (!glint_win32_host::shouldScheduleTimerRedraw(self->mDocument.get(), self->mRedrawRequested))
            return 0;

          if (telemetryEnabled())
            ++self->mTimerWakeCount;

          glint_win32_host::invalidateWindow(hwnd);
          return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);

      default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
  }

  HWND  mParent = nullptr;
  HWND  mHWND = nullptr;
  bool  mRedrawRequested = false;
  uint64_t mPaintCount = 0;
  uint64_t mRedrawRequestCount = 0;
  uint64_t mTimerWakeCount = 0;
  double mPaintDrawMsTotal = 0.0;
  double mPaintPresentMsTotal = 0.0;
  uint64_t mLastPaintCountSample = 0;
  uint64_t mLastRedrawRequestCountSample = 0;
  uint64_t mLastTimerWakeCountSample = 0;
  double mLastPaintDrawMsSample = 0.0;
  double mLastPaintPresentMsSample = 0.0;
  std::chrono::steady_clock::time_point mLastTelemetryLogTime{};
  std::unique_ptr<glint_renderer_backend_win32> mRenderer;
};