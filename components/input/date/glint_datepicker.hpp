#pragma once

/**
 * glint_datepicker.hpp
 *
 *   glint_datepicker  - standalone inline month-calendar built from child elements.
 *
 * All visual/layout styles live in default_style.hpp under the CSS classes:
 *   datepicker, .dp-header, .dp-nav-btn, .dp-nav-btn-label, .dp-header-label,
 *   .dp-header-text, .dp-dow-row, .dp-dow-cell, .dp-dow-label, .dp-grid,
 *   .dp-row, .dp-cell, .dp-cell-label, .dp-spacer, .dp-today-btn, .dp-today-label
 *
 * For the <input type="date"> spinner see glint_date_input.hpp.
 * For a native OS popup window see glint_datepicker_window.hpp.
 */

#include "../../../glint_element.hpp"
#include "../../../default_style.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>

// =============================================================================
// glint_datepicker  -- inline month-calendar built from child elements
// =============================================================================
class glint_datepicker : public glint_element
{
public:
  std::function<void(int,int,int)> onChange;

  int navYear = 2024, navMonth = 1;
  int selYear = 0, selMonth = 0, selDay = 0;

  // Sizing constants used by glint_datepicker_window
  static constexpr float kW     = 224.f;
  static constexpr float kPad   =   8.f;
  static constexpr float kHdrH  =  32.f;
  static constexpr float kDowH  =  20.f;
  static constexpr float kCellW =  30.f;
  static constexpr float kCellH =  28.f;
  static constexpr float kFtrH  =  32.f;
  static float kH() { return kPad + kHdrH + kDowH + 6.f*kCellH + kPad + kFtrH + kPad; }

  glint_datepicker()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
    mAcceptsFocus = true;
    _build();
  }

  const char* typeName() const override { return "datepicker"; }

  void setDate(int year, int month, int day)
  {
    navYear  = selYear  = year;
    navMonth = selMonth = std::max(1, std::min(12, month));
    selDay   = std::max(1, std::min(_daysInMonth(year, selMonth), day));
    _refresh();
  }

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (key.vk == 0x25) { _prevMonth(); return true; }
    if (key.vk == 0x27) { _nextMonth(); return true; }
    if (key.vk == 0x26) { if (selDay>0) _moveSelDay(-7); else _prevMonth(); return true; }
    if (key.vk == 0x28) { if (selDay>0) _moveSelDay(+7); else _nextMonth(); return true; }
    if (key.vk == 0x21) { _prevMonth(); return true; }
    if (key.vk == 0x22) { _nextMonth(); return true; }
    if (key.vk == 0x24) { selYear=navYear; selMonth=navMonth; selDay=1; _refresh(); return true; }
    if (key.vk == 0x23) { selYear=navYear; selMonth=navMonth; selDay=_daysInMonth(navYear,navMonth); _refresh(); return true; }
    if (key.vk == 0x0D && selDay>0) { if (onChange) onChange(selYear,selMonth,selDay); return true; }
    return false;
  }

