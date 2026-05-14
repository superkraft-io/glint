#pragma once

/**
 * glint_style.hpp
 * CSS-inspired styling struct for glint components.
 *
 * Usage:
 *   glint_style s;
 *   s.backgroundColor = "#1a1a1a";
 *   s.color           = "#ffffff";   // text / foreground colour
 *   s.borderColor     = "#555555";
 *   s.borderWidth     = 1.f;
 *   s.borderRadius    = 6.f;
 *   s.padding         = SKEdgeInsets(8.f, 12.f);
 *   s.padding         = "8 12";          // CSS shorthand  (top/bottom  left/right)
 *   s.padding         = "4 8 4 8";       // top right bottom left
 *   s.paddingLeft     = "12px";
 *   s.margin          = "10 20";
 *   s.marginTop       = 5.f;
 *
 *   s.backgroundColor = glint_color(255, 26, 26, 26);
 */

#include "glint_graphics.hpp"
#  include "include/core/SkBlendMode.h"
#  include "include/core/SkM44.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace glint_graphics;

// ── glint_length ───────────────────────────────────────────────────────────────────────────────
// A CSS-inspired length type. Accepts floats (pixels) or strings ("50%", "12px").
//   _c.style.width  = 82.f;     // pixels — existing code unchanged
//   _c.style.width  = "100%";   // percentage of parent
//   _c.style.height = "50%";
struct glint_length
{
  // Empty raw means "unset" — resolves to 0 but is distinguishable from
  // an explicit 0 assignment. ComponentAdd uses this to auto-flow children.
  std::string raw = "";

  // Set to true by the builder when it auto-injects a cursor-position value
  // for block-flow stacking.  The inspector treats builderInjected lengths as
  // unset so only developer-authored inline properties appear in element.style {}.
  // Cleared automatically whenever the field is reassigned (constructor resets it).
  bool builderInjected = false;

  glint_length() = default;

  // Implicit construction from numeric types — keeps existing float assignments working.
  glint_length(float f)  { char b[32]; std::snprintf(b, sizeof(b), "%g", f); raw = b; }  // NOLINT
  glint_length(double f) { char b[32]; std::snprintf(b, sizeof(b), "%g", static_cast<float>(f)); raw = b; }  // NOLINT
  glint_length(int f)    { raw = std::to_string(f); }  // NOLINT

  // Implicit construction from string literals and std::string.
  glint_length(const char* s)        : raw(s ? s : "0") {}  // NOLINT
  glint_length(const std::string& s) : raw(s) {}             // NOLINT

  // Resolve to pixels given the parent dimension on the same axis.
  // "50%"  → parentSize * 0.5
  // "12px" or "12" → 12.f
  // "auto" → 0.f (reserved for future flex-like layout)
  // Invalid / unknown strings (e.g. "fill") → 0.f (treated as unset)
  float resolve(float parentSize) const
  {
    if (raw.empty() || raw == "0" || raw == "auto") return 0.f;
    // Guard against non-numeric strings — avoids std::stof throwing on e.g. "fill" or "-px"
    const char fc = raw.front();
    const char sc = raw.size() > 1 ? raw[1] : 0;
    const bool startsNumeric = std::isdigit(static_cast<unsigned char>(fc)) || fc == '.'
        || ((fc == '-' || fc == '+') && (std::isdigit(static_cast<unsigned char>(sc)) || sc == '.'));
    if (!startsNumeric) return 0.f;
    if (raw.back() == '%')
      return std::stof(raw) * parentSize / 100.f;
    return std::stof(raw); // stof stops at non-numeric suffix, so "12px" → 12.f
  }

  // Allow reading back as float (resolve with no parent — percentages → 0).
  float toFloat() const { return resolve(0.f); }
};

struct glint_text_align
{
  EAlign value = EAlign::Near;

  glint_text_align() = default;
  glint_text_align(EAlign align) : value(align) {}  // NOLINT
  glint_text_align(const char* align)
  {
    EAlign parsed = EAlign::Near;
    if (tryParse(align, parsed)) value = parsed;
  }
  glint_text_align(const std::string& align)
  {
    EAlign parsed = EAlign::Near;
    if (tryParse(align, parsed)) value = parsed;
  }

  glint_text_align& operator=(EAlign align)
  {
    value = align;
    return *this;
  }

  glint_text_align& operator=(const char* align)
  {
    EAlign parsed = value;
    if (tryParse(align, parsed)) value = parsed;
    return *this;
  }

  glint_text_align& operator=(const std::string& align)
  {
    EAlign parsed = value;
    if (tryParse(align, parsed)) value = parsed;
    return *this;
  }

  operator EAlign() const { return value; }  // NOLINT

  bool operator==(const glint_text_align& other) const { return value == other.value; }
  bool operator!=(const glint_text_align& other) const { return value != other.value; }
  bool operator==(EAlign align) const { return value == align; }
  bool operator!=(EAlign align) const { return value != align; }

  friend bool operator==(EAlign lhs, const glint_text_align& rhs) { return lhs == rhs.value; }
  friend bool operator!=(EAlign lhs, const glint_text_align& rhs) { return lhs != rhs.value; }

private:
  static bool tryParse(const char* align, EAlign& out)
  {
    if (!align) return false;
    return tryParse(std::string(align), out);
  }

  static bool tryParse(const std::string& align, EAlign& out)
  {
    std::string low;
    low.reserve(align.size());
    for (char c : align)
      low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (low == "left" || low == "start" || low == "near")
    {
      out = EAlign::Near;
      return true;
    }
    if (low == "center" || low == "middle")
    {
      out = EAlign::Center;
      return true;
    }
    if (low == "right" || low == "end" || low == "far")
    {
      out = EAlign::Far;
      return true;
    }
    return false;
  }
};

//   sk_color c = "#ffaa0080";      // #rrggbbaa
//   sk_color c = "#fa0";           // #rgb shorthand
//   sk_color c = "cornflowerblue"; // CSS named color
//   sk_color c = glint_color(255, 255, 170, 0);
struct sk_color
{
  glint_color value {255, 0, 0, 0};

  sk_color() = default;

  sk_color(const glint_color& c) : value(c) {}   // NOLINT

  // Implicit construction from a CSS hex string or named color
  sk_color(const char* css)                  // NOLINT
  {
    if (!css) { value = glint_color(0, 0, 0, 0); return; }

    if (css[0] == '#')
    {
      // ── Hex parsing ───────────────────────────────────────────────────────
      const char*  s   = css + 1;
      const size_t len = std::strlen(s);
      unsigned int v   = 0;

      for (size_t i = 0; i < len && i < 8; i++)
      {
        v <<= 4;
        const char c = s[i];
        if      (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      }

      if (len == 6)       // #rrggbb → opaque
        value = glint_color(255,
                       static_cast<int>((v >> 16) & 0xff),
                       static_cast<int>((v >>  8) & 0xff),
                       static_cast<int>( v         & 0xff));
      else if (len == 8)  // #rrggbbaa
        value = glint_color(static_cast<int>( v         & 0xff),
                       static_cast<int>((v >> 24)  & 0xff),
                       static_cast<int>((v >> 16)  & 0xff),
                       static_cast<int>((v >>  8)  & 0xff));
      else if (len == 3)  // #rgb shorthand
      {
        const int r = (v >> 8) & 0xf;
        const int g = (v >> 4) & 0xf;
        const int b =  v       & 0xf;
        value = glint_color(255, r | (r << 4), g | (g << 4), b | (b << 4));
      }
      else
        value = glint_color(0, 0, 0, 0);
      return;
    }

    // ── CSS named-color lookup (case-insensitive, sorted for binary search) ─
    // Source: https://developer.mozilla.org/en-US/docs/Web/CSS/named-color
    // Format: { name, 0xRRGGBB } — alpha is 255 unless noted.
    struct Entry { const char* name; unsigned int rgb; };
    static const Entry kTable[] = {
      { "aliceblue",            0xf0f8ff },
      { "antiquewhite",         0xfaebd7 },
      { "aqua",                 0x00ffff },
      { "aquamarine",           0x7fffd4 },
      { "azure",                0xf0ffff },
      { "beige",                0xf5f5dc },
      { "bisque",               0xffe4c4 },
      { "black",                0x000000 },
      { "blanchedalmond",       0xffebcd },
      { "blue",                 0x0000ff },
      { "blueviolet",           0x8a2be2 },
      { "brown",                0xa52a2a },
      { "burlywood",            0xdeb887 },
      { "cadetblue",            0x5f9ea0 },
      { "chartreuse",           0x7fff00 },
      { "chocolate",            0xd2691e },
      { "coral",                0xff7f50 },
      { "cornflowerblue",       0x6495ed },
      { "cornsilk",             0xfff8dc },
      { "crimson",              0xdc143c },
      { "cyan",                 0x00ffff },
      { "darkblue",             0x00008b },
      { "darkcyan",             0x008b8b },
      { "darkgoldenrod",        0xb8860b },
      { "darkgray",             0xa9a9a9 },
      { "darkgreen",            0x006400 },
      { "darkgrey",             0xa9a9a9 },
      { "darkkhaki",            0xbdb76b },
      { "darkmagenta",          0x8b008b },
      { "darkolivegreen",       0x556b2f },
      { "darkorange",           0xff8c00 },
      { "darkorchid",           0x9932cc },
      { "darkred",              0x8b0000 },
      { "darksalmon",           0xe9967a },
      { "darkseagreen",         0x8fbc8f },
      { "darkslateblue",        0x483d8b },
      { "darkslategray",        0x2f4f4f },
      { "darkslategrey",        0x2f4f4f },
      { "darkturquoise",        0x00ced1 },
      { "darkviolet",           0x9400d3 },
      { "deeppink",             0xff1493 },
      { "deepskyblue",          0x00bfff },
      { "dimgray",              0x696969 },
      { "dimgrey",              0x696969 },
      { "dodgerblue",           0x1e90ff },
      { "firebrick",            0xb22222 },
      { "floralwhite",          0xfffaf0 },
      { "forestgreen",          0x228b22 },
      { "fuchsia",              0xff00ff },
      { "gainsboro",            0xdcdcdc },
      { "ghostwhite",           0xf8f8ff },
      { "gold",                 0xffd700 },
      { "goldenrod",            0xdaa520 },
      { "gray",                 0x808080 },
      { "green",                0x008000 },
      { "greenyellow",          0xadff2f },
      { "grey",                 0x808080 },
      { "honeydew",             0xf0fff0 },
      { "hotpink",              0xff69b4 },
      { "indianred",            0xcd5c5c },
      { "indigo",               0x4b0082 },
      { "ivory",                0xfffff0 },
      { "khaki",                0xf0e68c },
      { "lavender",             0xe6e6fa },
      { "lavenderblush",        0xfff0f5 },
      { "lawngreen",            0x7cfc00 },
      { "lemonchiffon",         0xfffacd },
      { "lightblue",            0xadd8e6 },
      { "lightcoral",           0xf08080 },
      { "lightcyan",            0xe0ffff },
      { "lightgoldenrodyellow", 0xfafad2 },
      { "lightgray",            0xd3d3d3 },
      { "lightgreen",           0x90ee90 },
      { "lightgrey",            0xd3d3d3 },
      { "lightpink",            0xffb6c1 },
      { "lightsalmon",          0xffa07a },
      { "lightseagreen",        0x20b2aa },
      { "lightskyblue",         0x87cefa },
      { "lightslategray",       0x778899 },
      { "lightslategrey",       0x778899 },
      { "lightsteelblue",       0xb0c4de },
      { "lightyellow",          0xffffe0 },
      { "lime",                 0x00ff00 },
      { "limegreen",            0x32cd32 },
      { "linen",                0xfaf0e6 },
      { "magenta",              0xff00ff },
      { "maroon",               0x800000 },
      { "mediumaquamarine",     0x66cdaa },
      { "mediumblue",           0x0000cd },
      { "mediumorchid",         0xba55d3 },
      { "mediumpurple",         0x9370db },
      { "mediumseagreen",       0x3cb371 },
      { "mediumslateblue",      0x7b68ee },
      { "mediumspringgreen",    0x00fa9a },
      { "mediumturquoise",      0x48d1cc },
      { "mediumvioletred",      0xc71585 },
      { "midnightblue",         0x191970 },
      { "mintcream",            0xf5fffa },
      { "mistyrose",            0xffe4e1 },
      { "moccasin",             0xffe4b5 },
      { "navajowhite",          0xffdead },
      { "navy",                 0x000080 },
      { "oldlace",              0xfdf5e6 },
      { "olive",                0x808000 },
      { "olivedrab",            0x6b8e23 },
      { "orange",               0xffa500 },
      { "orangered",            0xff4500 },
      { "orchid",               0xda70d6 },
      { "palegoldenrod",        0xeee8aa },
      { "palegreen",            0x98fb98 },
      { "paleturquoise",        0xafeeee },
      { "palevioletred",        0xdb7093 },
      { "papayawhip",           0xffefd5 },
      { "peachpuff",            0xffdab9 },
      { "peru",                 0xcd853f },
      { "pink",                 0xffc0cb },
      { "plum",                 0xdda0dd },
      { "powderblue",           0xb0e0e6 },
      { "purple",               0x800080 },
      { "rebeccapurple",        0x663399 },
      { "red",                  0xff0000 },
      { "rosybrown",            0xbc8f8f },
      { "royalblue",            0x4169e1 },
      { "saddlebrown",          0x8b4513 },
      { "salmon",               0xfa8072 },
      { "sandybrown",           0xf4a460 },
      { "seagreen",             0x2e8b57 },
      { "seashell",             0xfff5ee },
      { "sienna",               0xa0522d },
      { "silver",               0xc0c0c0 },
      { "skyblue",              0x87ceeb },
      { "slateblue",            0x6a5acd },
      { "slategray",            0x708090 },
      { "slategrey",            0x708090 },
      { "snow",                 0xfffafa },
      { "springgreen",          0x00ff7f },
      { "steelblue",            0x4682b4 },
      { "tan",                  0xd2b48c },
      { "teal",                 0x008080 },
      { "thistle",              0xd8bfd8 },
      { "tomato",               0xff6347 },
      { "turquoise",            0x40e0d0 },
      { "violet",               0xee82ee },
      { "wheat",                0xf5deb3 },
      { "white",                0xffffff },
      { "whitesmoke",           0xf5f5f5 },
      { "yellow",               0xffff00 },
      { "yellowgreen",          0x9acd32 },
    };

    // Special-case: transparent
    std::string low(css);
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == "transparent") { value = glint_color(0, 0, 0, 0); return; }

    // ── rgb() / rgba() functional notation ─────────────────────────────────
    // Supports:
    //   rgb(r, g, b)            — r/g/b 0-255 integers or 0%-100%
    //   rgba(r, g, b, a)        — a is 0-1 float or 0%-100%
    //   rgb(r g b / a)          — CSS Color Level 4 space / slash syntax
    {
      const bool isRgb  = low.size() > 4 && low.substr(0, 4) == "rgb(";
      const bool isRgba = low.size() > 5 && low.substr(0, 5) == "rgba(";
      if (isRgb || isRgba)
      {
        const size_t openP  = low.find('(');
        const size_t closeP = low.rfind(')');
        if (openP != std::string::npos && closeP != std::string::npos && closeP > openP)
        {
          std::string args = low.substr(openP + 1, closeP - openP - 1);
          // Normalise separators: commas and CSS4 '/' → spaces
          for (char& c : args) if (c == ',' || c == '/') c = ' ';
          std::istringstream ss(args);
          float vals[4] = {0.f, 0.f, 0.f, 255.f};  // r g b a (a pre-set 255 for rgb())
          int i = 0;
          std::string tok;
          while (ss >> tok && i < 4)
          {
            if (tok == "none") { ++i; continue; }
            try
            {
              if (!tok.empty() && tok.back() == '%')
              {
                const float pct = std::stof(tok);
                // r,g,b: 0%-100% → 0-255; alpha: 0%-100% → 0-255
                vals[i] = pct * 2.55f;
              }
              else
              {
                const float v = std::stof(tok);
                if (i == 3)
                  vals[3] = v * 255.f;  // alpha 0-1 → 0-255
                else
                  vals[i] = v;           // r,g,b already 0-255
              }
            }
            catch (...) {}
            ++i;
          }
          const auto clamp255 = [](float v) -> int {
            return static_cast<int>(std::min(255.f, std::max(0.f, v + 0.5f)));
          };
          value = glint_color(clamp255(vals[3]), clamp255(vals[0]),
                         clamp255(vals[1]), clamp255(vals[2]));
          return;
        }
      }
    }

    // Binary search (table is sorted by name)
    int lo = 0, hi = static_cast<int>(sizeof(kTable) / sizeof(kTable[0])) - 1;
    while (lo <= hi)
    {
      const int mid = (lo + hi) / 2;
      const int cmp = low.compare(kTable[mid].name);
      if      (cmp == 0) {
        const unsigned int rgb = kTable[mid].rgb;
        value = glint_color(255,
                       static_cast<int>((rgb >> 16) & 0xff),
                       static_cast<int>((rgb >>  8) & 0xff),
                       static_cast<int>( rgb         & 0xff));
        return;
      }
      else if (cmp < 0) hi = mid - 1;
      else              lo = mid + 1;
    }

    // Unknown keyword → transparent
    value = glint_color(0, 0, 0, 0);
  }

