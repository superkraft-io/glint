#pragma once

/**
 * glint_filter.hpp
 * CSS-compatible filter API for glint components. (Skia backend only.)
 *
 * Maps the CSS `filter` property syntax to Skia img / colour-matrix filters
 * and wraps draw calls in a saveLayer / restore pair.
 *
 * Supported functions (chainable, space-separated):
 *   blur(<px>)                  Gaussian blur (sigma = px value)
 *   brightness(<0..2>)          Scale RGB (1=normal, 0=black, 2=double)
 *   contrast(<0..2>)            Contrast around mid-grey (1=normal)
 *   saturate(<0..2>)            Saturation (0=grey, 1=normal, >1=boosted)
 *   grayscale(<0..1>)           Greyscale blend (0=off, 1=full grey)
 *   sepia(<0..1>)               Sepia tone blend
 *   invert(<0..1>)              Colour inversion blend
 *   opacity(<0..1>)             Alpha scale (1=opaque, 0=transparent)
 *   hue-rotate(<deg>)           Hue rotation in degrees
 *   drop-shadow(<dx> <dy> <blur> <#color>)   Drop shadow
 *
 * Values ending in '%' are divided by 100 (e.g. "grayscale(100%)" = 1.0).
 *
 * Usage in glint_style:
 *   _c.style.filter = "blur(4px)";
 *   _c.style.filter = "saturate(0) brightness(0.8)";
 *   _c.style.filter = "drop-shadow(2px 4px 6px #000000)";
 *
 * Usage directly (e.g. inside drawContent):
 *   glint_filter::BeginLayer(g, mRect, "blur(8px)");
 *   // ... draw calls ...
 *   glint_filter::EndLayer(g);
 */


#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkColorMatrix.h"

#include "../glint_style.hpp"   // for sk_color
#include "../glint_graphics.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace glint_graphics;

namespace glint_filter {

// ── Internal helpers ──────────────────────────────────────────────────────────

inline void ClipBackdropShape(SkCanvas* canvas, const glint_rect& bounds, const glint_style& style)
{
  if (!canvas) return;

  const auto radii = style.resolveCornerRadii(bounds.W(), bounds.H());
  if (radii[0] > 0.f || radii[1] > 0.f || radii[2] > 0.f || radii[3] > 0.f)
  {
    const SkVector corners[4] = {
      { radii[0], radii[0] },
      { radii[1], radii[1] },
      { radii[2], radii[2] },
      { radii[3], radii[3] },
    };
    SkRRect rr;
    rr.setRectRadii(SkRect::MakeLTRB(bounds.L, bounds.T, bounds.R, bounds.B), corners);
    canvas->clipRRect(rr, SkClipOp::kIntersect, true);
    return;
  }

  canvas->clipRect(SkRect::MakeLTRB(bounds.L, bounds.T, bounds.R, bounds.B));
}

/** Parse a numeric argument from a filter token ("4px" → 4.0, "50%" → 0.5). */
inline bool TryParseNum(const std::string& s, float* out)
{
  if (!out) return false;
  *out = 0.f;
  if (s.empty()) return false;

  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || !std::isfinite(v)) return false;

  *out = (s.find('%') != std::string::npos) ? (v / 100.f) : v;
  return true;
}

inline float ParseNum(const std::string& s)
{
  float v = 0.f;
  TryParseNum(s, &v);
  return v;
}

/** Convert a CSS colour string to SkColor, delegating to sk_color. */
inline SkColor ParseSkColor(const std::string& s)
{
  if (s.empty()) return SK_ColorBLACK;
  sk_color c(s.c_str());
  return SkColorSetARGB(c.value.A, c.value.R, c.value.G, c.value.B);
}

/**
 * Tokenise a CSS filter string into individual function calls.
 * "blur(4px) saturate(0.5)" → { "blur(4px)", "saturate(0.5)" }
 */
