#pragma once

/**
 * glint_colorpicker.hpp
 * A CSS-inspired color picker component for glint.
 *
 * Collapsed: colored swatch + hex label + chevron. Click to expand.
 * Expanded:  SV (saturation/value) canvas + hue strip + alpha strip + hex input.
 * Changes fire `onChange` in real time as the user drags.
 *
 * Layout note: the component has a fixed collapsed height (kSwatchRowH = 36px)
 * and updates `style.height` when toggled so the parent flex/block layout
 * accommodates it correctly in the same draw frame.
 *
 * Usage via builder:
 *   add.colorpicker([](glint_colorpicker& _c) {
 *     _c.value        = glint_color(255, 80, 140, 200);
 *     _c.style.width  = 240.f;
 *     _c.onChange     = [](glint_color c) { DBGMSG("color: #%02X%02X%02X\n", c.R, c.G, c.B); };
 *   });
 *
 * Or directly from ComponentAdd:
 *   add.colorpicker([](glint_colorpicker& _c) { _c.value = "#3399ff"; ... }, &mPicker);
 */

#include "../glint_element.hpp"
#include "../default_style.hpp"
#include "input/glint_input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/effects/SkGradientShader.h"

// --- Height constants ---------------------------------------------------------
// Layout (top?bottom, all inside padding):
//   paddingTop(8) + sv(140) + gap(6) + hue(16) + gap(6) + alpha(16) +
//   gap(6) + inputRow+modeBtn(32) + paddingBottom(10)  =  240
static constexpr float kCP_SwatchRowH    = 36.f;   // kept for ABI compat; no longer used
static constexpr float kCP_SVCanvasH     = 140.f;
static constexpr float kCP_StripH        = 16.f;
static constexpr float kCP_HexRowH       = 32.f;
static constexpr float kCP_ModeBarH      = 24.f;   // kept for ABI compat; no longer used
static constexpr float kCP_Gap           = 6.f;
static constexpr float kCP_PaddingLR     = 8.f;
static constexpr float kCP_PaddingTop    = 8.f;
static constexpr float kCP_PaddingBot    = 10.f;
static constexpr float kCP_PickerPanelH  =
    kCP_PaddingTop + kCP_SVCanvasH + 3 * kCP_Gap + kCP_StripH * 2
    + kCP_HexRowH + kCP_PaddingBot;
static constexpr float kCP_ExpandedH     = kCP_PickerPanelH;

// --- Color conversion helpers -------------------------------------------------

static inline void sk_cp_rgb_to_hsv(int r, int g, int b, float& h, float& s, float& v)
{
    float fr = r / 255.f, fg = g / 255.f, fb = b / 255.f;
    float maxc = std::max({ fr, fg, fb });
    float minc = std::min({ fr, fg, fb });
    v = maxc;
    float d = maxc - minc;
    s = (maxc > 1e-6f) ? d / maxc : 0.f;
    if (d < 1e-6f) { h = 0.f; return; }
    if      (maxc == fr) h = 60.f * std::fmod((fg - fb) / d, 6.f);
    else if (maxc == fg) h = 60.f * ((fb - fr) / d + 2.f);
    else                 h = 60.f * ((fr - fg) / d + 4.f);
    if (h < 0.f) h += 360.f;
}

static inline glint_color sk_cp_hsv_to_rgb(float h, float s, float v, int a)
{
    h = std::fmod(h, 360.f);
    if (h < 0.f) h += 360.f;
    const float c  = v * s;
    const float x  = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
    const float m  = v - c;
    float r1 = 0.f, g1 = 0.f, b1 = 0.f;
    const int hi = (int)(h / 60.f) % 6;
    switch (hi) {
        case 0: r1 = c; g1 = x; b1 = 0; break;
        case 1: r1 = x; g1 = c; b1 = 0; break;
        case 2: r1 = 0; g1 = c; b1 = x; break;
        case 3: r1 = 0; g1 = x; b1 = c; break;
        case 4: r1 = x; g1 = 0; b1 = c; break;
        default: r1 = c; g1 = 0; b1 = x; break;
    }
    return glint_color(a,
        (int)std::round((r1 + m) * 255.f),
        (int)std::round((g1 + m) * 255.f),
        (int)std::round((b1 + m) * 255.f));
}

static inline std::string sk_cp_to_hex(const glint_color& c)
{
    char buf[16];
    if (c.A == 255)
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X", c.R, c.G, c.B);
    else
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", c.R, c.G, c.B, c.A);
    return buf;
}

// Returns false on parse failure. Accepts RRGGBB or RRGGBBAA (leading '#' optional).
static inline bool sk_cp_parse_hex(const std::string& s, glint_color& out)
{
    const char* p = s.c_str();
    while (*p == '#' || *p == ' ') ++p;
    const size_t len = std::strlen(p);
    if (len != 6 && len != 8) return false;
    unsigned rv = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = p[i];
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        rv = rv * 16 + (unsigned)d;
    }
    if (len == 6)
        out = glint_color(255,  (rv >> 16) & 0xFF, (rv >> 8) & 0xFF,  rv & 0xFF);
    else
        out = glint_color(rv & 0xFF, (rv >> 24) & 0xFF, (rv >> 16) & 0xFF, (rv >> 8) & 0xFF);
    return true;
}

