#pragma once

#include <functional>
#include <string>

class glint_element;

glint_element* glint_create_week_input_delegate(const std::function<void(int, int)>& onChange);
std::string glint_week_input_delegate_get_value(const glint_element* element);
bool glint_week_input_delegate_set_value(glint_element* element, const std::string& value);
void glint_week_input_delegate_clear(glint_element* element);
void glint_week_input_delegate_set_interaction_state(glint_element* element, bool disabled, bool readonly);