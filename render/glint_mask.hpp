#pragma once

/**
 * glint_mask.hpp
 * CSS `mask` property — layer parsing and Skia shader/img building.
 *
 * This header is pure Skia + glint_style.  It does NOT include glint_element or
 * glint_document.  The rendering glue that requires glint_element* (url(#id),
 * url(file.svg), url(img)) is handled in glint_element_render.hpp which has
 * full access to the element and document types.
 *
 * Supported mask-img sources (detected by glint_parse_mask_layers):
 *   linear-gradient(...)          → GRADIENT
 *   radial-gradient(...)          → GRADIENT
 *   conic-gradient(...)           → GRADIENT
 *   url("#elementId")             → URL_ELEMENT_ID
 *   url("file.svg")               → URL_SVG_FILE
 *   url("file.svg#maskId")        → URL_SVG_FILE_ID
 *   url("img.png/.jpg/.webp")   → URL_IMAGE
 *   none                          → NONE (skipped)
 *
 * Per-layer CSS sub-properties resolved from the glint_style comma-lists:
 *   mask-mode          "alpha" | "luminance" | "match-source"
 *   mask-position      "0% 0%" | "center" | "top left" etc.
 *   mask-size          "auto" | "cover" | "contain" | "W H"
 *   mask-repeat        "no-repeat" | "repeat" | "repeat-x" | "repeat-y"
 *   mask-origin        "border-box" | "padding-box" | "content-box"
 *   mask-clip          "border-box" | "padding-box" | "content-box" | "no-clip"
 *   mask-composite     "add" | "subtract" | "intersect" | "exclude"
 */

#include "../glint_style.hpp"
#include "glint_resource_cache.hpp"
#include "glint_svg_cache.hpp"

#include "glint_resource_request.hpp"
#include "../utils/glint_network_log.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkColorFilter.h"
#include "include/effects/SkColorMatrix.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "modules/svg/include/SkSVGDOM.h"
#include "modules/svg/include/SkSVGRenderContext.h"
#include "modules/svg/include/SkSVGSVG.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ── glint_mask_layer ─────────────────────────────────────────────────────────

/** One parsed mask-img layer plus its resolved per-layer sub-properties. */
struct glint_mask_layer
{
  enum Type {
    NONE,
    GRADIENT,          // linear-gradient / radial-gradient / conic-gradient
    URL_ELEMENT_ID,    // url("#domId")
    URL_SVG_FILE,      // url("file.svg")          — whole SVG
    URL_SVG_FILE_ID,   // url("file.svg#maskId")   — specific node in an SVG file
    URL_IMAGE,         // url("img.png") / .jpg / .webp etc.
  };

  Type        type         = NONE;

  // For GRADIENT: the full gradient function string
  std::string gradientStr;

  // For URL_* types: the path or element id
  std::string urlTarget;   // element id, svg path, or img path
  std::string urlFragId;   // fragment id for URL_SVG_FILE_ID ("maskId" part)

  // Per-layer sub-properties (resolved from comma-lists in the glint_style fields)
  std::string mode      = "alpha";        // "alpha" | "luminance"
  std::string position  = "0% 0%";        // CSS mask-position
  std::string size      = "auto";         // "auto" | "cover" | "contain" | "W H"
  std::string repeat    = "no-repeat";    // "no-repeat" | "repeat" | "repeat-x" | "repeat-y"
  std::string origin    = "border-box";   // box for position/size resolution
  std::string clip      = "border-box";   // box for paint clipping
  std::string composite = "add";          // "add" | "subtract" | "intersect" | "exclude"
};

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace glint_mask_detail {

/** Split a CSS list by commas, respecting function-call parenthesis depth. */
inline std::vector<std::string> splitByComma(const std::string& src)
{
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : src) {
    if (c == '(') ++depth;
    else if (c == ')') --depth;
    if (c == ',' && depth == 0) {
      // Trim whitespace
      size_t b = cur.find_first_not_of(" \t\r\n");
      size_t e = cur.find_last_not_of(" \t\r\n");
      if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
      else                        out.push_back("");
      cur.clear();
    } else {
      cur += c;
    }
  }
  {
    size_t b = cur.find_first_not_of(" \t\r\n");
    size_t e = cur.find_last_not_of(" \t\r\n");
    if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
    else if (!cur.empty())      out.push_back("");
  }
  return out;
}

/** Trim whitespace from both ends. */
inline std::string trim(const std::string& s)
{
  size_t b = s.find_first_not_of(" \t\r\n\"'");
  size_t e = s.find_last_not_of(" \t\r\n\"'");
  if (b == std::string::npos) return {};
  return s.substr(b, e - b + 1);
}

/** Case-insensitive startsWith. */
inline bool startsWith(const std::string& s, const char* prefix)
{
  size_t n = std::strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  return true;
}

/** Parse url("...") → returns the inner string (strips url( quotes )). */
inline std::string parseUrl(const std::string& tok)
{
  // Expects: url("...") or url('...') or url(...)
  size_t open = tok.find('(');
  size_t close = tok.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return {};
  return trim(tok.substr(open + 1, close - open - 1));
}

/** Nth value from a comma-separated list; wraps if idx >= count. */
inline std::string nthOrLast(const std::vector<std::string>& list, size_t idx)
{
  if (list.empty()) return {};
  return trim(list[idx < list.size() ? idx : list.size() - 1]);
}

} // namespace glint_mask_detail

// ── glint_parse_mask_layers ───────────────────────────────────────────────────

/**
 * Parse style.mask (and all mask-* sub-property strings) into a vector of
 * resolved glint_mask_layer structs, one per mask-img layer.
 *
 * The `mask` field holds the comma-separated mask-img list.
 * All other mask-* fields may also be comma-lists (one entry per layer),
 * otherwise the single value applies to all layers (CSS spec §mask-img).
 */
