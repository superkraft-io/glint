#pragma once

#include <string>
#include <windows.h>

class glint_i18n_native_win
{
public:
  static std::string detected_language()
  {
    wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {};
    if (::GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH) <= 0)
      return {};
    return utf8_from_wide(buffer);
  }

  static std::string localized(glint_i18n_native_phrase phrase)
  {
    const UINT stringId = resource_id(phrase);
    if (stringId == 0)
      return {};

    wchar_t buffer[256] = {};
    const int length = ::LoadStringW(::GetModuleHandleW(L"user32.dll"), stringId, buffer, 256);
    if (length <= 0)
      return {};

    return utf8_from_wide(std::wstring(buffer, static_cast<size_t>(length)));
  }

  static std::string utf8_from_wide(const std::wstring& wide)
  {
    if (wide.empty())
      return {};

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
      return {};

    std::string utf8(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
  }

private:
  static UINT resource_id(glint_i18n_native_phrase phrase)
  {
    switch (phrase)
    {
      case glint_i18n_native_phrase::edit_cut:
        return 31961;
      case glint_i18n_native_phrase::edit_copy:
        return 31962;
      case glint_i18n_native_phrase::edit_paste:
        return 31963;
      case glint_i18n_native_phrase::edit_select_all:
        return 31965;
      case glint_i18n_native_phrase::none:
        return 0;
    }
    return 0;
  }

};