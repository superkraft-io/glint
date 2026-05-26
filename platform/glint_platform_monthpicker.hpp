#pragma once

#include "glint_platform_rect.hpp"

#include <functional>

namespace glint_platform {

struct monthpicker_handle;

monthpicker_handle* showMonthPicker(int initialYear,
                                    int initialMonth,
                                    const RECT& anchorScreenRect,
                                    std::function<void(int, int)> onChange = nullptr,
                                    std::function<void(int, int)> onConfirm = nullptr,
                                    std::function<void()> onReset = nullptr,
                                    std::function<void()> onClosed = nullptr);

monthpicker_handle* reopenMonthPicker(monthpicker_handle* handle,
                                      int initialYear,
                                      int initialMonth,
                                      const RECT& anchorScreenRect,
                                      std::function<void(int, int)> onChange = nullptr,
                                      std::function<void(int, int)> onConfirm = nullptr,
                                      std::function<void()> onReset = nullptr,
                                      std::function<void()> onClosed = nullptr);

void hideMonthPicker(monthpicker_handle* handle);
void destroyMonthPicker(monthpicker_handle* handle);

} // namespace glint_platform