#pragma once

/**
 * glint_datepicker_window.hpp
 * A standalone native popup window hosting a single glint_datepicker calendar.
 * Platform: Windows + macOS (same dual-impl pattern as glint_colorpicker_window).
 *
 * The window is *persistent* — created once via open() and kept alive for the
 * lifetime of its owner.  Subsequent opens use reopen() which repositions,
 * updates the date, and shows the window with no GPU-context teardown.
 *
 * Lifecycle:
 *   auto* w = glint_datepicker_window::open(year, month, day, anchorRect, onChange, onClosed);
 *   // re-show later:
 *   w->reopen(year, month, day, newAnchor, newOnChange, newOnClosed);
 *   // programmatic hide:
 *   w->hide();
 *   // permanent teardown:
 *   w->destroy();   // async; afterRun() deletes *this
 *
 * The anchor rect is in screen coordinates (pixels from top-left of primary
 * monitor).  The window positions itself below the anchor, flipping above if
 * there is not enough space underneath.
 */

#include "../../../platform/glint_apple_platform.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Windows implementation
// ─────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32) || defined(OS_WIN)

#include "../../../glint_window.hpp"   // glint_window_win32 + all components
#include "glint_datepicker.hpp"

#include <functional>
#include <mutex>

// =============================================================================
// glint_datepicker_window  (Win32)
// =============================================================================
class glint_datepicker_window : public glint_window_win32
{
public:
  // ── Factory ──────────────────────────────────────────────────────────────
  static glint_datepicker_window* open(
    int  year, int month, int day,
    RECT anchorScreenRect,
    std::function<void(int,int,int)> onChange = nullptr,
    std::function<void()>            onClosed = nullptr)
  {
    auto* w = new glint_datepicker_window();
    {
      std::lock_guard<std::mutex> lk(w->mMtx);
      w->mYear        = year;
      w->mMonth       = month;
      w->mDay         = day;
      w->mAnchorRect  = anchorScreenRect;
      w->mOnChange    = std::move(onChange);
      w->mOnClosed    = std::move(onClosed);
    }
    w->startThread();
    return w;
  }

  // ── Static active-popup registry ─────────────────────────────────────────
  // Exactly one glint_datepicker_window may be "active" at a time.  When any
  // instance is shown via reopen(), it registers itself here and installs a
  // single capture-phase wheel listener on the document canvas that hides it.
  // This means callers never need to manage wheel listeners themselves.
  static void _registerActive(glint_datepicker_window* w, glint_element* docCanvas)
  {
    _unregisterActive(nullptr); // clear any previous
    sActiveInstance = w;
    sDocCanvas      = docCanvas;
    if (docCanvas && sWheelListenerId < 0)
    {
      sWheelListenerId = docCanvas->addEventListener(
        "wheel",
        [](glint_event&) {
          if (sActiveInstance) sActiveInstance->hide();
        },
        /*useCapture=*/true);
    }
  }

  static void _unregisterActive(glint_datepicker_window* w)
  {
    if (w && w != sActiveInstance) return;
    if (sDocCanvas && sWheelListenerId >= 0)
    {
      sDocCanvas->removeEventListener(sWheelListenerId);
      sWheelListenerId = -1;
    }
    sActiveInstance = nullptr;
    sDocCanvas      = nullptr;
  }

  // ── reopen ───────────────────────────────────────────────────────────────
  // Pending callbacks are promoted to active inside WM_SKUI_REOPEN_DP so that
  // any WM_SKUI_HIDE_DP already queued fires the old-generation callback.
  void reopen(int  year, int month, int day,
              RECT anchorScreenRect,
              std::function<void(int,int,int)> onChange,
              std::function<void()>            onClosed,
              glint_element*                   docCanvas = nullptr)
  {
    {
      std::lock_guard<std::mutex> lk(mMtx);
      mYear              = year;
      mMonth             = month;
      mDay               = day;
      mAnchorRect        = anchorScreenRect;
      mPendingOnChange   = std::move(onChange);
      mPendingOnClosed   = std::move(onClosed);
      mDocCanvas         = docCanvas;
    }
    if (HWND h = mHWNDAtom.load())
      ::PostMessage(h, WM_SKUI_REOPEN_DP, 0, 0);
  }

