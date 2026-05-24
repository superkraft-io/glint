#pragma once

/**
 * glint_weekpicker.hpp
 *
 *   glint_weekpicker - standalone inline ISO week picker built from child elements.
 */

#include "glint_iso_week.hpp"
#include "../../../glint_element.hpp"
#include "../../../default_style.hpp"

#include <ctime>
#include <functional>
#include <string>

class glint_weekpicker : public glint_element
{
public:
  std::function<void(int, int)> onChange;

  int navYear = 2024;
  int navMonth = 1;
  int selWeekYear = 0;
  int selWeek = 0;

  static constexpr float kW = 272.f;
  static constexpr float kPad = 8.f;
  static constexpr float kHdrH = 32.f;
  static constexpr float kDowH = 20.f;
  static constexpr float kRowH = 28.f;
  static constexpr float kRowGap = 2.f;
  static constexpr float kFtrH = 24.f;
  static float kH() { return kPad + kHdrH + kDowH + 6.f * kRowH + 5.f * kRowGap + kPad + kFtrH + kPad; }

  glint_weekpicker()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
    mAcceptsFocus = true;
    _build();
  }

  const char* typeName() const override { return "weekpicker"; }

  void setWeek(int weekYear, int week)
  {
    const glint_ymd monday = glint_ymd_from_iso_week(weekYear, week, 1);
    selWeekYear = weekYear;
    selWeek = week;
    navYear = monday.year;
    navMonth = monday.month;
    _refresh();
  }

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (key.vk == 0x25 || key.vk == 0x26) { _moveSelWeek(-1); return true; }
    if (key.vk == 0x27 || key.vk == 0x28) { _moveSelWeek(+1); return true; }
    if (key.vk == 0x21) { _prevMonth(); return true; }
    if (key.vk == 0x22) { _nextMonth(); return true; }
    if (key.vk == 0x24) { _pickVisibleRow(0); return true; }
    if (key.vk == 0x23) { _pickVisibleRow(5); return true; }
    if (key.vk == 0x0D && selWeek > 0) { if (onChange) onChange(selWeekYear, selWeek); return true; }
    return false;
  }

