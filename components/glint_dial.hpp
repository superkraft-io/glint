#pragma once

/**
 * glint_dial.hpp
 * Generic circular angle dial component.
 *
 * Renders a circular knob with an indicator line pointing at the current
 * angle.  Dragging anywhere inside the circle updates the angle.
 *
 * Public fields:
 *   angle     — current angle in degrees; CSS convention: 0 = North (up / "to top"),
 *               90 = East (right / "to right"), 180 = South, 270 = West.
 *               Matches CSS linear-gradient angle semantics exactly.
 *   onChange  — called with the new angle (float, degrees) whenever the user
 *               drags to a new position.
 *   tag       — integer tag for lookup via glint_document::GetNodeWithTag.
 *
 * Default style:
 *   border-radius: 9999px (renders as a circle)
 *   background-color: #1a1a1a
 *   border: 1px solid #505050
 *   (Set style.width / style.height to control size.)
 *
 * Builder usage (inside glint_body or any constructor-built subtree):
 *   add.dial([](glint_dial& _c) {
 *     _c.angle         = 0.f;
 *     _c.style.width   = 30.f;
 *     _c.style.height  = 30.f;
 *     _c.onChange = [](float degrees) { ... };
 *   });
 *
 * Visual:
 *   • Faint outer ring (grey, 50% alpha)
 *   • Indicator line from center to edge at the current angle
 *   • Yellow accent dot at the arrowhead
 *   • Dim center pivot dot
 */

#include "../glint_element.hpp"
#include "../default_style.hpp"

#include <cmath>
#include <functional>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

// ─── glint_dial ───────────────────────────────────────────────────────────────

class glint_dial : public glint_element
{
public:
    // ── Public API ────────────────────────────────────────────────────────────
    float angle = 0.f;   // degrees; CSS convention: 0 = North (up), 90 = East (right)
    std::function<void(float)> onChange;
    int tag = glint_no_tag;

    // ── Construction ──────────────────────────────────────────────────────────
    glint_dial()
    {
        setCssStyleLayer(glint_default_user_agent_style_for(*this));
        computedStyle = mergedStyleForLayout();
    }

    const char* typeName() const override { return "dial"; }

    // ── Mouse interaction ─────────────────────────────────────────────────────
    void OnMouseDown(float x, float y, const glint_mouse_mod&) override { _updateAngle(x, y); }
    void OnMouseDrag(float x, float y, float, float, const glint_mouse_mod&) override { _updateAngle(x, y); }

    // ── Drawing ───────────────────────────────────────────────────────────────
    void drawContent(glint_canvas& g) override
    {
        if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext())) { _drawSkia(canvas); return; }
        (void)g;
    }

    void DrawContentToCanvas(SkCanvas* canvas) override { _drawSkia(canvas); }

private:
    // ── Helpers ───────────────────────────────────────────────────────────────
    void _updateAngle(float x, float y)
    {
        const float cx = mPaintRECT.MW();
        const float cy = mPaintRECT.MH();
        // atan2 returns math angle (0=East). Convert to CSS angle (0=North) by adding 90°.
        const float mathDeg = std::atan2(y - cy, x - cx) * 180.f / (float)M_PI;
        angle = std::fmod(mathDeg + 90.f + 360.f, 360.f);
        if (onChange) onChange(angle);
        setDirty(false);
    }

    void _drawSkia(SkCanvas* canvas)
    {
        const float cx  = mPaintRECT.MW();
        const float cy  = mPaintRECT.MH();
        const float r   = std::min(mPaintRECT.W(), mPaintRECT.H()) * 0.5f - 3.f;
        // Convert CSS angle (0=North) to math angle (0=East) by subtracting 90°.
        const float rad = (angle - 90.f) * (float)M_PI / 180.f;

        // Faint outer ring
        SkPaint rp;
        rp.setStyle(SkPaint::kStroke_Style);
        rp.setColor(SkColorSetARGB(50, 200, 200, 200));
        rp.setStrokeWidth(1.f);
        rp.setAntiAlias(true);
        canvas->drawCircle(cx, cy, r, rp);

        // Indicator line from center to edge
        const float ex = cx + std::cos(rad) * r;
        const float ey = cy + std::sin(rad) * r;
        SkPaint lp;
        lp.setStyle(SkPaint::kStroke_Style);
        lp.setColor(SkColorSetARGB(255, 210, 210, 210));
        lp.setStrokeWidth(1.5f);
        lp.setAntiAlias(true);
        lp.setStrokeCap(SkPaint::kRound_Cap);
        canvas->drawLine(cx, cy, ex, ey, lp);

        // Yellow accent dot at arrowhead
        SkPaint dp;
        dp.setAntiAlias(true);
        dp.setColor(SkColorSetARGB(255, 255, 210, 60));
        canvas->drawCircle(ex, ey, 2.5f, dp);

        // Dim center pivot dot
        SkPaint cp;
        cp.setAntiAlias(true);
        cp.setColor(SkColorSetARGB(180, 170, 170, 170));
        canvas->drawCircle(cx, cy, 1.8f, cp);
    }
};

// New API name — both refer to the same class.
namespace { struct _glint_dial_reg { _glint_dial_reg() { glint_element::registerElement("dial", []{ return new glint_dial(); }); } } _glint_dial_reg_; }
