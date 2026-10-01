#pragma once

#include "../glint_graphics.hpp"
#include "glint_platform_rect.hpp"

#include <functional>

namespace glint_platform {

struct colorpicker_handle;

colorpicker_handle* showColorPicker(const glint_color& initialColor,
                                    const RECT& anchorScreenRect,
                                    std::function<void(glint_color)> onChange = nullptr,
                                    std::function<void()> onClosed = nullptr);

colorpicker_handle* reopenColorPicker(colorpicker_handle* handle,
                                      const glint_color& initialColor,
                                      const RECT& anchorScreenRect,
                                      std::function<void(glint_color)> onChange = nullptr,
                                      std::function<void()> onClosed = nullptr);

void hideColorPicker(colorpicker_handle* handle);
void destroyColorPicker(colorpicker_handle* handle);

} // namespace glint_platform