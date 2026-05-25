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
  std::function<void(int, int)> onApply;
  std::function<void()> onRestore;
  std::function<void()> onDismiss;

  static constexpr float kW = 168.f;
  static constexpr float kH() { return 252.f; }

  glint_timepicker()
  {
    mAcceptsFocus = true;
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
    _scrollSelectionIntoView();
  }

  void focusHourList()
  {
    mFocusedList = kFocusHour;
    setDirty(false);
  }

  int hour() const { return mHour; }
  int minute() const { return mMinute; }

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (key.vk == 0x25)
    {
      mFocusedList = kFocusHour;
      setDirty(false);
      return true;
    }

    if (key.vk == 0x27)
    {
      mFocusedList = kFocusMinute;
      setDirty(false);
      return true;
    }

    if (key.vk == 0x26)
    {
      _stepSelection(-1);
      return true;
    }

    if (key.vk == 0x28)
    {
      _stepSelection(+1);
      return true;
    }

    if (key.vk == 0x0D)
    {
      if (onApply) onApply(mHour, mMinute);
      return true;
    }

    if (key.vk == 0x1B)
    {
      if (onRestore) onRestore();
      return true;
    }

    return false;
  }

private:
  enum _FocusList
  {
    kFocusHour = 0,
    kFocusMinute = 1,
  };

  int mHour = 0;
  int mMinute = 0;
  _FocusList mFocusedList = kFocusHour;
  glint_element* mHoursList = nullptr;
  glint_element* mMinutesList = nullptr;
  std::vector<glint_element*> mHourCells;
  std::vector<glint_element*> mHourLabels;
  std::vector<glint_element*> mMinuteCells;
  std::vector<glint_element*> mMinuteLabels;

  void _build()
  {
    auto* cols = new glint_element();
    cols->className = "glint-tp-columns";
    addChild(cols);


    auto* hoursList = new glint_element();
    hoursList->className = "glint-tp-list";
    hoursList->classList.add("glint-tp-hours-list");
    cols->addChild(hoursList);
    mHoursList = hoursList;

    auto* minutesList = new glint_element();
    minutesList->className = "glint-tp-list";
    minutesList->classList.add("glint-tp-minutes-list");
    cols->addChild(minutesList);
    mMinutesList = minutesList;

    mHourCells.reserve(24);
    mHourLabels.reserve(24);
    mMinuteCells.reserve(60);
    mMinuteLabels.reserve(60);

    for (int hour = 0; hour < 24; ++hour)
    {
      auto* hourCell = new glint_element();
      hourCell->className = "glint-tp-row";
      hourCell->classList.add("glint-tp-hour-row");
      hourCell->addEventListener("click", [this, hour](glint_event&) {
        mFocusedList = kFocusHour;
        mHour = hour;
        _refreshSelection();
        _scrollSelectionIntoView();
        if (onChange) onChange(mHour, mMinute);
      });

      auto* hourLabel = new glint_element();
      hourLabel->className = "glint-tp-row-label";
      hourLabel->classList.add("glint-tp-hour-row-label");
      hourLabel->innerText = _twoDigit(hour);
      hourCell->addChild(hourLabel);
      hoursList->addChild(hourCell);
      mHourCells.push_back(hourCell);
      mHourLabels.push_back(hourLabel);
    }

    for (int minute = 0; minute < 60; ++minute)
    {
      auto* minuteCell = new glint_element();
      minuteCell->className = "glint-tp-row";
      minuteCell->classList.add("glint-tp-minute-row");
      minuteCell->addEventListener("click", [this, minute](glint_event&) {
        mFocusedList = kFocusMinute;
        mMinute = minute;
        _refreshSelection();
        _scrollSelectionIntoView();
        if (onChange) onChange(mHour, mMinute);
      });

      auto* minuteLabel = new glint_element();
      minuteLabel->className = "glint-tp-row-label";
      minuteLabel->classList.add("glint-tp-minute-row-label");
      minuteLabel->innerText = _twoDigit(minute);
      minuteCell->addChild(minuteLabel);
      minutesList->addChild(minuteCell);
      mMinuteCells.push_back(minuteCell);
      mMinuteLabels.push_back(minuteLabel);
    }

    _refreshSelection();
  }

  void _refreshSelection()
  {
    for (int hour = 0; hour < static_cast<int>(mHourCells.size()); ++hour)
    {
      auto* cell = mHourCells[hour];
      auto* label = hour < static_cast<int>(mHourLabels.size()) ? mHourLabels[hour] : nullptr;
      if (!cell) continue;
      if (hour == mHour)
      {
        cell->classList.add("glint-tp-row-selected");
        if (label) label->classList.add("glint-tp-row-label-selected");
      }
      else
      {
        cell->classList.remove("glint-tp-row-selected");
        if (label) label->classList.remove("glint-tp-row-label-selected");
      }
      cell->setDirty(false);
      if (label) label->setDirty(false);
    }

    for (int minute = 0; minute < static_cast<int>(mMinuteCells.size()); ++minute)
    {
      auto* cell = mMinuteCells[minute];
      auto* label = minute < static_cast<int>(mMinuteLabels.size()) ? mMinuteLabels[minute] : nullptr;
      if (!cell) continue;
      if (minute == mMinute)
      {
        cell->classList.add("glint-tp-row-selected");
        if (label) label->classList.add("glint-tp-row-label-selected");
      }
      else
      {
        cell->classList.remove("glint-tp-row-selected");
        if (label) label->classList.remove("glint-tp-row-label-selected");
      }
      cell->setDirty(false);
      if (label) label->setDirty(false);
    }

    if (mHoursList) mHoursList->setDirty(false);
    if (mMinutesList) mMinutesList->setDirty(false);
    setDirty(false);
  }

  void _scrollSelectionIntoView()
  {
    if (mHour >= 0 && mHour < static_cast<int>(mHourCells.size()) && mHourCells[mHour])
      mHourCells[mHour]->scrollIntoView();
    if (mMinute >= 0 && mMinute < static_cast<int>(mMinuteCells.size()) && mMinuteCells[mMinute])
      mMinuteCells[mMinute]->scrollIntoView();
  }

  void _stepSelection(int delta)
  {
    if (mFocusedList == kFocusHour)
      mHour = (mHour + delta + 24) % 24;
    else
      mMinute = (mMinute + delta + 60) % 60;

    _refreshSelection();
    _scrollSelectionIntoView();
  }

  static std::string _twoDigit(int value)
  {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d", value);
    return buffer;
  }
};