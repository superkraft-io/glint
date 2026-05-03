#pragma once

/**
 * glint_button.hpp
 * A clickable button component with normal / hover / pressed states.
 *
 * State transitions animate like a real browser — setting style.transition applies
 * to hover-in, hover-out, press, and release, not just explicit style mutations.
 * Mid-flight reversal is handled correctly: the animation starts from the current
 * interpolated position so there is no visual jump.
 *
 * Usage via the builder (preferred):
 *   add.button([](glint_button& _c) {
 *     _c.innerText = "Click me";
 *     _c.onClick = [] { ... };
 *     _c.style.backgroundColor   = "#2a2a2a";
 *     _c.style.color             = "#ffffff";
 *     _c.style.transition        = "background-color 150ms ease-out";
 *     _c.hover.backgroundColor   = "#3c3c3c";
 *     _c.pressed.backgroundColor = "#505050";
 *     _c.style.borderRadius      = 4.f;
 *   });
 */

#include "../glint_element.hpp"
#include "../glint_animator.hpp"
#include "../default_style.hpp"

#include <chrono>
#include <functional>
#include <string>

class glint_button : public glint_element
{
public:
  // ── Fields — set directly in the add.button() callback ───────────────────
  std::function<void()> onClick;
  glint_style hover;    // mouse-over state   (`style` inherited = normal state)
  glint_style pressed;  // mouse-down state
  // tag is inherited from glint_element.

  glint_button()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
    mCssStyleBase = computedStyle;
    mStateComputedStyle = computedStyle;
    mLastStateTick = std::chrono::steady_clock::now();
  }

  // ── Accessors ──────────────────────────────────────────────────────────────

  void SetLabel  (const std::string& s)     { innerText = s; setDirty(false); }
  void SetOnClick(std::function<void()> cb) { onClick = std::move(cb); }

  const char* typeName() const override { return "button"; }

  // ── Mouse events ───────────────────────────────────────────────────────────

  void OnMouseOver(float /*x*/, float /*y*/, const glint_mouse_mod& /*mod*/) override
  {
    // mIsHovered is set by glint_document pass-1 before this is called,
    // so do NOT guard on it — just start the transition unconditionally.
    _startStateTransition();
    setDirty(false);
  }

  void OnMouseOut() override
  {
    // Update the CSS baseline only when NOT in :active state.  If the element
    // is currently active (user is dragging out while pressed), cssStyle_ still
    // contains the :active rules and would produce a wrong "no-pseudo" snapshot.
    // The baseline was already correctly set during the last idle Draw() frame.
    if (!mIsActive)
      mCssStyleBase = cssStyle_;
    mIsHovered = mIsPressed = false;
    _startStateTransition();
    setDirty(false);
  }

  void OnMouseDown(float /*x*/, float /*y*/, const glint_mouse_mod& /*mod*/) override
  {
    mIsPressed = true;
    _startStateTransition();
    setDirty(false);
  }

  void OnMouseUp(float x, float y, const glint_mouse_mod& /*mod*/) override
  {
    const bool wasPressed = mIsPressed;
    mIsPressed = false;
    _startStateTransition();
    setDirty(false);
    if (wasPressed && mRect.Contains(x, y) && onClick) onClick();
  }

  // ── Draw ──────────────────────────────────────────────────────────────────

  void Draw(glint_canvas& g) override
  {
    computedStyle = _mergedStyle();           // freshen CSS cascade (incl. :hover rules)
    if (computedStyle.display == "none") return;
    // Keep the non-pseudo CSS baseline in sync every frame we are not hovering/pressing,
    // so the next hover delta reads the correct pre-hover cssStyle_ values.
    if (!mIsHovered && !mIsPressed)
    {
      mCssStyleBase = cssStyle_;
      if (mStateTransitions.empty())
        mStateComputedStyle = computedStyle;
    }
    if (_tickStateTransitions()) setDirty(false);  // keep animating

    glint_rect _expandedRECT = mRect;
    if (mFilterPad > 0.f) mRect = mPaintRECT;

    const bool _hasFilter = !style.filter.empty() && style.filter != "none";
    if (_hasFilter) glint_filter::BeginLayer(g, mRect, style.filter);
    // Apply animated transform (mirrors DrawToCanvas path).
    SkCanvas* _sc = static_cast<SkCanvas*>(g.GetDrawContext());
    const SkM44 _tmat = mStateComputedStyle.ResolveTransformMatrix(mRect.W(), mRect.H(), mRect.MW(), mRect.MH());
    const bool _hasTx = _sc && !(_tmat == SkM44{});
    if (_hasTx)
    {
      _sc->save();
      _sc->concat(_tmat);
    }
    DrawBackground(g, mStateComputedStyle);
    std::vector<glint_element*> _drawOrder;
    _drawOrder.reserve(mChildren.size());
    for (auto& child : mChildren)
      _drawOrder.push_back(child.get());
    std::stable_sort(_drawOrder.begin(), _drawOrder.end(),
      [](const glint_element* a, const glint_element* b) {
        return a->computedStyle.zIndex < b->computedStyle.zIndex;
      });

    for (auto* c : _drawOrder)
    {
      if (c->computedStyle.zIndex >= 0) break;
      c->Draw(g);
    }

    mActiveStyle = &mStateComputedStyle;
    drawContent(g);

    // Recurse children (e.g. an svg icon nested inside a button).
    // Mirrors the base draw path's negative/non-negative z-index split.
    for (auto* c : _drawOrder)
    {
      if (c->computedStyle.zIndex < 0) continue;
      c->Draw(g);
    }

    // Border paints on top of content — mirrors DrawToCanvas and the base _drawImpl.
    if (_sc) _drawBorderSkia(_sc, mStateComputedStyle, mRect);
    if (_hasTx) _sc->restore();
    if (_hasFilter) glint_filter::EndLayer(g);

    if (mFilterPad > 0.f) mRect = _expandedRECT;
  }

