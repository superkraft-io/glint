#pragma once

#if !defined(_WIN32) && !defined(GLINT_PLATFORM_RECT_DEFINED)
#  define GLINT_PLATFORM_RECT_DEFINED 1
struct RECT
{
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};
#endif