#pragma once

#include "../../glint_element.hpp"

class glint_scrollbar_thumb : public glint_element
{
public:
  std::function<void(float, float)> onPress;
  std::function<void(float, float, float, float)> onDrag;
  std::function<void()> onRelease;

  glint_scrollbar_thumb()
  {
    style.position = "absolute";
  }

  void OnMouseDown(float x, float y, const glint_mouse_mod&) override
  {
    if (onPress) onPress(x, y);
  }

  void OnMouseDrag(float x, float y, float dx, float dy, const glint_mouse_mod&) override
  {
    if (onDrag) onDrag(x, y, dx, dy);
  }

  void OnMouseUp(float, float, const glint_mouse_mod&) override
  {
    if (onRelease) onRelease();
  }

  const char* typeName() const override
  {
    return "scrollbar-thumb";
  }
};