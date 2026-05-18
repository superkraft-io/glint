#pragma once

/**
 * glint_gradient_editor.hpp
 * Gradient ramp editor with draggable colour stops and an embedded colour picker.
 *
 * Interactions:
 *   • Click empty ramp area   → add a stop at that position (colour sampled from gradient)
 *   • Click a handle          → select it (opens glint_colorpicker_window popup)
 *   • Drag a handle           → reposition the stop (stops re-sorted after drag ends)
 *   • Double-click a handle   → remove the stop (minimum 1 stop enforced)
 *
 * The component has a fixed height of kGE_BaseH (ramp bar + handle strip).
 * The colour picker opens as an independent HWND_TOPMOST popup window and
 * auto-dismisses when it loses focus.
 *
 * Builder shorthand:
 *   add.gradientEditor([](glint_gradient_editor& _c) {
 *     _c.stops = {{ 0.f, glint_color(255,0,0,0) }, { 1.f, glint_color(255,255,255,255) }};
 *     _c.style.width = 300.f;
 *     _c.onChange = [](const std::vector<sk_gradient_stop>& s) { ... };
 *   });
 *
 * Or directly inside a component constructor:
 *   add.gradientEditor([](glint_gradient_editor& _c) { ... }, &mGradientEditor);
 */

#include "../glint_element.hpp"
#include "../default_style.hpp"
#include "../glint_document.hpp"       // needed for mRoot->hwnd (screen-space conversion)
#include "../platform/glint_apple_platform.hpp"
#include "glint_colorpicker_window.hpp"   // standalone popup color picker window
#include "glint_dial.hpp"          // glint_dial — circular angle dial

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/effects/SkGradientShader.h"

// ─── Layout constants ─────────────────────────────────────────────────────────

static constexpr float kGE_RampH       = 32.f;   // gradient preview bar height
static constexpr float kGE_HandleZoneH = 16.f;   // handle strip height below ramp
static constexpr float kGE_BaseH       = kGE_RampH + kGE_HandleZoneH;
static constexpr float kGE_TriHW      = 5.f;    // triangle half-width at base (px)
static constexpr float kGE_TriH       = 6.f;    // triangle height (px);  tip overlaps ramp by 3px
static constexpr float kGE_SqSize     = 9.f;    // color-square side length (px)
static constexpr float kGE_SqGap      = 1.f;    // gap between triangle base and square top
static constexpr float kGE_HitR       = 8.f;    // mouse hit half-width for handles

static constexpr float kGE_DialSize    = 30.f;   // direction dial diameter (px)
static constexpr float kGE_DialGapLeft = 8.f;    // gap between ramp right edge and dial left edge
static constexpr float kGE_DialMargin  = 4.f;    // gap between dial right edge and component right border
static constexpr float kGE_TypeBtnW   = 20.f;   // type cycle button width
static constexpr float kGE_TypeBtnGap = 4.f;    // gap between type button and dial
static constexpr float kGE_DialColumnW = kGE_TypeBtnW + kGE_TypeBtnGap + kGE_DialGapLeft + kGE_DialSize + kGE_DialMargin;   // total width reserved to the right of the ramp
static constexpr float kGE_DialInputH  = 14.f;   // height of the degree text input below the dial

// ─── GE_TypeButton — draws "L", "R", or "C" depending on the current type ───

class glint_gradient_editor;  // forward declaration

class GE_TypeButton : public glint_element
{
public:
    glint_gradient_editor* mOwner = nullptr;
    const char* typeName() const override { return "ge_type_btn"; }
    void DrawContentToCanvas(SkCanvas* canvas) override;  // defined after the main class
    void drawContent(glint_canvas& g) override;              // graphics fallback, defined after
};

// ─── glint_gradient_editor ───────────────────────────────────────────────

class glint_gradient_editor : public glint_element
{
public:
    // ── Public API ────────────────────────────────────────────────────────────
    std::vector<sk_gradient_stop> stops;
    std::function<void(const std::vector<sk_gradient_stop>&)> onChange;
    float direction = 0.f;   // gradient angle in degrees: 0 = left→right, 90 = top→bottom
    std::function<void(float)> onDirectionChange;
    // Gradient type: "linear" | "radial" | "conic"
    std::string gradientType = "linear";
    std::function<void(const std::string&)> onTypeChange;
    // Radial / conic center [0..1] relative to element size
    float centerX = 0.5f;
    float centerY = 0.5f;
    std::function<void(float,float)> onCenterChange;
    // Radial radius [0..1] relative to min(W,H)*0.5
    float radius = 1.0f;
    std::function<void(float)> onRadiusChange;
    int tag = glint_no_tag;

