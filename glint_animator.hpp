#pragma once

/**
 * glint_animator.hpp
 * CSS-compatible transition animation system for glint — Option A (dual-style).
 *
 * Each glint_element maintains a `computedStyle` that Draw, getContent and
 * HitTest read from. Callers write to `style` as normal. `tickTransitions()`
 * (called at the start of every Draw frame) syncs `computedStyle` from `style`,
 * then interpolates any active transitions on top.
 *
 * Usage:
 *   // Declare a 300 ms ease-out backgroundColor transition:
 *   comp->style.transition = "background-color 300ms ease-out";
 *
 *   // Now any write to style.backgroundColor causes an animated transition:
 *   comp->style.backgroundColor = "#ff0000";
 *   comp->setDirty(false);   // kick off the first frame; animation chains redraws automatically
 *
 *   // Multiple properties, mixed easings:
 *   comp->style.transition = "background-color 300ms ease-out, opacity 200ms linear";
 *
 *   // Hover effect example (wire via DOM events):
 *   comp->element.addEventListener("mouseenter", [comp](glint_event&) {
 *       comp->style.backgroundColor = "#3399ff";  // triggers the transition
 *       comp->setDirty(false);
 *   });
 *   comp->element.addEventListener("mouseleave", [comp](glint_event&) {
 *       comp->style.backgroundColor = "#1a1a1a";  // reverses it mid-flight if needed
 *       comp->setDirty(false);
 *   });
 *
 * Animatable properties (CSS name → C++ field):
 *   background-color           → backgroundColor              (ARGB colour lerp)
 *   border-color               → borderColor                  (ARGB colour lerp)
 *   border-top-color           → borderTopColor               (ARGB colour lerp)
 *   border-right-color         → borderRightColor             (ARGB colour lerp)
 *   border-bottom-color        → borderBottomColor            (ARGB colour lerp)
 *   border-left-color          → borderLeftColor              (ARGB colour lerp)
 *   color                      → color                        (ARGB colour lerp)
 *   shadow-color               → shadowColor                  (ARGB colour lerp)
 *   opacity                    → opacity                      (float [0..1])
 *   border-width               → borderWidth                  (float px)
 *   border-top-width           → borderTopWidth               (length px)
 *   border-right-width         → borderRightWidth             (length px)
 *   border-bottom-width        → borderBottomWidth            (length px)
 *   border-left-width          → borderLeftWidth              (length px)
 *   border-radius              → borderRadius                 (length px)
 *   border-top-left-radius     → borderTopLeftRadius          (length px)
 *   border-top-right-radius    → borderTopRightRadius         (length px)
 *   border-bottom-right-radius → borderBottomRightRadius      (length px)
 *   border-bottom-left-radius  → borderBottomLeftRadius       (length px)
 *   shadow-offset-x            → shadowOffsetX                (float px)
 *   shadow-offset-y            → shadowOffsetY                (float px)
 *   shadow-blur                → shadowBlur                   (float px)
 *   font-size                  → fontSize                     (length px) *   line-height                -> lineHeight                   (float multiplier, re-flows label sizing) *   flex-grow                  → flexGrow                     (float)
 *   width                      → width                        (length px — re-flows layout via pre-pass tickTransitionsAll)
 *   height                     → height                       (length px — re-flows layout via pre-pass tickTransitionsAll)
 *   gap                        → gap                          (length px)
 *   padding-top                → paddingTop                   (float px)
 *   padding-right              → paddingRight                 (float px)
 *   padding-bottom             → paddingBottom                (float px)
 *   padding-left               → paddingLeft                  (float px)
 *   margin-top                 → marginTop                    (float px)
 *   margin-right               → marginRight                  (float px)
 *   margin-bottom              → marginBottom                 (float px)
 *   margin-left                → marginLeft                   (float px)
 *   top                        → top                          (length px)
 *   left                       → left                         (length px)
 *   right                      → right                        (length px)
 *   bottom                     → bottom                       (length px)
 *   scrollbar-thumb-color      → scrollbarThumbColor          (ARGB colour lerp)
 *   scrollbar-track-color      → scrollbarTrackColor          (ARGB colour lerp)
 *   transform                  → transform                    (numeric-arg lerp: "rotate(0deg)"→"rotate(90deg)")
 *   filter                     → filter                       (numeric-arg lerp: "blur(0px)"→"blur(8px)")
 *
 * Easing functions:
 *   linear, ease (default), ease-in, ease-out, ease-in-out, step-start, step-end
 *
 * Reversing transitions (browser behaviour):
 *   If a property changes while it is already mid-flight, the new "from" value is
 *   the current in-flight computed value (not the original start). Duration resets
 *   to the full declared duration. This makes rapid hover-in/out feel fluid.
 *
 * Notes:
 *   - Layout reads childPrefH/W from computedStyle (not style). A pre-pass
 *     tickTransitionsAll() runs before Layout() each frame so animated width/height
 *     values are already in computedStyle when Layout needs them.
 *   - Percentage lengths for `width`, `height`, `left`, `right`, `top`, `bottom`, and `gap`
 *     animate correctly: `lerpLen` interpolates the numeric part and preserves the `%` suffix
 *     so `computedStyle` holds e.g. `"55%"` mid-animation, which `Layout()` resolves against
 *     the parent content width each frame. Other length properties (e.g. `borderRadius`)
 *     still require explicit px values.
 *   - Included automatically by glint_element.hpp; no need to include separately.
 */