// --- Skia drawing helpers -----------------------------------------------------


static inline SkRect sk_cp_to_sk(const glint_rect& r)
{
    return SkRect::MakeLTRB(r.L, r.T, r.R, r.B);
}

static inline void sk_cp_draw_sv_canvas(SkCanvas* canvas, const glint_rect& r,
                                         float hue, float sat, float val)
{
    if (r.W() <= 0 || r.H() <= 0) return;
    const SkRect sr = sk_cp_to_sk(r);

    // 1 � Horizontal gradient: white (L) ? fully-saturated hue colour (R)
    {
        const glint_color   hc   = sk_cp_hsv_to_rgb(hue, 1.f, 1.f, 255);
        const SkColor  hueC = SkColorSetARGB(255, hc.R, hc.G, hc.B);
        SkPoint  pts[2]    = {{r.L, r.T}, {r.R, r.T}};
        SkColor  cols[2]   = {SK_ColorWHITE, hueC};
        auto shader = SkGradientShader::MakeLinear(pts, cols, nullptr, 2, SkTileMode::kClamp);
        SkPaint p;
        p.setShader(shader);
        canvas->drawRect(sr, p);
    }

    // 2 � Vertical overlay gradient: transparent (T) ? black (B) � darkens toward bottom
    {
        SkPoint  pts[2]  = {{r.L, r.T}, {r.L, r.B}};
        SkColor  cols[2] = {SkColorSetARGB(0, 0, 0, 0), SkColorSetARGB(255, 0, 0, 0)};
        auto shader = SkGradientShader::MakeLinear(pts, cols, nullptr, 2, SkTileMode::kClamp);
        SkPaint p;
        p.setShader(shader);
        canvas->drawRect(sr, p);
    }

    // 3 � Cursor circle at (sat, 1-val) � outline in contrasting colour
    const float cx = r.L + sat * r.W();
    const float cy = r.T + (1.f - val) * r.H();
    {
        SkPaint p;
        p.setStyle(SkPaint::kStroke_Style);
        p.setAntiAlias(true);
        p.setStrokeWidth(2.f);
        p.setColor(val > 0.4f ? SK_ColorBLACK : SK_ColorWHITE);
        canvas->drawCircle(cx, cy, 5.f, p);
        p.setStrokeWidth(1.f);
        p.setColor(val > 0.4f ? SK_ColorWHITE : SK_ColorBLACK);
        canvas->drawCircle(cx, cy, 5.6f, p);
    }
}

static inline void sk_cp_draw_hue_strip(SkCanvas* canvas, const glint_rect& r, float hue)
{
    if (r.W() <= 0 || r.H() <= 0) return;
    const SkRect sr = sk_cp_to_sk(r);

    // Full-spectrum rainbow �  7-stop horizontal gradient
    {
        SkPoint  pts[2]    = {{r.L, r.T}, {r.R, r.T}};
        const SkColor cols[7] = {
            0xFFFF0000, 0xFFFFFF00, 0xFF00FF00,
            0xFF00FFFF, 0xFF0000FF, 0xFFFF00FF, 0xFFFF0000
        };
        const float pos[7] = {0.f, 1.f/6.f, 2.f/6.f, 3.f/6.f, 4.f/6.f, 5.f/6.f, 1.f};
        auto shader = SkGradientShader::MakeLinear(pts, cols, pos, 7, SkTileMode::kClamp);
        SkPaint p;
        p.setShader(shader);
        canvas->drawRect(sr, p);
    }

    // Cursor � rounded-rect thumb centred at hue position
    {
        const float cx     = r.L + (hue / 360.f) * r.W();
        const float tHalf  = 1.5f;    // half-width of thumb
        const float overhang = 3.f;  // how many px it sticks above/below the strip
        const SkRect thumb = SkRect::MakeLTRB(cx - tHalf, r.T - overhang,
                                               cx + tHalf, r.B + overhang);
        // Shadow / dark border
        SkPaint sp;
        sp.setAntiAlias(true);
        sp.setColor(SkColorSetARGB(200, 0, 0, 0));
        sp.setStyle(SkPaint::kFill_Style);
        canvas->drawRoundRect(SkRect::MakeLTRB(thumb.left() - 1.f, thumb.top() + 1.f,
                                                thumb.right() + 1.f, thumb.bottom() + 1.f),
                              3.f, 3.f, sp);
        // White fill
        SkPaint fp;
        fp.setAntiAlias(true);
        fp.setColor(SK_ColorWHITE);
        fp.setStyle(SkPaint::kFill_Style);
        canvas->drawRoundRect(thumb, 3.f, 3.f, fp);
        // Thin dark outline
        SkPaint op;
        op.setAntiAlias(true);
        op.setColor(SkColorSetARGB(180, 30, 30, 30));
        op.setStyle(SkPaint::kStroke_Style);
        op.setStrokeWidth(1.f);
        canvas->drawRoundRect(thumb, 3.f, 3.f, op);
    }
}

