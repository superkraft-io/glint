#pragma once

#include "glint_platform_rect.hpp"

#include <functional>

namespace glint_platform {

struct timepicker_handle;

timepicker_handle* showTimePicker(int initialHour,
                                  int initialMinute,
                                  const RECT& anchorScreenRect,
                                  std::function<void(int, int)> onConfirm = nullptr,
                                                                    std::function<void()> onReset = nullptr,
                                  std::function<void()> onClosed = nullptr);

timepicker_handle* reopenTimePicker(timepicker_handle* handle,
                                    int initialHour,
                                    int initialMinute,
                                    const RECT& anchorScreenRect,
                                    std::function<void(int, int)> onConfirm = nullptr,
                                                                        std::function<void()> onReset = nullptr,
                                    std::function<void()> onClosed = nullptr);

void hideTimePicker(timepicker_handle* handle);
void destroyTimePicker(timepicker_handle* handle);

} // namespace glint_platform