#include "glint_style.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ── Easing functions ──────────────────────────────────────────────────────────

enum class glint_easing
{
    Linear,
    Ease,
    EaseIn,
    EaseOut,
    EaseInOut,
    StepStart,
    StepEnd,
};

/**
 * Evaluate easing function at parameter t ∈ [0, 1] → [0, 1].
 * Matches the corresponding CSS timing function curves.
 */
inline float glint_ease_eval(glint_easing e, float t)
{
    t = std::max(0.f, std::min(1.f, t));
    switch (e)
    {
        case glint_easing::Linear:    return t;
        case glint_easing::EaseIn:    return t * t * t;                           // cubic-in
        case glint_easing::EaseOut:   { float u = 1.f - t; return 1.f - u*u*u; } // cubic-out
        case glint_easing::StepStart: return t > 0.f  ? 1.f : 0.f;
        case glint_easing::StepEnd:   return t >= 1.f ? 1.f : 0.f;
        case glint_easing::Ease:
        case glint_easing::EaseInOut: // smoothstep — matches CSS ease-in-out well
        default:                      return t * t * (3.f - 2.f * t);
    }
}

// ── Transition specs (parsed from style.transition) ───────────────────────────

/** One entry from the parsed `style.transition` string. */
struct glint_transition_spec
{
    std::string   key;          // dash-case property name, e.g. "background-color"
    float         durationMs;   // duration in milliseconds
    glint_easing  easing;
};

/** One active, in-flight interpolation. */
struct glint_transition_entry
{
    std::string   key;
    std::string   fromVal;      // serialised value at transition-start time
    std::string   toVal;        // serialised target (read from style at change time)
    float         durationMs;
    float         elapsedMs = 0.f;
    glint_easing  easing;
};

// ── CSS property name passthrough ─────────────────────────────────────────────

/** Internal keys now use the same dash-case as CSS (e.g. "background-color").
 *  This function is kept for compatibility but is now a no-op. */
inline std::string glint_transition_camel(const std::string& key)
{
    return key;
}

// ── Animatable property names (dash-case) ───────────────────────────────────
// Shared by glint_parse_transition (for 'all' expansion) and tickTransitions().
inline const std::vector<std::string>& glint_animatable_keys()
{
    static const std::vector<std::string> keys = {
        // Colors
        "background-color", "border-color", "color", "shadow-color",
        "scrollbar-thumb-color", "scrollbar-track-color",
        "selection-color",
        "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
        "stroke",
        // Numeric floats
        "opacity", "border-width", "shadow-offset-x", "shadow-offset-y",
        "shadow-blur", "flex-grow", "line-height", "stroke-dashoffset",
        "stroke-width", "stroke-opacity", "stroke-miterlimit",
        // Lengths (px)
        "border-radius", "font-size", "width", "height", "gap",
        "min-width", "max-width", "min-height", "max-height",
        "left", "top", "right", "bottom",
        "border-top-left-radius", "border-top-right-radius",
        "border-bottom-right-radius", "border-bottom-left-radius",
        "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
        "vertical-align",
        // Padding / margin sides (stored as plain float in SKEdgeInsets)
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "margin-top",  "margin-right",  "margin-bottom",  "margin-left",
        // String-arg lerp
        "transform", "filter", "backdrop-filter", "stroke-dasharray",
        // Mask
        "mask", "mask-mode", "mask-position", "mask-size",
        "mask-repeat", "mask-origin", "mask-clip", "mask-composite",
    };
    return keys;
}

// ── Parse `style.transition` string ──────────────────────────────────────────
// Accepts CSS format: "background-color 300ms ease-out, opacity 200ms"
// A missing property name defaults to "all" (CSS spec behaviour: transition: 0.2s).
// Delay (4th time-token) is parsed but discarded.

