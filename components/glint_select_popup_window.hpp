#pragma once

/**
 * glint_select_popup_window.hpp
 * The option list a glint_select drops down, drawn by glint (Win32): a
 * borderless topmost popup window as wide as the select, directly below it
 * (above when there is no room), listing the options with one highlighted
 * row that follows the mouse and the arrow keys — the way Chrome draws a
 * <select>'s popup, instead of a native context menu.
 *
 * Same persistent popup-window pattern as glint_datepicker_window: one shared
 * window on its own thread, shown and hidden. It hides when it loses
 * activation (a click anywhere else), on Escape, after a pick, and on a wheel
 * over the owning document (glint_picker_registry).
 *
 * Threads: show() runs on the owning document's thread; onPicked / onClosed
 * run on the popup's thread (the select hops back with ownerThreadPoster()).
 *
 * Other platforms: glint_select keeps the native menu.
 */

#if defined(_WIN32) || defined(OS_WIN)

#include "../glint_window.hpp"   // glint_window_win32 + all components
#include "input/glint_picker_registry.hpp"

#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class glint_select_popup_window : public glint_window_win32
{
public:
  struct request
  {
    std::vector<std::string> options;
    int                      selectedIndex = -1;
    RECT                     anchor{};        // the select on screen, device pixels
    std::string              fontFamily;
    float                    fontSize = 13.f;
    std::function<void(int)> onPicked;        // popup thread
    std::function<void()>    onClosed;        // popup thread; once per show()
  };

  /** The shared popup: created on first use, then kept alive (fast reopen). */
  static glint_select_popup_window* shared()
  {
    static glint_select_popup_window* sInstance = [] {
      auto* w = new glint_select_popup_window();
      w->startThread();   // returns once the window exists
      return w;
    }();
    return sInstance;
  }

  /** Show the list for `req`. Owning document's thread. */
  void show(request req, glint_element* docCanvas)
  {
    glint_picker_registry<glint_select_popup_window>::attachCanvas(docCanvas);
    {
      std::lock_guard<std::mutex> lk(mMtx);
      mPending = std::move(req);
    }
    if (HWND h = mHWNDAtom.load()) ::PostMessage(h, WM_SKUI_SHOW_SEL, 0, 0);
  }

  /** Close without picking. Any thread. */
  void hide()
  {
    if (HWND h = mHWNDAtom.load()) ::PostMessage(h, WM_SKUI_HIDE_SEL, 0, 0);
  }

protected:
  static constexpr UINT WM_SKUI_HIDE_SEL = WM_USER + 230;
  static constexpr UINT WM_SKUI_SHOW_SEL = WM_USER + 231;

  static constexpr float kRowH    = 26.f;   // logical px
  static constexpr int   kMaxRows = 12;     // taller lists scroll
  static constexpr float kPadY    = 4.f;

  LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM /*lp*/) override
  {
    if (msg == WM_SKUI_HIDE_SEL)
    {
      _close();
      return 0;
    }

    if (msg == WM_SKUI_SHOW_SEL)
    {
      request next;
      {
        std::lock_guard<std::mutex> lk(mMtx);
        next = std::move(mPending);
      }
      _close();   // a list still open for another select ends first
      mReq = std::move(next);
      glint_picker_registry<glint_select_popup_window>::setActive(this);
      _rebuild();
      _reposition();
      ::ShowWindow(mHWND, SW_SHOW);
      ::SetForegroundWindow(mHWND);   // so a click elsewhere deactivates (and closes) it
      if (mActive >= 0 && mActive < static_cast<int>(mRows.size())) mRows[mActive]->scrollIntoView();
      ::PostMessage(mHWND, WM_SKUI_REDRAW, 0, 0);
      return 0;
    }

    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
    {
      ::PostMessage(mHWND, WM_SKUI_HIDE_SEL, 0, 0);
      return 0;
    }

    return -1;
  }

  void onKeyDown(const glint_key_press& kp) override
  {
    const int n = static_cast<int>(mRows.size());
    switch (kp.vk)
    {
      case VK_ESCAPE: _close(); break;
      case VK_RETURN:
      case VK_SPACE:  if (mActive >= 0) _pick(mActive); break;
      case VK_UP:     _setActive(std::max(0, mActive - 1), true); break;
      case VK_DOWN:   _setActive(std::min(n - 1, mActive + 1), true); break;
      case VK_HOME:   _setActive(0, true); break;
      case VK_END:    _setActive(n - 1, true); break;
      default: break;
    }
  }

  const wchar_t* windowClassName() const override { return L"glint_select_popup"; }
  const wchar_t* windowTitle()     const override { return L""; }
  DWORD          windowStyle()     const override { return WS_POPUP; }
  DWORD          windowExStyle()   const override { return WS_EX_TOOLWINDOW; }
  int            defaultWidth()    const override { return 160; }
  int            defaultHeight()   const override { return 100; }
  COLORREF       bgColor()         const override { return RGB(43, 43, 43); }
  SkColor        clearColor()      const override { return SkColorSetARGB(0, 43, 43, 43); }
  bool useTransparency() const override { return true; }
  bool useGpu()          const override { return false; }
  bool showOnCreate()    const override { return false; }

  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;
    mOwnRoot->addStylesheetText(
      ".glint-select-list {"
      "  width: 100%; height: 100%; overflow-y: auto; padding: 4px 0;"
      "  background-color: #2b2b2b; border: 1px solid #414141; border-radius: 6px;"
      "  color: #dcdcdc; user-select: none; cursor: default;"
      "}"
      ".glint-select-row { height: 26px; padding: 4px 10px; white-space: nowrap; }"
      ".glint-select-row.active { background-color: #2f5f9e; color: #ffffff; }");

    mList = new glint_element();
    mList->className = "glint-select-list";
    mOwnRoot->mCanvas.addChild(mList);
  }

  void afterRun() override { delete this; }