  // Implicit conversion back to glint_color
  operator glint_color()   const { return value; }           // NOLINT

  // Implicit conversion to glint_pattern (needed for AttachPanelBackground etc.)
  operator glint_pattern() const { return glint_pattern(value); } // NOLINT
};

// ── glint_optional_color ───────────────────────────────────────────────────────
// A per-side border color that tracks whether it was explicitly set.
// Any assignment marks isSet = true, so resolvedBorderColor() can
// distinguish "no override" from "explicitly transparent".
struct glint_optional_color
{
  bool   isSet = false;
  glint_color value  = glint_color(0, 0, 0, 0);

  glint_optional_color() = default;
  glint_optional_color(const glint_optional_color&) = default;
  glint_optional_color& operator=(const glint_optional_color&) = default;

  glint_optional_color& operator=(const sk_color& c)      { value = c.value;            isSet = true; return *this; }
  glint_optional_color& operator=(const glint_color& c)        { value = c;                  isSet = true; return *this; }
  glint_optional_color& operator=(const char* css)        { value = sk_color(css).value; isSet = true; return *this; }
  glint_optional_color& operator=(const std::string& s)   { return operator=(s.c_str()); }

  operator sk_color() const { return sk_color(value); }  // NOLINT
  operator glint_color()   const { return value; }            // NOLINT
};

// ── glint_optional_float ────────────────────────────────────────────────────────
// A float property that explicitly tracks whether it was set by inline user code.
// Follows the same isSet pattern as glint_optional_color so that _mergedStyle()
// can implement the correct CSS cascade: inline style.*= always wins over class
// rules regardless of value, exactly as Chrome does.
//
// Usage:
//   el->style.opacity = 0.7f;   // sets value = 0.7, isSet = true  → overrides CSS
//   el->style.opacity = 1.0f;   // sets value = 1.0, isSet = true  → still overrides CSS
//   el->style.opacity = "";     // clears isSet                    → CSS cascade wins
//
// operator float() provides implicit read-back so all existing float read sites
// (render, animation, inspector) compile and run unchanged.
struct glint_optional_float
{
  float value = 1.f;   // CSS initial value — overridden per field at declaration site
  bool  isSet = false;

  glint_optional_float() = default;
  explicit constexpr glint_optional_float(float initVal) : value(initVal), isSet(false) {}
  glint_optional_float(const glint_optional_float&)            = default;
  glint_optional_float& operator=(const glint_optional_float&) = default;

  // Assignment from float marks the property as explicitly set.
  glint_optional_float& operator=(float v)
  { value = v; isSet = true; return *this; }

  // Assignment from string: empty string clears isSet (mirrors element.style.removeProperty()).
  glint_optional_float& operator=(const char* s)
  {
    if (!s || !*s) { isSet = false; }
    else           { try { value = std::stof(s); isSet = true; } catch (...) {} }
    return *this;
  }
  glint_optional_float& operator=(const std::string& s) { return operator=(s.c_str()); }

  // Implicit float read-back — all existing code that reads .opacity as a float compiles.
  operator float() const { return value; }  // NOLINT

  bool operator==(float v) const { return value == v; }
  bool operator!=(float v) const { return value != v; }
};

// ── sk_gradient_stop ──────────────────────────────────────────────────────────
// One colour stop in a linear gradient.  Shared by glint_style::backgroundGradient
// and glint_gradient_editor so both operate on the same type.
struct sk_gradient_stop {
  float  position = 0.f;                        // [0..1] along the gradient
  glint_color color    = glint_color(255, 255, 255, 255);
  bool operator<(const sk_gradient_stop& o) const { return position < o.position; }
};

// ── Edge-insets (padding / margin) ────────────────────────────────────────────
struct SKEdgeInsets
{
  SKEdgeInsets() = default;

  // Uniform
  explicit SKEdgeInsets(float all)
    : top(all), right(all), bottom(all), left(all) {}

  // Vertical / horizontal
  SKEdgeInsets(float topBottom, float leftRight)
    : top(topBottom), right(leftRight), bottom(topBottom), left(leftRight) {}

  // Explicit sides (CSS order: top right bottom left)
  SKEdgeInsets(float t, float r, float b, float l)
    : top(t), right(r), bottom(b), left(l) {}

  // ── Shorthand assignment — the ONLY public way to set insets in user code. ──
  // For individual sides use the paddingTop / marginLeft etc. proxies on glint_style.
  SKEdgeInsets& operator=(float all)   { top = right = bottom = left = all; return *this; }
  SKEdgeInsets& operator=(double all)  { return operator=(static_cast<float>(all)); }
  SKEdgeInsets& operator=(int all)     { return operator=(static_cast<float>(all)); }
  // CSS shorthand string:
  //   "10"          → all sides 10
  //   "10 20"       → top/bottom 10, left/right 20
  //   "10 20 30"    → top 10, left/right 20, bottom 30
  //   "10 20 30 40" → top right bottom left
  //   Values may carry a "px" suffix which is ignored.
  SKEdgeInsets& operator=(const char* css)
  {
    if (!css || !*css) return *this;
    std::istringstream ss(css);
    std::vector<float> v;
    std::string tok;
    while (ss >> tok)
    {
      if (!tok.empty()) { try { v.push_back(std::stof(tok)); } catch (...) {} } // stof stops at 'px'
    }
    if      (v.size() == 1) { top = right = bottom = left = v[0]; }
    else if (v.size() == 2) { top = bottom = v[0]; right = left = v[1]; }
    else if (v.size() == 3) { top = v[0]; right = left = v[1]; bottom = v[2]; }
    else if (v.size() >= 4) { top = v[0]; right = v[1]; bottom = v[2]; left = v[3]; }
    return *this;
  }
  SKEdgeInsets& operator=(const std::string& css) { return operator=(css.c_str()); }

private:
  float top    = 0.f;
  float right  = 0.f;
  float bottom = 0.f;
  float left   = 0.f;
  // Raw CSS strings, preserved through struct copy so sk_side_proxy::resolve()
  // can detect %-based values at layout time (e.g. marginLeft = "50%").
  std::string rawTop, rawRight, rawBottom, rawLeft;

  friend struct glint_style;  // glint_style binds sk_side_proxy pointers to these fields
};

// ── Single-side proxy (used for paddingLeft, marginTop, etc.) ─────────────────
// Holds a pointer back into the owning SKEdgeInsets field.
// Supports float, px-string ("10px" / "10") and percentage-string ("50%").
// Call resolve(containerWidth) in layout to get the correct pixel value;
// operator float() returns the raw stored number (px or pct numerator).
struct sk_side_proxy
{
  float*       _p    = nullptr;
  std::string* _rawp = nullptr;  // points into SKEdgeInsets::rawXxx — survives copy

  sk_side_proxy() = default;
  explicit sk_side_proxy(float* p, std::string* rawp = nullptr) : _p(p), _rawp(rawp) {}

  // Don't copy the pointer — rebinding is done explicitly in glint_style ctor.
  sk_side_proxy(const sk_side_proxy&) {}
  sk_side_proxy& operator=(const sk_side_proxy&) { return *this; }

  sk_side_proxy& operator=(float v)  {
    if (_p) *_p = v;
    if (_rawp) _rawp->clear();   // clear raw: this is a plain px value
    return *this;
  }
  sk_side_proxy& operator=(double v) { return operator=(static_cast<float>(v)); }
  sk_side_proxy& operator=(int v)    { return operator=(static_cast<float>(v)); }
  sk_side_proxy& operator=(const char* s) {
    if (!_p || !s || !*s) return *this;
    if (_rawp) *_rawp = s;              // store raw string for resolve()
    try { *_p = std::stof(s); } catch (...) {}  // stof stops at '%' or 'px' — stores numeric part
    return *this;
  }
  sk_side_proxy& operator=(const std::string& s)  { return operator=(s.c_str()); }

  // Resolve to pixels. If the stored raw string ends with '%', the numeric part
  // is treated as a percentage of containerWidth (CSS spec: all margin % values
  // resolve against the containing block's inline size / width).
  // For plain px values (or when set from float/int), returns the stored value.
  float resolve(float containerWidth) const {
    if (_rawp && !_rawp->empty() && _rawp->back() == '%')
    {
      try { return std::stof(*_rawp) * containerWidth / 100.f; } catch (...) {}
    }
    return _p ? *_p : 0.f;
  }

  operator float() const { return _p ? *_p : 0.f; } // NOLINT
};

// ── glint_mat3 ─────────────────────────────────────────────────────────────────
// Lightweight row-major 3×3 affine matrix for 2-D CSS transforms.
// m[row*3 + col]; bottom row is always (0, 0, 1).
// Point transform: x' = m[0]*x + m[1]*y + m[2]
//                  y' = m[3]*x + m[4]*y + m[5]
struct glint_mat3
{
  float m[9] = {1,0,0, 0,1,0, 0,0,1}; // identity

  bool isIdentity() const {
    return m[0]==1.f&&m[1]==0.f&&m[2]==0.f&&
           m[3]==0.f&&m[4]==1.f&&m[5]==0.f&&
           m[6]==0.f&&m[7]==0.f&&m[8]==1.f;
  }

  void mapPoint(float& x, float& y) const {
    const float nx = m[0]*x + m[1]*y + m[2];
    const float ny = m[3]*x + m[4]*y + m[5];
    x = nx; y = ny;
  }

  // Concatenate: this * o  (applies o-transform after this-transform to points)
  glint_mat3 operator*(const glint_mat3& o) const {
    glint_mat3 r;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        r.m[row*3+col] = m[row*3+0]*o.m[col] + m[row*3+1]*o.m[3+col] + m[row*3+2]*o.m[6+col];
    return r;
  }

  glint_mat3& operator*=(const glint_mat3& o) { *this = *this * o; return *this; }

  // Invert (affine only — bottom row stays 0 0 1).
  glint_mat3 inverse() const {
    const float det = m[0]*m[4] - m[1]*m[3];
    glint_mat3 inv;
    if (std::abs(det) < 1e-9f) return inv; // singular → identity fallback
    const float id = 1.f / det;
    inv.m[0] =  m[4]*id;
    inv.m[1] = -m[1]*id;
    inv.m[2] = (m[1]*m[5] - m[4]*m[2])*id;
    inv.m[3] = -m[3]*id;
    inv.m[4] =  m[0]*id;
    inv.m[5] = (m[3]*m[2] - m[0]*m[5])*id;
    inv.m[6] = 0.f; inv.m[7] = 0.f; inv.m[8] = 1.f;
    return inv;
  }

  static glint_mat3 makeTranslate(float tx, float ty) {
    glint_mat3 r; r.m[2] = tx; r.m[5] = ty; return r;
  }
  static glint_mat3 makeScale(float sx, float sy) {
    glint_mat3 r; r.m[0] = sx; r.m[4] = sy; return r;
  }
  // Positive degrees = clockwise (CSS / screen Y-down convention).
  static glint_mat3 makeRotateDeg(float deg) {
    const float rad = deg * (3.14159265358979323846f / 180.f);
    const float c = std::cos(rad), s = std::sin(rad);
    glint_mat3 r;
    r.m[0] =  c; r.m[1] = -s;
    r.m[3] =  s; r.m[4] =  c;
    return r;
  }
  // pivot-aware rotate/scale (around (cx, cy))
  static glint_mat3 makeRotateDegAround(float deg, float cx, float cy) {
    return makeTranslate(cx, cy) * makeRotateDeg(deg) * makeTranslate(-cx, -cy);
  }
  static glint_mat3 makeScaleAround(float sx, float sy, float cx, float cy) {
    return makeTranslate(cx, cy) * makeScale(sx, sy) * makeTranslate(-cx, -cy);
  }
};

// ── glint_shader_string ────────────────────────────────────────────────────────
// A std::string wrapper used for style.filter and style.backdropFilter.
// Behaves identically to std::string for reads; fires an optional callback
// on every write so glint_element can auto-populate its shaders map the
// moment one of these properties is assigned (Option B eager auto-creation).
//
// Rules:
//   Copy constructor  — copies VALUE only; callback is NOT propagated.
//     (computedStyle copies and transition lerp targets stay inert.)
//   Copy-assign       — copies VALUE then fires THIS object's callback.
//   Assign from string/char* — same: set value + fire callback.
//   _setCallback()    — called once by glint_element in its constructor.
class glint_shader_string
{
public:
  glint_shader_string() = default;

