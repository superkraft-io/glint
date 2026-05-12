#pragma once

/**
 * glint_element_style.hpp
 * CSS transition animation state + tickTransitions() for glint_element.
 *
 * Included INSIDE the glint_element class body by glint_element.hpp.
 * Contains private fields and the tickTransitions() method that drives
 * CSS transition interpolation every frame.
 *
 * Why here: tickTransitions() is ~120 lines; placing it in a sub-header
 * keeps glint_element.hpp focused on the class interface.
 */

  // ── Protected helpers ──────────────────────────────────────────────────────

  // ── CSS transition animation state ─────────────────────────────────────────
  // Used by tickTransitions(), called at the start of Draw() and DrawToCanvas().

  /** Cached parse of style.transition — re-parsed when the string changes. */
  std::string                         mLastTransStr_;
  std::vector<glint_transition_spec>  mParsedTransSpecs_;

  /** Currently running interpolations — one per animating property. */
  std::vector<glint_transition_entry> mActiveTransitions_;

  /** Snapshot of the MERGED style (CSS + inline cascade, no transition overrides)
   *  as of the last tickTransitions() call.  Change detection compares this
   *  against _mergedStyle() to detect target value changes across frames.
   *  Using the merged base (not raw inline) means CSS-only property changes
   *  are detected, while in-flight lerp values never trigger false restarts. */
  glint_style                         mPrevStyle_;

  /** Timing state for computing per-frame dt. */
  std::chrono::steady_clock::time_point mLastTransTick_;
  bool                                  mHasLastTick_ = false;
  bool                                  mSkipNextTick_ = false;

  // ── CSS animation (@keyframes) state ───────────────────────────────────────────
  /** Pointer to the document’s @keyframes registry — set by addChild().
   *  Null when no document registry exists (standalone element). */
  const glint_keyframe_registry* mKeyframeRegistryPtr_ = nullptr;

  /** Cached parse of the merged `animation:` string. Re-parsed on change. */
  std::string                       mLastAnimStr_;
  std::vector<glint_animation_spec>  mParsedAnimSpecs_;

  /** In-flight @keyframes animations — one entry per active animation. */
  std::vector<glint_animation_entry> mActiveAnimations_;

  /**
   * Sync computedStyle from style, tick in-flight transitions, and chain
   * redraws while any animation is running.
   *
   * Called at the start of Draw() and DrawToCanvas() so every component
   * manages its own animation timeline independently.
   *
   * Fast path: when style.transition == "" and no active transitions,
   * this is just a struct copy (computedStyle = style) with no allocation.
   */
  void tickTransitions()
  {
    if (mSkipNextTick_)
    {
      mSkipNextTick_ = false;
      return;
    }

    // Resolve the active transition spec from the merged cascade:
    // inline style.transition wins when set; cssStyle_.transition is the fallback.
    // This matches Chrome: a stylesheet `transition: opacity 300ms` drives animations
    // even when no JS/C++ code sets style.transition inline.
    const std::string& mergedTransition = !style.transition.empty()
                                          ? style.transition : cssStyle_.transition;

    // Re-parse spec list when the merged transition string changes.
    if (mergedTransition != mLastTransStr_)
    {
      mLastTransStr_     = mergedTransition;
      mParsedTransSpecs_ = glint_parse_transition(mergedTransition);
    }

    // Compute the new merged style (CSS + inline cascade).
    // We do this unconditionally so that even the fast path sets computedStyle
    // via the canonical merge path, and so change detection can compare the
    // previous computed value against the new computed value.
    // This is the correct Chrome-spec approach: transitions fire when the
    // *computed* value of a property changes, regardless of whether the change
    // originated from an inline assignment or a CSS rule.
    const glint_style newMerged = _mergedStyle();

    // Resolve the merged animation string early so we can check whether there
    // are any animations before deciding whether to take the fast path.
    const std::string& mergedAnim = !style.animation.empty()
                                    ? style.animation : cssStyle_.animation;
    const bool hasAnimSpecs = !mergedAnim.empty() && mergedAnim != "none";
    const bool hasActiveAnims = !mActiveAnimations_.empty();

    // Hidden nodes must not tick transitions or keyframes. In Chrome semantics,
    // display:none removes the element from rendering, so its animations stop
    // advancing and must not keep the host repaint loop alive while hidden.
    if (newMerged.display == "none")
    {
      computedStyle = newMerged;
      mPrevStyle_ = newMerged;
      mActiveTransitions_.clear();
      mActiveAnimations_.clear();
      mHasLastTick_ = false;
      return;
    }

    // Fast path: nothing to animate (no transitions AND no animations).
    if (mParsedTransSpecs_.empty() && mActiveTransitions_.empty()
        && !hasAnimSpecs && !hasActiveAnims)
    {
      computedStyle = newMerged;
      mPrevStyle_   = newMerged;
      return;
    }

    // ── Detect property changes → start / amend transitions ──────────────────
    // Compare the PREVIOUS MERGED BASE (mPrevStyle_, set at end of last tick,
    // CSS+inline with NO transition overrides) against the NEW MERGED BASE
    // (newMerged, also no overrides).  This detects changes at the CSS/inline
    // level — i.e. when the *target* value changes — while ignoring the
    // intermediate lerped values held in computedStyle (which would otherwise
    // trigger false "reversing" restarts every frame and cause Zeno's paradox).
    //
    // The "from" value for a new or reversed transition is always taken from
    // computedStyle (the actual visual state at the time of the change).
    bool anyNewEntries = false;  // true when brand-new (not reversed) entries are added
    auto processTransKey = [&](const std::string& propKey, const glint_transition_spec& spec)
    {
      const std::string oldVal = glint_style_get_by_name(mPrevStyle_,  propKey);
      const std::string newVal = glint_style_get_by_name(newMerged,    propKey);
      if (oldVal == newVal) return;

      // The visual start point is the current in-flight computed value so that
      // reversals and interruptions continue smoothly from the current position.
      const std::string fromVal = glint_style_get_by_name(computedStyle, propKey);

      auto it = std::find_if(mActiveTransitions_.begin(), mActiveTransitions_.end(),
                             [&](const glint_transition_entry& e) { return e.key == propKey; });

      if (it != mActiveTransitions_.end())
      {
        // Already animating: browser reversing — restart from in-flight visual position.
        it->fromVal    = fromVal;
        it->toVal      = newVal;
        it->elapsedMs  = 0.f;
        it->durationMs = spec.durationMs;
        it->easing     = spec.easing;
      }
      else
      {
        glint_transition_entry entry;
        entry.key        = propKey;
        entry.fromVal    = fromVal;  // current visual state (may equal oldVal when idle)
        entry.toVal      = newVal;
        entry.durationMs = spec.durationMs;
        entry.elapsedMs  = 0.f;
        entry.easing     = spec.easing;
        mActiveTransitions_.push_back(entry);
        anyNewEntries = true;
      }
    };

    for (const auto& spec : mParsedTransSpecs_)
    {
      if (spec.key == "all")
      {
        for (const auto& propKey : glint_animatable_keys())
          processTransKey(propKey, spec);
      }
      else
      {
        processTransKey(spec.key, spec);
      }
    }

    // ── Compute dt (ms since last tick, clamped to 100 ms to skip freeze jumps) ──
    const auto now = std::chrono::steady_clock::now();
    float dtMs = 16.f;
    if (mHasLastTick_)
    {
      dtMs = std::chrono::duration<float, std::milli>(now - mLastTransTick_).count();
      dtMs = std::max(0.f, std::min(dtMs, 100.f));
    }
    mLastTransTick_ = now;
    mHasLastTick_   = true;

    // When brand-new entries were added this frame, the timestamp in mLastTransTick_
    // may be stale (components with transitions set but no active animation don't
    // self-redraw, so the last tick could be seconds ago). Zero the delta so the
    // first rendered frame is always at t=0 — the correct starting value — and
    // timing starts fresh from mLastTransTick_ = now set above.
    if (anyNewEntries) dtMs = 0.f;

    // ── Start from the newly merged style, then overwrite animated properties ────
    // newMerged was computed once before change-detection above; using it here
    // avoids a second (expensive) _mergedStyle() call and ensures the transition
    // lerp is applied on top of the correct CSS+inline base.
    computedStyle = newMerged;
    // Snapshot the merged base for next frame's change detection.
    // Must be newMerged (CSS+inline, no lerp overrides), NOT computedStyle (which
    // will be overwritten by the lerp below), and NOT raw style (which misses CSS).
    mPrevStyle_ = newMerged;
    // ── Tick @keyframes animations (lower cascade than transitions) ─────────────
    // Animations apply first so that CSS transitions on the same property
    // can overwrite them, matching the CSS Cascade Level 5 origin order:
    //   animations (origin 3) < transitions (origin 4).
    tickAnimations_(dtMs, computedStyle);
    bool anyActive = false;
    for (auto& entry : mActiveTransitions_)
    {
      entry.elapsedMs      += dtMs;
      const float rawT      = entry.durationMs > 0.f
                              ? (entry.elapsedMs / entry.durationMs) : 1.f;
      const float easedT    = glint_ease_eval(entry.easing, std::min(rawT, 1.f));
      glint_style_lerp_by_name(computedStyle, entry.key,
                                entry.fromVal, entry.toVal, easedT);
      if (rawT < 1.f) anyActive = true;
    }

    // ── Remove completed transitions ──────────────────────────────────────────
    mActiveTransitions_.erase(
      std::remove_if(mActiveTransitions_.begin(), mActiveTransitions_.end(),
                     [](const glint_transition_entry& e) { return e.elapsedMs >= e.durationMs; }),
      mActiveTransitions_.end());

    // ── Chain redraws while any animation is still in flight ─────────────────
    if (anyActive) setDirty(false);
  }
  // ── @keyframes animation tick helper ────────────────────────────────────────
  /** Called from tickTransitions() to apply CSS animations on top of the
   *  merged base style.  Transitions applied afterwards can overwrite.
   *  `dtMs` is clamped to 0 when brand-new transition entries were added
   *  this frame (same zeroing convention as the transition system). */
  void tickAnimations_(float dtMs, glint_style& target)
  {
    if (!mKeyframeRegistryPtr_) return;

    // Resolve animation property from merged cascade (inline > CSS).
    const std::string& mergedAnim = !style.animation.empty()
                                    ? style.animation : cssStyle_.animation;

    // Re-parse spec list when the merged animation string changes.
    if (mergedAnim != mLastAnimStr_)
    {
      mLastAnimStr_     = mergedAnim;
      mParsedAnimSpecs_ = glint_parse_animation(mergedAnim);

      // Reconcile active animations:
      //   • Keep already-running ones that still appear in the new spec.
      //   • Start brand-new entries (with their delay).
      //   • Drop entries that no longer appear.
      std::vector<glint_animation_entry> next;
      next.reserve(mParsedAnimSpecs_.size());
      for (const auto& spec : mParsedAnimSpecs_)
      {
        auto it = std::find_if(mActiveAnimations_.begin(), mActiveAnimations_.end(),
                               [&](const glint_animation_entry& e) {
                                   return e.spec.name == spec.name; });
        if (it != mActiveAnimations_.end())
        {
          // Update spec (duration/easing may have changed) but keep timing.
          it->spec      = spec;
          it->delayLeft = it->started ? 0.f : spec.delayMs;
          next.push_back(*it);
        }
        else
        {
          glint_animation_entry e;
          e.spec      = spec;
          e.delayLeft = spec.delayMs;
          next.push_back(std::move(e));
        }
      }
      mActiveAnimations_ = std::move(next);
    }

    if (mActiveAnimations_.empty()) return;

    bool anyActiveAnim = false;
    for (auto& entry : mActiveAnimations_)
    {
      if (entry.finished && !entry.spec.fillForwards) continue;

      // ── Delay ─────────────────────────────────────────────────────────────
      if (entry.delayLeft > 0.f)
      {
        entry.delayLeft -= dtMs;
        if (entry.delayLeft > 0.f) { anyActiveAnim = true; continue; }
        entry.delayLeft = 0.f;
      }
      entry.started = true;

      // ── Advance time ─────────────────────────────────────────────────────────
      if (!entry.finished)
        entry.elapsedMs += dtMs;

      const float durMs = entry.spec.durationMs > 0.f ? entry.spec.durationMs : 1.f;
      const bool  infinite = std::isinf(entry.spec.iterCount);
      const float totalMs  = infinite ? durMs : (durMs * entry.spec.iterCount);

      if (!infinite && entry.elapsedMs >= totalMs)
      {
        entry.elapsedMs = totalMs;
        entry.finished  = true;
      }

      // ── Compute normalised t ───────────────────────────────────────────────────
      // Iteration index & per-iteration local t.
      const int   iterIdx   = (durMs > 0.f) ? static_cast<int>(entry.elapsedMs / durMs) : 0;
      float       iterFrac  = std::fmod(entry.elapsedMs, durMs) / durMs;
      // Snap to end of last iteration rather than wrapping to 0.
      if (iterFrac == 0.f && entry.elapsedMs > 0.f) iterFrac = 1.f;

      // Direction: alternate flips odd iterations; reverse flips all.
      bool flip = entry.spec.reverse;
      if (entry.spec.alternate && (iterIdx % 2 == 1)) flip = !flip;
      float t = flip ? (1.f - iterFrac) : iterFrac;

      // fill-mode: forwards → hold at end when finished.
      if (entry.finished && entry.spec.fillForwards)
      {
        const bool lastIterOdd = (static_cast<int>(entry.spec.iterCount) % 2 == 1);
        t = (entry.spec.alternate && !lastIterOdd) ? (entry.spec.reverse ? 1.f : 0.f)
                                                   : (entry.spec.reverse ? 0.f : 1.f);
      }

      // ── Apply keyframe stops ──────────────────────────────────────────────────
      glint_keyframe_apply(*mKeyframeRegistryPtr_, entry.spec.name,
                           t, entry.spec.easing, target);

      if (!entry.finished) anyActiveAnim = true;
    }

    // Remove completed non-fill-forwards entries.
    mActiveAnimations_.erase(
      std::remove_if(mActiveAnimations_.begin(), mActiveAnimations_.end(),
        [](const glint_animation_entry& e) {
            return e.finished && !e.spec.fillForwards; }),
      mActiveAnimations_.end());

    if (anyActiveAnim) setDirty(false);
  }
  // ── CSS cascade layer ───────────────────────────────────────────────────────

  /** Author stylesheet cascade result — set by glint_document::_applyCssToElement().
   *  Never written by C++ UI code; that always goes to `style` (the inline layer). */
  glint_style cssStyle_;
  bool       mHasCssStyle_ = false;

  /** CSS property names (e.g. "background-color") whose winning cascade declaration
   *  carried !important.  Inline styles must not override these in _mergedStyle(). */
  std::unordered_set<std::string> mCssImportantProps_;

  /** Snapshot of cssStyle_ taken when no pseudo-class states (:hover/:active/etc.) are
  *  active.  Used to compute the CSS delta applied on top of inline styles during hover.
  *  Kept in sync during subtree finalization and by _drawImpl() when the element is not hovered. */
  glint_style mCssStyleBase;

  /**
   * Merge cssStyle_ (author stylesheets) and style (inline) following Chrome rules:
   * for each property, the INLINE value wins if it differs from the CSS-spec default.
   * This is called every frame from tickTransitions() in place of `computedStyle = style`.
   */
  glint_style _mergedStyle() const
  {
    static const glint_style sD{};  // default-constructed — all CSS-spec baseline values
    glint_style r = mHasCssStyle_ ? cssStyle_ : glint_style{};
    // Start from the stylesheet layer when present, otherwise from the CSS
    // initial values so inherited properties can still be resolved below.

    auto ceq = [](const glint_color& a, const glint_color& b) noexcept {
      return a.A == b.A && a.R == b.R && a.G == b.G && a.B == b.B;
    };

    // For each property: if the INLINE style differs from the spec default, it overrides.
#define _IW_F(f)   if (style.f != sD.f)                                    r.f = style.f
#define _IW_S(f)   if (style.f != sD.f)                                    r.f = style.f
#define _IW_C(f)   if (!ceq(style.f.value, sD.f.value))                    r.f = style.f
#define _IW_L(f)   if (style.f.raw != sD.f.raw && !(style.f.builderInjected && !cssStyle_.f.raw.empty()))  r.f = style.f.raw
#define _IW_OC(f)  if (style.f.isSet)                                       r.f = style.f
    // glint_optional_float: isSet flag tracks whether user code explicitly set this
    // property inline. Value-comparison is NOT used — matches Chrome's cascade where
    // inline style.*= always wins over class rules regardless of the assigned value.
#define _IW_OF(f)  if (style.f.isSet)                                       r.f = style.f
    // sk_side_proxy variant: preserves "%" raw strings so resolve() works correctly.
    // Without this, assigning via float cast would clear _rawp, turning "50%" into 50px.
#define _IW_SIDE(f) do { \
      const std::string& _sr = (style.f._rawp && !style.f._rawp->empty()) ? *style.f._rawp : _emptyStr; \
      const std::string& _dr = (sD.f._rawp    && !sD.f._rawp->empty())    ? *sD.f._rawp    : _emptyStr; \
      const float _sv = (float)style.f; \
      const float _dv = (float)sD.f; \
      if (_sr != _dr || _sv != _dv) { if (!_sr.empty()) r.f = _sr.c_str(); else r.f = _sv; } \
    } while(0)
    static const std::string _emptyStr;

    // Colors & opacity
    _IW_C(color);  _IW_C(backgroundColor);  _IW_OF(opacity);
    // Background gradient / img
    if (!style.backgroundGradient.empty())  r.backgroundGradient = style.backgroundGradient;
    _IW_S(backgroundGradientType);
    // When inline gradient stops are present the angle is meaningful even at 0°
    // (the default), so we must not skip it via _IW_F which ignores default values.
    if (!style.backgroundGradient.empty() || style.backgroundGradientAngle != sD.backgroundGradientAngle)
    {
      r.backgroundGradientAngle = style.backgroundGradientAngle;
      // If the inline style owns the gradient (stops present) but has no explicit
      // keyword direction, clear any keyword direction inherited from the CSS rule
      // so the numeric angle takes effect instead (e.g. "to right" would otherwise
      // override an inline 0° angle and keep the gradient horizontal).
      if (!style.backgroundGradient.empty() && style.backgroundGradientDirection.empty())
        r.backgroundGradientDirection.clear();
    }
    _IW_S(backgroundGradientDirection);
    _IW_F(backgroundGradientCX);     _IW_F(backgroundGradientCY);   _IW_F(backgroundGradientRadius);
    _IW_S(backgroundImage);  _IW_S(backgroundSize);
    _IW_S(backgroundPosition);  _IW_S(backgroundRepeat);
    // Border width
    if (style.borderWidth._val != sD.borderWidth._val) r.borderWidth = style.borderWidth._val;
    _IW_L(borderTopWidth);  _IW_L(borderRightWidth);
    _IW_L(borderBottomWidth);  _IW_L(borderLeftWidth);
    // Border color
    if (!ceq(style.borderColor.value, sD.borderColor.value)) r.borderColor = style.borderColor.value;
    _IW_OC(borderTopColor);   _IW_OC(borderRightColor);
    _IW_OC(borderBottomColor); _IW_OC(borderLeftColor);
    // Border style
    _IW_S(borderStyle);  _IW_S(borderTopStyle);  _IW_S(borderRightStyle);
    _IW_S(borderBottomStyle);  _IW_S(borderLeftStyle);
    // Border radius
    _IW_L(borderRadius);
    _IW_L(borderTopLeftRadius);  _IW_L(borderTopRightRadius);
    _IW_L(borderBottomRightRadius);  _IW_L(borderBottomLeftRadius);
    // SVG stroke
    _IW_OC(strokeColor);  _IW_S(strokeDasharray);  _IW_F(strokeDashoffset);
    _IW_S(strokeLinecap);  _IW_S(strokeLinejoin);
    _IW_F(strokeMiterlimit);  _IW_OF(strokeOpacity);  _IW_F(strokeWidth);
    // Shadow
    if (style.boxShadow.isSet) r.boxShadow = style.boxShadow;
    else
    {
      _IW_F(shadowEnabled);  _IW_C(shadowColor);
      _IW_F(shadowOffsetX);  _IW_F(shadowOffsetY);  _IW_F(shadowBlur);
    }
    // Typography
    _IW_L(fontSize);  _IW_F(lineHeight);
    _IW_S(fontFamily);  _IW_S(fontStyle);
    _IW_OF(fontWeight);
    if (style.textAlign  != sD.textAlign)  r.textAlign  = style.textAlign;
    _IW_S(verticalAlign);
    _IW_S(textDecoration);
    _IW_C(selectionColor);
    // Padding — use _IW_SIDE to preserve % raw strings (e.g. "50%") so that
    // resolve() can multiply against the parent dimension at layout time.
    _IW_SIDE(paddingTop);  _IW_SIDE(paddingRight);
    _IW_SIDE(paddingBottom);  _IW_SIDE(paddingLeft);
    // Margin
    _IW_SIDE(marginTop);  _IW_SIDE(marginRight);
    _IW_SIDE(marginBottom);  _IW_SIDE(marginLeft);
    // Position / sizing
    _IW_S(position);
    // For builder-injected top/left (auto-flow cursor): suppress them not only when CSS
    // sets top/left explicitly, but also when CSS places the element in a positioned
    // context (absolute/relative/fixed/sticky). In that case the auto-cursor is
    // irrelevant and must not fight CSS-driven bottom/right placement — Chrome parity.
    {
      const bool _cssPositioned = !cssStyle_.position.empty() && cssStyle_.position != "static";
      if (style.top.raw  != sD.top.raw  && !(style.top.builderInjected  && (!cssStyle_.top.raw.empty()  || _cssPositioned)))  r.top  = style.top;
      if (style.left.raw != sD.left.raw && !(style.left.builderInjected && (!cssStyle_.left.raw.empty() || _cssPositioned)))  r.left = style.left;
    }
    _IW_L(right);  _IW_L(bottom);
    _IW_L(width);  _IW_L(height);
    _IW_L(minWidth);  _IW_L(maxWidth);  _IW_L(minHeight);  _IW_L(maxHeight);
    _IW_F(zIndex);
    // Flex / layout
    _IW_S(display);  _IW_S(flexDirection);  _IW_S(justifyContent);
    _IW_S(alignItems);  _IW_L(gap);  _IW_F(flexGrow);
    _IW_S(pointerEvents);  _IW_S(cursor);  _IW_S(userSelect);  _IW_S(whiteSpace);
    // Overflow / scrollbar
    _IW_S(overflowX);  _IW_S(overflowY);
    _IW_F(scrollbarWidth);
    _IW_C(scrollbarThumbColor);  _IW_C(scrollbarTrackColor);  _IW_C(scrollbarButtonColor);
    // Visual / effects
    _IW_S(objectFit);  _IW_S(objectPosition);
    _IW_S(transform);  _IW_S(filter);  _IW_S(backdropFilter);
    _IW_S(mixBlendMode);  _IW_S(backgroundBlendMode);  _IW_S(isolation);
    // Mask
    _IW_S(mask);  _IW_S(maskMode);  _IW_S(maskPosition);  _IW_S(maskSize);
    _IW_S(maskRepeat);  _IW_S(maskOrigin);  _IW_S(maskClip);  _IW_S(maskComposite);
    // Transition / animation
    _IW_S(transition);
    _IW_S(animation);

    // Restore !important CSS properties — inline styles (the _IW_* pass above) must
    // not win over !important stylesheet declarations per the CSS cascade spec.
    if (!mCssImportantProps_.empty() && mHasCssStyle_)
    {
      for (const auto& prop : mCssImportantProps_)
      {
        const std::string cssVal = glint_style_get_by_name(cssStyle_, prop);
        glint_style_lerp_by_name(r, prop, cssVal, cssVal, 1.0f);
      }
    }

    if (mInspectorRemoved)
      r.display = "none";