inline std::vector<glint_mask_layer> glint_parse_mask_layers(const glint_style& s)
{
  using namespace glint_mask_detail;

  if (s.mask.empty() || s.mask == "none") return {};

  const auto imgTokens  = splitByComma(s.mask);
  const auto modeList   = splitByComma(s.maskMode);
  const auto posList    = splitByComma(s.maskPosition);
  const auto sizeList   = splitByComma(s.maskSize);
  const auto repList    = splitByComma(s.maskRepeat);
  const auto origList   = splitByComma(s.maskOrigin);
  const auto clipList   = splitByComma(s.maskClip);
  const auto compList   = splitByComma(s.maskComposite);

  std::vector<glint_mask_layer> layers;
  layers.reserve(imgTokens.size());

  for (size_t i = 0; i < imgTokens.size(); ++i)
  {
    const std::string& tok = imgTokens[i];
    if (tok.empty() || tok == "none") continue;

    glint_mask_layer layer;

    // ── Detect img type ────────────────────────────────────────────────────
    if (startsWith(tok, "linear-gradient(") ||
        startsWith(tok, "radial-gradient(")  ||
        startsWith(tok, "conic-gradient("))
    {
      layer.type        = glint_mask_layer::GRADIENT;
      layer.gradientStr = tok;
    }
    else if (startsWith(tok, "url("))
    {
      const std::string inner = parseUrl(tok);
      if (!inner.empty() && inner[0] == '#')
      {
        // url("#elementId") — DOM element reference
        layer.type      = glint_mask_layer::URL_ELEMENT_ID;
        layer.urlTarget = inner.substr(1);  // strip '#'
      }
      else
      {
        // Check for SVG fragment: "file.svg#maskId"
        const size_t hash = inner.rfind('#');
        // Detect if the file is SVG (before or without fragment)
        const std::string pathOnly = (hash != std::string::npos) ? inner.substr(0, hash) : inner;
        const bool isSVG = pathOnly.size() > 4 &&
                           (pathOnly.substr(pathOnly.size() - 4) == ".svg" ||
                            pathOnly.substr(pathOnly.size() - 4) == ".SVG");
        if (isSVG && hash != std::string::npos)
        {
          layer.type      = glint_mask_layer::URL_SVG_FILE_ID;
          layer.urlTarget = pathOnly;
          layer.urlFragId = inner.substr(hash + 1);
        }
        else if (isSVG)
        {
          layer.type      = glint_mask_layer::URL_SVG_FILE;
          layer.urlTarget = pathOnly;
        }
        else
        {
          layer.type      = glint_mask_layer::URL_IMAGE;
          layer.urlTarget = inner;
        }
      }
    }
    else
    {
      continue;  // unrecognised token (e.g. "none")
    }

    // ── Resolve per-layer sub-properties ────────────────────────────────────
    layer.mode      = nthOrLast(modeList,  i);  if (layer.mode.empty())      layer.mode      = "alpha";
    layer.position  = nthOrLast(posList,   i);  if (layer.position.empty())  layer.position  = "0% 0%";
    layer.size      = nthOrLast(sizeList,  i);  if (layer.size.empty())      layer.size      = "auto";
    layer.repeat    = nthOrLast(repList,   i);  if (layer.repeat.empty())    layer.repeat    = "no-repeat";
    layer.origin    = nthOrLast(origList,  i);  if (layer.origin.empty())    layer.origin    = "border-box";
    layer.clip      = nthOrLast(clipList,  i);  if (layer.clip.empty())      layer.clip      = "border-box";
    layer.composite = nthOrLast(compList,  i);  if (layer.composite.empty()) layer.composite = "add";

    // "match-source" → "alpha" for non-SVG sources (SVG sources → "luminance" per spec,
    // but we default to "alpha" for simplicity since our DOM elements use alpha naturally)
    if (layer.mode == "match-source")
      layer.mode = (layer.type == glint_mask_layer::URL_SVG_FILE ||
                    layer.type == glint_mask_layer::URL_SVG_FILE_ID) ? "luminance" : "alpha";

    layers.push_back(std::move(layer));
  }

  return layers;
}


// ── Gradient shader builder ──────────────────────────────────────────────────

/**
 * Build an SkShader from a CSS gradient function string and a bounds rect.
 * Uses the existing sk_background_shorthand parser by routing through a
 * temporary glint_style, then replicating the _drawBackgroundSkia shader logic.
 *
 * Returns nullptr if the string cannot be parsed as a valid gradient.
 */
inline sk_sp<SkShader> glint_mask_gradient_shader(const glint_mask_layer& layer,
                                                   const glint_rect& bounds)
{
  if (layer.gradientStr.empty()) return nullptr;

  // Parse via the existing background shorthand (populates gradient* fields).
  glint_style tmp;
  tmp.background = layer.gradientStr.c_str();

  if (tmp.backgroundGradient.size() < 2) return nullptr;

  // Sort stops by position.
  std::vector<sk_gradient_stop> sorted = tmp.backgroundGradient;
  std::stable_sort(sorted.begin(), sorted.end());

  std::vector<SkColor>  colors;
  std::vector<SkScalar> positions;
  colors.reserve(sorted.size());
  positions.reserve(sorted.size());
  for (const auto& st : sorted) {
    colors.push_back(SkColorSetARGB(st.color.A, st.color.R,
                                    st.color.G, st.color.B));
    positions.push_back(static_cast<SkScalar>(st.position));
  }

  const float rW = bounds.W(), rH = bounds.H();
  sk_sp<SkShader> shader;
  const std::string& gtype = tmp.backgroundGradientType;

  if (gtype == "radial") {
    const float cx = bounds.L + rW * tmp.backgroundGradientCX;
    const float cy = bounds.T + rH * tmp.backgroundGradientCY;
    const float r  = std::min(rW, rH) * 0.5f * tmp.backgroundGradientRadius;
    shader = SkGradientShader::MakeRadial(
        {cx, cy}, std::max(r, 1.f),
        colors.data(), positions.data(),
        static_cast<int>(colors.size()), SkTileMode::kClamp);
  }
  else if (gtype == "conic") {
    const float cx       = bounds.L + rW * tmp.backgroundGradientCX;
    const float cy       = bounds.T + rH * tmp.backgroundGradientCY;
    const float startDeg = tmp.backgroundGradientAngle;
    SkMatrix localMatrix;
    localMatrix.setRotate(startDeg - 90.f, cx, cy);
    shader = SkGradientShader::MakeSweep(
        cx, cy,
        colors.data(), positions.data(),
        static_cast<int>(colors.size()),
        SkTileMode::kClamp,
      0.f, 360.f,
      0, &localMatrix);
  }
  else {
    // Linear gradient
    const float rad = tmp.backgroundGradientAngle * 3.14159265358979323846f / 180.f;
    const float cx  = bounds.L + rW * 0.5f;
    const float cy  = bounds.T + rH * 0.5f;
    const float dx  = std::sin(rad) * rW * 0.5f;
    const float dy  = -std::cos(rad) * rH * 0.5f;
    SkPoint pts[2]  = { {cx - dx, cy - dy}, {cx + dx, cy + dy} };
    shader = SkGradientShader::MakeLinear(
        pts, colors.data(), positions.data(),
        static_cast<int>(colors.size()), SkTileMode::kClamp);
  }

  return shader;
}

// ── Image shader builder (position / size / repeat) ──────────────────────────

/**
 * Resolve mask-position string into [x, y] fractions relative to bounds.
 * Supports: "0% 0%", "50% 50%", "center", "top", "left", "right", "bottom",
 *           as well as "left top", "center bottom", etc.
 */
