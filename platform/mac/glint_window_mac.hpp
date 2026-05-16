#pragma once

/**
 * glint_window_mac.hpp
 * macOS implementation of glint_window_base.
 *
 * Provides the macOS plumbing that mirrors glint_window_win32 on Windows:
 *
 *   - NSPanel creation / lifecycle (main-thread, no background thread needed)
 *   - NSView subclass for Skia CPU-raster rendering via CGContext
 *   - Mouse routing → glint_document (down/up/move/drag/leave/wheel)
 *   - Keyboard routing → glint_document (keyDown/keyUp with full modifier mapping)
 *   - NSTimer-based timer management replacing Win32 SetTimer/KillTimer
 *   - requestRedraw() replacing Win32 InvalidateRect
 *   - startThread()/stopThread() API (matching Win32 naming used by inspector)
 *   - openFileInDefaultApp() replacing ShellExecuteA
 *   - Ctrl+Shift+I/C global shortcut to open the inspector (mirrors Win32 base behaviour)
 *
 * Win32 compat type aliases are defined here for files that include this header
 * transitively (inspector/window.hpp, style_editor.hpp) so they compile on macOS
 * without including <windows.h>.
 *
 * Include via the umbrella glint_window.hpp which resolves to this on Apple platforms.
 */

#include "../glint_window_base.hpp"
#include "../../glint_bus.hpp"   // glint_bus::subscribe/unsubscribe
#include "../glint_platform.hpp"  // glint_platform::setClipboardText / getClipboardText

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// ── Win32 compat type aliases ─────────────────────────────────────────────────
// Thin replacements for Windows types used in inspector/window.hpp,
// style_editor.hpp, and glint_attributes_list.hpp.  Scoped to Apple builds so
// they never collide with the real definitions on Windows.
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
// LOWORD / HIWORD macros used in glint_attributes_list.hpp WM_ACTIVATE handling.
#define LOWORD(l)  (static_cast<unsigned short>(static_cast<unsigned long>(l) & 0xFFFF))
#define HIWORD(l)  (static_cast<unsigned short>((static_cast<unsigned long>(l) >> 16) & 0xFFFF))
#define WA_INACTIVE 0
#define WM_ACTIVATE (WM_USER + 500)   // unused on Mac — just needs to compile
// Common Win32 virtual key codes used by cross-platform inspector logic.
#define VK_DELETE 0x2E
#endif  // !_WIN32

// ── Forward declarations ──────────────────────────────────────────────────────

// Declared here so inspector/window.hpp compiles without pulling in glint_window_win32.
// Defined inline in inspector/window.hpp (when the inspector is enabled) or
// in glint_standalone_stubs.cpp (when GLINT_INSPECTOR_DISABLED).
struct glint_insp_bridge {
  static void open(glint_document*);
  static void close(glint_document*);
  static bool isOpen(glint_document*);
  static void openAndEnableInspect(glint_document*);
};

// ── glint_window_mac ──────────────────────────────────────────────────────────

class glint_window_mac : public glint_window_base
{
public:
  virtual ~glint_window_mac();

  // ── Matching Win32 startThread()/stopThread() API ────────────────────────
  // Creates an NSPanel on the main thread (safe to call from any thread).
  void startThread();

  // Closes the NSPanel and destroys the window (safe to call from any thread).
  void stopThread();

  // True while the panel is open.
  bool isRunning() const { return mRunning.load(); }

  // ── Panel visibility ──────────────────────────────────────────────────────
  // Show the panel (bring to front) and clear mSuppressAutoClose.
  // Safe to call from any thread.
  void showPanel();
  // Hide the panel without destroying or stopping its thread.
  // Sets mSuppressAutoClose = true so that outside-click monitors do not call
  // stopThread() while the panel is hidden.  showPanel() clears the flag.
  // Must be called from the main thread.
  void hidePanel();

  /** Called when an outside click (or windowDidResignKey) would normally
   *  dismiss the popup.  Default: stopThread().  Subclasses can override to
   *  call hide() instead (keeping the window alive for reopen). */
  virtual void onOutsideClick() { if (isRunning()) stopThread(); }

