#pragma once

/**
 * glint_checkbox.hpp
 * A labelled checkbox component. Ported from glint_checkbox.js.
 *
 * Layout: flex-row, vertically centered.
 *   [BoxComp (bordered square + checkmark draw)] [glint_element (optional)]
 *
 * The box border brightens on hover (mouseenter/leave on the container).
 * Clicking anywhere on the component toggles `checked` and fires `onChange`.
 * Set `keepChecked = true` to prevent unchecking (radio-button behaviour).
 *
 * Usage via the builder (preferred):
 *   add.checkbox([](glint_checkbox& _c) {
 *     _c.text      = "Enable feature";
 *     _c.checked   = false;
 *     _c.size      = 16.f;   // drives box side, label fontSize, and gap
 *     _c.onChange  = [](bool v) { DBGMSG("checked: %d\n", v); };
 *   });
 *
 * Or with an out-pointer:
 *   glint_checkbox* cb = nullptr;
 *   add.checkbox([](glint_checkbox& _c){ _c.text = "Dry"; }, &cb);
 *   // cb points at the live checkbox instance immediately
 */

#include "../glint_element.hpp"

#include <functional>
#include <string>

class glint_checkbox : public glint_element
{
public:
  // -- Public fields � set in the builder callback ---------------------------
  bool        checked      = false;
  std::string text;                                               // optional label text
  float       size         = 16.f;                               // box side, label fontSize, and gap all scale with this
  sk_color    boxBg        = glint_color(255,  32,  32,  32);         // unchecked bg
  sk_color    checkedBg    = glint_color(255,  51, 153, 255);         // checked bg
  sk_color    borderCol    = glint_color(255, 110, 110, 110);         // resting border
  sk_color    checkmarkCol = glint_color(255, 255, 255, 255);         // ? stroke colour
  sk_color    textCol      = glint_color(255, 210, 210, 210);
  bool        keepChecked  = false;                              // prevent unchecking
  std::function<void(bool)> onChange;
  int tag = glint_no_tag;

    glint_checkbox()
    {
      align = "left middle";
      style.width  = "fit-content";
      style.height = "fit-content";

      auto* box = new BoxComp(this);
      mBox = box;
      addChild(box);

      auto* lbl = new glint_element();
      mLabel = lbl;
      addChild(lbl);

      element.addEventListener("click", [this](glint_event&)
      {
        if (checked && keepChecked) return;
        checked = !checked;
        _applyChecked();
        setDirty(false);
        if (onChange) onChange(checked);
      });

      element.addEventListener("mouseenter", [this](glint_event&)
      {
        if (mBox) { mBox->style.borderColor = glint_color(255, 210, 210, 210); setDirty(false); }
      });

      element.addEventListener("mouseleave", [this](glint_event&)
      {
        if (mBox) { mBox->style.borderColor = borderCol; setDirty(false); }
      });

      // NOTE: do NOT call _syncFromProps() here. Public fields like `size`
      // and `text` are typically assigned by the caller AFTER construction
      // (e.g. `auto* c = new glint_checkbox(); c->size = 11; c->text = ...`).
      // The first frame's tickTransitionsAll() override below will sync from
      // the final field values BEFORE the per-frame computedStyle snapshot,
      // so child sizes never flash a default-then-correct value.
    }

  // -- Accessors -------------------------------------------------------------
  bool  GetChecked()    const { return checked; }
  void  SetChecked(bool v)   { checked = v; _applyChecked(); setDirty(false); }

  const char* typeName() const override { return "checkbox"; }

  // Sync mLabel/mBox `style` from public fields (text, size, colors) BEFORE
  // the per-frame computedStyle snapshot pass walks our descendants. If we
  // synced later (e.g. inside Layout()), the parent's flex layout would
  // already have queried preferredH/W using stale computedStyle.display="none"
  // on mLabel — sizing the checkbox row with no room for the label and
  // hiding it until the next dirty event re-merges. Hooking tickTransitionsAll
  // makes the synced values visible to *this* frame's intrinsic-size queries.
  void tickTransitionsAll() override
  {
    _syncFromProps();
    glint_element::tickTransitionsAll();
  }

  void Layout(glint_canvas* g) override
  {
    // Newly-added checkboxes can miss this frame's tickTransitionsAll() when
    // they are inserted during event handling after the root pre-pass has
    // already run. Sync again here so the first layout/draw uses the final
    // public field values instead of waiting for the next mouse-driven frame.
    _syncFromProps();
    glint_element::Layout(g);
  }

