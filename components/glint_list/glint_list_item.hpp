#pragma once

/**
 * glint_list_item.hpp
 * A single row in a glint_list.  Extends glint_button with:
 *   - A four-tier style priority: selected > pressed > hover > normal
 *   - An optional leading SVG icon
 *   - Stable string id and std::any userData for lookup / callbacks
 *   - A back-pointer to its owning glint_list (set by glint_list::items.add)
 *
 * In typical usage the item is configured inside the items.add() callback:
 *
 *   list->items.add([](glint_list_item& _c) {
 *     _c.innerText = "My Item";
 *     _c.id       = "item_01";
 *     _c.userData = std::make_any<int>(42);
 *     _c.icon     = mySvg;
 *     _c.selectedStyle.backgroundColor = "#5a9fff";
 *   });
 *
 * Mouse event routing (click / mouseenter / mouseleave) is wired via DOM
 * event listeners in glint_list::items.add() — no virtual override needed.
 * mList is kept as a public convenience back-pointer for ad-hoc access from
 * onClick callbacks or external code.
 */

#include "../glint_button.hpp"

#include <any>
#include <functional>
#include <string>

// Forward-declared — full definition is in glint_list.hpp.
class glint_list;

// ─── glint_list_item ─────────────────────────────────────────────────────────

class glint_list_item : public glint_button
{
public:
  // ── Identity / data ────────────────────────────────────────────────────────
  std::string  id;        // caller-assigned stable identifier
  int          idx = -1;  // 0-based index in glint_list::mList (stamped by items.add)
  std::any     userData;  // arbitrary per-item data

  // ── Selection ──────────────────────────────────────────────────────────────
  bool        selected = false;
  glint_style selectedStyle; // highest priority: overrides pressed/hover/normal

  // ── Icon (optional leading SVG) ────────────────────────────────────────────
  glint_svg         icon { nullptr };
  float        iconSize  = 16.f;
  sk_color     iconColor { glint_color(255, 200, 200, 200) };

  // ── Back-pointer (set by glint_list::items.add) ────────────────────────────
  glint_list*  mList = nullptr;

  // ── Constructor ────────────────────────────────────────────────────────────
  glint_list_item()
  {
    // Sensible defaults — override in the items.add() callback as needed.
    style.width       = "100%";
    style.height      = 32.f;
    style.paddingLeft = 8.f;
    style.fontSize    = 13.f;
    style.color       = glint_color(255, 200, 200, 200);

    hover.backgroundColor   = glint_color(60,  255, 255, 255); // subtle white tint
    pressed.backgroundColor = glint_color(90,  255, 255, 255);

    selectedStyle.backgroundColor = glint_color(180, 60, 100, 200); // blue-ish
    selectedStyle.color           = glint_color(255, 255, 255, 255);
  }

  const char* typeName() const override { return "list-item"; }
  const char* tagName()  const override { return "li"; }

  // ── Selection helper ───────────────────────────────────────────────────────
  void setSelected(bool v)
  {
    if (selected == v) return;
    selected = v;
    setDirty(false);
  }

  // ── Draw — four-tier active style ──────────────────────────────────────────
  // Priority: selected > pressed > hover > normal
  void Draw(glint_canvas& g) override
  {
    const glint_style& active =
      selected   ? selectedStyle :
      mIsPressed ? pressed :
      mIsHovered ? hover : style;

    glint_rect _expandedRECT = mRect;
    if (mFilterPad > 0.f) mRect = mPaintRECT;

    const bool _hasFilter = !style.filter.empty() && style.filter != "none";
    if (_hasFilter) glint_filter::BeginLayer(g, mRect, style.filter);

    DrawBackground(g, active);
    mActiveStyle = &active;
    drawContent(g);

    if (_hasFilter) glint_filter::EndLayer(g);

    if (mFilterPad > 0.f) mRect = _expandedRECT;
  }

protected:
  // ── drawContent — optional icon then left-aligned text ─────────────────────
  void drawContent(glint_canvas& g) override
  {
    const glint_style& s = mActiveStyle ? *mActiveStyle : style;
    glint_rect content = getContent();

    if (icon.IsValid())
    {
      const float ic      = iconSize;
      const float iconTop = content.T + (content.H() - ic) * 0.5f;
      const glint_rect iconRect(content.L, iconTop, content.L + ic, iconTop + ic);
      const glint_color* pFill = &iconColor.value;
      g.DrawSVG(icon, iconRect, nullptr, nullptr, pFill);
      content.L += ic + 4.f;  // 4 px gap between icon and text
    }

    if (!innerText.empty())
    {
      const std::string _fid_ = glint_font_registry::resolveFontFaceId(
          s.fontFamily.c_str(), (int)s.fontWeight, s.fontStyle.c_str());
      const char* _fn_ = _fid_.empty()
          ? (s.fontFamily.empty() ? nullptr : s.fontFamily.c_str())
          : _fid_.c_str();
      glint_text t(style.fontSize.toFloat(), s.color, _fn_,
              EAlign::Near, EVAlign::Middle);
      g.DrawText(t, innerText.c_str(), content);
    }
  }

  // ── DrawToCanvas — inspector path ──────────────────────────────────────────
  void DrawToCanvas(SkCanvas* canvas) override
  {
    const glint_style& active =
      selected   ? selectedStyle :
      mIsPressed ? pressed :
      mIsHovered ? hover : style;

    // Background (manual — DrawBackgroundToCanvas uses `style`, not `active`)
    const glint_color bg  = ApplyOpacity(active.backgroundColor.value, active.opacity);
    const glint_color brd = ApplyOpacity(active.borderColor.value,      active.opacity);
    const float  br  = active.borderRadius.resolve(std::min(mRect.W(), mRect.H()));

    if (bg.A > 0)
    {
      SkPaint p; p.setColor(skColor(bg)); p.setAntiAlias(true);
      if (br > 0.f)
        canvas->drawRoundRect(skRect(mRect), br, br, p);
      else
        canvas->drawRect(skRect(mRect), p);
    }

    if (active.borderWidth > 0.f && brd.A > 0)
    {
      SkPaint p; p.setColor(skColor(brd)); p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(active.borderWidth);
      if (br > 0.f)
        canvas->drawRoundRect(skRect(mRect), br, br, p);
      else
        canvas->drawRect(skRect(mRect), p);
    }

    // Icon placeholder (small colored square in the inspector)
    const glint_rect content = getContent();
    float textX = content.L;

    if (icon.IsValid())
    {
      const float ic      = iconSize;
      const float iconTop = content.T + (content.H() - ic) * 0.5f;
      SkPaint ip; ip.setColor(skColor(iconColor.value)); ip.setAntiAlias(true);
      canvas->drawRect(
        skRect(glint_rect(content.L, iconTop, content.L + ic, iconTop + ic)), ip);
      textX += ic + 4.f;
    }

    // Text
    if (!innerText.empty())
    {
      const float fontSize = style.fontSize.toFloat() > 0.f ? style.fontSize.toFloat() : 13.f;
      SkFont  font = skFont(fontSize);
      SkPaint tp;  tp.setColor(skColor(active.color.value)); tp.setAntiAlias(true);
      const float textY = content.T + content.H() * 0.5f + fontSize * 0.35f;
      canvas->drawString(innerText.c_str(), textX, textY, font, tp);
    }

    for (auto& child : mChildren) child->DrawToCanvas(canvas);
  }
};

// New API name — both refer to the same class.
namespace { struct _glint_list_item_reg { _glint_list_item_reg() { glint_element::registerElement("list-item", []{ return new glint_list_item(); }); } } _glint_list_item_reg_; }