inline std::vector<glint_transition_spec> glint_parse_transition(const std::string& css)
{
    std::vector<glint_transition_spec> specs;
    if (css.empty() || css == "none") return specs;

    // Split on commas to get individual property specs.
    std::vector<std::string> parts;
    std::string buf;
    for (const char c : css)
    {
        if (c == ',') { parts.push_back(buf); buf.clear(); }
        else           buf += c;
    }
    parts.push_back(buf);

    for (const auto& part : parts)
    {
        std::istringstream ss(part);
        std::string tok;
        glint_transition_spec spec;
        spec.durationMs = 300.f;
        spec.easing     = glint_easing::Ease;
        bool hasKey     = false;
        bool hasMs      = false;  // distinguishes duration from delay

        while (ss >> tok)
        {
            // ── Time tokens: "300ms" or "0.3s" ──
            const bool isMs = tok.size() > 2 && tok.substr(tok.size() - 2) == "ms";
            const bool isS  = !isMs && tok.size() > 1 && tok.back() == 's'
                              && (std::isdigit(static_cast<unsigned char>(tok.front()))
                                  || tok.front() == '.');
            const bool firstIsNumeric = !tok.empty() &&
                              (std::isdigit(static_cast<unsigned char>(tok.front()))
                               || tok.front() == '.' || tok.front() == '-' || tok.front() == '+');
            if ((isMs || isS) && firstIsNumeric)
            {
                if (!hasMs)  // first time-token = duration; second = delay (ignored)
                {
                    try
                    {
                        spec.durationMs = isMs ? std::stof(tok)
                                                : std::stof(tok) * 1000.f;
                    }
                    catch (...) {}
                    hasMs = true;
                }
                continue;
            }

            // ── Easing keywords (check ease-in-out before ease-in/ease-out) ──
            if (tok == "linear")      { spec.easing = glint_easing::Linear;    continue; }
            if (tok == "ease-in-out") { spec.easing = glint_easing::EaseInOut; continue; }
            if (tok == "ease-in")     { spec.easing = glint_easing::EaseIn;    continue; }
            if (tok == "ease-out")    { spec.easing = glint_easing::EaseOut;   continue; }
            if (tok == "ease")        { spec.easing = glint_easing::Ease;      continue; }
            if (tok == "step-start")  { spec.easing = glint_easing::StepStart; continue; }
            if (tok == "step-end")    { spec.easing = glint_easing::StepEnd;   continue; }

            // ── Property name (first unrecognised token) ──
            if (!hasKey)
            {
                spec.key = glint_transition_camel(tok);
                hasKey   = true;
            }
        }

        // CSS default: missing property name means "all"
        if (!hasKey) { spec.key = "all"; hasKey = true; }

        if (hasKey && spec.durationMs > 0.f)
            specs.push_back(spec);
    }

    return specs;
}

// ── Serialise one style property → string ─────────────────────────────────────
// Used for change detection and for capturing the "from" value at transition start.
// Returns "" for unknown / non-animatable property names.