  // ── isVisible ────────────────────────────────────────────────────────────
  bool isVisible() const
  {
    HWND h = mHWNDAtom.load();
    return h && ::IsWindowVisible(h);
  }

  // ── hide ─────────────────────────────────────────────────────────────────
  void hide()
  {
    if (HWND h = mHWNDAtom.load())
      ::PostMessage(h, WM_SKUI_HIDE_DP, 0, 0);
  }

  // ── destroy ──────────────────────────────────────────────────────────────
  void destroy() { stopThread(); }

protected:
  static constexpr UINT WM_SKUI_HIDE_DP   = WM_USER + 212;
  static constexpr UINT WM_SKUI_REOPEN_DP = WM_USER + 213;

  LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM /*lp*/) override
  {
    if (msg == WM_SKUI_HIDE_DP)
    {
      std::function<void()> cb;
      { std::lock_guard<std::mutex> lk(mMtx); cb = mOnClosed; }
      _unregisterActive(this);
      if (cb) cb();
      ::ShowWindow(mHWND, SW_HIDE);
      return 0;
    }

    if (msg == WM_SKUI_REOPEN_DP)
    {
      int y, m, d; RECT anchor; glint_element* docCanvas;
      {
        std::lock_guard<std::mutex> lk(mMtx);
        y = mYear; m = mMonth; d = mDay;
        anchor     = mAnchorRect;
        docCanvas  = mDocCanvas;
        mOnChange  = std::move(mPendingOnChange);
        mOnClosed  = std::move(mPendingOnClosed);
      }
      _registerActive(this, docCanvas);
      _reposition(anchor);
      if (mPicker) mPicker->setDate(y, m, d);
      ::ShowWindow(mHWND, SW_SHOW);
      ::SetForegroundWindow(mHWND);   // ensure activation so WA_INACTIVE fires on dismiss
      ::PostMessage(mHWND, WM_SKUI_REDRAW, 0, 0);
      return 0;
    }

    // Deactivate → hide (keeps thread alive for fast reuse)
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
    {
      ::PostMessage(mHWND, WM_SKUI_HIDE_DP, 0, 0);
      return 0;
    }

    return -1;
  }

  const wchar_t* windowClassName() const override { return L"glint_date_picker"; }
  const wchar_t* windowTitle()     const override { return L""; }
  DWORD          windowStyle()     const override { return WS_POPUP; }
  DWORD          windowExStyle()   const override { return WS_EX_TOOLWINDOW; }
  int            defaultWidth()    const override { return (int)glint_datepicker::kW; }
  int            defaultHeight()   const override { return (int)glint_datepicker::kH(); }
  COLORREF       bgColor()         const override { return RGB(30, 30, 30); }
  SkColor        clearColor()      const override { return SkColorSetARGB(0, 30, 30, 30); }
  bool useTransparency() const override { return true; }
  bool useGpu()          const override { return false; }
  bool showOnCreate()    const override { return false; }

  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width           = "100%";
    wrap->style.height          = "100%";
    wrap->style.backgroundColor = glint_color(255, 30, 30, 30);
    wrap->style.borderRadius    = 8.f;
    wrap->style.overflow        = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_datepicker();
    mPicker->style.width  = "100%";
    mPicker->style.height = "100%";
    {
      std::lock_guard<std::mutex> lk(mMtx);
      mPicker->setDate(mYear, mMonth, mDay);
    }
    mPicker->onChange = [this](int y, int m, int d)
    {
      std::function<void(int,int,int)> cb;
      { std::lock_guard<std::mutex> lk(mMtx); cb = mOnChange; }
      if (cb) cb(y, m, d);
      // Auto-close after a date is picked (same UX as Chrome's date picker)
      if (HWND h = mHWNDAtom.load()) ::PostMessage(h, WM_SKUI_HIDE_DP, 0, 0);
    };
    wrap->addChild(mPicker);
  }

  void onCreated() override
  {
    RECT anchor;
    { std::lock_guard<std::mutex> lk(mMtx); anchor = mAnchorRect; }
    _reposition(anchor);
    // Window stays hidden; reopen() / WM_SKUI_REOPEN_DP shows it.
  }

  void afterRun() override { delete this; }

