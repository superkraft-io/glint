#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <string>

class glint_i18n_native_apple
{
public:
  static std::string detected_language()
  {
    CFArrayRef preferredLanguages = CFLocaleCopyPreferredLanguages();
    if (!preferredLanguages)
      return {};

    std::string language;
    const CFIndex count = CFArrayGetCount(preferredLanguages);
    if (count > 0)
    {
      CFStringRef preferredLanguage = static_cast<CFStringRef>(CFArrayGetValueAtIndex(preferredLanguages, 0));
      if (preferredLanguage)
        language = utf8_from_cfstring(preferredLanguage);
    }

    CFRelease(preferredLanguages);
    return language;
  }

  static std::string localized(glint_i18n_native_phrase phrase)
  {
    (void)phrase;
    return {};
  }

private:
  static std::string utf8_from_cfstring(CFStringRef text)
  {
    if (!text)
      return {};

    const CFIndex length = CFStringGetLength(text);
    const CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string utf8(static_cast<size_t>(maxSize), '\0');
    if (!CFStringGetCString(text, utf8.data(), maxSize, kCFStringEncodingUTF8))
      return {};
    utf8.resize(std::strlen(utf8.c_str()));
    return utf8;
  }
};