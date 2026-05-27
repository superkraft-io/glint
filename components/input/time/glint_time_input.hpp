#pragma once

#include "glint_time_value.hpp"
#include "glint_timepicker_window.hpp"
#include "../../../default_style.hpp"
#include "../../../glint_document.hpp"
#include "../../../platform/glint_apple_platform.hpp"
#include "../../../platform/glint_platform_timepicker.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

class glint_time_input : public glint_element
{
  struct _IconElem : glint_element
  {
    bool hovered = false;

    _IconElem()
    {
      className = "ti-icon";
      addEventListener("mouseenter", [this](glint_event&) { hovered = true; setDirty(false); });
      addEventListener("mouseleave", [this](glint_event&) { hovered = false; setDirty(false); });
    }

    const char* typeName() const override { return "time-input-icon-elem"; }

    void drawContent(glint_canvas& g) override
    {
      const glint_color col = hovered
        ? glint_color{255, 180, 180, 180}
        : glint_color{255, 110, 110, 110};
      const float cx = mRect.MW();
      const float cy = mRect.MH();
      g.DrawCircle(col, cx, cy, 6.5f, nullptr, 1.2f);
      g.DrawLine(col, cx, cy, cx, cy - 3.5f, nullptr, 1.2f);
      g.DrawLine(col, cx, cy, cx + 2.8f, cy + 1.8f, nullptr, 1.2f);
    }

    void DrawContentToCanvas(SkCanvas* canvas) override
    {
      glint_canvas g(canvas);
      drawContent(g);
    }
  };

public:
  std::function<void(int, int)> onChange;

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

  ~glint_time_input() override
  {
#if GLINT_PLATFORM_IOS
    glint_platform::destroyTimePicker(mPlatformPicker_);
    mPlatformPicker_ = nullptr;
#endif
  }

  glint_time_input()
  {
    const std::time_t now = std::time(nullptr);
    const std::tm* lt = std::localtime(&now);
    mHour = lt->tm_hour;
    mMinute = lt->tm_min;

    mAcceptsFocus = true;
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();

    _buildChildren();

#if !GLINT_PLATFORM_IOS
    if (!_sharedWindow())
    {
      RECT dummy{};
      _sharedWindow() = glint_timepicker_window::open(mHour, mMinute, dummy, nullptr, nullptr);
    }
#endif
  }

  void setTime(int hour, int minute)
  {
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
    return glint_format_time_value(mHour, mMinute);
  }

  bool setValue(const std::string& value)
  {
    if (value.empty())
    {
      clear();
      return true;
    }

    int hour = 0;
    int minute = 0;
    if (!glint_parse_time_value(value, hour, minute))
      return false;

    setTime(hour, minute);
    return true;
  }

  int hour() const { return mHour; }
  int minute() const { return mMinute; }

  const char* typeName() const override { return "time-input"; }

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

    if ((key.alt && key.vk == 0x28) || key.vk == 0x73) { _togglePicker(); return true; }
    if (mPickerOpen && _shouldRouteKeyToPicker(key) && _sharedWindow()->handleKey(key))
      return true;

    if (mActiveField < 0)
    {
      if (key.vk == 0x26 || key.vk == 0x28 || key.vk == 0x25 || key.vk == 0x27)
        { _setActiveField(kHour); return true; }
      if (key.vk == 0x09)
        { _setActiveField(key.shift ? kMinute : kHour); return true; }
      if (key.vk >= '0' && key.vk <= '9')
      {
        _setActiveField(kHour);
        _handleDigit((char)key.vk);
        return true;
      }
      return false;
    }

    if (key.vk == 0x26) { _stepField(mActiveField, +1); return true; }
    if (key.vk == 0x28) { _stepField(mActiveField, -1); return true; }

