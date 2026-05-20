#pragma once

/**
 * glint_select.hpp
 * A <select>-equivalent dropdown component backed by a native OS context menu.
 *
 * Clicking the select opens a native popup menu (Win32 TrackPopupMenu via the
 * glint_popup_menu routing already wired in glint_document / glint_window_win32).
 * The menu is anchored at the bottom-left of the element.
 *
 * Hover and pressed state transitions are animated identically to glint_button —
 * set style.transition, hover.*, and pressed.* to get the same behaviour.
 *
 * Usage via the builder (preferred):
 *   add.select([](glint_select& _c) {
 *     _c.options       = { "Option A", "Option B", "Option C" };
 *     _c.selectedIndex = 0;
 *     _c.placeholder   = "Choose…";
 *     _c.onChange      = [](int idx, const std::string& val) {
 *       DBGMSG("selected %d: %s\n", idx, val.c_str());
 *     };
 *     _c.style.width          = 160.f;
 *     _c.style.height         = 32.f;
 *     _c.style.backgroundColor = "#2a2a2a";
 *     _c.style.color           = "#e0e0e0";
 *     _c.style.borderRadius    = 4.f;
 *     _c.style.borderWidth     = 1.f;
 *     _c.style.borderColor     = "#555555";
 *     _c.style.padding         = 8.f;
 *     _c.style.transition      = "background-color 120ms ease-out";
 *     _c.hover.backgroundColor = "#383838";
 *   });
 *
 * Direct access:
 *   glint_select* sel = nullptr;
 *   add.select([](glint_select& _c){ ... }, &sel);
 *   // later:
 *   sel->selectedIndex = 2;
 *   sel->setDirty(false);
 */

#include "../glint_element.hpp"
#include "../glint_animator.hpp"
#include "../platform/glint_platform.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

class glint_select : public glint_element
{
public:
  // ── Public fields — set in the builder callback ───────────────────────────
  std::vector<std::string>                    options;
  int                                         selectedIndex = -1;   // -1 = nothing selected
  std::string                                 placeholder   = "Select…";
  std::function<void(int, const std::string&)> onChange;

  glint_style hover;    // mouse-over state  (base `style` = normal state)
  glint_style pressed;  // mouse-down state

  glint_select()
  {
    mStateComputedStyle = computedStyle;
    mLastStateTick = std::chrono::steady_clock::now();
  }

  const char* typeName() const override { return "select"; }

  // ── Accessors ──────────────────────────────────────────────────────────────

  /** Text of the currently selected option, or the placeholder if nothing is selected. */
  const std::string& value() const
  {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(options.size()))
      return options[selectedIndex];
    return placeholder;
  }

  /** Programmatically pick an option by index and fire onChange. */
  void setValue(int idx)
  {
    if (idx < 0 || idx >= static_cast<int>(options.size())) return;
    selectedIndex = idx;
    setDirty(false);
    if (onChange) onChange(selectedIndex, options[selectedIndex]);
  }

  // ── Mouse events ───────────────────────────────────────────────────────────

  void OnMouseOver(float, float, const glint_mouse_mod&) override
  {
    if (!mIsHovered)
    {
      mIsHovered = true;
      _startStateTransition();
      setDirty(false);
    }
  }

  void OnMouseOut() override
  {
    mIsHovered = mIsPressed = false;
    _startStateTransition();
    setDirty(false);
  }

  void OnMouseDown(float, float, const glint_mouse_mod&) override
  {
    mIsPressed = true;
    _startStateTransition();
    setDirty(false);
  }

  void OnMouseUp(float x, float y, const glint_mouse_mod&) override
  {
    const bool wasPressed = mIsPressed;
    mIsPressed = false;
    _startStateTransition();
    setDirty(false);
    if (wasPressed && mRect.Contains(x, y))
      _openMenu();
  }

  // ── Popup menu result ──────────────────────────────────────────────────────

  // ── Draw ──────────────────────────────────────────────────────────────────

  void Draw(glint_canvas& g) override
  {
    computedStyle = _mergedStyle();
    if (!mIsHovered && !mIsPressed && mStateTransitions.empty())
      mStateComputedStyle = computedStyle;
    if (_tickStateTransitions()) setDirty(false);

    glint_rect expanded = mRect;
    if (mFilterPad > 0.f) mRect = mPaintRECT;

    const bool hasFilter = !style.filter.empty() && style.filter != "none";
    if (hasFilter) glint_filter::BeginLayer(g, mRect, style.filter);

    DrawBackground(g, mStateComputedStyle);
    mActiveStyle = &mStateComputedStyle;
    drawContent(g);

    if (hasFilter) glint_filter::EndLayer(g);

    if (mFilterPad > 0.f) mRect = expanded;
  }