  // Copy: value only — callback stays null in the new object.
  glint_shader_string(const glint_shader_string& o) : _v(o._v) {}

  // Assign value, then fire THIS object's callback (if wired).
  glint_shader_string& operator=(const glint_shader_string& o)
  { _v = o._v; if (_cb) _cb(_v); return *this; }
  glint_shader_string& operator=(const std::string& s)
  { _v = s;    if (_cb) _cb(_v); return *this; }
  glint_shader_string& operator=(const char* s)
  { _v = s ? s : ""; if (_cb) _cb(_v); return *this; }

  // Implicit conversion — covers all pass-by-const-ref call sites.
  operator const std::string&() const { return _v; }

  // Forwarded helpers (needed when called directly as a member).
  bool empty()                         const { return _v.empty(); }
  bool operator==(const std::string& s) const { return _v == s; }
  bool operator!=(const std::string& s) const { return _v != s; }
  bool operator==(const char* s)        const { return _v == s; }
  bool operator!=(const char* s)        const { return _v != s; }

  // Called once by the owning glint_element in its constructor.
  void _setCallback(std::function<void(const std::string&)> cb) { _cb = std::move(cb); }

private:
  std::string _v;
  std::function<void(const std::string&)> _cb;
};

class glint_box_shadow_proxy
{
public:
  bool isSet = false;

  glint_box_shadow_proxy() = default;

  glint_box_shadow_proxy(bool* enabled,
                        sk_color* color,
                        float* offsetX,
                        float* offsetY,
                        float* blur,
                        float* spread,
                        bool* inset)
    : _enabled(enabled)
    , _color(color)
    , _offsetX(offsetX)
    , _offsetY(offsetY)
    , _blur(blur)
    , _spread(spread)
    , _inset(inset)
  {}

  glint_box_shadow_proxy(const glint_box_shadow_proxy&) {}
  glint_box_shadow_proxy& operator=(const glint_box_shadow_proxy& o)
  {
    if (this == &o) return *this;
    if (!o.isSet) { clear(); return *this; }
    return operator=(o._raw);
  }

  glint_box_shadow_proxy& operator=(const std::string& s)
  {
    apply(s);
    return *this;
  }

  glint_box_shadow_proxy& operator=(const char* s)
  {
    apply(s ? std::string(s) : std::string());
    return *this;
  }

  operator std::string() const { return toString(); }  // NOLINT

  std::string toString() const
  {
    return isSet ? _raw : std::string{};
  }

  void clear()
  {
    isSet = false;
    _raw.clear();
    resetShadowFieldsToDefaults();
  }

private:
  static std::string trim(const std::string& value)
  {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
  }

  static std::string toLower(std::string value)
  {
    for (char& c : value)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
  }

  static std::vector<std::string> splitCssTokens(const std::string& value)
  {
    std::vector<std::string> tokens;
    std::string current;
    int depth = 0;

    const auto flush = [&]() {
      const std::string token = trim(current);
      if (!token.empty()) tokens.push_back(token);
      current.clear();
    };

    for (char c : value)
    {
      if (c == '(')
      {
        ++depth;
        current += c;
      }
      else if (c == ')')
      {
        if (depth > 0) --depth;
        current += c;
      }
      else if (std::isspace(static_cast<unsigned char>(c)) && depth == 0)
      {
        flush();
      }
      else
      {
        current += c;
      }
    }

    flush();
    return tokens;
  }

  static std::string firstTopLevelCommaSegment(const std::string& value)
  {
    std::string out;
    int depth = 0;
    for (char c : value)
    {
      if (c == '(') ++depth;
      else if (c == ')' && depth > 0) --depth;
      else if (c == ',' && depth == 0) break;
      out += c;
    }
    return trim(out);
  }

  void apply(const std::string& value)
  {
    if (!_enabled || !_color || !_offsetX || !_offsetY || !_blur || !_spread || !_inset)
      return;

    const std::string normalized = trim(value);
    if (normalized.empty())
    {
      clear();
      return;
    }

    const std::string lower = toLower(normalized);
    isSet = true;
    _raw = firstTopLevelCommaSegment(normalized);

    if (lower == "none")
    {
      *_enabled = false;
      *_inset = false;
      *_offsetX = 0.f;
      *_offsetY = 0.f;
      *_blur = 0.f;
      *_spread = 0.f;
      return;
    }

    *_enabled = true;
    *_inset = false;
    *_offsetX = 0.f;
    *_offsetY = 0.f;
    *_blur = 0.f;
    *_spread = 0.f;

    std::vector<float> lengths;
    std::string colorToken;
    for (const std::string& token : splitCssTokens(firstTopLevelCommaSegment(normalized)))
    {
      const std::string tokenLower = toLower(token);
      if (tokenLower == "inset")
      {
        *_inset = true;
        continue;
      }

      const char first = token.empty() ? 0 : token.front();
      const char second = token.size() > 1 ? token[1] : 0;
      const bool startsNumeric = std::isdigit(static_cast<unsigned char>(first)) || first == '.'
          || ((first == '-' || first == '+') && (std::isdigit(static_cast<unsigned char>(second)) || second == '.'));
      if (startsNumeric)
      {
        lengths.push_back(std::stof(token));
      }
      else
      {
        colorToken = token;
      }
    }

    if (lengths.size() >= 1) *_offsetX = lengths[0];
    if (lengths.size() >= 2) *_offsetY = lengths[1];
    if (lengths.size() >= 3) *_blur = lengths[2];
    if (lengths.size() >= 4) *_spread = lengths[3];
    if (!colorToken.empty()) *_color = colorToken.c_str();
  }

  void resetShadowFieldsToDefaults()
  {
    if (!_enabled || !_color || !_offsetX || !_offsetY || !_blur || !_spread || !_inset)
      return;

    *_enabled = false;
    *_color = glint_color(120, 0, 0, 0);
    *_offsetX = 2.f;
    *_offsetY = 2.f;
    *_blur = 4.f;
    *_spread = 0.f;
    *_inset = false;
  }

  bool* _enabled = nullptr;
  sk_color* _color = nullptr;
  float* _offsetX = nullptr;
  float* _offsetY = nullptr;
  float* _blur = nullptr;
  float* _spread = nullptr;
  bool* _inset = nullptr;
  std::string _raw;
};

// ── CSS blend mode → SkBlendMode ────────────────────────────────────────────
// Converts a CSS mix-blend-mode / background-blend-mode keyword to the
// corresponding SkBlendMode.  Returns kSrcOver (= "normal") for unknown,
// empty, or "normal" values.
inline SkBlendMode glint_css_blend_mode(const std::string& kw)
{
  if (kw.empty() || kw == "normal")  return SkBlendMode::kSrcOver;
  if (kw == "multiply")              return SkBlendMode::kMultiply;
  if (kw == "screen")                return SkBlendMode::kScreen;
  if (kw == "overlay")               return SkBlendMode::kOverlay;
  if (kw == "darken")                return SkBlendMode::kDarken;
  if (kw == "lighten")               return SkBlendMode::kLighten;
  if (kw == "color-dodge")           return SkBlendMode::kColorDodge;
  if (kw == "color-burn")            return SkBlendMode::kColorBurn;
  if (kw == "hard-light")            return SkBlendMode::kHardLight;
  if (kw == "soft-light")            return SkBlendMode::kSoftLight;
  if (kw == "difference")            return SkBlendMode::kDifference;
  if (kw == "exclusion")             return SkBlendMode::kExclusion;
  if (kw == "hue")                   return SkBlendMode::kHue;
  if (kw == "saturation")            return SkBlendMode::kSaturation;
  if (kw == "color")                 return SkBlendMode::kColor;
  if (kw == "luminosity")            return SkBlendMode::kLuminosity;
  return SkBlendMode::kSrcOver;
}