inline std::string glint_style_get_by_name(const glint_style& s, const std::string& key)
{
    auto cc = [](const sk_color& c) -> std::string {
        char b[10];
        std::snprintf(b, sizeof(b), "#%02x%02x%02x%02x",
                      c.value.R, c.value.G, c.value.B, c.value.A);
        return b;
    };
    auto cco = [&cc](const glint_optional_color& c) -> std::string {
        if (!c.isSet) return "";
        return cc(sk_color(c.value));
    };
    auto ff = [](float f) -> std::string {
        char b[32]; std::snprintf(b, sizeof(b), "%g", f); return b;
    };
    auto ll = [](const glint_length& l) -> std::string {
        // Return raw verbatim — empty string means "unset", which is the correct
        // sentinel for _IW_L (style.f.raw != sD.f.raw).  Returning "0" for an
        // empty raw caused glint_default_style_info()["top"] == "0", so anything
        // that roundtripped through the serialised default (e.g. inspector's
        // "disable inline property") wrote "0" to style.top instead of "", making
        // _IW_L treat itas an explicit inline override that won over CSS values.
        return l.raw;
    };
    // Read a side proxy: return the raw string if it carries a unit suffix
    // (e.g. "50%", "10px"), otherwise fall back to the float representation.
    auto sp = [&ff](const sk_side_proxy& p) -> std::string {
        if (p._rawp && !p._rawp->empty()) return *p._rawp;
        return ff(static_cast<float>(p));
    };

    if (key == "background-color")          return cc(s.backgroundColor);
    if (key == "border-color")              return cc(s.borderColor);
    if (key == "color")                     return cc(s.color);
    if (key == "shadow-color")              return cc(s.shadowColor);
    if (key == "scrollbar-thumb-color")     return cc(s.scrollbarThumbColor);
    if (key == "scrollbar-track-color")     return cc(s.scrollbarTrackColor);
    if (key == "selection-color")           return cc(s.selectionColor);
    if (key == "border-top-color")          return cco(s.borderTopColor);
    if (key == "border-right-color")        return cco(s.borderRightColor);
    if (key == "border-bottom-color")       return cco(s.borderBottomColor);
    if (key == "border-left-color")         return cco(s.borderLeftColor);
    if (key == "stroke")                    return cco(s.strokeColor);
    if (key == "fill")                      return cco(s.fill);
    if (key == "opacity")                   return ff(s.opacity);
    if (key == "stroke-dashoffset")         return ff(s.strokeDashoffset);
    if (key == "stroke-width")              return ff(s.strokeWidth);
    if (key == "stroke-opacity")            return ff(s.strokeOpacity);
    if (key == "stroke-miterlimit")         return ff(s.strokeMiterlimit);
    if (key == "border-width")              return ff(s.borderWidth);
    if (key == "shadow-offset-x")           return ff(s.shadowOffsetX);
    if (key == "shadow-offset-y")           return ff(s.shadowOffsetY);
    if (key == "shadow-blur")               return ff(s.shadowBlur);
    if (key == "flex-grow")                 return ff(s.flexGrow);
    if (key == "padding-top")               return sp(s.paddingTop);
    if (key == "padding-right")             return sp(s.paddingRight);
    if (key == "padding-bottom")            return sp(s.paddingBottom);
    if (key == "padding-left")              return sp(s.paddingLeft);
    if (key == "margin-top")                return sp(s.marginTop);
    if (key == "margin-right")              return sp(s.marginRight);
    if (key == "margin-bottom")             return sp(s.marginBottom);
    if (key == "margin-left")               return sp(s.marginLeft);
    if (key == "border-radius")             return ll(s.borderRadius);
    if (key == "font-size")                 return ll(s.fontSize);
    if (key == "line-height")               return ff(s.lineHeight);
    if (key == "vertical-align")            return s.verticalAlign;
    if (key == "width")                     return ll(s.width);
    if (key == "height")                    return ll(s.height);    if (key == "min-width")                  return ll(s.minWidth);
    if (key == "max-width")                  return ll(s.maxWidth);
    if (key == "min-height")                 return ll(s.minHeight);
    if (key == "max-height")                 return ll(s.maxHeight);    if (key == "gap")                       return ll(s.gap);
    if (key == "left")                      return ll(s.left);
    if (key == "top")                       return ll(s.top);
    if (key == "right")                     return ll(s.right);
    if (key == "bottom")                    return ll(s.bottom);
    if (key == "border-top-left-radius")     return ll(s.borderTopLeftRadius);
    if (key == "border-top-right-radius")    return ll(s.borderTopRightRadius);
    if (key == "border-bottom-right-radius") return ll(s.borderBottomRightRadius);
    if (key == "border-bottom-left-radius")  return ll(s.borderBottomLeftRadius);
    if (key == "border-top-width")           return ll(s.borderTopWidth);
    if (key == "border-right-width")         return ll(s.borderRightWidth);
    if (key == "border-bottom-width")        return ll(s.borderBottomWidth);
    if (key == "border-left-width")          return ll(s.borderLeftWidth);
    if (key == "transform")                  return s.transform;
    if (key == "filter")                     return s.filter;
    if (key == "backdrop-filter")            return s.backdropFilter;
    if (key == "mask")                       return s.mask;
    if (key == "mask-mode")                  return s.maskMode;
    if (key == "mask-position")              return s.maskPosition;
    if (key == "mask-size")                  return s.maskSize;
    if (key == "mask-repeat")                return s.maskRepeat;
    if (key == "mask-origin")                return s.maskOrigin;
    if (key == "mask-clip")                  return s.maskClip;
    if (key == "mask-composite")             return s.maskComposite;
    if (key == "stroke-dasharray")          return s.strokeDasharray;

    return "";  // not animatable
}

// ── Lerp and write one property into a target style ──────────────────────────
// t ∈ [0, 1]: 0 → fromVal, 1 → toVal.
// Unknown keys are silently ignored (harmless no-op).