protected:
  void drawContent(glint_canvas& g) override
  {
    const glint_style& s = mActiveStyle ? *mActiveStyle : style;
    const std::string _fid_ = glint_font_registry::resolveFontFaceId(
        s.fontFamily.c_str(), (int)s.fontWeight, s.fontStyle.c_str());
    const char* _fn_ = _fid_.empty()
        ? (s.fontFamily.empty() ? nullptr : s.fontFamily.c_str())
        : _fid_.c_str();
        glint_text t(style.fontSize.toFloat(), ApplyOpacity(s.color.value, s.opacity), _fn_,
          s.textAlign, EVAlign::Middle);
    g.DrawText(t, innerText.c_str(), getContent());
  }

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (!canvas || innerText.empty()) return;

    const glint_style& s = mActiveStyle ? *mActiveStyle : style;
    const float fontSize = s.fontSize.toFloat() > 0.f ? s.fontSize.toFloat() : 12.f;
    SkFont font = skFont(fontSize, s.fontFamily.c_str(), s.fontWeight, s.fontStyle.c_str());
    const float textW = font.measureText(innerText.c_str(), innerText.size(), SkTextEncoding::kUTF8);
    SkPaint tp;
    tp.setColor(skColor(ApplyOpacity(s.color.value, s.opacity)));
    tp.setAntiAlias(true);

    const glint_rect r = getContent();
    float textX;
    if (s.textAlign == EAlign::Far)
      textX = r.R - textW;
    else if (s.textAlign == EAlign::Center)
      textX = r.L + (r.W() - textW) * 0.5f;
    else
      textX = r.L;

    const float textY = r.T + r.H() * 0.5f + fontSize * 0.35f;
    canvas->drawString(innerText.c_str(), textX, textY, font, tp);

    const std::string& td = s.textDecoration;
    const bool doStrike = td == "line-through" || td == "underline line-through" || td == "line-through underline";
    const bool doUnderline = td == "underline" || td == "underline line-through" || td == "line-through underline";
    if (doStrike || doUnderline)
    {
      const float adv = font.measureText(innerText.c_str(), innerText.size(), SkTextEncoding::kUTF8);
      SkFontMetrics metrics;
      font.getMetrics(&metrics);
      SkPaint lp;
      lp.setAntiAlias(true);
      lp.setColor(tp.getColor());
      lp.setStyle(SkPaint::kStroke_Style);
      if (doStrike)
      {
        lp.setStrokeWidth(std::max(0.5f, fontSize / 14.f));
        const float ly = textY + (metrics.fStrikeoutPosition != 0.f ? metrics.fStrikeoutPosition : -metrics.fXHeight * 0.5f);
        canvas->drawLine(textX, ly, textX + adv, ly, lp);
      }
      if (doUnderline)
      {
        const float thickness = std::max(1.f, std::round(metrics.fUnderlineThickness > 0.f ? metrics.fUnderlineThickness : std::max(1.f, fontSize / 14.f)));
        lp.setStrokeWidth(thickness);
        const float rawLy = textY + (metrics.fUnderlinePosition != 0.f ? metrics.fUnderlinePosition : fontSize * 0.08f);
        const float ly = std::floor(rawLy) + 0.5f;
        canvas->drawLine(textX, ly, textX + adv, ly, lp);
      }
    }
  }

  bool               mIsHovered   = false;
  bool               mIsPressed   = false;
  const glint_style* mActiveStyle = nullptr;
  // mCssStyleBase is inherited from glint_element (glint_element_style.hpp)

  // ── State-transition animation (hover / pressed) ──────────────────────────
  glint_style  mStateComputedStyle;
  std::vector<glint_transition_entry> mStateTransitions;
  std::chrono::steady_clock::time_point mLastStateTick;
  bool mStateTransJustStarted = false;

  /** The style we are animating toward for the current hover/pressed state. */
  const glint_style& _stateTarget() const
  {
    return mIsPressed ? pressed : mIsHovered ? hover : computedStyle;
  }

  /**
   * CSS-accurate target: computedStyle with only the explicitly-overridden
   * properties from hover/pressed applied on top.
   * A default-constructed glint_style acts as the zero baseline: any property
   * in hover/pressed that differs from the default is considered "set" and
   * overrides the matching property in computedStyle.  Everything else keeps
   * the element's fully-resolved base value — matching exactly how browsers
   * handle :hover / :active selectors.
   */
  glint_style _buildMergedTarget() const
  {
    glint_style merged = _mergedStyle();  // always use fresh CSS merge as base
    if (!mIsPressed && !mIsHovered) return merged;

    // 1. Apply C++ hover/pressed field overrides (existing mechanism).
    const glint_style& over = mIsPressed ? pressed : hover;
    const glint_style  def;           // zero-default reference
    for (const auto& key : glint_animatable_keys())
    {
      const std::string overVal = glint_style_get_by_name(over, key);
      const std::string defVal  = glint_style_get_by_name(def,  key);
      if (overVal != defVal)
        glint_style_lerp_by_name(merged, key, defVal, overVal, 1.f);
    }

    // 2. Apply CSS pseudo-class delta — per CSS spec, inline styles win over
    // pseudo-class rules unless the pseudo rule carries !important.
    {
      static const glint_style sDefaultStyle{};
      for (const auto& key : glint_animatable_keys())
      {
        const std::string cssNow  = glint_style_get_by_name(cssStyle_,     key);
        const std::string cssBase = glint_style_get_by_name(mCssStyleBase, key);
        if (cssNow != cssBase)
        {
          if (mCssImportantProps_.count(key) == 0)
          {
            const std::string inlineVal  = glint_style_get_by_name(style,          key);
            const std::string defaultVal = glint_style_get_by_name(sDefaultStyle,  key);
            if (inlineVal != defaultVal) continue;  // inline style wins
          }
          glint_style_lerp_by_name(merged, key, cssBase, cssNow, 1.f);
        }
      }
    }

    return merged;
  }

  /**
   * Call after every state change (hover/pressed toggled).
   * Builds transition entries from the current mStateComputedStyle toward the new
   * target. If no transition is declared the entries list is cleared so
   * _tickStateTransitions() will snap instantly.
   */
  void _startStateTransition()
  {
    // Use computedStyle.transition so CSS-defined transitions (not just inline) work.
    const auto rawSpecs = glint_parse_transition(computedStyle.transition);

    if (rawSpecs.empty())
    {
      mStateTransitions.clear();  // _tickStateTransitions will snap on next frame
      return;
    }

    // Expand "all" to the full animatable key list
    std::vector<glint_transition_spec> specs;
    for (const auto& sp : rawSpecs)
    {
      if (sp.key == "all")
        for (const auto& k : glint_animatable_keys())
          specs.push_back({k, sp.durationMs, sp.easing});
      else
        specs.push_back(sp);
    }

    const glint_style target = _buildMergedTarget();

    // Build entries — from current animated position (correct mid-flight reversal)
    std::vector<glint_transition_entry> entries;
    for (const auto& sp : specs)
    {
      const std::string fromVal = glint_style_get_by_name(mStateComputedStyle, sp.key);
      const std::string toVal   = glint_style_get_by_name(target, sp.key);
      if (fromVal == toVal) continue;
      entries.push_back({sp.key, fromVal, toVal, sp.durationMs, 0.f, sp.easing});
    }

    mStateTransitions      = std::move(entries);
    mLastStateTick         = std::chrono::steady_clock::now();
    mStateTransJustStarted = true;
  }

  /**
   * Advance state transitions by wall-clock dt. Writes into mStateComputedStyle.
   * Returns true if transitions are still running (caller should call setDirty).
   * When no transitions are active it snaps mStateComputedStyle to the target.
   */
  bool _tickStateTransitions()
  {
    auto now = std::chrono::steady_clock::now();
    const float dtMs = mStateTransJustStarted ? 0.f
        : std::chrono::duration<float, std::milli>(now - mLastStateTick).count();
    mLastStateTick         = now;
    mStateTransJustStarted = false;

    if (mStateTransitions.empty())
    {
      // Snap: normal state tracks base computedStyle (base animator may be running)
      mStateComputedStyle = _buildMergedTarget();
      return false;
    }

    bool anyRunning = false;
    for (auto& e : mStateTransitions)
    {
      e.elapsedMs  += dtMs;
      const float rawT = (e.durationMs > 0.f)
          ? std::min(1.f, e.elapsedMs / e.durationMs) : 1.f;
      const float t    = glint_ease_eval(e.easing, rawT);
      glint_style_lerp_by_name(mStateComputedStyle, e.key, e.fromVal, e.toVal, t);
      if (rawT < 1.f) anyRunning = true;
    }

    if (!anyRunning) mStateTransitions.clear();
    return anyRunning;
  }

  /**
   * SkCanvas draw path — used by the inspector window.
   * Draws background using the animated state style then text.
   */
  void DrawToCanvas(SkCanvas* canvas) override
  {
    computedStyle = _mergedStyle();           // freshen CSS cascade (incl. :hover rules)
    if (computedStyle.display == "none") return;
    if (!mIsHovered && !mIsPressed)
      mCssStyleBase = cssStyle_;
    if (_tickStateTransitions()) setDirty(false);

    const glint_style& active = mStateComputedStyle;

    // Apply animated transform (e.g. :hover { transform: scale(1.1) })
    const SkM44 _tmat = active.ResolveTransformMatrix(mRect.W(), mRect.H(), mRect.MW(), mRect.MH());
    const bool hasTransform = !(_tmat == SkM44{});
    if (hasTransform)
    {
      canvas->save();
      canvas->concat(_tmat);
    }

    // Background
    const glint_color bg  = ApplyOpacity(active.backgroundColor.value, active.opacity);
    const glint_color brd = ApplyOpacity(active.borderColor.value,     active.opacity);
    const float  br  = active.borderRadius.resolve(std::min(mRect.W(), mRect.H()));
    if (bg.A > 0)
    {
      SkPaint p;  p.setColor(skColor(bg));  p.setAntiAlias(true);
      if (br > 0.f)
        canvas->drawRoundRect(skRect(mRect), br, br, p);
      else
        canvas->drawRect(skRect(mRect), p);
    }
    if (active.borderWidth > 0.f && brd.A > 0)
    {
      SkPaint p;  p.setColor(skColor(brd));  p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style);  p.setStrokeWidth(active.borderWidth);
      if (br > 0.f)
        canvas->drawRoundRect(skRect(mRect), br, br, p);
      else
        canvas->drawRect(skRect(mRect), p);
    }

    std::vector<glint_element*> _drawOrder;
    _drawOrder.reserve(mChildren.size());
    for (auto& child : mChildren)
      _drawOrder.push_back(child.get());
    std::stable_sort(_drawOrder.begin(), _drawOrder.end(),
      [](const glint_element* a, const glint_element* b) {
        return a->computedStyle.zIndex < b->computedStyle.zIndex;
      });

    for (auto* child : _drawOrder)
    {
      if (child->computedStyle.zIndex >= 0) break;
      child->DrawToCanvas(canvas);
    }

    mActiveStyle = &mStateComputedStyle;
    DrawContentToCanvas(canvas);

    // Recurse children (rare for buttons but supported)
    for (auto* child : _drawOrder)
    {
      if (child->computedStyle.zIndex < 0) continue;
      child->DrawToCanvas(canvas);
    }

    mActiveStyle = nullptr;

    if (hasTransform) canvas->restore();
  }
};

// New API name — both refer to the same class.
namespace { struct _glint_button_reg { _glint_button_reg() { glint_element::registerElement("button", []{ return new glint_button(); }); } } _glint_button_reg_; }