protected:
  bool               mIsHovered           = false;
  bool               mIsPressed           = false;
  const glint_style*  mActiveStyle         = nullptr;

  // ── State-transition animation (mirrors glint_button exactly) ──────────────
  glint_style  mStateComputedStyle;
  std::vector<glint_transition_entry> mStateTransitions;
  std::chrono::steady_clock::time_point mLastStateTick;
  bool mStateTransJustStarted = false;

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
    if (!mIsPressed && !mIsHovered) return computedStyle;
    const glint_style& over = mIsPressed ? pressed : hover;
    const glint_style  def;           // zero-default reference
    glint_style merged = computedStyle;
    for (const auto& key : glint_animatable_keys())
    {
      const std::string overVal = glint_style_get_by_name(over, key);
      const std::string defVal  = glint_style_get_by_name(def,  key);
      if (overVal != defVal)
        glint_style_lerp_by_name(merged, key, defVal, overVal, 1.f);
    }
    return merged;
  }

  void _startStateTransition()
  {
    const auto rawSpecs = glint_parse_transition(style.transition);
    if (rawSpecs.empty()) { mStateTransitions.clear(); return; }

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

  bool _tickStateTransitions()
  {
    auto now = std::chrono::steady_clock::now();
    const float dtMs = mStateTransJustStarted ? 0.f
        : std::chrono::duration<float, std::milli>(now - mLastStateTick).count();
    mLastStateTick         = now;
    mStateTransJustStarted = false;

    if (mStateTransitions.empty())
    {
      mStateComputedStyle = _buildMergedTarget();
      return false;
    }

    bool anyRunning = false;
    for (auto& e : mStateTransitions)
    {
      e.elapsedMs += dtMs;
      const float rawT = (e.durationMs > 0.f)
          ? std::min(1.f, e.elapsedMs / e.durationMs) : 1.f;
      const float t = glint_ease_eval(e.easing, rawT);
      glint_style_lerp_by_name(mStateComputedStyle, e.key, e.fromVal, e.toVal, t);
      if (rawT < 1.f) anyRunning = true;
    }

    if (!anyRunning) mStateTransitions.clear();
    return anyRunning;
  }

  // ── Content: label + chevron ───────────────────────────────────────────────

  void drawContent(glint_canvas& g) override
  {
    const glint_style& s = mActiveStyle ? *mActiveStyle : style;
    const glint_rect content = getContent();

    // Reserve a fixed-width column on the right for the drawn chevron.
    const float fSize      = s.fontSize.toFloat() > 0.f ? s.fontSize.toFloat() : 12.f;
    const float chevronW   = fSize + 4.f;
    const glint_rect labelRect(content.L, content.T, content.R - chevronW, content.B);
    const glint_rect chevRect(content.R - chevronW, content.T, content.R, content.B);

    const std::string _fid_ = glint_font_registry::resolveFontFaceId(
        s.fontFamily.c_str(), (int)s.fontWeight, s.fontStyle.c_str());
    const char* _fn_ = _fid_.empty()
        ? (s.fontFamily.empty() ? nullptr : s.fontFamily.c_str())
        : _fid_.c_str();
    glint_text labelText(fSize, s.color, _fn_,
            EAlign::Near, EVAlign::Middle);
    g.DrawText(labelText, value().c_str(), labelRect);

    // Draw a downward-pointing triangle centred in chevRect.
    const float triW  = std::round(fSize * 0.45f);
    const float triH  = std::round(triW * 0.6f);
    const float cx    = chevRect.MW();
    const float cy    = chevRect.MH();
    const glint_color col  = s.color.value;
    const float xs[] = { cx - triW * 0.5f, cx + triW * 0.5f, cx };
    const float ys[] = { cy - triH * 0.5f, cy - triH * 0.5f, cy + triH * 0.5f };
    g.FillConvexPolygon(col, xs, ys, 3);
  }

  // ── Open the native popup menu anchored at the bottom-left of the element ──

  void _openMenu()
  {
    if (options.empty()) return;

    using P = std::pair<int, std::string>;
    std::vector<P> items;
    items.reserve(options.size());
    for (int i = 0; i < static_cast<int>(options.size()); ++i)
      items.push_back({i + 1, options[i]});
    const std::vector<int> disabled;
    const int selectedId = (selectedIndex >= 0 && selectedIndex < static_cast<int>(options.size()))
      ? selectedIndex + 1
      : 0;
    const int result = glint_platform::showSelectMenu(0, 0, items, selectedId, disabled);
    if (result >= 1 && result - 1 < static_cast<int>(options.size()))
    {
      selectedIndex = result - 1;
      setDirty(false);
      if (onChange) onChange(selectedIndex, options[selectedIndex]);
    }
  }

  void DrawToCanvas(SkCanvas* canvas) override
  {
    if (_tickStateTransitions()) setDirty(false);

    const glint_style& active = mStateComputedStyle;
    const glint_color bg  = ApplyOpacity(active.backgroundColor.value, active.opacity);
    const glint_color brd = ApplyOpacity(active.borderColor.value,     active.opacity);
    const float  br  = active.borderRadius.resolve(std::min(mRect.W(), mRect.H()));

    if (bg.A > 0)
    {
      SkPaint p; p.setColor(skColor(bg)); p.setAntiAlias(true);
      if (br > 0.f) canvas->drawRoundRect(skRect(mRect), br, br, p);
      else          canvas->drawRect(skRect(mRect), p);
    }
    if (active.borderWidth > 0.f && brd.A > 0)
    {
      SkPaint p; p.setColor(skColor(brd)); p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(active.borderWidth);
      if (br > 0.f) canvas->drawRoundRect(skRect(mRect), br, br, p);
      else          canvas->drawRect(skRect(mRect), p);
    }

    const float fSize = active.fontSize.toFloat() > 0.f ? active.fontSize.toFloat() : 12.f;
    SkFont       font  = skFont(fSize);
    SkPaint      tp;
    tp.setColor(skColor(active.color.value));
    tp.setAntiAlias(true);

    const glint_rect r        = getContent();
    const float chevronW = fSize + 4.f;
    const glint_rect labelR(r.L, r.T, r.R - chevronW, r.B);
    const glint_rect chevR(r.R - chevronW, r.T, r.R, r.B);

    // Helper: component-specific centered label baseline.
    auto baselineY = [&](const glint_rect& rect) -> float {
      return rect.T + rect.H() * 0.5f + fSize * 0.35f;
    };

    // Label (left-aligned)
    const std::string& lbl = value();
    if (!lbl.empty())
      canvas->drawString(lbl.c_str(), labelR.L, baselineY(labelR), font, tp);

    // Chevron — downward-pointing filled triangle centred in chevR
    {
      const float triW = fSize * 0.45f;
      const float triH = triW * 0.6f;
      const float cx   = chevR.MW();
      const float cy   = chevR.MH();
      SkPath path;
      path.moveTo(cx - triW * 0.5f, cy - triH * 0.5f);
      path.lineTo(cx + triW * 0.5f, cy - triH * 0.5f);
      path.lineTo(cx,               cy + triH * 0.5f);
      path.close();
      canvas->drawPath(path, tp);
    }

    for (auto& child : mChildren) child->DrawToCanvas(canvas);
  }
};

namespace { struct _glint_select_reg { _glint_select_reg() { glint_element::registerElement("select", []{ return new glint_select(); }); } } _glint_select_reg_; }