static inline void sk_cp_draw_alpha_strip(SkCanvas* canvas, const glint_rect& r,
                                           float hue, float sat, float val, float alpha)
{
    if (r.W() <= 0 || r.H() <= 0) return;
    const SkRect sr = sk_cp_to_sk(r);

    // 1 � Checkerboard background
    {
        const float cs = std::max(r.H() / 2.f, 4.f);
        SkPaint cp;
        for (float x = r.L; x < r.R; x += cs) {
            for (float y = r.T; y < r.B; y += cs) {
                const int parity = ((int)((x - r.L) / cs) + (int)((y - r.T) / cs)) % 2;
                cp.setColor(parity == 0 ? 0xFFCCCCCC : 0xFF888888);
                canvas->drawRect(SkRect::MakeLTRB(x, y,
                    std::min(x + cs, r.R), std::min(y + cs, r.B)), cp);
            }
        }
    }

    // 2 � Gradient: transparent colour (L) ? opaque colour (R)
    {
        const glint_color  cc     = sk_cp_hsv_to_rgb(hue, sat, val, 255);
        SkPoint  pts[2]      = {{r.L, r.T}, {r.R, r.T}};
        SkColor  cols[2]     = {SkColorSetARGB(0,   cc.R, cc.G, cc.B),
                                SkColorSetARGB(255, cc.R, cc.G, cc.B)};
        auto shader = SkGradientShader::MakeLinear(pts, cols, nullptr, 2, SkTileMode::kClamp);
        SkPaint gp;
        gp.setShader(shader);
        canvas->drawRect(sr, gp);
    }

    // 3 � Cursor � rounded-rect thumb centred at alpha position
    {
        const float cx     = r.L + alpha * r.W();
        const float tHalf  = 1.5f;
        const float overhang = 3.f;
        const SkRect thumb = SkRect::MakeLTRB(cx - tHalf, r.T - overhang,
                                               cx + tHalf, r.B + overhang);
        // Shadow
        SkPaint sp;
        sp.setAntiAlias(true);
        sp.setColor(SkColorSetARGB(200, 0, 0, 0));
        sp.setStyle(SkPaint::kFill_Style);
        canvas->drawRoundRect(SkRect::MakeLTRB(thumb.left() - 1.f, thumb.top() + 1.f,
                                                thumb.right() + 1.f, thumb.bottom() + 1.f),
                              3.f, 3.f, sp);
        // White fill
        SkPaint fp;
        fp.setAntiAlias(true);
        fp.setColor(SK_ColorWHITE);
        fp.setStyle(SkPaint::kFill_Style);
        canvas->drawRoundRect(thumb, 3.f, 3.f, fp);
        // Outline
        SkPaint op;
        op.setAntiAlias(true);
        op.setColor(SkColorSetARGB(180, 30, 30, 30));
        op.setStyle(SkPaint::kStroke_Style);
        op.setStrokeWidth(1.f);
        canvas->drawRoundRect(thumb, 3.f, 3.f, op);
    }
}

// Checkerboard helper � used by swatch to show alpha
static inline void sk_cp_draw_checkerboard(SkCanvas* canvas, const SkRect& rect, float cellSize)
{
    SkPaint p;
    for (float x = rect.left(); x < rect.right(); x += cellSize) {
        for (float y = rect.top(); y < rect.bottom(); y += cellSize) {
            const int parity = ((int)((x - rect.left()) / cellSize) +
                                (int)((y - rect.top())  / cellSize)) % 2;
            p.setColor(parity == 0 ? 0xFFCCCCCC : 0xFF888888);
            canvas->drawRect(SkRect::MakeLTRB(x, y,
                std::min(x + cellSize, rect.right()),
                std::min(y + cellSize, rect.bottom())), p);
        }
    }
}

// --- sk_cp_sv_canvas ---------------------------------------------------------
// 2-D saturation / value picker canvas.

class sk_cp_sv_canvas final : public glint_element
{
public:
    float hue = 0.f;  //  0 � 360
    float sat = 1.f;  //  0 � 1  (X position)
    float val = 1.f;  //  0 � 1  (Y position, top = 1)
    std::function<void(float, float)> onChange;   // (sat, val)

    const char* typeName() const override { return "colorpicker-sv"; }

    glint_element* HitTest(float x, float y) override
    {
        return GetPaintRECT().Contains(x, y) ? this : nullptr;
    }

    void OnMouseDown(float x, float y, const glint_mouse_mod&) override { pick(x, y); }
    void OnMouseDrag(float x, float y, float, float, const glint_mouse_mod&) override { pick(x, y); }

    void drawContent(glint_canvas& g) override
    {
        if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext())) {
            sk_cp_draw_sv_canvas(canvas, getContent(), hue, sat, val);
            return;
        }
        // Fallback: fill with current colour approximation
        g.FillRect(sk_cp_hsv_to_rgb(hue, sat, val, 255), getContent());
    }

    void DrawContentToCanvas(SkCanvas* canvas) override
    {
        sk_cp_draw_sv_canvas(canvas, getContent(), hue, sat, val);
    }