inline void glint_style_lerp_by_name(glint_style&       target,
                                      const std::string& key,
                                      const std::string& from,
                                      const std::string& to,
                                      float              t)
{
    // ── Colour lerp (per-channel ARGB) ──────────────────────────────────────
    auto parseColor = [](const std::string& s) -> glint_color {
        if (s.size() >= 7 && s[0] == '#')
        {
            unsigned r = 0, g = 0, b = 0, a = 255;
            if (s.size() == 9)
                std::sscanf(s.c_str() + 1, "%02x%02x%02x%02x", &r, &g, &b, &a);
            else
                std::sscanf(s.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
            return glint_color(static_cast<int>(a), static_cast<int>(r),
                          static_cast<int>(g), static_cast<int>(b));
        }
        return glint_color(255, 0, 0, 0);
    };
    auto lerpCh = [](int a, int b_, float t_) -> int {
        return std::max(0, std::min(255, static_cast<int>(std::round(a + (b_ - a) * t_))));
    };
    auto lerpColor = [&](const std::string& f, const std::string& tt) -> glint_color {
        const glint_color a = parseColor(f), b = parseColor(tt);
        return glint_color(lerpCh(a.A, b.A, t), lerpCh(a.R, b.R, t),
                      lerpCh(a.G, b.G, t), lerpCh(a.B, b.B, t));
    };

    // ── Numeric parse helpers (no-throw, strtof-based) ──────────────────────
    auto safeStof = [](const std::string& s) -> float {
        if (s.empty()) return 0.f;
        const char* p = s.c_str();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        return (end != p) ? v : 0.f;
    };
    auto safeStofValid = [](const std::string& s) -> bool {
        if (s.empty()) return false;
        const char* p = s.c_str();
        char* end = nullptr;
        (void) std::strtof(p, &end);
        return end != p;
    };

    // ── Float lerp ──────────────────────────────────────────────────────────
    auto lerpF = [&](const std::string& f, const std::string& tt) -> float {
        return safeStof(f) + (safeStof(tt) - safeStof(f)) * t;
    };

    // ── Length lerp: lerps the numeric part, preserving the % suffix if present.
    // Both px ("44", "44px") and percent ("10%", "100%") are supported.
    // Mixed units (one % / one px) are uncommon — the % side determines the unit.
    auto lerpLen = [&](const std::string& f, const std::string& tt) -> std::string {
        const bool fIsPct = !f.empty()  && f.back()  == '%';
        const bool tIsPct = !tt.empty() && tt.back() == '%';
        float fv = safeStof(f);
        float tv = safeStof(tt);
        const float v = fv + (tv - fv) * t;
        char buf[32]; std::snprintf(buf, sizeof(buf), "%g", v);
        if (fIsPct || tIsPct) return std::string(buf) + "%";
        return buf;
    };
    auto isLenLike = [&safeStofValid](const std::string& s) -> bool {
        if (s.empty()) return false;
        const std::string low = [&]() {
            std::string t = s;
            std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            return t;
        }();
        if (low.size() > 2 && low.substr(low.size() - 2) == "px")
            return safeStofValid(low.substr(0, low.size() - 2));
        if (low.back() == '%')
            return safeStofValid(low.substr(0, low.size() - 1));
        return safeStofValid(low);
    };

    // ── Dispatch ────────────────────────────────────────────────────────────
    if      (key == "background-color")         target.backgroundColor    = lerpColor(from, to);
    else if (key == "border-color")              target.borderColor         = lerpColor(from, to);
    else if (key == "color")                     target.color               = lerpColor(from, to);
    else if (key == "shadow-color")              target.shadowColor         = lerpColor(from, to);
    else if (key == "scrollbar-thumb-color")     target.scrollbarThumbColor = lerpColor(from, to);
    else if (key == "scrollbar-track-color")     target.scrollbarTrackColor = lerpColor(from, to);
    else if (key == "selection-color")           target.selectionColor      = lerpColor(from, to);
    else if (key == "border-top-color")    target.borderTopColor    = lerpColor(from, to);
    else if (key == "border-right-color")  target.borderRightColor  = lerpColor(from, to);
    else if (key == "border-bottom-color") target.borderBottomColor = lerpColor(from, to);
    else if (key == "border-left-color")   target.borderLeftColor   = lerpColor(from, to);
    else if (key == "stroke")              { target.strokeColor = lerpColor(from, to); }
    else if (key == "fill")               { target.fill        = lerpColor(from, to); }
    else if (key == "opacity")              target.opacity          = std::max(0.f, std::min(1.f, lerpF(from, to)));
    else if (key == "stroke-dashoffset")    target.strokeDashoffset  = lerpF(from, to);
    else if (key == "stroke-width")         target.strokeWidth       = std::max(0.f, lerpF(from, to));
    else if (key == "stroke-opacity")       target.strokeOpacity     = std::max(0.f, std::min(1.f, lerpF(from, to)));
    else if (key == "stroke-miterlimit")    target.strokeMiterlimit  = std::max(1.f, lerpF(from, to));
    else if (key == "border-width")         target.borderWidth      = std::max(0.f, lerpF(from, to));
    else if (key == "shadow-offset-x")      target.shadowOffsetX = lerpF(from, to);
    else if (key == "shadow-offset-y")      target.shadowOffsetY = lerpF(from, to);
    else if (key == "shadow-blur")          target.shadowBlur    = std::max(0.f, lerpF(from, to));
    else if (key == "flex-grow")            target.flexGrow      = std::max(0.f, lerpF(from, to));
    // Padding sides (via sk_side_proxy, which writes the private SKEdgeInsets float)
    else if (key == "padding-top")          target.paddingTop    = std::max(0.f, lerpF(from, to));
    else if (key == "padding-right")        target.paddingRight  = std::max(0.f, lerpF(from, to));
    else if (key == "padding-bottom")       target.paddingBottom = std::max(0.f, lerpF(from, to));
    else if (key == "padding-left")         target.paddingLeft   = std::max(0.f, lerpF(from, to));
    // Margin sides (via sk_side_proxy)
    else if (key == "margin-top")           target.marginTop    = lerpF(from, to);
    else if (key == "margin-right")         target.marginRight  = lerpF(from, to);
    else if (key == "margin-bottom")        target.marginBottom = lerpF(from, to);
    else if (key == "margin-left")          target.marginLeft   = lerpF(from, to);
    // Lengths
    else if (key == "border-radius")               target.borderRadius             = lerpLen(from, to);
    else if (key == "font-size")                   target.fontSize                 = lerpLen(from, to);
    else if (key == "line-height")                 target.lineHeight               = std::max(0.1f, lerpF(from, to));
    else if (key == "vertical-align")              target.verticalAlign            = (isLenLike(from) && isLenLike(to)) ? lerpLen(from, to)
                                                                                                                    : (t < 0.5f ? from : to);
    else if (key == "width")                       target.width                    = lerpLen(from, to);
    else if (key == "height")                      target.height                   = lerpLen(from, to);
    else if (key == "min-width")                   target.minWidth                 = lerpLen(from, to);
    else if (key == "max-width")                   target.maxWidth                 = lerpLen(from, to);
    else if (key == "min-height")                  target.minHeight                = lerpLen(from, to);
    else if (key == "max-height")                  target.maxHeight                = lerpLen(from, to);
    else if (key == "gap")                         target.gap                      = lerpLen(from, to);
    else if (key == "left")                        target.left                     = lerpLen(from, to);
    else if (key == "top")                         target.top                      = lerpLen(from, to);
    else if (key == "right")                       target.right                    = lerpLen(from, to);
    else if (key == "bottom")                      target.bottom                   = lerpLen(from, to);
    else if (key == "border-top-left-radius")      target.borderTopLeftRadius      = lerpLen(from, to);
    else if (key == "border-top-right-radius")     target.borderTopRightRadius     = lerpLen(from, to);
    else if (key == "border-bottom-right-radius")  target.borderBottomRightRadius  = lerpLen(from, to);
    else if (key == "border-bottom-left-radius")   target.borderBottomLeftRadius   = lerpLen(from, to);
    else if (key == "border-top-width")            target.borderTopWidth           = lerpLen(from, to);
    else if (key == "border-right-width")          target.borderRightWidth         = lerpLen(from, to);
    else if (key == "border-bottom-width")         target.borderBottomWidth        = lerpLen(from, to);
    else if (key == "border-left-width")           target.borderLeftWidth          = lerpLen(from, to);
    else if (key == "transform" || key == "filter" || key == "backdrop-filter" ||
             key == "stroke-dasharray" ||
             key == "mask" || key == "mask-mode" || key == "mask-position" ||
             key == "mask-size" || key == "mask-repeat" || key == "mask-origin" ||
             key == "mask-clip" || key == "mask-composite")
    {
        // Lerp numeric arguments inside CSS function strings, e.g.
        // "rotate(0deg)"→"rotate(90deg)", "blur(0px)"→"blur(8px)".
        // Preserves function names and unit suffixes; only the numbers move.
        struct NumToken { size_t pos, len; float val; };
        auto scanNums = [](const std::string& s) -> std::vector<NumToken> {
            std::vector<NumToken> out;
            size_t i = 0, n = s.size();
            while (i < n) {
                bool neg = s[i] == '-' && i+1 < n &&
                           (std::isdigit(static_cast<unsigned char>(s[i+1])) || s[i+1] == '.');
                bool pos = std::isdigit(static_cast<unsigned char>(s[i])) ||
                           (s[i] == '.' && i+1 < n && std::isdigit(static_cast<unsigned char>(s[i+1])));
                if (neg || pos) {
                    size_t start = i;
                    if (s[i] == '-') ++i;
                    while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                    if (i < n && s[i] == '.') { ++i; while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i; }
                    float val = 0.f; try { val = std::stof(s.substr(start, i-start)); } catch (...) {}
                    out.push_back({start, i-start, val});
                    // skip unit suffix (letters / %)
                    while (i < n && (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '%')) ++i;
                } else { ++i; }
            }
            return out;
        };
        // Returns true when the number at `numPos` inside `src` is an argument
        // of a scale/scaleX/scaleY function (identity value = 1, not 0).
        auto isScaleFn = [](const std::string& src, size_t numPos) -> bool {
            size_t parenPos = src.rfind('(', numPos);
            if (parenPos == std::string::npos) return false;
            size_t nameEnd = parenPos, nameStart = nameEnd;
            while (nameStart > 0 &&
                   (std::isalpha(static_cast<unsigned char>(src[nameStart-1])) ||
                    src[nameStart-1] == '-'))
                --nameStart;
            std::string nm = src.substr(nameStart, nameEnd - nameStart);
            for (char& c : nm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return nm == "scale" || nm == "scalex" || nm == "scaley";
        };
        // Build the CSS identity transform from a template string:
        // scale/scaleX/scaleY args → 1, all other function args → 0.
        auto identityOf = [&](const std::string& tmpl) -> std::string {
            auto nums = scanNums(tmpl);
            if (nums.empty()) return tmpl;
            std::string z = tmpl;
            for (int i = (int)nums.size()-1; i >= 0; --i) {
                float iv = isScaleFn(tmpl, nums[i].pos) ? 1.f : 0.f;
                char buf[16]; std::snprintf(buf, sizeof(buf), "%g", iv);
                z.replace(nums[i].pos, nums[i].len, buf);
            }
            return z;
        };
        // When from is empty/none, synthesise identity of `to` (e.g. scale(1)).
        // When to   is empty/none, synthesise identity of `from` so we can lerp back.
        auto effectiveFrom = [&](const std::string& f, const std::string& tt) -> std::string {
            if (!f.empty() && f != "none") return f;
            return identityOf(tt);
        };
        auto effectiveTo = [&](const std::string& f, const std::string& tt) -> std::string {
            if (!tt.empty() && tt != "none") return tt;
            return identityOf(f);
        };
        const std::string f2 = effectiveFrom(from, to);
        const std::string t2 = effectiveTo(from, to);
        auto fn = scanNums(f2), tn = scanNums(t2);
        std::string result;
        if (fn.size() == tn.size() && !fn.empty()) {
            size_t prev = 0;
            for (size_t i = 0; i < fn.size(); ++i) {
                result += f2.substr(prev, fn[i].pos - prev);
                float lv = fn[i].val + (tn[i].val - fn[i].val) * t;
                char buf[32]; std::snprintf(buf, sizeof(buf), "%g", lv);
                result += buf;
                prev = fn[i].pos + fn[i].len;
            }
            result += f2.substr(prev);
        } else {
            result = (t < 0.5f ? f2 : t2);
        }
        if (key == "transform")              target.transform        = result;
        else if (key == "filter")             target.filter           = result;
        else if (key == "stroke-dasharray")   target.strokeDasharray  = result;
        else                                  target.backdropFilter   = result;
    }
}

// ── CSS @keyframes rule representation ────────────────────────────────────────────────

/** One keyframe stop (e.g. "from", "to", "50%").
 *  `properties` maps CSS dash-case property names to their serialised values as
 *  produced by glint_style_get_by_name() — ready for glint_style_lerp_by_name(). */
struct glint_keyframe_stop
{
    float offset = 0.f;  // 0.0 = from/0%, 1.0 = to/100%
    std::unordered_map<std::string, std::string> properties;
};

/** One resolved @keyframes rule with all stops sorted ascending by offset. */
struct glint_keyframe_rule
{
    std::string                   name;
    std::vector<glint_keyframe_stop> stops;
};

/** Document-level registry: animation-name → resolved @keyframes rule.
 *  Built by glint_document::_processKeyframes() from loaded stylesheets.
 *  Referenced by elements via mKeyframeRegistryPtr_. */
using glint_keyframe_registry = std::unordered_map<std::string, glint_keyframe_rule>;

// ── CSS animation property types ────────────────────────────────────────────────────

/** One parsed entry from the CSS `animation:` shorthand.
 *
 * CSS spec order: name duration timing-function delay iteration-count direction fill-mode
 *   e.g.  "spin 1s linear infinite"
 *         "fade 300ms ease-out 0s 1 normal both"
 */
struct glint_animation_spec
{
    std::string   name;                                 // @keyframes rule name
    float         durationMs    = 0.f;                  // animation-duration (ms)
    float         delayMs       = 0.f;                  // animation-delay (ms)
    glint_easing   easing        = glint_easing::Ease;    // animation-timing-function
    float         iterCount     = 1.f;                  // animation-iteration-count (inf = "infinite")
    bool          alternate     = false;                // direction: alternate | alternate-reverse
    bool          reverse       = false;                // direction: reverse | alternate-reverse
    bool          fillForwards  = false;                // fill-mode: forwards | both
    bool          fillBackwards = false;                // fill-mode: backwards | both
};

/** Runtime state for one in-flight CSS animation on an element. */
struct glint_animation_entry
{
    glint_animation_spec spec;
    float elapsedMs  = 0.f;    // total elapsed ms after delay expiry
    float delayLeft  = 0.f;    // remaining delay ms
    bool  started    = false;  // true once delay has expired and first frame played
    bool  finished   = false;  // true when all iterations complete
};

// ── Parse `animation:` shorthand string ──────────────────────────────────────────────
// Comma-separated per animation.  Each animation:
//   "name duration timing-function delay iteration-count direction fill-mode"
//   Unrecognised identifier tokens that aren’t easing/direction/fill keywords
//   are treated as the animation name; the first time-value is duration,
//   the second is delay — matching the CSS spec.

inline std::vector<glint_animation_spec> glint_parse_animation(const std::string& css)
{
    std::vector<glint_animation_spec> specs;
    if (css.empty() || css == "none") return specs;

    // Split on commas
    std::vector<std::string> parts;
    {
        std::string buf;
        for (const char c : css)
        {
            if (c == ',') { parts.push_back(buf); buf.clear(); }
            else           buf += c;
        }
        parts.push_back(buf);
    }

    auto toLow = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    for (const auto& part : parts)
    {
        std::istringstream ss(part);
        std::string tok;
        glint_animation_spec spec;
        int timeTokensSeen = 0;

        while (ss >> tok)
        {
            const std::string low = toLow(tok);

            // ── Time tokens: "300ms" or "0.3s" ──────────────────────────────
            const bool isMs = tok.size() > 2 && low.substr(low.size() - 2) == "ms";
            const bool isS  = !isMs && tok.size() > 1 && low.back() == 's'
                              && (std::isdigit(static_cast<unsigned char>(tok.front()))
                                  || tok.front() == '.' || tok.front() == '-');
            if (isMs || isS)
            {
                float val = 0.f;
                try { val = isMs ? std::stof(tok) : std::stof(tok) * 1000.f; } catch (...) {}
                if (timeTokensSeen == 0) spec.durationMs = val;  // first  = duration
                else                     spec.delayMs    = val;  // second = delay
                ++timeTokensSeen;
                continue;
            }

            // ── Iteration count keywords ──────────────────────────────────────
            if (low == "infinite") { spec.iterCount = std::numeric_limits<float>::infinity(); continue; }

            // ── Easing keywords ────────────────────────────────────────────
            if (low == "linear")       { spec.easing = glint_easing::Linear;    continue; }
            if (low == "ease-in-out") { spec.easing = glint_easing::EaseInOut; continue; }
            if (low == "ease-in")     { spec.easing = glint_easing::EaseIn;    continue; }
            if (low == "ease-out")    { spec.easing = glint_easing::EaseOut;   continue; }
            if (low == "ease")        { spec.easing = glint_easing::Ease;      continue; }
            if (low == "step-start")  { spec.easing = glint_easing::StepStart; continue; }
            if (low == "step-end")    { spec.easing = glint_easing::StepEnd;   continue; }

            // ── Direction ──────────────────────────────────────────────────────
            if (low == "alternate-reverse") { spec.alternate = true;  spec.reverse = true;  continue; }
            if (low == "alternate")         { spec.alternate = true;  spec.reverse = false; continue; }
            if (low == "reverse")           { spec.alternate = false; spec.reverse = true;  continue; }
            if (low == "normal")            { spec.alternate = false; spec.reverse = false; continue; }

            // ── Fill mode ────────────────────────────────────────────────────
            if (low == "forwards")  { spec.fillForwards = true;                          continue; }
            if (low == "backwards") { spec.fillBackwards = true;                         continue; }
            if (low == "both")      { spec.fillForwards = true; spec.fillBackwards = true; continue; }

            // ── Numeric iteration count ─────────────────────────────────────────
            if (std::isdigit(static_cast<unsigned char>(tok.front())) || tok.front() == '.')
            {
                try { spec.iterCount = std::stof(tok); } catch (...) {}
                continue;
            }

            // ── Anything else is the animation name (first unrecognised token) ───
            if (spec.name.empty()) spec.name = tok;
        }

        if (!spec.name.empty() && spec.durationMs > 0.f)
            specs.push_back(spec);
    }

    return specs;
}

// ── Apply one @keyframes animation frame to a target style ───────────────────────
//
// Looks up `animName` in `registry`, finds the two bracketing stops at
// normalised t ∈ [0,1], and writes interpolated values to `target` via
// glint_style_lerp_by_name.  The per-stop local t is further eased by `easing`.
//
// Browser cascade note: transitions override animations for the same property.
// Callers should apply animations BEFORE transitions each frame so transitions
// can overwrite on top (matching the CSS Cascade Level 5 origin order).
//
// No-op if `animName` is not found in the registry.

inline void glint_keyframe_apply(const glint_keyframe_registry& registry,
                                const std::string&             animName,
                                float                          t,
                                glint_easing                    easing,
                                glint_style&                    target)
{
    const auto it = registry.find(animName);
    if (it == registry.end() || it->second.stops.empty()) return;

    const auto& rule = it->second;
    t = std::max(0.f, std::min(1.f, t));

    // Find the two adjacent stops that bracket t.
    const glint_keyframe_stop* lo = &rule.stops.front();
    const glint_keyframe_stop* hi = &rule.stops.back();

    for (size_t i = 0; i + 1 < rule.stops.size(); ++i)
    {
        if (t <= rule.stops[i + 1].offset)
        {
            lo = &rule.stops[i];
            hi = &rule.stops[i + 1];
            break;
        }
    }

    // Local t within the [lo.offset, hi.offset] span, then eased.
    const float span   = hi->offset - lo->offset;
    const float localT = glint_ease_eval(easing,
                            span > 0.f ? ((t - lo->offset) / span) : 1.f);

    // Interpolate every property declared in `hi`.
    // When `lo` does NOT declare the same property, fall back to the current
    // computed value in `target` as the from-value (matches Chrome behaviour).
    for (const auto& kv : hi->properties)
    {
        const std::string& prop  = kv.first;
        const std::string& toVal = kv.second;

        std::string fromVal;
        const auto loIt = lo->properties.find(prop);
        if (loIt != lo->properties.end())
            fromVal = loIt->second;
        else
            fromVal = glint_style_get_by_name(target, prop);

        glint_style_lerp_by_name(target, prop, fromVal, toVal, localT);
    }
}