    if (key.vk == 0x25)
    {
      if (mActiveField > kHour) { mTypedStr.clear(); _setActiveField(mActiveField - 1); return true; }
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
      if (key.shift && mActiveField > kHour)
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
  static constexpr int kHour = 0, kMinute = 1;

  int mHour = 0;
  int mMinute = 0;
  bool mHasValue = false;
  bool mDisabled = false;
  bool mReadonly = false;
  int mActiveField = -1;
  bool mPickerOpen = false;
  float mLastRectL = 0.f, mLastRectT = 0.f;
  std::string mTypedStr;

  glint_element* mFieldEls[2] = {};
  glint_element* mFieldTexts[2] = {};
  _IconElem* mIconEl = nullptr;
#if GLINT_PLATFORM_IOS
  glint_platform::timepicker_handle* mPlatformPicker_ = nullptr;
#endif

  static glint_timepicker_window*& _sharedWindow()
  {
    static glint_timepicker_window* sInstance = nullptr;
    return sInstance;
  }

  void _buildChildren()
  {
    auto makeField = [this](int idx) {
      auto* el = new glint_element();
      el->className = "ti-field";
      mFieldEls[idx] = el;

      auto* txt = new glint_element();
      txt->className = "ti-field-text";
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

    makeField(kHour);

    auto* sep = new glint_element();
    sep->className = "ti-sep";
    auto* sepLbl = new glint_element();
    sepLbl->className = "ti-sep-label";
    sepLbl->innerText = ":";
    sep->addChild(sepLbl);
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

    makeField(kMinute);

    auto* spacer = new glint_element();
    spacer->className = "ti-spacer";
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
    char bufH[8]; std::snprintf(bufH, sizeof(bufH), "%02d", mHour);
    char bufM[8]; std::snprintf(bufM, sizeof(bufM), "%02d", mMinute);

    const char* bufs[2] = { bufH, bufM };
    if (!mHasValue)
      bufs[0] = bufs[1] = "";

    for (int i = 0; i < 2; ++i)
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
    const glint_point pos = mRoot ? getPosition(&mRoot->mCanvas) : getPosition();
    const float cl = pos.x;
    const float ct = pos.y;
    const float bW = mRect.W(), bH = mRect.H();

#if defined(_WIN32) || defined(OS_WIN)
    POINT bottomLeft{ (LONG)cl, (LONG)(ct + bH) };
    if (HWND hwnd = mRoot ? mRoot->hwnd : nullptr)
      ::ClientToScreen(hwnd, &bottomLeft);
    return { bottomLeft.x, bottomLeft.y - (LONG)bH, bottomLeft.x + (LONG)bW, bottomLeft.y };
#elif defined(__linux__)
    return (mRoot && mRoot->linuxWindow)
      ? mRoot->linuxWindow->contentRectToScreen(cl, ct, bW, bH)
      : RECT{};
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
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
    mPlatformPicker_ = glint_platform::reopenTimePicker(
      mPlatformPicker_,
      mHour,
      mMinute,
      popupAnchor,
      [this](int hour, int minute) {
        if (!_canMutate())
          return;
        setTime(hour, minute);
        if (onChange) onChange(mHour, mMinute);
      },
      [this]() {
        if (!_canMutate())
          return;
        clear();
        if (onChange) onChange(mHour, mMinute);
      },
      [this]() {
        mPickerOpen = false;
        setDirty(false);
      });
    return;
#endif

    mPickerOpen = true;
    const RECT anchor = _anchorScreenRect();

    auto onChanged = [this](int hour, int minute)
    {
      if (!_canMutate())
        return;
      setTime(hour, minute);
      if (onChange) onChange(mHour, mMinute);
    };
    auto onClosed = [this]() { mPickerOpen = false; };

    _sharedWindow()->reopen(mHour, mMinute, anchor, onChanged, onClosed,
                            mRoot ? &mRoot->mCanvas : nullptr);
  }

  void _closePicker()
  {
#if GLINT_PLATFORM_IOS
    glint_platform::hideTimePicker(mPlatformPicker_);
    mPickerOpen = false;
    return;
#endif

    if (!mPickerOpen) return;
    mPickerOpen = false;
    _sharedWindow()->hide();
  }

  void _stepField(int field, int delta)
  {
    if (!_canMutate())
      return;

    if (!mHasValue)
      mHasValue = true;
    mTypedStr.clear();

    if (field == kHour)
      mHour = (mHour + delta + 24) % 24;
    else
      mMinute = (mMinute + delta + 60) % 60;

    _refreshDisplay();
    if (onChange) onChange(mHour, mMinute);
  }

  static bool _shouldRouteKeyToPicker(const glint_key_press& key)
  {
    switch (key.vk)
    {
      case 0x25:
      case 0x26:
      case 0x27:
      case 0x28:
      case 0x21:
      case 0x22:
      case 0x24:
      case 0x23:
      case 0x09:
      case 0x0D:
      case 0x1B:
        return true;
      default:
        return false;
    }
  }

  void _handleDigit(char ch)
  {
    if (!_canMutate())
      return;

    if (!mHasValue)
      mHasValue = true;
    const int digit = ch - '0';

    if (mActiveField == kHour)
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
    if (onChange) onChange(mHour, mMinute);
  }
};