  // syncBeforeLayout(): called by the builder immediately after user setup so
  // that preferredH/W return correct values at build time — no explicit row
  // height is needed in the caller.  Safe to call multiple times; _syncFromProps
  // is idempotent once mBox/mLabel exist.
  void syncBeforeLayout() override
  {
    _syncFromProps();
  }

  // preferredH / preferredW: intrinsic size of the checkbox row.
  // Height = box side (all children share the same cross-axis height).
  // Width  = box + gap + label text width (0 when text is empty).
  float preferredH(float /*availW*/ = 0.f) const override { return size; }

  float preferredW() const override
  {
    if (text.empty()) return size;
    const float gap = std::roundf(size * 0.5f);
    const float sz  = size > 0.f ? size : 12.f;
    SkFont font = skFont(sz,
                        computedStyle.fontFamily.c_str(),
                        computedStyle.fontWeight,
                        computedStyle.fontStyle.c_str());
    SkRect bounds;
    const float tw = font.measureText(text.c_str(), text.size(),
                                      SkTextEncoding::kUTF8, &bounds);
    return size + gap + tw;
  }

private:
  // -- BoxComp -- bordered square that draws a geometric checkmark ------------
  class BoxComp : public glint_element
  {
  public:
    glint_checkbox* owner;
    explicit BoxComp(glint_checkbox* o) : owner(o) {}
    const char* typeName() const override { return "checkbox-box"; }

    void drawContent(glint_canvas& g) override
    {
      if (!owner->checked) return;
      const glint_rect r = getContent();
      const float w = r.W(), h = r.H();

      // ? geometry: left leg, then right leg
      const float x0 = r.L + w * 0.15f, y0 = r.T + h * 0.52f;   // start (left)
      const float x1 = r.L + w * 0.40f, y1 = r.B - h * 0.18f;   // corner (bottom)
      const float x2 = r.R - w * 0.12f, y2 = r.T + h * 0.20f;   // end   (top-right)

      const sk_color& c = owner->checkmarkCol;
      g.DrawLine(c, x0, y0, x1, y1, nullptr, 1.5f);
      g.DrawLine(c, x1, y1, x2, y2, nullptr, 1.5f);
    }

    void DrawContentToCanvas(SkCanvas* canvas) override
    {
      if (!owner->checked) return;
      const glint_rect r = getContent();
      const float w = r.W(), h = r.H();

      const float x0 = r.L + w * 0.15f, y0 = r.T + h * 0.52f;
      const float x1 = r.L + w * 0.40f, y1 = r.B - h * 0.18f;
      const float x2 = r.R - w * 0.12f, y2 = r.T + h * 0.20f;

      SkPaint paint;
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(1.8f);
      paint.setStrokeCap(SkPaint::kRound_Cap);
      paint.setStrokeJoin(SkPaint::kRound_Join);
      paint.setAntiAlias(true);
      const glint_color& c = owner->checkmarkCol;
      paint.setColor(SkColorSetARGB(c.A, c.R, c.G, c.B));

      SkPath path;
      path.moveTo(x0, y0);
      path.lineTo(x1, y1);
      path.lineTo(x2, y2);
      canvas->drawPath(path, paint);
    }
  };

  BoxComp*     mBox   = nullptr;
  glint_element* mLabel = nullptr;

  void _syncFromProps()
  {
    style.gap = std::roundf(size * 0.5f);
    if (mBox)
    {
      mBox->style.width           = size;
      mBox->style.height          = size;
      mBox->style.borderRadius    = std::roundf(size * 0.2f);
      mBox->style.borderWidth     = 1.f;
      mBox->style.padding         = std::roundf(size * 0.15f);
      mBox->style.borderColor     = borderCol;
      mBox->style.backgroundColor = checked ? checkedBg : boxBg;
    }
    if (mLabel)
    {
      mLabel->innerText      = text;
      mLabel->style.fontSize = size;
      mLabel->style.color    = textCol;
      mLabel->style.display  = text.empty() ? "none" : "block";
    }
  }

  void _applyChecked()
  {
    _syncFromProps();
    if (!mBox) return;
    mBox->style.backgroundColor = checked ? checkedBg : boxBg;
  }
};

// New API name � both refer to the same class.
namespace { struct _glint_checkbox_reg { _glint_checkbox_reg() { glint_element::registerElement("checkbox", []{ return new glint_checkbox(); }); } } _glint_checkbox_reg_; }
