#pragma once

#include <string_view>
#include <unordered_map>

inline const std::unordered_map<std::string_view, std::string_view> glint_i18n_english = {
  {"glint.file_input.photo_library", "Photo Library"},
  {"glint.file_input.take_photo", "Take Photo"},
  {"glint.file_input.take_video", "Take Video"},
  {"glint.file_input.take_photo_or_video", "Take Photo or Video"},
  {"glint.file_input.choose_file", "Choose File"},
  {"glint.file_dialog.untitled", "Untitled"},
  {"glint.common.ok", "OK"},
  {"glint.common.no", "No"},
  {"glint.common.cancel", "Cancel"},
  {"glint.common.reset", "Reset"},
  {"glint.common.options", "Options"},
  {"glint.edit.cut", "Cut"},
  {"glint.edit.copy", "Copy"},
  {"glint.edit.paste", "Paste"},
  {"glint.edit.select_all", "Select All"},
};