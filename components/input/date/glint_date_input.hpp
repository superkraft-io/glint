#pragma once
/**
 * glint_date_input.hpp
 * <input type="date"> spinner backed by child elements (CSS-styleable).
 * Must be included AFTER glint_datepicker_window.hpp (see glint_builder.hpp).
 *
 * CSS class names exposed (all in default_style.hpp):
 *   date-input        — root container
 *   .di-field         — month / day field container  (year gets style.width=40 inline)
 *   .di-field-text    — text child inside each field
 *   .di-sep           — "/" separator container
 *   .di-sep-label     — text child inside separator
 *   .di-spacer        — flex-grow:1 gap before icon
 *   .di-icon          — calendar icon button (drawn, not text)
 */

#include "glint_datepicker_window.hpp"
#include "../../../default_style.hpp"
#include "../../../glint_document.hpp"
#include "../../../platform/glint_apple_platform.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>

class glint_date_input : public glint_element
{
  // ── Drawn calendar icon ───────────────────────────────────────────────────
  struct _IconElem : glint_element
  {
    bool hovered = false;

    _IconElem()
    {
      className = "di-icon";
      addEventListener("mouseenter", [this](glint_event&) { hovered = true;  setDirty(false); });
      addEventListener("mouseleave", [this](glint_event&) { hovered = false; setDirty(false); });
    }

    const char* typeName() const override { return "di-icon-elem"; }

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
  std::function<void(int /*year*/, int /*month*/, int /*day*/)> onChange;

  glint_date_input()
  {
    const std::time_t now = std::time(nullptr);
    const std::tm* lt    = std::localtime(&now);
    mYear  = lt->tm_year + 1900;
    mMonth = lt->tm_mon  + 1;
    mDay   = lt->tm_mday;

    mAcceptsFocus = true;
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();

    _buildChildren();

    // Pre-warm the shared popup window on first construction.
#if !GLINT_PLATFORM_IOS
    if (!_sharedWindow())
    {
      RECT dummy{};
      _sharedWindow() = glint_datepicker_window::open(
        mYear, mMonth, mDay, dummy, nullptr, nullptr);
    }
#endif
  }

  void setDate(int year, int month, int day)
  {
    mYear  = year;
    mMonth = std::max(1, std::min(12, month));
    mDay   = std::max(1, std::min(_daysInMonth(mYear, mMonth), day));
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
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", mYear, mMonth, mDay);
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
    if (!_tryParseIsoDate(value, year, month, day))
      return false;

    setDate(year, month, day);
    return true;
  }

  int year()  const { return mYear;  }
  int month() const { return mMonth; }
  int day()   const { return mDay;   }

  const char* typeName() const override { return "date-input"; }

  // Close popup when our screen position changes (ancestor scrolled/moved).
  void Layout(glint_canvas* g) override
  {
    glint_element::Layout(g);
    if (mCalendarOpen)
    {
      if (mRect.L != mLastRectL || mRect.T != mLastRectT)
        _closeCalendar();
    }
    mLastRectL = mRect.L;
    mLastRectT = mRect.T;
  }

  // -- Keyboard ---------------------------------------------------------------
  bool OnKeyDown(const glint_key_press& key) override
  {
    if (key.vk == 0x1B && mCalendarOpen) { _closeCalendar(); return true; }
    if ((key.alt && key.vk == 0x28) || key.vk == 0x73) { _toggleCalendar(); return true; }

    if (mActiveField < 0)
    {
      if (key.vk==0x26||key.vk==0x28||key.vk==0x25||key.vk==0x27)
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
      if ( key.shift && mActiveField > kMonth)
        { mTypedStr.clear(); _setActiveField(mActiveField - 1); return true; }
      mActiveField = -1; mTypedStr.clear(); _refreshDisplay(); return false;
    }
    if (key.vk == 0x08) { mTypedStr.clear(); _refreshDisplay(); return true; }
    if (key.vk >= '0' && key.vk <= '9') { _handleDigit((char)key.vk); return true; }
    return false;
  }

  // -- Focus ------------------------------------------------------------------
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
  static constexpr int kMonth = 0, kDay = 1, kYear = 2;
  int  mYear = 2024, mMonth = 1, mDay = 1;
  bool mHasValue = false;
  int  mActiveField   = -1;
  bool mCalendarOpen  = false;
  float mLastRectL = 0.f, mLastRectT = 0.f;
  std::string mTypedStr;

  glint_element* mFieldEls  [3] = {};  // month, day, year container
  glint_element* mFieldTexts[3] = {};  // text child of each field
  _IconElem*     mIconEl        = nullptr;

  static glint_datepicker_window*& _sharedWindow()
  {
    static glint_datepicker_window* sInstance = nullptr;
    return sInstance;
  }

