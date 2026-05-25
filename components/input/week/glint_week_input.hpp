#pragma once
/**
 * glint_week_input.hpp
 * <input type="week"> spinner backed by child elements (CSS-styleable).
 */

#include "glint_iso_week.hpp"
#include "glint_weekpicker_window.hpp"
#include "../../../default_style.hpp"
#include "../../../platform/glint_apple_platform.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

class glint_week_input : public glint_element
{
  struct _IconElem : glint_element
  {
    bool hovered = false;

    _IconElem()
    {
      className = "wi-icon";
      addEventListener("mouseenter", [this](glint_event&) { hovered = true; setDirty(false); });
      addEventListener("mouseleave", [this](glint_event&) { hovered = false; setDirty(false); });
    }

    const char* typeName() const override { return "week-input-icon-elem"; }

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

  glint_week_input()
  {
    const std::time_t now = std::time(nullptr);
    const std::tm* lt = std::localtime(&now);
    const glint_iso_week_value iso = glint_iso_week_from_ymd(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    mWeekYear = iso.weekYear;
    mWeek = iso.week;

    mAcceptsFocus = true;
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();

    _buildChildren();

#if !GLINT_PLATFORM_IOS
    if (!_sharedWindow())
    {
      RECT dummy{};
      _sharedWindow() = glint_weekpicker_window::open(mWeekYear, mWeek, dummy, nullptr, nullptr);
    }
#endif
  }

  void setWeek(int weekYear, int week)
  {
    mWeekYear = std::max(1, weekYear);
    mWeek = std::max(1, std::min(glint_iso_weeks_in_year(mWeekYear), week));
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
    return glint_format_iso_week_value(mWeekYear, mWeek);
  }

  bool setValue(const std::string& value)
  {
    if (value.empty())
    {
      clear();
      return true;
    }

    int weekYear = 0;
    int week = 0;
    if (!glint_parse_iso_week_value(value, weekYear, week))
      return false;

    setWeek(weekYear, week);
    return true;
  }

  int weekYear() const { return mWeekYear; }
  int week() const { return mWeek; }

  const char* typeName() const override { return "week-input"; }

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

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (key.vk == 0x1B && mPickerOpen) { _closePicker(); return true; }
    if ((key.alt && key.vk == 0x28) || key.vk == 0x73) { _togglePicker(); return true; }
    if (mPickerOpen && _shouldRouteKeyToPicker(key) && _sharedWindow()->handleKey(key))
      return true;

    if (mActiveField < 0)
    {
      if (key.vk == 0x26 || key.vk == 0x28 || key.vk == 0x25 || key.vk == 0x27)
        { _setActiveField(kWeek); return true; }
      if (key.vk == 0x09)
        { _setActiveField(key.shift ? kYear : kWeek); return true; }
      if (key.vk >= '0' && key.vk <= '9')
      {
        _setActiveField(kWeek);
        _handleDigit((char)key.vk);
        return true;
      }
      return false;
    }

    if (key.vk == 0x26) { _stepField(mActiveField, +1); return true; }
    if (key.vk == 0x28) { _stepField(mActiveField, -1); return true; }

    if (key.vk == 0x25)
    {
      if (mActiveField > kWeek) { mTypedStr.clear(); _setActiveField(mActiveField - 1); return true; }
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
      if (key.shift && mActiveField > kWeek)
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
  static constexpr int kWeek = 0, kYear = 1;

  int mWeekYear = 2024;
  int mWeek = 1;
  bool mHasValue = false;
  int mActiveField = -1;
  bool mPickerOpen = false;
  float mLastRectL = 0.f, mLastRectT = 0.f;
  std::string mTypedStr;

  glint_element* mFieldEls[2] = {};
  glint_element* mFieldTexts[2] = {};
  _IconElem* mIconEl = nullptr;

  static glint_weekpicker_window*& _sharedWindow()
  {
    static glint_weekpicker_window* sInstance = nullptr;
    return sInstance;
  }

  void _buildChildren()
  {
    auto makeField = [this](int idx) {
      auto* el = new glint_element();
      el->className = "wi-field";
      if (idx == kYear) el->style.width = 40.f;
      mFieldEls[idx] = el;

      auto* txt = new glint_element();
      txt->className = "wi-field-text";
      mFieldTexts[idx] = txt;
      el->addChild(txt);

      el->addEventListener("mousedown", [this, idx](glint_event&) {
        if (mRoot) mRoot->SetFocus(this);
        _setActiveField(idx);
      });
      addChild(el);
    };

    auto* prefix = new glint_element();
    prefix->className = "wi-prefix";
    auto* prefixLbl = new glint_element();
    prefixLbl->className = "wi-prefix-label";
    prefixLbl->innerText = "W";
    prefix->addChild(prefixLbl);
    addChild(prefix);

    makeField(kWeek);

    auto* sep = new glint_element();
    sep->className = "wi-sep";
    auto* sepLbl = new glint_element();
    sepLbl->className = "wi-sep-label";
    sepLbl->innerText = "/";
    sep->addChild(sepLbl);
    addChild(sep);

    makeField(kYear);

    auto* spacer = new glint_element();
    spacer->className = "wi-spacer";
    addChild(spacer);

    mIconEl = new _IconElem();
    mIconEl->addEventListener("mousedown", [this](glint_event&) {
      if (mRoot) mRoot->SetFocus(this);
      _togglePicker();
    });
    addChild(mIconEl);

    _refreshDisplay();
  }

  void _refreshDisplay()
  {
    char bufW[8]; std::snprintf(bufW, sizeof(bufW), "%02d", mWeek);
    char bufY[8]; std::snprintf(bufY, sizeof(bufY), "%04d", mWeekYear);

    const char* bufs[2] = { bufW, bufY };
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
    return;
#endif

    mPickerOpen = true;
    const RECT anchor = _anchorScreenRect();

    auto onPicked = [this](int weekYear, int week)
    {
      setWeek(weekYear, week);
      mPickerOpen = false;
      if (mRoot) mRoot->SetFocus(this);
      if (onChange) onChange(mWeekYear, mWeek);
    };
    auto onClosed = [this]() { mPickerOpen = false; };

    _sharedWindow()->reopen(mWeekYear, mWeek, anchor, onPicked, onClosed,
                            mRoot ? &mRoot->mCanvas : nullptr);
  }

  void _closePicker()
  {
#if GLINT_PLATFORM_IOS
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

    if (field == kWeek)
    {
      const glint_iso_week_value shifted = glint_shift_iso_week(mWeekYear, mWeek, delta);
      mWeekYear = shifted.weekYear;
      mWeek = shifted.week;
    }
    else
    {
      mWeekYear = std::max(1, mWeekYear + delta);
      mWeek = std::min(mWeek, glint_iso_weeks_in_year(mWeekYear));
    }

    _refreshDisplay();
    if (onChange) onChange(mWeekYear, mWeek);
  }

  static bool _shouldRouteKeyToPicker(const glint_key_press& key)
  {
    switch (key.vk)
    {
      case 0x25: // left
      case 0x26: // up
      case 0x27: // right
      case 0x28: // down
      case 0x21: // page up
      case 0x22: // page down
      case 0x24: // home
      case 0x23: // end
      case 0x0D: // enter
        return true;
      default:
        return false;
    }
  }

  void _handleDigit(char ch)
  {
    if (!mHasValue)
      mHasValue = true;
    const int digit = ch - '0';

    if (mActiveField == kWeek)
    {
      mTypedStr += ch;
      if (mTypedStr.size() == 1 && digit > 5)
      {
        mWeek = digit;
        mTypedStr.clear();
        _setActiveField(kYear);
      }
      else if (mTypedStr.size() == 2)
      {
        int parsed = std::stoi(mTypedStr);
        if (parsed >= 1 && parsed <= glint_iso_weeks_in_year(mWeekYear)) mWeek = parsed;
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
        int parsed = std::stoi(mTypedStr);
        if (parsed >= 1)
        {
          mWeekYear = parsed;
          mWeek = std::min(mWeek, glint_iso_weeks_in_year(mWeekYear));
        }
        mTypedStr.clear();
      }
    }

    _refreshDisplay();
    if (onChange) onChange(mWeekYear, mWeek);
  }
};