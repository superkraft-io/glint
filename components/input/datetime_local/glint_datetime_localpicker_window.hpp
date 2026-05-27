#pragma once

#include "../../../platform/glint_apple_platform.hpp"

#if GLINT_PLATFORM_MAC

#include "../../../platform/glint_window.hpp"
#include "../date/glint_datepicker.hpp"
#include "../time/glint_timepicker.hpp"

#include <algorithm>
#include <functional>

class glint_datetime_localpicker_window : public glint_window_mac
{
public:
  static glint_datetime_localpicker_window* open(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    RECT anchorScreenRect,
    std::function<void(int, int, int, int, int)> onChange = nullptr,
    std::function<void()> onClosed = nullptr)
  {
    auto* w = new glint_datetime_localpicker_window();
    w->mYear = year;
    w->mMonth = month;
    w->mDay = day;
    w->mHour = hour;
    w->mMinute = minute;
    w->mAnchorRect = anchorScreenRect;
    w->mOnChange = std::move(onChange);
    w->mOnClosed = std::move(onClosed);
    w->startThread();
    return w;
  }

  void reopen(int year,
              int month,
              int day,
              int hour,
              int minute,
              RECT anchorScreenRect,
              std::function<void(int, int, int, int, int)> onChange,
              std::function<void()> onClosed,
              glint_element* docCanvas = nullptr)
  {
    mYear = year;
    mMonth = month;
    mDay = day;
    mHour = hour;
    mMinute = minute;
    mAnchorRect = anchorScreenRect;
    mOnChange = std::move(onChange);
    mOnClosed = std::move(onClosed);
    mClosedFired = false;

    _registerActive(this, docCanvas);
    _reposition(mAnchorRect);
    if (mDatePicker)
      mDatePicker->setDate(mYear, mMonth, mDay);
    if (mTimePicker)
    {
      mTimePicker->setTime(mHour, mMinute);
      mTimePicker->focusHourList();
    }
    showPanel();
    requestRedraw();
    if (mOwnRoot && mTimePicker)
      mOwnRoot->SetFocus(mTimePicker);
  }

  void hide()
  {
    _unregisterActive(this);
    if (!mClosedFired)
    {
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
  int defaultWidth() const override { return static_cast<int>(glint_datepicker::kW + glint_timepicker::kW + 24.f); }
  int defaultHeight() const override { return static_cast<int>(std::max(glint_datepicker::kH(), glint_timepicker::kH()) + 16.f); }
  const wchar_t* windowClassName() const override { return L"glint_datetime_local_picker"; }
  const wchar_t* windowTitle() const override { return L""; }
  SkColor clearColor() const override { return SkColorSetARGB(0, 30, 30, 30); }

protected:
  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    auto* root = new glint_element();
    root->style.width = "100%";
    root->style.height = "100%";
    root->style.display = "flex";
    root->style.flexDirection = "row";
    root->style.alignItems = "flex-start";
    root->style.gap = 8.f;
    root->style.padding = 8.f;
    root->style.backgroundColor = glint_color(255, 30, 30, 30);
    root->style.borderRadius = 12.f;
    root->style.overflow = "hidden";
    mOwnRoot->mCanvas.addChild(root);

    mDatePicker = new glint_datepicker();
    mDatePicker->style.width = glint_datepicker::kW;
    mDatePicker->style.height = glint_datepicker::kH();
    mDatePicker->setDate(mYear, mMonth, mDay);
    mDatePicker->onChange = [this](int year, int month, int day)
    {
      mYear = year;
      mMonth = month;
      mDay = day;
      if (mOnChange) mOnChange(mYear, mMonth, mDay, mHour, mMinute);
      requestRedraw();
    };
    root->addChild(mDatePicker);

    mTimePicker = new glint_timepicker();
    mTimePicker->style.width = glint_timepicker::kW;
    mTimePicker->style.height = glint_timepicker::kH();
    mTimePicker->setTime(mHour, mMinute);
    mTimePicker->onChange = [this](int hour, int minute)
    {
      mHour = hour;
      mMinute = minute;
      if (mOnChange) mOnChange(mYear, mMonth, mDay, mHour, mMinute);
      requestRedraw();
    };
    root->addChild(mTimePicker);
  }

  void onCreated() override
  {
    _reposition(mAnchorRect);
    hidePanel();
  }

  void afterRun() override
  {
    if (!mDestroyRequested && !mClosedFired)
    {
      mClosedFired = true;
      if (mOnClosed) mOnClosed();
    }
    delete this;
  }

private:
  static inline glint_datetime_localpicker_window* sActiveInstance = nullptr;
  static inline glint_element* sDocCanvas = nullptr;
  static inline int sWheelListenerId = -1;

  static void _registerActive(glint_datetime_localpicker_window* w, glint_element* docCanvas)
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

  static void _unregisterActive(glint_datetime_localpicker_window* w)
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
  int mDay = 1;
  int mHour = 0;
  int mMinute = 0;
  RECT mAnchorRect = {100, 100, 114, 114};
  std::function<void(int, int, int, int, int)> mOnChange;
  std::function<void()> mOnClosed;
  glint_datepicker* mDatePicker = nullptr;
  glint_timepicker* mTimePicker = nullptr;
  bool mClosedFired = false;
  bool mDestroyRequested = false;

  void _reposition(RECT anchor)
  {
    const int W = defaultWidth();
    const int H = defaultHeight();
    const int kGap = 4;
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