  // ── Build element tree ─────────────────────────────────────────────────────
  void _buildChildren()
  {
    auto makeField = [this](int idx) {
      auto* el = new glint_element();
      el->className = "di-field";
      if (idx == kYear) el->style.width = 40.f;
      mFieldEls[idx] = el;

      auto* txt = new glint_element();
      txt->className = "di-field-text";
      mFieldTexts[idx] = txt;
      el->addChild(txt);

      el->addEventListener("mousedown", [this, idx](glint_event&) {
        if (mRoot) mRoot->SetFocus(this);
        _setActiveField(idx);
      });
      addChild(el);
    };

    auto makeSep = [this]() {
      auto* sep = new glint_element();
      sep->className = "di-sep";
      auto* lbl = new glint_element();
      lbl->className = "di-sep-label";
      lbl->innerText = "/";
      sep->addChild(lbl);
      addChild(sep);
    };

    makeField(kMonth);
    makeSep();
    makeField(kDay);
    makeSep();
    makeField(kYear);

    // Spacer pushes icon to the right end.
    auto* spacer = new glint_element();
    spacer->className = "di-spacer";
    addChild(spacer);

    mIconEl = new _IconElem();
    mIconEl->addEventListener("mousedown", [this](glint_event&) {
      if (mRoot) mRoot->SetFocus(this);
      _toggleCalendar();
    });
    addChild(mIconEl);

    _refreshDisplay();
  }

  // ── Sync text and highlight state to child elements ────────────────────────
  void _refreshDisplay()
  {
    char bufM[8]; std::snprintf(bufM, sizeof(bufM), "%02d", mMonth);
    char bufD[8]; std::snprintf(bufD, sizeof(bufD), "%02d", mDay);
    char bufY[8]; std::snprintf(bufY, sizeof(bufY), "%04d", mYear);

    const char* bufs[3] = { bufM, bufD, bufY };
    if (!mHasValue)
      bufs[0] = bufs[1] = bufs[2] = "";

    for (int i = 0; i < 3; ++i)
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

  void _setActiveField(int f)
  {
    if (mActiveField != f) mTypedStr.clear();
    mActiveField = f;
    _refreshDisplay();
  }

  // ── Calendar open / close ──────────────────────────────────────────────────
  void _toggleCalendar() { mCalendarOpen ? _closeCalendar() : _openCalendar(); }

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
    return { bottomLeft.x,
             bottomLeft.y - (LONG)bH,
             bottomLeft.x + (LONG)bW,
             bottomLeft.y };
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

  void _openCalendar()
  {
#if GLINT_PLATFORM_IOS
    return;
#endif

    mCalendarOpen = true;
    const RECT anchor = _anchorScreenRect();

    auto onPicked = [this](int y, int m, int d)
    {
      setDate(y, m, d);
      mCalendarOpen = false;
      if (mRoot) mRoot->SetFocus(this);
      if (onChange) onChange(mYear, mMonth, mDay);
    };
    auto onClosed = [this]() { mCalendarOpen = false; };

    _sharedWindow()->reopen(mYear, mMonth, mDay, anchor, onPicked, onClosed,
                            mRoot ? &mRoot->mCanvas : nullptr);
  }

  void _closeCalendar()
  {
#if GLINT_PLATFORM_IOS
    mCalendarOpen = false;
    return;
#endif

    if (!mCalendarOpen) return;
    mCalendarOpen = false;
    _sharedWindow()->hide();
  }

  // ── Step / type ────────────────────────────────────────────────────────────
  void _stepField(int f, int delta)
  {
    if (!mHasValue)
      mHasValue = true;
    mTypedStr.clear();
    if (f == kMonth)
    {
      mMonth += delta;
      if (mMonth <  1) mMonth = 12;
      if (mMonth > 12) mMonth =  1;
      mDay = std::min(mDay, _daysInMonth(mYear, mMonth));
    }
    else if (f == kDay)
    {
      const int mx = _daysInMonth(mYear, mMonth);
      mDay += delta;
      if (mDay <  1) mDay = mx;
      if (mDay > mx) mDay =  1;
    }
    else
    {
      mYear = std::max(1, mYear + delta);
      mDay  = std::min(mDay, _daysInMonth(mYear, mMonth));
    }
    _refreshDisplay();
    if (onChange) onChange(mYear, mMonth, mDay);
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
        { mMonth = digit; mTypedStr.clear(); _setActiveField(kDay); }
      else if (mTypedStr.size() == 2)
        { int v = std::stoi(mTypedStr); if (v >= 1 && v <= 12) mMonth = v; mTypedStr.clear(); _setActiveField(kDay); }
    }
    else if (mActiveField == kDay)
    {
      mTypedStr += ch;
      const int mx = _daysInMonth(mYear, mMonth);
      if (mTypedStr.size() == 1 && digit > 3)
        { if (digit <= mx) mDay = digit; mTypedStr.clear(); _setActiveField(kYear); }
      else if (mTypedStr.size() == 2)
        { int v = std::stoi(mTypedStr); if (v >= 1 && v <= mx) mDay = v; mTypedStr.clear(); _setActiveField(kYear); }
    }
    else if (mActiveField == kYear)
    {
      if (mTypedStr.size() >= 4) mTypedStr.clear();
      mTypedStr += ch;
      if (mTypedStr.size() == 4)
      {
        int v = std::stoi(mTypedStr);
        if (v >= 1) { mYear = v; mDay = std::min(mDay, _daysInMonth(mYear, mMonth)); }
        mTypedStr.clear();
      }
    }
    _refreshDisplay();
    if (onChange) onChange(mYear, mMonth, mDay);
  }

  static int _daysInMonth(int y, int m)
  {
    static const int kD[] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2) { bool l = (y%4==0 && y%100!=0) || (y%400==0); return l ? 29 : 28; }
    return (m >= 1 && m <= 12) ? kD[m] : 30;
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
};
