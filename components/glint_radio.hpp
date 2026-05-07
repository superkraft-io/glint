#pragma once

/**
 * glint_radio.hpp
 * Radio button component for glint.
 *
 * Layout: flex-row, vertically centered.
 *   [RadioBox (circle + dot)] [glint_element (optional label)]
 *
 * Group management: create a shared glint_radio_group and assign it to each
 * radio in the group.  When one radio is selected, the group deselects all others.
 *
 * Usage:
 *   auto group = std::make_shared<glint_radio_group>();
 *
 *   auto* r1 = row->add.fromClass<glint_radio>([&](glint_radio& r) {
 *     r.text    = "Option A";
 *     r.value   = "a";
 *     r.checked = true;
 *     r.size    = 16.f;
 *     r.group   = group;
 *   });
 *   auto* r2 = row->add.fromClass<glint_radio>([&](glint_radio& r) {
 *     r.text  = "Option B";
 *     r.value = "b";
 *     r.size  = 16.f;
 *     r.group = group;
 *   });
 *   group->onChange = [](const std::string& v) { ... };
 */

#include "../glint_element.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// ── glint_radio_group ─────────────────────────────────────────────────────────
// Shared state object that coordinates a set of radio buttons.
struct glint_radio_group
{
  std::string selected;   // value of the currently-selected radio
  std::function<void(const std::string&)> onChange;

  std::vector<class glint_radio*> members;

  void register_member(glint_radio* r)   { members.push_back(r); }
  void unregister_member(glint_radio* r)
  {
    members.erase(std::remove(members.begin(), members.end(), r), members.end());
  }

  // Select a specific value and deselect all others.
  void select(const std::string& value);   // defined after glint_radio
};

// ── glint_radio ───────────────────────────────────────────────────────────────

class glint_radio : public glint_element
{
public:
  // ── Public fields ──────────────────────────────────────────────────────────
  bool        checked   = false;
  std::string text;
  std::string value;   // the logical value this radio represents
  float       size      = 16.f;

  sk_color    ringCol    = glint_color(255, 110, 110, 110);   // outer ring (unchecked)
  sk_color    checkedCol = glint_color(255,  51, 153, 255);   // outer ring + dot (checked)
  sk_color    dotCol     = glint_color(255, 255, 255, 255);   // inner dot
  sk_color    textCol    = glint_color(255, 210, 210, 210);
  sk_color    boxBg      = glint_color(255,  32,  32,  32);   // unchecked background

  std::shared_ptr<glint_radio_group> group;
  std::function<void(bool)> onChange;

  glint_radio()
  {
    style.display       = "flex";
    style.flexDirection = "row";
    style.alignItems    = "center";
    style.cursor        = "pointer";

    mBox = new BoxComp(this);
    mBox->style.borderWidth    = 1.5f;
    mBox->style.display        = "flex";
    mBox->style.alignItems     = "center";
    mBox->style.justifyContent = "center";
    addChild(mBox);

    mDot = new glint_element();
    mDot->className = "glint_radio_dot";
    mBox->addChild(mDot);

    mLabel = new glint_element();
    mLabel->style.cursor = "pointer";
    addChild(mLabel);

    element.addEventListener("click", [this](glint_event&) { _activate(); });

    // Hover: brighten border
    element.addEventListener("mouseenter", [this](glint_event&) {
      static const sk_color kHoverBorder = glint_color(255, 180, 180, 180);
      if (mBox) mBox->style.borderColor = checked ? checkedCol : kHoverBorder;
      setDirty(false);
    });
    element.addEventListener("mouseleave", [this](glint_event&) {
      if (mBox) mBox->style.borderColor = checked ? checkedCol : ringCol;
      setDirty(false);
    });
  }

  ~glint_radio()
  {
    if (group) group->unregister_member(this);
  }

  void setSize(float s)
  {
    size                     = s;
    style.gap                = std::roundf(s * 0.5f);
    mBox->style.width        = s;
    mBox->style.height       = s;
    mBox->style.borderRadius = s * 0.5f;
    const float dotSz        = std::roundf(s * 0.4f);
    mDot->style.width        = dotSz;
    mDot->style.height       = dotSz;
    mLabel->style.fontSize   = s;
    setDirty(false);
  }

  void setChecked(bool v, bool fireCallback = true)
  {
    checked = v;
    mBox->style.borderColor     = v ? checkedCol : ringCol;
    mBox->style.backgroundColor = v ? checkedCol : boxBg;
    mDot->style.display         = v ? "" : "none";
    setDirty(false);
    if (fireCallback && onChange) onChange(v);
  }

  const char* typeName() const override { return "radio"; }

  void Layout(glint_canvas* g) override
  {
    _sync();
    glint_element::Layout(g);
  }

  void syncBeforeLayout() override { _sync(); }

  float preferredH(float /*availW*/) const override { return size; }

private:
  class BoxComp : public glint_element
  {
  public:
    glint_radio* owner;
    explicit BoxComp(glint_radio* o) : owner(o) {}
    const char* typeName() const override { return "radio-box"; }
  };

  BoxComp*       mBox   = nullptr;
  glint_element* mDot   = nullptr;
  glint_element* mLabel = nullptr;

  bool _needsSync = true;

  // Runs once before first layout to apply values set via direct field assignment
  // (e.g. radio.size = 20.f) during the fromClass config lambda.
  // After that, use setSize() / setChecked() for any dynamic updates.
  void _sync()
  {
    if (!_needsSync) return;
    _needsSync = false;

    setSize(size);
    setChecked(checked, false);

    if (mLabel)
    {
      mLabel->innerText     = text;
      mLabel->style.color   = textCol;
      mLabel->style.display = text.empty() ? "none" : "block";
    }
  }

  void _activate()
  {
    if (checked) return;   // already selected

    if (group)
    {
      group->select(value);
    }
    else
    {
      setChecked(true);
    }
  }
};

// ── glint_radio_group::select (defined after glint_radio) ────────────────────
inline void glint_radio_group::select(const std::string& value)
{
  selected = value;
  for (glint_radio* r : members)
  {
    const bool shouldBe = (r->value == value);
    if (r->checked != shouldBe)
      r->setChecked(shouldBe, false);
  }
  if (onChange) onChange(value);
}
