#pragma once

/**
 * glint_window_win32.hpp
 * Win32 implementation of glint_window_base.
 *
 * Provides the complete Win32 plumbing that was previously duplicated between
 * glint_inspector_window and glint_demos_window:
 *
 *   - WNDCLASSEXW registration (once per class name via ::RegisterClassExW
 *     which is harmless if the class is already registered)
 *   - CreateWindowExW + message loop on a background thread
 *   - Shared Win32 renderer backend selection for opaque windows
 *   - CPU layered-window presentation via UpdateLayeredWindow when requested
 *   - Full mouse routing (down/up/move/drag/leave/wheel) → glint_document
 *   - Keyboard routing (WM_CHAR / WM_KEYDOWN / WM_KEYUP) → glint_document
 *   - WM_SIZE → updates root bounds + recreates bitmap (no scene-graph rebuild)
 *   - handleMessage() extension point for custom WM_ messages
 *
 * Subclasses only need to provide:
 *   windowClassName(), windowTitle(), buildUI()          [required]
 *   defaultWidth/Height(), clearColor(), bgColor()       [optional identity]
 *   onCreated(), onThreadStarted(), onThreadEnded(),
 *   onDestroyed(), afterRun(), handleMessage()           [optional hooks]
 */

#include "../glint_window_base.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include "glint_renderer_backend_win32.hpp"
#include "glint_win32_host_shared.hpp"
#include "glint_win32_surface_shared.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Bridge to glint_inspector_window — declared here, defined inline in
// inspector/window.hpp (which is always included downstream). This breaks the
// circular-include dependency while still letting the base class wire the
// Ctrl+Shift+I shortcut for every glint_window subclass automatically.
struct glint_insp_bridge {
  static void open (glint_document*);
  static void close(glint_document*);
  static bool isOpen(glint_document*);
  static void openAndEnableInspect(glint_document*);
};

class glint_window_win32 : public glint_window_base
{
protected:
  // ── Win32 state ───────────────────────────────────────────────────────────
  HWND              mHWND     = nullptr;
  std::atomic<HWND> mHWNDAtom{ nullptr };
  std::atomic<bool> mRedrawRequested{ false };
  uint64_t mPaintCount = 0;
  uint64_t mRedrawRequestCount = 0;
  uint64_t mTimerWakeCount = 0;
  double mPaintDrawMsTotal = 0.0;
  double mPaintPresentMsTotal = 0.0;
  double mRenderTransformDirectMsTotal = 0.0;
  double mRenderTransformOffscreenMsTotal = 0.0;
  double mRenderFilterInPlaceMsTotal = 0.0;
  double mRenderFilterOffscreenMsTotal = 0.0;
  double mRenderBackdropMsTotal = 0.0;
  double mRenderSelfPaintMsTotal = 0.0;
  double mRenderContentMsTotal = 0.0;
  double mRenderChildrenMsTotal = 0.0;
  double mRenderMaskMsTotal = 0.0;
  uint64_t mLastPaintCountSample = 0;
  uint64_t mLastRedrawRequestCountSample = 0;
  uint64_t mLastTimerWakeCountSample = 0;
  double mLastPaintDrawMsSample = 0.0;
  double mLastPaintPresentMsSample = 0.0;
  double mLastRenderTransformDirectMsSample = 0.0;
  double mLastRenderTransformOffscreenMsSample = 0.0;
  double mLastRenderFilterInPlaceMsSample = 0.0;
  double mLastRenderFilterOffscreenMsSample = 0.0;
  double mLastRenderBackdropMsSample = 0.0;
  double mLastRenderSelfPaintMsSample = 0.0;
  double mLastRenderContentMsSample = 0.0;
  double mLastRenderChildrenMsSample = 0.0;
  double mLastRenderMaskMsSample = 0.0;
  std::chrono::steady_clock::time_point mLastStatsTitleUpdate{};
  std::unordered_map<std::string, uint64_t> mRedrawByType;
  std::unordered_map<std::string, uint64_t> mLastRedrawByTypeSample;
  std::unordered_map<std::string, double> mRenderChildSubtreeMs;
  std::unordered_map<std::string, double> mLastRenderChildSubtreeMsSample;

  std::unique_ptr<glint_renderer_backend_win32> mRenderer;
  glint_backend mActiveBackend = glint_backend::CPU;

  // ── Optional Win32 override ───────────────────────────────────────────────

  /** Background brush colour for the Win32 window class. */
  virtual COLORREF bgColor()     const { return RGB(20, 20, 20); }

  /** Win32 window style passed to CreateWindowExW.
   *  Override with WS_POPUP for a fully borderless, titlebar-less window. */
  virtual DWORD    windowStyle() const { return WS_OVERLAPPEDWINDOW; }

  /** When true, WS_EX_LAYERED is added and painting uses UpdateLayeredWindow
   *  so per-pixel alpha in the Skia bitmap is composited by the DWM.
   *  clearColor() should have alpha = 0 for the transparent areas.
   *  Subclasses override to opt in. */
  virtual bool useTransparency() const { return false; }

  /** Return false to force CPU raster rendering even in a GLINT_RENDER_GPU
   *  build.  Opaque windows stay on the CPU renderer backend; layered windows
   *  continue to use the CPU bitmap + UpdateLayeredWindow path in paint().
   *  Useful for small popup windows (e.g. colour picker) where GPU spin-up
   *  latency is unacceptable. */
  virtual bool useGpu() const { return true; }

  /** Preferred renderer backend for opaque windows. Auto preserves the existing
   *  Win32 behavior and resolves to OpenGL when GPU rendering is enabled. */
  virtual glint_backend preferredBackend() const { return glint_backend::Auto; }

  /** When false, the window is NOT shown via ShowWindow(SW_SHOW) immediately
   *  after creation.  It remains hidden until the subclass explicitly calls
   *  ShowWindow(mHWND, SW_SHOW) (e.g. inside reopen()).  Override to false for
   *  pre-warmed popup windows (e.g. colour picker) that must be invisible until
   *  the user triggers them — avoids a brief flash at position 0,0 that occurs
   *  when the window is created with a dummy anchor rect and then hidden via a
   *  race-prone PostMessage(WM_SKUI_HIDE_CP). */
  virtual bool showOnCreate() const { return true; }

