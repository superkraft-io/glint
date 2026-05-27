#pragma once

#include "../../../default_style.hpp"
#include "../../../glint_document.hpp"
#include "../../../platform/glint_apple_platform.hpp"
#include "../../../platform/glint_platform_datetime_local_picker.hpp"
#include "../time/glint_time_value.hpp"

#if GLINT_PLATFORM_MAC
#include "glint_datetime_localpicker_window.hpp"
#endif

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

class glint_datetime_local_input : public glint_element
{
  struct _IconElem : glint_element
  {
    bool hovered = false;

    _IconElem()
    {
      className = "dli-icon";
      addEventListener("mouseenter", [this](glint_event&) { hovered = true; setDirty(false); });
      addEventListener("mouseleave", [this](glint_event&) { hovered = false; setDirty(false); });
    }

    const char* typeName() const override { return "datetime-local-input-icon-elem"; }

    void drawContent(glint_canvas& g) override
    {
      const glint_color col = hovered
        ? glint_color{255, 180, 180, 180}
        : glint_color{255, 110, 110, 110};
      const float cx = mRect.MW(), cy = mRect.MH(), iW = 14.f, iH = 12.f;
      const glint_rect box(cx - iW * .5f, cy - iH * .5f, cx + iW * .5f, cy + iH * .5f);
      g.DrawRoundRect(col, box, 1.5f, nullptr, 1.f);
      g.DrawLine(col, box.L, box.T + 3.5f, box.R, box.T + 3.5f, nullptr, 1.f);
      g.DrawLine(col, box.L + 3.f, box.T - 1.f, box.L + 3.f, box.T + 2.f, nullptr, 1.5f);
      g.DrawLine(col, box.R - 3.f, box.T - 1.f, box.R - 3.f, box.T + 2.f, nullptr, 1.5f);
      for (int row = 0; row < 2; ++row)
        for (int col2 = 0; col2 < 3; ++col2)
          g.FillCircle(col, box.L + 2.5f + col2 * 4.f, box.T + 5.5f + row * 3.f, 0.8f);
    }

    void DrawContentToCanvas(SkCanvas* canvas) override
    {
      glint_canvas g(canvas);
      drawContent(g);
    }
  };

public:
  std::function<void(int, int, int, int, int)> onChange;

  void setInteractionState(bool disabled, bool readonly)
  {
    mDisabled = disabled;
    mReadonly = readonly;

    const bool interactive = _isInteractive();

    style.pointerEvents = interactive ? "" : "none";
    style.cursor = interactive ? "" : "default";

    if (mIconEl)
    {
      mIconEl->hovered = false;
      mIconEl->style.cursor = interactive ? "" : "default";
    }

    if (!interactive && mRoot && mRoot->getFocusedNode() == this)
      mRoot->SetFocus(nullptr);

    if (!_canOpenPicker())
      _closePicker();

    if (!_canMutate())
    {
      mActiveField = -1;
      mTypedStr.clear();
      _refreshDisplay();
    }
  }

  ~glint_datetime_local_input() override
  {
#if GLINT_PLATFORM_IOS
    glint_platform::destroyDateTimeLocalPicker(mPlatformPicker_);
    mPlatformPicker_ = nullptr;
#endif
  }

  glint_datetime_local_input()
  {
    const std::time_t now = std::time(nullptr);
    const std::tm* lt = std::localtime(&now);
    mYear = lt->tm_year + 1900;
    mMonth = lt->tm_mon + 1;
    mDay = lt->tm_mday;
    mHour = lt->tm_hour;
    mMinute = lt->tm_min;

    mAcceptsFocus = true;
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();

    _buildChildren();

#if GLINT_PLATFORM_MAC
    if (!_sharedWindow())
    {
      RECT dummy{};
      _sharedWindow() = glint_datetime_localpicker_window::open(
        mYear, mMonth, mDay, mHour, mMinute, dummy, nullptr, nullptr);
    }
#endif
  }

  void setDateTime(int year, int month, int day, int hour, int minute)
  {
    mYear = std::max(1, year);
    mMonth = std::max(1, std::min(12, month));
    mDay = std::max(1, std::min(_daysInMonth(mYear, mMonth), day));
    mHour = glint_clamp_time_hour(hour);
    mMinute = glint_clamp_time_minute(minute);
    mHasValue = true;
    mTypedStr.clear();
    _refreshDisplay();
  }

