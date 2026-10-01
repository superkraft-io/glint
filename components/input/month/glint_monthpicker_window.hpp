#pragma once

/**
 * glint_monthpicker_window.hpp
 * A standalone native popup window hosting a single glint_monthpicker.
 */

#include "../../../platform/glint_apple_platform.hpp"

#if defined(_WIN32) || defined(OS_WIN)

#include "../../../glint_window.hpp"
#include "glint_monthpicker.hpp"

#include <functional>
#include <mutex>

class glint_monthpicker_window : public glint_window_win32
{
public:
  static glint_monthpicker_window* open(
    int year, int month,
    RECT anchorScreenRect,
    std::function<void(int, int)> onChange = nullptr,
    std::function<void()> onClosed = nullptr)
  {
    auto* w = new glint_monthpicker_window();
    {
      std::lock_guard<std::mutex> lk(w->mMtx);
      w->mYear = year;
      w->mMonth = month;
      w->mAnchorRect = anchorScreenRect;
      w->mOnChange = std::move(onChange);
      w->mOnClosed = std::move(onClosed);
    }
    w->startThread();
    return w;
  }

  static void _registerActive(glint_monthpicker_window* w, glint_element* docCanvas)
  {
    _unregisterActive(nullptr);
    sActiveInstance = w;
    sDocCanvas = docCanvas;
    if (docCanvas && sWheelListenerId < 0)
    {
      sWheelListenerId = docCanvas->addEventListener(
        "wheel",
        [](glint_event&) {
          if (sActiveInstance) sActiveInstance->hide();
        },
        true);
    }
  }

  static void _unregisterActive(glint_monthpicker_window* w)
  {
    if (w && w != sActiveInstance) return;
    if (sDocCanvas && sWheelListenerId >= 0)
    {
      sDocCanvas->removeEventListener(sWheelListenerId);
      sWheelListenerId = -1;
    }
    sActiveInstance = nullptr;
    sDocCanvas = nullptr;
  }

  void reopen(int year,
              int month,
              RECT anchorScreenRect,
              std::function<void(int, int)> onChange,
              std::function<void()> onClosed,
              glint_element* docCanvas = nullptr)
  {
    {
      std::lock_guard<std::mutex> lk(mMtx);
      mYear = year;
      mMonth = month;
      mAnchorRect = anchorScreenRect;
      mPendingOnChange = std::move(onChange);
      mPendingOnClosed = std::move(onClosed);
      mDocCanvas = docCanvas;
    }
    if (HWND h = mHWNDAtom.load())
      ::PostMessage(h, WM_SKUI_REOPEN_MP, 0, 0);
  }

  bool isVisible() const
  {
    HWND h = mHWNDAtom.load();
    return h && ::IsWindowVisible(h);
  }

  void hide()
  {
    if (HWND h = mHWNDAtom.load())
      ::PostMessage(h, WM_SKUI_HIDE_MP, 0, 0);
  }

  void destroy() { stopThread(); }

protected:
  static constexpr UINT WM_SKUI_HIDE_MP = WM_USER + 214;
  static constexpr UINT WM_SKUI_REOPEN_MP = WM_USER + 215;

  LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM) override
  {
    if (msg == WM_SKUI_HIDE_MP)
    {
      std::function<void()> cb;
      { std::lock_guard<std::mutex> lk(mMtx); cb = mOnClosed; }
      _unregisterActive(this);
      if (cb) cb();
      ::ShowWindow(mHWND, SW_HIDE);
      return 0;
    }

    if (msg == WM_SKUI_REOPEN_MP)
    {
      int y, m; RECT anchor; glint_element* docCanvas;
      {
        std::lock_guard<std::mutex> lk(mMtx);
        y = mYear; m = mMonth;
        anchor = mAnchorRect;
        docCanvas = mDocCanvas;
        mOnChange = std::move(mPendingOnChange);
        mOnClosed = std::move(mPendingOnClosed);
      }
      _registerActive(this, docCanvas);
      _reposition(anchor);
      if (mPicker) mPicker->setMonth(y, m);
      ::ShowWindow(mHWND, SW_SHOW);
      ::SetForegroundWindow(mHWND);
      ::PostMessage(mHWND, WM_SKUI_REDRAW, 0, 0);
      return 0;
    }

    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
    {
      ::PostMessage(mHWND, WM_SKUI_HIDE_MP, 0, 0);
      return 0;
    }

    return -1;
  }

  const wchar_t* windowClassName() const override { return L"glint_month_picker"; }
  const wchar_t* windowTitle() const override { return L""; }
  DWORD windowStyle() const override { return WS_POPUP; }
  DWORD windowExStyle() const override { return WS_EX_TOOLWINDOW; }
  int defaultWidth() const override { return (int)glint_monthpicker::kW; }
  int defaultHeight() const override { return (int)glint_monthpicker::kH(); }
  COLORREF bgColor() const override { return RGB(30, 30, 30); }
  SkColor clearColor() const override { return SkColorSetARGB(0, 30, 30, 30); }
  bool useTransparency() const override { return true; }
  bool useGpu() const override { return false; }
  bool showOnCreate() const override { return false; }

  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width = "100%";
    wrap->style.height = "100%";
    wrap->style.backgroundColor = glint_color(255, 30, 30, 30);
    wrap->style.borderRadius = 8.f;
    wrap->style.overflow = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_monthpicker();
    mPicker->style.width = "100%";
    mPicker->style.height = "100%";
    {
      std::lock_guard<std::mutex> lk(mMtx);
      mPicker->setMonth(mYear, mMonth);
    }
    mPicker->onChange = [this](int y, int m)
    {
      std::function<void(int, int)> cb;
      { std::lock_guard<std::mutex> lk(mMtx); cb = mOnChange; }
      if (cb) cb(y, m);
      if (HWND h = mHWNDAtom.load()) ::PostMessage(h, WM_SKUI_HIDE_MP, 0, 0);
    };
    wrap->addChild(mPicker);
  }

  void onCreated() override
  {
    RECT anchor;
    { std::lock_guard<std::mutex> lk(mMtx); anchor = mAnchorRect; }
    _reposition(anchor);
  }

  void afterRun() override { delete this; }

private:
  static inline glint_monthpicker_window* sActiveInstance = nullptr;
  static inline glint_element* sDocCanvas = nullptr;
  static inline int sWheelListenerId = -1;

  std::mutex mMtx;
  int mYear = 2024;
  int mMonth = 1;
  RECT mAnchorRect = { 100, 100, 114, 114 };
  glint_element* mDocCanvas = nullptr;
  std::function<void(int, int)> mOnChange;
  std::function<void()> mOnClosed;
  std::function<void(int, int)> mPendingOnChange;
  std::function<void()> mPendingOnClosed;
  glint_monthpicker* mPicker = nullptr;

  void _reposition(RECT anchor)
  {
    const int W = defaultWidth(), H = defaultHeight(), kGap = 4;
    RECT wa{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top) y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right) x = wa.right - W;
    if (x < wa.left) x = wa.left;

    ::SetWindowPos(mHWND, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
  }
};

#elif defined(__linux__)

#include "../../../platform/glint_window.hpp"
#include "glint_monthpicker.hpp"

#include <functional>

class glint_monthpicker_window : public glint_window_linux
{
public:
  static glint_monthpicker_window* open(
    int year, int month,
    RECT anchorScreenRect,
    std::function<void(int, int)> onChange = nullptr,
    std::function<void()> onClosed = nullptr)
  {
    auto* w = new glint_monthpicker_window();
    w->mYear = year;
    w->mMonth = month;
    w->mAnchorRect = anchorScreenRect;
    w->mOnChange = std::move(onChange);
    w->mOnClosed = std::move(onClosed);
    w->startThread();
    return w;
  }

