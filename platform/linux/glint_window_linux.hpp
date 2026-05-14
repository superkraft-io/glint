#pragma once

/**
 * glint_window_linux.hpp
 * Linux (X11/Xlib) implementation of glint_window_base.
 *
 * Provides:
 *   - X11 window creation on a background thread (matching Win32/macOS model)
 *   - CPU-raster rendering: Skia SkBitmap → XPutImage  (GLINT_RENDER_GPU=OFF)
 *   - GPU rendering: EGL context + Skia Ganesh/GL       (GLINT_RENDER_GPU=ON)
 *   - Mouse, keyboard and scroll-wheel routing → glint_document
 *   - timerfd animation heartbeat (~60 fps)
 *   - DPI detection via Xft.dpi X resource / screen physical size
 *   - startThread()/stopThread() API matching Win32 and macOS
 *
 * Win32 compat type aliases are defined here so files that transitively
 * include this header (inspector/window.hpp) compile on Linux without
 * pulling in <windows.h>.
 */

#include "../glint_window_base.hpp"
#include "../../glint_bus.hpp"

#include <atomic>
#include <string>

// ── Win32 compat type aliases ─────────────────────────────────────────────────
// Thin replacements used by inspector/window.hpp, style_editor.hpp, and
// glint_attributes_list.hpp so they compile without <windows.h>.
#ifndef _WIN32
using UINT     = unsigned int;
using WPARAM   = uintptr_t;
using LPARAM   = intptr_t;
using LRESULT  = intptr_t;
using HWND     = void*;
using COLORREF = unsigned long;
using LONG     = int;
struct RECT  { int left = 0, top = 0, right = 0, bottom = 0; };
struct POINT { int x = 0, y = 0; };
static constexpr UINT WM_USER = 0x0400u;
#define LOWORD(l)  (static_cast<unsigned short>(static_cast<unsigned long>(l) & 0xFFFF))
#define HIWORD(l)  (static_cast<unsigned short>((static_cast<unsigned long>(l) >> 16) & 0xFFFF))
#define WA_INACTIVE 0
#define WM_ACTIVATE (WM_USER + 500)
#define VK_DELETE 0x2E
#endif  // !_WIN32

// ── Forward declarations ──────────────────────────────────────────────────────
// glint_insp_bridge is defined in inspector/window.hpp (when the inspector is
// enabled) or in glint_standalone_stubs.cpp (when GLINT_INSPECTOR_DISABLED).
struct glint_insp_bridge {
  static void open(glint_document*);
  static void close(glint_document*);
  static bool isOpen(glint_document*);
  static void openAndEnableInspect(glint_document*);
};

// ── glint_window_linux ────────────────────────────────────────────────────────

class glint_window_linux : public glint_window_base
{
public:
  virtual ~glint_window_linux();

  // ── Matching Win32 / macOS API ────────────────────────────────────────────

  /** Spawn the background thread; returns once the X11 window is visible. */
  void startThread();

  /** Close the window and stop the background thread. */
  void stopThread();

  /** True while the window thread is running. */
  bool isRunning() const { return mRunning.load(std::memory_order_relaxed); }

  /** Schedule a repaint.  Safe to call from any thread. */
  void requestRedraw();

  /** Called after the key press has been forwarded to the scene graph.
   *  Override in subclasses to intercept global accelerators. */
  virtual void onKeyDown(const glint_key_press& /*kp*/) {}

  /** Open a file with the system default application (xdg-open). */
  static void openFileInDefaultApp(const std::string& path);

  // ── Popup panel support (mirrors macOS glint_window_mac panel API) ────────

  /** Override to true to configure the window as a decoration-free popup.
   *  Sets _NET_WM_WINDOW_TYPE_POPUP_MENU and suppresses decorations. */
  virtual bool usePopupStyle() const { return false; }

  /** Override to false to keep the window unmapped after createXWindow.
   *  Popup subclasses call hidePanel() in onCreated() and rely on showPanel(). */
  virtual bool showOnCreate() const { return true; }

  /** Called when a popup window loses focus.
   *  Override in popup subclasses to auto-hide (= macOS onOutsideClick). */
  virtual void onOutsideClick() {}

  /** Show this window (for popup reuse).  Clears mSuppressAutoClose. */
  void showPanel();

  /** Hide this window without stopping the thread.
   *  Sets mSuppressAutoClose = true. */
  void hidePanel();

  /** Move the window to absolute screen coordinates (pixels). */
  void setPanelFrameOrigin(int x, int y);

  /** Return the usable desktop area for the primary screen.
   *  Queries _NET_WORKAREA; falls back to full screen dimensions. */
  static RECT screenWorkArea();

  /** Convert a rect in window content-space (CSS pixels) to screen coordinates.
   *  Uses XTranslateCoordinates to map relative to the root window. */
  RECT contentRectToScreen(float x, float y, float w, float h) const;

  /** Override to false to use CPU rendering.  Inspector disables GPU. */
  virtual bool useGpu() const { return true; }

  /** True while the panel is hidden; set by hidePanel(), cleared by showPanel(). */
  bool mSuppressAutoClose = false;

protected:
  // ── X11 handles (void* keeps Xlib headers out of this header) ────────────
  void*         mDisplay      = nullptr;   // Display*
  unsigned long mXWindow      = 0;         // Window  (= XID = unsigned long)
  void*         mGC           = nullptr;   // GC
  void*         mVisual       = nullptr;   // Visual*
  int           mDepth        = 24;
  unsigned long mWmDeleteAtom = 0;         // Atom for WM_DELETE_WINDOW

  // Pipe used to signal the event loop from other threads.
  // Writing any byte wakes poll(); the loop checks mRunning / mRedrawRequested.
  int mWakeFd[2] = {-1, -1};   // [0]=read  [1]=write

  std::atomic<bool> mRedrawRequested{false};

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
  // ── EGL state (GPU path) ──────────────────────────────────────────────────
  // All members are void* so <EGL/egl.h> is not included from this header.
  void* mEglDisplay = nullptr;   // EGLDisplay
  void* mEglSurface = nullptr;   // EGLSurface
  void* mEglContext = nullptr;   // EGLContext

  bool mGpuOk = false;   // true once EGL + GrDirectContext are initialised
#endif

  void refreshWindowTitle() override;

private:
  void run();
  bool createXWindow();
  void initRoot();
  void updateRootBounds();
  void paint();
  void processXEvent(void* xevent);   // parameter is XEvent*
  bool shouldTimerRedraw() const;

  float detectDpr() const;
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
  bool initGpu();
  void destroyGpu();
  void recreateSurface() override;
#endif

  static std::string wcsToUtf8(const wchar_t* wcs);
};
