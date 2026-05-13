#pragma once

/**
 * glint_tooltip.hpp
 * Tooltip component for glint — shows a floating label on hover.
 *
 * The tooltip popup is absolutely positioned above the element.
 * It appears on mouseenter and disappears on mouseleave.
 * It also hides on pointer press so rebuilt controls cannot leave the popup stuck.
 *
 * Usage (direct):
 *   add.fromClass<glint_tooltip>([](glint_tooltip& t) {
 *     t.text            = "This is a tooltip";
 *     t.style.display   = "inline-flex";
 *     t.style.alignItems = "center";
 *     // Add whatever child you want to trigger the tooltip:
 *     t.add.button([](glint_button& b) { b.innerText = "Hover me"; ... });
 *   });
 *
 * Positioning: the popup floats above the element centre.
 * If the popup would go off screen, it falls back to below.
 *
 * The tooltip element is added as the last child so it paints on top.
 */

#include "../glint_element.hpp"
#include <string>

class glint_tooltip : public glint_element
{
public:
  // ── Public fields ──────────────────────────────────────────────────────────
  std::string text;

  // ── Construction ──────────────────────────────────────────────────────────
  glint_tooltip()
  {
    className = "glint_tooltip";

    // Popup element — NOT added as a child here.
    // It is added lazily to mRoot->mCanvas (the body) on the first Layout so
    // it sits at the top of the paint tree and z-index: 1000 actually works
    // across the whole document.
    mPopup = new glint_element();
    mPopup->className = "glint_tooltip_popup";
    mPopup->innerText = text;

    element.addEventListener("mouseenter", [this](glint_event&) {
      if (text.empty()) return;
      _ensurePopupInBody();
      mPopup->innerText = text;
      mPopup->className = "glint_tooltip_popup glint_tooltip_popup--visible";
      _positionPopup();
      setDirty(false);
    });

    element.addEventListener("mouseleave", [this](glint_event&) {
      _hidePopup();
    });

    element.addEventListener("mousedown", [this](glint_event&) {
      _hidePopup();
    });
  }

  const char* typeName() const override { return "tooltip-wrapper"; }

  void Layout(glint_canvas* g) override
  {
    glint_element::Layout(g);
    _ensurePopupInBody();
    if (mPopupInBody && mPopup->className._value.find("--visible") != std::string::npos)
      _positionPopup();
  }

private:
  glint_element* mPopup      = nullptr;
  bool           mPopupInBody = false;

  void _hidePopup()
  {
    if (!mPopupInBody || !mPopup) return;
    mPopup->className = "glint_tooltip_popup";
    setDirty(false);
  }

  // Add the popup to the body element (mRoot->mCanvas) on first Layout.
  // After this, body owns the popup and it paints above all normal content.
  void _ensurePopupInBody()
  {
    if (mPopupInBody || !mRoot) return;
    mRoot->mCanvas.addChild(mPopup);
    mPopupInBody = true;
  }

  // Position the popup in body-absolute coordinates.
  // mRect is in document layout space, so subtract ancestor scroll offsets to
  // match the element's visible screen-space position inside scrolled parents.
  void _positionPopup()
  {
    if (!mPopup || !mPopupInBody) return;

    const float popW  = mPopup->mRect.W() > 0.f ? mPopup->mRect.W() : 100.f;
    const float selfW = mRect.W();
    const float selfH = mRect.H();

    float left = mRect.L;
    float top  = mRect.T;
    for (glint_element* p = mParent; p; p = p->mParent) {
      left -= p->mScrollLeft;
      top  -= p->mScrollTop;
    }

    mPopup->style.left = left + (selfW - popW) * 0.5f;
    mPopup->style.top  = top + selfH + 6.f;   // 6 px gap below element
  }
};
