#pragma once

#include "../../glint_element.hpp"

class glint_scrollbar_track : public glint_element
{
public:
  std::function<void(float, float)> onPress;

  void OnMouseDown(float x, float y, const glint_mouse_mod&) override
  {
    if (onPress) onPress(x, y);
  }

  const char* typeName() const override
  {
    return "scrollbar-track";
  }
};