inline void glint_mask_resolve_position(const std::string& pos,
                                        float& outFx, float& outFy)
{
  outFx = 0.f; outFy = 0.f;
  if (pos.empty()) return;

  auto parseOne = [](const std::string& tok, bool isX) -> float {
    if (tok == "left")   return 0.f;
    if (tok == "right")  return 1.f;
    if (tok == "top")    return 0.f;
    if (tok == "bottom") return 1.f;
    if (tok == "center") return 0.5f;
    if (!tok.empty() && tok.back() == '%') {
      try { return std::stof(tok) / 100.f; } catch (...) {}
    }
    // px — just use 0 normalised (caller converts later)
    return 0.f;
  };

  std::istringstream ss(pos);
  std::string t1, t2;
  ss >> t1;
  if (ss >> t2) {
    // CSS allows two-keyword positions in either order: "top right" == "right top".
    // If the first token is a vertical keyword (top/bottom), swap so that the
    // horizontal value always drives outFx and the vertical drives outFy.
    const bool t1IsVert = (t1 == "top" || t1 == "bottom");
    if (t1IsVert) {
      outFx = parseOne(t2, true);
      outFy = parseOne(t1, false);
    } else {
      outFx = parseOne(t1, true);
      outFy = parseOne(t2, false);
    }
  } else {
    // Single keyword
    if (t1 == "center") { outFx = 0.5f; outFy = 0.5f; }
    else if (t1 == "top")    { outFx = 0.5f; outFy = 0.f; }
    else if (t1 == "bottom") { outFx = 0.5f; outFy = 1.f; }
    else if (t1 == "left")   { outFx = 0.f;  outFy = 0.5f; }
    else if (t1 == "right")  { outFx = 1.f;  outFy = 0.5f; }
    else { outFx = parseOne(t1, true); outFy = 0.f; }
  }
}

/**
 * Precise CSS mask img shader builder.
 *
 * Unlike the generic helper below, this variant returns coverage information for
 * non-repeating axes and uses clamp sampling there, so callers can clip exactly
 * to the placed img rect and avoid the faint transparent fringe produced by
 * linear filtering against `kDecal` outside the img.
 */
inline sk_sp<SkShader> glint_mask_image_shader_precise(sk_sp<SkImage> img,
                                                       const glint_rect& bounds,
                                                       const glint_mask_layer& layer,
                                                       SkRect* outCoverageRect,
                                                       bool* outLimitX,
                                                       bool* outLimitY)
{
  if (outCoverageRect) *outCoverageRect = SkRect::MakeEmpty();
  if (outLimitX) *outLimitX = false;
  if (outLimitY) *outLimitY = false;
  if (!img) return nullptr;

  const float bW = bounds.W(), bH = bounds.H();
  const float iW = static_cast<float>(img->width());
  const float iH = static_cast<float>(img->height());
  if (iW <= 0.f || iH <= 0.f || bW <= 0.f || bH <= 0.f) return nullptr;

  float scaleX = 1.f, scaleY = 1.f;

  const std::string& sz = layer.size;
  if (sz == "cover") {
    const float scl = std::max(bW / iW, bH / iH);
    scaleX = scaleY = scl;
  }
  else if (sz == "contain") {
    const float scl = std::min(bW / iW, bH / iH);
    scaleX = scaleY = scl;
  }
  else if (sz == "auto") {
    scaleX = scaleY = 1.f;
  }
  else {
    std::istringstream ss(sz);
    std::string sw, sh;
    if (ss >> sw) {
      float fW = bW, fH = bH;
      try {
        if (!sw.empty() && sw.back() == '%')
          fW = std::stof(sw) * bW / 100.f;
        else
          fW = std::stof(sw);
      } catch (...) {}
      if (ss >> sh) {
        try {
          if (!sh.empty() && sh.back() == '%')
            fH = std::stof(sh) * bH / 100.f;
          else
            fH = std::stof(sh);
        } catch (...) {}
      }
      scaleX = (iW > 0.f) ? fW / iW : 1.f;
      scaleY = (iH > 0.f) ? fH / iH : 1.f;
    }
  }

  const float scaledW = iW * scaleX;
  const float scaledH = iH * scaleY;

  float fx = 0.f, fy = 0.f;
  glint_mask_resolve_position(layer.position, fx, fy);
  const float tx = bounds.L + fx * (bW - scaledW);
  const float ty = bounds.T + fy * (bH - scaledH);

  SkTileMode tmX = SkTileMode::kRepeat;
  SkTileMode tmY = SkTileMode::kRepeat;
  bool limitX = false;
  bool limitY = false;
  const std::string& rep = layer.repeat;
  if (rep == "repeat") {
    tmX = SkTileMode::kRepeat; tmY = SkTileMode::kRepeat;
  }
  else if (rep == "repeat-x") {
    tmX = SkTileMode::kRepeat; tmY = SkTileMode::kClamp; limitY = true;
  }
  else if (rep == "repeat-y") {
    tmX = SkTileMode::kClamp; limitX = true; tmY = SkTileMode::kRepeat;
  }
  else {
    tmX = SkTileMode::kClamp; tmY = SkTileMode::kClamp; limitX = true; limitY = true;
  }

  if (outCoverageRect)
    *outCoverageRect = SkRect::MakeLTRB(tx, ty, tx + scaledW, ty + scaledH);
  if (outLimitX) *outLimitX = limitX;
  if (outLimitY) *outLimitY = limitY;

  SkMatrix lm = SkMatrix::Scale(scaleX, scaleY);
  lm.postTranslate(tx, ty);
  if (!lm.isFinite()) {
    SkMatrix fill;
    fill.setRectToRect(SkRect::MakeWH(iW, iH),
                       SkRect::MakeLTRB(bounds.L, bounds.T, bounds.R, bounds.B),
                       SkMatrix::kFill_ScaleToFit);
    if (!fill.isFinite()) return nullptr;
    return img->makeShader(tmX, tmY, SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), &fill);
  }

  return img->makeShader(tmX, tmY, SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), &lm);
}

/**
 * Scale from an img's intrinsic pixels to CSS px for mask-size /
 * background-size on the given bounds. False when nothing would show.
 */
inline bool glint_mask_image_scale(SkISize intrinsic,
                                   const glint_rect& bounds,
                                   const glint_mask_layer& layer,
                                   float& scaleX, float& scaleY)
{
  const float bW = bounds.W(), bH = bounds.H();
  const float iW = static_cast<float>(intrinsic.width());
  const float iH = static_cast<float>(intrinsic.height());
  scaleX = scaleY = 1.f;
  if (iW <= 0.f || iH <= 0.f || bW <= 0.f || bH <= 0.f) return false;

  // ── mask-size ──────────────────────────────────────────────────────────────
  const std::string& sz = layer.size;
  if (sz == "cover") {
    const float scl = std::max(bW / iW, bH / iH);
    scaleX = scaleY = scl;
  }
  else if (sz == "contain") {
    const float scl = std::min(bW / iW, bH / iH);
    scaleX = scaleY = scl;
  }
  else if (sz == "auto") {
    // Intrinsic size — 1:1 pixel mapping
    scaleX = scaleY = 1.f;
  }
  else {
    // "W H" explicit sizes — parse as px or %
    std::istringstream ss(sz);
    std::string sw, sh;
    if (ss >> sw) {
      float fW = bW, fH = bH;
      try {
        if (!sw.empty() && sw.back() == '%')
          fW = std::stof(sw) * bW / 100.f;
        else
          fW = std::stof(sw);
      } catch (...) {}
      if (ss >> sh) {
        try {
          if (!sh.empty() && sh.back() == '%')
            fH = std::stof(sh) * bH / 100.f;
          else
            fH = std::stof(sh);
        } catch (...) {}
      }
      scaleX = (iW > 0.f) ? fW / iW : 1.f;
      scaleY = (iH > 0.f) ? fH / iH : 1.f;
    }
  }
  return true;
}

