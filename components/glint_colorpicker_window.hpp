#pragma once

/**
 * glint_colorpicker_window.hpp
 * A standalone Win32 popup window hosting a single glint_colorpicker.
 * Platform: Windows only (guarded by glint_window.hpp's static_assert).
 *
 * The window is *persistent* — created once via open() and kept alive for
 * the lifetime of its owner (e.g. InspStylePanel).  Subsequent opens use
 * reopen() which repositions, updates the color, and calls ShowWindow with
 * no thread teardown/respawn and no GPU context recreation.  This eliminates
 * the 100–400 ms delay that happened when the window was destroyed and
 * recreated from scratch on every swatch click.
 *
 * Lifecycle:
 *   auto* w = glint_colorpicker_window::open(color, rect, onChange, onClosed);
 *   // re-show for a different swatch later:
 *   w->reopen(newColor, newRect, newOnChange, newOnClosed);
 *   // programmatic hide (panel rebuild, element switch, …):
 *   w->hide();
 *   // permanent teardown (panel destructor):
 *   w->destroy();   // async; afterRun() deletes *this
 */

#if defined(_WIN32) || defined(OS_WIN)

#include "../glint_window.hpp"   // glint_window_win32 + all components

#include <functional>
#include <mutex>

// =============================================================================
// glint_colorpicker_window
// =============================================================================
class glint_colorpicker_window : public glint_window_win32
{
public:
  // ── Factory ──────────────────────────────────────────────────────────────
  // Creates the window once and keeps its thread alive until destroy().
  // Use reopen() to re-show with a new color/position.
  static glint_colorpicker_window* open(
    glint_color                       initialColor,
    RECT                         anchorScreenRect,
    std::function<void(glint_color)>  onChange  = nullptr,
    std::function<void()>        onClosed  = nullptr)
  {
    auto* w = new glint_colorpicker_window();
    {
      std::lock_guard<std::mutex> lk(w->mMtx);
      w->mInitialColor = initialColor;
      w->mAnchorRect   = anchorScreenRect;
      w->mOnChange     = std::move(onChange);
      w->mOnClosed     = std::move(onClosed);
    }
    w->startThread();
    return w;
  }

  // ── reopen ───────────────────────────────────────────────────────────────
  // Update color, position and callbacks, then show the already-running window.
  // Callbacks are written to *pending* fields and only promoted to active inside
  // the WM_SKUI_REOPEN_CP handler (on the picker thread).  This ensures that
  // any WM_SKUI_HIDE_CP messages already in the queue fire the *old* generation
  // callback — which the inspector discards as stale — rather than the new
  // generation callback, which would incorrectly clear the inspector's swatch
  // state before the picker is even visible.
  void reopen(glint_color                       initialColor,
              RECT                         anchorScreenRect,
              std::function<void(glint_color)>  onChange,
              std::function<void()>        onClosed)
  {
    {
      std::lock_guard<std::mutex> lk(mMtx);
      mInitialColor      = initialColor;
      mAnchorRect        = anchorScreenRect;
      mPendingOnChange   = std::move(onChange);   // promoted at REOPEN_CP time
      mPendingOnClosed   = std::move(onClosed);   // promoted at REOPEN_CP time
    }
    if (HWND h = mHWNDAtom.load())
      ::PostMessage(h, WM_SKUI_REOPEN_CP, 0, 0);
  }

  // ── hide ─────────────────────────────────────────────────────────────────
  // Hides the window without stopping its thread.  Fires the current
  // onClosed callback so the inspector receives its WM_INSP_CP_CLOSED.
  void hide()
  {
    if (HWND h = mHWNDAtom.load())
      ::PostMessage(h, WM_SKUI_HIDE_CP, 0, 0);
  }

  // ── destroy ──────────────────────────────────────────────────────────────
  // Stops the window thread.  afterRun() will delete *this.
  // Do NOT call any method after destroy().
  void destroy() { stopThread(); }

protected:
  // Custom message IDs (above WM_USER + 200 used by the win32 base).
  static constexpr UINT WM_SKUI_HIDE_CP   = WM_USER + 210;
  static constexpr UINT WM_SKUI_REOPEN_CP = WM_USER + 211;

  LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM /*lp*/) override
  {
    // ── Hide: fire onClosed, then hide the window.
    if (msg == WM_SKUI_HIDE_CP)
    {
      std::function<void()> cb;
      { std::lock_guard<std::mutex> lk(mMtx); cb = mOnClosed; }
      if (cb) cb();
      ::ShowWindow(mHWND, SW_HIDE);
      return 0;
    }

    // ── Reopen: promote pending callbacks, reposition, seed picker, show.
    if (msg == WM_SKUI_REOPEN_CP)
    {
      glint_color color; RECT anchor;
      {
        std::lock_guard<std::mutex> lk(mMtx);
        color     = mInitialColor;
        anchor    = mAnchorRect;
        // Atomically promote pending callbacks to active.  Any WM_SKUI_HIDE_CP
        // processed before this point used the OLD callbacks (old generation),
        // so the inspector discarded them as stale.  From now on HIDE_CP fires
        // the new callbacks correctly.
        mOnChange   = std::move(mPendingOnChange);
        mOnClosed   = std::move(mPendingOnClosed);
      }
      _reposition(anchor);
      if (mPicker) mPicker->setValue(color);
      ::ShowWindow(mHWND, SW_SHOW);
      ::PostMessage(mHWND, WM_SKUI_REDRAW, 0, 0);  // force first repaint
      return 0;
    }

    // ── WA_INACTIVE: hide instead of destroying to preserve the thread.
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
    {
      ::PostMessage(mHWND, WM_SKUI_HIDE_CP, 0, 0);
      return 0;
    }

    return -1;  // not handled — fall through to DefWindowProc
  }

  const wchar_t* windowClassName() const override { return L"glint_color_picker"; }
  const wchar_t* windowTitle()     const override { return L""; }
  DWORD          windowStyle()     const override { return WS_POPUP; }
  DWORD          windowExStyle()   const override { return WS_EX_TOOLWINDOW; }
  int            defaultWidth()    const override { return 240; }
  int            defaultHeight()   const override { return 240; }
  COLORREF       bgColor()         const override { return RGB(25, 25, 28); }
  SkColor        clearColor()      const override { return SkColorSetARGB(0, 25, 25, 28); }
  bool useTransparency() const override { return true; }
  bool useGpu()          const override { return false; }
  // Always start hidden — reopen() calls ShowWindow(SW_SHOW) explicitly.
  // This eliminates the 0,0 flash that occurred when prewarmPicker() called
  // open() + hide() and hide()'s PostMessage raced against mHWNDAtom being set.
  bool showOnCreate()    const override { return false; }

  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width           = "100%";
    wrap->style.height          = "100%";
    wrap->style.display         = "flex";
    wrap->style.flexDirection   = "column";
    wrap->style.backgroundColor = glint_color(255, 25, 25, 28);
    wrap->style.borderRadius    = 8.f;
    wrap->style.overflow        = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_colorpicker();
    mPicker->style.width  = "100%";
    mPicker->style.height = "100%";
    { std::lock_guard<std::mutex> lk(mMtx); mPicker->value = mInitialColor; }
    mPicker->onChange = [this](glint_color c) {
      std::function<void(glint_color)> cb;
      { std::lock_guard<std::mutex> lk(mMtx); cb = mOnChange; }
      if (cb) cb(c);
    };
    wrap->addChild(mPicker);
  }

  void onCreated() override
  {
    RECT anchor;
    { std::lock_guard<std::mutex> lk(mMtx); anchor = mAnchorRect; }
    _reposition(anchor);
  }

  // afterRun() fires only on a true destroy() → WM_CLOSE.
  // We do NOT call onClosed here — the inspector may already be gone.
  void afterRun() override { delete this; }

private:
  // Protected by mMtx — written from the inspector/main thread (reopen/open),
  // read from the picker thread (onChange handler, WM_SKUI_REOPEN_CP handler).
  std::mutex                   mMtx;
  glint_color                       mInitialColor    = glint_color(255, 128, 128, 128);
  RECT                         mAnchorRect      = { 100, 100, 114, 114 };
  // Active callbacks — used by WM_SKUI_HIDE_CP and picker onChange.
  std::function<void(glint_color)>  mOnChange;
  std::function<void()>        mOnClosed;
  // Pending callbacks — written by reopen(), promoted to active at WM_SKUI_REOPEN_CP.
  // Keeping them separate means any queued WM_SKUI_HIDE_CP fires the OLD (stale)
  // generation callback and is safely discarded by the inspector.
  std::function<void(glint_color)>  mPendingOnChange;
  std::function<void()>        mPendingOnClosed;

  glint_colorpicker* mPicker = nullptr;   // owned by the scene graph

  void _reposition(RECT anchor)
  {
    const int W    = defaultWidth();
    const int H    = defaultHeight();
    const int kGap = 4;

    RECT wa{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top)        y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right) x = wa.right - W;
    if (x < wa.left)      x = wa.left;

    // SWP_NOACTIVATE: don't steal focus when repositioning from another thread.
    ::SetWindowPos(mHWND, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
  }
};

