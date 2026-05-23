#include "../render/glint_tree_node.hpp"
#include "../glint_document.hpp"
#include "../platform/glint_platform_colorpicker.hpp"
#include "glint_input_colorpicker_bridge.hpp"
#include "glint_colorpicker_window.hpp"

struct glint_input_colorpicker_bridge
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
  glint_platform::colorpicker_handle* picker = nullptr;
#else
  glint_colorpicker_window* window = nullptr;
#endif
};

RECT glint_input_colorpicker_anchor_screen_rect(const glint_element* anchorElement)
{
  if (!anchorElement) return RECT{};

  float cl = anchorElement->mRect.L;
  float ct = anchorElement->mRect.T;
  for (glint_element* p = anchorElement->mParent; p; p = p->mParent)
  {
    cl -= p->mScrollLeft;
    ct -= p->mScrollTop;
  }

  const float width = anchorElement->mRect.W();
  const float height = anchorElement->mRect.H();

#if defined(_WIN32) || defined(OS_WIN)
  POINT topLeft{ static_cast<LONG>(cl), static_cast<LONG>(ct) };
  POINT bottomRight{ static_cast<LONG>(cl + width), static_cast<LONG>(ct + height) };
  if (HWND hwnd = anchorElement->mRoot ? anchorElement->mRoot->hwnd : nullptr)
  {
    ::ClientToScreen(hwnd, &topLeft);
    ::ClientToScreen(hwnd, &bottomRight);
  }
  return { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
#elif defined(__linux__)
  return (anchorElement->mRoot && anchorElement->mRoot->linuxWindow)
    ? anchorElement->mRoot->linuxWindow->contentRectToScreen(cl, ct, width, height)
    : RECT{};
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
  return (anchorElement->mRoot && anchorElement->mRoot->macWindow)
    ? anchorElement->mRoot->macWindow->contentRectToScreen(cl, ct, width, height)
    : RECT{};
#else
  return RECT{};
#endif
}

glint_input_colorpicker_bridge* glint_input_colorpicker_reopen(
  glint_input_colorpicker_bridge* bridge,
  glint_color initialColor,
  RECT anchorScreenRect,
  std::function<void(glint_color)> onChange,
  std::function<void()> onClosed)
{
  if (!bridge) bridge = new glint_input_colorpicker_bridge();

#if defined(__APPLE__) && TARGET_OS_IPHONE
  bridge->picker = glint_platform::reopenColorPicker(bridge->picker,
                                                     initialColor,
                                                     anchorScreenRect,
                                                     std::move(onChange),
                                                     std::move(onClosed));
  return bridge;
#else
  if (!bridge->window)
    bridge->window = glint_colorpicker_window::open(initialColor, anchorScreenRect, std::move(onChange), std::move(onClosed));
  else
    bridge->window->reopen(initialColor, anchorScreenRect, std::move(onChange), std::move(onClosed));
  return bridge;
#endif
}

void glint_input_colorpicker_hide(glint_input_colorpicker_bridge* bridge)
{
  if (!bridge) return;
#if defined(__APPLE__) && TARGET_OS_IPHONE
  glint_platform::hideColorPicker(bridge->picker);
  return;
#else
  if (bridge->window) bridge->window->hide();
#endif
}

void glint_input_colorpicker_destroy(glint_input_colorpicker_bridge* bridge)
{
  if (!bridge) return;
#if defined(__APPLE__) && TARGET_OS_IPHONE
  glint_platform::destroyColorPicker(bridge->picker);
  bridge->picker = nullptr;
  delete bridge;
#else
  if (bridge->window)
  {
    bridge->window->destroy();
    bridge->window = nullptr;
  }
  delete bridge;
#endif
}