/**
 * Build an SkShader from a decoded SkImage, applying mask-size, mask-position,
 * and mask-repeat to fit the img onto the given bounds rect.
 *
 * intrinsic : the img's intrinsic size, which sizing is based on. The decoded
 *             `img` may be smaller (glint_img_level_dims()); empty means
 *             img's own size.
 */
inline sk_sp<SkShader> glint_mask_image_shader(sk_sp<SkImage> img,
                                               const glint_rect& bounds,
                                               const glint_mask_layer& layer,
                                               SkISize intrinsic = {})
{
  if (!img) return nullptr;
  if (intrinsic.isEmpty()) intrinsic = img->dimensions();

  const float bW = bounds.W(), bH = bounds.H();
  const float iW = static_cast<float>(intrinsic.width());
  const float iH = static_cast<float>(intrinsic.height());
  float scaleX = 1.f, scaleY = 1.f;
  if (!glint_mask_image_scale(intrinsic, bounds, layer, scaleX, scaleY)) return nullptr;

  const float scaledW = iW * scaleX;
  const float scaledH = iH * scaleY;

  // ── mask-position ─────────────────────────────────────────────────────────
  float fx = 0.f, fy = 0.f;
  glint_mask_resolve_position(layer.position, fx, fy);
  const float tx = bounds.L + fx * (bW - scaledW);
  const float ty = bounds.T + fy * (bH - scaledH);

  // ── mask-repeat → tile modes ───────────────────────────────────────────────
  SkTileMode tmX = SkTileMode::kDecal;
  SkTileMode tmY = SkTileMode::kDecal;
  const std::string& rep = layer.repeat;
  if (rep == "repeat")   { tmX = SkTileMode::kRepeat; tmY = SkTileMode::kRepeat; }
  if (rep == "repeat-x") { tmX = SkTileMode::kRepeat; tmY = SkTileMode::kDecal;  }
  if (rep == "repeat-y") { tmX = SkTileMode::kDecal;  tmY = SkTileMode::kRepeat; }

  // ── Build local matrix: img → canvas coords ──────────────────────────────
  // (decoded pixels → intrinsic pixels → CSS px)
  SkMatrix lm = SkMatrix::Scale(scaleX * iW / static_cast<float>(img->width()),
                                scaleY * iH / static_cast<float>(img->height()));
  lm.postTranslate(tx, ty);

  // SkImage::makeShader() expects the forward local matrix that maps img space
  // into canvas space. Passing the inverse here makes the sampled img appear
  // massively zoomed/cropped because the shader interprets canvas coordinates as
  // already being in img space. CSS background-size / position / repeat need
  // the forward img→canvas transform.
  if (!lm.isFinite()) {
    // Fallback: stretch to fill.
    SkMatrix fill;
    fill.setRectToRect(SkRect::MakeWH(static_cast<float>(img->width()), static_cast<float>(img->height())),
                       SkRect::MakeLTRB(bounds.L, bounds.T, bounds.R, bounds.B),
                       SkMatrix::kFill_ScaleToFit);
    if (!fill.isFinite()) return nullptr;
    return img->makeShader(tmX, tmY, SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), &fill);
  }

  return img->makeShader(tmX, tmY, SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), &lm);
}

// ── Luminance-to-alpha color filter ──────────────────────────────────────────

/**
 * Returns an SkColorFilter that converts RGB → alpha using the ITU-R BT.709
 * luminance formula.  Used for mask-mode: luminance.
 *
 *   A' = 0.2126*R + 0.7152*G + 0.0722*B
 */
inline sk_sp<SkColorFilter> glint_mask_luma_color_filter()
{
  // SkColorMatrix row-major 4x5: each row is [R G B A offset] for one output channel.
  const float m[20] = {
    0,      0,      0,      0, 0,          // R' = 0
    0,      0,      0,      0, 0,          // G' = 0
    0,      0,      0,      0, 0,          // B' = 0
    0.2126f,0.7152f,0.0722f,0, 0,          // A' = luma
  };
  return SkColorFilters::Matrix(m);
}

// ── mask-composite → Skia blend mode (accumulation surface) ──────────────────

/**
 * Blend mode for compositing one CSS mask layer INTO an accumulation surface.
 *
 * The accumulation surface starts fully transparent.  Each mask layer is painted
 * into it using the blend mode that implements the CSS mask-composite keyword
 * between THIS layer and the previous accumulated mask.
 *
 * First layer always uses kSrc (write into the blank accumulation surface).
 * Subsequent layers use the inter-mask blend corresponding to mask-composite.
 */
inline SkBlendMode glint_mask_accum_blend_mode(const std::string& composite, bool isFirst)
{
  if (isFirst) return SkBlendMode::kSrc;  // paint into blank surface

  if (composite == "add")       return SkBlendMode::kPlus;     // A = As + Ad
  if (composite == "subtract")  return SkBlendMode::kDstOut;   // A = Ad*(1-As)
  if (composite == "intersect") return SkBlendMode::kDstIn;    // A = As*Ad
  if (composite == "exclude")   return SkBlendMode::kXor;      // A = As+Ad-2*As*Ad

  return SkBlendMode::kSrcOver;  // default
}

// Keep old name for any legacy call sites.
inline SkBlendMode glint_mask_blend_mode(const std::string& composite, bool isFirst)
{
  return glint_mask_accum_blend_mode(composite, isFirst);
}

// ── Destination rect for a mask img layer ───────────────────────────────────

/**
 * Compute the canvas-space destination SkRect where a mask img layer is placed,
 * applying mask-size and mask-position.  Used by drawImageRect (avoids the
 * kDecal half-pixel fringe produced by shader sampling at texel-row boundaries).
 */
