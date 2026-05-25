#pragma once

#include "../../../glint_element.hpp"
#include "../../../default_style.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

class glint_timepicker : public glint_element
{
public:
  std::function<void(int, int)> onChange;
  std::function<void()> onDismiss;

  static constexpr float kW = 168.f;
  static constexpr float kH() { return 252.f; }

  glint_timepicker()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
    _build();
  }

  const char* typeName() const override { return "timepicker"; }

  void setTime(int hour, int minute)
  {
    mHour = std::max(0, std::min(hour, 23));
    mMinute = std::max(0, std::min(minute, 59));

    _refreshSelection();

    if (mHour >= 0 && mHour < static_cast<int>(mHourCells.size()) && mHourCells[mHour])
      mHourCells[mHour]->scrollIntoView();
    if (mMinute >= 0 && mMinute < static_cast<int>(mMinuteCells.size()) && mMinuteCells[mMinute])
      mMinuteCells[mMinute]->scrollIntoView();
  }

private:
  int mHour = 0;
  int mMinute = 0;
  glint_element* mHoursList = nullptr;
  glint_element* mMinutesList = nullptr;
  std::vector<glint_element*> mHourCells;
  std::vector<glint_element*> mMinuteCells;

  void _build()
  {
    auto* cols = new glint_element();
    cols->className = "tp-columns";
    addChild(cols);


    auto* hoursList = new glint_element();
    hoursList->className = "tp-list";
    hoursList->style.flexGrow = 1.f;
    hoursList->style.height = "100%";
    cols->addChild(hoursList);
  mHoursList = hoursList;
    
    

    auto* minutesList = new glint_element();
    minutesList->className = "tp-list";
    minutesList->style.flexGrow = 1.f;
    minutesList->style.height = "100%";
    cols->addChild(minutesList);
    mMinutesList = minutesList;

    mHourCells.reserve(24);
    mMinuteCells.reserve(60);

    for (int hour = 0; hour < 24; ++hour)
    {
      auto* hourCell = new glint_element();
      hourCell->className = "tp-row";
      hourCell->addEventListener("click", [this, hour](glint_event&) {
        mHour = hour;
        _refreshSelection();
        if (onChange) onChange(mHour, mMinute);
      });

      auto* hourLabel = new glint_element();
      hourLabel->className = "tp-row-label";
      hourLabel->innerText = _twoDigit(hour);
      hourCell->addChild(hourLabel);
      hoursList->addChild(hourCell);
      mHourCells.push_back(hourCell);
    }

    for (int minute = 0; minute < 60; ++minute)
    {
      auto* minuteCell = new glint_element();
      minuteCell->className = "tp-row";
      minuteCell->addEventListener("click", [this, minute](glint_event&) {
        mMinute = minute;
        _refreshSelection();
        if (onChange) onChange(mHour, mMinute);
      });

      auto* minuteLabel = new glint_element();
      minuteLabel->className = "tp-row-label";
      minuteLabel->innerText = _twoDigit(minute);
      minuteCell->addChild(minuteLabel);
      minutesList->addChild(minuteCell);
      mMinuteCells.push_back(minuteCell);
    }

    _refreshSelection();
  }

  void _refreshSelection()
  {
    for (int hour = 0; hour < static_cast<int>(mHourCells.size()); ++hour)
    {
      auto* cell = mHourCells[hour];
      if (!cell) continue;
      if (hour == mHour)
        cell->style.backgroundColor = glint_color{255, 26, 115, 232};
      else
        cell->style.backgroundColor = "";
      cell->style.color = (hour == mHour)
        ? glint_color{255, 255, 255, 255}
        : glint_color{255, 220, 220, 220};
      cell->setDirty(false);
    }

    for (int minute = 0; minute < static_cast<int>(mMinuteCells.size()); ++minute)
    {
      auto* cell = mMinuteCells[minute];
      if (!cell) continue;
      if (minute == mMinute)
        cell->style.backgroundColor = glint_color{255, 26, 115, 232};
      else
        cell->style.backgroundColor = "";
      cell->style.color = (minute == mMinute)
        ? glint_color{255, 255, 255, 255}
        : glint_color{255, 220, 220, 220};
      cell->setDirty(false);
    }

    if (mHoursList) mHoursList->setDirty(false);
    if (mMinutesList) mMinutesList->setDirty(false);
    setDirty(false);
  }

  static std::string _twoDigit(int value)
  {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d", value);
    return buffer;
  }
};