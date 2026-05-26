#pragma once

#include "glint_platform_rect.hpp"

#include <functional>

namespace glint_platform {

struct datepicker_handle;

datepicker_handle* showDatePicker(int initialYear,
                                  int initialMonth,
                                  int initialDay,
                                  const RECT& anchorScreenRect,
                                  std::function<void(int, int, int)> onChange = nullptr,
                                  std::function<void(int, int, int)> onConfirm = nullptr,
                                  std::function<void()> onReset = nullptr,
                                  std::function<void()> onClosed = nullptr);

datepicker_handle* reopenDatePicker(datepicker_handle* handle,
                                    int initialYear,
                                    int initialMonth,
                                    int initialDay,
                                    const RECT& anchorScreenRect,
                                    std::function<void(int, int, int)> onChange = nullptr,
                                    std::function<void(int, int, int)> onConfirm = nullptr,
                                    std::function<void()> onReset = nullptr,
                                    std::function<void()> onClosed = nullptr);

void hideDatePicker(datepicker_handle* handle);
void destroyDatePicker(datepicker_handle* handle);

} // namespace glint_platform