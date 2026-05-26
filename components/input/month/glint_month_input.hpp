#pragma once
/**
 * glint_month_input.hpp
 * <input type="month"> spinner backed by child elements (CSS-styleable).
 */

#include "glint_monthpicker_window.hpp"
#include "../../../default_style.hpp"
#include "../../../glint_document.hpp"
#include "../../../platform/glint_apple_platform.hpp"
#include "../../../platform/glint_platform_monthpicker.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

class glint_month_input : public glint_element
{
  struct _IconElem : glint_element
  {
    bool hovered = false;

    _IconElem()
    {
      className = "mi-icon";
      addEventListener("mouseenter", [this](glint_event&) { hovered = true; setDirty(false); });
      addEventListener("mouseleave", [this](glint_event&) { hovered = false; setDirty(false); });
    }

    const char* typeName() const override { return "month-input-icon-elem"; }

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
  std::function<void(int, int)> onChange;

  ~glint_month_input() override
  {
#if GLINT_PLATFORM_IOS
    glint_platform::destroyMonthPicker(mPlatformPicker_);
    mPlatformPicker_ = nullptr;
#endif
  }

  glint_month_input()
  {
    const std::time_t now = std::time(nullptr);
    const std::tm* lt = std::localtime(&now);
    mYear = lt->tm_year + 1900;
    mMonth = lt->tm_mon + 1;

    mAcceptsFocus = true;
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();

    _buildChildren();

#if !GLINT_PLATFORM_IOS
    if (!_sharedWindow())
    {
      RECT dummy{};
      _sharedWindow() = glint_monthpicker_window::open(mYear, mMonth, dummy, nullptr, nullptr);
    }
#endif
  }

  void setMonth(int year, int month)
  {
    mYear = std::max(1, year);
    mMonth = std::max(1, std::min(12, month));
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

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d", mYear, mMonth);
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
    if (!_tryParseIsoMonth(value, year, month))
      return false;

    setMonth(year, month);
    return true;
  }

  int year() const { return mYear; }
  int month() const { return mMonth; }

  const char* typeName() const override { return "month-input"; }

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
    if (mod.R)
      return;
    if (mRoot)
      mRoot->SetFocus(this);
    _openPicker();
    return;
#else
    glint_element::OnMouseDown(x, y, mod);
#endif
  }

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (key.vk == 0x1B && mPickerOpen) { _closePicker(); return true; }
    if ((key.alt && key.vk == 0x28) || key.vk == 0x73) { _togglePicker(); return true; }

