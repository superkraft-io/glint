#include "glint_time_input_bridge.hpp"

#include "glint_time_input.hpp"

glint_element* glint_create_time_input_delegate(const std::function<void(int, int)>& onChange)
{
  auto* input = new glint_time_input();
  input->onChange = onChange;
  return input;
}

std::string glint_time_input_delegate_get_value(const glint_element* element)
{
  auto* input = dynamic_cast<const glint_time_input*>(element);
  return input ? input->getValue() : std::string();
}

bool glint_time_input_delegate_set_value(glint_element* element, const std::string& value)
{
  auto* input = dynamic_cast<glint_time_input*>(element);
  return input ? input->setValue(value) : false;
}

void glint_time_input_delegate_clear(glint_element* element)
{
  if (auto* input = dynamic_cast<glint_time_input*>(element))
    input->clear();
}