  void reopen(int year,
              int month,
              RECT anchorScreenRect,
              std::function<void(int, int)> onChange,
              std::function<void()> onClosed,
              glint_element* docCanvas = nullptr)
  {
    mYear = year;
    mMonth = month;
    mAnchorRect = anchorScreenRect;
    mOnChange = std::move(onChange);
    mOnClosed = std::move(onClosed);
    mClosedFired = false;

    _registerActive(this, docCanvas);
    _reposition(mAnchorRect);
    if (mPicker) mPicker->setMonth(mYear, mMonth);
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

  bool usePopupStyle() const override { return true; }
  bool showOnCreate() const override { return false; }
  void onOutsideClick() override { hide(); }
  int defaultWidth() const override { return (int)glint_monthpicker::kW; }
  int defaultHeight() const override { return (int)glint_monthpicker::kH(); }
  const wchar_t* windowClassName() const override { return L"glint_month_picker"; }
  const wchar_t* windowTitle() const override { return L""; }
  SkColor clearColor() const override { return SkColorSetARGB(0, 30, 30, 30); }

protected:
  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width = "100%";
    wrap->style.height = "100%";
    wrap->style.backgroundColor = glint_color(255, 30, 30, 30);
    wrap->style.borderRadius = 8.f;
    wrap->style.overflow = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_monthpicker();
    mPicker->style.width = "100%";
    mPicker->style.height = "100%";
    mPicker->setMonth(mYear, mMonth);
    mPicker->onChange = [this](int y, int m)
    {
      if (mOnChange) mOnChange(y, m);
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
  static inline glint_monthpicker_window* sActiveInstance = nullptr;
  static inline glint_element* sDocCanvas = nullptr;
  static inline int sWheelListenerId = -1;

  static void _registerActive(glint_monthpicker_window* w, glint_element* docCanvas)
  {
    _unregisterActive(nullptr);
    sActiveInstance = w;
    sDocCanvas = docCanvas;
    if (docCanvas && sWheelListenerId < 0)
    {
      sWheelListenerId = docCanvas->addEventListener(
        "wheel",
        [](glint_event&) {
          if (sActiveInstance) sActiveInstance->hide();
        },
        true);
    }
  }

  static void _unregisterActive(glint_monthpicker_window* w)
  {
    if (w && w != sActiveInstance) return;
    if (sDocCanvas && sWheelListenerId >= 0)
    {
      sDocCanvas->removeEventListener(sWheelListenerId);
      sWheelListenerId = -1;
    }
    sActiveInstance = nullptr;
    sDocCanvas = nullptr;
  }

  int mYear = 2024;
  int mMonth = 1;
  RECT mAnchorRect = { 100, 100, 114, 114 };
  std::function<void(int, int)> mOnChange;
  std::function<void()> mOnClosed;
  glint_monthpicker* mPicker = nullptr;
  bool mClosedFired = false;
  bool mDestroyRequested = false;

  void _reposition(RECT anchor)
  {
    const int W = defaultWidth(), H = defaultHeight(), kGap = 4;
    const RECT wa = glint_window_linux::screenWorkArea();

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top) y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right) x = wa.right - W;
    if (x < wa.left) x = wa.left;

    setPanelFrameOrigin(x, y);
  }
};

#elif defined(__APPLE__) && TARGET_OS_IPHONE

class glint_monthpicker_window
{
public:
  static glint_monthpicker_window* open(
    int,
    int,
    RECT,
    std::function<void(int, int)> = nullptr,
    std::function<void()> = nullptr)
  {
    static glint_monthpicker_window sInstance;
    return &sInstance;
  }

  static void _registerActive(glint_monthpicker_window*, glint_element*) {}
  static void _unregisterActive(glint_monthpicker_window*) {}

  void reopen(int,
              int,
              RECT,
              std::function<void(int, int)>,
              std::function<void()>,
              glint_element* = nullptr)
  {
  }

  void hide() {}
  void destroy() {}
  bool isVisible() const { return false; }
};

#else

#include "../../../platform/glint_window.hpp"
#include "glint_monthpicker.hpp"

#include <functional>

