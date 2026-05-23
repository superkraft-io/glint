#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>

#include "native/glint_i18n_native.hpp"
#include "languages/glint_i18n_english.hpp"
#include "languages/glint_i18n_swedish.hpp"

enum class glint_i18n_key
{
  file_input_photo_library,
  file_input_take_photo,
  file_input_take_video,
  file_input_take_photo_or_video,
  file_input_choose_file,
  file_dialog_untitled,
  common_ok,
  common_no,
  common_cancel,
  common_options,
  edit_cut,
  edit_copy,
  edit_paste,
  edit_select_all,
};

class glint_i18n
{
public:
  static std::string localized(glint_i18n_key key)
  {
    const std::string native = glint_i18n_native::localized(native_phrase(key));
    if (!native.empty())
      return native;
    return lookup(language_map(detected_language()), phrase_id(key), fallback_map());
  }

private:
  static glint_i18n_native_phrase native_phrase(glint_i18n_key key)
  {
    switch (key)
    {
      case glint_i18n_key::edit_cut:
        return glint_i18n_native_phrase::edit_cut;
      case glint_i18n_key::edit_copy:
        return glint_i18n_native_phrase::edit_copy;
      case glint_i18n_key::edit_paste:
        return glint_i18n_native_phrase::edit_paste;
      case glint_i18n_key::edit_select_all:
        return glint_i18n_native_phrase::edit_select_all;
      default:
        return glint_i18n_native_phrase::none;
    }
  }

  static std::string_view phrase_id(glint_i18n_key key)
  {
    switch (key)
    {
      case glint_i18n_key::file_input_photo_library:
        return "glint.file_input.photo_library";
      case glint_i18n_key::file_input_take_photo:
        return "glint.file_input.take_photo";
      case glint_i18n_key::file_input_take_video:
        return "glint.file_input.take_video";
      case glint_i18n_key::file_input_take_photo_or_video:
        return "glint.file_input.take_photo_or_video";
      case glint_i18n_key::file_input_choose_file:
        return "glint.file_input.choose_file";
      case glint_i18n_key::file_dialog_untitled:
        return "glint.file_dialog.untitled";
      case glint_i18n_key::common_ok:
        return "glint.common.ok";
      case glint_i18n_key::common_no:
        return "glint.common.no";
      case glint_i18n_key::common_cancel:
        return "glint.common.cancel";
      case glint_i18n_key::common_options:
        return "glint.common.options";
      case glint_i18n_key::edit_cut:
        return "glint.edit.cut";
      case glint_i18n_key::edit_copy:
        return "glint.edit.copy";
      case glint_i18n_key::edit_paste:
        return "glint.edit.paste";
      case glint_i18n_key::edit_select_all:
        return "glint.edit.select_all";
    }
    return "";
  }

  static const std::unordered_map<std::string_view, std::string_view>& fallback_map()
  {
    return glint_i18n_english;
  }

  static const std::unordered_map<std::string_view, std::string_view>& language_map(const std::string& language)
  {
    if (language == "sv")
      return glint_i18n_swedish;
    return glint_i18n_english;
  }

  static std::string lookup(const std::unordered_map<std::string_view, std::string_view>& language,
                            std::string_view phrase,
                            const std::unordered_map<std::string_view, std::string_view>& fallback)
  {
    const auto languageIt = language.find(phrase);
    if (languageIt != language.end())
      return std::string(languageIt->second);
    const auto fallbackIt = fallback.find(phrase);
    if (fallbackIt != fallback.end())
      return std::string(fallbackIt->second);

    return std::string(phrase);
  }

  static std::string detected_language()
  {
    const std::string nativeLanguage = glint_i18n_native::detected_language();
    if (!nativeLanguage.empty())
      return normalize_language(nativeLanguage);

    const char* candidates[] = {
      std::getenv("LC_ALL"),
      std::getenv("LC_MESSAGES"),
      std::getenv("LANG")
    };
    for (const char* candidate : candidates)
    {
      if (candidate && *candidate)
        return normalize_language(candidate);
    }
    return "en";
  }

  static std::string normalize_language(std::string locale)
  {
    std::transform(locale.begin(), locale.end(), locale.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });

    const std::string::size_type separator = locale.find_first_of("-_.@");
    if (separator != std::string::npos)
      locale.erase(separator);
    if (locale.empty())
      return "en";
    return locale;
  }
};