inline std::vector<std::string> Tokenize(const std::string& str)
{
  std::vector<std::string> out;
  size_t i = 0;
  while (i < str.size())
  {
    while (i < str.size() && std::isspace((unsigned char)str[i])) ++i;
    if (i >= str.size()) break;

    const size_t start = i;
    while (i < str.size() && str[i] != '(' && !std::isspace((unsigned char)str[i])) ++i;
    if (i >= str.size() || str[i] != '(')
    {
      while (i < str.size() && !std::isspace((unsigned char)str[i])) ++i;
      continue;
    }

    int depth = 0;
    while (i < str.size())
    {
      if (str[i] == '(') ++depth;
      else if (str[i] == ')')
      {
        --depth;
        if (depth == 0)
        {
          ++i;
          break;
        }
      }
      ++i;
    }

    if (depth != 0) break;
    out.push_back(str.substr(start, i - start));
  }
  return out;
}

inline std::vector<std::string> SplitArgs(const std::string& str)
{
  std::vector<std::string> out;
  size_t i = 0;
  while (i < str.size())
  {
    while (i < str.size() && std::isspace((unsigned char)str[i])) ++i;
    if (i >= str.size()) break;

    const size_t start = i;
    int depth = 0;
    while (i < str.size())
    {
      if (str[i] == '(') ++depth;
      else if (str[i] == ')' && depth > 0) --depth;
      else if (std::isspace((unsigned char)str[i]) && depth == 0) break;
      ++i;
    }

    out.push_back(str.substr(start, i - start));
  }
  return out;
}

inline bool ParseDropShadowArgs(const std::string& arg, float* dxOut, float* dyOut,
                                float* blurOut, SkColor* colorOut = nullptr)
{
  const auto args = SplitArgs(arg);
  if (args.size() < 3) return false;

  float dx = 0.f;
  float dy = 0.f;
  float blur = 0.f;
  if (!TryParseNum(args[0], &dx) || !TryParseNum(args[1], &dy) || !TryParseNum(args[2], &blur))
    return false;

  if (dxOut) *dxOut = dx;
  if (dyOut) *dyOut = dy;
  if (blurOut) *blurOut = blur;
  if (colorOut)
  {
    if (args.size() <= 3)
    {
      *colorOut = SK_ColorBLACK;
    }
    else
    {
      std::string color = args[3];
      for (size_t index = 4; index < args.size(); ++index)
      {
        color += ' ';
        color += args[index];
      }
      *colorOut = ParseSkColor(color);
    }
  }

  return true;
}

/**
 * Split "blur(4px)" → name = "blur", arg = "4px".
 * Also lowercases the function name.
 */
inline bool SplitFn(const std::string& token, std::string& name, std::string& arg)
{
  auto p = token.find('(');
  if (p == std::string::npos) return false;
  name = token.substr(0, p);
  while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
  for (char& c : name) c = static_cast<char>(std::tolower((unsigned char)c));
  auto q = token.find(')', p);
  arg = (q != std::string::npos) ? token.substr(p + 1, q - p - 1)
                                  : token.substr(p + 1);
  return true;
}

inline bool ParseSingleBlur(const std::string& filterStr, float* sigmaOut = nullptr)
{
  auto tokens = Tokenize(filterStr);
  if (tokens.size() != 1) return false;

  std::string name, arg;
  if (!SplitFn(tokens[0], name, arg) || name != "blur") return false;

  float sigma = 0.f;
  if (!TryParseNum(arg, &sigma)) return false;

  if (sigmaOut) *sigmaOut = sigma;
  return true;
}

// ── Color-matrix filter builders ──────────────────────────────────────────────
// Each function post-concatenates its effect onto an existing SkColorMatrix.

inline void CMBrightness(SkColorMatrix& m, float f)
{
  SkColorMatrix b;
  b.setScale(f, f, f, 1.f);
  m.postConcat(b);
}

inline void CMContrast(SkColorMatrix& m, float f)
{
  float t = (1.f - f) * 0.5f;
  const float v[20] = {
    f, 0, 0, 0, t,
    0, f, 0, 0, t,
    0, 0, f, 0, t,
    0, 0, 0, 1, 0
  };
  SkColorMatrix c;
  c.setRowMajor(v);
  m.postConcat(c);
}