  /** Extended window style flags passed to CreateWindowExW (not including
   *  WS_EX_LAYERED which is added automatically when useTransparency()==true).
   *  Default: 0 — a normal top-level window that appears in the taskbar and
   *  Alt+Tab list and receives focus normally.
   *  Override to return WS_EX_TOOLWINDOW for transient popup windows (color
   *  picker, attribute list) that should NOT appear in the taskbar. */
  virtual DWORD windowExStyle() const { return 0u; }

  // Custom posted message used to trigger paint() on layered windows
  // (WM_PAINT is not delivered to WS_EX_LAYERED windows updated via
  // UpdateLayeredWindow, so InvalidateRect would be a no-op).
  static constexpr UINT WM_SKUI_REDRAW  = WM_USER + 200;

  // WM_TIMER id for the ~60 fps animation heartbeat.  Ensures CSS-transition
  // WM_PAINT chains never stall when Windows withholds its low-priority
  // background WM_PAINT (e.g. during a burst of WM_MOUSEMOVE messages).
  static constexpr UINT SKUI_ANIM_TIMER = 1;

  // Layered transparency is intentionally top-level-only. Embedded views stay
  // on the normal opaque child-HWND paint path and never use UpdateLayeredWindow.
  bool usesLayeredTransparency() const { return useTransparency(); }

  void scheduleWindowRedraw(HWND hwnd) const
  {
    glint_win32_host::scheduleRedraw(hwnd, usesLayeredTransparency(), WM_SKUI_REDRAW);
  }

  glint_backend requestedBackend() const
  {
    if (usesLayeredTransparency())
      return glint_backend::CPU;

    if (!useGpu())
      return glint_backend::CPU;

    return glint_resolve_backend(preferredBackend());
  }

  bool usesRendererBackends() const
  {
    return !usesLayeredTransparency();
  }