    // ── Construction ──────────────────────────────────────────────────────────
    glint_gradient_editor()
    {
        setCssStyleLayer(glint_default_user_agent_style_for(*this));
        computedStyle = mergedStyleForLayout();
        _ensureDefaultStops();

        auto* typeBtn = new GE_TypeButton();
        typeBtn->mOwner               = this;
        typeBtn->style.position       = "absolute";
        typeBtn->style.width          = kGE_TypeBtnW;
        typeBtn->style.height         = kGE_RampH;
        typeBtn->style.top            = 0.f;
        typeBtn->style.left           = 0.f;  // overwritten in Layout()
        typeBtn->style.backgroundColor = glint_color(255, 38, 38, 38);
        typeBtn->style.borderColor    = glint_color(255, 65, 65, 65);
        typeBtn->style.borderWidth    = 1.f;
        typeBtn->style.borderRadius   = 3.f;
        typeBtn->element.id           = "ge-type-btn";
        typeBtn->element.addEventListener("click", [this](glint_event& e) {
            e.stopPropagation();
            if      (gradientType == "linear") gradientType = "radial";
            else if (gradientType == "radial") gradientType = "conic";
            else                               gradientType = "linear";
            _syncTypeControls();
            if (onTypeChange) onTypeChange(gradientType);
            setDirty(false);
        });
        addChild(typeBtn);
        mTypeBtn = typeBtn;

        // ── Direction dial ────────────────────────────────────────────────────
        mDialComp = new glint_dial();
        mDialComp->angle            = direction;
        mDialComp->style.position   = "absolute";
        mDialComp->style.width      = kGE_DialSize;
        mDialComp->style.height     = kGE_DialSize;
        // left/top are set every frame in Layout() so the dial tracks the component width.
        mDialComp->style.top        = (kGE_RampH - kGE_DialSize) * 0.5f;
        mDialComp->style.left       = 0.f;  // overwritten in Layout()
        mDialComp->element.id       = "ge-direction-dial";
        mDialComp->onChange = [this](float a) {
            direction = a;
            if (mAngleInput) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", (int)std::round(a));
                mAngleInput->setValue(buf);
            }
            if (onDirectionChange) onDirectionChange(direction);
            setDirty(false);
        };
        addChild(mDialComp);

