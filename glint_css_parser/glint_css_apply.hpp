#pragma once

/**
 * glint_css_apply.hpp
 * Declaration→glint_style property mapper.
 *
 * GlintCssApply::apply(decls, style) writes each parsed CSS declaration into
 * the corresponding glint_style field, following the same assignment API that
 * application code uses (so every shorthand proxy and unit-conversion path
 * in glint_style is exercised exactly as it is in hand-written code).
 *
 * Supported properties (CSS name ↔ glint_style field):
 *
 *   COLOR / BACKGROUND
 *     color                       ↔ color
 *     background-color            ↔ backgroundColor
 *     opacity                     ↔ opacity
 *     background                  ↔ background  (shorthand proxy)
 *     background-img            ↔ backgroundImageProp
 *     background-size             ↔ backgroundSize
 *     background-position         ↔ backgroundPosition
 *     background-repeat           ↔ backgroundRepeat
 *
 *   BORDER
 *     border                      ↔ border  (shorthand proxy)
 *     border-top / -right / …     ↔ borderTop / borderRight / …
 *     border-radius               ↔ borderRadius
 *     border-top-left-radius …    ↔ borderTopLeftRadius …
 *     border-style                ↔ borderStyle
 *     border-width                ↔ borderWidth  (shorthand proxy)
 *     border-top-width / …        ↔ borderTopWidth / …
 *     border-color                ↔ borderColor  (shorthand proxy)
 *     border-top-color / …        ↔ borderTopColor / …
 *     border-top-style / …        ↔ borderTopStyle / …
 *
 *   SVG STROKE
 *     stroke                      ↔ strokeColor
 *     stroke-dasharray            ↔ strokeDasharray
 *     stroke-dashoffset           ↔ strokeDashoffset
 *     stroke-linecap              ↔ strokeLinecap
 *     stroke-linejoin             ↔ strokeLinejoin
 *     stroke-miterlimit           ↔ strokeMiterlimit
 *     stroke-opacity              ↔ strokeOpacity
 *     stroke-width                ↔ strokeWidth
 *
 *   SHADOW (box-shadow)
 *     box-shadow                  ↔ shadowEnabled/Color/OffsetX/Y/Blur
 *
 *   TYPOGRAPHY
 *     font-size                   ↔ fontSize
 *     line-height                 ↔ lineHeight
 *     font-family                 ↔ fontFace
 *     font-weight                 ↔ fontWeight
 *     text-align                  ↔ textAlign
 *     vertical-align              ↔ verticalAlign
 *
 *   SPACING
 *     padding / padding-*         ↔ padding / paddingTop / …
 *     margin  / margin-*          ↔ margin  / marginTop  / …
 *
 *   SIZING & POSITIONING
 *     position                    ↔ position
 *     left / top / right / bottom ↔ left / top / right / bottom
 *     width / height              ↔ width / height
 *     min-width / max-width       ↔ minWidth / maxWidth
 *     min-height / max-height     ↔ minHeight / maxHeight
 *     z-index                     ↔ zIndex
 *
 *   LAYOUT
 *     display                     ↔ display
 *     flex-direction              ↔ flexDirection
 *     justify-content             ↔ justifyContent
 *     align-items                 ↔ alignItems
 *     gap                         ↔ gap
 *     flex-grow                   ↔ flexGrow
 *     pointer-events              ↔ pointerEvents
 *     cursor                      ↔ cursor
 *     user-select                 ↔ userSelect
 *     white-space                 ↔ whiteSpace
 *     overflow                    ↔ overflow  (shorthand proxy)
 *     overflow-x / overflow-y     ↔ overflowX / overflowY
 *
 *   SCROLL
 *     scrollbar-width             ↔ scrollbarWidth
 *
 *   OBJECT
 *     object-fit                  ↔ objectFit
 *     object-position             ↔ objectPosition
 *
 *   TRANSFORM / FILTER / MASK
 *     transform                   ↔ transform
 *     filter                      ↔ filter
 *     backdrop-filter             ↔ backdropFilter
 *     mask                        ↔ mask
 *     mask-mode …                 ↔ maskMode …
 *
 *   ANIMATION
 *     transition                  ↔ transition
 *
 * Usage:
 *   // Inline style
 *   auto decls = GlintCssParser::parseInlineStyle("color: red; font-size: 14px");
 *   GlintCssApply::apply(decls, el.style);
 *
 *   // Stylesheet cascade
 *   auto sheet = GlintCssParser::parseStylesheet(cssSource);
 *   auto decls = GlintCssCascade::resolve(domEl, {&sheet}, {});
 *   GlintCssApply::apply(decls, el.style);
 */