  void clear()
  {
    mHasValue = false;
    mActiveField = -1;
    mTypedStr.clear();
    _refreshDisplay();
  }

  std::string getValue() const
  {
    if (!mHasValue)
      return {};

    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d", mYear, mMonth, mDay, mHour, mMinute);
    return buffer;
  }

  bool setValue(const std::string& value)
  {
    if (value.empty())
    {
      clear();
      return true;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (!_tryParseIsoDateTimeLocal(value, year, month, day, hour, minute))
      return false;

    setDateTime(year, month, day, hour, minute);
    return true;
  }

  const char* typeName() const override { return "datetime-local-input"; }

  void Layout(glint_canvas* g) override
  {
    glint_element::Layout(g);
    if (mPickerOpen)
    {
      if (mRect.L != mLastRectL || mRect.T != mLastRectT)
        _closePicker();
    }
    mLastRectL = mRect.L;
    mLastRectT = mRect.T;
  }

  void OnMouseDown(float x, float y, const glint_mouse_mod& mod) override
  {
    (void)x;
    (void)y;
#if GLINT_PLATFORM_IOS
    if (mod.R || !_isInteractive())
      return;
    if (mRoot)
      mRoot->SetFocus(this);
    if (!_canOpenPicker())
      return;
    _openPicker();
    return;
#else
    if (!_isInteractive())
      return;
    glint_element::OnMouseDown(x, y, mod);
#endif
  }

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (!_isInteractive())
      return false;

    if (key.vk == 0x1B && mPickerOpen) { _closePicker(); return true; }
    if (!_canMutate())
      return false;
    if ((key.alt && key.vk == 0x28) || key.vk == 0x73) { _togglePicker(); return true; }

    if (mActiveField < 0)
    {
      if (key.vk == 0x26 || key.vk == 0x28 || key.vk == 0x25 || key.vk == 0x27)
        { _setActiveField(kMonth); return true; }
      if (key.vk == 0x09)
        { _setActiveField(key.shift ? kMinute : kMonth); return true; }
      return false;
    }

    if (key.vk == 0x26) { _stepField(mActiveField, +1); return true; }
    if (key.vk == 0x28) { _stepField(mActiveField, -1); return true; }

    if (key.vk == 0x25)
    {
      if (mActiveField > kMonth) { mTypedStr.clear(); _setActiveField(mActiveField - 1); return true; }
      mActiveField = -1; mTypedStr.clear(); _refreshDisplay(); return false;
    }
    if (key.vk == 0x27)
    {
      if (mActiveField < kMinute) { mTypedStr.clear(); _setActiveField(mActiveField + 1); return true; }
      mActiveField = -1; mTypedStr.clear(); _refreshDisplay(); return false;
    }
    if (key.vk == 0x09)
    {
      if (!key.shift && mActiveField < kMinute)
        { mTypedStr.clear(); _setActiveField(mActiveField + 1); return true; }
      if (key.shift && mActiveField > kMonth)
        { mTypedStr.clear(); _setActiveField(mActiveField - 1); return true; }
      mActiveField = -1; mTypedStr.clear(); _refreshDisplay(); return false;
    }
    if (key.vk == 0x08) { mTypedStr.clear(); _refreshDisplay(); return true; }
    if (key.vk >= '0' && key.vk <= '9') { _handleDigit((char)key.vk); return true; }
    return false;
  }

  void onFocusGained() override
  {
    style.borderColor = glint_color{255, 74, 144, 217};
#if GLINT_PLATFORM_IOS
    if (_canOpenPicker())
      _openPicker();
#endif
    setDirty(false);
  }

  void onFocusLost() override
  {
    style.borderColor = glint_color(0, 0, 0, 0);
    mActiveField = -1;
    mTypedStr.clear();
    _refreshDisplay();
  }

private:
  static constexpr int kMonth = 0;
  static constexpr int kDay = 1;
  static constexpr int kYear = 2;
  static constexpr int kHour = 3;
  static constexpr int kMinute = 4;