        // ── Degree input (below the dial, handle zone height) ────────────────
        mAngleInput = new glint_input();
        mAngleInput->type                   = "number";
        mAngleInput->min                    = 0.f;
        mAngleInput->max                    = 359.f;
        mAngleInput->style.position         = "absolute";
        mAngleInput->style.width            = kGE_DialSize;
        mAngleInput->style.height           = kGE_DialInputH;
        mAngleInput->style.fontSize         = 12.f;
        mAngleInput->style.textAlign        = EAlign::Center;
        mAngleInput->style.padding          = "0 2";
        mAngleInput->style.backgroundColor  = glint_color(255, 38, 38, 38);
        mAngleInput->style.borderColor      = glint_color(255, 55, 55, 55);
        mAngleInput->style.borderWidth      = 1.f;
        mAngleInput->style.borderRadius     = 2.f;
        mAngleInput->style.color            = glint_color(255, 180, 180, 180);
        mAngleInput->style.left             = 0.f;  // overwritten in Layout()
        mAngleInput->style.top              = kGE_RampH + (kGE_HandleZoneH - kGE_DialInputH) * 0.5f;
        mAngleInput->element.id             = "ge-angle-input";
        mAngleInput->onChange = [this](const std::string& v) {
            if (v.empty()) return;
            try {
                float a = std::fmod(std::stof(v) + 3600.f, 360.f);
                direction = a;
                if (mDialComp) mDialComp->angle = a;
                if (onDirectionChange) onDirectionChange(direction);
                setDirty(false);
            } catch (...) {}
        };
        mAngleInput->onSubmit = [this](const std::string& v) {
            if (v.empty()) return;
            try {
                float a = std::fmod(std::stof(v) + 3600.f, 360.f);
                direction = a;
                if (mDialComp) mDialComp->angle = a;
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", (int)std::round(a));
                mAngleInput->setValue(buf);
                if (onDirectionChange) onDirectionChange(direction);
                setDirty(false);
            } catch (...) {}
        };
        addChild(mAngleInput);
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", (int)std::round(direction));
            mAngleInput->setValue(buf);
        }

        // ── Radius input (shown in radial mode, replaces dial+angle) ─────────
        mRadiusInput = new glint_input();
        mRadiusInput->type                   = "number";
        mRadiusInput->min                    = 0.01f;
        mRadiusInput->style.position         = "absolute";
        mRadiusInput->style.width            = kGE_DialSize;
        mRadiusInput->style.height           = 26.f;
        mRadiusInput->style.fontSize         = 12.f;
        mRadiusInput->style.textAlign        = EAlign::Center;
        mRadiusInput->style.padding          = "0 2";
        mRadiusInput->style.backgroundColor  = glint_color(255, 38, 38, 38);
        mRadiusInput->style.borderColor      = glint_color(255, 55, 55, 55);
        mRadiusInput->style.borderWidth      = 1.f;
        mRadiusInput->style.borderRadius     = 2.f;
        mRadiusInput->style.color            = glint_color(255, 180, 180, 180);
        mRadiusInput->style.left             = 0.f;  // overwritten in Layout()
        mRadiusInput->style.top              = (kGE_BaseH - 26.f) * 0.5f;
        mRadiusInput->style.display          = "none";  // hidden until radial mode
        mRadiusInput->element.id             = "ge-radius-input";
        mRadiusInput->onChange = [this](const std::string& v) {
            if (v.empty()) return;
            try {
                radius = std::max(0.01f, std::stof(v));
                if (onRadiusChange) onRadiusChange(radius);
                setDirty(false);
            } catch (...) {}
        };
        mRadiusInput->onSubmit = [this](const std::string& v) {
            if (v.empty()) return;
            try {
                radius = std::max(0.01f, std::stof(v));
                _syncRadiusInput();
                if (onRadiusChange) onRadiusChange(radius);
                setDirty(false);
            } catch (...) {}
        };
        mRadiusInput->element.addEventListener("wheel", [this](glint_event& e) {
            auto& we = static_cast<glint_wheel_event&>(e);
            we.preventDefault();
            const float step = we.shiftKey ? 0.01f : 0.05f;
            radius = std::max(0.01f, radius + (we.deltaY < 0.f ? step : -step));
            _syncRadiusInput();
            if (onRadiusChange) onRadiusChange(radius);
            setDirty(false);
        });
        addChild(mRadiusInput);
        _syncRadiusInput();

        // Apply initial visibility based on gradientType.
        _syncTypeControls();

        // ── DOM double-click → remove handle ─────────────────────────────────
        element.addEventListener("dblclick", [this](glint_event& e) {
            auto& me = static_cast<glint_mouse_event&>(e);
            const int idx = _hitHandle(me.clientX, me.clientY);
            const bool isEndStop = (idx == 0 || idx == (int)stops.size() - 1);
            if (idx >= 0 && !isEndStop && (int)stops.size() > 2) {
                stops.erase(stops.begin() + idx);
                if (mSelectedStop == idx)
                    _selectStop(-1);
                else if (mSelectedStop > idx)
                    --mSelectedStop;
                _fireOnChange();
                setDirty(false);
                e.preventDefault();
                e.stopPropagation();
            }
        });

        element.addEventListener("mousedown", [](glint_event& e) {
            e.stopPropagation();
        });
    }

    const char* typeName() const override { return "gradient-editor"; }

    // ── Destructor — dismiss floating picker before tree linkage is torn down ──
    ~glint_gradient_editor() override
    {
        *mAlive = false;   // prevent onClosed from touching the dead object
        _dismissFloatingPicker();
    }

    // ── setValue — set all stops programmatically without firing onChange ─────
    void setStops(std::vector<sk_gradient_stop> s)
    {
        stops = std::move(s);
        std::sort(stops.begin(), stops.end());
        _ensureDefaultStops();
        _selectStop(-1);     // deselect, hides picker
        setDirty(false);
    }

    // ── Layout override — keep controls anchored to top-right edge ─────────────
    void Layout(glint_canvas* pG) override
    {
        // mRect is set by the parent before calling our Layout(), so getContent()
        // already reflects the real measured width here. Compute the positions
        // and write them into BOTH style (inline layer) and computedStyle before
        // calling the base layout pass — the absolute-child positioning code reads
        // computedStyle.left directly (before child->Layout merges style into it).
        const glint_rect content = getContent();
        const float contentW = std::max(0.f, content.W());
        const float reservedW = _reservedSideColumnWidth(contentW);

        const float typeBtnLeft = contentW - reservedW;
        const float dialLeft    = typeBtnLeft + kGE_TypeBtnW + kGE_TypeBtnGap + kGE_DialGapLeft;

        auto _setLeft = [](glint_element* el, float v) {
            if (!el) return;
            el->style.left         = v;
            el->computedStyle.left = v;
        };

        if (mTypeBtn)
        {
            _setLeft(mTypeBtn, typeBtnLeft);
            mTypeBtn->style.top         = 0.f;
            mTypeBtn->computedStyle.top = 0.f;
        }
        if (mDialComp)
        {
            _setLeft(mDialComp, dialLeft);
            mDialComp->angle = direction;
        }
        if (mAngleInput)
        {
            _setLeft(mAngleInput, dialLeft);
            char _angBuf[8];
            snprintf(_angBuf, sizeof(_angBuf), "%d", (int)std::round(direction));
            if (mAngleInput->getValue() != _angBuf)
                mAngleInput->setValue(_angBuf);
        }
        if (mRadiusInput)
            _setLeft(mRadiusInput, dialLeft);

        glint_element::Layout(pG);
    }

    // ── Mouse ─────────────────────────────────────────────────────────────────
    // Ignore clicks that land on the type button or dial.
    bool _isOverTypeOrDial(float x, float y) const
    {
        if (mTypeBtn && mTypeBtn->mPaintRECT.Contains(x, y)) return true;
        if (!mDialComp || mDialComp->style.display == "none") return false;
        const float cx = mDialComp->mPaintRECT.MW();
        const float cy = mDialComp->mPaintRECT.MH();
        const float r  = kGE_DialSize * 0.5f + 2.f;
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
    }

    void OnMouseDown(float x, float y, const glint_mouse_mod&) override
    {
        if (_isOverTypeOrDial(x, y)) return;

        // Center dot / radius edge drag for radial and conic (checked before stop handles).
        const int ch = _hitCenterOrRadius(x, y);
        if (ch == 1) {
            _dismissFloatingPicker();
            mSelectedStop   = -1;
            mDraggingCenter = true;
            _updateCenterFromMouse(x, y);
            if (onCenterChange) onCenterChange(centerX, centerY);
            setDirty(false);
            return;
        }
        if (ch == 2) {
            _dismissFloatingPicker();
            mSelectedStop   = -1;
            mDraggingRadius = true;
            _updateRadiusFromMouse(x, y);
            _syncRadiusInput();
            if (onRadiusChange) onRadiusChange(radius);
            setDirty(false);
            return;
        }

        const int idx = _hitHandle(x, y);
        if (idx >= 0) {
            // Selecting a different stop — dismiss any open picker.
            if (idx != mSelectedStop)
                _dismissFloatingPicker();
            mSelectedStop = idx;
            mDragging    = true;
            mDragMoved   = false;
            mDragOrigPos = stops[idx].position;
        } else if (_inRampOrHandleZone(x, y)) {
            // Click on empty ramp or handle zone → add a new stop.
            _dismissFloatingPicker();
            const float pos = _rampXToPos(x);
            sk_gradient_stop ns;
            ns.position = pos;
            ns.color    = _sampleGradient(pos);
            stops.push_back(ns);
            std::sort(stops.begin(), stops.end());
            const int ni = _findNearestStop(pos);
            mSelectedStop = ni;
            mDragging    = true;
            mDragMoved   = false;
            mDragOrigPos = pos;
            _fireOnChange();
        } else {
            _selectStop(-1);
        }
        setDirty(false);
    }

    void OnMouseDrag(float x, float y, float, float, const glint_mouse_mod&) override
    {
        if (mDraggingCenter) {
            _updateCenterFromMouse(x, y);
            if (onCenterChange) onCenterChange(centerX, centerY);
            setDirty(false);
            return;
        }
        if (mDraggingRadius) {
            _updateRadiusFromMouse(x, y);
            _syncRadiusInput();
            if (onRadiusChange) onRadiusChange(radius);
            setDirty(false);
            return;
        }
        if (!mDragging || mSelectedStop < 0 || mSelectedStop >= (int)stops.size()) return;
        const float pos = std::clamp(_rampXToPos(x), 0.f, 1.f);
        // Track whether the stop actually moved from its original position.
        if (!mDragMoved) {
            const float rampW = _rampRect().W();
            if (rampW > 0.f && std::fabs(pos - mDragOrigPos) * rampW > 2.f)
                mDragMoved = true;
        }
        stops[mSelectedStop].position = pos;
        // Re-sort and track the dragged stop by proximity to the new position.
        std::sort(stops.begin(), stops.end());
        mSelectedStop = _findNearestStop(pos);
        _fireOnChange();
        setDirty(false);
    }

    void OnMouseUp(float x, float y, const glint_mouse_mod&) override
    {
        mDraggingCenter = false;
        mDraggingRadius = false;
        // Show colorpicker on release only when the stop was not dragged.
        if (mDragging && !mDragMoved && mSelectedStop >= 0)
            _showOrUpdateFloatingPicker();
        mDragging = false;
    }

    void Draw(glint_canvas& g) override
    {
        _syncFromPublicState();
        glint_element::Draw(g);
    }

    // ── drawContent ──────────────────────────────────────────────────────────
    void drawContent(glint_canvas& g) override
    {
        if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext())) {
            _drawSkia(canvas);
            return;
        }
        _drawFallback(g);
    }

    void DrawContentToCanvas(SkCanvas* canvas) override { _drawSkia(canvas); }