private:
    void pick(float x, float y)
    {
        const glint_rect r = getContent();
        sat = std::clamp((x - r.L) / std::max(r.W(), 1.f), 0.f, 1.f);
        val = std::clamp(1.f - (y - r.T) / std::max(r.H(), 1.f), 0.f, 1.f);
        setDirty(false);
        if (onChange) onChange(sat, val);
    }
};

// --- sk_cp_hue_strip ---------------------------------------------------------

class sk_cp_hue_strip final : public glint_element
{
public:
    float hue = 0.f;   //  0 � 360
    std::function<void(float)> onChange;   // (hue)

    const char* typeName() const override { return "colorpicker-hue"; }

    glint_element* HitTest(float x, float y) override
    {
        return GetPaintRECT().Contains(x, y) ? this : nullptr;
    }

    void OnMouseDown(float x, float y, const glint_mouse_mod&) override { pick(x); }
    void OnMouseDrag(float x, float y, float, float, const glint_mouse_mod&) override { pick(x); }

    void drawContent(glint_canvas& g) override
    {
        if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext())) {
            sk_cp_draw_hue_strip(canvas, getContent(), hue);
            return;
        }
        // Fallback: grey bar with white cursor
        const glint_rect cont = getContent();
        g.FillRect(glint_color(255, 100, 100, 100), cont);
        const float cx = cont.L + (hue / 360.f) * cont.W();
        g.FillRect(glint_color(255, 255, 255, 255), glint_rect(cx - 1.f, cont.T, cx + 1.f, cont.B));
    }

    void DrawContentToCanvas(SkCanvas* canvas) override
    {
        sk_cp_draw_hue_strip(canvas, getContent(), hue);
    }

private:
    void pick(float x)
    {
        const glint_rect r = getContent();
        hue = std::clamp((x - r.L) / std::max(r.W(), 1.f), 0.f, 1.f) * 360.f;
        setDirty(false);
        if (onChange) onChange(hue);
    }
};

// --- sk_cp_alpha_strip -------------------------------------------------------

class sk_cp_alpha_strip final : public glint_element
{
public:
    float hue   = 0.f;   //  used to build the colour for the transparency gradient
    float sat   = 1.f;
    float val   = 1.f;
    float alpha = 1.f;   //  0 � 1
    std::function<void(float)> onChange;   // (alpha 0-1)

    const char* typeName() const override { return "colorpicker-alpha"; }

    glint_element* HitTest(float x, float y) override
    {
        return GetPaintRECT().Contains(x, y) ? this : nullptr;
    }

    void OnMouseDown(float x, float y, const glint_mouse_mod&) override { pick(x); }
    void OnMouseDrag(float x, float y, float, float, const glint_mouse_mod&) override { pick(x); }

    void drawContent(glint_canvas& g) override
    {
        if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext())) {
            sk_cp_draw_alpha_strip(canvas, getContent(), hue, sat, val, alpha);
            return;
        }
        const glint_rect cont = getContent();
        g.FillRect(glint_color(255, 100, 100, 100), cont);
        const float cx = cont.L + alpha * cont.W();
        g.FillRect(glint_color(255, 255, 255, 255), glint_rect(cx - 1.f, cont.T, cx + 1.f, cont.B));
    }

    void DrawContentToCanvas(SkCanvas* canvas) override
    {
        sk_cp_draw_alpha_strip(canvas, getContent(), hue, sat, val, alpha);
    }

private:
    void pick(float x)
    {
        const glint_rect r = getContent();
        alpha = std::clamp((x - r.L) / std::max(r.W(), 1.f), 0.f, 1.f);
        setDirty(false);
        if (onChange) onChange(alpha);
    }
};

// --- sk_cp_swatch ------------------------------------------------------------
// Small square showing the current colour with a checkerboard for alpha < 255.

class sk_cp_swatch final : public glint_element
{
public:
    glint_color color = glint_color(255, 255, 255, 255);

    const char* typeName() const override { return "colorpicker-swatch"; }

    glint_element* HitTest(float x, float y) override { return nullptr; }  // pass to row

    void drawContent(glint_canvas& g) override
    {
        if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext())) {
            drawSkia(canvas);
            return;
        }
        g.FillRect(color, getContent());
    }

    void DrawContentToCanvas(SkCanvas* canvas) override { drawSkia(canvas); }
private:
    void drawSkia(SkCanvas* canvas)
    {
        const glint_rect r   = getContent();
        const SkRect sr = sk_cp_to_sk(r);
        const float  _radR = style.borderRadius.resolve(std::min(r.W(), r.H()));
        const float  rad = _radR > 0.f ? _radR : 3.f;

        // 1 � Checkerboard background (always, so alpha is visible)
        canvas->save();
        canvas->clipRRect(SkRRect::MakeRectXY(sr, rad, rad), true);
        sk_cp_draw_checkerboard(canvas, sr, std::max(r.H() / 2.f, 4.f));

        // 2 � Colour overlay (semi-transparent if alpha < 255)
        SkPaint p;
        p.setColor(SkColorSetARGB(color.A, color.R, color.G, color.B));
        canvas->drawRect(sr, p);
        canvas->restore();
    }
};

