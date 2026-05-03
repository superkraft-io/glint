#pragma once

/**
 * glint_tree_node.hpp
 * Data types for the glint scene-graph tree export.
 *
 * glint_tree_node  — recursive node produced by glint_document::getUITree().
 *                    Consumed by glint_tree (the general-purpose tree component)
 *                    and by the inspector window.
 *
 * glint_style_info — flat map<string,string> of every glint_style field, so the
 *                    inspector's CSS-style panel can display and (later) edit them.
 *
 * glint_style_serialize() — converts a live glint_style into an glint_style_info.
 */

#include "../glint_style.hpp"   // glint_style, sk_color, glint_length, glint_color, EAlign, EVAlign

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// ── glint_style_info ─────────────────────────────────────────────────────────
// Flat string-string map representing a serialized glint_style.
// Keys are identical to the CSS property names used in glint_style.
using glint_style_info = std::map<std::string, std::string>;

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace glint_tree_detail {

inline std::string hexColor(const glint_color& c)
{
    char buf[12];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.R, c.G, c.B, c.A);
    return buf;
}

inline std::string ftos(float f)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", f);
    return buf;
}

} // namespace glint_tree_detail

// ── glint_style_serialize ────────────────────────────────────────────────────
// Converts a live glint_style to a flat string map.
// All fields are included so the inspector CSS panel has the full picture.
inline glint_style_info glint_style_serialize(const glint_style& s)
{
    using namespace glint_tree_detail;
    glint_style_info m;

    // ── Color / opacity ──────────────────────────────────────────────────────
    m["color"]            = hexColor(s.color.value);
    m["background-color"] = hexColor(s.backgroundColor.value);
    m["opacity"]          = ftos(s.opacity);

    // ── Gradient background ──────────────────────────────────────────────────
    // Serialised as "<pos>:#rrggbbaa|<pos>:#rrggbbaa|..."  (empty = no gradient).
    if (!s.backgroundGradient.empty())
    {
        std::string gs;
        for (const auto& st : s.backgroundGradient)
        {
            if (!gs.empty()) gs += '|';
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.3f:", st.position);
            gs += buf;
            gs += hexColor(st.color);
        }
        m["background"] = gs;
    }
    else
    {
        m["background"] = "";
    }

    // ── Background image ─────────────────────────────────────────────────────
    m["background-image"]    = s.backgroundImage.empty() ? "" : ("url(\"" + s.backgroundImage + "\")");
    m["background-size"]     = s.backgroundSize;
    m["background-position"] = s.backgroundPosition;
    m["background-repeat"]   = s.backgroundRepeat;
    m["background-gradient-type"]   = s.backgroundGradientType;
    m["background-gradient-angle"]  = ftos(s.backgroundGradientAngle);
    m["background-gradient-cx"]     = ftos(s.backgroundGradientCX);
    m["background-gradient-cy"]     = ftos(s.backgroundGradientCY);
    m["background-gradient-radius"] = ftos(s.backgroundGradientRadius);
    // ── Border ───────────────────────────────────────────────────────────────
    m["border-color"]  = hexColor(s.borderColor.value);
    m["border-width"]  = ftos(s.borderWidth);
    m["border-radius"] = s.borderRadius.raw;
    m["border-style"]  = s.borderStyle;
    m["stroke-dashoffset"] = ftos(s.strokeDashoffset);
    m["fill"]             = s.fill.isSet        ? hexColor(s.fill.value)        : "";
    m["stroke"]           = s.strokeColor.isSet ? hexColor(s.strokeColor.value) : "";
    m["stroke-dasharray"] = s.strokeDasharray;
    m["stroke-linecap"]   = s.strokeLinecap;
    m["stroke-linejoin"]  = s.strokeLinejoin;
    m["stroke-miterlimit"] = ftos(s.strokeMiterlimit);
    m["stroke-opacity"]   = ftos(s.strokeOpacity);
    m["stroke-width"]     = ftos(s.strokeWidth);
    // Per-corner radii (empty raw = inherits from border-radius)
    m["border-top-left-radius"]     = s.borderTopLeftRadius.raw;
    m["border-top-right-radius"]    = s.borderTopRightRadius.raw;
    m["border-bottom-right-radius"] = s.borderBottomRightRadius.raw;
    m["border-bottom-left-radius"]  = s.borderBottomLeftRadius.raw;
    // Per-side widths (-1 = inherits from border-width)
    m["border-top-width"]    = s.borderTopWidth.raw;
    m["border-right-width"]  = s.borderRightWidth.raw;
    m["border-bottom-width"] = s.borderBottomWidth.raw;
    m["border-left-width"]   = s.borderLeftWidth.raw;
    // Per-side colors (only when explicitly set)
    if (s.borderTopColor.isSet)    m["border-top-color"]    = hexColor(s.borderTopColor.value);
    if (s.borderRightColor.isSet)  m["border-right-color"]  = hexColor(s.borderRightColor.value);
    if (s.borderBottomColor.isSet) m["border-bottom-color"] = hexColor(s.borderBottomColor.value);
    if (s.borderLeftColor.isSet)   m["border-left-color"]   = hexColor(s.borderLeftColor.value);
    // Per-side styles (empty = inherits from border-style)
    m["border-top-style"]    = s.borderTopStyle;
    m["border-right-style"]  = s.borderRightStyle;
    m["border-bottom-style"] = s.borderBottomStyle;
    m["border-left-style"]   = s.borderLeftStyle;

    // ── Shadow ───────────────────────────────────────────────────────────────
    m["box-shadow"] = static_cast<std::string>(s.boxShadow);

    // ── Typography ───────────────────────────────────────────────────────────
    m["font-size"]    = s.fontSize.raw;
    m["line-height"]  = ftos(s.lineHeight);
    m["font-family"]  = s.fontFamily;
    m["font-style"]   = s.fontStyle;
    m["font-weight"]  = std::to_string((int)(float)s.fontWeight);
    m["text-align"] =
        s.textAlign == EAlign::Near ? "left"  :
        s.textAlign == EAlign::Far  ? "right" : "center";
    m["vertical-align"] = s.verticalAlign;
    m["text-decoration"] = s.textDecoration;
    m["selection-color"] = hexColor(s.selectionColor.value);

    // ── Padding ──────────────────────────────────────────────────────────────
    // Emit "padding" shorthand when all sides are equal (→ one inspector row);
    // fall back to four per-side keys only when values differ (→ individual rows).
    {
        const std::string pT = ftos(static_cast<float>(s.paddingTop));
        const std::string pR = ftos(static_cast<float>(s.paddingRight));
        const std::string pB = ftos(static_cast<float>(s.paddingBottom));
        const std::string pL = ftos(static_cast<float>(s.paddingLeft));
        if (pT == pR && pT == pB && pT == pL) {
            m["padding"] = pT;
        } else {
            m["padding-top"]    = pT;
            m["padding-right"]  = pR;
            m["padding-bottom"] = pB;
            m["padding-left"]   = pL;
        }
    }

    // ── Margin ───────────────────────────────────────────────────────────────
    // Emit the raw CSS string when a percentage was specified (e.g. "50%"),
    // otherwise fall back to the resolved pixel value as a string.
    {
        auto mStr = [](const sk_side_proxy& p) -> std::string {
            if (p._rawp && !p._rawp->empty()) return *p._rawp;
            return ftos(static_cast<float>(p));
        };
        const std::string mT = mStr(s.marginTop);
        const std::string mR = mStr(s.marginRight);
        const std::string mB = mStr(s.marginBottom);
        const std::string mL = mStr(s.marginLeft);
        if (mT == mR && mT == mB && mT == mL) {
            m["margin"] = mT;
        } else {
            m["margin-top"]    = mT;
            m["margin-right"]  = mR;
            m["margin-bottom"] = mB;
            m["margin-left"]   = mL;
        }
    }

    // ── Position / box ───────────────────────────────────────────────────────
    // builderInjected lengths are suppressed from the serialization so the
    // inspector's element.style {} block only shows developer-authored values.
    // The layout engine reads raw directly from the struct, so layout is unaffected.
    auto emitL = [](const glint_length& l) -> std::string {
      return l.builderInjected ? std::string{} : l.raw;
    };
    m["position"] = s.position;
    m["z-index"]  = std::to_string(s.zIndex);
    m["left"]     = emitL(s.left);
    m["top"]      = emitL(s.top);
    m["right"]    = emitL(s.right);
    m["bottom"]   = emitL(s.bottom);
    m["width"]      = emitL(s.width);
    m["height"]     = emitL(s.height);
    m["min-width"]  = emitL(s.minWidth);
    m["max-width"]  = emitL(s.maxWidth);
    m["min-height"] = emitL(s.minHeight);
    m["max-height"] = emitL(s.maxHeight);

    // ── Flex ─────────────────────────────────────────────────────────────────
    m["display"]          = s.display;
    m["pointer-events"]   = s.pointerEvents;
    m["user-select"]      = s.userSelect;
    m["flex-direction"]   = s.flexDirection;
    m["justify-content"]  = s.justifyContent;
    m["align-items"]      = s.alignItems;
    m["gap"]              = s.gap.raw;
    m["flex-grow"]        = ftos(s.flexGrow);

    // ── Misc ─────────────────────────────────────────────────────────────────
    m["object-fit"]        = s.objectFit;
    m["object-position"]   = s.objectPosition;
    m["transform"]         = s.transform;
    m["overflow-x"]        = s.overflowX;
    m["overflow-y"]        = s.overflowY;
    m["overflow"]          = static_cast<std::string>(s.overflow);
    m["scrollbar-width"]   = ftos(s.scrollbarWidth);
    m["scrollbar-thumb"]   = hexColor(s.scrollbarThumbColor.value);
    m["scrollbar-track"]   = hexColor(s.scrollbarTrackColor.value);
    m["scrollbar-button"]  = hexColor(s.scrollbarButtonColor.value);
    m["filter"]            = s.filter;
    m["backdrop-filter"]   = s.backdropFilter;
    m["mix-blend-mode"]    = s.mixBlendMode;
    m["background-blend-mode"] = s.backgroundBlendMode;
    m["isolation"]         = s.isolation.empty() ? "auto" : s.isolation;
    m["mask"]              = s.mask;
    m["mask-mode"]         = s.maskMode;
    m["mask-position"]     = s.maskPosition;
    m["mask-size"]         = s.maskSize;
    m["mask-repeat"]       = s.maskRepeat;
    m["mask-origin"]       = s.maskOrigin;
    m["mask-clip"]         = s.maskClip;
    m["mask-composite"]    = s.maskComposite;
    m["transition"]        = s.transition;

    return m;
}

// ── glint_tree_node ──────────────────────────────────────────────────────────
// Represents one node in the exported component tree.
// Produced by glint_document::getUITree() — a snapshot, not a live reference.
struct glint_tree_node
{
    uint64_t         id        = 0;
    std::string      typeName;            // from glint_element::tagName() — HTML tag for display
    std::string      elementId;           // from glint_element::element.id (may be empty)
    std::string      className;           // from glint_element::className (may be empty)
    std::string      innerText;           // from glint_element::innerText (may be empty)
    glint_rect            rect      {};         // from GetPaintRECT()
    glint_style_info styleInfo;           // serialized style fields
    std::vector<glint_tree_node> children;
};