inline void CMSaturate(SkColorMatrix& m, float s)
{
  SkColorMatrix sat;
  sat.setSaturation(s);
  m.postConcat(sat);
}

/** Blend between identity and full grayscale (amount 0=off, 1=full grey). */
inline void CMGrayscale(SkColorMatrix& m, float amount)
{
  float ia = 1.f - amount;
  const float v[20] = {
    0.2126f + 0.7874f * ia,  0.7152f - 0.7152f * ia,  0.0722f - 0.0722f * ia,  0, 0,
    0.2126f - 0.2126f * ia,  0.7152f + 0.2848f * ia,  0.0722f - 0.0722f * ia,  0, 0,
    0.2126f - 0.2126f * ia,  0.7152f - 0.7152f * ia,  0.0722f + 0.9278f * ia,  0, 0,
    0, 0, 0, 1, 0
  };
  SkColorMatrix g;
  g.setRowMajor(v);
  m.postConcat(g);
}

/** Blend between identity and full sepia tone (amount 0=off, 1=full sepia). */
inline void CMSepia(SkColorMatrix& m, float amount)
{
  float ia = 1.f - amount;
  const float v[20] = {
    0.393f + 0.607f * ia,  0.769f - 0.769f * ia,  0.189f - 0.189f * ia,  0, 0,
    0.349f - 0.349f * ia,  0.686f + 0.314f * ia,  0.168f - 0.168f * ia,  0, 0,
    0.272f - 0.272f * ia,  0.534f - 0.534f * ia,  0.131f + 0.869f * ia,  0, 0,
    0, 0, 0, 1, 0
  };
  SkColorMatrix s;
  s.setRowMajor(v);
  m.postConcat(s);
}

/** Blend between identity and full inversion (amount 0=off, 1=fully inverted). */
inline void CMInvert(SkColorMatrix& m, float amount)
{
  float f = amount;
  const float v[20] = {
    1.f - 2.f * f, 0,            0,            0, f,
    0,             1.f - 2.f*f,  0,            0, f,
    0,             0,            1.f - 2.f*f,  0, f,
    0,             0,            0,            1, 0
  };
  SkColorMatrix inv;
  inv.setRowMajor(v);
  m.postConcat(inv);
}

inline void CMOpacity(SkColorMatrix& m, float amount)
{
  SkColorMatrix o;
  o.setScale(1.f, 1.f, 1.f, amount);
  m.postConcat(o);
}

/**
 * Standard CSS hue-rotate matrix in linear RGB.
 * Matches the W3C filter-effects specification.
 */
inline void CMHueRotate(SkColorMatrix& m, float degrees)
{
  constexpr float kPi = 3.14159265358979f;
  float rad = degrees * kPi / 180.f;
  float c   = std::cos(rad);
  float s   = std::sin(rad);
  const float v[20] = {
    0.213f + c * 0.787f - s * 0.213f,  0.715f - c * 0.715f - s * 0.715f,  0.072f - c * 0.072f + s * 0.928f,  0, 0,
    0.213f - c * 0.213f + s * 0.143f,  0.715f + c * 0.285f + s * 0.140f,  0.072f - c * 0.072f - s * 0.283f,  0, 0,
    0.213f - c * 0.213f - s * 0.787f,  0.715f - c * 0.715f + s * 0.715f,  0.072f + c * 0.928f + s * 0.072f,  0, 0,
    0, 0, 0, 1, 0
  };
  SkColorMatrix h;
  h.setRowMajor(v);
  m.postConcat(h);
}

// ── Main builder ──────────────────────────────────────────────────────────────

/**
 * Parse a CSS filter string and return a composed SkImageFilter.
 * Returns nullptr if the string is empty or "none".
 *
 * Color-matrix operations (brightness, contrast, saturate, grayscale,
 * sepia, invert, opacity, hue-rotate) are all composed into a single
 * SkColorMatrix pass for efficiency, then wrapped as an img filter.
 *
 * Blur and drop-shadow are chained as separate img filters.
 */