// --- sk_cp_dots_btn ---------------------------------------------------------
// Mode-cycle button: draws 3 vertically-stacked filled circles.
// Font-independent � avoids tofu glyphs from U+22EE on the system font.

class sk_cp_dots_btn final : public glint_element
{
public:
    glint_color dotColor = glint_color(255, 160, 160, 160);
    glint_color bgNormal = glint_color(255, 45,  45,  45);
    glint_color bgHover  = glint_color(255, 62,  62,  62);

    const char* typeName() const override { return "colorpicker-mode-btn"; }

    glint_element* HitTest(float x, float y) override
    {
        return GetPaintRECT().Contains(x, y) ? this : nullptr;
    }

    void drawContent(glint_canvas& g) override
    {
        const glint_rect r  = getContent();
        const float cx = r.MW();
        const float cy = r.MH();
        const float dot = 2.f;
        const float sp  = 4.f;   // centre-to-centre spacing
        for (int i = -1; i <= 1; ++i)
            g.FillCircle(dotColor, cx, cy + i * sp, dot);
    }

    void DrawContentToCanvas(SkCanvas* canvas) override
    {
        const glint_rect r  = getContent();
        const float cx = r.MW();
        const float cy = r.MH();
        const float dot = 2.f;
        const float sp  = 4.f;
        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(SkColorSetARGB(dotColor.A, dotColor.R, dotColor.G, dotColor.B));
        for (int i = -1; i <= 1; ++i)
            canvas->drawCircle(cx, cy + i * sp, dot, p);
    }
};

// --- glint_colorpicker -------------------------------------------------------

class glint_colorpicker : public glint_element
{
public:
    // -- Color mode ------------------------------------------------------------
    enum class ColorMode { HEX = 0, RGBA = 1, HSV = 2 };

    // -- Public fields ---------------------------------------------------------
    glint_color    value  = glint_color(255, 255, 255, 255);
    ColorMode mode   = ColorMode::HEX;
    std::function<void(glint_color)> onChange;
    int tag = glint_no_tag;

    // -- Construction ----------------------------------------------------------
    glint_colorpicker()
    {
        setCssStyleLayer(glint_default_user_agent_style_for(*this));
        computedStyle = mergedStyleForLayout();
        sk_cp_rgb_to_hsv(value.R, value.G, value.B, mH, mS, mV);
        mA = value.A / 255.f;
        buildPicker();
    }

    const char* typeName() const override { return "colorpicker"; }

    // -- Programmatic value update ---------------------------------------------
    void setValue(glint_color c)
    {
        value = c;
        sk_cp_rgb_to_hsv(c.R, c.G, c.B, mH, mS, mV);
        mA = c.A / 255.f;
        syncChildren(/*forceInputs=*/true);
        setDirty(false);
    }

    void setMode(ColorMode m)
    {
        mode = m;
        _applyMode();
        syncInputs(/*force=*/true);
        setDirty(false);
    }

    // -- Backward-compat no-ops (were expand/collapse) -------------------------
    void expand()            {}
    void collapse()          {}
    void setExpanded(bool)   {}
    bool isExpanded() const  { return true; }

    void Draw(glint_canvas& g) override
    {
        _syncFromPublicState();
        glint_element::Draw(g);
    }

private:
    // -- HSV + alpha state -----------------------------------------------------
    float mH = 0.f, mS = 1.f, mV = 1.f, mA = 1.f;
    int   mFocusCount = 0;   // incremented on focus, decremented on blur
    bool  mSyncInitialized = false;
    glint_color mLastSyncedValue = glint_color(255, 255, 255, 255);
    ColorMode mLastSyncedMode = ColorMode::HEX;

    // -- Sliders ---------------------------------------------------------------
    sk_cp_sv_canvas*    mSVCanvas   = nullptr;
    sk_cp_hue_strip*    mHueStrip   = nullptr;
    sk_cp_alpha_strip*  mAlphaStrip = nullptr;

    // -- Mode button -----------------------------------------------------------
    sk_cp_dots_btn* mModeBtn = nullptr;

    // -- Input rows (one visible at a time) -----------------------------------
    glint_element* mInputRows[3]   = {}; // HEX=0, RGBA=1, HSV=2

    // HEX row
    glint_input* mHexInput = nullptr;

    // RGBA row
    glint_input* mRGBAInputs[4] = {};  // R, G, B, A

    // HSV row
    glint_input* mHSVInputs[4]  = {};  // H, S, V, A