// ── glint_style ────────────────────────────────────────────────────────────────
struct glint_style
{
  // ── Transform helpers ─────────────────────────────────────────────────────
  static float ResolveTransformLength(const std::string& token, float ref)
  {
    auto trim = [](const std::string& v) -> std::string {
      size_t b = 0;
      while (b < v.size() && std::isspace(static_cast<unsigned char>(v[b]))) ++b;
      size_t e = v.size();
      while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1]))) --e;
      return v.substr(b, e - b);
    };

    const std::string s = trim(token);
    if (s.empty()) return 0.f;

    const char fc = s.front();
    if (!std::isdigit(static_cast<unsigned char>(fc)) && fc != '-' && fc != '+' && fc != '.')
      return 0.f;

    try
    {
      if (s.back() == '%')
        return std::stof(s) * ref / 100.f;
      return std::stof(s);  // "px" suffix is accepted by stof.
    }
    catch (...) { return 0.f; }
  }

  static float ResolveAngleDeg(const std::string& token)
  {
    auto trim = [](const std::string& v) -> std::string {
      size_t b = 0;
      while (b < v.size() && std::isspace(static_cast<unsigned char>(v[b]))) ++b;
      size_t e = v.size();
      while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1]))) --e;
      return v.substr(b, e - b);
    };
    const std::string s = trim(token);
    if (s.empty()) return 0.f;
    try {
      float v = std::stof(s);
      // "rad" suffix → convert to degrees
      if (s.size() > 3 && s.substr(s.size()-3) == "rad")
        v = v * (180.f / 3.14159265358979323846f);
      // "turn" suffix → convert to degrees
      else if (s.size() > 4 && s.substr(s.size()-4) == "turn")
        v = v * 360.f;
      // "grad" suffix → convert to degrees
      else if (s.size() > 4 && s.substr(s.size()-4) == "grad")
        v = v * (360.f / 400.f);
      // "deg" suffix (or bare number) → already degrees
      return v;
    }
    catch (...) { return 0.f; }
  }

  /**
   * Build the full CSS transform matrix for this style.
   * selfW / selfH  : component paint-rect dimensions (for % translate resolution).
   * cx / cy        : transform-origin in canvas/screen coordinates
   *                  (default = center of the paint rect = paintRECT.MW(), MH()).
   *
   * Functions applied left-to-right per CSS spec (Transforms Level 2):
   *   translate, translateX, translateY, translateZ, translate3d
   *   scale, scaleX, scaleY, scaleZ, scale3d
   *   rotate / rotateZ (Z-axis, screen plane)
   *   rotateX / rotateY  (3-D axis rotations with full 4×4 matrix)
   *   rotate3d(x, y, z, angle)
   *   perspective(d)  — CSS inline perspective projection
   *   matrix(a,b,c,d,e,f)  — 2-D affine
   *   matrix3d(...)         — full 4×4
   *
   * Returns identity when transform is empty / "none".
   */
  SkM44 ResolveTransformMatrix(float selfW, float selfH, float cx, float cy) const
  {
    SkM44 mat; // identity
    if (transform.empty() || transform == "none") return mat;

    static constexpr float kDeg2Rad = 3.14159265358979323846f / 180.f;

    auto trim = [](const std::string& v) -> std::string {
      size_t b = 0;
      while (b < v.size() && std::isspace(static_cast<unsigned char>(v[b]))) ++b;
      size_t e = v.size();
      while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1]))) --e;
      return v.substr(b, e - b);
    };

    auto lower = [](std::string v) -> std::string {
      for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return v;
    };

    auto parseList = [&trim](const std::string& args) -> std::vector<std::string> {
      std::string flat = args;
      for (char& c : flat) if (c == ',') c = ' ';
      std::istringstream ss(flat);
      std::vector<std::string> out;
      std::string tok;
      while (ss >> tok) out.push_back(trim(tok));
      return out;
    };

    // NOTE: transform-origin wrapping is applied ONCE around the entire accumulated
    // matrix at the end of parsing — NOT per-function.  See CSS Transforms Level 2
    // §7.1: "the transformation matrix is equal to translate(ox, oy) × M × translate(-ox,-oy)"
    // where M is the concatenation of all functions in the list.

    const std::string src = transform;
    size_t i = 0;
    while (i < src.size())
    {
      while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
      if (i >= src.size()) break;

      const size_t nameBeg = i;
      while (i < src.size() &&
         (std::isalpha(static_cast<unsigned char>(src[i])) ||
          std::isdigit(static_cast<unsigned char>(src[i])) ||
          src[i] == '-')) ++i;
      if (i >= src.size() || src[i] != '(') { ++i; continue; }

      const std::string name = lower(src.substr(nameBeg, i - nameBeg));
      ++i;
      int depth = 1;
      const size_t argBeg = i;
      while (i < src.size() && depth > 0)
      {
        if (src[i] == '(') ++depth;
        else if (src[i] == ')') --depth;
        ++i;
      }
      if (depth != 0) break;

      const std::string args = src.substr(argBeg, (i - 1) - argBeg);
      const auto vals = parseList(args);

      if (name == "translate")
      {
        const float tx = vals.size() > 0 ? ResolveTransformLength(vals[0], selfW) : 0.f;
        const float ty = vals.size() > 1 ? ResolveTransformLength(vals[1], selfH) : 0.f;
        mat.preConcat(SkM44::Translate(tx, ty, 0));
      }
      else if (name == "translatex")
      {
        const float tx = vals.size() > 0 ? ResolveTransformLength(vals[0], selfW) : 0.f;
        mat.preConcat(SkM44::Translate(tx, 0, 0));
      }
      else if (name == "translatey")
      {
        const float ty = vals.size() > 0 ? ResolveTransformLength(vals[0], selfH) : 0.f;
        mat.preConcat(SkM44::Translate(0, ty, 0));
      }
      else if (name == "translatez")
      {
        const float tz = vals.size() > 0 ? ResolveTransformLength(vals[0], 1.f) : 0.f;
        mat.preConcat(SkM44::Translate(0, 0, tz));
      }
      else if (name == "translate3d")
      {
        const float tx = vals.size() > 0 ? ResolveTransformLength(vals[0], selfW) : 0.f;
        const float ty = vals.size() > 1 ? ResolveTransformLength(vals[1], selfH) : 0.f;
        const float tz = vals.size() > 2 ? ResolveTransformLength(vals[2], 1.f)   : 0.f;
        mat.preConcat(SkM44::Translate(tx, ty, tz));
      }
      else if (name == "scale")
      {
        const float sx = vals.size() > 0 ? ResolveTransformLength(vals[0], 1.f) : 1.f;
        const float sy = vals.size() > 1 ? ResolveTransformLength(vals[1], 1.f) : sx;
        mat.preConcat(SkM44::Scale(sx, sy, 1));
      }
      else if (name == "scalex")
      {
        const float sx = vals.size() > 0 ? ResolveTransformLength(vals[0], 1.f) : 1.f;
        mat.preConcat(SkM44::Scale(sx, 1, 1));
      }
      else if (name == "scaley")
      {
        const float sy = vals.size() > 0 ? ResolveTransformLength(vals[0], 1.f) : 1.f;
        mat.preConcat(SkM44::Scale(1, sy, 1));
      }
      else if (name == "scalez")
      {
        const float sz = vals.size() > 0 ? ResolveTransformLength(vals[0], 1.f) : 1.f;
        mat.preConcat(SkM44::Scale(1, 1, sz));
      }
      else if (name == "scale3d")
      {
        const float sx = vals.size() > 0 ? ResolveTransformLength(vals[0], 1.f) : 1.f;
        const float sy = vals.size() > 1 ? ResolveTransformLength(vals[1], 1.f) : 1.f;
        const float sz = vals.size() > 2 ? ResolveTransformLength(vals[2], 1.f) : 1.f;
        mat.preConcat(SkM44::Scale(sx, sy, sz));
      }
      // rotate / rotateZ — Z-axis rotation (screen plane).  Positive = clockwise.
      else if (name == "rotate" || name == "rotatez")
      {
        const float rad = ResolveAngleDeg(vals.size() > 0 ? vals[0] : "0") * kDeg2Rad;
        mat.preConcat(SkM44::Rotate({0, 0, 1}, rad));
      }
      // rotateX(a) — 3-D rotation around the X axis.
      else if (name == "rotatex")
      {
        const float rad = ResolveAngleDeg(vals.size() > 0 ? vals[0] : "0") * kDeg2Rad;
        mat.preConcat(SkM44::Rotate({1, 0, 0}, rad));
      }
      // rotateY(a) — 3-D rotation around the Y axis.
      else if (name == "rotatey")
      {
        const float rad = ResolveAngleDeg(vals.size() > 0 ? vals[0] : "0") * kDeg2Rad;
        mat.preConcat(SkM44::Rotate({0, 1, 0}, rad));
      }
      // rotate3d(x, y, z, angle) — arbitrary axis rotation.
      else if (name == "rotate3d")
      {
        float rx = 0.f, ry = 0.f, rz = 0.f;
        try { if (vals.size() > 0) rx = std::stof(vals[0]); } catch (...) {}
        try { if (vals.size() > 1) ry = std::stof(vals[1]); } catch (...) {}
        try { if (vals.size() > 2) rz = std::stof(vals[2]); } catch (...) {}
        const float rad = ResolveAngleDeg(vals.size() > 3 ? vals[3] : "0") * kDeg2Rad;
        mat.preConcat(SkM44::Rotate({rx, ry, rz}, rad));
      }
      // perspective(d) — CSS inline perspective projection.
      // In local (origin-centered) space: x_screen = x / (1 - z/d).
      // The perspective vanishing point ends up at the transform-origin because
      // the entire matrix is wrapped with T(cx,cy)*...*T(-cx,-cy) below.
      else if (name == "perspective")
      {
        if (!vals.empty())
        {
          const float d = ResolveTransformLength(vals[0], 1.f);
          if (d != 0.f)
          {
            const SkM44 persp(
              1, 0,       0, 0,
              0, 1,       0, 0,
              0, 0,       1, 0,
              0, 0, -1.f/d, 1);
            mat.preConcat(persp);
          }
        }
      }
      // matrix(a,b,c,d,e,f) — raw CSS 2-D affine (column-major CSS notation).
      // CSS column layout:  [ a b 0 0 ]  [ c d 0 0 ]  [ 0 0 1 0 ]  [ e f 0 1 ]
      // SkM44 row-major:    row0: a c 0 e
      //                     row1: b d 0 f
      //                     row2: 0 0 1 0
      //                     row3: 0 0 0 1
      else if (name == "matrix")
      {
        if (vals.size() >= 6)
        {
          try {
            const SkM44 raw(
              std::stof(vals[0]), std::stof(vals[2]), 0, std::stof(vals[4]),
              std::stof(vals[1]), std::stof(vals[3]), 0, std::stof(vals[5]),
              0, 0, 1, 0,
              0, 0, 0, 1);
            mat.preConcat(raw);
          } catch (...) {}
        }
      }
      // matrix3d(a1,b1,c1,d1, a2,b2,c2,d2, a3,b3,c3,d3, a4,b4,c4,d4)
      // CSS uses column-major order; SkM44 constructor is row-major → transpose.
      else if (name == "matrix3d")
      {
        if (vals.size() >= 16)
        {
          try {
            const SkM44 raw(
              std::stof(vals[0]), std::stof(vals[4]), std::stof(vals[8]),  std::stof(vals[12]),
              std::stof(vals[1]), std::stof(vals[5]), std::stof(vals[9]),  std::stof(vals[13]),
              std::stof(vals[2]), std::stof(vals[6]), std::stof(vals[10]), std::stof(vals[14]),
              std::stof(vals[3]), std::stof(vals[7]), std::stof(vals[11]), std::stof(vals[15]));
            mat.preConcat(raw);
          } catch (...) {}
        }
      }
    }
    // ── Transform-origin wrap ─────────────────────────────────────────────────
    // CSS Transforms Level 2 §7.1: the effective matrix is
    //   T(ox, oy, 0) × mat × T(-ox, -oy, 0)
    // where (ox, oy) is the transform-origin.  This is applied ONCE around the
    // entire concatenated matrix — NOT per individual function.  This ensures
    // that perspective() also converges toward the transform-origin (element
    // center by default) rather than the screen origin.
    if (!(mat == SkM44{}))
      mat = SkM44::Translate(cx, cy, 0) * mat * SkM44::Translate(-cx, -cy, 0);
    return mat;
  }

  // Foreground (text / icon tint — mirrors CSS `color`).
  // Default is transparent (not white) so that the cascade can distinguish
  // "never set" from "explicitly set to white".  _mergedStyle() falls back to
  // white for root elements that have no ancestor providing a color.
  sk_color color = glint_color(0, 0, 0, 0);

  // Background — transparent by default, just like CSS
  sk_color backgroundColor = glint_color(0, 0, 0, 0);
  glint_optional_float opacity{1.f}; // 0.0 – 1.0  (isSet = true when set inline)

  // Gradient background — takes priority over backgroundColor when non-empty.
  // Format: sorted list of colour stops + type/angle/center/radius fields.
  //   type: "linear" (default), "radial", "conic"
  //   angle: degrees, 0=left→right, 90=top→bottom (linear + conic start)
  //   centerX/Y: [0..1] relative to element size, default 0.5 (center)
  //   radius: [0..1] relative to min(W,H)*0.5, default 1.0
  std::vector<sk_gradient_stop> backgroundGradient;
  std::string backgroundGradientType      = "linear";  // "linear"|"radial"|"conic"
  float       backgroundGradientAngle     = 0.f;        // degrees
  std::string backgroundGradientDirection;              // original keyword e.g. "to bottom right" (empty = numeric angle)
  float       backgroundGradientCX        = 0.5f;       // radial/conic center X [0..1]
  float       backgroundGradientCY     = 0.5f;       // radial/conic center Y [0..1]
  float       backgroundGradientRadius = 1.0f;       // radial radius [0..1] rel to min(W,H)*0.5

  // Background img — set via background-img: url(...) or the `background` shorthand.
  // Accepts the same disk paths as style.mask url(...): PNG/JPEG/WebP and SVG files.
  //   el.style.backgroundImage = "url(\"assets/bg.png\")";   // with quotes inside url()
  //   el.style.backgroundImage = "url(assets/bg.png)";       // without quotes
  //   el.style.background      = "url(\"assets/bg.png\")";   // via shorthand
  std::string backgroundImage    = "";           // disk path parsed from url(); "" = none
  std::string backgroundSize     = "auto";       // "auto" | "cover" | "contain" | "W H"
  std::string backgroundPosition = "50% 50%";   // CSS background-position
  std::string backgroundRepeat   = "no-repeat"; // "repeat" | "no-repeat" | "repeat-x" | "repeat-y"

  // ── background shorthand proxy ──────────────────────────────────────────────
  // Parses the CSS `background` shorthand and writes into the owning glint_style's
  // color/gradient fields.  Supports:
  //   style.background = "#rrggbb"                            — solid colour
  //   style.background = "rgba(r,g,b,a)"                     — solid colour
  //   style.background = "linear-gradient(angle, c1 p%, ...)" — linear gradient
  //   style.background = "radial-gradient(...stops)"          — radial gradient
  //   style.background = "conic-gradient(angle, ...stops)"   — conic gradient
  //   style.background = "none" / "transparent"              — clear
  // Angle follows CSS convention: 0deg = to top (bottom→top), 90deg = to right.
  struct sk_background_shorthand
  {
    sk_color*                      _pBgColor    = nullptr;
    std::vector<sk_gradient_stop>* _pStops      = nullptr;
    std::string*                   _pType       = nullptr;
    float*                         _pAngle      = nullptr;
    std::string*                   _pDirection  = nullptr;  // original direction keyword
    float*                         _pCX         = nullptr;
    float*                         _pCY         = nullptr;
    float*                         _pRadius     = nullptr;
    std::string*                   _pBgImage    = nullptr;  // backgroundImage field

    sk_background_shorthand() = default;
    sk_background_shorthand(sk_color* bgc, std::vector<sk_gradient_stop>* stops,
                            std::string* type, float* angle, std::string* direction,
                            float* cx,  float* cy, float* radius,
                            std::string* bgImage = nullptr)
      : _pBgColor(bgc), _pStops(stops), _pType(type), _pAngle(angle), _pDirection(direction)
      , _pCX(cx), _pCY(cy), _pRadius(radius), _pBgImage(bgImage) {}

    // Copy/move: preserve DEST's pointers (back-pointers always point to owner).
    sk_background_shorthand(const sk_background_shorthand&) {}
    sk_background_shorthand& operator=(const sk_background_shorthand&) { return *this; }

    // ── Internal helpers ─────────────────────────────────────────────────────
    static std::string _bgTrim(const std::string& s)
    {
      size_t b = 0;
      while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
      size_t e = s.size();
      while (e > b && std::isspace(static_cast<unsigned char>(s[e-1]))) --e;
      return s.substr(b, e - b);
    }
    static std::string _bgLower(std::string s)
    {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    // Split `s` by commas that are NOT inside parentheses.
    static std::vector<std::string> _splitArgs(const std::string& s)
    {
      std::vector<std::string> out;
      int depth = 0;
      std::string cur;
      for (char c : s)
      {
        if      (c == '(') { ++depth; cur += c; }
        else if (c == ')') { --depth; cur += c; }
        else if (c == ',' && depth == 0) { out.push_back(_bgTrim(cur)); cur.clear(); }
        else { cur += c; }
      }
      const std::string last = _bgTrim(cur);
      if (!last.empty()) out.push_back(last);
      return out;
    }

    // Parse CSS linear-gradient angle / direction keyword → internal degrees.
    // Internal convention (matches the render code):
    //   0deg   = to top   (bottom→top)
    //   90deg  = to right (left→right)
    //   180deg = to bottom
    //   270deg = to left
    static float _parseAngle(const std::string& raw)
    {
      const std::string s = _bgLower(_bgTrim(raw));
      if (s == "to top")                                   return   0.f;
      if (s == "to right")                                 return  90.f;
      if (s == "to bottom")                                return 180.f;
      if (s == "to left")                                  return 270.f;
      if (s == "to top right"    || s == "to right top")   return  45.f;
      if (s == "to bottom right" || s == "to right bottom")return 135.f;
      if (s == "to bottom left"  || s == "to left bottom") return 225.f;
      if (s == "to top left"     || s == "to left top")    return 315.f;
      try {
        float v = std::stof(s);
        if (s.size() > 3  && s.substr(s.size()-3)  == "rad")  v = v * (180.f / 3.14159265358979323846f);
        else if (s.size() > 4 && s.substr(s.size()-4) == "turn") v = v * 360.f;
        else if (s.size() > 4 && s.substr(s.size()-4) == "grad") v = v * (360.f / 400.f);
        return v;
      } catch (...) { return 0.f; }
    }

    // Parse one comma-separated gradient stop, e.g. "#2f2e2e47 0%",
    // "rgba(0,0,0,.5) 50%", or conic stops like "black 120deg".
    // The caller resolves omitted positions after the full stop list is parsed.
    static bool _parseStop(const std::string& raw, sk_gradient_stop& out, bool* outHasExplicitPos = nullptr)
    {
      const std::string t = _bgTrim(raw);
      if (t.empty()) return false;

      std::string colorPart;
      float pos = 0.f;
      bool hasExplicitPos = false;

      auto isAngleToken = [](const std::string& token) {
        const std::string low = _bgLower(_bgTrim(token));
        if (low.empty()) return false;
        return low.size() > 3  && low.substr(low.size() - 3) == "deg"
            || low.size() > 4  && low.substr(low.size() - 4) == "turn"
            || low.size() > 3  && low.substr(low.size() - 3) == "rad"
            || low.size() > 4  && low.substr(low.size() - 4) == "grad";
      };

      // The optional position suffix is the last whitespace-delimited word.
      // We accept percentages for linear/radial gradients and angle units for
      // conic gradients, normalizing angle stops into [0,1] turns.
      const size_t lastSp = t.rfind(' ');
      if (lastSp != std::string::npos)
      {
        const std::string maybePos = _bgTrim(t.substr(lastSp + 1));
        if (!maybePos.empty() && maybePos.back() == '%')
        {
          try { pos = std::stof(maybePos) / 100.f; }
          catch (...) {}
          colorPart = _bgTrim(t.substr(0, lastSp));
          hasExplicitPos = true;
        }
        else if (isAngleToken(maybePos))
        {
          float deg = 0.f;
          try {
            deg = ResolveAngleDeg(maybePos);
            pos = deg / 360.f;
            if (std::isfinite(pos))
            {
              pos = std::fmod(pos, 1.f);
              if (pos < 0.f) pos += 1.f;
            }
          } catch (...) {}
          colorPart = _bgTrim(t.substr(0, lastSp));
          hasExplicitPos = true;
        }
      }
      if (colorPart.empty()) colorPart = t;

      out.position = pos;
      out.color    = sk_color(colorPart.c_str()).value;
      if (outHasExplicitPos) *outHasExplicitPos = hasExplicitPos;
      return true;
    }

    static void _resolveGradientStopPositions(std::vector<sk_gradient_stop>& stops,
                          const std::vector<unsigned char>& explicitPos)
    {
      if (stops.empty()) return;

      const size_t count = stops.size();
      std::vector<unsigned char> fixed = explicitPos;
      if (fixed.size() != count) return;

      if (!fixed.front())
      {
        stops.front().position = 0.f;
        fixed.front() = true;
      }
      if (count > 1 && !fixed.back())
      {
        stops.back().position = 1.f;
        fixed.back() = true;
      }

      float lastFixedPos = stops.front().position;
      for (size_t i = 1; i < count; ++i)
      {
        if (!fixed[i]) continue;
        if (stops[i].position < lastFixedPos)
          stops[i].position = lastFixedPos;
        lastFixedPos = stops[i].position;
      }

      size_t prevFixed = 0;
      while (prevFixed + 1 < count)
      {
        size_t nextFixed = prevFixed + 1;
        while (nextFixed < count && !fixed[nextFixed]) ++nextFixed;
        if (nextFixed >= count) break;

        const size_t gapCount = nextFixed - prevFixed;
        if (gapCount > 1)
        {
          const float start = stops[prevFixed].position;
          const float end   = stops[nextFixed].position;
          for (size_t j = 1; j < gapCount; ++j)
          {
            const float t = static_cast<float>(j) / static_cast<float>(gapCount);
            stops[prevFixed + j].position = start + (end - start) * t;
          }
        }
        prevFixed = nextFixed;
      }
    }

    sk_background_shorthand& operator=(const char* css)
    {
      if (!css || !_pStops) return *this;

      const std::string src = _bgTrim(std::string(css));
      const std::string low = _bgLower(src);

      // Reset all gradient/color/img state
      _pStops->clear();
      *_pBgColor = glint_color(0, 0, 0, 0);
      *_pType    = "linear";
      *_pAngle   = 0.f;
      if (_pDirection) *_pDirection = "";
      *_pCX      = 0.5f;
      *_pCY      = 0.5f;
      *_pRadius  = 1.0f;
      if (_pBgImage) *_pBgImage = "";

      if (low == "none" || low == "transparent") return *this;

      // Detect url(...) → background-img from disk
      if (low.size() > 4 && low.substr(0, 4) == "url(")
      {
        const size_t open  = src.find('(');
        const size_t close = src.rfind(')');
        if (open != std::string::npos && close != std::string::npos && close > open)
        {
          std::string inner = src.substr(open + 1, close - open - 1);
          // Strip surrounding whitespace and quotes
          size_t b = inner.find_first_not_of(" \t\r\n\"'");
          size_t e = inner.find_last_not_of(" \t\r\n\"'");
          if (b != std::string::npos) inner = inner.substr(b, e - b + 1);
          else                        inner.clear();
          if (_pBgImage) *_pBgImage = inner;
        }
        return *this;
      }

      // Detect gradient function
      const bool isLinear = low.size() > 16 && low.substr(0, 16) == "linear-gradient(";
      const bool isRadial = !isLinear && low.size() > 16 && low.substr(0, 16) == "radial-gradient(";
      const bool isConic  = !isLinear && !isRadial && low.size() > 15 && low.substr(0, 15) == "conic-gradient(";

      if (isLinear || isRadial || isConic)
      {
        const size_t funcLen   = isLinear ? 16 : isRadial ? 16 : 15;
        const size_t closePar  = src.rfind(')');
        if (closePar == std::string::npos || closePar <= funcLen) return *this;

        const std::string inner = src.substr(funcLen, closePar - funcLen);
        *_pType = isLinear ? "linear" : isRadial ? "radial" : "conic";

        std::vector<std::string> args = _splitArgs(inner);
        if (args.empty()) return *this;

        size_t stopStart = 0;
        if (isLinear || isConic)
        {
          // First arg is an angle or direction keyword if it does NOT look like a colour.
          const std::string firstLow = _bgLower(_bgTrim(args[0]));
          const bool startsWithDigit = !firstLow.empty() &&
            (std::isdigit(static_cast<unsigned char>(firstLow[0])) ||
             firstLow[0] == '-' || firstLow[0] == '+' || firstLow[0] == '.');
          const bool isDir = firstLow.size() >= 2 && firstLow.substr(0, 2) == "to";
          const bool isAngleToken = startsWithDigit || isDir;
          if (isAngleToken) {
            *_pAngle = _parseAngle(args[0]);
            if (_pDirection) *_pDirection = isDir ? _bgLower(_bgTrim(args[0])) : "";
            stopStart = 1;
          }
        }
        else // radial — skip optional shape/size/position descriptor
        {
          const std::string firstLow = _bgLower(_bgTrim(args[0]));
          const bool looksLikeColor  = !firstLow.empty() &&
            (firstLow[0] == '#'  ||
             firstLow.substr(0, 4) == "rgb(" ||
             firstLow.substr(0, 5) == "rgba(" ||
             firstLow.substr(0, 4) == "hsl("  ||
             firstLow.substr(0, 5) == "hsla(");
          if (!looksLikeColor) stopStart = 1;
        }

        const size_t nStops = args.size() - stopStart;
        if (nStops < 2) return *this;
        _pStops->resize(nStops);
        std::vector<unsigned char> explicitPos(nStops, 0);
        for (size_t i = 0; i < nStops; ++i)
        {
          bool hasExplicitPos = false;
          _parseStop(args[stopStart + i], (*_pStops)[i], &hasExplicitPos);
          explicitPos[i] = hasExplicitPos ? 1 : 0;
        }
        _resolveGradientStopPositions(*_pStops, explicitPos);
        return *this;
      }

      // Plain solid colour (hex, rgb(), rgba(), named)
      *_pBgColor = sk_color(src.c_str());
      return *this;
    }

    sk_background_shorthand& operator=(const std::string& css)
    { return operator=(css.c_str()); }
  };

  // ── background shorthand instance ──────────────────────────────────────────
  // Assignment writes through to backgroundColor / backgroundGradient* / backgroundImage fields.
  // Read back those fields directly for rendering.
  sk_background_shorthand background {
    &backgroundColor,
    &backgroundGradient,
    &backgroundGradientType,
    &backgroundGradientAngle,
    &backgroundGradientDirection,
    &backgroundGradientCX,
    &backgroundGradientCY,
    &backgroundGradientRadius,
    &backgroundImage
  };

  // background-img: url("path") — distinct from the `background` shorthand so
  // backgroundSize / backgroundPosition / backgroundRepeat can be set independently.
  // Storing the parsed path directly avoids re-parsing the url() wrapper each frame.
  struct sk_bg_image_shorthand
  {
    std::string* _pBgImage = nullptr;
    sk_bg_image_shorthand() = default;
    explicit sk_bg_image_shorthand(std::string* p) : _pBgImage(p) {}
    sk_bg_image_shorthand(const sk_bg_image_shorthand&) {}
    sk_bg_image_shorthand& operator=(const sk_bg_image_shorthand&) { return *this; }

    sk_bg_image_shorthand& operator=(const char* css)
    {
      if (!_pBgImage || !css) return *this;
      const std::string src(css);
      // Accept bare paths OR url("...") / url(...)
      std::string low = src;
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (low == "none" || low == "") { *_pBgImage = ""; return *this; }
      if (low.size() > 4 && low.substr(0, 4) == "url(")
      {
        const size_t open  = src.find('(');
        const size_t close = src.rfind(')');
        if (open != std::string::npos && close != std::string::npos && close > open)
        {
          std::string inner = src.substr(open + 1, close - open - 1);
          size_t b = inner.find_first_not_of(" \t\r\n\"'");
          size_t e = inner.find_last_not_of(" \t\r\n\"'");
          *_pBgImage = (b != std::string::npos) ? inner.substr(b, e - b + 1) : "";
        }
        return *this;
      }
      // Bare path
      *_pBgImage = src;
      return *this;
    }
    sk_bg_image_shorthand& operator=(const std::string& s) { return operator=(s.c_str()); }
  } backgroundImageProp { &backgroundImage };

  // Border fields — set individually or via the `border` shorthand below.
  // CSS shorthand: style.border = "solid 1px #555555"
  // Token order is flexible: style keyword, size (Npx or N), colour (hex or named).
  // Style values: "solid" (default), "dashed", "dotted", "none"
  glint_length borderRadius = 0.f;                 // 0 = sharp corners; also accepts "50%", "8px"
  std::string  borderStyle  = "solid";             // "solid"|"dashed"|"dotted"|"none"
  float        strokeDashoffset = 0.f;             // CSS stroke-dashoffset: shifts the dash phase (px)

  // SVG fill / stroke decoration.
  // fill:   CSS SVG fill property — tints every path fill inside an <svg> element.
  // stroke: overrides border-color for the stroke paint.
  glint_optional_color fill;                         // fill:              SVG path fill tint colour
  glint_optional_color strokeColor;                  // stroke:            overrides border-color for the stroke paint
  std::string         strokeDasharray = "";          // stroke-dasharray:  "5 10" custom intervals; "" = use border-style
  std::string         strokeLinecap   = "butt";     // stroke-linecap:    "butt"|"round"|"square"
  std::string         strokeLinejoin  = "miter";    // stroke-linejoin:   "miter"|"round"|"bevel"
  float               strokeMiterlimit = 4.f;       // stroke-miterlimit  (SVG default = 4)
  glint_optional_float strokeOpacity{1.f};           // stroke-opacity:    multiplied onto stroke-color alpha
  float               strokeWidth     = 0.f;        // stroke-width:      0 = use border-width

  // ── Per-side border widths ──────────────────────────────────────────────────
  // Empty raw = inherit from global borderWidth. Accepts float (px), "Npx", "N.Npx".
  //   style.borderTopWidth = 2.f;   style.borderBottomWidth = "0px";  // no bottom
  glint_length borderTopWidth;
  glint_length borderRightWidth;
  glint_length borderBottomWidth;
  glint_length borderLeftWidth;

  // ── borderWidth shorthand proxy ─────────────────────────────────────────────
  // Accepts float (global) OR CSS multi-value string mirroring border-width:
  //   style.borderWidth = 1.f;                → global width (all un-overridden sides)
  //   style.borderWidth = "1px 0px 0px 1px";  → top=1 right=0 bottom=0 left=1
  // 1 value  → global (clears any per-side overrides)
  // 2 values → top/bottom, left/right
  // 3 values → top, left/right, bottom
  // 4 values → top, right, bottom, left
  struct sk_border_width_shorthand
  {
    float        _val  = 0.f;
    glint_length* _pTop = nullptr;
    glint_length* _pRgt = nullptr;
    glint_length* _pBot = nullptr;
    glint_length* _pLft = nullptr;

    sk_border_width_shorthand() = default;
    sk_border_width_shorthand(glint_length* t, glint_length* r, glint_length* b, glint_length* l)
      : _pTop(t), _pRgt(r), _pBot(b), _pLft(l) {}

    // Copy/move: preserve DEST back-pointers; transfer value only.
    sk_border_width_shorthand(const sk_border_width_shorthand& o) : _val(o._val) {}
    sk_border_width_shorthand& operator=(const sk_border_width_shorthand& o) { _val = o._val; return *this; }

    operator float() const { return _val; }
    sk_border_width_shorthand& operator=(float v) { _val = v; return *this; }

    sk_border_width_shorthand& operator=(const char* css)
    {
      if (!css) return *this;
      std::string src(css);
      std::istringstream ss(src);
      std::vector<std::string> toks;
      std::string tok;
      while (ss >> tok) toks.push_back(tok);
      if (toks.empty()) return *this;

      auto stripPx = [](const std::string& s) -> float {
        std::string v = s;
        if (v.size() >= 2 && v.substr(v.size() - 2) == "px")
          v = v.substr(0, v.size() - 2);
        try { return std::stof(v); } catch (...) { return 0.f; }
      };

      if (toks.size() == 1) {
        // Single value: set global, clear per-side overrides so they inherit
        _val = stripPx(toks[0]);
        if (_pTop) _pTop->raw.clear();
        if (_pRgt) _pRgt->raw.clear();
        if (_pBot) _pBot->raw.clear();
        if (_pLft) _pLft->raw.clear();
      } else {
        // Multi-value: distribute per CSS border-width spec
        glint_length v[4];
        if (toks.size() == 2) {
          v[0] = v[2] = toks[0];  // top, bottom
          v[1] = v[3] = toks[1];  // right, left
        } else if (toks.size() == 3) {
          v[0] = toks[0];          // top
          v[1] = v[3] = toks[1];  // right, left
          v[2] = toks[2];          // bottom
        } else {
          v[0] = toks[0];  // top
          v[1] = toks[1];  // right
          v[2] = toks[2];  // bottom
          v[3] = toks[3];  // left
        }
        if (_pTop) *_pTop = v[0];
        if (_pRgt) *_pRgt = v[1];
        if (_pBot) *_pBot = v[2];
        if (_pLft) *_pLft = v[3];
      }
      return *this;
    }
    sk_border_width_shorthand& operator=(const std::string& s) { return operator=(s.c_str()); }
  } borderWidth { &borderTopWidth, &borderRightWidth, &borderBottomWidth, &borderLeftWidth };

  // ── borderColor shorthand proxy ────────────────────────────────────────────
  // CSS border-color shorthand; dispatches 1–4 space-separated values to per-side
  // color fields, mirroring CSS border-color: top [right [bottom [left]]].
  //   style.borderColor = "#555"                              → all sides
  //   style.borderColor = "grey transparent"                  → top/bottom, left/right
  //   style.borderColor = "grey grey transparent transparent" → top right bottom left
  struct sk_border_color_shorthand
  {
    glint_color                value = glint_color(0, 0, 0, 0); // global fallback
    glint_optional_color* _pTop = nullptr;
    glint_optional_color* _pRgt = nullptr;
    glint_optional_color* _pBot = nullptr;
    glint_optional_color* _pLft = nullptr;

    sk_border_color_shorthand() = default;
    sk_border_color_shorthand(glint_optional_color* t, glint_optional_color* r,
                               glint_optional_color* b, glint_optional_color* l)
      : _pTop(t), _pRgt(r), _pBot(b), _pLft(l) {}

    // Copy: preserve DEST's back-pointers; transfer value only.
    sk_border_color_shorthand(const sk_border_color_shorthand& o) : value(o.value) {}
    sk_border_color_shorthand& operator=(const sk_border_color_shorthand& o) { value = o.value; return *this; }

    operator sk_color() const { return sk_color(value); }  // NOLINT
    operator glint_color()   const { return value; }            // NOLINT

    sk_border_color_shorthand& operator=(const sk_color& c)    { value = c.value; _clearSides(); return *this; }
    sk_border_color_shorthand& operator=(const glint_color& c)      { value = c;       _clearSides(); return *this; }
    sk_border_color_shorthand& operator=(const std::string& s) { return operator=(s.c_str()); }
    sk_border_color_shorthand& operator=(const char* css)
    {
      if (!css || !*css) return *this;
      std::string src(css);
      std::istringstream ss(src);
      std::vector<std::string> toks;
      std::string tok;
      while (ss >> tok) toks.push_back(tok);
      if (toks.empty()) return *this;

      if (toks.size() == 1) {
        // Single value: set global fallback, clear per-side overrides
        value = _resolveColor(toks[0]).value;
        _clearSides();
      } else {
        // Multi-value: CSS border-color shorthand
        glint_color v[4];
        if (toks.size() == 2) {
          v[0] = v[2] = _resolveColor(toks[0]).value;  // top, bottom
          v[1] = v[3] = _resolveColor(toks[1]).value;  // right, left
        } else if (toks.size() == 3) {
          v[0]        = _resolveColor(toks[0]).value;  // top
          v[1] = v[3] = _resolveColor(toks[1]).value;  // right, left
          v[2]        = _resolveColor(toks[2]).value;  // bottom
        } else {
          v[0] = _resolveColor(toks[0]).value;  // top
          v[1] = _resolveColor(toks[1]).value;  // right
          v[2] = _resolveColor(toks[2]).value;  // bottom
          v[3] = _resolveColor(toks[3]).value;  // left
        }
        if (_pTop) *_pTop = v[0];
        if (_pRgt) *_pRgt = v[1];
        if (_pBot) *_pBot = v[2];
        if (_pLft) *_pLft = v[3];
      }
      return *this;
    }

  private:
    static sk_color _resolveColor(const std::string& tok)
    {
      if (tok.empty()) return glint_color(0, 0, 0, 0);
      if (tok[0] == '#') return sk_color(tok.c_str());
      std::string low = tok;
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (low == "transparent") return glint_color(0, 0, 0, 0);
      if (low == "white")       return sk_color("#ffffff");
      if (low == "black")       return sk_color("#000000");
      if (low == "grey" || low == "gray") return sk_color("#808080");
      if (low == "red")         return sk_color("#ff0000");
      if (low == "green")       return sk_color("#00cc44");
      if (low == "blue")        return sk_color("#3399ff");
      return sk_color(tok.c_str()); // try to parse as-is
    }
    void _clearSides()
    {
      if (_pTop) _pTop->isSet = false;
      if (_pRgt) _pRgt->isSet = false;
      if (_pBot) _pBot->isSet = false;
      if (_pLft) _pLft->isSet = false;
    }
  };

  // ── border shorthand proxy ───────────────────────────────────────────────
  // Allows: style.border = "solid 1px #555555"
  //         style.border = "dashed 2px rgba is unsupported; use hex"
  //         style.border = "none"
  struct sk_border_shorthand
  {
    // Back-pointers into the owning glint_style's fields.
    // Rebind on every copy so they always point to THIS instance.
    std::string*  _pStyle    = nullptr;
    float*        _pWidth    = nullptr;  // global border only (borderWidth is float)
    glint_length* _pWidthL   = nullptr;  // per-side borders (glint_length, empty = inherit)
    sk_border_color_shorthand* _pColor     = nullptr;
    glint_optional_color*  _pSideColor = nullptr;  // non-null for per-side proxies

    sk_border_shorthand() = default;
    // Global border shorthand (borderWidth is float):
    sk_border_shorthand(std::string* ps, float* pw, sk_border_color_shorthand* pc)
      : _pStyle(ps), _pWidth(pw), _pColor(pc) {}
    // Per-side shorthand (width is glint_length, color is glint_optional_color):
    sk_border_shorthand(std::string* ps, glint_length* pw, glint_optional_color* pc)
      : _pStyle(ps), _pWidthL(pw), _pSideColor(pc) {}

    // Copy/move: preserve DEST's pointers (back-pointers always point to owner).
    sk_border_shorthand(const sk_border_shorthand&) {}
    sk_border_shorthand& operator=(const sk_border_shorthand&) { return *this; }

    static std::string _trim(const std::string& s)
    {
      size_t b = 0;
      while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
      size_t e = s.size();
      while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
      return s.substr(b, e - b);
    }

    static std::vector<std::string> _splitTopLevelWhitespace(const std::string& s)
    {
      std::vector<std::string> out;
      std::string cur;
      int depth = 0;
      for (char c : s)
      {
        if (c == '(')
        {
          ++depth;
          cur += c;
          continue;
        }
        if (c == ')')
        {
          if (depth > 0) --depth;
          cur += c;
          continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) && depth == 0)
        {
          const std::string tok = _trim(cur);
          if (!tok.empty()) out.push_back(tok);
          cur.clear();
          continue;
        }
        cur += c;
      }
      const std::string last = _trim(cur);
      if (!last.empty()) out.push_back(last);
      return out;
    }

    static sk_color resolveNamedColor(const std::string& tok)
    {
      if (tok.empty()) return glint_color(0, 0, 0, 0);
      if (tok[0] == '#') return sk_color(tok.c_str());
      // Common named colours
      if (tok == "white")       return sk_color("#ffffff");
      if (tok == "black")       return sk_color("#000000");
      if (tok == "grey" ||
          tok == "gray")        return sk_color("#808080");
      if (tok == "red")         return sk_color("#ff0000");
      if (tok == "green")       return sk_color("#00cc44");
      if (tok == "blue")        return sk_color("#3399ff");
      if (tok == "transparent") return glint_color(0, 0, 0, 0);
      if (tok == "white")       return sk_color("#ffffff");
      return sk_color(tok.c_str());
    }

    sk_border_shorthand& operator=(const char* css)
    {
      if (!css || (!_pWidth && !_pWidthL)) return *this;

      // "none" clears the border
      std::string src(css);
      if (src == "none" || src == "0")
      {
        *_pStyle = "none";
        if (_pWidthL) *_pWidthL = 0.f; else *_pWidth = 0.f;
        if (_pSideColor) _pSideColor->isSet = false;
        return *this;
      }

      for (const std::string& tok : _splitTopLevelWhitespace(src))
      {
        if (tok.empty()) continue;
        // Lowercase token for style/named-colour matching
        std::string low = tok;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (low == "solid" || low == "dashed" ||
            low == "dotted" || low == "none")
        {
          *_pStyle = low;
        }
        else if (std::isdigit(static_cast<unsigned char>(low[0])) ||
                 low[0] == '+' || low[0] == '-' ||
                 (low[0] == '.' && low.size() > 1))
        {
          // Size token: "1px", "1.5px", "2"
          if (_pWidthL) *_pWidthL = tok;            // glint_length preserves "1px" etc.
          else
          {
            try { *_pWidth = std::stof(tok); } catch (...) {}
          }
        }
        else
        {
          // Colour token (hex, named, rgb(), rgba(), ...)
          const sk_color resolved = resolveNamedColor(low[0] == '#' ? tok : low);
          if (_pSideColor) *_pSideColor = resolved;
          else if (_pColor) *_pColor = resolved;
        }
      }
      return *this;
    }

    sk_border_shorthand& operator=(const std::string& css)
    { return operator=(css.c_str()); }
  };

  // ── Per-corner border radii ─────────────────────────────────────────────────
  // Empty raw = inherit from global borderRadius. Accepts float (px), "Npx", "N%".
  // % resolves against min(W, H) matching the CSS circle idiom.
  //   style.borderTopLeftRadius     = 12.f;   style.borderTopRightRadius    = 12.f;
  //   style.borderBottomRightRadius = 0.f;    style.borderBottomLeftRadius  = 0.f;
  glint_length borderTopLeftRadius;
  glint_length borderTopRightRadius;
  glint_length borderBottomRightRadius;
  glint_length borderBottomLeftRadius;

  // ── Per-side border colors ──────────────────────────────────────────────────
  // Assignment to any of these automatically marks it as set (isSet = true),
  // so resolvedBorderColor() can distinguish "not overridden" from "transparent".
  glint_optional_color borderTopColor;
  glint_optional_color borderRightColor;
  glint_optional_color borderBottomColor;
  glint_optional_color borderLeftColor;

  // ── borderColor shorthand instance ─────────────────────────────────────────
  // Declared after per-side colors so back-pointers are valid at in-class init.
  sk_border_color_shorthand borderColor { &borderTopColor, &borderRightColor, &borderBottomColor, &borderLeftColor };

  // ── border shorthand instance ───────────────────────────────────────────────
  sk_border_shorthand border { &borderStyle, &borderWidth._val, &borderColor };

  // ── Per-side border styles ──────────────────────────────────────────────────
  // "" = inherit from global borderStyle.
  std::string borderTopStyle;
  std::string borderRightStyle;
  std::string borderBottomStyle;
  std::string borderLeftStyle;

  // ── Per-side border shorthand proxies ───────────────────────────────────────
  // Usage: style.borderTop = "solid 2px #ff0000"   style.borderBottom = "none"
  sk_border_shorthand borderTop    { &borderTopStyle,    &borderTopWidth,    &borderTopColor    };
  sk_border_shorthand borderRight  { &borderRightStyle,  &borderRightWidth,  &borderRightColor  };
  sk_border_shorthand borderBottom { &borderBottomStyle, &borderBottomWidth, &borderBottomColor };
  sk_border_shorthand borderLeft   { &borderLeftStyle,   &borderLeftWidth,   &borderLeftColor   };

  // ── Resolve helpers ─────────────────────────────────────────────────────────

  // Returns [TL, TR, BR, BL] effective radii in pixels.
  // Per-corner empty raw → falls back to global borderRadius.
  // % resolves against min(W, H).
  std::array<float, 4> resolveCornerRadii(float W, float H) const
  {
    const float m    = std::min(W, H);
    const float glob = borderRadius.resolve(m);
    return {{
      borderTopLeftRadius.raw.empty()     ? glob : borderTopLeftRadius.resolve(m),
      borderTopRightRadius.raw.empty()    ? glob : borderTopRightRadius.resolve(m),
      borderBottomRightRadius.raw.empty() ? glob : borderBottomRightRadius.resolve(m),
      borderBottomLeftRadius.raw.empty()  ? glob : borderBottomLeftRadius.resolve(m),
    }};
  }

  // Effective border width for one side (0=top 1=right 2=bottom 3=left).
  // -1.f per-side → falls back to global borderWidth.
  float resolvedBorderWidth(int side) const
  {
    switch (side)
    {
      case 0: return !borderTopWidth.raw.empty()    ? borderTopWidth.resolve(0)    : borderWidth;
      case 1: return !borderRightWidth.raw.empty()  ? borderRightWidth.resolve(0)  : borderWidth;
      case 2: return !borderBottomWidth.raw.empty() ? borderBottomWidth.resolve(0) : borderWidth;
      case 3: return !borderLeftWidth.raw.empty()   ? borderLeftWidth.resolve(0)   : borderWidth;
    }
    return borderWidth;
  }

  // Effective border color for one side.
  glint_color resolvedBorderColor(int side) const
  {
    switch (side)
    {
      case 0: if (borderTopColor.isSet)    return borderTopColor.value;    break;
      case 1: if (borderRightColor.isSet)  return borderRightColor.value;  break;
      case 2: if (borderBottomColor.isSet) return borderBottomColor.value; break;
      case 3: if (borderLeftColor.isSet)   return borderLeftColor.value;   break;
    }
    return borderColor.value;
  }

  // Effective border style for one side ("" → global borderStyle).
  const std::string& resolvedBorderStyle(int side) const
  {
    switch (side)
    {
      case 0: if (!borderTopStyle.empty())    return borderTopStyle;    break;
      case 1: if (!borderRightStyle.empty())  return borderRightStyle;  break;
      case 2: if (!borderBottomStyle.empty()) return borderBottomStyle; break;
      case 3: if (!borderLeftStyle.empty())   return borderLeftStyle;   break;
    }
    return borderStyle;
  }

  // True if any per-side property overrides the global border values.
  bool hasSidedBorder() const
  {
    return !borderTopWidth.raw.empty()    || !borderRightWidth.raw.empty()  ||
           !borderBottomWidth.raw.empty() || !borderLeftWidth.raw.empty()   ||
           borderTopColor.isSet    || borderRightColor.isSet  ||
           borderBottomColor.isSet || borderLeftColor.isSet   ||
           !borderTopStyle.empty()    || !borderRightStyle.empty()  ||
           !borderBottomStyle.empty() || !borderLeftStyle.empty();
  }

  // True if any per-corner radius is explicitly set.
  bool hasNonUniformRadius() const
  {
    return !borderTopLeftRadius.raw.empty()     ||
           !borderTopRightRadius.raw.empty()    ||
           !borderBottomRightRadius.raw.empty() ||
           !borderBottomLeftRadius.raw.empty();
  }

  // ── Constructors / copy semantics ─────────────────────────────────────────
  // Custom copy ctor + copy-assign so `border` proxy pointers are rebound
  // to THIS instance's fields, not the source's.

  glint_style()
    : background(&backgroundColor, &backgroundGradient, &backgroundGradientType,
                 &backgroundGradientAngle, &backgroundGradientDirection,
                 &backgroundGradientCX,   &backgroundGradientCY,
                 &backgroundGradientRadius, &backgroundImage)
    , backgroundImageProp(&backgroundImage)
    , border(&borderStyle, &borderWidth._val, &borderColor)
    , paddingTop   (&padding.top)   , paddingRight (&padding.right)
    , paddingBottom(&padding.bottom), paddingLeft  (&padding.left)
    , marginTop   (&margin.top,    &margin.rawTop)   , marginRight (&margin.right,  &margin.rawRight)
    , marginBottom(&margin.bottom, &margin.rawBottom), marginLeft  (&margin.left,   &margin.rawLeft)
    , overflow(&overflowX, &overflowY)
    , borderTop   (&borderTopStyle,    &borderTopWidth,    &borderTopColor)
    , borderRight (&borderRightStyle,  &borderRightWidth,  &borderRightColor)
    , borderBottom(&borderBottomStyle, &borderBottomWidth, &borderBottomColor)
    , borderLeft  (&borderLeftStyle,   &borderLeftWidth,   &borderLeftColor)
    , boxShadow(&shadowEnabled, &shadowColor, &shadowOffsetX, &shadowOffsetY,
                &shadowBlur, &shadowSpread, &shadowInset)
  {}

  glint_style(const glint_style& o)
    : color(o.color), backgroundColor(o.backgroundColor), opacity(o.opacity)
    , backgroundGradient(o.backgroundGradient)
    , backgroundGradientType(o.backgroundGradientType)
    , backgroundGradientAngle(o.backgroundGradientAngle)
    , backgroundGradientDirection(o.backgroundGradientDirection)
    , backgroundGradientCX(o.backgroundGradientCX), backgroundGradientCY(o.backgroundGradientCY)
    , backgroundGradientRadius(o.backgroundGradientRadius)
    , backgroundImage(o.backgroundImage)
    , backgroundSize(o.backgroundSize)
    , backgroundPosition(o.backgroundPosition)
    , backgroundRepeat(o.backgroundRepeat)
    , borderColor(o.borderColor), borderWidth(o.borderWidth)
    , borderRadius(o.borderRadius), borderStyle(o.borderStyle), strokeDashoffset(o.strokeDashoffset)
    , fill(o.fill)
    , strokeColor(o.strokeColor), strokeDasharray(o.strokeDasharray)
    , strokeLinecap(o.strokeLinecap), strokeLinejoin(o.strokeLinejoin)
    , strokeMiterlimit(o.strokeMiterlimit), strokeOpacity(o.strokeOpacity), strokeWidth(o.strokeWidth)
    , shadowEnabled(o.shadowEnabled), shadowColor(o.shadowColor)
    , shadowOffsetX(o.shadowOffsetX), shadowOffsetY(o.shadowOffsetY), shadowBlur(o.shadowBlur)
    , shadowSpread(o.shadowSpread), shadowInset(o.shadowInset)
    , boxShadow(&shadowEnabled, &shadowColor, &shadowOffsetX, &shadowOffsetY,
          &shadowBlur, &shadowSpread, &shadowInset)
    , fontSize(o.fontSize), lineHeight(o.lineHeight), fontFamily(o.fontFamily), fontStyle(o.fontStyle), fontWeight(o.fontWeight)
    , textAlign(o.textAlign), verticalAlign(o.verticalAlign), textDecoration(o.textDecoration)
    , selectionColor(o.selectionColor)
    , padding(o.padding)
    , paddingTop   (&padding.top)   , paddingRight (&padding.right)
    , paddingBottom(&padding.bottom), paddingLeft  (&padding.left)
    , margin(o.margin)
    , marginTop   (&margin.top,    &margin.rawTop)   , marginRight (&margin.right,  &margin.rawRight)
    , marginBottom(&margin.bottom, &margin.rawBottom), marginLeft  (&margin.left,   &margin.rawLeft)
    , position(o.position)
    , left(o.left), top(o.top), right(o.right), bottom(o.bottom), width(o.width), height(o.height)
    , minWidth(o.minWidth), maxWidth(o.maxWidth), minHeight(o.minHeight), maxHeight(o.maxHeight)
    , zIndex(o.zIndex)
    , display(o.display), pointerEvents(o.pointerEvents), cursor(o.cursor), userSelect(o.userSelect), whiteSpace(o.whiteSpace), flexDirection(o.flexDirection)
    , justifyContent(o.justifyContent), alignItems(o.alignItems), gap(o.gap), flexGrow(o.flexGrow)
    , overflowX(o.overflowX), overflowY(o.overflowY)
    , overflow(&overflowX, &overflowY)                   // rebind to THIS
    , scrollbarWidth(o.scrollbarWidth)
    , scrollbarThumbColor(o.scrollbarThumbColor)
    , scrollbarTrackColor(o.scrollbarTrackColor)
    , scrollbarButtonColor(o.scrollbarButtonColor)
    , scrollbarThumbBorderRadius(o.scrollbarThumbBorderRadius)
    , scrollbarThumbWidth(o.scrollbarThumbWidth)
    , scrollbarThumbHeight(o.scrollbarThumbHeight)
    , objectFit(o.objectFit), objectPosition(o.objectPosition)
    , transform(o.transform)
    , filter(o.filter)
    , backdropFilter(o.backdropFilter)
    , mask(o.mask), maskMode(o.maskMode)
    , maskPosition(o.maskPosition), maskSize(o.maskSize)
    , maskRepeat(o.maskRepeat), maskOrigin(o.maskOrigin)
    , maskClip(o.maskClip), maskComposite(o.maskComposite)
    , mixBlendMode(o.mixBlendMode)
    , backgroundBlendMode(o.backgroundBlendMode)
    , isolation(o.isolation)
    , transition(o.transition)
    , animation(o.animation)
    , borderTopLeftRadius(o.borderTopLeftRadius)
    , borderTopRightRadius(o.borderTopRightRadius)
    , borderBottomRightRadius(o.borderBottomRightRadius)
    , borderBottomLeftRadius(o.borderBottomLeftRadius)
    , borderTopWidth(o.borderTopWidth), borderRightWidth(o.borderRightWidth)
    , borderBottomWidth(o.borderBottomWidth), borderLeftWidth(o.borderLeftWidth)
    , borderTopColor(o.borderTopColor)
    , borderRightColor(o.borderRightColor)
    , borderBottomColor(o.borderBottomColor)
    , borderLeftColor(o.borderLeftColor)
    , borderTopStyle(o.borderTopStyle), borderRightStyle(o.borderRightStyle)
    , borderBottomStyle(o.borderBottomStyle), borderLeftStyle(o.borderLeftStyle)
    , background(&backgroundColor, &backgroundGradient, &backgroundGradientType,  // rebind to THIS
                 &backgroundGradientAngle, &backgroundGradientDirection,
                 &backgroundGradientCX,   &backgroundGradientCY,
                 &backgroundGradientRadius, &backgroundImage)
    , backgroundImageProp(&backgroundImage)              // rebind to THIS
    , border(&borderStyle, &borderWidth._val, &borderColor)   // rebind to THIS
    , borderTop   (&borderTopStyle,    &borderTopWidth,    &borderTopColor)
    , borderRight (&borderRightStyle,  &borderRightWidth,  &borderRightColor)
    , borderBottom(&borderBottomStyle, &borderBottomWidth, &borderBottomColor)
    , borderLeft  (&borderLeftStyle,   &borderLeftWidth,   &borderLeftColor)
  {
    // Rebind borderColor's per-side back-pointers to THIS instance's color fields
    // (sk_border_color_shorthand's copy ctor copies only `value`, leaving them null).
    borderColor._pTop = &borderTopColor;
    borderColor._pRgt = &borderRightColor;
    borderColor._pBot = &borderBottomColor;
    borderColor._pLft = &borderLeftColor;
    boxShadow = o.boxShadow;
  }

  glint_style& operator=(const glint_style& o)
  {
    if (this == &o) return *this;
    color           = o.color;
    backgroundColor = o.backgroundColor;
    opacity         = o.opacity;
    backgroundGradient       = o.backgroundGradient;
    backgroundGradientType   = o.backgroundGradientType;
    backgroundGradientAngle     = o.backgroundGradientAngle;
    backgroundGradientDirection  = o.backgroundGradientDirection;
    backgroundGradientCX         = o.backgroundGradientCX;
    backgroundGradientCY     = o.backgroundGradientCY;
    backgroundGradientRadius = o.backgroundGradientRadius;
    backgroundImage          = o.backgroundImage;
    backgroundSize           = o.backgroundSize;
    backgroundPosition       = o.backgroundPosition;
    backgroundRepeat         = o.backgroundRepeat;
    borderColor     = o.borderColor;
    borderWidth     = o.borderWidth;
    borderRadius       = o.borderRadius;
    borderStyle        = o.borderStyle;
    strokeDashoffset   = o.strokeDashoffset;
    fill               = o.fill;
    strokeColor        = o.strokeColor;
    strokeDasharray    = o.strokeDasharray;
    strokeLinecap      = o.strokeLinecap;
    strokeLinejoin     = o.strokeLinejoin;
    strokeMiterlimit   = o.strokeMiterlimit;
    strokeOpacity      = o.strokeOpacity;
    strokeWidth        = o.strokeWidth;
    shadowEnabled      = o.shadowEnabled;
    shadowColor     = o.shadowColor;
    shadowOffsetX   = o.shadowOffsetX;
    shadowOffsetY   = o.shadowOffsetY;
    shadowBlur      = o.shadowBlur;
    shadowSpread    = o.shadowSpread;
    shadowInset     = o.shadowInset;
    fontSize        = o.fontSize;
    lineHeight      = o.lineHeight;
    fontFamily      = o.fontFamily;
    fontStyle       = o.fontStyle;
    fontWeight      = o.fontWeight;
    textAlign       = o.textAlign;
    verticalAlign   = o.verticalAlign;
    textDecoration  = o.textDecoration;
    selectionColor  = o.selectionColor;
    padding         = o.padding;
    // padding/margin side proxies already point to our own SKEdgeInsets — don't overwrite.
    margin          = o.margin;
    position        = o.position;
    left            = o.left;
    top             = o.top;
    right           = o.right;
    bottom          = o.bottom;
    width           = o.width;
    height          = o.height;
    minWidth        = o.minWidth;
    maxWidth        = o.maxWidth;
    minHeight       = o.minHeight;
    maxHeight       = o.maxHeight;
    zIndex          = o.zIndex;
    display         = o.display;
    pointerEvents   = o.pointerEvents;
    cursor          = o.cursor;
    userSelect      = o.userSelect;
    whiteSpace      = o.whiteSpace;
    flexDirection   = o.flexDirection;
    justifyContent  = o.justifyContent;
    alignItems      = o.alignItems;
    gap             = o.gap;
    flexGrow        = o.flexGrow;
    objectFit          = o.objectFit;
    objectPosition     = o.objectPosition;
    overflowX          = o.overflowX;
    overflowY          = o.overflowY;
    // overflow proxy pointers already point to our own fields — don't overwrite.
    scrollbarWidth             = o.scrollbarWidth;
    scrollbarThumbColor        = o.scrollbarThumbColor;
    scrollbarTrackColor        = o.scrollbarTrackColor;
    scrollbarButtonColor       = o.scrollbarButtonColor;
    scrollbarThumbBorderRadius = o.scrollbarThumbBorderRadius;
    scrollbarThumbWidth        = o.scrollbarThumbWidth;
    scrollbarThumbHeight       = o.scrollbarThumbHeight;
    transform        = o.transform;
    filter           = o.filter;
    backdropFilter   = o.backdropFilter;
    mask             = o.mask;
    maskMode         = o.maskMode;
    maskPosition     = o.maskPosition;
    maskSize         = o.maskSize;
    maskRepeat       = o.maskRepeat;
    maskOrigin       = o.maskOrigin;
    maskClip         = o.maskClip;
    maskComposite    = o.maskComposite;
    mixBlendMode     = o.mixBlendMode;
    backgroundBlendMode = o.backgroundBlendMode;
    isolation        = o.isolation;
    transition       = o.transition;
    animation        = o.animation;
    borderTopLeftRadius     = o.borderTopLeftRadius;
    borderTopRightRadius    = o.borderTopRightRadius;
    borderBottomRightRadius = o.borderBottomRightRadius;
    borderBottomLeftRadius  = o.borderBottomLeftRadius;
    borderTopWidth    = o.borderTopWidth;  borderRightWidth  = o.borderRightWidth;
    borderBottomWidth = o.borderBottomWidth; borderLeftWidth = o.borderLeftWidth;
    borderTopColor    = o.borderTopColor;
    borderRightColor  = o.borderRightColor;
    borderBottomColor = o.borderBottomColor;
    borderLeftColor   = o.borderLeftColor;
    borderTopStyle    = o.borderTopStyle;    borderRightStyle  = o.borderRightStyle;
    borderBottomStyle = o.borderBottomStyle; borderLeftStyle   = o.borderLeftStyle;
    // border proxy pointers already point to our own fields — don't overwrite.
    boxShadow = o.boxShadow;
    return *this;
  }

  // Shadow
  bool     shadowEnabled = false;
  sk_color shadowColor   = glint_color(120, 0, 0, 0);
  float    shadowOffsetX = 2.f;
  float    shadowOffsetY = 2.f;
  float    shadowBlur    = 4.f;
  float    shadowSpread  = 0.f;
  bool     shadowInset   = false;
  glint_box_shadow_proxy boxShadow;

  // Typography (mirrors CSS font-size / font-family / line-height / font-style)
  glint_length fontSize  = 14.f;  // float or "16px" string
  float lineHeight       = 1.2f;  // CSS line-height multiplier (1.2 = browser default)
  std::string fontFamily  = "";   // CSS font-family (e.g. "Kanit", "Roboto")
  std::string fontStyle   = "";   // CSS font-style: "" | "normal" | "italic" | "oblique"
  glint_optional_float fontWeight{400.f};  // CSS font-weight: 100–900; isSet tracks explicit inline assignment
  glint_text_align textAlign = EAlign::Near;
  std::string verticalAlign = "baseline";  // CSS vertical-align: baseline | middle | sub | super | text-top | text-bottom | top | bottom | <length> | <percentage>
  std::string textDecoration = "";  // "" | "none" | "underline" | "line-through" | "underline line-through"
  sk_color    selectionColor = glint_color(180, 93, 177, 255);  // CSS ::selection highlight

  // Spacing
  // style.padding = "8 12";    ← shorthand (top/bottom  left/right)
  // style.padding = "8 12 8 12"; ← top right bottom left
  // style.paddingLeft = "12px";  ← individual side
  SKEdgeInsets  padding;
  // Initialized in constructors (not NSDMIs) to avoid MSVC friendship-in-NSDMI bug.
  sk_side_proxy paddingTop;
  sk_side_proxy paddingRight;
  sk_side_proxy paddingBottom;
  sk_side_proxy paddingLeft;

  // style.margin = "10 20";     ← shorthand
  // style.marginLeft = 5.f;    ← individual side
  SKEdgeInsets  margin;
  sk_side_proxy marginTop;
  sk_side_proxy marginRight;
  sk_side_proxy marginBottom;
  sk_side_proxy marginLeft;

  // Positioning — mirrors CSS `position`, `left`, `top`, `right`, `bottom`, `width`, `height`.
  // "" / "static":   default. Normal flow; top/left/right/bottom have no effect. NOT a
  //                  containing block — absolute children skip past this element to find a
  //                  positioned ancestor. Matches Chrome/CSS spec default.
  // "relative":      stays in flow; top/left/right/bottom are visual DELTAS from the flow
  //                  position. Also makes this element a containing block for absolute descendants.
  // "absolute":      out of flow; top/left/right/bottom are relative to the nearest positioned
  //                  ancestor (position != "" / "static"). Removed from normal flow sizing.
  // Browser CSS precedence: left wins over right; top wins over bottom.
  // All fields accept floats (pixels) or strings ("50%", "12px").
  std::string  position = "";  // "" = static (Chrome default)
  glint_length left;    // CSS left
  glint_length top;     // CSS top
  glint_length right;   // CSS right  (used when left is unset)
  glint_length bottom;  // CSS bottom (used when top is unset)
  glint_length width;     // CSS width
  glint_length height;    // CSS height
  glint_length minWidth;  // CSS min-width  (default: unset = 0)
  glint_length maxWidth;  // CSS max-width  (default: unset = none)
  glint_length minHeight; // CSS min-height (default: unset = 0)
  glint_length maxHeight; // CSS max-height (default: unset = none)

  // CSS z-index — controls sibling paint order within the same stacking context.
  // Higher values paint on top. Default 0 matches CSS `z-index: auto` / 0 behaviour.
  // Negative values paint before the parent's own content on the active standalone path,
  // which is closer to Chrome's stacking order for positioned descendants.
  int zIndex = 0;

  // ── Flex layout ─────────────────────────────────────────────────────────────
  // Set display = "flex" on a PANEL to make it lay out its children with flex.
  //
  //   panelStyle.display        = "flex";
  //   panelStyle.flexDirection  = "row";          // "row" | "column"
  //   panelStyle.justifyContent = "center";       // main axis
  //   panelStyle.alignItems     = "center";       // cross axis
  //   panelStyle.gap            = 8.f;            // px between items (float or "8px" string)
  //
  // justifyContent: "flex-start" (default) | "center" | "flex-end"
  //                 "space-between" | "space-around" | "space-evenly"
  // alignItems:     "flex-start" (default) | "center" | "flex-end" | "stretch"
  std::string display         = "";            // "" | "block" | "inline" | "flex" | "table" | "table-row" | "table-cell"
  std::string pointerEvents   = "";            // "" (auto) | "none"
  std::string cursor          = "";            // "" (default) | "pointer" | "text" | "crosshair" | "move" | "grab" | "grabbing" | "not-allowed" | "wait" | "progress" | "help" | "none" | resize variants | etc.
  std::string userSelect      = "";            // "" (auto) | "none" | "text" | "all"
  std::string whiteSpace      = "";            // "" (normal) | "nowrap" | "pre" | "pre-line" | "pre-wrap" | "break-spaces"
  std::string flexDirection   = "row";         // "row" | "column"
  std::string justifyContent  = "flex-start";  // main-axis alignment
  std::string alignItems      = "flex-start";  // cross-axis alignment
  glint_length gap           = 0.f;           // gap between items (px or "8px" string)
  float       flexGrow        = 0.f;           // CSS flex-grow: expand to fill remaining space

  // CSS overflow-x / overflow-y — per-axis clip and scroll control.
  // "visible" (default): children paint outside the component bounds unclipped.
  // "hidden":  clip to padding box (no scroll).
  // "scroll":  always show scrollbar; clip and scroll content.
  // "auto":    show scrollbar only when content overflows; clip and scroll.
  // The `overflow` field is a shorthand that sets both axes simultaneously.
  std::string overflowX = "visible";
  std::string overflowY = "visible";

  struct sk_overflow_shorthand
  {
    std::string* _pX = nullptr;
    std::string* _pY = nullptr;

    sk_overflow_shorthand() = default;
    sk_overflow_shorthand(std::string* px, std::string* py) : _pX(px), _pY(py) {}

    // Copy/move: preserve DEST pointers (always point to the owning instance).
    sk_overflow_shorthand(const sk_overflow_shorthand&) {}
    sk_overflow_shorthand& operator=(const sk_overflow_shorthand&) { return *this; }

    sk_overflow_shorthand& operator=(const char* v)
    {
      const char* val = v ? v : "visible";
      if (_pX) *_pX = val;
      if (_pY) *_pY = val;
      return *this;
    }
    sk_overflow_shorthand& operator=(const std::string& v) { return operator=(v.c_str()); }

    // Comparison helpers — true when BOTH axes have the given value.
    bool operator==(const char* s)        const { return _pX && _pY && *_pX == s && *_pY == s; }
    bool operator!=(const char* s)        const { return !(*this == s); }
    bool operator==(const std::string& s) const { return operator==(s.c_str()); }
    bool operator!=(const std::string& s) const { return !(*this == s); }

    // Implicit read: returns overflowX (the canonical value when both axes are equal).
    operator std::string() const { static const std::string def = "visible"; return _pX ? *_pX : def; } // NOLINT
  } overflow { &overflowX, &overflowY };

  // ── Scrollbar appearance ──────────────────────────────────────────────────────
  // Mirrors CSS scrollbar-width and scrollbar-color (Firefox / CSS Scrollbars spec).
  //
  //   style.scrollbarWidth       = 12.f;         // track+thumb combined width (px)
  //   style.scrollbarThumbColor  = "#888";       // draggable thumb colour
  //   style.scrollbarTrackColor  = "#222";       // track background colour
  //   style.scrollbarButtonColor = "#444";       // arrow button colour
  //
  // When unset on the glint_scrollbar itself, these are inherited from the
  // parent scrollable component at construction time.
  float    scrollbarWidth             = 12.f;
  sk_color scrollbarThumbColor        = glint_color(255, 110, 110, 110);
  sk_color scrollbarTrackColor        = glint_color(255,  40,  40,  40);
  sk_color scrollbarButtonColor       = glint_color(255,  65,  65,  65);
  float    scrollbarThumbBorderRadius = 3.f;    // thumb corner radius (px)
  float    scrollbarThumbWidth        = -1.f;   // thumb width for vertical bar; -1 = fill track
  float    scrollbarThumbHeight       = -1.f;   // thumb height for horizontal bar; -1 = fill track

  // Object fit — img/media components (mirrors CSS object-fit / object-position).
  // Consumed by glint_image; ignored by other components.
  std::string objectFit      = "fill";           // "fill"|"contain"|"cover"|"none" — matches Chrome <img> default
  std::string objectPosition = "center center";  // "left|center|right  top|center|middle|bottom"

  // CSS transform (translate subset).
  // Supported functions: translate(), translateX(), translateY().
  // Percent values are relative to this component's own size.
  // Examples:
  //   style.transform = "translateX(-50%)";
  //   style.transform = "translate(10px, 20px)";
  //   style.transform = "translateY(8)";
  std::string transform = "";

  // CSS filter — applied to the entire component layer (Skia backend only).
  // Syntax identical to CSS: chainable, space-separated function calls.
  // Examples:
  //   style.filter = "blur(4px)";
  //   style.filter = "saturate(0) brightness(0.8)";
  //   style.filter = "drop-shadow(2px 4px 6px #000000)";
  //   style.filter = "hue-rotate(180deg) invert(0.2)";
  // Supported: blur, brightness, contrast, saturate, grayscale, sepia,
  //            invert, opacity, hue-rotate, drop-shadow
  glint_shader_string filter;         // "" / "none" = no filter

  // CSS backdrop-filter — applied to the area behind the element (Skia backend only).
  // Same syntax as `filter`. The element itself is rendered on top of the filtered backdrop.
  // Examples:
  //   style.backdropFilter = "blur(8px)";
  //   style.backdropFilter = "blur(4px) brightness(1.2)";
  //   style.backdropFilter = "saturate(2.0) hue-rotate(30deg)";
  // Pair with a semi-transparent backgroundColor for the classic frosted-glass effect.
  glint_shader_string backdropFilter;  // "" / "none" = no backdrop filter

  // ── CSS blend modes ─────────────────────────────────────────────────────────
  // mix-blend-mode: how this element's layer composites with its backdrop.
  //   style.mixBlendMode = "multiply";   // screen, overlay, darken, lighten …
  //   style.mixBlendMode = "normal";     // (default — no blending)
  // Creates a stacking context (like opacity < 1).  Requires Skia backend.
  // All 16 CSS Compositing Level 1 keywords are supported.
  std::string mixBlendMode;          // "" / "normal" = kSrcOver

  // background-blend-mode: how background-img layers blend against
  // background-color (or each other when multiple images are stacked).
  //   style.backgroundBlendMode = "multiply";
  // Only affects background rendering — does NOT create a stacking context.
  std::string backgroundBlendMode;   // "" / "normal" = kSrcOver

  // isolation: whether this element acts as a blend-mode isolation group.
  //   style.isolation = "isolate";   // children blend against this layer only
  //   style.isolation = "auto";      // default — no isolation
  // Equivalent to CSS `isolation: isolate`.
  std::string isolation;             // "" / "auto" = no isolation

  // ── CSS mask ────────────────────────────────────────────────────────────────
  // Clips the element's painted output using one or more mask layers.
  // mask-img syntax — comma-separated list of:
  //   linear-gradient(…)  radial-gradient(…)  conic-gradient(…)
  //   url("#elementId")          — DOM element alpha as mask
  //   url("file.svg")            — whole SVG file rasterised as mask
  //   url("file.svg#maskId")     — specific node inside an SVG file
  //   url("img.png")           — bitmap alpha channel as mask
  // Examples:
  //   style.mask = "linear-gradient(to bottom, black, transparent)";
  //   style.mask = "url(\"#myMaskEl\")";
  //   style.mask = "radial-gradient(circle, black 40%, transparent 70%)";
  std::string mask;                    // "" / "none" = no mask

  // mask-mode per layer (comma-separated for multi-layer):
  //   "alpha"        — alpha channel of mask img drives the mask (default)
  //   "luminance"    — BT.709 luminance of mask img drives the mask
  //   "match-source" — SVG sources use luminance; others use alpha
  std::string maskMode      = "alpha";

  // mask-position: CSS background-position syntax, per layer.
  //   "0% 0%"  "center"  "top left"  "50% 50%"  etc.
  std::string maskPosition  = "0% 0%";

  // mask-size: per layer.
  //   "auto"     — intrinsic img size (1:1 pixel mapping)
  //   "cover"    — scale to fill the mask box (no letterboxing)
  //   "contain"  — scale to fit within the mask box
  //   "W H"      — explicit width height (px or %)
  std::string maskSize      = "auto";

  // mask-repeat: per layer.  CSS default for mask is "no-repeat".
  //   "no-repeat" | "repeat" | "repeat-x" | "repeat-y"
  std::string maskRepeat    = "no-repeat";

  // mask-origin: coordinate box for mask-position / mask-size resolution.
  //   "border-box" | "padding-box" | "content-box"
  std::string maskOrigin    = "border-box";

  // mask-clip: area the mask painting is clipped to.
  //   "border-box" | "padding-box" | "content-box" | "no-clip"
  std::string maskClip      = "border-box";

  // mask-composite: how successive mask layers are composited together.
  //   "add" | "subtract" | "intersect" | "exclude"
  std::string maskComposite = "add";

  // CSS transition — parsed by tickTransitions() inside glint_element each frame.
  // Format: "property duration easing, ..."  e.g.  "background-color 300ms ease-out"
  // Accepts CSS kebab-case and camelCase property names. See glint_animator.hpp.
  std::string transition = "";

  // CSS animation — parsed by tickAnimations() inside glint_element each frame.
  // Drives @keyframes animations declared in the active stylesheet.
  // Format (CSS shorthand): "name duration easing iteration-count direction fill-mode delay"
  //   e.g.  "spin 1s linear infinite"
  //         "fade 300ms ease-out 1 normal both"
  // Multiple animations: comma-separated, e.g. "spin 1s linear infinite, fade 300ms ease-out"
  std::string animation = "";

  // ── Factory helpers ──────────────────────────────────────────────────────────

  /** Fully transparent panel (no background, no border). */
  static glint_style Transparent()
  {
    glint_style s;
    s.backgroundColor = "#00000000";
    return s;
  }

  /** Solid filled panel, optional rounded corners. */
  static glint_style Filled(sk_color bg, float radius = 0.f)
  {
    glint_style s;
    s.backgroundColor = bg;
    s.borderRadius    = radius;
    return s;
  }

  /** Filled panel with a border stroke. */
  static glint_style Outlined(sk_color bg, sk_color border,
                               float strokeWidth = 1.f, float radius = 0.f)
  {
    glint_style s;
    s.backgroundColor = bg;
    s.borderColor     = border;
    s.borderWidth     = strokeWidth;
    s.borderRadius    = radius;
    return s;
  }

  /** Outlined panel with a drop shadow. */
  static glint_style Card(sk_color bg, sk_color border, float radius = 8.f)
  {
    glint_style s   = Outlined(bg, border, 1.f, radius);
    s.shadowEnabled = true;
    return s;
  }

  // ── ApplyAlign ──────────────────────────────────────────────────────────────
  // Translates a space-separated `align` string (CSS-style shorthand tokens)
  // directly into the appropriate style properties on `style`.
  //
  // Token → axis (direction-independent):
  //   center      horizontal centering   → justifyContent (row) | alignItems (column)
  //   middle      vertical centering     → alignItems (row)     | justifyContent (column)
  //   left        horizontal flex-start  → justifyContent (row) | alignItems (column)
  //   right       horizontal flex-end    → justifyContent (row) | alignItems (column)
  //   top         vertical flex-start    → alignItems (row)     | justifyContent (column)
  //   bottom      vertical flex-end      → alignItems (row)     | justifyContent (column)
  //   ttb         flex-direction: column → flexDirection  / display=flex
  //   btt         flex-direction: column-reverse          / display=flex
  //   fill        flex: 1               → flexGrow = 1
  //   fullwidth   width: 100%           → width
  //   fullheight  height: 100%          → height
  //
  // center/middle are always horizontal/vertical regardless of ttb being set.
  // A two-pass scan detects direction first, then applies tokens accordingly.
  // Multiple tokens are space-separated and applied left-to-right, e.g.:
  //   glint_style::ApplyAlign("left middle", style);
  static void ApplyAlign(const std::string& align, glint_style& style)
  {
    if (align.empty()) return;

    // Pass 1: detect column direction
    bool isColumn = false;
    {
      std::istringstream ss(align);
      std::string tok;
      while (ss >> tok)
        if (tok == "ttb" || tok == "btt") { isColumn = true; break; }
    }

    // Pass 2: apply tokens with direction-aware center/middle/left/right/top/bottom
    std::istringstream ss(align);
    std::string token;
    while (ss >> token)
    {
      if (token == "ttb")        { style.display = "flex"; style.flexDirection = "column"; }
      else if (token == "btt")   { style.display = "flex"; style.flexDirection = "column-reverse"; }
      // Horizontal tokens — map to cross-axis (alignItems) in column, main-axis (justifyContent) in row
      else if (token == "left")  { style.display = "flex"; if (isColumn) style.alignItems = "flex-start"; else style.justifyContent = "flex-start"; }
      else if (token == "center"){ style.display = "flex"; if (isColumn) style.alignItems = "center";     else style.justifyContent = "center"; }
      else if (token == "right") { style.display = "flex"; if (isColumn) style.alignItems = "flex-end";   else style.justifyContent = "flex-end"; }
      // Vertical tokens — map to main-axis (justifyContent) in column, cross-axis (alignItems) in row
      else if (token == "top")   { style.display = "flex"; if (isColumn) style.justifyContent = "flex-start"; else style.alignItems = "flex-start"; }
      else if (token == "middle"){ style.display = "flex"; if (isColumn) style.justifyContent = "center";     else style.alignItems = "center"; }
      else if (token == "bottom"){ style.display = "flex"; if (isColumn) style.justifyContent = "flex-end";   else style.alignItems = "flex-end"; }
      else if (token == "fill")       { style.flexGrow = 1.f; }
      else if (token == "fullwidth")  { style.width  = "100%"; }
      else if (token == "fullheight") { style.height = "100%"; }
    }
  }
};

// ── glint_style ──────────────────────────────────────────────────────────────────
// New API name for glint_style.