#elif defined(__linux__)  // ── Linux (X11) implementation ──────────────────────

#include "../platform/glint_window.hpp"   // glint_window_linux + all components
#include "glint_colorpicker.hpp"

#include <functional>

// =============================================================================
// glint_colorpicker_window (Linux / X11)
// Persistent popup window hosting a glint_colorpicker.
// Same open/reopen/hide/destroy API as Win32 and macOS versions.
// =============================================================================
class glint_colorpicker_window : public glint_window_linux
{
public:
  static glint_colorpicker_window* open(
    glint_color                       initialColor,
    RECT                         anchorScreenRect,
    std::function<void(glint_color)>  onChange  = nullptr,
    std::function<void()>        onClosed  = nullptr)
  {
    auto* w = new glint_colorpicker_window();
    w->mInitialColor = initialColor;
    w->mAnchorRect   = anchorScreenRect;
    w->mOnChange     = std::move(onChange);
    w->mOnClosed     = std::move(onClosed);
    w->startThread();
    return w;
  }

  void reopen(glint_color                       initialColor,
              RECT                         anchorScreenRect,
              std::function<void(glint_color)>  onChange,
              std::function<void()>        onClosed)
  {
    mInitialColor = initialColor;
    mAnchorRect   = anchorScreenRect;
    mOnChange     = std::move(onChange);
    mOnClosed     = std::move(onClosed);
    mClosedFired  = false;
    _reposition(mAnchorRect);
    if (mPicker) mPicker->setValue(initialColor);
    requestRedraw();
    showPanel();
  }

  void hide()
  {
    if (!mClosedFired) {
      mClosedFired = true;
      if (mOnClosed) mOnClosed();
    }
    hidePanel();
  }

  void destroy()
  {
    mDestroyRequested = true;
    stopThread();
  }

  bool usePopupStyle() const override { return true; }
  bool showOnCreate()  const override { return false; }
  void onOutsideClick() override { hide(); }
  int  defaultWidth()   const override { return 240; }
  int  defaultHeight()  const override { return 240; }
  const wchar_t* windowClassName() const override { return L"glint_color_picker"; }
  const wchar_t* windowTitle()     const override { return L""; }
  SkColor clearColor() const override { return SkColorSetARGB(0, 25, 25, 28); }

protected:
  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width           = "100%";
    wrap->style.height          = "100%";
    wrap->style.display         = "flex";
    wrap->style.flexDirection   = "column";
    wrap->style.backgroundColor = glint_color(255, 25, 25, 28);
    wrap->style.borderRadius    = 8.f;
    wrap->style.overflow        = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_colorpicker();
    mPicker->style.width  = "100%";
    mPicker->style.height = "100%";
    mPicker->value = mInitialColor;
    mPicker->onChange = [this](glint_color c) {
      if (mOnChange) mOnChange(c);
    };
    wrap->addChild(mPicker);
  }

  void onCreated() override
  {
    _reposition(mAnchorRect);
    hidePanel();
  }

  void afterRun() override
  {
    if (!mDestroyRequested && !mClosedFired) {
      mClosedFired = true;
      if (mOnClosed) mOnClosed();
    }
    delete this;
  }

private:
  glint_color                       mInitialColor    = glint_color(255, 128, 128, 128);
  RECT                         mAnchorRect      = { 100, 100, 114, 114 };
  std::function<void(glint_color)>  mOnChange;
  std::function<void()>        mOnClosed;
  glint_colorpicker*           mPicker          = nullptr;
  bool                         mClosedFired     = false;
  bool                         mDestroyRequested = false;

  void _reposition(RECT anchor)
  {
    const int W    = defaultWidth();
    const int H    = defaultHeight();
    const int kGap = 4;
    const RECT wa  = glint_window_linux::screenWorkArea();

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top)        y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right) x = wa.right - W;
    if (x < wa.left)      x = wa.left;

    setPanelFrameOrigin(x, y);
  }
};

#else  // ── macOS implementation ─────────────────────────────────────────────────