inline SkRect glint_mask_image_dst_rect(sk_sp<SkImage> img,
                                        const glint_rect& bounds,
                                        const glint_mask_layer& layer)
{
  if (!img) return SkRect::MakeEmpty();

  const float bW = bounds.W(), bH = bounds.H();
  const float iW = static_cast<float>(img->width());
  const float iH = static_cast<float>(img->height());
  if (iW <= 0.f || iH <= 0.f || bW <= 0.f || bH <= 0.f) return SkRect::MakeEmpty();

  float scaleX = 1.f, scaleY = 1.f;
  const std::string& sz = layer.size;
  if (sz == "cover") {
    const float scl = std::max(bW / iW, bH / iH); scaleX = scaleY = scl;
  } else if (sz == "contain") {
    const float scl = std::min(bW / iW, bH / iH); scaleX = scaleY = scl;
  } else if (sz != "auto") {
    std::istringstream ss(sz);
    std::string sw, sh;
    if (ss >> sw) {
      float fW = bW, fH = bH;
      try { fW = sw.back()=='%' ? std::stof(sw)*bW/100.f : std::stof(sw); } catch(...) {}
      if (ss >> sh) {
        try { fH = sh.back()=='%' ? std::stof(sh)*bH/100.f : std::stof(sh); } catch(...) {}
      }
      scaleX = iW > 0.f ? fW / iW : 1.f;
      scaleY = iH > 0.f ? fH / iH : 1.f;
    }
  }

  const float scaledW = iW * scaleX;
  const float scaledH = iH * scaleY;
  float fx = 0.f, fy = 0.f;
  glint_mask_resolve_position(layer.position, fx, fy);
  const float tx = bounds.L + fx * (bW - scaledW);
  const float ty = bounds.T + fy * (bH - scaledH);
  return SkRect::MakeLTRB(tx, ty, tx + scaledW, ty + scaledH);
}

// ── SVG DOM cache ─────────────────────────────────────────────────────────────


/** Load (or retrieve from cache) an SkSVGDOM for the given file path.
 *  The cache is always checked first so the onRequest handler (and its disk
 *  I/O) only fires once per unique path, not once per render frame. */
inline sk_sp<SkSVGDOM> glint_load_svg_dom(
    const std::string& path,
    const std::function<void(glint_resource_request&)>* onRequest = nullptr,
    const glint_element* source = nullptr,
    glint_network_log* netLog = nullptr)
{
  // ── Cache — checked first regardless of handler registration ─────────────
  {
    std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
    auto& cache = glint_svg_dom_cache();
    auto it = cache.find(path);
    if (it != cache.end()) return it->second.dom;
  }

  GlintCachedResource cachedRes;
  if (glint_resource_cache_lookup(path, glint_resource_request::Type::SVG, &cachedRes))
  {
    if (!cachedRes.data) return nullptr;
    return glint_load_svg_dom_cached(path, cachedRes.data);
  }

  // ── Callback path ─────────────────────────────────────────────────────────
  if (onRequest && *onRequest)
  {
    glint_resource_request req;
    req.url    = path;
    req.parseUrl();
    req.type   = glint_resource_request::Type::SVG;
    req.source = source;
    (*onRequest)(req);
    if (!req.handled) req.error(500, "Handler did not respond to request");
    glint_network_log_push(netLog, path, glint_resource_request::Type::SVG, req);
    glint_resource_cache_store(path, glint_resource_request::Type::SVG, req);
    sk_sp<SkSVGDOM> dom;
    if (req.responseData)
      dom = glint_load_svg_dom_cached(path, req.responseData);
    return dom;
  }
  // ── Disk path (no handler registered) ────────────────────────────────────
  auto data = SkData::MakeFromFileName(path.c_str());
  glint_resource_cache_store_disk(path, glint_resource_request::Type::SVG, data, path);
  sk_sp<SkSVGDOM> dom = data ? glint_load_svg_dom_cached(path, data) : nullptr;
  glint_network_log_push_disk(netLog, path, glint_resource_request::Type::SVG, dom != nullptr);
  return dom;
}

/** Rasterize an SkSVGDOM (optionally a specific node) to an SkImage of the given size.
 *
 *  The canvas is scaled from the SVG's intrinsic (container) coordinate space to W×H
 *  before calling render/renderNode.  This mirrors the expected destination-fit draw path:
 *    float xScale = dest.W() / svg.W();
 *    float yScale = dest.H() / svg.H();
 *    PathTransformScale(scale); dom->render(canvas);
 *
 *  Without this transform, SVGs whose root <svg> has absolute pixel dimensions
 *  (e.g. width="2048" height="1529") render at their native size and are almost
 *  entirely clipped by the small raster surface.  setContainerSize() is a no-op
 *  for such SVGs — it only affects relative-length roots (width="100%").
 */
inline sk_sp<SkImage> glint_rasterize_svg(const sk_sp<SkSVGDOM>& dom,
                                          int W, int H,
                                          const char* nodeId = nullptr)
{
  if (!dom || W <= 0 || H <= 0) return nullptr;

  auto surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(W, H));
  if (!surf) return nullptr;

  SkCanvas* mc = surf->getCanvas();
  mc->clear(SK_ColorTRANSPARENT);

  // The DOM is shared through the SVG cache (and across window threads), and
  // the relative-size path below mutates it: serialize mutate + render.
  static std::mutex sRasterizeMutex;
  std::lock_guard<std::mutex> lock(sRasterizeMutex);

  // Apply a scale transform so the SVG's native coordinate space maps to W×H.
  // Use the root's own intrinsic size (absolute width / height), not
  // containerSize(): the fallback below changes the container size, after
  // which containerSize() looked absolute and every later call scaled by the
  // first caller's W/H.
  SkSize natural = SkSize::Make(0.f, 0.f);
  if (dom->getRoot())
    natural = dom->getRoot()->intrinsicSize(SkSVGLengthContext(SkSize::Make(0.f, 0.f)));
  const float natW = natural.width();
  const float natH = natural.height();
  if (natW > 0.f && natH > 0.f)
  {
    mc->scale(static_cast<float>(W) / natW,
              static_cast<float>(H) / natH);
  }
  else
  {
    // Relative-size root: tell the DOM the viewport size (every call).
    dom->setContainerSize(SkSize::Make(static_cast<float>(W), static_cast<float>(H)));
  }

  if (nodeId && nodeId[0] != '\0') {
    SkSVGPresentationContext pctx;
    dom->renderNode(mc, pctx, nodeId);
  } else {
    dom->render(mc);
  }

  return surf->makeImageSnapshot();
}


// ── Raster img cache ────────────────────────────────────────────────────────

/** Global thread-safe cache: file path → SkImage */
inline std::unordered_map<std::string, sk_sp<SkImage>>& glint_img_cache()
{
  static std::unordered_map<std::string, sk_sp<SkImage>> cache;
  return cache;
}
// inline (one mutex for the program), not static: glint_img_cache() is one
// shared map, and a static mutex gave every translation unit its own lock.
inline std::mutex gGlintImgCacheMutex;

/** When each cached img was last looked up or stored (glint_trim_image_cache()).
 *  Guarded by gGlintImgCacheMutex. */
inline std::unordered_map<std::string, std::chrono::steady_clock::time_point>& glint_img_last_used()
{
  static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastUsed;
  return lastUsed;
}

/** Caller holds gGlintImgCacheMutex. */
inline void glint_img_touch_locked(const std::string& path)
{
  glint_img_last_used()[path] = std::chrono::steady_clock::now();
}

/** Looks `path` up in glint_img_cache(). True when it has an entry (which is
 *  nullptr for an img that failed to load). */
