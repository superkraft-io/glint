#pragma once

/**
 * glint_view.hpp
 * Platform-dispatching umbrella for embedded glint views.
 */

#include "glint_view_base.hpp"

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

#elif defined(__APPLE__)
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

#elif defined(__linux__)
  static_assert(false, "glint_view: Linux backend not yet implemented.");

#else
  static_assert(false, "glint_view: unsupported platform.");
#endif