private:
    // ── Private state ─────────────────────────────────────────────────────────
    int   mSelectedStop = -1;
    bool  mDragging     = false;
    bool  mDragMoved    = false;
    float mDragOrigPos  = 0.f;

    // Center/radius drag state (for radial and conic types).
    bool  mDraggingCenter = false;
    bool  mDraggingRadius = false;

    // Child pointers for the direction controls.
    GE_TypeButton*    mTypeBtn     = nullptr;  // gradient type cycle button
    glint_dial*       mDialComp    = nullptr;
    glint_input*      mAngleInput  = nullptr;
    glint_input*      mRadiusInput = nullptr;  // shown in radial mode (replaces dial+angle)

    // Floating picker window — self-deletes after close(); null your pointer immediately.
    glint_colorpicker_window* mFloatingPicker = nullptr;

    // Alive guard: shared with picker callbacks so they don't touch a destroyed editor.
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    bool mSyncInitialized = false;
    float mLastSyncedDirection = 0.f;
    std::string mLastSyncedGradientType = "linear";
    float mLastSyncedCenterX = 0.5f;
    float mLastSyncedCenterY = 0.5f;
    float mLastSyncedRadius = 1.f;
    std::vector<sk_gradient_stop> mLastSyncedStops;

    static bool _sameColor(const glint_color& lhs, const glint_color& rhs)
    {
        return lhs.A == rhs.A && lhs.R == rhs.R && lhs.G == rhs.G && lhs.B == rhs.B;
    }

    bool _sameStops() const
    {
        if (mLastSyncedStops.size() != stops.size()) return false;
        for (std::size_t stopIndex = 0; stopIndex < stops.size(); ++stopIndex)
        {
            const auto& lhs = mLastSyncedStops[stopIndex];
            const auto& rhs = stops[stopIndex];
            if (lhs.position != rhs.position) return false;
            if (!_sameColor(lhs.color, rhs.color)) return false;
        }
        return true;
    }

    void _captureSyncedState()
    {
        mSyncInitialized = true;
        mLastSyncedDirection = direction;
        mLastSyncedGradientType = gradientType;
        mLastSyncedCenterX = centerX;
        mLastSyncedCenterY = centerY;
        mLastSyncedRadius = radius;
        mLastSyncedStops = stops;
    }

    void _syncFromPublicState()
    {
        if (!mSyncInitialized
            || mLastSyncedDirection != direction
            || mLastSyncedGradientType != gradientType
            || mLastSyncedCenterX != centerX
            || mLastSyncedCenterY != centerY
            || mLastSyncedRadius != radius
            || !_sameStops())
        {
            _ensureDefaultStops();

            if (mSelectedStop >= static_cast<int>(stops.size()))
                _selectStop(-1);

            if (mDialComp)
                mDialComp->angle = direction;

            if (mAngleInput && !mAngleInput->mIsFocused)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", (int)std::round(direction));
                mAngleInput->setValue(buf);
            }

            if (mRadiusInput && !mRadiusInput->mIsFocused)
                _syncRadiusInput();

            _syncTypeControls();

            if (mFloatingPicker && mSelectedStop >= 0 && mSelectedStop < static_cast<int>(stops.size()))
                _showOrUpdateFloatingPicker();

            _captureSyncedState();
        }
    }

    // ── Sync control visibility to the current gradientType ───────────────────
    // Linear: dial + angle input shown.
    // Radial: dial hidden (no angle); center dot + radius ring on ramp.
    // Conic:  dial + angle input shown (sweep start); center dot on ramp.
    void _syncTypeControls()
    {
        const bool isLinear = (gradientType == "linear");
        const bool isConic  = (gradientType == "conic");
        if (mDialComp)    mDialComp->style.display    = (isLinear || isConic) ? "flex" : "none";
        if (mAngleInput)  mAngleInput->style.display   = (isLinear || isConic) ? "flex" : "none";
        if (mRadiusInput) mRadiusInput->style.display  = (gradientType == "radial") ? "flex" : "none";
        if (mTypeBtn) setDirty(false);
    }

    // ── Sync radius input text to current radius value ──────────────────────
    void _syncRadiusInput()
    {
        if (!mRadiusInput) return;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", radius);
        mRadiusInput->setValue(buf);
    }

    // ── Ensure at least 2 default stops ───────────────────────────────────────
    void _ensureDefaultStops()
    {
        if (stops.empty()) {
            stops.push_back({ 0.f, glint_color(255, 0,   0,   0)   });
            stops.push_back({ 1.f, glint_color(255, 255, 255, 255) });
        }
        std::sort(stops.begin(), stops.end());
    }

    // ── Geometry helpers ─────────────────────────────────────────────────────
    float _reservedSideColumnWidth(float contentW) const
    {
        return std::clamp(kGE_DialColumnW, 0.f, std::max(0.f, contentW - kGE_TriHW * 2.f));
    }

    glint_rect _rampRect() const
    {
        const glint_rect c = getContent();
        const float reservedW = _reservedSideColumnWidth(std::max(0.f, c.W()));
        const float minRight = c.L + kGE_TriHW * 2.f;
        const float rampRight = std::max(minRight, c.R - reservedW);
        return glint_rect(c.L, c.T, rampRight, c.T + kGE_RampH);
    }

    glint_rect _handleZoneRect() const
    {
        const glint_rect c = getContent();
        const float reservedW = _reservedSideColumnWidth(std::max(0.f, c.W()));
        const float minRight = c.L + kGE_TriHW * 2.f;
        const float handleRight = std::max(minRight, c.R - reservedW);
        return glint_rect(c.L, c.T + kGE_RampH, handleRight, c.T + kGE_BaseH);
    }

    float _handleCX(int idx) const
    {
        const glint_rect r = _rampRect();
        const float raw = r.L + stops[idx].position * r.W();
        // Clamp so the triangle/square stays fully inside the paint rect.
        return std::clamp(raw, r.L + kGE_TriHW, r.R - kGE_TriHW);
    }

    // Y-center of the clickable region for a handle (midpoint between triangle tip and square bottom)
    float _handleCY() const
    {
        const glint_rect r  = _rampRect();
        const float tipY = r.B - (kGE_TriH * 0.5f);                 // triangle tip slightly inside ramp
        const float sqB  = r.B + kGE_TriH * 0.5f + kGE_SqGap + kGE_SqSize;
        return (tipY + sqB) * 0.5f;
    }

    // Y of triangle tip (slightly overlaps ramp bottom)
    float _triTipY() const { return _rampRect().B - (kGE_TriH * 0.5f); }
    // Y of triangle base
    float _triBaseY() const { return _triTipY() + kGE_TriH; }

    bool _inRampOrHandleZone(float x, float y) const
    {
        // Treat both zones as one combined interactive strip (excludes the dial column on the right).
        const glint_rect ramp = _rampRect();
        const glint_rect zone(ramp.L, ramp.T, ramp.R, ramp.T + kGE_BaseH);
        return zone.Contains(x, y);
    }

    float _rampXToPos(float x) const
    {
        const glint_rect r = _rampRect();
        return std::clamp((x - r.L) / std::max(r.W(), 1.f), 0.f, 1.f);
    }

    // ── Center dot screen-space position ─────────────────────────────────────
    float _centerDotScreenX() const
    {
        const glint_rect r = _rampRect();
        return r.L + centerX * r.W();
    }
    float _centerDotScreenY() const
    {
        const glint_rect r = _rampRect();
        return r.T + centerY * r.H();
    }

    // ── Hit-test center dot and radius ring ───────────────────────────────────
    // Returns: 0=none, 1=center dot, 2=radius ring edge (radial only).
    int _hitCenterOrRadius(float x, float y) const
    {
        if (gradientType == "linear") return 0;
        const float dcx  = _centerDotScreenX();
        const float dcy  = _centerDotScreenY();
        const float dist = std::sqrt((x - dcx) * (x - dcx) + (y - dcy) * (y - dcy));
        if (dist <= 8.f) return 1;
        if (gradientType == "radial") {
            const glint_rect r        = _rampRect();
            const float radiusPx = radius * std::min(r.W(), r.H()) * 0.5f;
            if (std::fabs(dist - radiusPx) <= 5.f) return 2;
        }
        return 0;
    }

    // ── Update center from mouse coordinates (clamped to ramp rect) ───────────
    void _updateCenterFromMouse(float x, float y)
    {
        const glint_rect r = _rampRect();
        centerX = std::clamp((x - r.L) / std::max(r.W(), 1.f), 0.f, 1.f);
        centerY = std::clamp((y - r.T) / std::max(r.H(), 1.f), 0.f, 1.f);
    }

    // ── Update radius from mouse distance-to-center ───────────────────────────
    void _updateRadiusFromMouse(float x, float y)
    {
        const glint_rect r        = _rampRect();
        const float dcx      = _centerDotScreenX();
        const float dcy      = _centerDotScreenY();
        const float dist     = std::sqrt((x - dcx) * (x - dcx) + (y - dcy) * (y - dcy));
        const float minDim   = std::min(r.W(), r.H());
        radius = std::max(0.01f, dist / std::max(minDim * 0.5f, 1.f));
    }

    // ── Hit-test handles (reverse order — topmost drawn last) ─────────────────
    int _hitHandle(float x, float y) const
    {
        for (int i = (int)stops.size() - 1; i >= 0; --i) {
            const float cx  = _handleCX(i);
            const float tipY = _triTipY();
            const float sqB  = _triBaseY() + kGE_SqGap + kGE_SqSize;
            if (std::fabs(x - cx) <= kGE_HitR && y >= tipY && y <= sqB)
                return i;
        }
        return -1;
    }

    // ── Find stop index nearest to given ramp position ────────────────────────
    int _findNearestStop(float pos) const
    {
        int best = 0;
        float bestD = 1e9f;
        for (int i = 0; i < (int)stops.size(); ++i) {
            const float d = std::fabs(stops[i].position - pos);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    // ── Sample gradient at position [0..1] (for newly added stop colour) ──────
    glint_color _sampleGradient(float pos) const
    {
        if (stops.empty()) return glint_color(255, 127, 127, 127);
        if ((int)stops.size() == 1) return stops[0].color;

        // Find surrounding stops.
        int lo = 0, hi = (int)stops.size() - 1;
        for (int i = 0; i < (int)stops.size() - 1; ++i) {
            if (pos >= stops[i].position && pos <= stops[i + 1].position) {
                lo = i; hi = i + 1; break;
            }
        }
        const float span = stops[hi].position - stops[lo].position;
        const float t    = (span > 1e-6f)
            ? std::clamp((pos - stops[lo].position) / span, 0.f, 1.f)
            : 0.f;
        const glint_color& a = stops[lo].color;
        const glint_color& b = stops[hi].color;
        return glint_color(
            (int)std::round(a.A + t * (b.A - a.A)),
            (int)std::round(a.R + t * (b.R - a.R)),
            (int)std::round(a.G + t * (b.G - a.G)),
            (int)std::round(a.B + t * (b.B - a.B))
        );
    }

    // ── Show / dismiss the floating picker window ────────────────────────────

    void _showOrUpdateFloatingPicker()
    {
    #if GLINT_PLATFORM_IOS || (!defined(_WIN32) && !defined(OS_WIN) && !GLINT_PLATFORM_MAC)
        return;
    #elif GLINT_PLATFORM_MAC
        const int idx = mSelectedStop;
        if (idx < 0 || idx >= (int)stops.size()) return;
        if (!mRoot || !mRoot->macWindow) return;

        if (mFloatingPicker) {
            mFloatingPicker->destroy();
            mFloatingPicker = nullptr;
        }

        // Convert the handle's canvas-local (document-space) position to client
        // coordinates by subtracting the cumulative scroll offsets of all
        // scrollable ancestors, then convert to screen via contentRectToScreen.
        float docX = _handleCX(idx);
        float docY = _triTipY();
        for (auto* p = mParent; p; p = p->mParent) {
            docX -= p->mScrollLeft;
            docY -= p->mScrollTop;
        }
        const RECT anchor = mRoot->macWindow->contentRectToScreen(
          docX - 4, docY, 8, 1);

        auto alive = mAlive;
        mFloatingPicker = glint_colorpicker_window::open(stops[idx].color, anchor);
        mFloatingPicker->reopen(
            stops[idx].color,
            anchor,
            /*onChange=*/[this, alive](glint_color c) {
                if (!*alive) return;
                if (mSelectedStop < 0 || mSelectedStop >= (int)stops.size()) return;
                stops[mSelectedStop].color = c;
                _fireOnChange();
                setDirty(false);
            },
            /*onClosed=*/[this, alive]() {
                if (!*alive) return;
                mFloatingPicker = nullptr;
            }
        );
#else
        const int idx = mSelectedStop;
        if (idx < 0 || idx >= (int)stops.size()) return;
        if (!mRoot || !mRoot->hwnd) return;

        // Close any existing picker window before opening a new one.
        if (mFloatingPicker) {
            mFloatingPicker->destroy();
            mFloatingPicker = nullptr;
        }

        // Convert the handle's canvas-local (document-space) position to HWND
        // client coordinates by subtracting the cumulative scroll offsets of all
        // scrollable ancestors, then call ClientToScreen.
        float docX = _handleCX(idx);
        float docY = _triTipY();
        for (auto* p = mParent; p; p = p->mParent) {
            docX -= p->mScrollLeft;
            docY -= p->mScrollTop;
        }
        POINT pt = { (LONG)docX, (LONG)docY };
        ::ClientToScreen(mRoot->hwnd, &pt);
        const RECT anchor = { pt.x - 4, pt.y, pt.x + 4, pt.y };

        auto alive = mAlive;
        // open() starts the picker thread with the window hidden (showOnCreate
        // returns false).  reopen() repositions, seeds the picker, applies the
        // real callbacks, and shows the window — all in one atomic PostMessage.
        mFloatingPicker = glint_colorpicker_window::open(stops[idx].color, anchor);
        mFloatingPicker->reopen(
            stops[idx].color,
            anchor,
            /*onChange=*/[this, alive](glint_color c) {
                if (!*alive) return;
                if (mSelectedStop < 0 || mSelectedStop >= (int)stops.size()) return;
                stops[mSelectedStop].color = c;
                _fireOnChange();
                setDirty(false);
            },
            /*onClosed=*/[this, alive]() {
                if (!*alive) return;
                mFloatingPicker = nullptr;
            }
        );
#endif
    }

    void _dismissFloatingPicker()
    {
        if (!mFloatingPicker) return;
        mFloatingPicker->destroy();
        mFloatingPicker = nullptr;
    }

    // ── Select / deselect a stop ──────────────────────────────────────────────
    void _selectStop(int idx)
    {
        mSelectedStop = idx;
        if (idx < 0)
            _dismissFloatingPicker();
        else
            _showOrUpdateFloatingPicker();
        setDirty(false);
    }

    void _fireOnChange()
    {
        if (onChange) onChange(stops);
    }

    // ── Drawing ───────────────────────────────────────────────────────────────
    void _drawSkia(SkCanvas* canvas)
    {
        const glint_rect rampR   = _rampRect();
        const glint_rect handleR = _handleZoneRect();

        // ── Ramp (gradient bar) ──────────────────────────────────────────────
        if (!stops.empty() && rampR.W() > 0.f) {
            const SkRect skRamp = SkRect::MakeLTRB(rampR.L, rampR.T, rampR.R, rampR.B);

            // Checkerboard background so alpha is visible.
            sk_cp_draw_checkerboard(canvas, skRamp, 8.f);

            // Build colour / position arrays for SkGradientShader.
            std::vector<SkColor>  skCols(stops.size());
            std::vector<SkScalar> skPos(stops.size());
            for (int i = 0; i < (int)stops.size(); ++i) {
                const glint_color& c = stops[i].color;
                skCols[i] = SkColorSetARGB(c.A, c.R, c.G, c.B);
                skPos[i]  = (SkScalar)stops[i].position;
            }

            // Preview bar always shows left-to-right so stop colours are visible
            // regardless of the direction angle (angle is just metadata for rendering).
            SkPoint pts[2] = {
                { rampR.L, rampR.T },
                { rampR.R, rampR.T }
            };
            auto shader = SkGradientShader::MakeLinear(
                pts, skCols.data(), skPos.data(), (int)skCols.size(), SkTileMode::kClamp);
            SkPaint gp;
            gp.setShader(shader);
            canvas->drawRect(skRamp, gp);

            // Subtle border around ramp.
            SkPaint bp;
            bp.setStyle(SkPaint::kStroke_Style);
            bp.setColor(SkColorSetARGB(120, 80, 80, 80));
            bp.setStrokeWidth(1.f);
            canvas->drawRect(skRamp, bp);
        }

        // ── Handle zone background ────────────────────────────────────────────
        {
            SkPaint hp;
            hp.setColor(SkColorSetARGB(200, 24, 24, 24));
            canvas->drawRect(SkRect::MakeLTRB(handleR.L, handleR.T, handleR.R, handleR.B), hp);
        }

        // ── Draw each handle: up-pointing triangle + small colour square ─────
        const float tipY  = _triTipY();
        const float baseY = _triBaseY();
        const float sqT   = baseY + kGE_SqGap;
        for (int i = 0; i < (int)stops.size(); ++i) {
            const float   cx  = _handleCX(i);
            const glint_color& sc  = stops[i].color;
            const bool    sel = (i == mSelectedStop);

            // 80% white for normal stops, accent-yellow for the selected one.
            const SkColor accent = sel
                ? SkColorSetARGB(255, 255, 210, 60)
                : SkColorSetARGB(204, 255, 255, 255);

            // — Up-pointing triangle (tip overlaps gradient bar) ——————————————
            {
                SkPath tri;
                tri.moveTo(cx,              tipY);
                tri.lineTo(cx - kGE_TriHW,  baseY);
                tri.lineTo(cx + kGE_TriHW,  baseY);
                tri.close();

                SkPaint tp;
                tp.setStyle(SkPaint::kFill_Style);
                tp.setAntiAlias(true);
                tp.setColor(accent);
                canvas->drawPath(tri, tp);
            }

            // — Colour square —————————————————————————————————————————————
            const float sqL  = cx - kGE_SqSize * 0.5f;
            const SkRect sq  = SkRect::MakeLTRB(sqL, sqT, sqL + kGE_SqSize, sqT + kGE_SqSize);

            // Fill = stop colour
            {
                SkPaint fp;
                fp.setStyle(SkPaint::kFill_Style);
                fp.setColor(SkColorSetARGB(255, sc.R, sc.G, sc.B));
                canvas->drawRect(sq, fp);
            }
            // Border = 80% white (or accent)
            {
                SkPaint bp;
                bp.setStyle(SkPaint::kStroke_Style);
                bp.setStrokeWidth(1.f);
                bp.setAntiAlias(true);
                bp.setColor(accent);
                canvas->drawRect(sq, bp);
            }
        }

        // ── Center dot + radius ring for radial / conic ───────────────────────
        if (gradientType != "linear" && rampR.W() > 0.f && rampR.H() > 0.f) {
            const float dcx = _centerDotScreenX();
            const float dcy = _centerDotScreenY();

            canvas->save();
            canvas->clipRect(SkRect::MakeLTRB(rampR.L, rampR.T, rampR.R, rampR.B));

            // Radius ring (radial only) — draw first so dot is on top.
            if (gradientType == "radial") {
                const float radiusPx = radius * std::min(rampR.W(), rampR.H()) * 0.5f;
                SkPaint rp;
                rp.setStyle(SkPaint::kStroke_Style);
                rp.setColor(SkColorSetARGB(170, 255, 255, 255));
                rp.setStrokeWidth(1.f);
                rp.setAntiAlias(true);
                canvas->drawCircle(dcx, dcy, radiusPx, rp);
                // Cardinal tick marks on the ring — makes the draggable edge visible.
                constexpr float kTickLen = 5.f;
                for (float angDeg : { 0.f, 90.f, 180.f, 270.f }) {
                    const float rad = angDeg * 3.14159265f / 180.f;
                    canvas->drawLine(
                        dcx + (radiusPx - kTickLen) * std::cos(rad),
                        dcy + (radiusPx - kTickLen) * std::sin(rad),
                        dcx + (radiusPx + kTickLen) * std::cos(rad),
                        dcy + (radiusPx + kTickLen) * std::sin(rad),
                        rp);
                }
            }

            // Center dot (drawn on top of everything).
            constexpr float kDotR   = 5.f;
            constexpr float kCrossR = 8.f;

            // Drop shadow.
            SkPaint shadowP;
            shadowP.setStyle(SkPaint::kFill_Style);
            shadowP.setColor(SkColorSetARGB(100, 0, 0, 0));
            shadowP.setAntiAlias(true);
            canvas->drawCircle(dcx + 1.f, dcy + 1.f, kDotR, shadowP);

            // White fill.
            SkPaint dotP;
            dotP.setStyle(SkPaint::kFill_Style);
            dotP.setColor(SkColorSetARGB(220, 255, 255, 255));
            dotP.setAntiAlias(true);
            canvas->drawCircle(dcx, dcy, kDotR, dotP);

            // Dark crosshair lines.
            SkPaint crossP;
            crossP.setStyle(SkPaint::kStroke_Style);
            crossP.setColor(SkColorSetARGB(200, 40, 40, 40));
            crossP.setStrokeWidth(1.f);
            crossP.setAntiAlias(true);
            canvas->drawLine(dcx - kCrossR, dcy, dcx + kCrossR, dcy, crossP);
            canvas->drawLine(dcx, dcy - kCrossR, dcx, dcy + kCrossR, crossP);

            // Outer ring.
            SkPaint ringP;
            ringP.setStyle(SkPaint::kStroke_Style);
            ringP.setColor(SkColorSetARGB(160, 100, 100, 100));
            ringP.setStrokeWidth(1.f);
            ringP.setAntiAlias(true);
            canvas->drawCircle(dcx, dcy, kDotR, ringP);

            canvas->restore();
        }
    }

    void _drawFallback(glint_canvas& g)
    {
        const glint_rect rampR = _rampRect();

        // Simple solid-fill segments (no true gradient on non-Skia backends).
        if ((int)stops.size() >= 2) {
            for (int i = 0; i + 1 < (int)stops.size(); ++i) {
                const float xL = rampR.L + stops[i].position     * rampR.W();
                const float xR = rampR.L + stops[i + 1].position * rampR.W();
                g.FillRect(stops[i].color, glint_rect(xL, rampR.T, xR, rampR.B));
            }
        } else if (!stops.empty()) {
            g.FillRect(stops[0].color, rampR);
        }

        // Fallback: simple tick (triangle approximated as vertical bar) + square.
        const float tipY  = _triTipY();
        const float baseY = _triBaseY();
        const float sqT   = baseY + kGE_SqGap;
        for (int i = 0; i < (int)stops.size(); ++i) {
            const float   cx  = _handleCX(i);
            const glint_color& sc  = stops[i].color;
            const bool    sel = (i == mSelectedStop);
            const glint_color  tc  = sel ? glint_color(255, 255, 210, 60) : glint_color(200, 255, 255, 255);
            // triangle as narrow rect
            g.FillRect(tc, glint_rect(cx - kGE_TriHW, tipY, cx + kGE_TriHW, baseY));
            // colour square
            g.FillRect(glint_color(255, sc.R, sc.G, sc.B), glint_rect(cx - kGE_SqSize * 0.5f, sqT, cx + kGE_SqSize * 0.5f, sqT + kGE_SqSize));
        }
        // ── Center dot fallback for radial / conic ───────────────────────────
        if (gradientType != "linear") {
            const glint_rect r   = _rampRect();
            const float dcx = r.L + centerX * r.W();
            const float dcy = r.T + centerY * r.H();
            constexpr float kS = 5.f;
            g.FillRect(glint_color(220, 255, 255, 255), glint_rect(dcx - kS, dcy - 1.f, dcx + kS, dcy + 1.f));
            g.FillRect(glint_color(220, 255, 255, 255), glint_rect(dcx - 1.f, dcy - kS, dcx + 1.f, dcy + kS));
        }
    }
};

// New API name — both refer to the same class.
namespace { struct _glint_gradient_editor_reg { _glint_gradient_editor_reg() { glint_element::registerElement("gradient-editor", []{ return new glint_gradient_editor(); }); } } _glint_gradient_editor_reg_; }

// ─── GE_TypeButton out-of-class draw definitions ─────────────────────────────
// Draws "L", "R", or "C" in the center of the button using the owner's gradientType.

inline void GE_TypeButton::DrawContentToCanvas(SkCanvas* canvas)
{
    if (!mOwner) return;
    const char letter = (mOwner->gradientType == "radial") ? 'R'
                      : (mOwner->gradientType == "conic")  ? 'C'
                      :                                      'L';
    char str[2] = { letter, '\0' };

    SkFont font = skFont(11.f);
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(SkColorSetARGB(220, 200, 200, 200));

    SkRect bounds;
    font.measureText(str, 1, SkTextEncoding::kUTF8, &bounds);
    const float tx = mPaintRECT.MW() - bounds.centerX();
    const float ty = mPaintRECT.MH() - bounds.centerY();
    canvas->drawString(str, tx, ty, font, p);
}

inline void GE_TypeButton::drawContent(glint_canvas& g)
{
    if (!mOwner) return;
    const glint_rect c = getContent();
    // Simple label approximation on non-Skia backends.
    std::string s = (mOwner->gradientType == "radial") ? "R"
                  : (mOwner->gradientType == "conic")  ? "C"
                  :                                      "L";
    glint_text txt(11.f, glint_color(255, 200, 200, 200));
    g.DrawText(txt, s.c_str(), c);
}