  int mYear = 2024;
  int mMonth = 1;
  int mDay = 1;
  int mHour = 0;
  int mMinute = 0;
  bool mHasValue = false;
  bool mDisabled = false;
  bool mReadonly = false;
  int mActiveField = -1;
  bool mPickerOpen = false;
  float mLastRectL = 0.f;
  float mLastRectT = 0.f;
  std::string mTypedStr;

  glint_element* mFieldEls[5] = {};
  glint_element* mFieldTexts[5] = {};
  _IconElem* mIconEl = nullptr;
#if GLINT_PLATFORM_IOS
  glint_platform::datetime_local_picker_handle* mPlatformPicker_ = nullptr;
#endif

#if GLINT_PLATFORM_MAC
  static glint_datetime_localpicker_window*& _sharedWindow()
  {
    static glint_datetime_localpicker_window* sInstance = nullptr;
    return sInstance;
  }
#endif

  void _buildChildren()
  {
    auto makeField = [this](int idx) {
      auto* el = new glint_element();
      el->className = "dli-field";
      if (idx == kYear) el->style.width = 40.f;
      mFieldEls[idx] = el;

      auto* txt = new glint_element();
      txt->className = "dli-field-text";
      mFieldTexts[idx] = txt;
      el->addChild(txt);

      el->addEventListener("mousedown", [this, idx](glint_event&) {
        if (!_isInteractive())
          return;
        if (mRoot) mRoot->SetFocus(this);
#if GLINT_PLATFORM_IOS
        (void)idx;
        if (!_canOpenPicker())
          return;
        _openPicker();
#else
        if (!_canMutate())
          return;
        _setActiveField(idx);
#endif
      });
      addChild(el);
    };

    auto makeSep = [this](const char* text) {
      auto* sep = new glint_element();
      sep->className = "dli-sep";
      auto* lbl = new glint_element();
      lbl->className = "dli-sep-label";
      lbl->innerText = text;
      sep->addChild(lbl);
#if GLINT_PLATFORM_IOS
      sep->addEventListener("mousedown", [this](glint_event&) {
        if (!_isInteractive())
          return;
        if (mRoot) mRoot->SetFocus(this);
        if (!_canOpenPicker())
          return;
        _openPicker();
      });
#endif
      addChild(sep);
    };

    makeField(kMonth);
    makeSep("/");
    makeField(kDay);
    makeSep("/");
    makeField(kYear);

    auto* gap = new glint_element();
    gap->className = "dli-gap";
#if GLINT_PLATFORM_IOS
    gap->addEventListener("mousedown", [this](glint_event&) {
      if (!_isInteractive())
        return;
      if (mRoot) mRoot->SetFocus(this);
      if (!_canOpenPicker())
        return;
      _openPicker();
    });
#endif
    addChild(gap);

    makeField(kHour);
    makeSep(":");
    makeField(kMinute);

    auto* spacer = new glint_element();
    spacer->className = "dli-spacer";
#if GLINT_PLATFORM_IOS
    spacer->addEventListener("mousedown", [this](glint_event&) {
      if (!_isInteractive())
        return;
      if (mRoot) mRoot->SetFocus(this);
      if (!_canOpenPicker())
        return;
      _openPicker();
    });
#endif
    addChild(spacer);

    mIconEl = new _IconElem();
    mIconEl->addEventListener("mousedown", [this](glint_event&) {
      if (!_isInteractive())
        return;
      if (mRoot) mRoot->SetFocus(this);
#if GLINT_PLATFORM_IOS
      if (!_canOpenPicker())
        return;
      _openPicker();
#else
      if (!_canOpenPicker())
        return;
      _togglePicker();
#endif
    });
    addChild(mIconEl);

    _refreshDisplay();
  }

