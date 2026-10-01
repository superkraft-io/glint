#pragma once

/**
 * glint_monthpicker.hpp
 *
 *   glint_monthpicker  - standalone inline year/month picker built from child elements.
 *
 * All visual/layout styles live in default_style.hpp under the CSS classes:
 *   monthpicker, .mp-header, .mp-nav-btn, .mp-nav-btn-label, .mp-header-label,
 *   .mp-header-text, .mp-grid, .mp-row, .mp-cell, .mp-cell-label, .mp-spacer,
 *   .mp-today-btn, .mp-today-label
 *
 * For the <input type="month"> shell see glint_month_input.hpp.
 * For a native OS popup window see glint_monthpicker_window.hpp.
 */

#include "../../../glint_element.hpp"
#include "../../../default_style.hpp"

#include <algorithm>
#include <ctime>
#include <functional>
#include <string>

class glint_monthpicker : public glint_element
{
public:
  std::function<void(int, int)> onChange;

  int navYear = 2024;
  int selYear = 0;
  int selMonth = 0;

  static constexpr float kW = 224.f;
  static constexpr float kPad = 8.f;
  static constexpr float kHdrH = 32.f;
  static constexpr float kCellH = 34.f;
  static constexpr float kFtrH = 24.f;
  static float kH() { return kPad + kHdrH + 4.f * kCellH + kPad + kFtrH + kPad; }

  glint_monthpicker()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
    mAcceptsFocus = true;
    _build();
  }

  const char* typeName() const override { return "monthpicker"; }

  void setMonth(int year, int month)
  {
    navYear = selYear = std::max(1, year);
    selMonth = std::max(1, std::min(12, month));
    _refresh();
  }

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (key.vk == 0x25) { _moveSelMonth(-1); return true; }
    if (key.vk == 0x27) { _moveSelMonth(+1); return true; }
    if (key.vk == 0x26) { _moveSelMonth(-3); return true; }
    if (key.vk == 0x28) { _moveSelMonth(+3); return true; }
    if (key.vk == 0x21) { _prevYear(); return true; }
    if (key.vk == 0x22) { _nextYear(); return true; }
    if (key.vk == 0x24) { _pickMonth(navYear, 1); return true; }
    if (key.vk == 0x23) { _pickMonth(navYear, 12); return true; }
    if (key.vk == 0x0D && selMonth > 0) { if (onChange) onChange(selYear, selMonth); return true; }
    return false;
  }