#include "../platform/glint_window.hpp"   // glint_window_mac + all components
#include "glint_colorpicker.hpp"

#include <functional>

// =============================================================================
// glint_colorpicker_window (macOS)
// Persistent NSPanel popup hosting a glint_colorpicker.
// Same open/reopen/hide/destroy API as the Win32 version.
// =============================================================================
class glint_colorpicker_window : public glint_window_mac
{
public:
  // ── Factory ──────────────────────────────────────────────────────────────
  static glint_colorpicker_window* open(
    glint_color                       initialColor,
    RECT                         anchorScreenRect,
    std::function<void(glint_color)>  onChange  = nullptr,
    std::function<void()>        onClosed  = nullptr)
  {
    auto* w = new glint_colorpicker_window();
    w->mInitialColor = initialColor;
    w->mAnchorRect   = anchorScreenRect;
    w->mOnChange     = std::move(onChange);
    w->mOnClosed     = std::move(onClosed);
    w->startThread();   // synchronous on main thread: panel + UI created immediately
    return w;
  }

  // ── reopen ───────────────────────────────────────────────────────────────
  // Update color, position, and callbacks; then show the hidden panel.
  void reopen(glint_color                       initialColor,
              RECT                         anchorScreenRect,
              std::function<void(glint_color)>  onChange,
              std::function<void()>        onClosed)
  {
    mInitialColor = initialColor;
    mAnchorRect   = anchorScreenRect;
    mOnChange     = std::move(onChange);
    mOnClosed     = std::move(onClosed);
    mClosedFired  = false;

    _reposition(mAnchorRect);
    if (mPicker) mPicker->setValue(initialColor);
    requestRedraw();
    showPanel();   // clears mSuppressAutoClose, makes panel key + visible
  }

  // ── hide ─────────────────────────────────────────────────────────────────
  // Fire onClosed, then hide the panel (keep thread alive for reopen).
  void hide()
  {
    if (!mClosedFired) {
      mClosedFired = true;
      if (mOnClosed) mOnClosed();
    }
    hidePanel();   // sets mSuppressAutoClose = true, calls orderOut
  }

  // ── destroy ──────────────────────────────────────────────────────────────
  void destroy()
  {
    mDestroyRequested = true;
    stopThread();   // → afterRun() → delete this
  }

  bool usePopupStyle() const override { return true; }
  void onOutsideClick() override { hide(); }
  int  defaultWidth()  const override { return 240; }
  int  defaultHeight() const override { return 240; }
  const wchar_t* windowClassName() const override { return L"glint_color_picker"; }
  const wchar_t* windowTitle()     const override { return L""; }

  SkColor clearColor() const override { return SkColorSetARGB(0, 25, 25, 28); }

protected:
  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width           = "100%";
    wrap->style.height          = "100%";
    wrap->style.display         = "flex";
    wrap->style.flexDirection   = "column";
    wrap->style.backgroundColor = glint_color(255, 25, 25, 28);
    wrap->style.borderRadius    = 8.f;
    wrap->style.overflow        = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_colorpicker();
    mPicker->style.width  = "100%";
    mPicker->style.height = "100%";
    mPicker->value = mInitialColor;
    mPicker->onChange = [this](glint_color c) {
      if (mOnChange) mOnChange(c);
    };
    wrap->addChild(mPicker);
  }

  void onCreated() override
  {
    _reposition(mAnchorRect);
    // Start hidden — reopen() will show it explicitly.
    hidePanel();
  }

  void afterRun() override
  {
    if (!mDestroyRequested && !mClosedFired) {
      mClosedFired = true;
      if (mOnClosed) mOnClosed();
    }
    delete this;
  }

private:
  glint_color                       mInitialColor    = glint_color(255, 128, 128, 128);
  RECT                         mAnchorRect      = { 100, 100, 114, 114 };
  std::function<void(glint_color)>  mOnChange;
  std::function<void()>        mOnClosed;
  glint_colorpicker*           mPicker          = nullptr;   // owned by scene graph
  bool                         mClosedFired     = false;
  bool                         mDestroyRequested = false;

  void _reposition(RECT anchor)
  {
    const int W    = defaultWidth();
    const int H    = defaultHeight();
    const int kGap = 4;

    const RECT wa = glint_window_mac::screenWorkArea();

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top)        y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right) x = wa.right - W;
    if (x < wa.left)      x = wa.left;

    setPanelFrameOrigin(x, y);
  }
};

#endif
