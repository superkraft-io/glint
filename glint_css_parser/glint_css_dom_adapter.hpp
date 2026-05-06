#pragma once

/**
 * glint_css_dom_adapter.hpp
 * Bridges glint_element to GlintCssDomElement so the CSS selector engine
 * and cascade algorithms can match rules against live scene-graph elements.
 *
 * Include AFTER glint_element.hpp (requires the full glint_element definition).
 *
 * Usage:
 *   GlintCssDomAdapter dom(myElement);
 *   if (complexSelector.matches(dom)) { ... }
 */

#include "glint_css_selector.hpp"
#include "../glint_element.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ── GlintCssDomAdapter ─────────────────────────────────────────────────────────
// Wraps a glint_element* and exposes the GlintCssDomElement interface needed by
// the selector engine.  The adapter is lightweight (one pointer + one shared_ptr
// for the lazy parent chain) and can be stack-allocated.
struct GlintCssDomAdapter : GlintCssDomElement
{
  glint_element* el = nullptr;

  // When true, pseudoState() always returns true regardless of element state.
  // Used by the inspector's style panel so that :hover/:active/:focus rules
  // are always shown, even when the element is not currently in those states.
  bool forcePseudoClasses = false;

  // Lazily-created adapter for mParent.  Kept alive for the duration of any
  // matches() call that walks up the ancestry chain via parent().
  mutable std::shared_ptr<GlintCssDomAdapter> mParentAdapter;

  explicit GlintCssDomAdapter(glint_element* e) : el(e) {}

  // ── GlintCssDomElement interface ──────────────────────────────────────────

  // Tag name lower-case — comes from el->tagName() which already returns a
  // stable string (e.g. "div", "img", "svg", "insp_style_panel").
  std::string tagName() const override
  {
    return el ? std::string(el->tagName()) : "";
  }

  // DOM id attribute.
  std::string id() const override
  {
    return el ? el->element.id : "";
  }

  // Space-separated class names split into a vector.
  std::vector<std::string> classNames() const override
  {
    if (!el || el->className.empty()) return {};
    std::vector<std::string> result;
    std::istringstream ss(el->className);
    std::string tok;
    while (ss >> tok) result.push_back(tok);
    return result;
  }

  // Named attribute access.  Only "id" and "class" are wired for now.
  std::string attribute(const std::string& name, bool& found) const override
  {
    if (!el) { found = false; return ""; }
    if (name == "id")    { found = !el->element.id.empty(); return el->element.id; }
    if (name == "class") { found = !el->className.empty();  return el->className;  }
    return el->getAttribute(name, found);
  }

  // Pseudo-state queries: read flags set by glint_document event handlers.
  bool pseudoState(const std::string& pseudo) const override
  {
    if (!el) return false;
    if (forcePseudoClasses) return true;  // inspector mode: treat all pseudo-classes as active
    if (pseudo == "hover")        return el->mIsHovered;
    if (pseudo == "active")       return el->mIsActive;
    if (pseudo == "focus")        return el->mIsFocused;
    if (pseudo == "focus-within") return el->mIsFocusWithin;
    return false;
  }

  // Parent adapter — constructed lazily on first call.
  const GlintCssDomElement* parent() const override
  {
    if (!el || !el->mParent) return nullptr;
    if (!mParentAdapter)
    {
      mParentAdapter = std::make_shared<GlintCssDomAdapter>(el->mParent);
      mParentAdapter->forcePseudoClasses = forcePseudoClasses;
    }
    return mParentAdapter.get();
  }

  // 0-based index among all siblings.
  size_t childIndex() const override
  {
    if (!el || !el->mParent) return 0;
    size_t idx = 0;
    for (const auto& c : el->mParent->mChildren)
    {
      if (c.get() == el) return idx;
      ++idx;
    }
    return 0;
  }

  // Total number of siblings (including self).
  size_t siblingCount() const override
  {
    if (!el || !el->mParent) return 1;
    return el->mParent->mChildren.size();
  }

  // 0-based index among siblings of the same type.
  size_t typeChildIndex() const override
  {
    if (!el || !el->mParent) return 0;
    const std::string myType = tagName();
    size_t idx = 0;
    for (const auto& c : el->mParent->mChildren)
    {
      if (c.get() == el) return idx;
      if (std::string(c->tagName()) == myType) ++idx;
    }
    return 0;
  }

  // Total number of same-type siblings.
  size_t typeSiblingCount() const override
  {
    if (!el || !el->mParent) return 1;
    const std::string myType = tagName();
    size_t count = 0;
    for (const auto& c : el->mParent->mChildren)
      if (std::string(c->tagName()) == myType) ++count;
    return count;
  }

  // True when the element has no parent (i.e. it is the document root).
  bool isRoot() const override
  {
    return el && !el->mParent;
  }

  // True when the element has no children and no text content.
  bool isEmpty() const override
  {
    return el && el->mChildren.empty() && el->innerText.empty();
  }
};
