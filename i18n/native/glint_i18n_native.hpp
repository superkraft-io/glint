#pragma once

#include <string>

enum class glint_i18n_native_phrase
{
  none,
  edit_cut,
  edit_copy,
  edit_paste,
  edit_select_all,
};

#if defined(_WIN32)
#include "glint_i18n_native_win.hpp"
#elif defined(__APPLE__)
#include "glint_i18n_native_apple.hpp"
#endif

class glint_i18n_native
{
public:
  static std::string detected_language()
  {
#if defined(_WIN32)
    return glint_i18n_native_win::detected_language();
#elif defined(__APPLE__)
    return glint_i18n_native_apple::detected_language();
#else
    return {};
#endif
  }

  static std::string localized(glint_i18n_native_phrase phrase)
  {
#if defined(_WIN32)
    return glint_i18n_native_win::localized(phrase);
#elif defined(__APPLE__)
    return glint_i18n_native_apple::localized(phrase);
#else
    (void)phrase;
    return {};
#endif
  }
};