#pragma once

/**
 * glint_slider.hpp
 * A horizontal range-slider built entirely from child elements — no custom drawing.
 *
 * This file lives in third_party/glint/components/ and is included by
 * glint_input.hpp to power the type="range" delegate.
 *
 * Structure:
 *   glint_slider (root, position:relative, cursor:pointer)
 *     mTrack   — the full rail/line (position:absolute)
 *     mThumb   — the draggable handle (position:absolute)
 *
 * Usage standalone or via glint_input (preferred):
 *
 *   // Via glint_input (HTML-idiomatic):
 *   add.input([](glint_input& _c) {
 *     _c.type       = "range";
 *     _c.min        = 0.f;
 *     _c.max        = 100.f;
 *     _c.style.width  = 200.f;
 *     _c.style.height = 24.f;
 *     _c.onChange   = [](const std::string& v) { ... };
 *   });
 *
 *   // Standalone:
 *   auto* sl = new glint_slider();
 *   sl->style.width  = 200.f;
 *   sl->style.height = 24.f;
 *   sl->min        = 0.f;
 *   sl->max        = 100.f;
 *   sl->value      = 50.f;
 *   sl->trackColor = "#444444";
 *   sl->thumbColor = "#ffffff";
 *   sl->onChange   = [](float v) { ... };
 */

#include "../glint_element.hpp"
#include <algorithm>
#include <cmath>
#include <functional>

class glint_slider : public glint_element
{
public:
    // -- Configuration ---------------------------------------------------------
    float min   = 0.f;
    float max   = 1.f;
    float value = 0.f;
    float step  = 0.f;          // 0 = continuous

    float trackHeight = 4.f;
    float thumbSize   = 16.f;

    const char* trackColor = "#444444";
    const char* thumbColor = "#ffffff";

    std::function<void(float)> onChange;

    // -- Construction ----------------------------------------------------------
    glint_slider()
    {
        style.position = "relative";
        style.cursor   = "pointer";

        mTrack = new glint_element();
        mTrack->style.position = "absolute";
        addChild(mTrack);

        mThumb = new glint_element();
        mThumb->style.position = "absolute";
        addChild(mThumb);
    }

    // -- Accessors -------------------------------------------------------------
    void SetValue(float v)
    {
        value = _clamp(v);
        _sync();
        setDirty(false);
    }

    // -- Element metadata ------------------------------------------------------
    const char* typeName() const override { return "slider"; }
    const char* tagName()  const override { return "slider"; }

    // -- Hit testing -----------------------------------------------------------
    // Absorb all hits on the slider rect so clicks/drags anywhere on it — track
    // or thumb — route to this element's mouse handlers rather than children.
    glint_element* HitTest(float x, float y) override
    {
        return GetPaintRECT().Contains(x, y) ? this : nullptr;
    }

    // -- Mouse events ----------------------------------------------------------
    void OnMouseDown(float x, float /*y*/, const glint_mouse_mod& /*mod*/) override
    {
        mDragging = true;
        _setValue(_xToValue(x));
    }

    void OnMouseDrag(float x, float /*y*/, float /*dX*/, float /*dY*/,
                     const glint_mouse_mod& /*mod*/) override
    {
        if (!mDragging) return;
        _setValue(_xToValue(x));
    }

    void OnMouseUp(float /*x*/, float /*y*/, const glint_mouse_mod& /*mod*/) override
    {
        mDragging = false;
    }

    // -- Layout ----------------------------------------------------------------
    void Layout(glint_canvas* g) override
    {
        // mRect is already valid here (set by parent before calling Layout).
        _sync();
        // tickTransitionsAll() already ran this frame before Layout(), so
        // computedStyle on track/thumb was snapshotted BEFORE _sync() wrote
        // the correct style.width/height/left/top values.  Force-refresh now
        // so childPrefW() / absolute positioning reads current values.
        if (mTrack) mTrack->computedStyle = mTrack->mergedStyleForLayout();
        if (mThumb) mThumb->computedStyle = mThumb->mergedStyleForLayout();
        glint_element::Layout(g);
    }

private:
    bool           mDragging = false;
    glint_element* mTrack    = nullptr;
    glint_element* mThumb    = nullptr;

    // Clamp v to [min, max] and snap to the nearest step if step > 0.
    float _clamp(float v) const
    {
        v = std::clamp(v, min, max);
        if (step > 0.f)
            v = min + std::round((v - min) / step) * step;
        return std::clamp(v, min, max);
    }

    // Convert a window-space x coordinate to a value in [min, max].
    float _xToValue(float x) const
    {
        const glint_rect r      = GetPaintRECT();
        const float      usable = r.W() - thumbSize;
        if (usable <= 0.f) return min;
        const float t = (x - r.L - thumbSize * 0.5f) / usable;
        return _clamp(min + std::clamp(t, 0.f, 1.f) * (max - min));
    }

    // Update value, sync child layout, fire callback.
    void _setValue(float v)
    {
        const float prev = value;
        value = v;
        _sync();
        setDirty(false);
        if (value != prev && onChange) onChange(value);
    }

    // Apply current value and dimensions to the child element styles.
    // Called from Layout() (where mRect is valid) and from SetValue() / _setValue().
    void _sync()
    {
        const float w = mRect.W();
        const float h = mRect.H();
        const float t = (max > min) ? (value - min) / (max - min) : 0.f;

        if (mTrack)
        {
            mTrack->style.left            = 0.f;
            mTrack->style.top             = (h - trackHeight) * 0.5f;
            mTrack->style.width           = w;
            mTrack->style.height          = trackHeight;
            mTrack->style.borderRadius    = trackHeight * 0.5f;
            mTrack->style.backgroundColor = trackColor;
        }

        if (mThumb)
        {
            const float usable = w - thumbSize;
            mThumb->style.left            = usable > 0.f ? t * usable : 0.f;
            mThumb->style.top             = (h - thumbSize) * 0.5f;
            mThumb->style.width           = thumbSize;
            mThumb->style.height          = thumbSize;
            mThumb->style.borderRadius    = thumbSize * 0.5f;
            mThumb->style.backgroundColor = thumbColor;
        }
    }
};

namespace {
    struct _glint_slider_reg {
        _glint_slider_reg() {
            glint_element::registerElement("slider", []() -> glint_element* { return new glint_slider(); });
        }
    } _glint_slider_reg_;
}