inline sk_sp<SkImageFilter> Build(const std::string& filterStr)
{
  if (filterStr.empty() || filterStr == "none") return nullptr;

  auto tokens = Tokenize(filterStr);
  if (tokens.empty()) return nullptr;

  SkColorMatrix cm;
  cm.setIdentity();
  bool hasCM = false;

  sk_sp<SkImageFilter> composed = nullptr;

  for (auto& token : tokens)
  {
    std::string name, arg;
    if (!SplitFn(token, name, arg)) continue;

    if (name == "blur")
    {
      float sigma = 0.f;
      if (!TryParseNum(arg, &sigma)) continue;
      composed = SkImageFilters::Blur(sigma, sigma, std::move(composed));
    }
    else if (name == "drop-shadow")
    {
      float dx = 0.f, dy = 0.f, blur = 0.f;
      SkColor color = SK_ColorBLACK;
      if (!ParseDropShadowArgs(arg, &dx, &dy, &blur, &color)) continue;
      composed = SkImageFilters::DropShadow(dx, dy, blur, blur, color, std::move(composed));
    }
    else
    {
      float value = 0.f;
      if (!TryParseNum(arg, &value)) continue;

      if (name == "brightness")      { CMBrightness(cm, value); hasCM = true; }
      else if (name == "contrast")   { CMContrast  (cm, value); hasCM = true; }
      else if (name == "saturate")   { CMSaturate  (cm, value); hasCM = true; }
      else if (name == "grayscale")  { CMGrayscale (cm, value); hasCM = true; }
      else if (name == "sepia")      { CMSepia     (cm, value); hasCM = true; }
      else if (name == "invert")     { CMInvert    (cm, value); hasCM = true; }
      else if (name == "opacity")    { CMOpacity   (cm, value); hasCM = true; }
      else if (name == "hue-rotate") { CMHueRotate (cm, value); hasCM = true; }
    }
  }

  // Wrap accumulated color-matrix operations as an img filter, chained
  // with any img filters (blur / drop-shadow) collected above.
  if (hasCM)
  {
    auto cf = SkColorFilters::Matrix(cm);
    composed = SkImageFilters::ColorFilter(std::move(cf), std::move(composed));
  }

  return composed;
}

// ── Layer helpers ─────────────────────────────────────────────────────────────

/**
 * Compute the maximum outward pixel expansion required by a filter string so
 * that blur and drop-shadow effects are not clipped at the component edge.
 *
 * Rules:
 *   blur(sigma)                 → expand by sigma * 3  (covers 99.7% of Gaussian)
 *   drop-shadow(dx dy blur …)   → expand by |dx|+blur*3 / |dy|+blur*3 per axis
 *   All color-matrix filters    → 0 (never extend outside the bounds)
 */
inline float ComputeExpansion(const std::string& filterStr)
{
  if (filterStr.empty() || filterStr == "none") return 0.f;
  float expand = 0.f;
  for (auto& token : Tokenize(filterStr))
  {
    std::string name, arg;
    if (!SplitFn(token, name, arg)) continue;

    if (name == "blur")
    {
      float sigma = 0.f;
      if (TryParseNum(arg, &sigma))
        expand = std::max(expand, sigma * 3.f);
    }
    else if (name == "drop-shadow")
    {
      float dx = 0.f, dy = 0.f, blur = 0.f;
      if (!ParseDropShadowArgs(arg, &dx, &dy, &blur)) continue;
      expand = std::max(expand, std::abs(dx) + blur * 3.f);
      expand = std::max(expand, std::abs(dy) + blur * 3.f);
    }
  }
  return expand;
}

/**
 * Open a filtered saveLayer around a component's rect.
 *
 * glint_element::EnsureFilterPad() physically inflates mRect by
 * ComputeExpansion() on the first Draw() call so the host redraw region and
 * clip rect include the full filtered bounds.
 * The saveLayer bitmap is also inflated here so blur / shadow spread has
 * room inside the layer and composites without clipping.
 *
 * Must be followed by a paired EndLayer() call.
 */