    // -- Build everything ------------------------------------------------------
    void buildPicker()
    {
        // Outer flex-column with padding
        style.paddingLeft    = kCP_PaddingLR;
        style.paddingRight   = kCP_PaddingLR;
        style.paddingTop     = kCP_PaddingTop;
        style.paddingBottom  = kCP_PaddingBot;
        style.gap            = kCP_Gap;

        // � SV canvas �
        mSVCanvas = new sk_cp_sv_canvas();
        mSVCanvas->hue = mH;
        mSVCanvas->sat = mS;
        mSVCanvas->val = mV;
        mSVCanvas->style.width        = "100%";
        mSVCanvas->style.height       = kCP_SVCanvasH;
        mSVCanvas->style.borderRadius = 4.f;
        mSVCanvas->style.overflow     = "hidden";
        mSVCanvas->onChange = [this](float s, float v) { mS = s; mV = v; onValueChanged(); };
        addChild(mSVCanvas);

        // � Hue strip �
        mHueStrip = new sk_cp_hue_strip();
        mHueStrip->hue = mH;
        mHueStrip->style.width        = "100%";
        mHueStrip->style.height       = kCP_StripH;
        mHueStrip->style.borderRadius = 8.f;
        mHueStrip->style.overflow     = "hidden";
        mHueStrip->onChange = [this](float h) {
            mH = h;
            if (mSVCanvas) mSVCanvas->hue = h;
            onValueChanged();
        };
        addChild(mHueStrip);

        // � Alpha strip �
        mAlphaStrip = new sk_cp_alpha_strip();
        mAlphaStrip->hue = mH; mAlphaStrip->sat = mS;
        mAlphaStrip->val = mV; mAlphaStrip->alpha = mA;
        mAlphaStrip->style.width        = "100%";
        mAlphaStrip->style.height       = kCP_StripH;
        mAlphaStrip->style.borderRadius = 8.f;
        mAlphaStrip->style.overflow     = "hidden";
        mAlphaStrip->onChange = [this](float a) { mA = a; onValueChanged(); };
        addChild(mAlphaStrip);

        // � Bottom row: input area (3 modes, one visible) + ? mode button �
        auto* bottomRow = new glint_element();
        bottomRow->style.display       = "flex";
        bottomRow->style.flexDirection = "row";
        bottomRow->style.alignItems    = "center";
        bottomRow->style.width         = "100%";
        bottomRow->style.height        = kCP_HexRowH;
        bottomRow->style.gap           = 4.f;
        addChild(bottomRow);

        buildHexRow(bottomRow);
        buildRgbaRow(bottomRow);
        buildHsvRow(bottomRow);
        buildModeButton(bottomRow);

        _applyMode();
        syncInputs(/*force=*/true);
    }

    // -- Mode button (? cycles HEX ? RGBA ? HSV) ------------------------------
    void buildModeButton(glint_element* parent)
    {
        auto* btn = new sk_cp_dots_btn();
        btn->style.width           = 24.f;
        btn->style.height          = 24.f;
        btn->style.backgroundColor = btn->bgNormal;
        btn->style.borderRadius    = 4.f;
        btn->style.borderColor     = glint_color(255, 72, 72, 72);
        btn->style.borderWidth     = 1.f;
        btn->mAcceptsFocus         = false;
        btn->element.id            = "mode-btn";
        mModeBtn = btn;
        parent->addChild(btn);

        btn->element.addEventListener("click", [this](glint_event&) {
            mode = static_cast<ColorMode>((static_cast<int>(mode) + 1) % 3);
            _applyMode();
            syncInputs(/*force=*/true);
            setDirty(false);
        });
        btn->element.addEventListener("mouseenter", [btn](glint_event&) {
            btn->style.backgroundColor = btn->bgHover;
            btn->setDirty(false);
        });
        btn->element.addEventListener("mouseleave", [btn](glint_event&) {
            btn->style.backgroundColor = btn->bgNormal;
            btn->setDirty(false);
        });
    }

    // -- HEX input row ---------------------------------------------------------
    void buildHexRow(glint_element* parent)
    {
        auto* row = new glint_element();
        row->style.display       = "flex";
        row->style.flexDirection = "row";
        row->style.alignItems    = "center";
        row->style.flexGrow      = 1.f;
        row->style.height        = "100%";
        row->style.gap           = 4.f;
        mInputRows[0] = row;
        parent->addChild(row);

        auto* prefix = new glint_element();
        prefix->innerText           = "#";
        prefix->style.color    = glint_color(255, 130, 130, 130);
        prefix->style.fontSize = 13.f;
        prefix->style.width    = 12.f;
        row->addChild(prefix);

        mHexInput = _makeInput(row);
        mHexInput->setValue(sk_cp_to_hex(value));
        mHexInput->onChange = [this](const std::string& s) {
            if (s.size() == 6 || s.size() == 8) {
                glint_color p; if (!sk_cp_parse_hex(s, p)) return;
                value = p;
                sk_cp_rgb_to_hsv(p.R, p.G, p.B, mH, mS, mV);
                mA = p.A / 255.f;
                _syncSliders();
                syncInputs(/*force=*/false);  // don't overwrite the currently focused hex
                setDirty(false);
                if (onChange) onChange(value);
            }
        };
        mHexInput->onSubmit = mHexInput->onChange;
    }

