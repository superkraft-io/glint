#pragma once

#include "glint_platform_rect.hpp"

#include <functional>

namespace glint_platform {

struct weekpicker_handle;

weekpicker_handle* showWeekPicker(int initialWeekYear,
                                  int initialWeek,
                                  const RECT& anchorScreenRect,
                                                                    std::function<void(int, int)> onChange = nullptr,
                                  std::function<void(int, int)> onConfirm = nullptr,
                                  std::function<void()> onReset = nullptr,
                                  std::function<void()> onClosed = nullptr);

weekpicker_handle* reopenWeekPicker(weekpicker_handle* handle,
                                    int initialWeekYear,
                                    int initialWeek,
                                    const RECT& anchorScreenRect,
                                                                        std::function<void(int, int)> onChange = nullptr,
                                    std::function<void(int, int)> onConfirm = nullptr,
                                    std::function<void()> onReset = nullptr,
                                    std::function<void()> onClosed = nullptr);

void hideWeekPicker(weekpicker_handle* handle);
void destroyWeekPicker(weekpicker_handle* handle);

} // namespace glint_platform