private:
  // ── Static registry storage ─────────────────────────────────────────────
  static inline glint_datepicker_window* sActiveInstance  = nullptr;
  static inline glint_element*           sDocCanvas       = nullptr;
  static inline int                      sWheelListenerId = -1;

  std::mutex                       mMtx;
  int                              mYear = 2024, mMonth = 1, mDay = 1;
  RECT                             mAnchorRect      = { 100, 100, 114, 114 };
  glint_element*                   mDocCanvas       = nullptr;
  std::function<void(int,int,int)> mOnChange;
  std::function<void()>            mOnClosed;
  std::function<void(int,int,int)> mPendingOnChange;
  std::function<void()>            mPendingOnClosed;
  glint_datepicker*                mPicker = nullptr;   // owned by scene graph

  void _reposition(RECT anchor)
  {
    const int W = defaultWidth(), H = defaultHeight(), kGap = 4;
    RECT wa{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top)        y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right)  x = wa.right - W;
    if (x < wa.left)       x = wa.left;

    ::SetWindowPos(mHWND, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Linux (X11) implementation
// ─────────────────────────────────────────────────────────────────────────────

#elif defined(__linux__)

#include "../../../platform/glint_window.hpp"   // glint_window_linux + all components
#include "glint_datepicker.hpp"

#include <functional>

// =============================================================================
// glint_datepicker_window  (Linux / X11)
// =============================================================================
class glint_datepicker_window : public glint_window_linux
{
public:
  static glint_datepicker_window* open(
    int  year, int month, int day,
    RECT anchorScreenRect,
    std::function<void(int,int,int)> onChange = nullptr,
    std::function<void()>            onClosed = nullptr)
  {
    auto* w = new glint_datepicker_window();
    w->mYear       = year;
    w->mMonth      = month;
    w->mDay        = day;
    w->mAnchorRect = anchorScreenRect;
    w->mOnChange   = std::move(onChange);
    w->mOnClosed   = std::move(onClosed);
    w->startThread();
    return w;
  }

  void reopen(int  year, int month, int day,
              RECT anchorScreenRect,
              std::function<void(int,int,int)> onChange,
              std::function<void()>            onClosed,
              glint_element*                   docCanvas = nullptr)
  {
    mYear       = year;
    mMonth      = month;
    mDay        = day;
    mAnchorRect = anchorScreenRect;
    mOnChange   = std::move(onChange);
    mOnClosed   = std::move(onClosed);
    mClosedFired = false;

    _registerActive(this, docCanvas);
    _reposition(mAnchorRect);
    if (mPicker) mPicker->setDate(mYear, mMonth, mDay);
    requestRedraw();
    showPanel();
  }

  void hide()
  {
    _unregisterActive(this);
    if (!mClosedFired) {
      mClosedFired = true;
      if (mOnClosed) mOnClosed();
    }
    hidePanel();
  }

  bool isVisible() const { return !mSuppressAutoClose; }

  void destroy()
  {
    mDestroyRequested = true;
    stopThread();
  }

  bool usePopupStyle()   const override { return true; }
  bool showOnCreate()    const override { return false; }
  void onOutsideClick()  override       { hide(); }
  int  defaultWidth()    const override { return (int)glint_datepicker::kW; }
  int  defaultHeight()   const override { return (int)glint_datepicker::kH(); }
  const wchar_t* windowClassName() const override { return L"glint_date_picker"; }
  const wchar_t* windowTitle()     const override { return L""; }
  SkColor clearColor() const override { return SkColorSetARGB(0, 30, 30, 30); }

protected:
  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width           = "100%";
    wrap->style.height          = "100%";
    wrap->style.backgroundColor = glint_color(255, 30, 30, 30);
    wrap->style.borderRadius    = 8.f;
    wrap->style.overflow        = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_datepicker();
    mPicker->style.width  = "100%";
    mPicker->style.height = "100%";
    mPicker->setDate(mYear, mMonth, mDay);
    mPicker->onChange = [this](int y, int m, int d)
    {
      if (mOnChange) mOnChange(y, m, d);
      hide();
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
  static inline glint_datepicker_window* sActiveInstance  = nullptr;
  static inline glint_element*           sDocCanvas       = nullptr;
  static inline int                      sWheelListenerId = -1;

  static void _registerActive(glint_datepicker_window* w, glint_element* docCanvas)
  {
    _unregisterActive(nullptr);
    sActiveInstance = w;
    sDocCanvas      = docCanvas;
    if (docCanvas && sWheelListenerId < 0)
    {
      sWheelListenerId = docCanvas->addEventListener(
        "wheel",
        [](glint_event&) {
          if (sActiveInstance) sActiveInstance->hide();
        },
        /*useCapture=*/true);
    }
  }

  static void _unregisterActive(glint_datepicker_window* w)
  {
    if (w && w != sActiveInstance) return;
    if (sDocCanvas && sWheelListenerId >= 0)
    {
      sDocCanvas->removeEventListener(sWheelListenerId);
      sWheelListenerId = -1;
    }
    sActiveInstance = nullptr;
    sDocCanvas      = nullptr;
  }

  int                              mYear = 2024, mMonth = 1, mDay = 1;
  RECT                             mAnchorRect      = { 100, 100, 114, 114 };
  std::function<void(int,int,int)> mOnChange;
  std::function<void()>            mOnClosed;
  glint_datepicker*                mPicker          = nullptr;
  bool                             mClosedFired      = false;
  bool                             mDestroyRequested = false;

  void _reposition(RECT anchor)
  {
    const int W = defaultWidth(), H = defaultHeight(), kGap = 4;
    const RECT wa = glint_window_linux::screenWorkArea();

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top)        y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right)  x = wa.right - W;
    if (x < wa.left)       x = wa.left;

    setPanelFrameOrigin(x, y);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// macOS implementation
// ─────────────────────────────────────────────────────────────────────────────

#elif defined(__APPLE__) && TARGET_OS_IPHONE

// ── iOS stub implementation ────────────────────────────────────────────────
// iOS cannot host desktop popup windows. Keep a no-op API surface so shared
// components compile and run without linking macOS window symbols.
class glint_datepicker_window
{
public:
  static glint_datepicker_window* open(
    int,
    int,
    int,
    RECT,
    std::function<void(int,int,int)> = nullptr,
    std::function<void()> = nullptr)
  {
    static glint_datepicker_window sInstance;
    return &sInstance;
  }

  static void _registerActive(glint_datepicker_window*, glint_element*) {}
  static void _unregisterActive(glint_datepicker_window*) {}

  void reopen(int,
              int,
              int,
              RECT,
              std::function<void(int,int,int)>,
              std::function<void()>,
              glint_element* = nullptr)
  {
  }

  void hide() {}
  void destroy() {}
  bool isVisible() const { return false; }
};

#else

#include "../../../platform/glint_window.hpp"   // glint_window_mac + all components
#include "glint_datepicker.hpp"

#include <functional>

// =============================================================================
// glint_datepicker_window  (macOS)
// =============================================================================
class glint_datepicker_window : public glint_window_mac
{
public:
  static glint_datepicker_window* open(
    int  year, int month, int day,
    RECT anchorScreenRect,
    std::function<void(int,int,int)> onChange = nullptr,
    std::function<void()>            onClosed = nullptr)
  {
    auto* w = new glint_datepicker_window();
    w->mYear       = year;
    w->mMonth      = month;
    w->mDay        = day;
    w->mAnchorRect = anchorScreenRect;
    w->mOnChange   = std::move(onChange);
    w->mOnClosed   = std::move(onClosed);
    w->startThread();
    return w;
  }

  void reopen(int  year, int month, int day,
              RECT anchorScreenRect,
              std::function<void(int,int,int)> onChange,
              std::function<void()>            onClosed,
              glint_element*                   docCanvas = nullptr)
  {
    mYear       = year;
    mMonth      = month;
    mDay        = day;
    mAnchorRect = anchorScreenRect;
    mOnChange   = std::move(onChange);
    mOnClosed   = std::move(onClosed);
    mClosedFired = false;

    _registerActive(this, docCanvas);
    _reposition(mAnchorRect);
    if (mPicker) mPicker->setDate(mYear, mMonth, mDay);
    requestRedraw();
    showPanel();
  }

  void hide()
  {
    _unregisterActive(this);
    if (!mClosedFired) {
      mClosedFired = true;
      if (mOnClosed) mOnClosed();
    }
    hidePanel();
  }

  bool isVisible() const { return !mSuppressAutoClose; }

  void destroy()
  {
    mDestroyRequested = true;
    stopThread();
  }

  bool usePopupStyle()   const override { return true; }
  void onOutsideClick()  override       { hide(); }
  int  defaultWidth()    const override { return (int)glint_datepicker::kW; }
  int  defaultHeight()   const override { return (int)glint_datepicker::kH(); }
  const wchar_t* windowClassName() const override { return L"glint_date_picker"; }
  const wchar_t* windowTitle()     const override { return L""; }
  SkColor clearColor() const override { return SkColorSetARGB(0, 30, 30, 30); }

protected:
  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width           = "100%";
    wrap->style.height          = "100%";
    wrap->style.backgroundColor = glint_color(255, 30, 30, 30);
    wrap->style.borderRadius    = 8.f;
    wrap->style.overflow        = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_datepicker();
    mPicker->style.width  = "100%";
    mPicker->style.height = "100%";
    mPicker->setDate(mYear, mMonth, mDay);
    mPicker->onChange = [this](int y, int m, int d)
    {
      if (mOnChange) mOnChange(y, m, d);
      hide();   // auto-close after pick
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
  // ── Static registry storage ─────────────────────────────────────────────
  static inline glint_datepicker_window* sActiveInstance  = nullptr;
  static inline glint_element*           sDocCanvas       = nullptr;
  static inline int                      sWheelListenerId = -1;

  static void _registerActive(glint_datepicker_window* w, glint_element* docCanvas)
  {
    _unregisterActive(nullptr);
    sActiveInstance = w;
    sDocCanvas      = docCanvas;
    if (docCanvas && sWheelListenerId < 0)
    {
      sWheelListenerId = docCanvas->addEventListener(
        "wheel",
        [](glint_event&) {
          if (sActiveInstance) sActiveInstance->hide();
        },
        /*useCapture=*/true);
    }
  }

  static void _unregisterActive(glint_datepicker_window* w)
  {
    if (w && w != sActiveInstance) return;
    if (sDocCanvas && sWheelListenerId >= 0)
    {
      sDocCanvas->removeEventListener(sWheelListenerId);
      sWheelListenerId = -1;
    }
    sActiveInstance = nullptr;
    sDocCanvas      = nullptr;
  }

  int                              mYear = 2024, mMonth = 1, mDay = 1;
  RECT                             mAnchorRect      = { 100, 100, 114, 114 };
  std::function<void(int,int,int)> mOnChange;
  std::function<void()>            mOnClosed;
  glint_datepicker*                mPicker          = nullptr;
  bool                             mClosedFired      = false;
  bool                             mDestroyRequested = false;

  void _reposition(RECT anchor)
  {
    const int W = defaultWidth(), H = defaultHeight(), kGap = 4;
    const RECT wa = glint_window_mac::screenWorkArea();

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top)        y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right)  x = wa.right - W;
    if (x < wa.left)       x = wa.left;

    setPanelFrameOrigin(x, y);
  }
};

#endif