inline bool glint_img_cache_lookup(const std::string& path, sk_sp<SkImage>& out)
{
  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  auto& cache = glint_img_cache();
  auto it = cache.find(path);
  if (it == cache.end()) return false;
  out = it->second;
  glint_img_touch_locked(path);
  return true;
}

// ── Size levels ─────────────────────────────────────────────────────────────
// A large img is rarely drawn at its full size (a 2400x1792 photo filling a
// card is ~22 MB decoded with mipmaps, ~22 MB again on the GPU). Draw paths
// that know their size ask for a "level": the img decoded at its size halved
// `level` times (level 0 = full size), the smallest one still at least as
// large as what is drawn in device pixels, so it is never upscaled. Levels
// are cached under their own key; level 0 under the plain path.

constexpr int kGlintImgMaxLevel = 8;

/** Size of `intrinsic` halved `level` times (as its mipmap level). */
inline SkISize glint_img_level_dims(SkISize intrinsic, int level)
{
  return { std::max(1, intrinsic.width() >> level), std::max(1, intrinsic.height() >> level) };
}

/** The smallest level of an img of size `intrinsic` that is still at least
 *  needW x needH device pixels. */
inline int glint_img_level_for(SkISize intrinsic, float needW, float needH)
{
  if (intrinsic.isEmpty() || !(needW > 0.f) || !(needH > 0.f)) return 0;
  int level = 0;
  while (level < kGlintImgMaxLevel)
  {
    const SkISize next = glint_img_level_dims(intrinsic, level + 1);
    if (next == glint_img_level_dims(intrinsic, level)) break;
    if (static_cast<float>(next.width()) < needW || static_cast<float>(next.height()) < needH) break;
    ++level;
  }
  return level;
}

/** glint_img_cache() key of an img level. */
inline std::string glint_img_level_key(const std::string& path, int level)
{
  return level == 0 ? path : path + "\n@" + std::to_string(level);
}

/** The path a glint_img_level_key() was made from. */
inline std::string glint_img_key_path(const std::string& key)
{
  const size_t at = key.rfind("\n@");
  return at == std::string::npos ? key : key.substr(0, at);
}

/** A decoded level of `path` other than `level`, the nearest one, finer
 *  first; nullptr when none is cached. Drawn while `level` decodes. */
inline sk_sp<SkImage> glint_img_nearest_level(const std::string& path, int level)
{
  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  auto& cache = glint_img_cache();
  for (int d = 1; d <= kGlintImgMaxLevel; ++d)
    for (const int l : { level - d, level + d })
    {
      if (l < 0 || l > kGlintImgMaxLevel) continue;
      auto it = cache.find(glint_img_level_key(path, l));
      if (it != cache.end() && it->second && !it->second->isLazyGenerated()) return it->second;
    }
  return nullptr;
}

/** The img to draw for `level` of `path`: that level, or the one just finer
 *  when that is decoded (at most twice the size, and it saves holding both,
 *  e.g. when one element shows the img at full size and another at half).
 *  False when neither is cached; `out` is nullptr for an img that failed. */
inline bool glint_img_lookup_level(const std::string& path, int level, sk_sp<SkImage>& out)
{
  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  auto& cache = glint_img_cache();
  const std::string key = glint_img_level_key(path, level);
  auto it = cache.find(key);
  if (it != cache.end()) { out = it->second; glint_img_touch_locked(key); return true; }
  if (level == 0) return false;
  const std::string finerKey = glint_img_level_key(path, level - 1);
  it = cache.find(finerKey);
  if (it == cache.end() || !it->second || it->second->isLazyGenerated()) return false;
  out = it->second;
  glint_img_touch_locked(finerKey);
  return true;
}

/** The nearest decoded level of `path` finer than `level` that has mipmaps
 *  (a source to scale `level` from); nullptr when none is cached. */
inline sk_sp<SkImage> glint_img_finer_level(const std::string& path, int level)
{
  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  auto& cache = glint_img_cache();
  for (int l = level - 1; l >= 0; --l)
  {
    auto it = cache.find(glint_img_level_key(path, l));
    if (it != cache.end() && it->second && it->second->hasMipmaps()) return it->second;
  }
  return nullptr;
}

/** Intrinsic sizes read from img headers, by path (empty: not decodable).
 *  Guarded by gGlintImgCacheMutex. */
inline std::unordered_map<std::string, SkISize>& glint_img_intrinsic()
{
  static std::unordered_map<std::string, SkISize> sizes;
  return sizes;
}

/** Fetches the encoded bytes of an img: resource cache, then the onRequest
 *  handler, then disk. Records the request in the network log. nullptr when
 *  the img is missing. Runs on the calling thread. */
inline sk_sp<SkData> glint_fetch_image_bytes(
    const std::string& path,
    const std::function<void(glint_resource_request&)>* onRequest,
    const glint_element* source,
    glint_network_log* netLog)
{
  GlintCachedResource cachedRes;
  if (glint_resource_cache_lookup(path, glint_resource_request::Type::Image, &cachedRes))
    return cachedRes.data;

  // ── Callback path ─────────────────────────────────────────────────────────
  if (onRequest && *onRequest)
  {
    glint_resource_request req;
    req.url    = path;
    req.parseUrl();
    req.type   = glint_resource_request::Type::Image;
    req.source = source;
    (*onRequest)(req);
    // If the handler returned without calling any respond helper, auto-stamp a
    // 500 error so the network log always records a meaningful status.
    if (!req.handled) req.error(500, "Handler did not respond to request");
    glint_network_log_push(netLog, path, glint_resource_request::Type::Image, req);
    glint_resource_cache_store(path, glint_resource_request::Type::Image, req);
    return req.responseData;
  }
  // ── Disk path (no handler registered) ─────────────────────────────────────
  auto data = SkData::MakeFromFileName(path.c_str());
  glint_resource_cache_store_disk(path, glint_resource_request::Type::Image, data, path);
  glint_network_log_push_disk(netLog, path, glint_resource_request::Type::Image, data != nullptr);
  return data;
}

/** Load (or retrieve from cache) an SkImage for the given path.
 *  If onRequest is non-null, it is fired before the disk lookup. The result
 *  (also a failure, as nullptr) is cached, so the handler and the disk are hit
 *  once per path. Decodes lazily, inside the first draw: draw paths that can
 *  show the element without the img first use glint_load_image_async(). */
inline sk_sp<SkImage> glint_load_image(
    const std::string& path,
    const std::function<void(glint_resource_request&)>* onRequest = nullptr,
    const glint_element* source = nullptr,
    glint_network_log* netLog = nullptr)
{
  sk_sp<SkImage> cached;
  if (glint_img_cache_lookup(path, cached)) return cached;

  sk_sp<SkData> data = glint_fetch_image_bytes(path, onRequest, source, netLog);
  sk_sp<SkImage> img = data ? SkImages::DeferredFromEncodedData(std::move(data)) : nullptr;

  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  glint_img_touch_locked(path);
  // A background decode of the same path may have finished meanwhile; keep it.
  return glint_img_cache().emplace(path, img).first->second;
}

