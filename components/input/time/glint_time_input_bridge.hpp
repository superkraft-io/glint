#pragma once

#include <functional>
#include <string>

class glint_element;

glint_element* glint_create_time_input_delegate(const std::function<void(int, int)>& onChange);
std::string glint_time_input_delegate_get_value(const glint_element* element);
bool glint_time_input_delegate_set_value(glint_element* element, const std::string& value);
void glint_time_input_delegate_clear(glint_element* element);