#pragma once

/**
 * glint_view.hpp
 * Platform-dispatching umbrella for embedded glint views.
 */

#include "glint_apple_platform.hpp"
#include "glint_view_base.hpp"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
  #include "win32/glint_view_win32.hpp"
  using glint_view = glint_view_win32;

  inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
  {
    return glint_view::create(options);
  }

  namespace glint {
    using glint_backend = ::glint_backend;
    using glint_view = ::glint_view;
    using glint_view_options = ::glint_view_options;

    inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
    {
      return ::createView(options);
    }
  }

#elif defined(__APPLE__) && !TARGET_OS_IPHONE
  #include "mac/glint_view_mac.hpp"
  using glint_view = glint_view_mac;

  inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
  {
    return glint_view::create(options);
  }

  namespace glint {
    using glint_backend      = ::glint_backend;
    using glint_view         = ::glint_view;
    using glint_view_options = ::glint_view_options;

    inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
    {
      return ::createView(options);
    }
  }

#elif defined(__APPLE__) && TARGET_OS_IPHONE
  #include "ios/glint_view_ios.hpp"
  using glint_view = glint_view_ios;

  inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
  {
    return glint_view::create(options);
  }

  namespace glint {
    using glint_backend      = ::glint_backend;
    using glint_view         = ::glint_view;
    using glint_view_options = ::glint_view_options;

    inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
    {
      return ::createView(options);
    }
  }

#elif defined(__linux__)
  #include "linux/glint_view_linux.hpp"
  using glint_view = glint_view_linux;

  inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
  {
    return glint_view::create(options);
  }

  namespace glint {
    using glint_backend      = ::glint_backend;
    using glint_view         = ::glint_view;
    using glint_view_options = ::glint_view_options;

    inline std::unique_ptr<glint_view> createView(const glint_view_options& options = {})
    {
      return ::createView(options);
    }
  }

#else
  static_assert(false, "glint_view: unsupported platform.");
#endif