    // -- RGBA input row --------------------------------------------------------
    void buildRgbaRow(glint_element* parent)
    {
        auto* row = new glint_element();
        row->style.display       = "flex";
        row->style.flexDirection = "row";
        row->style.alignItems    = "stretch";
        row->style.flexGrow      = 1.f;
        row->style.height        = "100%";
        row->style.gap           = 3.f;
        row->style.marginTop     = "16px";
        mInputRows[1] = row;
        parent->addChild(row);

        const char* lbls[4] = {"R","G","B","A"};
        for (int i = 0; i < 4; ++i) {
            // Column: input on top, label below
            auto* col = new glint_element();
            col->align = "center ttb";
            col->style.display        = "flex";
            col->style.flexDirection  = "column";
            col->style.alignItems     = "center";
            col->style.justifyContent = "center";
            col->style.flexGrow       = 1.f;
            col->style.gap            = 2.f;
            row->addChild(col);

            mRGBAInputs[i] = _makeInput(col);
            mRGBAInputs[i]->style.flexGrow = 0.f;  // don't stretch vertically
            mRGBAInputs[i]->style.width    = "100%";
            mRGBAInputs[i]->style.height   = 18.f;
            mRGBAInputs[i]->type = "number";
            mRGBAInputs[i]->min  = 0.f;
            mRGBAInputs[i]->max  = (i == 3) ? 1.f : 255.f;

            auto* lbl = new glint_element();
            lbl->innerText           = lbls[i];
            lbl->style.color    = glint_color(255, 110, 110, 110);
            lbl->style.fontSize = 9.f;
            lbl->style.marginTop = "4px";
            col->addChild(lbl);

            const int ci = i;
            mRGBAInputs[i]->onChange = [this, ci](const std::string& s) {
                if (ci == 3) {
                    // Alpha: 0-1 float
                    float v = 0.f;
                    try { v = std::stof(s); } catch (...) { return; }
                    mA = std::clamp(v, 0.f, 1.f);
                    value.A = (int)std::round(mA * 255.f);
                } else {
                    int v = 0;
                    try { v = std::stoi(s); } catch (...) { return; }
                    v = std::clamp(v, 0, 255);
                    if      (ci == 0) value.R = v;
                    else if (ci == 1) value.G = v;
                    else              value.B = v;
                    sk_cp_rgb_to_hsv(value.R, value.G, value.B, mH, mS, mV);
                    mA = value.A / 255.f;
                }
                _syncSliders();
                syncInputs(/*force=*/false);
                setDirty(false);
                if (onChange) onChange(value);
            };
            mRGBAInputs[i]->onSubmit = mRGBAInputs[i]->onChange;
        }
    }

    // -- HSV input row ---------------------------------------------------------
    void buildHsvRow(glint_element* parent)
    {
        auto* row = new glint_element();
        row->style.display       = "flex";
        row->style.flexDirection = "row";
        row->style.alignItems    = "stretch";
        row->style.flexGrow      = 1.f;
        row->style.height        = "100%";
        row->style.gap           = 3.f;
        row->style.marginTop     = "16px";
        mInputRows[2] = row;
        parent->addChild(row);

        // H 0-360, S 0-100, V 0-100, A 0-255
        const char* lbls[4]  = {"H","S","V","A"};
        const float maxes[4] = {360.f, 100.f, 100.f, 1.f};
        for (int i = 0; i < 4; ++i) {
            // Column: input on top, label below
            auto* col = new glint_element();
            col->align = "center ttb";
            col->style.display        = "flex";
            col->style.flexDirection  = "column";
            col->style.alignItems     = "center";
            col->style.justifyContent = "center";
            col->style.flexGrow       = 1.f;
            col->style.gap            = 2.f;
            row->addChild(col);

            mHSVInputs[i] = _makeInput(col);
            mHSVInputs[i]->style.flexGrow = 0.f;
            mHSVInputs[i]->style.width    = "100%";
            mHSVInputs[i]->style.height   = 18.f;
            mHSVInputs[i]->type = "number";
            mHSVInputs[i]->min  = 0.f;
            mHSVInputs[i]->max  = maxes[i];

            auto* lbl = new glint_element();
            lbl->innerText           = lbls[i];
            lbl->style.color    = glint_color(255, 110, 110, 110);
            lbl->style.fontSize = 9.f;
            lbl->style.marginTop = "4px";
            col->addChild(lbl);

            const int   ci  = i;
            const float mx  = maxes[i];
            mHSVInputs[i]->onChange = [this, ci, mx](const std::string& s) {
                float v = 0.f;
                try { v = std::stof(s); } catch (...) { return; }
                v = std::clamp(v, 0.f, mx);
                if      (ci == 0) mH = v;
                else if (ci == 1) mS = v / 100.f;
                else if (ci == 2) mV = v / 100.f;
                else              mA = v;   // 0-1 directly
                value = sk_cp_hsv_to_rgb(mH, mS, mV, (int)std::round(mA * 255.f));
                _syncSliders();
                syncInputs(/*force=*/false);
                setDirty(false);
                if (onChange) onChange(value);
            };
            mHSVInputs[i]->onSubmit = mHSVInputs[i]->onChange;
        }
    }