#include "glint_css_rule.hpp"
#include "../glint_style.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ── GlintCssApply ──────────────────────────────────────────────────────────────
class GlintCssApply
{
public:
  // Apply a list of declarations to a glint_style.
  // Declarations are applied in order (last writer wins for duplicates).
  static void apply(const std::vector<GlintCssDeclaration>& decls, glint_style& style)
  {
    for (const auto& decl : decls)
      applyOne(decl.property, decl.value, style);
  }

  // Apply a single property/value pair.
  static void applyOne(const std::string& prop, const std::string& val, glint_style& style)
  {
    if (val.empty()) return;

    // ── COLOR / BACKGROUND ───────────────────────────────────────────────────
    if (prop == "color")               { style.color = val.c_str(); return; }
    if (prop == "background-color")    { style.backgroundColor = val.c_str(); return; }
    if (prop == "opacity")             { style.opacity = toFloat(val); return; }
    if (prop == "background")          { style.background = val.c_str(); return; }
    if (prop == "background-img")    { style.backgroundImageProp = val.c_str(); return; }
    if (prop == "background-size")     { style.backgroundSize     = val; return; }
    if (prop == "background-position") { style.backgroundPosition = val; return; }
    if (prop == "background-repeat")   { style.backgroundRepeat   = val; return; }

    // ── BORDER shorthands ────────────────────────────────────────────────────
    if (prop == "border")        { style.border        = val.c_str(); return; }
    if (prop == "border-top")    { style.borderTop     = val.c_str(); return; }
    if (prop == "border-right")  { style.borderRight   = val.c_str(); return; }
    if (prop == "border-bottom") { style.borderBottom  = val.c_str(); return; }
    if (prop == "border-left")   { style.borderLeft    = val.c_str(); return; }

    // ── BORDER individual ────────────────────────────────────────────────────
    if (prop == "border-radius")              { style.borderRadius = val.c_str(); return; }
    if (prop == "border-top-left-radius")     { style.borderTopLeftRadius     = val.c_str(); return; }
    if (prop == "border-top-right-radius")    { style.borderTopRightRadius    = val.c_str(); return; }
    if (prop == "border-bottom-right-radius") { style.borderBottomRightRadius = val.c_str(); return; }
    if (prop == "border-bottom-left-radius")  { style.borderBottomLeftRadius  = val.c_str(); return; }

    if (prop == "border-style")        { style.borderStyle = val; return; }
    if (prop == "border-top-style")    { style.borderTopStyle    = val; return; }
    if (prop == "border-right-style")  { style.borderRightStyle  = val; return; }
    if (prop == "border-bottom-style") { style.borderBottomStyle = val; return; }
    if (prop == "border-left-style")   { style.borderLeftStyle   = val; return; }

    if (prop == "border-width")        { style.borderWidth = val.c_str(); return; }
    if (prop == "border-top-width")    { style.borderTopWidth    = val.c_str(); return; }
    if (prop == "border-right-width")  { style.borderRightWidth  = val.c_str(); return; }
    if (prop == "border-bottom-width") { style.borderBottomWidth = val.c_str(); return; }
    if (prop == "border-left-width")   { style.borderLeftWidth   = val.c_str(); return; }

    if (prop == "border-color")        { style.borderColor = val.c_str(); return; }
    if (prop == "border-top-color")    { style.borderTopColor    = val.c_str(); return; }
    if (prop == "border-right-color")  { style.borderRightColor  = val.c_str(); return; }
    if (prop == "border-bottom-color") { style.borderBottomColor = val.c_str(); return; }
    if (prop == "border-left-color")   { style.borderLeftColor   = val.c_str(); return; }

    // ── SVG STROKE ───────────────────────────────────────────────────────────
    if (prop == "stroke")             { style.strokeColor    = val.c_str(); return; }
    if (prop == "stroke-dasharray")   { style.strokeDasharray   = val; return; }
    if (prop == "stroke-dashoffset")  { style.strokeDashoffset  = toFloat(val); return; }
    if (prop == "stroke-linecap")     { style.strokeLinecap     = val; return; }
    if (prop == "stroke-linejoin")    { style.strokeLinejoin    = val; return; }
    if (prop == "stroke-miterlimit")  { style.strokeMiterlimit  = toFloat(val); return; }
    if (prop == "stroke-opacity")     { style.strokeOpacity     = toFloat(val); return; }
    if (prop == "stroke-width")       { style.strokeWidth       = toFloat(val); return; }

    // ── BOX SHADOW ───────────────────────────────────────────────────────────
    if (prop == "box-shadow")         { applyBoxShadow(val, style); return; }

    // ── TYPOGRAPHY ───────────────────────────────────────────────────────────
    if (prop == "font-size")    { style.fontSize = val.c_str(); return; }
    if (prop == "line-height")
    {
      // Unitless → multiplier; px/em → convert
      style.lineHeight = toFloat(val);
      return;
    }
    if (prop == "font-family")
    {
      // Strip surrounding quotes and take the first comma-separated family name.
      // Per CSS spec, elements inherit the resolved first family only.
      std::string fam = val;
      // Remove surrounding whitespace
      const size_t fs = fam.find_first_not_of(" \t");
      const size_t fe = fam.find_last_not_of(" \t");
      if (fs == std::string::npos) { style.fontFamily = ""; return; }
      fam = fam.substr(fs, fe - fs + 1);
      // Take first comma-separated family
      const size_t comma = fam.find(',');
      if (comma != std::string::npos) fam = fam.substr(0, comma);
      // Strip surrounding quotes
      const size_t qs = fam.find_first_not_of(" \t");
      const size_t qe = fam.find_last_not_of(" \t");
      if (qs != std::string::npos) fam = fam.substr(qs, qe - qs + 1);
      if (fam.size() >= 2 && ((fam.front() == '"' && fam.back() == '"') ||
                               (fam.front() == '\'' && fam.back() == '\'')))
        fam = fam.substr(1, fam.size() - 2);
      style.fontFamily = fam;
      return;
    }
    if (prop == "font-style")
    {
      // Accepts: normal | italic | oblique
      const std::string low = toLower(val);
      if (low == "italic" || low == "oblique") style.fontStyle = low;
      else                                      style.fontStyle = "normal";
      return;
    }
    if (prop == "font-weight")
    {
      // Accepts integer or keyword: normal=400, bold=700
      const std::string low = toLower(val);
      if      (low == "normal")    style.fontWeight = 400.f;
      else if (low == "bold")      style.fontWeight = 700.f;
      else if (low == "lighter")   style.fontWeight = 300.f;
      else if (low == "bolder")    style.fontWeight = 900.f;
      else                         style.fontWeight = toFloat(val);
      return;
    }
    if (prop == "text-align")
    {
      const std::string low = toLower(val);
      if      (low == "left"   || low == "start") style.textAlign = EAlign::Near;
      else if (low == "center"               )    style.textAlign = EAlign::Center;
      else if (low == "right"  || low == "end")   style.textAlign = EAlign::Far;
      return;
    }
    if (prop == "vertical-align") { style.verticalAlign = toLower(val); return; }
    if (prop == "text-decoration") { style.textDecoration = toLower(val); return; }

    // ── SPACING ───────────────────────────────────────────────────────────────
    if (prop == "padding")        { style.padding = val.c_str(); return; }
    if (prop == "padding-top")    { style.paddingTop    = val.c_str(); return; }
    if (prop == "padding-right")  { style.paddingRight  = val.c_str(); return; }
    if (prop == "padding-bottom") { style.paddingBottom = val.c_str(); return; }
    if (prop == "padding-left")   { style.paddingLeft   = val.c_str(); return; }

    if (prop == "margin")         { style.margin = val.c_str(); return; }
    if (prop == "margin-top")     { style.marginTop    = val.c_str(); return; }
    if (prop == "margin-right")   { style.marginRight  = val.c_str(); return; }
    if (prop == "margin-bottom")  { style.marginBottom = val.c_str(); return; }
    if (prop == "margin-left")    { style.marginLeft   = val.c_str(); return; }

    // ── SIZING & POSITIONING ─────────────────────────────────────────────────
    if (prop == "position")   { style.position = val; return; }
    if (prop == "left")       { style.left     = val.c_str(); return; }
    if (prop == "top")        { style.top      = val.c_str(); return; }
    if (prop == "right")      { style.right    = val.c_str(); return; }
    if (prop == "bottom")     { style.bottom   = val.c_str(); return; }
    if (prop == "width")      { style.width    = val.c_str(); return; }
    if (prop == "height")     { style.height   = val.c_str(); return; }
    if (prop == "min-width")  { style.minWidth  = val.c_str(); return; }
    if (prop == "max-width")  { style.maxWidth  = val.c_str(); return; }
    if (prop == "min-height") { style.minHeight = val.c_str(); return; }
    if (prop == "max-height") { style.maxHeight = val.c_str(); return; }
    if (prop == "z-index")    { style.zIndex = static_cast<int>(toFloat(val)); return; }

    // ── LAYOUT ───────────────────────────────────────────────────────────────
    if (prop == "display")          { style.display        = val; return; }
    if (prop == "flex-direction")   { style.flexDirection  = val; return; }
    if (prop == "justify-content")  { style.justifyContent = val; return; }
    if (prop == "align-items")      { style.alignItems     = val; return; }
    if (prop == "gap")              { style.gap            = val.c_str(); return; }
    if (prop == "flex-grow")        { style.flexGrow       = toFloat(val); return; }
    if (prop == "pointer-events")   { style.pointerEvents  = val; return; }
    if (prop == "cursor")           { style.cursor         = val; return; }
    if (prop == "user-select")      { style.userSelect     = val; return; }
    if (prop == "white-space")      { style.whiteSpace     = val; return; }

    // flex shorthand: "flex-grow flex-shrink flex-basis" — map grow only for now
    if (prop == "flex")
    {
      const float v = toFloat(val);
      if (v > 0.f) style.flexGrow = v;
      return;
    }

    // ── OVERFLOW ─────────────────────────────────────────────────────────────
    if (prop == "overflow")   { style.overflow  = val.c_str(); return; }
    if (prop == "overflow-x") { style.overflowX = val; return; }
    if (prop == "overflow-y") { style.overflowY = val; return; }

    // ── SCROLLBAR ─────────────────────────────────────────────────────────────
    if (prop == "scrollbar-width")
    {
      // CSS scrollbar-width: auto | thin | none | <length>  (glint stores as float px)
      const std::string low = toLower(val);
      if      (low == "thin") style.scrollbarWidth = 6.f;
      else if (low == "none") style.scrollbarWidth = 0.f;
      else if (low == "auto") style.scrollbarWidth = 12.f; // platform default
      else                    style.scrollbarWidth = toFloat(val);
      return;
    }
    if (prop == "scrollbar-color")
    {
      // CSS scrollbar-color: <thumb-color> <track-color>
      std::istringstream ss(val);
      std::string thumbTok, trackTok;
      if (ss >> thumbTok) style.scrollbarThumbColor = sk_color(thumbTok.c_str());
      if (ss >> trackTok) style.scrollbarTrackColor = sk_color(trackTok.c_str());
      return;
    }

    // ── OBJECT ────────────────────────────────────────────────────────────────
    if (prop == "object-fit")      { style.objectFit      = val; return; }
    if (prop == "object-position") { style.objectPosition = val; return; }

    // ── TRANSFORM / FILTER ────────────────────────────────────────────────────
    if (prop == "transform")        { style.transform       = val; return; }
    if (prop == "filter")           { style.filter          = val; return; }
    if (prop == "backdrop-filter")  { style.backdropFilter  = val; return; }
    // ── BLEND MODES / ISOLATION ──────────────────────────────────────────
    if (prop == "mix-blend-mode")        { style.mixBlendMode        = val; return; }
    if (prop == "background-blend-mode") { style.backgroundBlendMode = val; return; }
    if (prop == "isolation")             { style.isolation           = val; return; }
    // ── MASK ──────────────────────────────────────────────────────────────────
    if (prop == "mask")             { style.mask           = val; return; }
    if (prop == "mask-mode")        { style.maskMode       = val; return; }
    if (prop == "mask-position")    { style.maskPosition   = val; return; }
    if (prop == "mask-size")        { style.maskSize       = val; return; }
    if (prop == "mask-repeat")      { style.maskRepeat     = val; return; }
    if (prop == "mask-origin")      { style.maskOrigin     = val; return; }
    if (prop == "mask-clip")        { style.maskClip       = val; return; }
    if (prop == "mask-composite")   { style.maskComposite  = val; return; }

    // ── TRANSITION / ANIMATION ────────────────────────────────────────────────
    if (prop == "transition")       { style.transition = val; return; }
    if (prop == "animation")        { style.animation  = val; return; }

    // ── CSS CUSTOM PROPERTIES (variables) — ignored silently ─────────────────
    // (--foo: bar)  Cascade users can handle these before calling apply().
  }

private:
  // ── box-shadow parser ────────────────────────────────────────────────────
  // CSS: "none" | <length> <length> [<length>] [<color>]
  // We map the first shadow only (multiple shadows not supported by glint_style).
  static void applyBoxShadow(const std::string& val, glint_style& style)
  {
    style.boxShadow = val;
  }

  // ── Helpers ──────────────────────────────────────────────────────────────

  static float toFloat(const std::string& s)
  {
    if (s.empty()) return 0.f;
    try { return std::stof(s); } catch (...) { return 0.f; }
  }

  static std::string firstTopLevelCommaSegment(const std::string& s)
  {
    int depth = 0;
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
      if (c == '(') ++depth;
      else if (c == ')' && depth > 0) --depth;
      else if (c == ',' && depth == 0) break;
      out.push_back(c);
    }
    return out;
  }

  static std::vector<std::string> splitCssTokens(const std::string& s)
  {
    std::vector<std::string> tokens;
    std::string current;
    int depth = 0;
    for (char c : s)
    {
      if (std::isspace(static_cast<unsigned char>(c)) && depth == 0)
      {
        if (!current.empty())
        {
          tokens.push_back(current);
          current.clear();
        }
        continue;
      }
      if (c == '(') ++depth;
      else if (c == ')' && depth > 0) --depth;
      current.push_back(c);
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
  }

  static std::string toLower(const std::string& s)
  {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
  }

};