  void _refreshDisplay()
  {
    char bufM[8]; std::snprintf(bufM, sizeof(bufM), "%02d", mMonth);
    char bufD[8]; std::snprintf(bufD, sizeof(bufD), "%02d", mDay);
    char bufY[8]; std::snprintf(bufY, sizeof(bufY), "%04d", mYear);
    char bufH[8]; std::snprintf(bufH, sizeof(bufH), "%02d", mHour);
    char bufMin[8]; std::snprintf(bufMin, sizeof(bufMin), "%02d", mMinute);

    const char* bufs[5] = { bufM, bufD, bufY, bufH, bufMin };
    if (!mHasValue)
      bufs[0] = bufs[1] = bufs[2] = bufs[3] = bufs[4] = "";

    for (int i = 0; i < 5; ++i)
    {
      if (mFieldTexts[i]) mFieldTexts[i]->innerText = bufs[i];
      if (!mFieldEls[i]) continue;
      if (i == mActiveField)
      {
        mFieldEls[i]->style.backgroundColor = glint_color{255, 26, 115, 232};
        if (mFieldTexts[i]) mFieldTexts[i]->style.color = glint_color{255, 255, 255, 255};
      }
      else
      {
        mFieldEls[i]->style.backgroundColor = "";
        if (mFieldTexts[i]) mFieldTexts[i]->style.color = "";
      }
    }
    setDirty(false);
  }

  void _setActiveField(int field)
  {
    if (mActiveField != field) mTypedStr.clear();
    mActiveField = field;
    _refreshDisplay();
  }

  bool _isInteractive() const { return !mDisabled && !mReadonly; }
  bool _canMutate() const { return _isInteractive(); }
  bool _canOpenPicker() const { return _isInteractive(); }

  void _togglePicker() { mPickerOpen ? _closePicker() : _openPicker(); }

  RECT _anchorScreenRect() const
  {
#if GLINT_PLATFORM_MAC
    const glint_point pos = mRoot ? getPosition(&mRoot->mCanvas) : getPosition();
    const float cl = pos.x;
    const float ct = pos.y;
    const float bW = mRect.W();
    const float bH = mRect.H();
    return (mRoot && mRoot->macWindow)
      ? mRoot->macWindow->contentRectToScreen(cl, ct, bW, bH)
      : RECT{};
#else
    return RECT{};
#endif
  }

  void _openPicker()
  {
    if (!_canOpenPicker())
      return;

#if GLINT_PLATFORM_IOS
    if (mPickerOpen)
      return;

    mPickerOpen = true;
    const RECT popupAnchor = _anchorScreenRect();
    mPlatformPicker_ = glint_platform::reopenDateTimeLocalPicker(
      mPlatformPicker_,
      mYear,
      mMonth,
      mDay,
      mHour,
      mMinute,
      popupAnchor,
      [this](int year, int month, int day, int hour, int minute) {
        if (!_canMutate())
          return;
        setDateTime(year, month, day, hour, minute);
        if (onChange) onChange(mYear, mMonth, mDay, mHour, mMinute);
      },
      nullptr,
      nullptr,
      [this]() {
        mPickerOpen = false;
        setDirty(false);
      });
    return;
#elif GLINT_PLATFORM_MAC
    mPickerOpen = true;
    const RECT anchor = _anchorScreenRect();

    auto onChanged = [this](int year, int month, int day, int hour, int minute)
    {
      if (!_canMutate())
        return;
      setDateTime(year, month, day, hour, minute);
      if (onChange) onChange(mYear, mMonth, mDay, mHour, mMinute);
    };
    auto onClosed = [this]() { mPickerOpen = false; };

    _sharedWindow()->reopen(mYear, mMonth, mDay, mHour, mMinute,
                            anchor, onChanged, onClosed,
                            mRoot ? &mRoot->mCanvas : nullptr);
    return;
#else
    return;
#endif
  }

  void _closePicker()
  {
#if GLINT_PLATFORM_IOS
    glint_platform::hideDateTimeLocalPicker(mPlatformPicker_);
#elif GLINT_PLATFORM_MAC
    if (mPickerOpen && _sharedWindow())
      _sharedWindow()->hide();
#endif
    mPickerOpen = false;
  }

  void _stepField(int field, int delta)
  {
    if (!_canMutate())
      return;

    if (!mHasValue)
      mHasValue = true;
    mTypedStr.clear();

    if (field == kMonth)
    {
      mMonth += delta;
      if (mMonth < 1) mMonth = 12;
      if (mMonth > 12) mMonth = 1;
      mDay = std::min(mDay, _daysInMonth(mYear, mMonth));
    }
    else if (field == kDay)
    {
      const int maxDay = _daysInMonth(mYear, mMonth);
      mDay += delta;
      if (mDay < 1) mDay = maxDay;
      if (mDay > maxDay) mDay = 1;
    }
    else if (field == kYear)
    {
      mYear = std::max(1, mYear + delta);
      mDay = std::min(mDay, _daysInMonth(mYear, mMonth));
    }
    else if (field == kHour)
    {
      mHour = (mHour + delta + 24) % 24;
    }
    else
    {
      mMinute = (mMinute + delta + 60) % 60;
    }

    _refreshDisplay();
    if (onChange) onChange(mYear, mMonth, mDay, mHour, mMinute);
  }