private:
  glint_element* mHeaderLabelText = nullptr;
  glint_element* mCells[12] = {};
  glint_element* mCellLabels[12] = {};
  glint_element* mThisMonthBtn = nullptr;

  glint_element* _makeNavBtn(const char* label)
  {
    auto* btn = new glint_element();
    btn->className = "mp-nav-btn";
    auto* lbl = new glint_element();
    lbl->className = "mp-nav-btn-label";
    lbl->innerText = label;
    btn->addChild(lbl);
    btn->addEventListener("mouseenter", [btn](glint_event&) {
      btn->style.backgroundColor = glint_color{255, 55, 55, 55};
      btn->setDirty(false);
    });
    btn->addEventListener("mouseleave", [btn](glint_event&) {
      btn->style.backgroundColor = "";
      btn->setDirty(false);
    });
    return btn;
  }

  void _build()
  {
    auto* hdr = new glint_element();
    hdr->className = "mp-header";
    addChild(hdr);

    auto* prevBtn = _makeNavBtn("<");
    prevBtn->addEventListener("click", [this](glint_event&) { _prevYear(); });
    hdr->addChild(prevBtn);

    auto* hdrLabel = new glint_element();
    hdrLabel->className = "mp-header-label";
    hdr->addChild(hdrLabel);

    mHeaderLabelText = new glint_element();
    mHeaderLabelText->className = "mp-header-text";
    hdrLabel->addChild(mHeaderLabelText);

    auto* nextBtn = _makeNavBtn(">");
    nextBtn->addEventListener("click", [this](glint_event&) { _nextYear(); });
    hdr->addChild(nextBtn);

    auto* grid = new glint_element();
    grid->className = "mp-grid";
    addChild(grid);

    for (int row = 0; row < 4; ++row)
    {
      auto* rowEl = new glint_element();
      rowEl->className = "mp-row";
      grid->addChild(rowEl);

      for (int col = 0; col < 3; ++col)
      {
        const int idx = row * 3 + col;
        auto* cell = new glint_element();
        cell->className = "mp-cell";
        auto* lbl = new glint_element();
        lbl->className = "mp-cell-label";
        cell->addChild(lbl);
        cell->addEventListener("click", [this, idx](glint_event&) { _onCellClick(idx); });
        cell->addEventListener("mouseenter", [this, idx](glint_event&) { _onCellEnter(idx); });
        cell->addEventListener("mouseleave", [this, idx](glint_event&) { _onCellLeave(idx); });
        mCells[idx] = cell;
        mCellLabels[idx] = lbl;
        rowEl->addChild(cell);
      }
    }

    auto* spacer = new glint_element();
    spacer->className = "mp-spacer";
    addChild(spacer);

    mThisMonthBtn = new glint_element();
    mThisMonthBtn->className = "mp-today-btn";
    auto* thisMonthLbl = new glint_element();
    thisMonthLbl->className = "mp-today-label";
    thisMonthLbl->innerText = "This month";
    mThisMonthBtn->addChild(thisMonthLbl);
    mThisMonthBtn->addEventListener("click", [this](glint_event&) {
      const std::time_t now = std::time(nullptr);
      const std::tm* lt = std::localtime(&now);
      _pickMonth(lt->tm_year + 1900, lt->tm_mon + 1);
    });
    addChild(mThisMonthBtn);

    _refresh();
  }

  void _refresh()
  {
    if (mHeaderLabelText)
    {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%d", navYear);
      mHeaderLabelText->innerText = buf;
      mHeaderLabelText->setDirty(false);
    }

    const std::time_t now = std::time(nullptr);
    const std::tm* lt = std::localtime(&now);
    const int todayY = lt->tm_year + 1900;
    const int todayM = lt->tm_mon + 1;

    for (int i = 0; i < 12; ++i)
    {
      auto* cell = mCells[i];
      auto* lbl = mCellLabels[i];
      if (!cell || !lbl) continue;

      const int month = i + 1;
      cell->tag = navYear * 100 + month;
      lbl->innerText = _monthShortName(month);

      const bool isSel = navYear == selYear && month == selMonth;
      const bool isToday = navYear == todayY && month == todayM;

      if (isSel)
      {
        cell->style.backgroundColor = glint_color{255, 26, 115, 232};
        cell->style.borderColor = glint_color{255, 26, 115, 232};
        cell->style.borderWidth = 0.f;
        lbl->style.color = glint_color{255, 255, 255, 255};
      }
      else if (isToday)
      {
        cell->style.backgroundColor = "";
        cell->style.borderColor = glint_color{255, 26, 115, 232};
        cell->style.borderWidth = 1.f;
        lbl->style.color = glint_color{255, 230, 230, 230};
      }
      else
      {
        cell->style.backgroundColor = "";
        cell->style.borderWidth = 0.f;
        lbl->style.color = glint_color{255, 230, 230, 230};
      }
      cell->setDirty(false);
      lbl->setDirty(false);
    }
    setDirty(false);
  }

  void _onCellClick(int idx)
  {
    auto* cell = mCells[idx];
    if (!cell) return;
    const int enc = cell->tag;
    _pickMonth(enc / 100, enc % 100);
  }

  void _onCellEnter(int idx)
  {
    auto* cell = mCells[idx];
    if (!cell) return;
    const int enc = cell->tag;
    const int y = enc / 100;
    const int m = enc % 100;
    if (!(y == selYear && m == selMonth))
    {
      cell->style.backgroundColor = glint_color{255, 55, 55, 55};
      cell->setDirty(false);
    }
  }

  void _onCellLeave(int idx)
  {
    auto* cell = mCells[idx];
    if (!cell) return;
    const int enc = cell->tag;
    const int y = enc / 100;
    const int m = enc % 100;
    if (!(y == selYear && m == selMonth))
    {
      cell->style.backgroundColor = "";
      cell->setDirty(false);
    }
  }

  void _pickMonth(int year, int month)
  {
    navYear = selYear = std::max(1, year);
    selMonth = std::max(1, std::min(12, month));
    _refresh();
    if (onChange) onChange(selYear, selMonth);
  }

  void _prevYear() { navYear = std::max(1, navYear - 1); _refresh(); }
  void _nextYear() { ++navYear; _refresh(); }

  void _moveSelMonth(int delta)
  {
    int year = selYear > 0 ? selYear : navYear;
    int month = selMonth > 0 ? selMonth : 1;
    month += delta;
    while (month < 1) { month += 12; year = std::max(1, year - 1); }
    while (month > 12) { month -= 12; ++year; }
    navYear = selYear = year;
    selMonth = month;
    _refresh();
  }

  static const char* _monthShortName(int month)
  {
    static const char* kNames[] = {
      "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    return (month >= 1 && month <= 12) ? kNames[month] : "?";
  }
};