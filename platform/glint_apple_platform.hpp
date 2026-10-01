#pragma once

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define GLINT_PLATFORM_IOS 1
#    define GLINT_PLATFORM_MAC 0
#  else
#    define GLINT_PLATFORM_IOS 0
#    define GLINT_PLATFORM_MAC 1
#  endif
#else
#  define GLINT_PLATFORM_IOS 0
#  define GLINT_PLATFORM_MAC 0
#endif

#if GLINT_PLATFORM_IOS && !defined(GLINT_PLATFORM_RECT_DEFINED)
#  define GLINT_PLATFORM_RECT_DEFINED 1
struct RECT
{
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};
#endif