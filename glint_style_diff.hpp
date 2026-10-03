#pragma once

/**
 * glint_style_diff.hpp
 * Classifies CSS property changes by what they invalidate.
 *
 * A change is "paint-only" when no layout code reads the property, so the
 * next frame can skip reflow and just repaint. Everything not listed here is
 * treated as layout-affecting (the safe default), including properties added
 * later: forgetting to classify a property costs speed, never correctness.
 *
 * Exceptions that look like paint but feed layout (handled by the caller,
 * which knows the element):
 *   - transform: none <-> non-none switches subpixel snapping
 *     (_hasSubpixelIntent in element/glint_element_layout.hpp).
 *   - background on an inline element: alpha 0 <-> >0 decides whether the
 *     inline gets its own box in layoutInline.
 *   - scrollbar-*: propagated into scrollbar children inside Layout().
 *   - filter: inflates the paint rect (EnsureFilterPad) during layout.
 */

#include "glint_style.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

/**
 * A glint_style that is only allocated once something assigns it (a
 * glint_style is ~4 KB; most elements never need their optional copies).
 * Reading an unassigned one yields a default-constructed style; callers that
 * need a different fallback check allocated().
 */
class glint_lazy_style
{
public:
  glint_lazy_style() = default;
  glint_lazy_style(const glint_lazy_style& o)
    : mStyle(o.mStyle ? std::make_unique<glint_style>(*o.mStyle) : nullptr) {}
  glint_lazy_style& operator=(const glint_lazy_style& o)
  {
    if (this != &o)
    {
      if (o.mStyle) *this = *o.mStyle;
      else          mStyle.reset();
    }
    return *this;
  }
  glint_lazy_style& operator=(const glint_style& s)
  {
    if (mStyle) *mStyle = s;
    else        mStyle = std::make_unique<glint_style>(s);
    return *this;
  }

  /** Update the copy only if one exists (keeps unused copies unallocated). */
  void assignIfAllocated(const glint_style& s) { if (mStyle) *mStyle = s; }
  bool allocated() const { return mStyle != nullptr; }
  void reset() { mStyle.reset(); }

  operator const glint_style&() const { return mStyle ? *mStyle : _default(); }  // NOLINT

private:
  static const glint_style& _default() { static const glint_style s{}; return s; }
  std::unique_ptr<glint_style> mStyle;
};

enum glint_restyle_flags : uint8_t
{
  glint_restyle_none   = 0,
  glint_restyle_paint  = 1,   // repaint only
  glint_restyle_layout = 2,   // reflow required
  glint_restyle_inherited_paint = 4,   // paint change that inherits (color)
};

/** True when a change to `prop` (lower-case CSS name) never affects layout.
 *  `transform` and the background properties need the caller's extra checks
 *  described above. */
inline bool glint_css_prop_is_paint_only(const std::string& prop)
{
  static const std::unordered_set<std::string> kPaintOnly = {
    "color",
    "opacity",
    "background", "background-color", "background-img", "background-position",
    "background-repeat", "background-size", "background-blend-mode",
    "border-color", "border-top-color", "border-right-color",
    "border-bottom-color", "border-left-color",
    "border-style", "border-top-style", "border-right-style",
    "border-bottom-style", "border-left-style",
    "border-radius", "border-top-left-radius", "border-top-right-radius",
    "border-bottom-right-radius", "border-bottom-left-radius",
    "box-shadow", "text-decoration",
    "cursor", "pointer-events", "user-select", "z-index", "isolation",
    "mix-blend-mode", "backdrop-filter", "transform", "transition",
    "object-fit", "object-position",
    "mask", "mask-clip", "mask-composite", "mask-mode", "mask-origin",
    "mask-position", "mask-repeat", "mask-size",
    "stroke", "stroke-dasharray", "stroke-dashoffset", "stroke-linecap",
    "stroke-linejoin", "stroke-miterlimit", "stroke-opacity", "stroke-width",
  };
  return kPaintOnly.count(prop) != 0;
}

/** True for properties that inherit into descendants (so a change reaches
 *  descendant components' Layout() through their merged style). */
inline bool glint_css_prop_is_inherited_paint(const std::string& prop)
{
  return prop == "color";
}

inline bool glint_css_prop_is_background(const std::string& prop)
{
  return prop.compare(0, 10, "background") == 0;
}

/** CSS value text that means "no transform/filter". */
inline bool glint_css_value_is_none(const std::string& v)
{
  return v.empty() || v == "none";
}