    // -- Helper: create a styled input, add it to parent -----------------------
    glint_input* _makeInput(glint_element* parent)
    {
        auto* inp = new glint_input();
        inp->style.flexGrow        = 1.f;
        inp->style.height          = 24.f;
        inp->style.backgroundColor = glint_color(255, 36, 36, 36);
        inp->style.borderColor     = glint_color(255, 72, 72, 72);
        inp->style.borderWidth     = 1.f;
        inp->style.borderRadius    = 3.f;
        inp->style.color           = glint_color(255, 215, 215, 215);
        inp->style.fontSize        = 11.f;
        // Track how many inputs are focused so syncInputs knows when to hold off.
        inp->element.addEventListener("focus", [this](glint_event&) { ++mFocusCount; });
        inp->element.addEventListener("blur",  [this](glint_event&) { --mFocusCount; });
        parent->addChild(inp);
        return inp;
    }

    // -- Apply mode: show/hide input rows -------------------------------------
    void _applyMode()
    {
        const int m = static_cast<int>(mode);
        for (int i = 0; i < 3; ++i)
        {
            if (mInputRows[i])
                mInputRows[i]->style.display = (i == m) ? "flex" : "none";
        }
    }

    static bool _sameColor(const glint_color& lhs, const glint_color& rhs)
    {
        return lhs.A == rhs.A && lhs.R == rhs.R && lhs.G == rhs.G && lhs.B == rhs.B;
    }

    void _captureSyncedState()
    {
        mSyncInitialized = true;
        mLastSyncedValue = value;
        mLastSyncedMode = mode;
    }

    void _syncFromPublicState()
    {
        if (!mSyncInitialized || !_sameColor(mLastSyncedValue, value))
        {
            sk_cp_rgb_to_hsv(value.R, value.G, value.B, mH, mS, mV);
            mA = value.A / 255.f;
            _syncSliders();
            syncInputs(/*force=*/false);
        }

        if (!mSyncInitialized || mLastSyncedMode != mode)
        {
            _applyMode();
            syncInputs(/*force=*/false);
        }

        _captureSyncedState();
    }

    // -- Sync slider visuals from current H/S/V/A ------------------------------
    void _syncSliders()
    {
        if (mSVCanvas)  { mSVCanvas->hue = mH; mSVCanvas->sat = mS; mSVCanvas->val = mV; }
        if (mHueStrip)    mHueStrip->hue  = mH;
        if (mAlphaStrip) {
            mAlphaStrip->hue   = mH; mAlphaStrip->sat = mS;
            mAlphaStrip->val   = mV; mAlphaStrip->alpha = mA;
        }
    }

    // -- Sync text inputs � skipped when user is actively typing (force=false) -
    void syncInputs(bool force)
    {
        if (!force && mFocusCount > 0) return;

        // HEX
        if (mHexInput) mHexInput->setValue(sk_cp_to_hex(value));

        // RGBA
        char buf[16];
        if (mRGBAInputs[0]) { std::snprintf(buf, 16, "%d", value.R); mRGBAInputs[0]->setValue(buf); }
        if (mRGBAInputs[1]) { std::snprintf(buf, 16, "%d", value.G); mRGBAInputs[1]->setValue(buf); }
        if (mRGBAInputs[2]) { std::snprintf(buf, 16, "%d", value.B); mRGBAInputs[2]->setValue(buf); }
        { float aV = value.A / 255.f; std::snprintf(buf, 16, (aV == 0.f || aV == 1.f) ? "%.0f" : "%.2f", aV); if (mRGBAInputs[3]) mRGBAInputs[3]->setValue(buf); }

        // HSV
        if (mHSVInputs[0]) { std::snprintf(buf, 16, "%.0f", mH);          mHSVInputs[0]->setValue(buf); }
        if (mHSVInputs[1]) { std::snprintf(buf, 16, "%.0f", mS * 100.f);  mHSVInputs[1]->setValue(buf); }
        if (mHSVInputs[2]) { std::snprintf(buf, 16, "%.0f", mV * 100.f);  mHSVInputs[2]->setValue(buf); }
        if (mHSVInputs[3]) { std::snprintf(buf, 16, (mA == 0.f || mA == 1.f) ? "%.0f" : "%.2f", mA); mHSVInputs[3]->setValue(buf); }
    }

    // -- Sync everything from current H/S/V/A ----------------------------------
    void syncChildren(bool forceInputs = false)
    {
        value = sk_cp_hsv_to_rgb(mH, mS, mV, (int)std::round(mA * 255.f));
        _syncSliders();
        syncInputs(forceInputs);
    }

    // -- Called by slider callbacks --------------------------------------------
    void onValueChanged()
    {
        syncChildren(/*forceInputs=*/false);
        setDirty(false);
        if (onChange) onChange(value);
    }
};

// New API name � both refer to the same class.
namespace { struct _glint_colorpicker_reg { _glint_colorpicker_reg() { glint_element::registerElement("colorpicker", []{ return new glint_colorpicker(); }); } } _glint_colorpicker_reg_; }