  void _handleDigit(char ch)
  {
    if (!_canMutate())
      return;

    if (!mHasValue)
      mHasValue = true;
    const int digit = ch - '0';

    if (mActiveField == kMonth)
    {
      mTypedStr += ch;
      if (mTypedStr.size() == 1 && digit > 1)
      {
        mMonth = digit;
        mTypedStr.clear();
        _setActiveField(kDay);
      }
      else if (mTypedStr.size() == 2)
      {
        const int value = std::stoi(mTypedStr);
        if (value >= 1 && value <= 12) mMonth = value;
        mTypedStr.clear();
        _setActiveField(kDay);
      }
    }
    else if (mActiveField == kDay)
    {
      mTypedStr += ch;
      const int maxDay = _daysInMonth(mYear, mMonth);
      if (mTypedStr.size() == 1 && digit > 3)
      {
        if (digit <= maxDay) mDay = digit;
        mTypedStr.clear();
        _setActiveField(kYear);
      }
      else if (mTypedStr.size() == 2)
      {
        const int value = std::stoi(mTypedStr);
        if (value >= 1 && value <= maxDay) mDay = value;
        mTypedStr.clear();
        _setActiveField(kYear);
      }
    }
    else if (mActiveField == kYear)
    {
      if (mTypedStr.size() >= 4) mTypedStr.clear();
      mTypedStr += ch;
      if (mTypedStr.size() == 4)
      {
        const int value = std::stoi(mTypedStr);
        if (value >= 1)
        {
          mYear = value;
          mDay = std::min(mDay, _daysInMonth(mYear, mMonth));
        }
        mTypedStr.clear();
        _setActiveField(kHour);
      }
    }
    else if (mActiveField == kHour)
    {
      mTypedStr += ch;
      if (mTypedStr.size() == 1 && digit > 2)
      {
        mHour = digit;
        mTypedStr.clear();
        _setActiveField(kMinute);
      }
      else if (mTypedStr.size() == 2)
      {
        const int value = std::stoi(mTypedStr);
        if (value >= 0 && value <= 23) mHour = value;
        mTypedStr.clear();
        _setActiveField(kMinute);
      }
    }
    else if (mActiveField == kMinute)
    {
      mTypedStr += ch;
      if (mTypedStr.size() == 1 && digit > 5)
      {
        mMinute = digit;
        mTypedStr.clear();
      }
      else if (mTypedStr.size() == 2)
      {
        const int value = std::stoi(mTypedStr);
        if (value >= 0 && value <= 59) mMinute = value;
        mTypedStr.clear();
      }
    }

    _refreshDisplay();
    if (onChange) onChange(mYear, mMonth, mDay, mHour, mMinute);
  }

  static int _daysInMonth(int year, int month)
  {
    static const int kDays[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2)
    {
      const bool leapYear = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
      return leapYear ? 29 : 28;
    }
    return (month >= 1 && month <= 12) ? kDays[month] : 30;
  }

  static bool _tryParseIsoDate(const std::string& value, int& year, int& month, int& day)
  {
    char trailing = 0;
    if (std::sscanf(value.c_str(), "%d-%d-%d%c", &year, &month, &day, &trailing) != 3)
      return false;
    if (year < 1 || month < 1 || month > 12)
      return false;
    if (day < 1 || day > _daysInMonth(year, month))
      return false;
    return true;
  }

  static bool _tryParseIsoDateTimeLocal(const std::string& value,
                                        int& year,
                                        int& month,
                                        int& day,
                                        int& hour,
                                        int& minute)
  {
    if (value.size() < 16 || value[10] != 'T')
      return false;

    if (!_tryParseIsoDate(value.substr(0, 10), year, month, day))
      return false;

    if (!glint_parse_time_value(value.substr(11), hour, minute))
      return false;

    return true;
  }
};