/** The intrinsic size of the img at `path`, read from its header (fetching
 *  its bytes like glint_load_image(), without decoding). Cached; empty when
 *  the img is missing or not decodable. */
inline SkISize glint_img_intrinsic_size(
    const std::string& path,
    const std::function<void(glint_resource_request&)>* onRequest,
    const glint_element* source,
    glint_network_log* netLog)
{
  {
    std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
    auto& sizes = glint_img_intrinsic();
    auto it = sizes.find(path);
    if (it != sizes.end()) return it->second;
  }
  SkISize size = SkISize::MakeEmpty();
  if (sk_sp<SkData> data = glint_fetch_image_bytes(path, onRequest, source, netLog))
    if (sk_sp<SkImage> header = SkImages::DeferredFromEncodedData(std::move(data)))
      size = header->dimensions();   // EXIF orientation applied, as when decoded
  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  glint_img_intrinsic()[path] = size;
  return size;
}

// ── Background img decode ───────────────────────────────────────────────────
// Decoding a large PNG / JPEG and building its mipmaps takes 50-150 ms (a
// 2400x1792 PNG: ~100 ms). Done lazily inside the first frame that draws the
// img, that stalls opening the page. glint_load_image_async() fetches the
// bytes on the calling thread (so the onRequest handler and the network log
// keep their threading), decodes them on one shared worker thread into a
// raster img with mipmaps (the GPU upload then only copies), stores it in
// glint_img_cache() and calls the waiters, from the worker thread.

/** Off (e.g. for headless captures that need every img in their first
 *  frame): glint_load_image_async() decodes synchronously, like
 *  glint_load_image(). */
inline std::atomic<bool> gGlintAsyncImageDecode{ true };

/** path -> callbacks waiting for its background decode.
 *  Guarded by gGlintImgCacheMutex. */
inline std::unordered_map<std::string, std::vector<std::function<void()>>>& glint_img_pending()
{
  static std::unordered_map<std::string, std::vector<std::function<void()>>> pending;
  return pending;
}

/** Stores the result for `path` (unless `store` is false) and runs (outside
 *  the lock) everything that waited for it. */
inline void glint_img_finish_pending(const std::string& path, sk_sp<SkImage> img, bool store = true)
{
  std::vector<std::function<void()>> waiters;
  {
    std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
    if (store)
    {
      glint_img_cache()[path] = std::move(img);
      glint_img_touch_locked(path);
    }
    auto& pending = glint_img_pending();
    auto it = pending.find(path);
    if (it != pending.end())
    {
      waiters.swap(it->second);
      pending.erase(it);
    }
  }
  for (auto& waiter : waiters)
    if (waiter) waiter();
}

class glint_img_decoder
{
public:
  static glint_img_decoder& instance()
  {
    static glint_img_decoder decoder;
    return decoder;
  }

  /** Decodes `data` at `level` of `intrinsic` (glint_img_level_dims()) and
   *  stores the result under `key`. */
  void enqueue(std::string key, sk_sp<SkData> data, int level = 0, SkISize intrinsic = {})
  {
    {
      std::lock_guard<std::mutex> lk(mMutex);
      if (!mThread.joinable()) mThread = std::thread([this] { run(); });
      mJobs.push_back({ std::move(key), std::move(data), level, intrinsic });
    }
    mCv.notify_one();
  }

  /** Full decode into a raster img with mipmaps, halved `level` times;
   *  nullptr when undecodable. */
  static sk_sp<SkImage> decode(sk_sp<SkData> data, int level = 0)
  {
    sk_sp<SkImage> img = SkImages::DeferredFromEncodedData(std::move(data));
    if (!img) return nullptr;
    sk_sp<SkImage> raster = img->makeRasterImage(nullptr);
    if (!raster) return img;
    sk_sp<SkImage> mipped = raster->withDefaultMipmaps();
    if (level > 0 && mipped)
      if (sk_sp<SkImage> scaled = downscale(mipped, glint_img_level_dims(raster->dimensions(), level)))
        return scaled;
    return mipped ? mipped : raster;
  }

  /** `src` (a raster img with mipmaps) resized to `dims`, with mipmaps;
   *  nullptr on failure. Sampling src's mipmaps makes this a box filter at
   *  exact halvings. (Drawn through a canvas: SkImage::scalePixels() drops
   *  src's mipmaps and builds them again in Skia's global cache, where they
   *  stay.) */
  static sk_sp<SkImage> downscale(const sk_sp<SkImage>& src, SkISize dims)
  {
    SkBitmap scaled;
    if (!scaled.tryAllocPixels(src->imageInfo().makeDimensions(dims))) return nullptr;
    {
      SkCanvas canvas(scaled);
      SkPaint copy;
      copy.setBlendMode(SkBlendMode::kSrc);
      canvas.drawImageRect(src, SkRect::Make(dims), SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear),
                           &copy);
    }
    scaled.setImmutable();
    sk_sp<SkImage> raster = scaled.asImage();
    if (!raster) return nullptr;
    sk_sp<SkImage> mipped = raster->withDefaultMipmaps();
    return mipped ? mipped : raster;
  }

  ~glint_img_decoder()
  {
    {
      std::lock_guard<std::mutex> lk(mMutex);
      mStop = true;
    }
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
  }

private:
  struct job { std::string key; sk_sp<SkData> data; int level = 0; SkISize intrinsic; };

  void run()
  {
    for (;;)
    {
      job next;
      {
        std::unique_lock<std::mutex> lk(mMutex);
        mCv.wait(lk, [this] { return mStop || !mJobs.empty(); });
        if (mStop) return;
        // The finest queued level of the front job's img goes first: the
        // coarser ones are then scaled from it instead of decoded again.
        auto pick = mJobs.begin();
        const std::string path = glint_img_key_path(pick->key);
        for (auto it = mJobs.begin(); it != mJobs.end(); ++it)
          if (it->level < pick->level && glint_img_key_path(it->key) == path) pick = it;
        next = std::move(*pick);
        mJobs.erase(pick);
      }
      const std::string path = glint_img_key_path(next.key);
      if (finishIfFinerCached(next, path)) continue;

      sk_sp<SkImage> img;
      if (next.level > 0 && !next.intrinsic.isEmpty())
        if (sk_sp<SkImage> finer = glint_img_finer_level(path, next.level))
          img = downscale(finer, glint_img_level_dims(next.intrinsic, next.level));
      if (img)
      {
        glint_img_finish_pending(next.key, std::move(img));
        continue;
      }

      // Decode the file at full size; this job's level and every other level
      // of the same img queued meanwhile (a page asks for several in its
      // first frame) are made from it rather than decoding the file again.
      const sk_sp<SkImage> full = decode(std::move(next.data));
      std::vector<job> same;
      {
        std::lock_guard<std::mutex> lk(mMutex);
        for (auto it = mJobs.begin(); it != mJobs.end();)
          if (glint_img_key_path(it->key) == path) { same.push_back(std::move(*it)); it = mJobs.erase(it); }
          else ++it;
      }
      same.insert(same.begin(), std::move(next));
      std::stable_sort(same.begin(), same.end(), [](const job& a, const job& b) { return a.level < b.level; });
      for (job& j : same)
      {
        if (finishIfFinerCached(j, path)) continue;
        sk_sp<SkImage> level = full;
        if (full && j.level > 0 && !full->isLazyGenerated())
          if (sk_sp<SkImage> scaled = downscale(full, glint_img_level_dims(full->dimensions(), j.level)))
            level = std::move(scaled);
        glint_img_finish_pending(j.key, std::move(level));
      }
    }
  }

  /** When the level just finer than `j`'s is decoded, it is drawn instead
   *  (glint_img_lookup_level()): wakes `j`'s waiters without storing. */
  static bool finishIfFinerCached(const job& j, const std::string& path)
  {
    sk_sp<SkImage> drawn;
    if (j.level == 0 || !glint_img_lookup_level(path, j.level, drawn) || !drawn) return false;
    glint_img_finish_pending(j.key, nullptr, /*store=*/false);
    return true;
  }

  std::mutex              mMutex;
  std::condition_variable mCv;
  std::deque<job>         mJobs;
  bool                    mStop = false;
  std::thread             mThread;
};