inline void BeginLayer(glint_canvas& g, const glint_rect& bounds, const std::string& filterStr)
{
  if (filterStr.empty() || filterStr == "none") return;

  auto filter = Build(filterStr);
  if (!filter) return;

  SkCanvas* canvas = static_cast<SkCanvas*>(g.GetDrawContext());
  if (!canvas) return;

  SkPaint paint;
  paint.setImageFilter(std::move(filter));
  canvas->saveLayer(nullptr, &paint);
}

/**
 * Close an open filter layer opened by BeginLayer().
 * Safe to call unconditionally — is a no-op if GetDrawContext() is null.
 */
inline void EndLayer(glint_canvas& g)
{
  SkCanvas* canvas = static_cast<SkCanvas*>(g.GetDrawContext());
  if (canvas) canvas->restore();
}

/**
 * SkCanvas* overloads for the standalone-window DrawToCanvas path.
 * Identical logic to the glint_canvas overloads but accept a raw SkCanvas ptr.
 */
inline void BeginLayer(SkCanvas* canvas, const glint_rect& bounds, const std::string& filterStr)
{
  if (!canvas || filterStr.empty() || filterStr == "none") return;
  auto filter = Build(filterStr);
  if (!filter) return;
  SkPaint paint;
  paint.setImageFilter(std::move(filter));
  canvas->saveLayer(nullptr, &paint);
}

inline void EndLayer(SkCanvas* canvas)
{
  if (canvas) canvas->restore();
}

// ── Backdrop-filter layer helpers ─────────────────────────────────────────────
//
// CSS backdrop-filter: applies a filter to the area *behind* the element.
// This uses Skia's SaveLayerRec with the fBackdrop field, which initialises
// the new layer's backdrop from the filtered current canvas content.
// Our element's content is then drawn on top of the filtered backdrop.
//
// Implementation note: a blur (or any expanding filter) used as fBackdrop reads
// pixels BEYOND fBounds for its kernel, and Skia composites those extra pixels
// outside the element bounds on restore — making siblings/parents appear affected.
// To match browser behaviour (backdrop-filter is strictly clipped to the element
// painted shape) we: (1) save, (2) clip to the resolved border-radius shape,
// (3) open saveLayer.
// EndBackdropLayer therefore issues two restores: layer first, then clip.
//
// Usage:
//   glint_filter::BeginBackdropLayer(canvas, mPaintRECT, computedStyle, computedStyle.backdropFilter);
//   // ... draw element background + children ...
//   glint_filter::EndBackdropLayer(canvas);

inline void BeginBackdropLayer(SkCanvas* canvas, const glint_rect& bounds, const glint_style& style, const std::string& filterStr)
{
  if (!canvas || filterStr.empty() || filterStr == "none") return;
  auto filter = Build(filterStr);
  if (!filter) return;
  SkRect rect = SkRect::MakeLTRB(bounds.L, bounds.T, bounds.R, bounds.B);
  // Clip first so the layer compositing on restore cannot bleed outside the
  // element's resolved rounded shape.
  canvas->save();
  ClipBackdropShape(canvas, bounds, style);
  // SaveLayerRec with fBackdrop applies the filter to the existing canvas pixels
  // within the clip (= element shape) — the CSS backdrop-filter model.
  SkCanvas::SaveLayerRec rec(&rect, nullptr, filter.get(), 0);
  canvas->saveLayer(rec);
}

inline void EndBackdropLayer(SkCanvas* canvas)
{
  if (!canvas) return;
  canvas->restore();  // restore the saveLayer (composites within clip)
  canvas->restore();  // restore the clip
}

inline void BeginBackdropLayer(glint_canvas& g, const glint_rect& bounds, const glint_style& style, const std::string& filterStr)
{
  BeginBackdropLayer(static_cast<SkCanvas*>(g.GetDrawContext()), bounds, style, filterStr);
}

inline void EndBackdropLayer(glint_canvas& g)
{
  EndBackdropLayer(static_cast<SkCanvas*>(g.GetDrawContext()));
}

} // namespace glint_filter

