#include "glint_week_input_bridge.hpp"

#include "glint_week_input.hpp"

glint_element* glint_create_week_input_delegate(const std::function<void(int, int)>& onChange)
{
  auto* input = new glint_week_input();
  input->onChange = onChange;
  return input;
}

std::string glint_week_input_delegate_get_value(const glint_element* element)
{
  auto* input = dynamic_cast<const glint_week_input*>(element);
  return input ? input->getValue() : std::string();
}

bool glint_week_input_delegate_set_value(glint_element* element, const std::string& value)
{
  auto* input = dynamic_cast<glint_week_input*>(element);
  return input ? input->setValue(value) : false;
}

void glint_week_input_delegate_clear(glint_element* element)
{
  if (auto* input = dynamic_cast<glint_week_input*>(element))
    input->clear();
}