  // When true, outside-click monitors and windowDidResignKey: skip stopThread().
  // Set by hidePanel(), cleared by showPanel().  Also checked in the ObjC
  // delegate and event-monitor blocks.
  bool mSuppressAutoClose = false;

  // ── Redraw ────────────────────────────────────────────────────────────────
  // Triggers a repaint on the main thread (replaces Win32 InvalidateRect).
  // Safe to call from any thread.
  void requestRedraw();

  // ── Timer management (replaces Win32 SetTimer / KillTimer) ───────────────
  // Sets a repeating (interval > 0) or one-shot (one-shot=true) NSTimer
  // identified by timerId.  Fires onTimerFired(timerId) on the main thread.
  // Safe to call from any thread.
  void setTimer(int timerId, double intervalSec, bool oneShot = false);
  void killTimer(int timerId);

  // Override in subclass to receive timer events (replaces WM_TIMER handling).
  virtual void onTimerFired(int timerId) {}

  // Override in subclass to intercept key events after the document processes them
  // (mirrors glint_window_win32::onKeyDown for cross-platform inspector accelerators).
  virtual void onKeyDown(const glint_key_press& /*kp*/) {}

  // ── File open (replaces ShellExecuteA "open") ────────────────────────────
  static void openFileInDefaultApp(const std::string& path);

  // ── Main-thread dispatch (replaces PostMessage for cross-thread UI updates) ─
  // Schedules fn() on the main queue.  Safe to call from any thread.
  static void _dispatchMain(std::function<void()> fn);

  // ── Screen coordinate helpers (replaces Win32 ClientToScreen / SetWindowPos) ─
  // Convert a content-space rect (Skia y-down coords from top-left of the
  // NSView) to a Win32-style screen RECT (y-down from top-left of the primary
  // screen).  Main-thread only; call from onCreated() or equivalent.
  RECT contentRectToScreen(float x, float y, float w, float h) const;

  // Reposition this panel so its top-left is at (screenX, screenYFromTop)
  // in Win32-style screen coordinates (y-down, origin at top of primary screen).
  // Main-thread only; call from onCreated().
  void setPanelFrameOrigin(int screenX, int screenYFromTop);

  // Work area (screen minus Dock + menu bar) in Win32-style coordinates.
  // Main-thread only.
  static RECT screenWorkArea();

  /** When true the panel is created borderless (no titlebar / traffic lights),
   *  floats above all windows, and auto-closes when it loses key focus.
   *  Public so the ObjC delegate can query it. Default: false. */
  virtual bool usePopupStyle() const { return false; }

  // ── Input routing ─────────────────────────────────────────────────────────
  // Called from the ObjC NSView subclass — not for external use.
  void routeMouseDown(float x, float y, bool rightButton,
                      bool shift, bool ctrl, bool alt, bool cmd);
  void routeMouseUp  (float x, float y, bool rightButton,
                      bool shift, bool ctrl, bool alt, bool cmd);
  void routeMouseMove(float x, float y,
                      bool shift, bool ctrl, bool alt, bool cmd,
                      bool leftDown, bool rightDown);
  void routeMouseLeave();
  void routeMouseWheel(float x, float y, float deltaX, float deltaY,
                       bool shift, bool ctrl, bool alt, bool cmd,
                       bool hasPreciseDeltas = false,
                       glint_input_phase phase = glint_input_phase::none,
                       glint_input_phase momentumPhase = glint_input_phase::none);
  void routeGesture(float x, float y, glint_gesture_kind kind,
                    glint_input_phase phase,
                    bool shift, bool ctrl, bool alt, bool cmd,
                    float deltaX = 0.f, float deltaY = 0.f,
                    float magnification = 0.f, float rotation = 0.f,
                    bool isInertial = false, bool hasPreciseDeltas = false);
  void routeKeyDown(const glint_key_press& kp);
  void routeKeyUp  (int vk);

