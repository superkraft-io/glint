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

enum class confirm_dialog_result {
  cancel = 0,
  primary = 1,
  secondary = 2,
};

#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
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
   * Implementations: glint_window_mac.mm (macOS), glint_window_win32.hpp (Win32),
   *                  glint_window_linux.cpp (Linux).
   */
  int showContextMenu(int screenX, int screenY,
                      const std::vector<std::pair<int, std::string>>& items,
                      const std::vector<int>& disabledIds = {},
                      const std::vector<int>& checkedIds  = {});

  /** Show a synchronous native open-file dialog filtered by extension. */
  std::string showOpenFileDialog(const std::vector<std::string>& extensions = {},
                                 const std::string& title = {},
                                 bool allowDirectories = false);

  /** Show a synchronous native save-file dialog filtered by extension. */
  std::string showSaveFileDialog(const std::vector<std::string>& extensions = {},
                                 const std::string& defaultExtension = {},
                                 const std::string& title = {},
                                 const std::string& suggestedPath = {});

  /** Show a synchronous native folder picker dialog. */
  std::string showOpenFolderDialog(const std::string& title = {});

  /** Show a synchronous native alert dialog with a title and message. */
  void showAlertDialog(const std::string& title, const std::string& message);

  /** Show a synchronous native confirmation dialog with up to three actions. */
  confirm_dialog_result showConfirmDialog(const std::string& title,
                                          const std::string& message,
                                          const std::string& primaryButton,
                                          const std::string& secondaryButton,
                                          const std::string& cancelButton);
#endif

} // namespace glint_platform