/** Like glint_load_image(), but never decodes on the calling thread: returns
 *  nullptr while the img is decoded in the background, and `onReady` is
 *  called (from the decode thread) once it is in the cache. Use it where the
 *  element can be drawn without the img for a frame or two. `level` > 0
 *  decodes it at a reduced size (glint_img_level_for()); pass its
 *  `intrinsic` size (glint_img_intrinsic_size()) so a level can be scaled
 *  from a finer one already decoded instead of decoding the file again. */
inline sk_sp<SkImage> glint_load_image_async(
    const std::string& path,
    const std::function<void(glint_resource_request&)>* onRequest,
    const glint_element* source,
    glint_network_log* netLog,
    std::function<void()> onReady,
    int level = 0,
    SkISize intrinsic = {})
{
  const std::string key = glint_img_level_key(path, level);
  const bool async = gGlintAsyncImageDecode.load(std::memory_order_relaxed);
  if (!async && level == 0)
    return glint_load_image(path, onRequest, source, netLog);

  {
    std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
    auto& cache = glint_img_cache();
    auto it = cache.find(key);
    if (it != cache.end()) { glint_img_touch_locked(key); return it->second; }
    if (async)
    {
      auto& pending = glint_img_pending();
      auto pit = pending.find(key);
      if (pit != pending.end())
      {
        pit->second.push_back(std::move(onReady));
        return nullptr;
      }
      pending[key].push_back(std::move(onReady));
    }
  }

  sk_sp<SkData> data = glint_fetch_image_bytes(path, onRequest, source, netLog);
  if (!async)
  {
    // Synchronous, at the requested size.
    sk_sp<SkImage> img = data ? glint_img_decoder::decode(std::move(data), level) : nullptr;
    std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
    glint_img_touch_locked(key);
    return glint_img_cache().emplace(key, img).first->second;
  }
  if (!data)
  {
    glint_img_finish_pending(key, nullptr);
    return nullptr;
  }
  glint_img_decoder::instance().enqueue(key, std::move(data), level, intrinsic);
  return nullptr;
}

// ── Img cache trimming ──────────────────────────────────────────────────────
// glint_img_cache() keeps every img any page loaded: leaving a page destroys
// its elements but not the decoded imgs (a 2400x1792 photo with mipmaps is
// ~23 MB, plus its encoded bytes in the resource cache). Elements hold the
// imgs they draw (background, mask), so an entry only the cache references
// is no longer shown anywhere. glint_trim_image_cache() keeps the most
// recently used of those within a budget (quick back-navigation) and drops
// the rest with their encoded bytes; a later use loads them again.

/** Budget for decoded imgs no element uses: GLINT_IMAGE_CACHE_MB, default 16. */
inline size_t glint_img_unused_budget_bytes()
{
  static const size_t bytes = [] {
    const char* v = std::getenv("GLINT_IMAGE_CACHE_MB");
    const long mb = v ? std::strtol(v, nullptr, 10) : -1;
    return static_cast<size_t>(mb >= 0 ? mb : 16) * 1024 * 1024;
  }();
  return bytes;
}

/** Memory an img keeps alive: decoded pixels (+ mipmaps), or its encoded data
 *  for one that decodes lazily. */
inline size_t glint_img_bytes(const SkImage& img)
{
  if (img.isLazyGenerated())
  {
    sk_sp<SkData> encoded = img.refEncodedData();
    return encoded ? encoded->size() : 0;
  }
  const size_t pixels = img.imageInfo().computeMinByteSize();
  return img.hasMipmaps() ? pixels + pixels / 3 : pixels;
}

/** Drops cached imgs no element uses (see above). Imgs used in the last few
 *  seconds stay: one that finished decoding may not have been drawn yet. */
inline void glint_trim_image_cache()
{
  using clock = std::chrono::steady_clock;
  struct candidate { std::string path; size_t bytes; clock::time_point used; };
  std::vector<candidate> unused;
  std::vector<std::string> evicted;
  {
    std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
    const auto now = clock::now();
    auto& lastUsed = glint_img_last_used();
    for (const auto& [path, img] : glint_img_cache())
    {
      if (!img || !img->unique()) continue;   // failed load, or still shown
      const auto it = lastUsed.find(path);
      const clock::time_point used = it != lastUsed.end() ? it->second : clock::time_point{};
      if (now - used < std::chrono::seconds(5)) continue;
      unused.push_back({ path, glint_img_bytes(*img), used });
    }
    std::sort(unused.begin(), unused.end(), [](const candidate& a, const candidate& b) { return a.used > b.used; });
    size_t kept = 0;
    for (const auto& c : unused)
    {
      if (kept + c.bytes <= glint_img_unused_budget_bytes()) { kept += c.bytes; continue; }
      evicted.push_back(c.path);
    }
    for (const auto& key : evicted)
    {
      glint_img_cache().erase(key);
      lastUsed.erase(key);
    }
    // Encoded bytes go with an img's last cached level (another level may
    // still be shown, and its bytes decode the next level it needs).
    std::vector<std::string> paths;
    for (const auto& key : evicted) paths.push_back(glint_img_key_path(key));
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (const auto& [key, img] : glint_img_cache())
    {
      const auto it = std::lower_bound(paths.begin(), paths.end(), glint_img_key_path(key));
      if (it != paths.end() && *it == glint_img_key_path(key)) paths.erase(it);
    }
    for (const auto& path : paths) glint_img_intrinsic().erase(path);
    evicted.swap(paths);
  }
  if (evicted.empty()) return;
  std::lock_guard<std::mutex> lock(glint_resource_cache_mutex());
  for (const auto& path : evicted)
    glint_resource_cache().erase({ path, glint_resource_request::Type::Image });
}