  // Query the effective DPI for an HWND with a runtime fallback for pre-Win10
  // systems. Returns the device pixel ratio (e.g. 1.5 at 150% scale).
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
      const UINT queried = getDpiForWindow(hwnd);
      if (queried > 0)
        dpi = queried;
    }
    else
    {
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

  // DPI for a point in virtual-screen coordinates (used before an HWND exists
  // so CreateWindowExW can be pre-sized for the target monitor).
  static float deviceScaleForPoint(int x, int y)
  {
    typedef HRESULT (WINAPI* GetDpiForMonitorProc)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorProc getDpiForMonitor = [] {
      HMODULE h = ::LoadLibraryW(L"shcore.dll");
      return h ? reinterpret_cast<GetDpiForMonitorProc>(::GetProcAddress(h, "GetDpiForMonitor"))
               : nullptr;
    }();

    const POINT point = { x, y };
    HMONITOR monitor = ::MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (getDpiForMonitor && monitor)
    {
      UINT dpiX = 96;
      UINT dpiY = 96;
      if (SUCCEEDED(getDpiForMonitor(monitor, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY)))
        return static_cast<float>(dpiX) / 96.f;
    }

    return deviceScaleForWindow(nullptr);
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

  void logRequestedBackend() const
  {
    char message[96] = {};
    std::snprintf(message, sizeof(message), "GLINT WINDOW: requested backend = %s", glint_backend_name(requestedBackend()));
    logRuntimeMessage(message);
  }

  void logActiveBackend() const
  {
    char message[128] = {};
    std::snprintf(
      message,
      sizeof(message),
      "GLINT WINDOW: active backend = %s (%s)",
      glint_backend_name(mActiveBackend),
      mRenderer && mRenderer->isGpu() ? "GPU" : "CPU");
    logRuntimeMessage(message);
  }

  bool activateFallbackRenderer(glint_backend failedBackend)
  {
    if (failedBackend == glint_backend::D3D11 || failedBackend == glint_backend::D3D12)
    {
      logRuntimeMessage("GLINT WINDOW: falling back to OpenGL backend");
      if (activateRenderer(glint_backend::OpenGL))
        return true;
    }

    if (failedBackend != glint_backend::CPU)
      logRuntimeMessage("GLINT WINDOW: falling back to CPU backend");

    if (!activateRenderer(glint_backend::CPU))
    {
      logRuntimeMessage("GLINT WINDOW: CPU backend init failed");
      return false;
    }

    return true;
  }

  void logUnavailableBackendCompileGate(glint_backend requestedBackend) const
  {
    if (requestedBackend != glint_backend::D3D12)
      return;

#if !defined(GLINT_RENDER_GPU) || !GLINT_RENDER_GPU
    logRuntimeMessage("GLINT WINDOW: D3D12 factory unavailable because GLINT_RENDER_GPU is disabled");
#elif !defined(GLINT_ENABLE_D3D12) || !GLINT_ENABLE_D3D12
    logRuntimeMessage("GLINT WINDOW: D3D12 factory unavailable because GLINT_ENABLE_D3D12 is disabled");
#elif !defined(SK_DIRECT3D)
    logRuntimeMessage("GLINT WINDOW: D3D12 factory unavailable because SK_DIRECT3D is not defined after Skia headers");
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
        "GLINT WINDOW: %s backend is not implemented for Win32 window host",
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
        "GLINT WINDOW: %s backend init failed%s%s",
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
        "GLINT WINDOW: %s backend resize failed%s%s",
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
      logRuntimeMessage("GLINT WINDOW: GrDirectContext created");
      logRuntimeMessage("GLINT WINDOW: GPU surface created (OpenGL)");
    }
    else if (mActiveBackend == glint_backend::D3D12)
    {
      logRuntimeMessage("GLINT WINDOW: GrDirectContext created");
      logRuntimeMessage("GLINT WINDOW: GPU surface created (D3D12)");
    }

    logActiveBackend();
    return true;
  }

  void initializeRenderer()
  {
    if (!usesRendererBackends())
    {
      recreateCpuSurface();
      return;
    }

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
    if (!usesRendererBackends())
    {
      recreateCpuSurface();
      return;
    }

    if (!mRenderer)
    {
      initializeRenderer();
      return;
    }

    if (mRenderer->resize(mWpx, mHpx))
    {
      mActiveBackend = mRenderer->backend();
      if (mActiveBackend == glint_backend::OpenGL)
        logRuntimeMessage("GLINT WINDOW: GPU surface created (OpenGL)");
      else if (mActiveBackend == glint_backend::D3D12)
        logRuntimeMessage("GLINT WINDOW: GPU surface created (D3D12)");
      return;
    }

    const char* diagnostic = mRenderer->diagnostic();
    char message[192] = {};
    std::snprintf(
      message,
      sizeof(message),
      "GLINT WINDOW: %s backend resize failed%s%s",
      glint_backend_name(mRenderer->backend()),
      diagnostic ? ": " : "",
      diagnostic ? diagnostic : "");
    logRuntimeMessage(message);

    if (mRenderer->backend() != glint_backend::CPU)
    {
      const glint_backend failedBackend = mRenderer->backend();
      destroyRenderer();
      activateFallbackRenderer(failedBackend);
    }
  }

  // ── Thread-management helpers ─────────────────────────────────────────────

  /** Returns true while the window thread is alive. */
  bool isRunning() const { return mRunning.load(); }

  /** Spawn the background thread and wait (max 3 s) for the HWND to appear. */
  void startThread()
  {
    mW = defaultWidth();
    mH = defaultHeight();
    // mWpx/mHpx are finalised inside createWindow() once the target monitor's
    // DPI is known. Seed them so a very early query returns something sane.
    mWpx = mW;
    mHpx = mH;
    mThread = std::thread([this]{ run(); });
    mThread.detach();
    for (int i = 0; i < 3000 && !mHWNDAtom.load(); ++i)
      ::Sleep(1);
  }

  /** Post WM_CLOSE to the window (triggers DestroyWindow → PostQuitMessage). */
  void stopThread()
  {
    if (HWND h = mHWNDAtom.load())
      ::PostMessage(h, WM_CLOSE, 0, 0);
  }

  // ── Extension point for custom messages ───────────────────────────────────

  /** Called by WndProc for any WM_ message not handled by the base.
   *  Return the LRESULT to send back to Windows.
   *  Return -1 to fall through to DefWindowProcW (default). */
  virtual LRESULT handleMessage(UINT /*msg*/, WPARAM /*wp*/, LPARAM /*lp*/)
  {
    return -1;
  }

  /** Called from the WM_KEYDOWN handler after the key press has been
   *  forwarded to the scene graph.  Override in subclasses to intercept
   *  global accelerators (e.g. Ctrl+S) that must work regardless of focus. */
  virtual void onKeyDown(const glint_key_press& /*kp*/) {}

private:

  static std::string describeRedrawRequester(const glint_element* requester)
  {
    if (!requester) return "unknown";

    std::ostringstream oss;
    const char* typeName = requester->typeName();
    oss << ((typeName && typeName[0] != '\0') ? typeName : "div");

    if (!requester->id.empty())
      oss << '#' << requester->id;

    if (!requester->className.empty())
    {
      std::istringstream classes(requester->className);
      std::string firstClass;
      if (classes >> firstClass)
        oss << '.' << firstClass;
    }

    return oss.str();
  }

  static std::string formatTopRedrawRequesters(
    const std::unordered_map<std::string, uint64_t>& redrawByType,
    const std::unordered_map<std::string, uint64_t>& lastSample,
    double elapsedSeconds)
  {
    if (elapsedSeconds <= 0.0) return "none";

    std::vector<std::pair<std::string, uint64_t>> topRequesters;
    topRequesters.reserve(redrawByType.size());

    for (const auto& [typeName, totalCount] : redrawByType)
    {
      const auto it = lastSample.find(typeName);
      const uint64_t prevCount = it != lastSample.end() ? it->second : 0;
      const uint64_t delta = totalCount - prevCount;
      if (delta == 0) continue;
      topRequesters.emplace_back(typeName, delta);
    }

    if (topRequesters.empty()) return "none";

    std::sort(topRequesters.begin(), topRequesters.end(),
      [](const auto& lhs, const auto& rhs) {
        if (lhs.second != rhs.second) return lhs.second > rhs.second;
        return lhs.first < rhs.first;
      });

    if (topRequesters.size() > 3)
      topRequesters.resize(3);

    std::ostringstream oss;
    for (std::size_t index = 0; index < topRequesters.size(); ++index)
    {
      if (index > 0) oss << ", ";
      const double perSecond = static_cast<double>(topRequesters[index].second) / elapsedSeconds;
      oss << topRequesters[index].first << ' ' << std::fixed << std::setprecision(1)
          << perSecond << "/s";
    }

    return oss.str();
  }

  static std::string formatTopRenderPhases(
    double transformDirectMs,
    double transformOffscreenMs,
    double filterInPlaceMs,
    double filterOffscreenMs,
    double backdropMs,
    double selfPaintMs,
    double contentMs,
    double childrenMs,
    double maskMs,
    uint64_t paintDelta)
  {
    if (paintDelta == 0) return "none";

    std::vector<std::pair<std::string, double>> phases = {
      {"txd", transformDirectMs / static_cast<double>(paintDelta)},
      {"txo", transformOffscreenMs / static_cast<double>(paintDelta)},
      {"fip", filterInPlaceMs / static_cast<double>(paintDelta)},
      {"fof", filterOffscreenMs / static_cast<double>(paintDelta)},
      {"bd", backdropMs / static_cast<double>(paintDelta)},
      {"self", selfPaintMs / static_cast<double>(paintDelta)},
      {"content", contentMs / static_cast<double>(paintDelta)},
      {"children", childrenMs / static_cast<double>(paintDelta)},
      {"mask", maskMs / static_cast<double>(paintDelta)}
    };

    phases.erase(
      std::remove_if(phases.begin(), phases.end(), [](const auto& entry) { return entry.second <= 0.01; }),
      phases.end());

    if (phases.empty()) return "none";

    std::sort(phases.begin(), phases.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.second != rhs.second) return lhs.second > rhs.second;
      return lhs.first < rhs.first;
    });

    if (phases.size() > 3)
      phases.resize(3);

    std::ostringstream oss;
    for (std::size_t index = 0; index < phases.size(); ++index)
    {
      if (index > 0) oss << ", ";
      oss << phases[index].first << ' ' << std::fixed << std::setprecision(2)
          << phases[index].second;
    }

    return oss.str();
  }

  static std::string formatTopChildSubtrees(
    const std::unordered_map<std::string, double>& childSubtreeMs,
    const std::unordered_map<std::string, double>& lastSample,
    uint64_t paintDelta)
  {
    if (paintDelta == 0) return "none";

    std::vector<std::pair<std::string, double>> topSubtrees;
    topSubtrees.reserve(childSubtreeMs.size());

    for (const auto& [label, totalMs] : childSubtreeMs)
    {
      const auto it = lastSample.find(label);
      const double previousMs = it != lastSample.end() ? it->second : 0.0;
      const double deltaMs = totalMs - previousMs;
      if (deltaMs <= 0.01) continue;
      topSubtrees.emplace_back(label, deltaMs / static_cast<double>(paintDelta));
    }

    if (topSubtrees.empty()) return "none";

    std::sort(topSubtrees.begin(), topSubtrees.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.second != rhs.second) return lhs.second > rhs.second;
      return lhs.first < rhs.first;
    });

    if (topSubtrees.size() > 3)
      topSubtrees.resize(3);

    std::ostringstream oss;
    for (std::size_t index = 0; index < topSubtrees.size(); ++index)
    {
      if (index > 0) oss << ", ";
      oss << topSubtrees[index].first << ' ' << std::fixed << std::setprecision(2)
          << topSubtrees[index].second;
    }

    return oss.str();
  }

  void updateStatsWindowTitle()
  {
    if (!telemetryEnabled()) return;
    if (!mHWND || !mOwnRoot) return;

    const auto now = std::chrono::steady_clock::now();
    if (mLastStatsTitleUpdate.time_since_epoch().count() == 0)
    {
      mLastStatsTitleUpdate = now;
      mLastPaintCountSample = mPaintCount;
      mLastRedrawRequestCountSample = mRedrawRequestCount;
      mLastTimerWakeCountSample = mTimerWakeCount;
      mLastPaintDrawMsSample = mPaintDrawMsTotal;
      mLastPaintPresentMsSample = mPaintPresentMsTotal;
      mLastRenderTransformDirectMsSample = mRenderTransformDirectMsTotal;
      mLastRenderTransformOffscreenMsSample = mRenderTransformOffscreenMsTotal;
      mLastRenderFilterInPlaceMsSample = mRenderFilterInPlaceMsTotal;
      mLastRenderFilterOffscreenMsSample = mRenderFilterOffscreenMsTotal;
      mLastRenderBackdropMsSample = mRenderBackdropMsTotal;
      mLastRenderSelfPaintMsSample = mRenderSelfPaintMsTotal;
      mLastRenderContentMsSample = mRenderContentMsTotal;
      mLastRenderChildrenMsSample = mRenderChildrenMsTotal;
      mLastRenderMaskMsSample = mRenderMaskMsTotal;
      mLastRenderChildSubtreeMsSample = mRenderChildSubtreeMs;
      return;
    }

    const double elapsedMs = std::chrono::duration<double, std::milli>(now - mLastStatsTitleUpdate).count();
    if (elapsedMs < 500.0) return;

    const double elapsedSeconds = elapsedMs / 1000.0;
    const uint64_t paintDelta = mPaintCount - mLastPaintCountSample;
    const uint64_t redrawDelta = mRedrawRequestCount - mLastRedrawRequestCountSample;
    const uint64_t timerDelta = mTimerWakeCount - mLastTimerWakeCountSample;
    const double drawMsDelta = mPaintDrawMsTotal - mLastPaintDrawMsSample;
    const double presentMsDelta = mPaintPresentMsTotal - mLastPaintPresentMsSample;
    const double transformDirectMsDelta = mRenderTransformDirectMsTotal - mLastRenderTransformDirectMsSample;
    const double transformOffscreenMsDelta = mRenderTransformOffscreenMsTotal - mLastRenderTransformOffscreenMsSample;
    const double filterInPlaceMsDelta = mRenderFilterInPlaceMsTotal - mLastRenderFilterInPlaceMsSample;
    const double filterOffscreenMsDelta = mRenderFilterOffscreenMsTotal - mLastRenderFilterOffscreenMsSample;
    const double backdropMsDelta = mRenderBackdropMsTotal - mLastRenderBackdropMsSample;
    const double selfPaintMsDelta = mRenderSelfPaintMsTotal - mLastRenderSelfPaintMsSample;
    const double contentMsDelta = mRenderContentMsTotal - mLastRenderContentMsSample;
    const double childrenMsDelta = mRenderChildrenMsTotal - mLastRenderChildrenMsSample;
    const double maskMsDelta = mRenderMaskMsTotal - mLastRenderMaskMsSample;

    const double paintsPerSecond = elapsedSeconds > 0.0 ? static_cast<double>(paintDelta) / elapsedSeconds : 0.0;
    const double redrawsPerSecond = elapsedSeconds > 0.0 ? static_cast<double>(redrawDelta) / elapsedSeconds : 0.0;
    const double timerPerSecond = elapsedSeconds > 0.0 ? static_cast<double>(timerDelta) / elapsedSeconds : 0.0;
    const double avgDrawMs = paintDelta > 0 ? drawMsDelta / static_cast<double>(paintDelta) : 0.0;
    const double avgPresentMs = paintDelta > 0 ? presentMsDelta / static_cast<double>(paintDelta) : 0.0;
    const std::string topRequesters = formatTopRedrawRequesters(
      mRedrawByType,
      mLastRedrawByTypeSample,
      elapsedSeconds);
    const std::string topRenderPhases = formatTopRenderPhases(
      transformDirectMsDelta,
      transformOffscreenMsDelta,
      filterInPlaceMsDelta,
      filterOffscreenMsDelta,
      backdropMsDelta,
      selfPaintMsDelta,
      contentMsDelta,
      childrenMsDelta,
      maskMsDelta,
      paintDelta);
    const std::string topChildSubtrees = formatTopChildSubtrees(
      mRenderChildSubtreeMs,
      mLastRenderChildSubtreeMsSample,
      paintDelta);

    wchar_t topRequestersWide[256] = {};
    ::MultiByteToWideChar(CP_UTF8, 0, topRequesters.c_str(), -1,
                          topRequestersWide,
                          static_cast<int>(sizeof(topRequestersWide) / sizeof(topRequestersWide[0])));
    wchar_t topRenderPhasesWide[256] = {};
    ::MultiByteToWideChar(CP_UTF8, 0, topRenderPhases.c_str(), -1,
                          topRenderPhasesWide,
                          static_cast<int>(sizeof(topRenderPhasesWide) / sizeof(topRenderPhasesWide[0])));
    wchar_t topChildSubtreesWide[256] = {};
    ::MultiByteToWideChar(CP_UTF8, 0, topChildSubtrees.c_str(), -1,
                          topChildSubtreesWide,
                          static_cast<int>(sizeof(topChildSubtreesWide) / sizeof(topChildSubtreesWide[0])));

    wchar_t title[1024] = {};
    std::swprintf(
      title,
      sizeof(title) / sizeof(title[0]),
      L"%ls | %.1f fps | %.2f ms | draw %.2f | present %.2f | phases %ls | subtrees %ls | paint/s %.1f | req/s %.1f | timer/s %.1f | top %ls | pending %d",
      windowTitle(),
      static_cast<double>(mOwnRoot->getFPS()),
      static_cast<double>(mOwnRoot->getFrameTimeMs()),
      avgDrawMs,
      avgPresentMs,
      topRenderPhasesWide,
      topChildSubtreesWide,
      paintsPerSecond,
      redrawsPerSecond,
      timerPerSecond,
      topRequestersWide,
      mRedrawRequested.load(std::memory_order_relaxed) ? 1 : 0);
    ::SetWindowTextW(mHWND, title);
    ::OutputDebugStringW(title);
    ::OutputDebugStringW(L"\n");

    char titleUtf8[1024] = {};
    const int titleUtf8Len = ::WideCharToMultiByte(
      CP_UTF8,
      0,
      title,
      -1,
      titleUtf8,
      static_cast<int>(sizeof(titleUtf8)),
      nullptr,
      nullptr);
    if (titleUtf8Len > 0)
    {
      std::fputs(titleUtf8, stdout);
      std::fputc('\n', stdout);
      std::fflush(stdout);
      logTelemetryMessage(titleUtf8);
    }

    mLastStatsTitleUpdate = now;
    mLastPaintCountSample = mPaintCount;
    mLastRedrawRequestCountSample = mRedrawRequestCount;
    mLastTimerWakeCountSample = mTimerWakeCount;
    mLastPaintDrawMsSample = mPaintDrawMsTotal;
    mLastPaintPresentMsSample = mPaintPresentMsTotal;
    mLastRenderTransformDirectMsSample = mRenderTransformDirectMsTotal;
    mLastRenderTransformOffscreenMsSample = mRenderTransformOffscreenMsTotal;
    mLastRenderFilterInPlaceMsSample = mRenderFilterInPlaceMsTotal;
    mLastRenderFilterOffscreenMsSample = mRenderFilterOffscreenMsTotal;
    mLastRenderBackdropMsSample = mRenderBackdropMsTotal;
    mLastRenderSelfPaintMsSample = mRenderSelfPaintMsTotal;
    mLastRenderContentMsSample = mRenderContentMsTotal;
    mLastRenderChildrenMsSample = mRenderChildrenMsTotal;
    mLastRenderMaskMsSample = mRenderMaskMsTotal;
    mLastRenderChildSubtreeMsSample = mRenderChildSubtreeMs;
    mLastRedrawByTypeSample = mRedrawByType;
  }

  void recreateSurface() override
  {
    recreateRendererSurface();
  }

  // ── Window creation (runs on the background thread) ───────────────────────
  bool createWindow()
  {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW existingClass = {};
    if (!::GetClassInfoExW(instance, windowClassName(), &existingClass))
    {
      WNDCLASSEXW wc   = {};
      wc.cbSize        = sizeof(wc);
      wc.style         = CS_HREDRAW | CS_VREDRAW;
      wc.lpfnWndProc   = WndProc;
      wc.hInstance     = instance;
      wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
      const HBRUSH backgroundBrush = ::CreateSolidBrush(bgColor());
      wc.hbrBackground = backgroundBrush;
      wc.lpszClassName = windowClassName();
      if (!::RegisterClassExW(&wc))
      {
        if (backgroundBrush)
          ::DeleteObject(backgroundBrush);
        return false;
      }
    }

    // Pre-size for the target monitor's DPI. CreateWindowExW at CW_USEDEFAULT
    // lands on the primary monitor; query its DPI so the window is physically
    // scaled to the caller's logical default{Width,Height}.
    POINT anchor = { 0, 0 };
    if (HMONITOR primary = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTOPRIMARY))
    {
      MONITORINFO info = { sizeof(MONITORINFO) };
      if (::GetMonitorInfoW(primary, &info))
        anchor = { info.rcWork.left, info.rcWork.top };
    }
    mDpr = deviceScaleForPoint(anchor.x, anchor.y);
    if (mDpr <= 0.f)
      mDpr = 1.f;

    mWpx = static_cast<int>(std::lround(static_cast<float>(mW) * mDpr));
    mHpx = static_cast<int>(std::lround(static_cast<float>(mH) * mDpr));

    mHWND = ::CreateWindowExW(
      windowExStyle() | (useTransparency() ? WS_EX_LAYERED : 0u),
      windowClassName(),
      windowTitle(),
      windowStyle(),
      CW_USEDEFAULT, CW_USEDEFAULT,
      mWpx, mHpx,
      nullptr, nullptr,
      instance,
      this    // passed to WM_NCCREATE as lpCreateParams
    );
    if (!mHWND) return false;

    if (showOnCreate())
    {
      ::ShowWindow(mHWND, SW_SHOW);
      ::UpdateWindow(mHWND);
    }
    mHWNDAtom = mHWND;
    return true;
  }

  // ── Root initialisation (called from WM_CREATE on the background thread) ──
  void initRoot()
  {
    // Reconcile logical/physical sizes with the client rect Windows actually
    // gave us. Refresh DPR in case the HWND ended up on a monitor that
    // differs from the primary we queried during createWindow().
    mDpr = deviceScaleForWindow(mHWND);
    if (mDpr <= 0.f)
      mDpr = 1.f;

    RECT cr; ::GetClientRect(mHWND, &cr);
    if (cr.right  > 0) mWpx = cr.right;
    if (cr.bottom > 0) mHpx = cr.bottom;
    mW = static_cast<int>(std::lround(static_cast<float>(mWpx) / mDpr));
    mH = static_cast<int>(std::lround(static_cast<float>(mHpx) / mDpr));

    const glint_rect bounds(0.f, 0.f, static_cast<float>(mW), static_cast<float>(mH));

    mOwnRoot = std::make_unique<glint_document>(
      bounds, nullptr,
      [this] {
        mRedrawRequested.store(true, std::memory_order_relaxed);
        if (telemetryEnabled()) ++mRedrawRequestCount;
        if (HWND h = mHWNDAtom.load())
          scheduleWindowRedraw(h);
      }
    );
    if (telemetryEnabled())
    {
      mOwnRoot->setDetailedRedrawReporter([this](glint_element* requester) {
        if (!requester) return;
        ++mRedrawByType[describeRedrawRequester(requester)];
      });
    }

    // Stamp the HWND on the root so components (labels, inputs) can open
    // Win32 context menus via TrackPopupMenu even when mpG is nullptr.
    mOwnRoot->hwnd = mHWND;
    mOwnRoot->devicePixelRatio = mDpr;

    // Vertical flex column so children stack top-to-bottom.
    mOwnRoot->mCanvas.style.display       = "flex";
    mOwnRoot->mCanvas.style.flexDirection = "column";

    // Ctrl+Shift+I — toggle the inspector for this window's root.
    // Ctrl+Shift+C — open and immediately activate element-picker (crosshair) mode.
    // Every glint_window subclass gets these for free; set mOwnRoot->skipInspectMode
    // to control whether the inspector can target this window's own UI.
    mOwnRoot->onGlobalKeyDown = [this](const glint_key_press& k) -> bool {
      if (k.ctrl && k.shift && k.vk == 'I') {
        glint_document* root = mOwnRoot.get();
        if (glint_insp_bridge::isOpen(root))
          glint_insp_bridge::close(root);
        else
          glint_insp_bridge::open(root);
        return true;
      }
      if (k.ctrl && k.shift && k.vk == 'C') {
        glint_insp_bridge::openAndEnableInspect(mOwnRoot.get());
        return true;
      }
      return false;
    };
  }

  // ── Update root canvas bounds on WM_SIZE (no scene-graph rebuild) ─────────
  void updateRootBounds()
  {
    if (!mOwnRoot) return;
    glint_win32_surface::updateDocumentBounds(*mOwnRoot, mW, mH);
  }

  void paintLayeredTransparentCpu(double* drawMs = nullptr, double* presentMs = nullptr)
  {
    const auto drawStart = std::chrono::steady_clock::now();
    mCanvas->clear(clearColor());
    if (mDpr > 0.f && mDpr != 1.f)
    {
      mCanvas->save();
      mCanvas->scale(mDpr, mDpr);
      mOwnRoot->DrawToCanvas(*mCanvas);
      mCanvas->restore();
    }
    else
    {
      mOwnRoot->DrawToCanvas(*mCanvas);
    }
    const auto presentStart = std::chrono::steady_clock::now();

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = mWpx;
    bitmapInfo.bmiHeader.biHeight = -mHpx;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HDC screenDeviceContext = ::GetDC(nullptr);
    HDC memoryDeviceContext = ::CreateCompatibleDC(screenDeviceContext);
    void* bits = nullptr;
    HBITMAP bitmapHandle = ::CreateDIBSection(memoryDeviceContext, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmapHandle && bits)
    {
      ::memcpy(bits, mBitmap.getPixels(), static_cast<size_t>(mWpx) * mHpx * 4);
      HBITMAP oldBitmap = static_cast<HBITMAP>(::SelectObject(memoryDeviceContext, bitmapHandle));
      POINT sourcePoint = { 0, 0 };
      SIZE windowSize = { static_cast<LONG>(mWpx), static_cast<LONG>(mHpx) };
      RECT windowRect = {};
      ::GetWindowRect(mHWND, &windowRect);
      POINT destinationPoint = { windowRect.left, windowRect.top };
      BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
      ::UpdateLayeredWindow(
        mHWND,
        screenDeviceContext,
        &destinationPoint,
        &windowSize,
        memoryDeviceContext,
        &sourcePoint,
        0,
        &blend,
        ULW_ALPHA);
      ::SelectObject(memoryDeviceContext, oldBitmap);
      ::DeleteObject(bitmapHandle);
    }
    ::DeleteDC(memoryDeviceContext);
    ::ReleaseDC(nullptr, screenDeviceContext);

    if (drawMs)
      *drawMs = std::chrono::duration<double, std::milli>(presentStart - drawStart).count();
    if (presentMs)
      *presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
  }

  // ── Background thread entry point ─────────────────────────────────────────
  void run()
  {
    mRunning = true;
    if (!createWindow()) { mRunning = false; return; }
    // WM_CREATE fires synchronously inside CreateWindowExW above, so
    // initRoot() + buildUI() + onCreated() have already completed by here.

    onThreadStarted();

    MSG msg;
    while (::GetMessage(&msg, nullptr, 0, 0) > 0)
    {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
    }

    onThreadEnded();
    mHWNDAtom = nullptr;
    mRunning  = false;
    afterRun(); // inspector uses this to delete this
  }

  // ── Rendering ─────────────────────────────────────────────────────────────
  void paint()
  {
    if (!mOwnRoot) return;

    double drawMs = 0.0;
    double presentMs = 0.0;
    if (telemetryEnabled())
      glint_element::resetRenderTimingProfile();

    if (telemetryEnabled()) ++mPaintCount;
    mRedrawRequested.store(false, std::memory_order_relaxed);

    if (usesLayeredTransparency())
    {
      if (!mCanvas) return;
      paintLayeredTransparentCpu(&drawMs, &presentMs);
    }
    else
    {
      if (!mRenderer) return;

      SkCanvas* canvas = mRenderer->beginFrame();
      if (!canvas)
      {
        const glint_backend failedBackend = mRenderer->backend();
        char message[160] = {};
        std::snprintf(
          message,
          sizeof(message),
          "GLINT WINDOW: %s backend beginFrame failed%s%s",
          glint_backend_name(failedBackend),
          mRenderer->diagnostic() ? ": " : "",
          mRenderer->diagnostic() ? mRenderer->diagnostic() : "");
        logRuntimeMessage(message);

        if (failedBackend != glint_backend::CPU)
        {
          destroyRenderer();
          if (activateFallbackRenderer(failedBackend) && mRenderer)
            canvas = mRenderer->beginFrame();
        }

        if (!canvas)
          return;
      }

      const auto drawStart = std::chrono::steady_clock::now();
      canvas->clear(clearColor());
      if (mDpr > 0.f && mDpr != 1.f)
      {
        canvas->save();
        canvas->scale(mDpr, mDpr);
        mOwnRoot->DrawToCanvas(*canvas);
        canvas->restore();
      }
      else
      {
        mOwnRoot->DrawToCanvas(*canvas);
      }
      mRenderer->endFrame();
      const auto presentStart = std::chrono::steady_clock::now();
      mRenderer->present();

      drawMs = std::chrono::duration<double, std::milli>(presentStart - drawStart).count();
      presentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
    }

    if (telemetryEnabled())
    {
      const auto renderProfile = glint_element::snapshotRenderTimingProfile();
      mPaintDrawMsTotal += drawMs;
      mPaintPresentMsTotal += presentMs;
      mRenderTransformDirectMsTotal += renderProfile.transformDirectMs;
      mRenderTransformOffscreenMsTotal += renderProfile.transformOffscreenMs;
      mRenderFilterInPlaceMsTotal += renderProfile.filterInPlaceMs;
      mRenderFilterOffscreenMsTotal += renderProfile.filterOffscreenMs;
      mRenderBackdropMsTotal += renderProfile.backdropMs;
      mRenderSelfPaintMsTotal += renderProfile.selfPaintMs;
      mRenderContentMsTotal += renderProfile.contentMs;
      mRenderChildrenMsTotal += renderProfile.childrenMs;
      mRenderMaskMsTotal += renderProfile.maskMs;
      for (const auto& [label, elapsedMs] : renderProfile.childSubtreeMs)
        mRenderChildSubtreeMs[label] += elapsedMs;

      updateStatsWindowTitle();
    }
  }

  // ── Win32 WndProc ─────────────────────────────────────────────────────────
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    glint_window_win32* self = nullptr;

    if (msg == WM_NCCREATE)
    {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      self     = static_cast<glint_window_win32*>(cs->lpCreateParams);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      self->mHWND = hwnd;
    }
    else
    {
      self = reinterpret_cast<glint_window_win32*>(
                 ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
    // ── Lifecycle ────────────────────────────────────────────────────────────
    case WM_CREATE:
      self->initRoot();
      self->buildUI();
      self->initializeRenderer();
      self->onCreated();
      if (self->usesLayeredTransparency()) self->paint();   // layered windows skip WM_PAINT
      // Heartbeat timer — fires every ~16 ms so CSS transitions never stall
      // in the WM_PAINT chain.  WM_PAINT is a low-priority "background" message
      // that Windows only synthesises when the message queue is otherwise empty.
      // Without this, a transition that calls InvalidateRect from inside
      // DrawToCanvas may not receive its follow-up WM_PAINT if the queue happens
      // to be momentarily non-empty (e.g. a burst of WM_MOUSEMOVE messages).
      ::SetTimer(hwnd, SKUI_ANIM_TIMER, 16, nullptr);
      return 0;

    case WM_SIZE:
      // lp carries client size in PHYSICAL pixels (per-monitor DPI aware).
      self->mWpx = LOWORD(lp);
      self->mHpx = HIWORD(lp);
      {
        const float dpr = self->mDpr > 0.f ? self->mDpr : 1.f;
        self->mW = static_cast<int>(std::lround(static_cast<float>(self->mWpx) / dpr));
        self->mH = static_cast<int>(std::lround(static_cast<float>(self->mHpx) / dpr));
      }
      if (self->mOwnRoot) self->mOwnRoot->devicePixelRatio = self->mDpr;
      self->updateRootBounds();
      self->recreateSurface();
      self->mRedrawRequested.store(true, std::memory_order_relaxed);
      if (self->usesLayeredTransparency()) self->paint();
      else
      {
        // Invalidate then immediately force a synchronous WM_PAINT via
        // UpdateWindow.  Without this, the WM_PAINT is queued as a
        // low-priority background message and is starved during the Win32
        // live-resize modal loop (rapid WM_SIZE bursts drain the queue before
        // WM_PAINT can fire), making the resized scene lag behind the window
        // frame.  UpdateWindow sends WM_PAINT synchronously so each WM_SIZE
        // produces exactly one rendered frame at the new size.
        glint_win32_host::invalidateWindow(hwnd);
        ::UpdateWindow(hwnd);
      }
      return 0;

    // Top-level windows receive WM_DPICHANGED when the effective DPI of their
    // monitor changes (e.g. the user drags the window onto a 150% display).
    // lp is a RECT* with the suggested new window rect in physical pixels.
    case WM_DPICHANGED:
    {
      const float newDpr = static_cast<float>(HIWORD(wp)) / 96.f;
      if (newDpr > 0.f && std::fabs(newDpr - self->mDpr) > 1e-4f)
        self->mDpr = newDpr;
      if (self->mOwnRoot) self->mOwnRoot->devicePixelRatio = self->mDpr;

      if (auto* suggested = reinterpret_cast<RECT*>(lp))
      {
        ::SetWindowPos(
          hwnd,
          nullptr,
          suggested->left,
          suggested->top,
          suggested->right  - suggested->left,
          suggested->bottom - suggested->top,
          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
      }
      return 0;
    }

    case WM_PAINT:
      self->paint();
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      ::KillTimer(hwnd, SKUI_ANIM_TIMER);
      self->onDestroyed();
      self->destroyRenderer();
      ::PostQuitMessage(0);
      return 0;

    // ── Mouse ────────────────────────────────────────────────────────────────
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
      ::SetFocus(hwnd);
      ::SetCapture(hwnd);
      glint_win32_host::routeLeftButtonDown(self->mOwnRoot.get(), self->mPrevX, self->mPrevY, wp, lp, self->mDpr);
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    case WM_LBUTTONUP:
    {
      ::ReleaseCapture();
      glint_win32_host::routeLeftButtonUp(self->mOwnRoot.get(), wp, lp, self->mDpr);
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    case WM_RBUTTONDOWN:
    {
      glint_win32_host::routeRightButtonDown(self->mOwnRoot.get(), wp, lp, self->mDpr);
      return 0;
    }

    case WM_RBUTTONUP:
    {
      glint_win32_host::routeRightButtonUp(self->mOwnRoot.get(), wp, lp, self->mDpr);
      return 0;
    }

    case WM_MBUTTONDOWN:
    {
      ::SetCapture(hwnd);
      glint_win32_host::routeMiddleButtonDown(self->mOwnRoot.get(), self->mPrevX, self->mPrevY, wp, lp, self->mDpr);
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    case WM_MBUTTONUP:
    {
      ::ReleaseCapture();
      glint_win32_host::routeMiddleButtonUp(self->mOwnRoot.get(), wp, lp, self->mDpr);
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    case WM_MOUSEMOVE:
    {
      glint_win32_host::routeMouseMove(hwnd, self->mOwnRoot.get(), self->mPrevX, self->mPrevY, wp, lp, self->mDpr);
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT)
        return glint_win32_host::routeSetCursor(self->mOwnRoot.get(), self->mPrevX, self->mPrevY);
      return ::DefWindowProcW(hwnd, msg, wp, lp);

    case WM_MOUSELEAVE:
      glint_win32_host::routeMouseLeave(self->mOwnRoot.get());
      glint_win32_host::invalidateWindow(hwnd);
      return 0;

    case WM_MOUSEWHEEL:
    {
      glint_win32_host::routeMouseWheel(hwnd, self->mOwnRoot.get(), wp, lp, self->mDpr);
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    case WM_MOUSEHWHEEL:
    {
      glint_win32_host::routeMouseWheelH(hwnd, self->mOwnRoot.get(), wp, lp, self->mDpr);
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    // ── Keyboard ─────────────────────────────────────────────────────────────
    case WM_CHAR:
    {
      glint_win32_host::routeChar(self->mOwnRoot.get(), wp);
      return 0;
    }

    case WM_KEYDOWN:
    {
      if (self->mOwnRoot)
      {
        const glint_key_press kp = glint_win32_host::virtualKeyPress(wp);
        self->mOwnRoot->OnKeyDown(kp);
        self->onKeyDown(kp);   // subclass accelerator hook
      }
      glint_win32_host::invalidateWindow(hwnd);
      return 0;
    }

    case WM_KEYUP:
    {
      if (self->mOwnRoot)
      {
        const glint_key_press kp = glint_win32_host::virtualKeyPress(wp);
        self->mOwnRoot->OnKeyUp(kp);
      }
      return 0;
    }

    // ── Layered-window repaint ────────────────────────────────────────────────
    case WM_SKUI_REDRAW:
      self->paint();
      return 0;

    // ── Animation heartbeat ──────────────────────────────────────────────────
    // Fires every ~16 ms.  Queues a WM_PAINT (or a direct repaint for layered
    // windows) so CSS transitions keep advancing even if the message queue
    // is briefly non-empty and Windows withholds its background WM_PAINT.
    case WM_TIMER:
      if (wp == SKUI_ANIM_TIMER)
      {
        if (!glint_win32_host::shouldScheduleTimerRedraw(
              self->mOwnRoot.get(),
              self->mRedrawRequested.load(std::memory_order_relaxed)))
          return 0;

        if (telemetryEnabled()) ++self->mTimerWakeCount;

        self->scheduleWindowRedraw(hwnd);
        return 0;
      }
      // Unknown timer id — give subclasses a chance to handle it.
      {
        LRESULT r = self->handleMessage(msg, wp, lp);
        if (r != -1) return r;
      }
      return ::DefWindowProcW(hwnd, msg, wp, lp);

    // ── Extension point ───────────────────────────────────────────────────────
    default:
    {
      LRESULT r = self->handleMessage(msg, wp, lp);
      if (r != -1) return r;
      return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    }
  }
};

// ── glint_platform Win32 implementations ─────────────────────────────────────
// Defined here (inline) because <windows.h> is already in scope.
// Declared in platform/glint_platform.hpp.

#include "../glint_platform.hpp"

namespace glint_platform {

inline void setClipboardText(const std::string& utf8)
{
  if (!::OpenClipboard(nullptr)) return;
  ::EmptyClipboard();
  const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (wlen > 0)
  {
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(WCHAR));
    if (h)
    {
      WCHAR* p = static_cast<WCHAR*>(::GlobalLock(h));
      if (p) { ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, p, wlen); ::GlobalUnlock(h); }
      ::SetClipboardData(CF_UNICODETEXT, h);
    }
  }
  ::CloseClipboard();
}

inline std::string getClipboardText()
{
  if (!::OpenClipboard(nullptr)) return {};
  std::string result;
  HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
  if (h)
  {
    const WCHAR* p = static_cast<const WCHAR*>(::GlobalLock(h));
    if (p)
    {
      const int len = ::WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
      if (len > 1)
      {
        result.resize(static_cast<size_t>(len - 1));
        ::WideCharToMultiByte(CP_UTF8, 0, p, -1, &result[0], len, nullptr, nullptr);
      }
      ::GlobalUnlock(h);
    }
  }
  ::CloseClipboard();
  return result;
}

inline int showContextMenu(int screenX, int screenY,
                           const std::vector<std::pair<int, std::string>>& items,
                           const std::vector<int>& disabledIds,
                           const std::vector<int>& checkedIds)
{
  HMENU hMenu = ::CreatePopupMenu();
  if (!hMenu) return 0;

  for (auto& [id, label] : items)
  {
    if (id == 0 && label == "-")
    {
      ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    else
    {
      const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, nullptr, 0);
      std::wstring wlabel(static_cast<size_t>(wlen > 0 ? wlen : 1), L'\0');
      if (wlen > 0) ::MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, &wlabel[0], wlen);
      const bool disabled =
        std::find(disabledIds.begin(), disabledIds.end(), id) != disabledIds.end();
      const bool checked =
        std::find(checkedIds.begin(), checkedIds.end(), id) != checkedIds.end();
      UINT flags = MF_STRING | (disabled ? MF_GRAYED : MF_ENABLED) | (checked ? MF_CHECKED : 0u);
      ::AppendMenuW(hMenu, flags, static_cast<UINT_PTR>(id), wlabel.c_str());
    }
  }

  // If caller passes 0,0 use the current cursor position.
  POINT pt = { static_cast<LONG>(screenX), static_cast<LONG>(screenY) };
  if (pt.x == 0 && pt.y == 0) ::GetCursorPos(&pt);

  HWND owner = ::GetForegroundWindow();
  const int result = static_cast<int>(::TrackPopupMenu(
    hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
    pt.x, pt.y, 0, owner ? owner : ::GetDesktopWindow(), nullptr));
  ::DestroyMenu(hMenu);
  return result;
}

} // namespace glint_platform