#undef _IW_F
#undef _IW_S
#undef _IW_C
#undef _IW_L
#undef _IW_OC
#undef _IW_SIDE
#undef _IW_OF

    // CSS inherited properties: when neither the stylesheet layer nor the
    // inline layer set a text-related value on this node, inherit the parent's
    // computed value instead of falling back to glint_style's built-in defaults.
    // This is required for rules such as `body { color: red; }` to affect
    // descendant text nodes.
    if (mParent)
    {
      const auto& p = mParent->computedStyle;

      const bool hasInlineColor = !ceq(style.color.value, sD.color.value);
      const bool hasCssColor    = mHasCssStyle_ && !ceq(cssStyle_.color.value, sD.color.value);
      if (!hasInlineColor && !hasCssColor)
        r.color = p.color;


      const bool hasInlineFontSize = (style.fontSize.raw != sD.fontSize.raw);
      const bool hasCssFontSize    = mHasCssStyle_ && (cssStyle_.fontSize.raw != sD.fontSize.raw);
      if (!hasInlineFontSize && !hasCssFontSize)
        r.fontSize = p.fontSize;

      const bool hasInlineLineHeight = (style.lineHeight != sD.lineHeight);
      const bool hasCssLineHeight    = mHasCssStyle_ && (cssStyle_.lineHeight != sD.lineHeight);
      if (!hasInlineLineHeight && !hasCssLineHeight)
        r.lineHeight = p.lineHeight;

      const bool hasInlineFontWeight = style.fontWeight.isSet;
      const bool hasCssFontWeight    = mHasCssStyle_ && cssStyle_.fontWeight.isSet;
      if (!hasInlineFontWeight && !hasCssFontWeight)
        r.fontWeight = p.fontWeight;

      const bool hasInlineFontFamily = !style.fontFamily.empty();
      const bool hasCssFontFamily    = mHasCssStyle_ && !cssStyle_.fontFamily.empty();
      if (!hasInlineFontFamily && !hasCssFontFamily)
        r.fontFamily = p.fontFamily;

      const bool hasInlineFontStyle = !style.fontStyle.empty();
      const bool hasCssFontStyle    = mHasCssStyle_ && !cssStyle_.fontStyle.empty();
      if (!hasInlineFontStyle && !hasCssFontStyle)
        r.fontStyle = p.fontStyle;

      const bool hasInlineTextAlign = (style.textAlign != sD.textAlign);
      const bool hasCssTextAlign    = mHasCssStyle_ && (cssStyle_.textAlign != sD.textAlign);
      if (!hasInlineTextAlign && !hasCssTextAlign)
        r.textAlign = p.textAlign;

      const bool hasInlineWhiteSpace = (style.whiteSpace != sD.whiteSpace);
      const bool hasCssWhiteSpace    = mHasCssStyle_ && (cssStyle_.whiteSpace != sD.whiteSpace);
      if (!hasInlineWhiteSpace && !hasCssWhiteSpace)
        r.whiteSpace = p.whiteSpace;

      const bool hasInlineUserSelect = (style.userSelect != sD.userSelect);
      const bool hasCssUserSelect    = mHasCssStyle_ && (cssStyle_.userSelect != sD.userSelect);
      if (!hasInlineUserSelect && !hasCssUserSelect)
        r.userSelect = p.userSelect;
    }
    else
    {
      // Root element (no parent): if no layer set color, fall back to the CSS
      // initial value for `color` (opaque white).  This restores the visual
      // default that previously came from glint_style's hard-coded white, now
      // that the default was changed to transparent to make the cascade correct.
      const bool hasInlineColor = !ceq(style.color.value, sD.color.value);
      const bool hasCssColor    = mHasCssStyle_ && !ceq(cssStyle_.color.value, sD.color.value);
      if (!hasInlineColor && !hasCssColor)
        r.color = glint_color(255, 255, 255, 255);
    }

    return r;
  }
