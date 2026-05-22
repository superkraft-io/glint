#pragma once

#include "../glint_graphics.hpp"
#include "../platform/glint_platform_rect.hpp"

#include <functional>

class glint_element;
struct glint_input_colorpicker_bridge;

RECT glint_input_colorpicker_anchor_screen_rect(const glint_element* anchorElement);

glint_input_colorpicker_bridge* glint_input_colorpicker_reopen(
  glint_input_colorpicker_bridge* bridge,
  glint_color initialColor,
  RECT anchorScreenRect,
  std::function<void(glint_color)> onChange = nullptr,
  std::function<void()> onClosed = nullptr);

void glint_input_colorpicker_hide(glint_input_colorpicker_bridge* bridge);
void glint_input_colorpicker_destroy(glint_input_colorpicker_bridge* bridge);