private:
  glint_element* mHeaderLabelText = nullptr;
  glint_element* mRows[6] = {};
  glint_element* mWeekNumLabels[6] = {};
  glint_element* mDayLabels[42] = {};
  glint_element* mThisWeekBtn = nullptr;

  glint_element* _makeNavBtn(const char* label)
  {
    auto* btn = new glint_element();
    btn->className = "wp-nav-btn";
    auto* lbl = new glint_element();
    lbl->className = "wp-nav-btn-label";
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
    hdr->className = "wp-header";
    addChild(hdr);

    auto* prevBtn = _makeNavBtn("<");
    prevBtn->addEventListener("click", [this](glint_event&) { _prevMonth(); });
    hdr->addChild(prevBtn);

    auto* hdrLabel = new glint_element();
    hdrLabel->className = "wp-header-label";
    hdr->addChild(hdrLabel);

    mHeaderLabelText = new glint_element();
    mHeaderLabelText->className = "wp-header-text";
    hdrLabel->addChild(mHeaderLabelText);

    auto* nextBtn = _makeNavBtn(">");
    nextBtn->addEventListener("click", [this](glint_event&) { _nextMonth(); });
    hdr->addChild(nextBtn);

    auto* dowRow = new glint_element();
    dowRow->className = "wp-dow-row";
    addChild(dowRow);

    auto* weekHdr = new glint_element();
    weekHdr->className = "wp-weeknum-cell";
    auto* weekHdrLbl = new glint_element();
    weekHdrLbl->className = "wp-weeknum-label";
    weekHdrLbl->innerText = "Week";
    weekHdr->addChild(weekHdrLbl);
    dowRow->addChild(weekHdr);

    static const char* kDow[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    for (int c = 0; c < 7; ++c)
    {
      auto* cell = new glint_element();
      cell->className = "wp-dow-cell";
      auto* lbl = new glint_element();
      lbl->className = "wp-dow-label";
      lbl->innerText = kDow[c];
      cell->addChild(lbl);
      dowRow->addChild(cell);
    }

    auto* grid = new glint_element();
    grid->className = "wp-grid";
    addChild(grid);

    for (int row = 0; row < 6; ++row)
    {
      auto* rowEl = new glint_element();
      rowEl->className = "wp-row";
      rowEl->addEventListener("click", [this, row](glint_event&) { _pickVisibleRow(row); });
      rowEl->addEventListener("mouseenter", [this, row](glint_event&) { _onRowEnter(row); });
      rowEl->addEventListener("mouseleave", [this, row](glint_event&) { _onRowLeave(row); });
      mRows[row] = rowEl;
      grid->addChild(rowEl);

      auto* weekNumCell = new glint_element();
      weekNumCell->className = "wp-weeknum-cell";
      auto* weekNumLbl = new glint_element();
      weekNumLbl->className = "wp-weeknum-label";
      weekNumCell->addChild(weekNumLbl);
      mWeekNumLabels[row] = weekNumLbl;
      rowEl->addChild(weekNumCell);

      for (int col = 0; col < 7; ++col)
      {
        const int idx = row * 7 + col;
        auto* cell = new glint_element();
        cell->className = "wp-day-cell";
        auto* lbl = new glint_element();
        lbl->className = "wp-day-label";
        cell->addChild(lbl);
        mDayLabels[idx] = lbl;
        rowEl->addChild(cell);
      }
    }

    auto* spacer = new glint_element();
    spacer->className = "wp-spacer";
    addChild(spacer);

    mThisWeekBtn = new glint_element();
    mThisWeekBtn->className = "wp-today-btn";
    auto* thisWeekLbl = new glint_element();
    thisWeekLbl->className = "wp-today-label";
    thisWeekLbl->innerText = "This week";
    mThisWeekBtn->addChild(thisWeekLbl);
    mThisWeekBtn->addEventListener("click", [this](glint_event&) {
      const std::time_t now = std::time(nullptr);
      const std::tm* lt = std::localtime(&now);
      const glint_iso_week_value iso = glint_iso_week_from_ymd(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
      _pickWeek(iso.weekYear, iso.week);
    });
    addChild(mThisWeekBtn);

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
    const glint_iso_week_value todayIso = glint_iso_week_from_ymd(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);

    const glint_ymd firstVisibleMonday = _firstVisibleMonday();
    for (int row = 0; row < 6; ++row)
    {
      const glint_ymd monday = glint_add_days(firstVisibleMonday.year, firstVisibleMonday.month, firstVisibleMonday.day, row * 7);
      const glint_iso_week_value iso = glint_iso_week_from_ymd(monday.year, monday.month, monday.day);

      auto* rowEl = mRows[row];
      auto* weekNumLbl = mWeekNumLabels[row];
      if (!rowEl || !weekNumLbl) continue;

      rowEl->tag = iso.weekYear * 100 + iso.week;
      weekNumLbl->innerText = std::to_string(iso.week);

      const bool isSel = iso.weekYear == selWeekYear && iso.week == selWeek;
      const bool isTodayWeek = iso.weekYear == todayIso.weekYear && iso.week == todayIso.week;

      if (isSel)
      {
        rowEl->style.backgroundColor = glint_color{255, 26, 115, 232};
        rowEl->style.borderColor = glint_color{255, 26, 115, 232};
        rowEl->style.borderWidth = 0.f;
      }
      else if (isTodayWeek)
      {
        rowEl->style.backgroundColor = "";
        rowEl->style.borderColor = glint_color{255, 26, 115, 232};
        rowEl->style.borderWidth = 1.f;
      }
      else
      {
        rowEl->style.backgroundColor = "";
        rowEl->style.borderWidth = 0.f;
      }

      for (int col = 0; col < 7; ++col)
      {
        auto* dayLbl = mDayLabels[row * 7 + col];
        if (!dayLbl) continue;

        const glint_ymd day = glint_add_days(monday.year, monday.month, monday.day, col);
        dayLbl->innerText = std::to_string(day.day);
        const bool inMonth = day.month == navMonth && day.year == navYear;
        dayLbl->style.color = isSel
          ? glint_color{255, 255, 255, 255}
          : (inMonth ? glint_color{255, 230, 230, 230} : glint_color{255, 110, 110, 110});
        dayLbl->setDirty(false);
      }

      weekNumLbl->style.color = isSel ? glint_color{255, 255, 255, 255} : glint_color{255, 160, 160, 160};
      rowEl->setDirty(false);
      weekNumLbl->setDirty(false);
    }
    setDirty(false);
  }

  void _pickVisibleRow(int row)
  {
    auto* rowEl = mRows[row];
    if (!rowEl) return;
    const int enc = rowEl->tag;
    _pickWeek(enc / 100, enc % 100);
  }

  void _onRowEnter(int row)
  {
    auto* rowEl = mRows[row];
    if (!rowEl) return;
    const int enc = rowEl->tag;
    const int weekYear = enc / 100;
    const int week = enc % 100;
    if (!(weekYear == selWeekYear && week == selWeek))
    {
      rowEl->style.backgroundColor = glint_color{255, 55, 55, 55};
      rowEl->setDirty(false);
    }
  }

  void _onRowLeave(int row)
  {
    auto* rowEl = mRows[row];
    if (!rowEl) return;
    const int enc = rowEl->tag;
    const int weekYear = enc / 100;
    const int week = enc % 100;
    if (!(weekYear == selWeekYear && week == selWeek))
    {
      rowEl->style.backgroundColor = "";
      rowEl->setDirty(false);
    }
  }

  void _pickWeek(int weekYear, int week)
  {
    selWeekYear = weekYear;
    selWeek = week;
    const glint_ymd monday = glint_ymd_from_iso_week(weekYear, week, 1);
    navYear = monday.year;
    navMonth = monday.month;
    _refresh();
    if (onChange) onChange(weekYear, week);
  }

  void _prevMonth()
  {
    --navMonth;
    if (navMonth < 1) { navMonth = 12; navYear = std::max(1, navYear - 1); }
    _refresh();
  }

  void _nextMonth()
  {
    ++navMonth;
    if (navMonth > 12) { navMonth = 1; ++navYear; }
    _refresh();
  }

  void _moveSelWeek(int delta)
  {
    const glint_iso_week_value shifted = glint_shift_iso_week(selWeekYear > 0 ? selWeekYear : navYear,
                                                              selWeek > 0 ? selWeek : 1,
                                                              delta);
    _pickWeek(shifted.weekYear, shifted.week);
  }

  glint_ymd _firstVisibleMonday() const
  {
    const std::tm firstOfMonth = glint_normalized_tm(navYear, navMonth, 1);
    const int isoWeekday = glint_iso_weekday(firstOfMonth);
    return glint_add_days(navYear, navMonth, 1, -(isoWeekday - 1));
  }

  static const char* _monthName(int month)
  {
    static const char* kNames[] = {
      "", "January", "February", "March", "April", "May", "June",
      "July", "August", "September", "October", "November", "December"
    };
    return (month >= 1 && month <= 12) ? kNames[month] : "?";
  }
};