private:
  std::mutex                   mMtx;
  request                      mPending;   // guarded by mMtx
  request                      mReq;       // popup thread only
  glint_element*               mList = nullptr;
  std::vector<glint_element*>  mRows;
  int                          mActive = -1;

  void _rebuild()
  {
    mList->clearChildren();
    mRows.clear();
    mList->style.fontFamily = mReq.fontFamily;
    mList->style.fontSize   = mReq.fontSize > 0.f ? mReq.fontSize : 13.f;
    for (int i = 0; i < static_cast<int>(mReq.options.size()); ++i)
    {
      auto* row = new glint_element();
      row->className = "glint-select-row";
      row->innerText = mReq.options[i];
      row->addEventListener("mouseenter", [this, i](glint_event&) { _setActive(i, false); });
      row->addEventListener("click",      [this, i](glint_event&) { _pick(i); });
      mList->addChild(row);
      mRows.push_back(row);
    }
    mActive = -1;
    _setActive(mReq.selectedIndex >= 0 ? mReq.selectedIndex : 0, false);
  }

  void _setActive(int i, bool scroll)
  {
    if (i < 0 || i >= static_cast<int>(mRows.size()) || i == mActive) return;
    if (mActive >= 0 && mActive < static_cast<int>(mRows.size()))
      mRows[mActive]->className = "glint-select-row";
    mActive = i;
    mRows[i]->className = "glint-select-row active";
    if (scroll) mRows[i]->scrollIntoView();
    ::PostMessage(mHWND, WM_SKUI_REDRAW, 0, 0);
  }

  void _pick(int i)
  {
    auto cb = mReq.onPicked;
    _close();
    if (cb) cb(i);
  }

  // Hide and report the close once per show().
  void _close()
  {
    if (!::IsWindowVisible(mHWND)) return;
    glint_picker_registry<glint_select_popup_window>::clearActive(this);
    ::ShowWindow(mHWND, SW_HIDE);
    auto cb = std::move(mReq.onClosed);
    mReq.onClosed = nullptr;
    mReq.onPicked = nullptr;
    if (cb) cb();
  }

  // As wide as the select; below it, or above when the monitor has no room.
  void _reposition()
  {
    const RECT a = mReq.anchor;
    const float dpr = deviceScaleForPoint((a.left + a.right) / 2, a.bottom);
    const int rows = std::min(static_cast<int>(mRows.size()), kMaxRows);
    const int W = std::max(1, static_cast<int>(a.right - a.left));
    const int H = static_cast<int>(std::lround((rows * kRowH + 2.f * kPadY + 2.f) * dpr));

    MONITORINFO mi{ sizeof(mi) };
    ::GetMonitorInfoW(::MonitorFromRect(&a, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;

    int y = a.bottom;
    if (y + H > wa.bottom && a.top - H >= wa.top) y = a.top - H;
    int x = a.left;
    if (x + W > wa.right) x = wa.right - W;
    if (x < wa.left)      x = wa.left;

    ::SetWindowPos(mHWND, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE);
  }
};

#endif