class glint_monthpicker_window : public glint_window_mac
{
public:
  static glint_monthpicker_window* open(
    int year, int month,
    RECT anchorScreenRect,
    std::function<void(int, int)> onChange = nullptr,
    std::function<void()> onClosed = nullptr)
  {
    auto* w = new glint_monthpicker_window();
    w->mYear = year;
    w->mMonth = month;
    w->mAnchorRect = anchorScreenRect;
    w->mOnChange = std::move(onChange);
    w->mOnClosed = std::move(onClosed);
    w->startThread();
    return w;
  }

  void reopen(int year,
              int month,
              RECT anchorScreenRect,
              std::function<void(int, int)> onChange,
              std::function<void()> onClosed,
              glint_element* docCanvas = nullptr)
  {
    mYear = year;
    mMonth = month;
    mAnchorRect = anchorScreenRect;
    mOnChange = std::move(onChange);
    mOnClosed = std::move(onClosed);
    mClosedFired = false;

    _registerActive(this, docCanvas);
    _reposition(mAnchorRect);
    if (mPicker) mPicker->setMonth(mYear, mMonth);
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

  bool usePopupStyle() const override { return true; }
  void onOutsideClick() override { hide(); }
  int defaultWidth() const override { return (int)glint_monthpicker::kW; }
  int defaultHeight() const override { return (int)glint_monthpicker::kH(); }
  const wchar_t* windowClassName() const override { return L"glint_month_picker"; }
  const wchar_t* windowTitle() const override { return L""; }
  SkColor clearColor() const override { return SkColorSetARGB(0, 30, 30, 30); }

protected:
  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* wrap = new glint_element();
    wrap->style.width = "100%";
    wrap->style.height = "100%";
    wrap->style.backgroundColor = glint_color(255, 30, 30, 30);
    wrap->style.borderRadius = 8.f;
    wrap->style.overflow = "hidden";
    mOwnRoot->mCanvas.addChild(wrap);

    mPicker = new glint_monthpicker();
    mPicker->style.width = "100%";
    mPicker->style.height = "100%";
    mPicker->setMonth(mYear, mMonth);
    mPicker->onChange = [this](int y, int m)
    {
      if (mOnChange) mOnChange(y, m);
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
  static inline glint_monthpicker_window* sActiveInstance = nullptr;
  static inline glint_element* sDocCanvas = nullptr;
  static inline int sWheelListenerId = -1;

  static void _registerActive(glint_monthpicker_window* w, glint_element* docCanvas)
  {
    _unregisterActive(nullptr);
    sActiveInstance = w;
    sDocCanvas = docCanvas;
    if (docCanvas && sWheelListenerId < 0)
    {
      sWheelListenerId = docCanvas->addEventListener(
        "wheel",
        [](glint_event&) {
          if (sActiveInstance) sActiveInstance->hide();
        },
        true);
    }
  }

  static void _unregisterActive(glint_monthpicker_window* w)
  {
    if (w && w != sActiveInstance) return;
    if (sDocCanvas && sWheelListenerId >= 0)
    {
      sDocCanvas->removeEventListener(sWheelListenerId);
      sWheelListenerId = -1;
    }
    sActiveInstance = nullptr;
    sDocCanvas = nullptr;
  }

  int mYear = 2024;
  int mMonth = 1;
  RECT mAnchorRect = { 100, 100, 114, 114 };
  std::function<void(int, int)> mOnChange;
  std::function<void()> mOnClosed;
  glint_monthpicker* mPicker = nullptr;
  bool mClosedFired = false;
  bool mDestroyRequested = false;

  void _reposition(RECT anchor)
  {
    const int W = defaultWidth(), H = defaultHeight(), kGap = 4;
    const RECT wa = glint_window_mac::screenWorkArea();

    int y = anchor.bottom + kGap;
    if (y + H > wa.bottom) y = anchor.top - kGap - H;
    if (y < wa.top) y = wa.top;

    int x = anchor.left;
    if (x + W > wa.right) x = wa.right - W;
    if (x < wa.left) x = wa.left;

    setPanelFrameOrigin(x, y);
  }
};

#endif