  // ── Paint ─────────────────────────────────────────────────────────────────
  // Called from NSView drawRect: — renders the glint scene graph to a
  // Skia CPU bitmap and then blits it via SkCGDrawBitmap.
  void routeDraw(void* cgContextRef, int pixelWidth, int pixelHeight, float scale);

  // ── Resize ────────────────────────────────────────────────────────────────
  void routeResize(int newW, int newH);

protected:
  // ── Platform handles (void* so this header compiles as plain C++) ─────────
  void* mPanelHandle    = nullptr;   // __strong NSPanel*
  void* mViewHandle     = nullptr;   // GlintWindowMacNSView*
  void* mDelegateHandle = nullptr;   // GlintWindowMacDelegate*
  void* mEventMonitor       = nullptr;   // id<NSObject> local NSEvent monitor (popup only)
  void* mEventMonitorGlobal = nullptr;   // id<NSObject> global NSEvent monitor (popup only)

  // Active NSTimers keyed by integer timer ID (stored as void*, i.e. NSTimer*).
  std::unordered_map<int, void*> mTimers;

  // ── Subclass interface (mirrors glint_window_win32 optional overrides) ────

  /** Window title shown in the titlebar (UTF-8). Default: "glint Window". */
  virtual const char* macTitleUTF8() const { return "glint Window"; }

  void refreshWindowTitle() override;

  /** Initial content size in points (not pixels). */
  int defaultWidth()  const override { return 820; }
  int defaultHeight() const override { return 650; }

  /** Returns a SkColor for the clear-colour painted each frame.
   *  Default: opaque near-black matching glint inspector dark theme. */
  SkColor clearColor() const override { return SkColorSetARGB(255, 26, 26, 26); }

  /** Whether to use GPU acceleration.  Default: true (Metal).  Override and
   *  return false to force the CPU raster path (e.g. inspector / colour-picker). */
  virtual bool useGpu() const { return true; }

  // ── Lifecycle hooks ───────────────────────────────────────────────────────
  // Subclass overrides (same as Win32):
  //   buildUI()           — required: populate mOwnRoot->mCanvas
  //   onCreated()         — optional: called after buildUI + recreateSurface
  //   onThreadStarted()   — optional: subscribe to glint_bus events here
  //   onThreadEnded()     — optional: unsubscribe and clean up
  //   onDestroyed()       — optional: called just before panel close
  //   afterRun()          — optional: called after panel is fully closed

  // ── Metal state (void* so this header compiles as plain C++) ─────────────
  void* mMetalDevice    = nullptr;  // id<MTLDevice>        (+1 retain, released in teardownMetal)
  void* mMetalQueue     = nullptr;  // id<MTLCommandQueue>  (+1 retain, released in teardownMetal)
  void* mMetalLayer     = nullptr;  // CAMetalLayer*        (+1 retain, released in teardownMetal)
  bool  mMetalEnabled   = false;
  bool  mRedrawRequested = false;   // set by requestRedraw(); cleared in paintMetal()

public:
  /** True when the Metal backend is active (set by setupMetal()). */
  bool metalEnabled() const { return mMetalEnabled; }

  /** Alpha channel of the Skia clear-colour (0 = fully transparent, 255 = opaque).
   *  Used by the ObjC NSView/CAMetalLayer setup to decide whether to be non-opaque. */
  uint8_t clearColorAlpha() const { return SkColorGetA(clearColor()); }

  /** Render the glint scene graph into the CAMetalLayer.  Called on the main
   *  thread from GlintWindowMacNSView animTimerFired: when Metal is active. */
  void paintMetal();

private:
  // ── Internal helpers (implemented in glint_window_mac.mm) ─────────────────
  void _createPanelAndView();
  void _closePanelAndCleanup();
  void _scheduleTimerOnMainThread(int timerId, double intervalSec, bool oneShot);
  void _killTimerOnMainThread(int timerId);
  void setupMetal();
  void teardownMetal();


public:
  // Called from ObjC delegate when the panel is about to close.
  // Public so GlintWindowMacDelegate (ObjC) can call it directly.
  void _panelWillClose();
};

using glint_window = glint_window_mac;
