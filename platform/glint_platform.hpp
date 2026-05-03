#pragma once
/**
 * glint_platform.hpp
 * Thin C++ declarations for OS-specific services used by Glint components.
 * Implementations live in the platform-specific .mm / .cpp files so that
 * these declarations can be included from plain C++ (.hpp) headers without
 * pulling in ObjC or Win32 system headers.
 */

#include <string>
#include <utility>
#include <vector>

namespace glint_platform {

#if defined(__APPLE__) || defined(_WIN32)
  /** Copy UTF-8 text to the system clipboard. */
  void        setClipboardText(const std::string& utf8);
  /** Read UTF-8 text from the system clipboard.  Returns "" if empty. */
  std::string getClipboardText();

  /**
   * Show a synchronous popup context menu.
   * screenX/screenY: preferred screen position (pass 0,0 to use cursor position).
   * items: vector of {id, label} pairs.  id==0 with label=="-" is a separator.
   * disabledIds: item ids drawn greyed and non-clickable.
   * checkedIds: item ids drawn with a checkmark (current selection indicator).
   * Returns the id of the selected item, or 0 if dismissed.
   * Implementations: glint_window_mac.mm (macOS), glint_window_win32.hpp (Win32).
   */
  int showContextMenu(int screenX, int screenY,
                      const std::vector<std::pair<int, std::string>>& items,
                      const std::vector<int>& disabledIds = {},
                      const std::vector<int>& checkedIds  = {});
#endif

} // namespace glint_platform