private:
  glint_element* mHeaderLabelText = nullptr;
  glint_element* mCells[42]       = {};
  glint_element* mCellLabels[42]  = {};
  glint_element* mTodayBtn        = nullptr;

  glint_element* _makeNavBtn(const char* label)
  {
    auto* btn = new glint_element();
    btn->className = "dp-nav-btn";
    auto* lbl = new glint_element();
    lbl->className = "dp-nav-btn-label";
    lbl->innerText = label;
    btn->addChild(lbl);
    btn->addEventListener("mouseenter", [btn](glint_event&){
      btn->style.backgroundColor = glint_color{255,55,55,55};
      btn->setDirty(false);
    });
    btn->addEventListener("mouseleave", [btn](glint_event&){
      btn->style.backgroundColor = "";
      btn->setDirty(false);
    });
    return btn;
  }

  void _build()
  {
    auto* hdr = new glint_element();
    hdr->className = "dp-header";
    addChild(hdr);

    auto* prevBtn = _makeNavBtn("<");
    prevBtn->addEventListener("click", [this](glint_event&){ _prevMonth(); });
    hdr->addChild(prevBtn);

    auto* hdrLabel = new glint_element();
    hdrLabel->className = "dp-header-label";
    hdr->addChild(hdrLabel);

    mHeaderLabelText = new glint_element();
    mHeaderLabelText->className = "dp-header-text";
    hdrLabel->addChild(mHeaderLabelText);

    auto* nextBtn = _makeNavBtn(">");
    nextBtn->addEventListener("click", [this](glint_event&){ _nextMonth(); });
    hdr->addChild(nextBtn);

    static const char* kDow[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    auto* dowRow = new glint_element();
    dowRow->className = "dp-dow-row";
    addChild(dowRow);

    for (int c = 0; c < 7; ++c)
    {
      auto* col = new glint_element();
      col->className = "dp-dow-cell";
      auto* lbl = new glint_element();
      lbl->className = "dp-dow-label";
      lbl->innerText = kDow[c];
      col->addChild(lbl);
      dowRow->addChild(col);
    }

    auto* grid = new glint_element();
    grid->className = "dp-grid";
    addChild(grid);

    for (int row = 0; row < 6; ++row)
    {
      auto* rowEl = new glint_element();
      rowEl->className = "dp-row";
      grid->addChild(rowEl);

      for (int col = 0; col < 7; ++col)
      {
        const int idx = row * 7 + col;
        auto* cell = new glint_element();
        cell->className = "dp-cell";
        auto* numLbl = new glint_element();
        numLbl->className = "dp-cell-label";
        cell->addChild(numLbl);
        cell->addEventListener("click",      [this, idx](glint_event&){ _onCellClick(idx); });
        cell->addEventListener("mouseenter", [this, idx](glint_event&){ _onCellEnter(idx); });
        cell->addEventListener("mouseleave", [this, idx](glint_event&){ _onCellLeave(idx); });
        mCells[idx]      = cell;
        mCellLabels[idx] = numLbl;
        rowEl->addChild(cell);
      }
    }

    auto* spacer = new glint_element();
    spacer->className = "dp-spacer";
    addChild(spacer);

    mTodayBtn = new glint_element();
    mTodayBtn->className = "dp-today-btn";
    auto* todayLbl = new glint_element();
    todayLbl->className = "dp-today-label";
    todayLbl->innerText = "Today";
    mTodayBtn->addChild(todayLbl);
    mTodayBtn->addEventListener("click", [this](glint_event&){
      const std::time_t n = std::time(nullptr);
      const std::tm* lt = std::localtime(&n);
      _pickDate(lt->tm_year+1900, lt->tm_mon+1, lt->tm_mday);
    });
    mTodayBtn->addEventListener("mouseenter", [this](glint_event&){
      mTodayBtn->setDirty(false);
    });
    mTodayBtn->addEventListener("mouseleave", [this](glint_event&){
      mTodayBtn->setDirty(false);
    });
    addChild(mTodayBtn);

    _refresh();
  }

  void _refresh()
  {
    if (mHeaderLabelText)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%s %d", _monthName(navMonth), navYear);
      mHeaderLabelText->innerText = buf;
      mHeaderLabelText->setDirty(false);
    }

    const std::time_t now = std::time(nullptr);
    const std::tm* lt = std::localtime(&now);
    const int todayY = lt->tm_year+1900, todayM = lt->tm_mon+1, todayD = lt->tm_mday;

    const int firstDow = _dayOfWeek(navYear, navMonth, 1);
    const int daysThis = _daysInMonth(navYear, navMonth);
    int day = 1 - firstDow;

    for (int i = 0; i < 42; ++i, ++day)
    {
      auto* cell = mCells[i];
      auto* lbl  = mCellLabels[i];
      if (!cell || !lbl) continue;

      int cellD, cellM, cellY; bool inMonth;
      if (day < 1)
      {
        const int pm = navMonth>1 ? navMonth-1 : 12;
        const int py = navMonth>1 ? navYear : navYear-1;
        cellD = _daysInMonth(py,pm)+day; cellM=pm; cellY=py; inMonth=false;
      }
      else if (day > daysThis)
      {
        cellD = day-daysThis;
        cellM = navMonth<12 ? navMonth+1 : 1;
        cellY = navMonth<12 ? navYear : navYear+1;
        inMonth = false;
      }
      else { cellD=day; cellM=navMonth; cellY=navYear; inMonth=true; }

      cell->tag = cellY*10000 + cellM*100 + cellD;
      char buf[4]; std::snprintf(buf, sizeof(buf), "%d", cellD);
      lbl->innerText = buf;

      const bool isSel   = (cellY==selYear && cellM==selMonth && cellD==selDay);
      const bool isToday = (cellY==todayY  && cellM==todayM  && cellD==todayD);

      if (isSel) {
        cell->style.backgroundColor = glint_color{255,26,115,232};
        cell->style.borderColor     = glint_color{255,26,115,232};
        cell->style.borderWidth     = 0.f;
        lbl->style.color            = glint_color{255,255,255,255};
      } else if (isToday) {
        cell->style.backgroundColor = "";
        cell->style.borderColor     = glint_color{255,26,115,232};
        cell->style.borderWidth     = 1.f;
        lbl->style.color            = glint_color{255,230,230,230};
      } else {
        cell->style.backgroundColor = "";
        cell->style.borderWidth     = 0.f;
        lbl->style.color = inMonth ? glint_color{255,230,230,230}
                                    : glint_color{255,80,80,80};
      }
      cell->setDirty(false);
      lbl->setDirty(false);
    }
    setDirty(false);
  }

  void _onCellClick(int idx)
  {
    auto* cell = mCells[idx]; if (!cell) return;
    const int enc = cell->tag;
    _pickDate(enc/10000, (enc/100)%100, enc%100);
  }

  void _onCellEnter(int idx)
  {
    auto* cell = mCells[idx]; if (!cell) return;
    const int enc = cell->tag;
    const int y=enc/10000, m=(enc/100)%100, d=enc%100;
    if (!(y==selYear && m==selMonth && d==selDay)) {
      cell->style.backgroundColor = glint_color{255,55,55,55};
      cell->setDirty(false);
    }
  }

  void _onCellLeave(int idx)
  {
    auto* cell = mCells[idx]; if (!cell) return;
    const int enc = cell->tag;
    const int y=enc/10000, m=(enc/100)%100, d=enc%100;
    if (!(y==selYear && m==selMonth && d==selDay)) {
      cell->style.backgroundColor = "";
      cell->setDirty(false);
    }
  }

  void _pickDate(int y, int m, int d)
  {
    selYear=navYear=y; selMonth=navMonth=m; selDay=d;
    _refresh();
    if (onChange) onChange(y,m,d);
  }

  void _prevMonth() { if (--navMonth<1) { navMonth=12; --navYear; } _refresh(); }
  void _nextMonth() { if (++navMonth>12){ navMonth=1; ++navYear;  } _refresh(); }

  void _moveSelDay(int delta)
  {
    int d=selDay+delta, m=selMonth, y=selYear;
    while (d<1)              { --m; if(m<1){m=12;--y;} d+=_daysInMonth(y,m); }
    while (d>_daysInMonth(y,m)){ d-=_daysInMonth(y,m); ++m; if(m>12){m=1;++y;} }
    selYear=navYear=y; selMonth=navMonth=m; selDay=d;
    _refresh();
  }

  static int _daysInMonth(int y, int m)
  {
    static const int kD[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m==2){ bool l=(y%4==0&&y%100!=0)||(y%400==0); return l?29:28; }
    return (m>=1&&m<=12)?kD[m]:30;
  }

  static int _dayOfWeek(int y, int m, int d)
  {
    std::tm t={}; t.tm_year=y-1900; t.tm_mon=m-1; t.tm_mday=d;
    std::mktime(&t); return t.tm_wday;
  }

  static const char* _monthName(int m)
  {
    static const char* kN[]={"","January","February","March","April","May","June",
                              "July","August","September","October","November","December"};
    return (m>=1&&m<=12)?kN[m]:"?";
  }
};