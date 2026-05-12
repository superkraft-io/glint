#pragma once

#include "../../glint_element.hpp"

class glint_scrollbar_arrow : public glint_element
{
public:
  explicit glint_scrollbar_arrow(bool isMinus)
    : mIsMinus(isMinus)
  {
    style.position = "absolute";
    element.addEventListener("mouseenter", [this](glint_event&) { mHovered = true;  setDirty(false); });
    element.addEventListener("mouseleave", [this](glint_event&) { mHovered = false; setDirty(false); });
  }

  std::function<void()> onPress;
  std::function<glint_color()> getBaseColor;

  void OnMouseDown(float, float, const glint_mouse_mod&) override
  {
    if (onPress) onPress();
  }

  void Draw(glint_canvas& g) override
  {
    if (mHovered && getBaseColor)
    {
      const glint_color base = getBaseColor();
      const auto lighten = [](int channel) {
        return channel + 25 > 255 ? 255 : channel + 25;
      };
      glint_style hoverStyle = style;
      hoverStyle.backgroundColor = glint_color(
        base.A,
        lighten(base.R),
        lighten(base.G),
        lighten(base.B));
      DrawBackground(g, hoverStyle);
      return;
    }

    glint_element::Draw(g);
  }

  const char* typeName() const override
  {
    return mIsMinus ? "scrollbar-arrow-minus" : "scrollbar-arrow-plus";
  }

private:
  bool mIsMinus = false;
  bool mHovered = false;
};