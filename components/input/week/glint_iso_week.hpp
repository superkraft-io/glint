#pragma once

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

struct glint_ymd
{
  int year = 0;
  int month = 0;
  int day = 0;
};

struct glint_iso_week_value
{
  int weekYear = 0;
  int week = 0;
  int weekday = 1; // Monday = 1, Sunday = 7
};

inline bool glint_is_leap_year(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline std::tm glint_normalized_tm(int year, int month, int day)
{
  std::tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = 12;
  std::mktime(&t);
  return t;
}

inline int glint_iso_weekday(const std::tm& t)
{
  return t.tm_wday == 0 ? 7 : t.tm_wday;
}

inline glint_ymd glint_add_days(int year, int month, int day, int delta)
{
  std::tm t = glint_normalized_tm(year, month, day + delta);
  return { t.tm_year + 1900, t.tm_mon + 1, t.tm_mday };
}

inline int glint_iso_weeks_in_year(int year)
{
  const std::tm jan1 = glint_normalized_tm(year, 1, 1);
  const int jan1Weekday = glint_iso_weekday(jan1);
  return (jan1Weekday == 4 || (jan1Weekday == 3 && glint_is_leap_year(year))) ? 53 : 52;
}

inline glint_iso_week_value glint_iso_week_from_ymd(int year, int month, int day)
{
  const std::tm t = glint_normalized_tm(year, month, day);
  const int isoWeekday = glint_iso_weekday(t);
  const int yearDay = t.tm_yday + 1;
  int week = (yearDay - isoWeekday + 10) / 7;
  int weekYear = year;

  if (week < 1)
  {
    weekYear = year - 1;
    week = glint_iso_weeks_in_year(weekYear);
  }
  else if (week > glint_iso_weeks_in_year(year))
  {
    weekYear = year + 1;
    week = 1;
  }

  return { weekYear, week, isoWeekday };
}

inline glint_ymd glint_ymd_from_iso_week(int weekYear, int week, int weekday = 1)
{
  weekYear = std::max(1, weekYear);
  week = std::max(1, std::min(glint_iso_weeks_in_year(weekYear), week));
  weekday = std::max(1, std::min(7, weekday));

  const std::tm jan4 = glint_normalized_tm(weekYear, 1, 4);
  const int jan4Weekday = glint_iso_weekday(jan4);
  const int dayOffset = - (jan4Weekday - 1) + (week - 1) * 7 + (weekday - 1);
  const std::tm t = glint_normalized_tm(weekYear, 1, 4 + dayOffset);
  return { t.tm_year + 1900, t.tm_mon + 1, t.tm_mday };
}

inline glint_iso_week_value glint_shift_iso_week(int weekYear, int week, int deltaWeeks)
{
  const glint_ymd monday = glint_ymd_from_iso_week(weekYear, week, 1);
  const glint_ymd shifted = glint_add_days(monday.year, monday.month, monday.day, deltaWeeks * 7);
  return glint_iso_week_from_ymd(shifted.year, shifted.month, shifted.day);
}

inline bool glint_parse_iso_week_value(const std::string& value, int& weekYear, int& week)
{
  char trailing = 0;
  if (std::sscanf(value.c_str(), "%d-W%d%c", &weekYear, &week, &trailing) != 2)
    return false;
  if (weekYear < 1 || week < 1 || week > glint_iso_weeks_in_year(weekYear))
    return false;
  return true;
}

inline std::string glint_format_iso_week_value(int weekYear, int week)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-W%02d", weekYear, week);
  return buffer;
}