    if (mActiveField < 0)
    {
      if (key.vk == 0x26 || key.vk == 0x28 || key.vk == 0x25 || key.vk == 0x27)
        { _setActiveField(kMonth); return true; }
      if (key.vk == 0x09)
        { _setActiveField(key.shift ? kYear : kMonth); return true; }
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
      if (mActiveField < kYear) { mTypedStr.clear(); _setActiveField(mActiveField + 1); return true; }
      mActiveField = -1; mTypedStr.clear(); _refreshDisplay(); return false;
    }
    if (key.vk == 0x09)
    {
      if (!key.shift && mActiveField < kYear)
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
  static constexpr int kMonth = 0, kYear = 1;

  int mYear = 2024;
  int mMonth = 1;
  bool mHasValue = false;
  int mActiveField = -1;
  bool mPickerOpen = false;
  float mLastRectL = 0.f, mLastRectT = 0.f;
  std::string mTypedStr;

  glint_element* mFieldEls[2] = {};
  glint_element* mFieldTexts[2] = {};
  _IconElem* mIconEl = nullptr;
#if GLINT_PLATFORM_IOS
  glint_platform::monthpicker_handle* mPlatformPicker_ = nullptr;
#endif

  static glint_monthpicker_window*& _sharedWindow()
  {
    static glint_monthpicker_window* sInstance = nullptr;
    return sInstance;
  }

  void _buildChildren()
  {
    auto makeField = [this](int idx) {
      auto* el = new glint_element();
      el->className = "mi-field";
      if (idx == kYear) el->style.width = 40.f;
      mFieldEls[idx] = el;

      auto* txt = new glint_element();
      txt->className = "mi-field-text";
      mFieldTexts[idx] = txt;
      el->addChild(txt);

      el->addEventListener("mousedown", [this, idx](glint_event&) {
        if (mRoot) mRoot->SetFocus(this);
#if GLINT_PLATFORM_IOS
        (void)idx;
        _openPicker();
#else
        _setActiveField(idx);
#endif
      });
      addChild(el);
    };

    auto makeSep = [this]() {
      auto* sep = new glint_element();
      sep->className = "mi-sep";
      auto* lbl = new glint_element();
      lbl->className = "mi-sep-label";
      lbl->innerText = "/";
      sep->addChild(lbl);
#if GLINT_PLATFORM_IOS
      sep->addEventListener("mousedown", [this](glint_event&) {
        if (mRoot) mRoot->SetFocus(this);
        _openPicker();
      });
#endif
      addChild(sep);
    };

    makeField(kMonth);
    makeSep();
    makeField(kYear);

    auto* spacer = new glint_element();
    spacer->className = "mi-spacer";
#if GLINT_PLATFORM_IOS
    spacer->addEventListener("mousedown", [this](glint_event&) {
      if (mRoot) mRoot->SetFocus(this);
      _openPicker();
    });
#endif
    addChild(spacer);

    mIconEl = new _IconElem();
    mIconEl->addEventListener("mousedown", [this](glint_event&) {
      if (mRoot) mRoot->SetFocus(this);
#if GLINT_PLATFORM_IOS
      _openPicker();
#else
      _togglePicker();
#endif
    });
    addChild(mIconEl);

    _refreshDisplay();
  }

  void _refreshDisplay()
  {
    char bufM[8]; std::snprintf(bufM, sizeof(bufM), "%02d", mMonth);
    char bufY[8]; std::snprintf(bufY, sizeof(bufY), "%04d", mYear);

    const char* bufs[2] = { bufM, bufY };
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
#if GLINT_PLATFORM_IOS
    if (mPickerOpen)
      return;

    mPickerOpen = true;
    const RECT popupAnchor = _anchorScreenRect();
    mPlatformPicker_ = glint_platform::reopenMonthPicker(
      mPlatformPicker_,
      mYear,
      mMonth,
      popupAnchor,
      [this](int year, int month) {
        setMonth(year, month);
        if (onChange) onChange(mYear, mMonth);
      },
      nullptr,
      nullptr,
      [this]() {
        mPickerOpen = false;
        setDirty(false);
      });
    return;
#endif

    mPickerOpen = true;
    const RECT anchor = _anchorScreenRect();

    auto onPicked = [this](int y, int m)
    {
      setMonth(y, m);
      mPickerOpen = false;
      if (mRoot) mRoot->SetFocus(this);
      if (onChange) onChange(mYear, mMonth);
    };
    auto onClosed = [this]() { mPickerOpen = false; };

    _sharedWindow()->reopen(mYear, mMonth, anchor, onPicked, onClosed,
                            mRoot ? &mRoot->mCanvas : nullptr);
  }

  void _closePicker()
  {
#if GLINT_PLATFORM_IOS
    glint_platform::hideMonthPicker(mPlatformPicker_);
    mPickerOpen = false;
    return;
#endif

    if (!mPickerOpen) return;
    mPickerOpen = false;
    _sharedWindow()->hide();
  }

  void _stepField(int field, int delta)
  {
    if (!mHasValue)
      mHasValue = true;
    mTypedStr.clear();

    if (field == kMonth)
    {
      mMonth += delta;
      while (mMonth < 1) { mMonth += 12; mYear = std::max(1, mYear - 1); }
      while (mMonth > 12) { mMonth -= 12; ++mYear; }
    }
    else
    {
      mYear = std::max(1, mYear + delta);
    }

    _refreshDisplay();
    if (onChange) onChange(mYear, mMonth);
  }

  void _handleDigit(char ch)
  {
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
        _setActiveField(kYear);
      }
      else if (mTypedStr.size() == 2)
      {
        int value = std::stoi(mTypedStr);
        if (value >= 1 && value <= 12) mMonth = value;
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
        int value = std::stoi(mTypedStr);
        if (value >= 1) mYear = value;
        mTypedStr.clear();
      }
    }

    _refreshDisplay();
    if (onChange) onChange(mYear, mMonth);
  }

  static bool _tryParseIsoMonth(const std::string& value, int& year, int& month)
  {
    char trailing = 0;
    if (std::sscanf(value.c_str(), "%d-%d%c", &year, &month, &trailing) != 2)
      return false;
    if (year < 1 || month < 1 || month > 12)
      return false;
    return true;
  }
};