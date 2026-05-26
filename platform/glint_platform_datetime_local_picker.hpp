#pragma once

#include "glint_platform_rect.hpp"

#include <functional>

namespace glint_platform {

struct datetime_local_picker_handle;

datetime_local_picker_handle* showDateTimeLocalPicker(int initialYear,
                                                      int initialMonth,
                                                      int initialDay,
                                                      int initialHour,
                                                      int initialMinute,
                                                      const RECT& anchorScreenRect,
                                                      std::function<void(int, int, int, int, int)> onChange = nullptr,
                                                      std::function<void(int, int, int, int, int)> onConfirm = nullptr,
                                                      std::function<void()> onReset = nullptr,
                                                      std::function<void()> onClosed = nullptr);

datetime_local_picker_handle* reopenDateTimeLocalPicker(datetime_local_picker_handle* handle,
                                                        int initialYear,
                                                        int initialMonth,
                                                        int initialDay,
                                                        int initialHour,
                                                        int initialMinute,
                                                        const RECT& anchorScreenRect,
                                                        std::function<void(int, int, int, int, int)> onChange = nullptr,
                                                        std::function<void(int, int, int, int, int)> onConfirm = nullptr,
                                                        std::function<void()> onReset = nullptr,
                                                        std::function<void()> onClosed = nullptr);

void hideDateTimeLocalPicker(datetime_local_picker_handle* handle);
void destroyDateTimeLocalPicker(datetime_local_picker_handle* handle);

} // namespace glint_platform