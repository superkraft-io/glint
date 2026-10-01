#pragma once

#include <algorithm>
#include <cstdio>
#include <string>

inline bool glint_is_valid_time_value(int hour, int minute)
{
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

inline bool glint_parse_time_value(const std::string& value, int& hour, int& minute)
{
  char trailing = 0;
  if (std::sscanf(value.c_str(), "%d:%d%c", &hour, &minute, &trailing) != 2)
    return false;
  return glint_is_valid_time_value(hour, minute);
}

inline std::string glint_format_time_value(int hour, int minute)
{
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
  return buffer;
}

inline int glint_clamp_time_hour(int hour)
{
  return std::max(0, std::min(23, hour));
}

inline int glint_clamp_time_minute(int minute)
{
  return std::max(0, std::min(59, minute));
}