// glint_graphics gives us glint_rect, glint_color, glint_canvas, glint_popup_menu, glint_text, and helpers.
// Host-specific tags stay out of this header — use glint_no_tag / glint_no_val_idx instead.
#pragma once

/**
 * glint_element.hpp
 * Base scene-graph node for glint.
 *
 * glint_element is the building block for all glint components.  It is a
 * plain C++ class owned by the Glint scene graph. One glint_document owns the
 * entire scene graph; all other components are glint_element nodes owned
 * by their parent (or by the root's canvas at the top level).
 *
 * Drawing, layout, and mouse-routing are all driven top-down by glint_document.
 *
 * glint_element renders a styled background (fill, border, optional shadow),
 * exposes a content-area rect that accounts for padding, and recursively
 * draws all owned children after drawContent().
 *
 * Usage:
 *   glint_style panelStyle = glint_style::Outlined(C_BG, C_BORDER, 1.f, 6.f);
 *   panelStyle.padding = "8";
 *
 *   // Usually created via the builder API:
 *   _c.add.component([](glint_component_style& _c) {
 *     _c.style = panelStyle;
 *     ...
 *   });
 */

#include "glint_graphics.hpp"      // glint_rect, glint_color, glint_canvas, glint_popup_menu — centralized seam
#include "glint_types.hpp"   // glint_mouse_mod, glint_no_tag, glint_no_val_idx
#include "utils/glint_debug.hpp"
#include "element/glint_html_element.hpp"  // includes glint_style.hpp transitively
#include "events/glint_keyboard_event.hpp" // glint_keyboard_event, glint_key_press
#include "glint_animator.hpp"      // tickTransitions(), glint_ease_eval, lerp helpers
#if defined(__APPLE__)
#include "platform/glint_platform.hpp"
#endif
#include "render/glint_filter.hpp"
#include "render/glint_mask.hpp"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkRRect.h"
#ifdef OS_WIN
#include "include/ports/SkTypeface_win.h"  // SkFontMgr_New_DirectWrite
#endif
#include "include/core/SkPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkDashPathEffect.h"
#include "shaders/glint_shader_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

// ── Global node-ID counter ──────────────────────────────────────────────────
// Each glint_element gets a unique 64-bit ID assigned by addChild().
// IDs start at 1; 0 means "unassigned" (e.g. the canvas root before the root
// stamps it in the glint_document constructor).
inline std::atomic<uint64_t> glint_id_counter{1};

// glint_graphics gives us glint_rect, glint_color, glint_canvas, glint_popup_menu, glint_text, and helpers.
// Host-specific tags stay out of this header — use glint_no_tag / glint_no_val_idx instead.
using namespace glint_graphics;

// Forward declaration so ComponentAdd can use glint_colorpicker** as a parameter type.
// Full definition is in components/glint_colorpicker.hpp.
class glint_colorpicker;
// Forward declaration so ComponentAdd can use glint_gradient_editor** as a parameter type.
// Full definition is in components/glint_gradient_editor.hpp.
class glint_gradient_editor;
// Forward declaration so ComponentAdd can use glint_dial** as a parameter type.
// Full definition is in components/glint_dial.hpp.
class glint_dial;

// ── glint_rect_xywh ───────────────────────────────────────────────────────────
// CSS-style glint_rect constructor: x, y, width, height.
inline glint_rect glint_rect_xywh(float x, float y, float width, float height)
{
  return glint_rect(x, y, x + width, y + height);
}

inline glint_rect sk_rect(float x, float y, float width, float height)
{
  return glint_rect_xywh(x, y, width, height);
}

// Forward declaration — glint_document is defined in glint_document.hpp.
class glint_document;
// Forward declaration — glint_scrollbar is defined in components/glint_scrollbar.hpp.
class glint_scrollbar;

// ── glint_element ───────────────────────────────────────────────────────────
// Plain C++ base owned by its parent component or by
// glint_document at the top of the tree.

class glint_element
{
  friend class glint_document;  // allowed to read/write cssStyle_ and mHasCssStyle_

public:
  struct glint_render_timing_profile
  {
    double transformDirectMs;
    double transformOffscreenMs;
    double filterInPlaceMs;
    double filterOffscreenMs;
    double backdropMs;
    double selfPaintMs;
    double contentMs;
    double childrenMs;
    double maskMs;
    std::unordered_map<std::string, double> childSubtreeMs;

    glint_render_timing_profile()
      : transformDirectMs(0.0)
      , transformOffscreenMs(0.0)
      , filterInPlaceMs(0.0)
      , filterOffscreenMs(0.0)
      , backdropMs(0.0)
      , selfPaintMs(0.0)
      , contentMs(0.0)
      , childrenMs(0.0)
      , maskMs(0.0)
    {
    }
  };

  static void resetRenderTimingProfile()
  {
    sRenderTimingProfile_ = {};
  }

  static glint_render_timing_profile snapshotRenderTimingProfile()
  {
    return sRenderTimingProfile_;
  }

  // ── Public fields ──────────────────────────────────────────────────────────

  // DOM-compatible event bus — owns element.style; use addEventListener/removeEventListener just like JS:
  //   element.addEventListener("click", [](glint_event& e) { ... });
  //   element.style.backgroundColor = "#1a1a1a";
  glint_html_element element;

  // Reference alias for element.style — write `style.backgroundColor` just as before.
  // Both `style` and `element.style` always refer to the same underlying object.
  glint_style& style;

  // Direct DOM id reference — write `el->id = "name"` without the .element. prefix.
  // Aliases element.id; both always refer to the same std::string.
  std::string& id;

  // CSS class list — space-separated class names, mirrors the DOM `className` attribute.
  // Used by the CSS selector engine (class selectors, e.g. `.text-dim`) and shown
  // in the inspector element tree as ".class-name" suffixes.
  // Example:  el->className = "text-dim active";
  //
  // Reactive: assigning className immediately re-applies the CSS cascade and
  // marks the document layout-dirty, mirroring Chrome's style-recalc-before-layout
  // guarantee. The element's _el pointer is wired up in glint_element's constructor.
  struct glint_reactive_class
  {
    glint_element* _el    = nullptr;  // set in glint_element constructor
    std::string    _value;            // the actual class string

    // Raw (non-notifying) access — used by classList internals which call
    // _notifyChange() themselves after finishing their multi-step mutation.
    std::string& _raw() { return _value; }

    // Assignment operators — fire CSS recascade + layout-dirty immediately.
    glint_reactive_class& operator=(const std::string& v)  { _value = v;            _notify(); return *this; }
    glint_reactive_class& operator=(std::string&&      v)  { _value = std::move(v); _notify(); return *this; }
    glint_reactive_class& operator=(const char*        v)  { _value = v ? v : "";   _notify(); return *this; }

    // Implicit read conversion — makes className usable anywhere a const std::string& is expected.
    operator const std::string&() const { return _value; }

    // Forward the most-used std::string predicates so existing code needs no changes.
    bool        empty()  const { return _value.empty();  }
    const char* c_str()  const { return _value.c_str();  }
    size_t      size()   const { return _value.size();   }
    size_t      length() const { return _value.length(); }

  private:
    void _notify()
    {
      if (!_el) return;
      if (_el->mApplyCss) _el->mApplyCss(_el);
      _el->setDirty(false);
    }
  } className;

  // ── classList — DOM-compatible class manipulation ─────────────────────────
  // Mirrors JS element.classList — same add/remove/toggle/contains API.
  // All mutations re-apply the CSS cascade and trigger a redraw automatically.
  //
  // Usage:
  //   el->classList.add("active");
  //   el->classList.remove("disabled");
  //   bool open = el->classList.toggle("open");  // true if now present
  //   bool has  = el->classList.contains("active");
  struct glint_class_list
  {
    glint_element* _el = nullptr;

    // Returns true when the element currently has the given class.
    bool contains(const std::string& cls) const
    {
      if (!_el || cls.empty()) return false;
      std::istringstream ss(_el->className._value);
      std::string tok;
      while (ss >> tok) if (tok == cls) return true;
      return false;
    }

    // Adds cls if not already present.
    void add(const std::string& cls)
    {
      if (!_el || cls.empty() || contains(cls)) return;
      if (!_el->className._raw().empty()) _el->className._raw() += ' ';
      _el->className._raw() += cls;
      _notifyChange();
    }

    // Removes cls if present.
    void remove(const std::string& cls)
    {
      if (!_el || cls.empty()) return;
      std::string result;
      std::istringstream ss(_el->className._value);
      std::string tok;
      bool changed = false;
      while (ss >> tok)
      {
        if (tok == cls) { changed = true; continue; }
        if (!result.empty()) result += ' ';
        result += tok;
      }
      if (changed) { _el->className._raw() = std::move(result); _notifyChange(); }
    }

    // Toggles cls: adds it if absent, removes it if present.
    // Returns true if the class is now present.
    bool toggle(const std::string& cls)
    {
      if (contains(cls)) { remove(cls); return false; }
      add(cls); return true;
    }

  private:
    void _notifyChange()
    {
      if (!_el) return;
      if (_el->mApplyCss) _el->mApplyCss(_el);
      _el->setDirty(false);
    }
  } classList;

  // Direct DOM reactive scroll properties — mirrors element.scrollTop / element.scrollLeft.
  // Write `el->scrollTop = 200.f;` to scroll programmatically (clamps, fires "scroll", redraws).
  // Read `float v = el->scrollTop;` to get current position.
  glint_html_element::sk_scroll_prop& scrollTop;
  glint_html_element::sk_scroll_prop& scrollLeft;

  // Computed style — what Draw, getContent and HitTest actually read.
  // Synced from `style` each frame by tickTransitions(). During a transition,
  // animated properties are interpolated between the captured "from" value and
  // the current `style` target. Non-animated properties always equal `style`.
  // Layout (Layout/layoutFlex/layoutBlock) continues to read from `style` directly.
  glint_style computedStyle;

  glint_style mergedStyleForLayout() const { return _mergedStyle(); }

  void setCssStyleLayer(const glint_style& css)
  {
    cssStyle_ = css;
    mCssStyleBase = css;
    mHasCssStyle_ = (glint_style_serialize(css) != glint_style_serialize(glint_style{}));
  }

  // CSS-style shorthand layout aliases — applied to `style` during tree finalization.
  // Tokens are space-separated; each maps to one or more style properties.
  // See glint_style::ApplyAlign() for the full token → property mapping.
  // Example:
  //   _c.align = "left middle";  // flex-start justify + center align-items
  //   _c.align = "ttb center";   // column direction, centered main-axis
  //   _c.align = "fullwidth";    // width: 100%
  std::string align = "";

  // Text content for inline flow — any element can carry text, not just glint_element.
  // When non-empty, drawContent() renders it using style typography properties.
  // Set display: "inline" on the element to participate in the parent's inline
  // formatting context (parent must have display: "" or display: "block").
  std::string innerText;

  // Optional host control tag — set in add.*() callbacks; picked up by
  // dispatchChild() so the element can be retrieved via glint_document::GetNodeWithTag().
  int tag = glint_no_tag;

  // Visual (tight) rect — the painted area of this component.
  glint_rect mRect{};

  // Filter expansion support.
  glint_rect mPaintRECT{};       // original tight rect (saved before glint_rect inflation)
  float mFilterPad       = 0.f;
  bool  mFilterPadApplied = false;

  // Parent container dimensions — stamped by addChild() from the parent's rect.
  // Used by ApplySelfSizing() to resolve % widths/heights during tree finalization.
  float mParentW = 0.f;
  float mParentH = 0.f;

  // Stable unique ID — assigned atomically in addChild() (and for the canvas
  // root in glint_document's constructor).  Used by the inspector to map tree-node
  // snapshots back to live glint_element pointers.
  uint64_t mId = 0;

  // Debug-only removal flag toggled by the inspector. Removed nodes stay alive
  // but are treated as hidden and excluded from the inspector tree.
  bool mInspectorRemoved = false;

  // Tag for optional lookup via glint_document::GetNodeWithTag().
  int mTag = glint_no_tag;

  // Set true on components that want keyboard focus (e.g. glint_text_editor_base).
  // glint_document::OnMouseDown calls SetFocus(hit) when this is true, SetFocus(nullptr)
  // when the user clicks a non-focusable area (matching browser focus behaviour).
  bool mAcceptsFocus = false;

  // When false, the component is excluded from Tab/Shift+Tab focus traversal even
  // though mAcceptsFocus is true.  Equivalent to HTML tabIndex="-1": the element
  // can still gain focus via mouse click (e.g. glint_element for text selection) but
  // is not reachable by keyboard navigation.  Defaults to true for interactive
  // controls; glint_element sets it to false during tree finalization.
  bool mTabStop = true;

  // Simple parameter value storage for host-integrated controls.
  // Populated by SetValue(); read by GetValue(). Subclasses may override both.
  double mParamValue = 0.0;

  // ── Tree linkage (set by addChild / glint_document) ───────────────────────────
  glint_canvas*       mpG    = nullptr;  // live glint_canvas pointer, propagated by addChild
  glint_document*      mRoot  = nullptr;  // owning root document node
  glint_element* mParent = nullptr;  // parent node (nullptr for top-level)

  // ── Pseudo-class interaction state (written by glint_document event handlers) ─
  bool mIsHovered    = false;   // :hover        — true while mouse is over this element
  bool mIsActive     = false;   // :active        — true while mouse button is held on this element
  bool mIsFocused    = false;   // :focus         — true while this element holds keyboard focus
  bool mIsFocusWithin = false;  // :focus-within  — true while any descendant (or self) holds focus

  // Set by glint_document; call setDirty() to request a redraw of the whole root.
  std::function<void()> mRequestRedraw;
  std::function<void(glint_element*)> mRequestRedrawDetailed;

  // Set by glint_document; called during tree finalization to apply loaded CSS.
  std::function<void(glint_element*)> mApplyCss;

  // True once this node has run its attachment lifecycle against a live tree.
  // Constructor-built children stay false until an attached ancestor stamps
  // real root/graphics context and calls attachSubtree().
  bool mIsAttachedToTree = false;

  /** Pointer to the document-level mutex that serialises tree snapshot reads
   *  (getUITree on inspector thread) against tree writes (addChild / clearChildren /
   *  removeChild on UI thread).  Set by glint_document on the canvas and propagated
   *  to every child in addChild().  Never owned — lifetime is that of glint_document. */
  std::mutex* mTreeMutex = nullptr;

  // Owned children — may be added either in constructors or during attachment.
  std::vector<std::unique_ptr<glint_element>> mChildren;

  /** SkSL shader instances keyed by user-assigned ID.
   *  Auto-created eagerly the moment style.filter / style.backdropFilter is
   *  assigned (Option B): after the assignment, shaders["id"] is already valid
   *  so params can be set immediately without an explicit make_unique.
   *  Callers may still assign their own instance to override defaults.
   *  Also created lazily at draw time as a fallback. */
  std::map<std::string, std::unique_ptr<glint_shader_base>> shaders;

  // ── Scroll state ──────────────────────────────────────────────────────
  // Set by Layout() every frame; read by Draw(), HitTest(), and element.scroll*.
  float mScrollTop    = 0.f;   // current vertical   scroll offset (px)
  float mScrollLeft   = 0.f;   // current horizontal scroll offset (px)
  float mScrollWidth  = 0.f;   // total measured content width  (set by Layout)
  float mScrollHeight = 0.f;   // total measured content height (set by Layout)

  // Scrollbar child components. Typed as glint_element* so this header doesn't
  // need the full glint_scrollbar definition (forward-declared above).
  // Populated by _ensureScrollbars(); null when no scroll is active.
  glint_element* mScrollbarV   = nullptr;   // vertical   scrollbar child
  glint_element* mScrollbarH   = nullptr;   // horizontal scrollbar child
  glint_element* mScrollCorner = nullptr;   // corner square child

  // ── Construction ────────────────────────────────────────────────────────────

  glint_element() : style(element.style), id(element.id), scrollTop(element.scrollTop), scrollLeft(element.scrollLeft)
  {
    className._el = this;
    classList._el = this;
    // Option B eager shader auto-creation: the moment style.filter or
    // style.backdropFilter is written, pre-populate the shaders map so the
    // caller can set params immediately without an explicit make_unique.
    style.filter        ._setCallback([this](const std::string& v){ _prePopulateShaders(v); });
    style.backdropFilter._setCallback([this](const std::string& v){ _prePopulateShaders(v); });
  }

  virtual ~glint_element()
  {
    // Null out any tracking pointer in the owning root that still points to
    // this node.  Without this, mHoveredNode / mFocusedNode / mMouseDownNode
    // become dangling pointers after clearChildren() destroys live children.
    // notifyDestroyed() is defined at the bottom of glint_document.hpp (same
    // pattern as Blur()) so that glint_document is complete.
    notifyDestroyed();
  }

  // ── API ────────────────────────────────────────────────────────────────────

  /** glint_canvas pointer — valid after addChild() has stamped it. */
  glint_canvas* GetUI() const { return mpG; }

  /** Request a full redraw of the owning root. Conservative: also marks the
   *  document's layout-dirty bit so the next frame will re-run Layout(). Use
   *  setPaintOnlyDirty() instead when the change is purely visual (e.g. a
   *  scroll-offset update). */
  virtual void setDirty(bool /*push*/ = false, int /*valIdx*/ = glint_no_val_idx)
  {
    if (mRoot) _markRootLayoutDirty();
    if (mRequestRedrawDetailed) mRequestRedrawDetailed(this);
    if (mRequestRedraw) mRequestRedraw();
  }

  /** Request a redraw without invalidating the cached layout. Suitable for
   *  changes that only move pixels (e.g. mScrollTop / mScrollLeft updates).
   *  If you mutate any style property that affects sizing/positioning, call
   *  setDirty() instead. */
  void setPaintOnlyDirty()
  {
    if (mRequestRedrawDetailed) mRequestRedrawDetailed(this);
    if (mRequestRedraw) mRequestRedraw();
  }

  /** True when this node needs the window heartbeat to wake it without a
   *  preceding setDirty() call. Default false: most nodes redraw only through
   *  explicit invalidation requests. */
  virtual bool wantsPeriodicRedraw() const { return false; }

  /** Earliest time the next heartbeat-driven redraw should occur.
   *  Default is max() so idle nodes never wake the host timer on their own. */
  virtual std::chrono::steady_clock::time_point nextPeriodicRedrawTime() const
  {
    return std::chrono::steady_clock::time_point::max();
  }

  /**
   * Dispatch a DOM event through the element listener system, then walk up
   * the parent chain if e.bubbles is true (mirrors DOM event propagation).
   *
   * Called internally by glint_document for every mouse event.  You can also call
   * it directly to synthesise custom events:
   *   glint_mouse_event e;
   *   e.type = "click";  e.bubbles = true; e.cancelable = true;
   *   myComp->dispatchDOMEvent(e);
   */
  void dispatchDOMEvent(glint_event& e)
  {
    e.target = this;
    glint_element* node = this;
    while (node)
    {
      e.currentTarget = node;
      node->element._dispatchToListeners(e);
      if (!e.bubbles || e._stopPropagation) break;
      node = node->mParent;
    }
  }

  // ── Simple parameter binding stubs ──────────────────────────────────────
  // For components that read/write a parameter value (e.g. VCRoutingSection).
  // mParamIdx is stored for future real parameter binding.  For now, value
  // is stored locally in mParamValue.
  virtual double GetValue() const       { return mParamValue; }
  virtual void   SetValue(double v,
                          int = kNoValIdx) { mParamValue = v; }

  /**
   * Returns the visual (tight) rect, ignoring any filter-pad inflation.
   * Use this whenever you need the actual painted bounds of the component.
   */
  glint_rect GetPaintRECT() const
  {
    return (mFilterPadApplied && mFilterPad > 0.f) ? mPaintRECT : mRect;
  }

  /** Returns the full rect (may be inflated by filter pad). */
  glint_rect GetRECT() const { return mRect; }

  /** Returns the inner rect after subtracting padding and border from all four sides. */
  glint_rect getContent() const
  {
    const glint_rect& base = GetPaintRECT();
    const float bT = computedStyle.resolvedBorderWidth(0);
    const float bR = computedStyle.resolvedBorderWidth(1);
    const float bB = computedStyle.resolvedBorderWidth(2);
    const float bL = computedStyle.resolvedBorderWidth(3);
    return glint_rect(
      base.L + static_cast<float>(computedStyle.paddingLeft)   + bL,
      base.T + static_cast<float>(computedStyle.paddingTop)    + bT,
      base.R - static_cast<float>(computedStyle.paddingRight)  - bR,
      base.B - static_cast<float>(computedStyle.paddingBottom) - bB
    );
  }

  /**
   * Returns the padding-box rect (border-box minus border widths only).
   * Per the CSS spec this is the containing block for position:absolute children.
   * Chrome behaviour: padding is NOT subtracted — only the border is.
   */
  glint_rect GetPaddingBox() const
  {
    const glint_rect& base = GetPaintRECT();
    const float bT = computedStyle.resolvedBorderWidth(0);
    const float bR = computedStyle.resolvedBorderWidth(1);
    const float bB = computedStyle.resolvedBorderWidth(2);
    const float bL = computedStyle.resolvedBorderWidth(3);
    return glint_rect(
      base.L + bL,
      base.T + bT,
      base.R - bR,
      base.B - bB
    );
  }

  // ── Tree mechanics ─────────────────────────────────────────────────────────

  bool isInspectorRemoved() const { return mInspectorRemoved; }

  /**
  * Add a child node. Sets up tree linkage (mpG, mRoot, mRequestRedraw, mParent,
  * mParentW/H) and takes ownership.
  *
  * If this parent is already live in a document tree, the child attachment
  * lifecycle runs immediately. If the parent is still being constructed
  * rootless, attachment is deferred until an attached ancestor calls
  * attachSubtree().
   *
   * If the child already has mParentW/mParentH stamped (non-zero), those values
   * are respected and not overwritten — the builder may pre-stamp them to provide
   * the flex content area (rather than the full panel width/height).
   */
  void addChild(glint_element* node)
  {
    node->mpG                    = mpG;
    node->mRoot                  = mRoot;
    node->mRequestRedraw         = mRequestRedraw;
    node->mRequestRedrawDetailed = mRequestRedrawDetailed;
    node->mApplyCss              = mApplyCss;
    node->mKeyframeRegistryPtr_  = mKeyframeRegistryPtr_;
    node->mTreeMutex             = mTreeMutex;
    node->mParent        = this;
    // Assign a stable unique ID to the child.
    node->mId = glint_id_counter.fetch_add(1, std::memory_order_relaxed);
    // Only stamp if not pre-set (builder may provide flex-content dimensions).
    // Use getContent() — the parent's content-box (after padding+border) — so
    // that percentage widths/heights on children resolve against the same area
    // that Chrome uses: the parent content box, not the border box.
    // Clamp to 0: if the parent's mRect is still {0,0,0,0} (not yet placed by
    // a layout pass), getContent() returns negative values (0 - padding).  A
    // clamped 0 lets tree finalization fall back to mpG->Width/Height() instead,
    // which is the same behaviour as before this change.
    if (node->mParentW == 0.f) node->mParentW = std::max(0.f, getContent().W());
    if (node->mParentH == 0.f) node->mParentH = std::max(0.f, getContent().H());
    // Propagate tree pointers to any grandchildren that were pre-built inside a
    // setup lambda before this node joined the tree (e.g. b.add.image(...) called
    // while 'b' was still rootless).  Those children went through addChild on a
    // null-rooted parent so mpG/mRoot/mApplyCss/etc. are all null — fix them up
    // now, and re-apply CSS that was skipped because mApplyCss was null at their
    // tree finalization time.
    if (!node->mChildren.empty())
    {
      std::function<void(glint_element*)> propagate = [&](glint_element* n)
      {
        for (auto& ch : n->mChildren)
        {
          ch->mpG                   = mpG;
          ch->mRoot                 = mRoot;
          ch->mRequestRedraw        = mRequestRedraw;
          ch->mRequestRedrawDetailed = mRequestRedrawDetailed;
          ch->mApplyCss             = mApplyCss;
          ch->mKeyframeRegistryPtr_ = mKeyframeRegistryPtr_;
          ch->mTreeMutex            = mTreeMutex;
          // Re-apply CSS cascade and re-sync computedStyle / mPrevStyle_ because
          // mApplyCss was null when the child finalized earlier.
          if (mApplyCss)
          {
            mApplyCss(ch.get());
            ch->computedStyle = ch->_mergedStyle();
            ch->mPrevStyle_   = ch->computedStyle;
          }
          propagate(ch.get());
        }
      };
      propagate(node);
    }

    // Add to mChildren BEFORE finalization so structural pseudo-classes
    // (:last-child, :nth-child, :first-child, etc.) evaluate against the final
    // sibling count when mApplyCss fires during tree finalization.
    {
      std::unique_lock<std::mutex> lk;
      if (mTreeMutex) lk = std::unique_lock<std::mutex>(*mTreeMutex);
      mChildren.emplace_back(node);
    }
    if (mIsAttachedToTree)
      node->attachSubtree();
    // Re-cascade the sibling that WAS the last child before this insertion so
    // :last-child is removed from it now that node is the new last sibling.
    if (mApplyCss && mChildren.size() >= 2)
      mApplyCss(mChildren[mChildren.size() - 2].get());
    // Tree shape changed — next frame must relayout.
    if (mRoot) _markRootLayoutDirty();
    // Notify the inspector (if open) that the tree has changed.
    callRootTreeChanged();
  }

  /**
   * Remove and destroy all children.
  * Safe to call at any time after a node has joined the tree; use to dynamically rebuild
   * child lists (e.g. glint_tree rebuilding rows on setTree()).
   * Fires the inspector's onTreeChanged callback after clearing.
   */
  void clearChildren()
  {
    {
      std::unique_lock<std::mutex> lk;
      if (mTreeMutex) lk = std::unique_lock<std::mutex>(*mTreeMutex);
      mChildren.clear();
      add.mCursorY = 0.f;
      // The scrollbar/corner children were owned by mChildren, so they are now
      // destroyed.  Reset the raw pointers so the next Layout() call can recreate
      // them; leaving them non-null would make _ensureScrollbars() skip creation
      // and leave HitTest / Draw with dangling pointers.
      mScrollbarV           = nullptr;
      mScrollbarH           = nullptr;
      mScrollCorner         = nullptr;
      element.scrollCornerBox = nullptr;
    }
    if (mRoot) _markRootLayoutDirty();
    callRootTreeChanged();
  }

  /**
   * Remove and destroy a specific child by raw pointer.
   * The pointer is invalid after this call — null it immediately.
   * No-op if the node is not a direct child.
   * Fires the inspector's onTreeChanged callback.
   */
  void removeChild(glint_element* node)
  {
    bool erased = false;
    {
      std::unique_lock<std::mutex> lk;
      if (mTreeMutex) lk = std::unique_lock<std::mutex>(*mTreeMutex);
      auto it = std::find_if(mChildren.begin(), mChildren.end(),
        [node](const std::unique_ptr<glint_element>& p) { return p.get() == node; });
      if (it != mChildren.end())
      {
        mChildren.erase(it);   // unique_ptr destruction removes the node
        erased = true;
      }
    }
    if (erased)
    {
      if (mRoot) _markRootLayoutDirty();
      callRootTreeChanged();
    }
  }

  /** Remove keyboard focus from this node (delegates to glint_document::SetFocus(nullptr)).
   *  Defined in glint_document.hpp after glint_document is complete. */
  void Blur();  // defined in glint_document.hpp

  /**
   * Notify the inspector that this node's style has been mutated.
   * Call this whenever you change a style property at runtime so the inspector
   * can refresh the CSS style panel for the currently selected node.
   *
   * Example:
   *   myComp->style.backgroundColor = "#ff0000";
   *   myComp->notifyStyleChanged();
   *   myComp->setDirty(false);
   */
  void notifyStyleChanged() { callRootStyleChanged(mId); }

  /** Called from the destructor — defined at the bottom of glint_document.hpp. */
  void notifyDestroyed();

  // ── Type name (for the inspector) ─────────────────────────────────────────
  // Override in subclasses to give each node a readable type label.
  // The inspector window uses this for every tree entry.
  // Set typeNameOverride on any instance to rename it without subclassing.
  std::string typeNameOverride;
  virtual const char* typeName() const
  {
    if (!typeNameOverride.empty()) return typeNameOverride.c_str();
    return "div";
  }

  // ── DOM-compatible API (glint_element interface) ────────────────────────────
  // These methods mirror the DOM Element API directly on the component,
  // eliminating the need for the .element. prefix in the new API.

  /** DOM tagName — identifies this element type. Same as typeName(). */
  virtual const char* tagName() const { return typeName(); }

  /** DOM getAttribute — returns a named attribute value.  Override in
   *  components that expose typed properties as HTML-style attributes
   *  (e.g. glint_input exposes `type`).  Returns "" and sets found=false
   *  when the attribute is not present. */
  virtual std::string getAttribute(const std::string& /*name*/, bool& found) const
  {
    found = false;
    return "";
  }

  /** DOM appendChild — adds a child node. Alias for addChild(). */
  void appendChild(glint_element* node) { addChild(node); }

  /** DOM replaceChildren — removes all children. Alias for clearChildren(). */
  void replaceChildren() { clearChildren(); }

  /**
   * DOM children — lightweight range view over direct child elements.
   * Iterates raw pointers; no heap allocation.  Use in range-for:
   *   for (auto* child : el->children()) { ... }
   * .length() and .item(i) also available.
   */
  auto children() const
  {
    struct View {
      const std::vector<std::unique_ptr<glint_element>>& v;
      struct Iter {
        std::vector<std::unique_ptr<glint_element>>::const_iterator it;
        glint_element* operator*()  const { return it->get(); }
        Iter&            operator++()       { ++it; return *this; }
        bool             operator!=(const Iter& o) const { return it != o.it; }
      };
      Iter   begin()        const { return {v.begin()}; }
      Iter   end()          const { return {v.end()}; }
      size_t length()       const { return v.size(); }
      glint_element* item(size_t i) const { return v[i].get(); }
    };
    return View{mChildren};
  }

  /** DOM parentElement — the parent element, or nullptr for root / canvas. */
  glint_element* parentElement() const { return mParent; }

  /**
   * DOM addEventListener — directly on the element without .element. prefix.
   * Returns an int listener ID for use with removeEventListener().
   * Identical to element.addEventListener().
   */
  int addEventListener(const std::string&          type,
                       glint_event_listener          listener,
                       glint_event_listener_options  options = {})
  {
    return element.addEventListener(type, std::move(listener), options);
  }

  /** Boolean useCapture overload — mirrors older DOM addEventListener signature. */
  int addEventListener(const std::string&   type,
                       glint_event_listener  listener,
                       bool                  useCapture)
  {
    return element.addEventListener(type, std::move(listener), useCapture);
  }

  /** DOM removeEventListener — pass the ID returned by addEventListener(). */
  void removeEventListener(int listenerId) { element.removeEventListener(listenerId); }

  // ── createElement tag registry ─────────────────────────────────────────────
  // Maps string tag names to factory functions.  Register built-in components
  // at static init time; call createElement("button") from scripts or C++.
  // Mirrors customElements.define() / document.createElement() semantics.

  /**
   * Register a factory for a tag name.  Can be called at static init time
   * from any translation unit that includes this header.
   *   glint_element::registerElement("button", []{ return new glint_button(); });
   */
  static void registerElement(const std::string& tag,
                               std::function<glint_element*()> factory)
  {
    _elementFactories()[tag] = std::move(factory);
  }

  /**
   * Create an element by tag name.  Falls back to a plain glint_element
   * if the tag has not been registered.
   *   auto* el = glint_document::createElement("button");
   */
  static glint_element* createElement(const std::string& tag)
  {
    auto& f = _elementFactories();
    auto it = f.find(tag);
    return (it != f.end()) ? it->second() : new glint_element();
  }

  // ── Intrinsic self-sizing (leaf components, e.g. glint_element) ─────────────
  // Override these to report natural pixel dimensions when style.width/height
  // is unset ("" / "fit-content" / "auto"). Explicit style values always win.
  // The layout engine calls these only for leaf nodes (no in-flow children).
  struct _TxtLine {
    std::string text;
    int byteStart;
    int byteEnd;
  };

  struct _TxtRenderLine {
    std::string text;
    int byteStart = 0;
    int byteEnd = 0;
    float x = 0.f;
    float top = 0.f;
    float baselineY = 0.f;
    float drawBaselineY = 0.f;
    float lineHeight = 0.f;
    float width = 0.f;
    float lineBoxTop = 0.f;
    float lineBoxBottom = 0.f;
    float inkTop = 0.f;
    float inkBottom = 0.f;
  };

  enum class _WhiteSpaceMode {
    Normal,
    NoWrap,
    Pre,
    PreLine,
    PreWrap,
    BreakSpaces
  };

  _WhiteSpaceMode _whiteSpaceMode() const
  {
    std::string ws = computedStyle.whiteSpace;
    for (char& c : ws)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ws.empty() || ws == "normal") return _WhiteSpaceMode::Normal;
    if (ws == "nowrap") return _WhiteSpaceMode::NoWrap;
    if (ws == "pre") return _WhiteSpaceMode::Pre;
    if (ws == "pre-line") return _WhiteSpaceMode::PreLine;
    if (ws == "pre-wrap") return _WhiteSpaceMode::PreWrap;
    if (ws == "break-spaces") return _WhiteSpaceMode::BreakSpaces;
    return _WhiteSpaceMode::Normal;
  }

  static bool _isWhiteSpaceChar(char c)
  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  }

  std::vector<_TxtLine> _buildWrappedLines(const SkFont& font, float availW) const
  {
    std::vector<_TxtLine> result;
    if (innerText.empty()) { result.push_back({"", 0, 0}); return result; }

    const _WhiteSpaceMode ws = _whiteSpaceMode();
    const bool collapseWS = (ws == _WhiteSpaceMode::Normal || ws == _WhiteSpaceMode::NoWrap || ws == _WhiteSpaceMode::PreLine);
    const bool preserveNL = (ws == _WhiteSpaceMode::Pre || ws == _WhiteSpaceMode::PreLine || ws == _WhiteSpaceMode::PreWrap || ws == _WhiteSpaceMode::BreakSpaces);
    const bool wrap       = (availW > 0.f) && (ws == _WhiteSpaceMode::Normal || ws == _WhiteSpaceMode::PreLine || ws == _WhiteSpaceMode::PreWrap || ws == _WhiteSpaceMode::BreakSpaces);
    const bool breakSpaces = (ws == _WhiteSpaceMode::BreakSpaces);

    struct Tok {
      std::string text;
      int byteOff = 0;
      int byteEnd = 0;
      bool isNL = false;
      bool isWord = false;
    };

    std::vector<Tok> toks;
    if (collapseWS)
    {
      std::string cur;
      int curOff = 0;
      int i = 0;
      while (i < static_cast<int>(innerText.size()))
      {
        const char c = innerText[static_cast<std::size_t>(i)];
        if (_isWhiteSpaceChar(c))
        {
          if (!cur.empty())
          {
            toks.push_back({cur, curOff, i, false, true});
            cur.clear();
          }
          if (preserveNL && c == '\n')
          {
            toks.push_back({"", i, i + 1, true, false});
          }
          ++i;
          continue;
        }

        if (cur.empty()) curOff = i;
        cur.push_back(c);
        ++i;
      }
      if (!cur.empty())
        toks.push_back({cur, curOff, static_cast<int>(innerText.size()), false, true});
    }
    else
    {
      std::string cur;
      int curOff = 0;
      bool curSpace = false;

      auto flush = [&](int endOff) {
        if (cur.empty()) return;
        toks.push_back({cur, curOff, endOff, false, !curSpace});
        cur.clear();
      };

      int i = 0;
      while (i < static_cast<int>(innerText.size()))
      {
        char c = innerText[static_cast<std::size_t>(i)];
        if (c == '\r') { ++i; continue; }
        if (c == '\t') c = ' ';

        if (preserveNL && c == '\n')
        {
          flush(i);
          toks.push_back({"", i, i + 1, true, false});
          ++i;
          continue;
        }

        const bool isSpace = (c == ' ');
        if (breakSpaces && isSpace)
        {
          flush(i);
          toks.push_back({" ", i, i + 1, false, false});
          ++i;
          continue;
        }

        if (cur.empty())
        {
          curOff = i;
          curSpace = isSpace;
          cur.push_back(c);
        }
        else if (isSpace == curSpace)
        {
          cur.push_back(c);
        }
        else
        {
          flush(i);
          curOff = i;
          curSpace = isSpace;
          cur.push_back(c);
        }
        ++i;
      }
      flush(static_cast<int>(innerText.size()));
    }

    std::string line;
    int lineByteStart = 0;
    int lineByteEnd = 0;
    bool lineStarted = false;

    auto flushLine = [&]() {
      result.push_back({line, lineStarted ? lineByteStart : lineByteEnd, lineByteEnd});
      line.clear();
      lineStarted = false;
    };

    for (const Tok& t : toks)
    {
      if (t.isNL)
      {
        flushLine();
        lineByteStart = lineByteEnd = t.byteEnd;
        continue;
      }

      std::string probe;
      if (collapseWS)
      {
        if (line.empty()) probe = t.text;
        else if (t.isWord) probe = line + " " + t.text;
        else probe = line;
      }
      else
      {
        probe = line + t.text;
      }

      SkRect bounds;
      const float probeAdv = font.measureText(probe.c_str(), probe.size(), SkTextEncoding::kUTF8, &bounds);

      if (wrap && !line.empty() && probeAdv > availW)
      {
        flushLine();
        line = collapseWS ? t.text : t.text;
        lineByteStart = t.byteOff;
        lineByteEnd = t.byteEnd;
        lineStarted = true;
      }
      else
      {
        if (!lineStarted)
        {
          lineByteStart = t.byteOff;
          lineStarted = true;
        }
        line = std::move(probe);
        lineByteEnd = t.byteEnd;
      }
    }

    flushLine();
    if (result.empty()) result.push_back({"", 0, 0});
    return result;
  }

  std::vector<_TxtLine> _buildWrappedLines(const SkFont& font) const
  {
    return _buildWrappedLines(font, getContent().W());
  }

  std::vector<_TxtRenderLine> _buildRenderLines(const SkFont& font) const
  {
    const glint_rect r = getContent();
    const float availW = r.W();

    // Reuse cached lines when the inputs that drive shaping/wrapping/positioning
    // are unchanged since the last build. This is the hot path on text-heavy
    // pages: layoutBlock/layoutFlex/layoutTable used to clear this cache every
    // frame, forcing a full re-measure even when nothing changed.
    if (!mInlineTextRenderLines.empty())
    {
      // Externally populated (e.g. by the inline formatting context in
      // layoutInline) — trust the existing lines, same as the legacy path.
      if (!mTxtCacheValid)
        return mInlineTextRenderLines;

      if (   mTxtCacheText      == innerText
          && mTxtCacheWidth     == availW
          && mTxtCacheLeft      == r.L
          && mTxtCacheTop       == r.T
          && mTxtCacheFontSize  == computedStyle.fontSize.toFloat()
          && mTxtCacheFontWeight== computedStyle.fontWeight
          && mTxtCacheLineHeight== computedStyle.lineHeight
          && mTxtCacheTextAlign == computedStyle.textAlign
          && mTxtCacheFontFamily== computedStyle.fontFamily
          && mTxtCacheFontStyle == computedStyle.fontStyle)
      {
        return mInlineTextRenderLines;
      }
    }

    std::vector<_TxtRenderLine> result;
    const auto wrapped = _buildWrappedLines(font, availW);
    if (wrapped.empty()) {
      mInlineTextRenderLines.clear();
      mTxtCacheValid = false;
      return result;
    }

    const float sz = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 12.f;
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const float ascent  = std::max(0.f, -metrics.fAscent);
    const float descent = std::max(0.f,  metrics.fDescent);
    const float normalLineH = (-metrics.fAscent + metrics.fDescent + std::max(0.f, metrics.fLeading));
    const float lh = computedStyle.lineHeight > 0.f
                     ? sz * computedStyle.lineHeight
             : normalLineH;
    const float halfLeading = std::max(0.f, lh - (ascent + descent)) * 0.5f;
    const float baselineFromTop = halfLeading + ascent;

    float top = r.T;
    for (const auto& ln : wrapped)
    {
      float lineW = 0.f;
      const float baselineY = top + baselineFromTop;
      float inkTopAboveBaseline = ascent;
      float inkBottomBelowBaseline = descent;
      if (!ln.text.empty())
      {
        SkRect bounds;
        lineW = font.measureText(ln.text.c_str(), ln.text.size(), SkTextEncoding::kUTF8, &bounds);
        if (bounds.height() > 0.f)
        {
          inkTopAboveBaseline = -bounds.top();
          inkBottomBelowBaseline = bounds.bottom();
        }
      }

      const float inkSpan = std::max(0.f, inkTopAboveBaseline + inkBottomBelowBaseline);
      const float drawLeadTop = std::max(0.f, lh - inkSpan) * 0.5f;
      const float drawBaselineY = top + drawLeadTop + inkTopAboveBaseline;
      const float inkTop = drawBaselineY - inkTopAboveBaseline;
      const float inkBottom = drawBaselineY + inkBottomBelowBaseline;

      float x = r.L + 2.f;
      if (computedStyle.textAlign == EAlign::Center)
        x = r.L + (r.W() - lineW) * 0.5f;
      else if (computedStyle.textAlign == EAlign::Far)
        x = r.R - lineW - 2.f;

      result.push_back({
        ln.text,
        ln.byteStart,
        ln.byteEnd,
        x,
        top,
        baselineY,
        drawBaselineY,
        lh,
        lineW,
        top,
        top + lh,
        inkTop,
        inkBottom
      });
      top += lh;
    }

    // Populate the cache and stamp the key so subsequent frames with identical
    // inputs short-circuit at the top of this function.
    mInlineTextRenderLines = result;
    mTxtCacheValid       = true;
    mTxtCacheText        = innerText;
    mTxtCacheWidth       = availW;
    mTxtCacheLeft        = r.L;
    mTxtCacheTop         = r.T;
    mTxtCacheFontSize    = computedStyle.fontSize.toFloat();
    mTxtCacheFontWeight  = computedStyle.fontWeight;
    mTxtCacheLineHeight  = computedStyle.lineHeight;
    mTxtCacheTextAlign   = computedStyle.textAlign;
    mTxtCacheFontFamily  = computedStyle.fontFamily;
    mTxtCacheFontStyle   = computedStyle.fontStyle;
    return result;
  }

  void _clearInlineTextRenderLines()
  {
    mInlineTextRenderLines.clear();
    mTxtCacheValid = false;
  }

  // syncBeforeLayout(): called by the builder immediately after user-code
  // setup, before resolving the bounding rect. Override to run initialisation
  // that normally defers to the first frame tick (e.g. _syncFromProps in
  // composite controls like glint_checkbox). The base implementation is a no-op.
  virtual void syncBeforeLayout() {}

  virtual float preferredW() const
  {
    if (innerText.empty()) return 0.f;
    const float sz = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 12.f;

    // Cache hit when text + font axes are unchanged. preferredW() takes no
    // width argument, so the only key components are text and font.
    if (   mPrefWValid
        && mPrefWText       == innerText
        && mPrefWFontSize   == sz
        && mPrefWFontWeight == computedStyle.fontWeight
        && mPrefWFontFamily == computedStyle.fontFamily
        && mPrefWFontStyle  == computedStyle.fontStyle)
    {
      return mPrefWValue;
    }

    SkFont font = skFont(sz,
                          computedStyle.fontFamily.c_str(),
                          computedStyle.fontWeight,
                          computedStyle.fontStyle.c_str());
    float maxW = 0.f;
    const auto lines = _buildWrappedLines(font, 0.f);
    for (const auto& ln : lines)
    {
      if (ln.text.empty()) continue;
      SkRect bounds;
      const float lineAdv = font.measureText(ln.text.c_str(), ln.text.size(), SkTextEncoding::kUTF8, &bounds);
      maxW = std::max(maxW, lineAdv);
    }
    const float result = maxW + 4.f;

    mPrefWValue      = result;
    mPrefWText       = innerText;
    mPrefWFontSize   = sz;
    mPrefWFontWeight = computedStyle.fontWeight;
    mPrefWFontFamily = computedStyle.fontFamily;
    mPrefWFontStyle  = computedStyle.fontStyle;
    mPrefWValid      = true;
    return result;
  }
  // preferredH(availW): returns the content height for this leaf element.
  // Pass availW > 0 to simulate word-wrapping at that pixel width; 0 = no wrap (explicit \n only).
  // Uses the identical greedy-probe algorithm as _renderText() so line counts always match.
  virtual float preferredH(float availW = 0.f) const
  {
    if (innerText.empty()) return 0.f;
    const float sz = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 12.f;

    // Cache hit when text + font axes + availW + lineHeight are unchanged.
    if (   mPrefHValid
        && mPrefHAvailW     == availW
        && mPrefHText       == innerText
        && mPrefHFontSize   == sz
        && mPrefHFontWeight == computedStyle.fontWeight
        && mPrefHLineHeight == computedStyle.lineHeight
        && mPrefHFontFamily == computedStyle.fontFamily
        && mPrefHFontStyle  == computedStyle.fontStyle)
    {
      return mPrefHValue;
    }

    SkFont font = skFont(sz,
                          computedStyle.fontFamily.c_str(),
                          computedStyle.fontWeight,
                          computedStyle.fontStyle.c_str());
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const float normalLineH = (-metrics.fAscent + metrics.fDescent + std::max(0.f, metrics.fLeading));
    const float lh = computedStyle.lineHeight > 0.f
                     ? sz * computedStyle.lineHeight
             : normalLineH;

    const auto linesVec = _buildWrappedLines(font, availW);
    const int lines = std::max(1, static_cast<int>(linesVec.size()));
    const float result = static_cast<float>(lines) * lh;

    mPrefHValue      = result;
    mPrefHAvailW     = availW;
    mPrefHText       = innerText;
    mPrefHFontSize   = sz;
    mPrefHFontWeight = computedStyle.fontWeight;
    mPrefHLineHeight = computedStyle.lineHeight;
    mPrefHFontFamily = computedStyle.fontFamily;
    mPrefHFontStyle  = computedStyle.fontStyle;
    mPrefHValid      = true;
    return result;
  }

  // ── Cross-element (global) text selection ──────────────────────────────────
  // Allows glint_document to coordinate selection spanning multiple text nodes,
  // mirroring browser behaviour: dragging across elements highlights all of them.
  // All implementations are in the base — any element with innerText is selectable.

  /** True when this component can contribute text to a cross-element drag. */
  virtual bool isGlobalSelectable() const
  {
    return !innerText.empty() && computedStyle.userSelect != "none";
  }

  /** Byte-offset in this component's text nearest to content-space (x, y). */
  virtual int globalHitTestPos(float x, float y) const
  {
    return _txtHitTestPos(x, y);
  }

  /** Apply the root-assigned selection range [start, end).
   *  start == -1 means no selection.  Must call setDirty(false). */
  virtual void setGlobalSelRange(int s, int e)
  {
    mSelStart = s; mSelEnd = e;
    setDirty(false);
  }

  /** Total byte length of this component's text (for select-all). */
  virtual int globalTextLen() const
  {
    return static_cast<int>(innerText.size());
  }

  /** Text within the currently assigned global-selection range (for Ctrl+C). */
  virtual std::string getGlobalSelText() const
  {
    if (mSelStart < 0 || mSelStart == mSelEnd) return {};
    const int lo = std::min(mSelStart, mSelEnd);
    const int hi = std::max(mSelStart, mSelEnd);
    return innerText.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo));
  }

  /** True when this component intercepts Ctrl+A for its own text selection,
   *  preventing the root's global select-all handler from firing.
   *  glint_text_editor_base overrides this to true so text inputs handle
   *  their own Ctrl+A without the root stealing the event. */
  virtual bool consumesCtrlA() const { return false; }

  /** True when the last drag on this element was word-granularity. */
  virtual bool globalDragIsWordMode() const
  {
    return mTxtDragMode == TxtDragMode::WORD;
  }

  /** Word boundary [start, end) at bytePos — used for double-click cross-element drag. */
  virtual std::pair<int,int> globalWordBoundaryAt(int bytePos) const
  {
    return _txtWordBoundary(innerText, bytePos);
  }

  // ── Hit testing ────────────────────────────────────────────────────────────

  /**
   * Returns the deepest child (or self) whose PaintRECT contains (x, y),
   * or nullptr if this node does not contain the point.
   */
  virtual glint_element* HitTest(float x, float y)
  {
    const glint_rect _pr = GetPaintRECT();
    const SkM44 _mat = computedStyle.ResolveTransformMatrix(_pr.W(), _pr.H(), _pr.MW(), _pr.MH());
    float lx = x, ly = y;
    if (!(_mat == SkM44{}))
    {
      SkM44 _inv;
      if (_mat.invert(&_inv))
      {
        const SkV4 _p = _inv.map(lx, ly, 0.f, 1.f);
        lx = _p.x / _p.w;
        ly = _p.y / _p.w;
      }
    }

    if (computedStyle.display == "none") return nullptr;
    if (computedStyle.pointerEvents == "none") return nullptr;
    if (!GetPaintRECT().Contains(lx, ly)) return nullptr;

    // Scrollable containers: check scrollbars first (they live in screen space),
    // then transform the hit coordinate into content (scroll) space for children.
    if (mScrollbarV || mScrollbarH || mScrollCorner)
    {
      if (mScrollbarV) { if (auto* h = mScrollbarV->HitTest(lx, ly)) return h; }
      if (mScrollbarH) { if (auto* h = mScrollbarH->HitTest(lx, ly)) return h; }
      if (mScrollCorner) { if (auto* h = mScrollCorner->HitTest(lx, ly)) return h; }

      // Clip hit to the visible content area (excluding scrollbar strips).
      const glint_rect cc = _getContentClipRect();
      if (!cc.Contains(lx, ly)) return this;

      // Offset into content (scroll) space.
      const float hx = lx + mScrollLeft;
      const float hy = ly + mScrollTop;
      // Hit-test in reverse z-index order (highest z-index first = topmost painted first).
      // Fast path when no child has a non-zero zIndex (the common case): walk
      // mChildren in reverse without allocating a sort vector.
      bool _anyNonZeroZ = false;
      for (auto& child : mChildren)
      {
        auto* c = child.get();
        if (c == mScrollbarV || c == mScrollbarH || c == mScrollCorner) continue;
        if (c->computedStyle.zIndex != 0) { _anyNonZeroZ = true; break; }
      }
      if (!_anyNonZeroZ)
      {
        for (auto it = mChildren.rbegin(); it != mChildren.rend(); ++it)
        {
          auto* c = it->get();
          if (c == mScrollbarV || c == mScrollbarH || c == mScrollCorner) continue;
          if (auto* hit = c->HitTest(hx, hy)) return hit;
        }
        return this;
      }
      std::vector<glint_element*> _hitOrder;
      _hitOrder.reserve(mChildren.size());
      for (auto it = mChildren.rbegin(); it != mChildren.rend(); ++it)
      {
        auto* c = it->get();
        if (c == mScrollbarV || c == mScrollbarH || c == mScrollCorner) continue;
        _hitOrder.push_back(c);
      }
      std::stable_sort(_hitOrder.begin(), _hitOrder.end(),
        [](const glint_element* a, const glint_element* b) {
          return a->computedStyle.zIndex > b->computedStyle.zIndex;
        });
      for (auto* c : _hitOrder)
        if (auto* hit = c->HitTest(hx, hy)) return hit;
      return this;
    }

    // Standard hit test (no scroll) — highest z-index tested first.
    {
      bool _anyNonZeroZ = false;
      for (auto& child : mChildren)
        if (child->computedStyle.zIndex != 0) { _anyNonZeroZ = true; break; }
      if (!_anyNonZeroZ)
      {
        for (auto it = mChildren.rbegin(); it != mChildren.rend(); ++it)
          if (auto* hit = it->get()->HitTest(lx, ly)) return hit;
        return this;
      }
      std::vector<glint_element*> _hitOrder;
      _hitOrder.reserve(mChildren.size());
      for (auto it = mChildren.rbegin(); it != mChildren.rend(); ++it)
        _hitOrder.push_back(it->get());
      std::stable_sort(_hitOrder.begin(), _hitOrder.end(),
        [](const glint_element* a, const glint_element* b) {
          return a->computedStyle.zIndex > b->computedStyle.zIndex;
        });
      for (auto* c : _hitOrder)
        if (auto* hit = c->HitTest(lx, ly)) return hit;
    }
    return this;
  }

  // ── Tree finalization ─────────────────────────────────────────────────────

  void finalizeTreeState()
  {
    // Auto-fill parent dimensions from the window if the builder has not set them.
    if (mParentW == 0.f && mpG) mParentW = static_cast<float>(mpG->Width());
    if (mParentH == 0.f && mpG) mParentH = static_cast<float>(mpG->Height());

    if (!align.empty())
    {
      glint_style::ApplyAlign(align, style);
      ApplySelfSizing();
    }

    // Initialise computedStyle and prev-style snapshot so tickTransitions() sees
    // no spurious changes on the first Draw() call.
    computedStyle = style;
    mPrevStyle_   = style;

    // Wire up element.scrollTop / element.scrollLeft reactive setters.
    _initScrollElement();

    // Enable text selection on any element with innerText (unless opted out).
    if (!innerText.empty() && style.userSelect != "none")
    {
      mAcceptsFocus = true;
      mTabStop      = false;
      element.addEventListener("blur", [this](glint_event&) {
        if (mSelStart != -1 || mSelEnd != -1)
        {
          mSelStart = mSelEnd = mDragAnchor = -1;
          setDirty(false);
        }
      });
    }

    // Apply CSS cascade from loaded stylesheets, if available.
    if (mApplyCss) mApplyCss(this);
    // Snapshot the non-pseudo CSS baseline (used for delta in _drawImpl).
    mCssStyleBase = cssStyle_;
    // Refresh computedStyle now that CSS has been applied so the Layout() pass
    // (which runs before Draw) sees the correct CSS values for width, height,
    // display, etc. Also sync mPrevStyle_ to avoid spurious transition detection.
    computedStyle = _mergedStyle();
    mPrevStyle_   = computedStyle;

    // If children were added before this node was attached to a document/root,
    // they missed the normal addChild() stamping path that propagates root/CSS
    // context. Bring the existing subtree up to date now so authored CSS and
    // computedStyle-dependent paint order (e.g. z-index) are correct.
    if (!mChildren.empty())
    {
      auto propagateAttachedContext = [&](auto&& self, glint_element* node) -> void {
        if (!node) return;
        node->mpG                  = mpG;
        node->mRoot                = mRoot;
        node->mRequestRedraw       = mRequestRedraw;
        node->mRequestRedrawDetailed = mRequestRedrawDetailed;
        node->mApplyCss            = mApplyCss;
        node->mKeyframeRegistryPtr_ = mKeyframeRegistryPtr_;
        node->mTreeMutex           = mTreeMutex;
        node->mParentW             = mRect.W();
        node->mParentH             = mRect.H();
        if (mApplyCss)
        {
          mApplyCss(node);
          node->mCssStyleBase = node->cssStyle_;
          node->computedStyle = node->_mergedStyle();
          node->mPrevStyle_   = node->computedStyle;
        }
        for (auto& child : node->mChildren)
          self(self, child.get());
      };

      for (auto& child : mChildren)
        propagateAttachedContext(propagateAttachedContext, child.get());
    }

    // EnsureFilterPad must run after CSS merge so computedStyle.filter is set.
    // It also runs each draw frame (from _drawImpl) to handle relayout resets.
    EnsureFilterPad();
  }

  void attachSubtree();

  // ── Drawing ────────────────────────────────────────────────────────────────

  void _drawImpl(glint_canvas& g, bool tickSelf)
  {
    if (tickSelf) tickTransitions();

    // CSS pseudo-class delta: when hovered/active/focused, CSS :hover/:active rules
    // stored in cssStyle_ may have changed relative to mCssStyleBase (the no-pseudo
    // snapshot). Apply changed properties on top of computedStyle, following the
    // CSS cascade spec: inline styles (el->style) win over pseudo-class rules unless
    // the pseudo rule carries !important (tracked in mCssImportantProps_).
    // When not in any pseudo-state, refresh the baseline snapshot.
    if (mIsHovered || mIsActive || mIsFocused || mIsFocusWithin)
    {
      if (mHasCssStyle_)
      {
        static const glint_style sDefaultStyle{};
        for (const auto& key : glint_animatable_keys())
        {
          const std::string cssNow  = glint_style_get_by_name(cssStyle_,     key);
          const std::string cssBase = glint_style_get_by_name(mCssStyleBase, key);
          if (cssNow != cssBase)
          {
            // Per CSS spec: inline beats pseudo-class unless the pseudo rule is !important.
            if (mCssImportantProps_.count(key) == 0)
            {
              const std::string inlineVal  = glint_style_get_by_name(style,          key);
              const std::string defaultVal = glint_style_get_by_name(sDefaultStyle,  key);
              if (inlineVal != defaultVal) continue;  // inline style wins
            }
            glint_style_lerp_by_name(computedStyle, key, cssBase, cssNow, 1.f);
          }
        }
      }
    }
    else
    {
      mCssStyleBase = cssStyle_;  // keep baseline in sync while no pseudo-state active
    }

    if (computedStyle.display == "none") return;

    SkCanvas* _rootCanvas = static_cast<SkCanvas*>(g.GetDrawContext());
    const glint_rect _tpr = GetPaintRECT();
    const glint_rect _stackBounds = _stackingVisualBounds();
    const SkM44 _tmat = computedStyle.ResolveTransformMatrix(_tpr.W(), _tpr.H(), _tpr.MW(), _tpr.MH());
    const bool _hasTransform = !(_tmat == SkM44{});
    const float _selfOpacity = computedStyle.opacity;
    const bool _needsOpacityLayer = (_selfOpacity < 0.9999f);
    const SkBlendMode _selfBlendMode   = glint_css_blend_mode(computedStyle.mixBlendMode);
    const bool _needsBlendLayer        = (_selfBlendMode != SkBlendMode::kSrcOver);
    const bool _needsIsolationLayer    = (computedStyle.isolation == "isolate");

    // ── CSS transform + opacity + mix-blend-mode + isolation stacking context ─
    // Same logic as _drawToCanvasImpl: open a saveLayer bounded by the untransformed
    // paint rect, apply the transform on the parent canvas before opening the layer,
    // draw the entire subtree inside the layer via DrawToCanvas (which uses the raw
    // Skia canvas directly and is unaware of host clip regions), then restore to
    // composite the layer back with transform + opacity as a unit.
    if (_rootCanvas && (_hasTransform || _needsOpacityLayer || _needsBlendLayer || _needsIsolationLayer))
    {
      // ── Pure transform (no opacity) ───────────────────────────────────────
      // Match Chrome's paint-then-composite model: rasterise the subtree into an
      // offscreen surface at natural coords, then drawImage back with bilinear
      // sampling.  drawImage through an SkM44 perspective CTM performs
      // perspective-correct texture coordinate interpolation (W-div per pixel).
      if (_hasTransform && !_needsOpacityLayer && !_needsBlendLayer && !_needsIsolationLayer)
      {
        const int _ow = std::max(1, static_cast<int>(std::ceil(_stackBounds.W())));
        const int _oh = std::max(1, static_cast<int>(std::ceil(_stackBounds.H())));
        auto _offscreen = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(_ow, _oh));
        if (_offscreen)
        {
          SkCanvas* _oc = _offscreen->getCanvas();
          _oc->translate(-_stackBounds.L, -_stackBounds.T);
          _drawToCanvasImpl(_oc, false, true);
          sk_sp<SkImage> _img = _offscreen->makeImageSnapshot();
          // Pass a paint with setAntiAlias(true) so Skia's CPU raster
          // scan-converter uses AntiFillPath for the projected quad boundary.
          SkPaint _imgPaint;
          _imgPaint.setAntiAlias(true);
          _rootCanvas->save();
          _rootCanvas->concat(_tmat);
          _rootCanvas->drawImage(_img.get(), _stackBounds.L, _stackBounds.T,
              SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
              &_imgPaint);
          _rootCanvas->restore();
        }
        else
        {
          // Fallback: direct draw
          _rootCanvas->save();
          _rootCanvas->concat(_tmat);
          _drawToCanvasImpl(_rootCanvas, false, true);
          _rootCanvas->restore();
        }
        return;
      }

      // ── Transform + opacity: offscreen-raster path (Chrome model) ─────────
      // Same as pure-transform: rasterise at natural bounds, composite back with
      // transform + opacity as post-steps so filter:blur is never degenerate.
      if (_hasTransform)
      {
        const int _ow2 = std::max(1, static_cast<int>(std::ceil(_stackBounds.W())));
        const int _oh2 = std::max(1, static_cast<int>(std::ceil(_stackBounds.H())));
        auto _offscreen2 = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(_ow2, _oh2));
        if (_offscreen2)
        {
          SkCanvas* _oc2 = _offscreen2->getCanvas();
          _oc2->translate(-_stackBounds.L, -_stackBounds.T);
          _drawToCanvasImpl(_oc2, false, true);
          sk_sp<SkImage> _img2 = _offscreen2->makeImageSnapshot();
          SkPaint _imgPaint2;
          _imgPaint2.setAntiAlias(true);
          _imgPaint2.setAlphaf(_selfOpacity);
          _imgPaint2.setBlendMode(_selfBlendMode);
          _rootCanvas->save();
          _rootCanvas->concat(_tmat);
          _rootCanvas->drawImage(_img2.get(), _stackBounds.L, _stackBounds.T,
              SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
              &_imgPaint2);
          _rootCanvas->restore();
        }
        else
        {
          SkPaint _fbPaint;
          _fbPaint.setAlphaf(_selfOpacity);
          _fbPaint.setBlendMode(_selfBlendMode);
          _rootCanvas->save();
          _rootCanvas->concat(_tmat);
          _rootCanvas->saveLayer(nullptr, &_fbPaint);
          _drawToCanvasImpl(_rootCanvas, false, true);
          _rootCanvas->restore();
          _rootCanvas->restore();
        }
        return;
      }

      // ── Opacity / blend-mode / isolation only (no transform) ─────────────
      // Pure mix-blend-mode needs the current backdrop pixels from the element's
      // actual clipped viewport region. Use an outer layer initialised from the
      // current canvas, then flatten the subtree through an inner blend layer.
      if (_needsBlendLayer && !_needsIsolationLayer)
      {
        _rootCanvas->save();
        auto _cr = computedStyle.resolveCornerRadii(_tpr.W(), _tpr.H());
        if (_cr[0]>0.f || _cr[1]>0.f || _cr[2]>0.f || _cr[3]>0.f)
        {
          SkVector _ck[4] = {{_cr[0],_cr[0]},{_cr[1],_cr[1]},{_cr[2],_cr[2]},{_cr[3],_cr[3]}};
          SkRRect rr; rr.setRectRadii(SkRect::MakeLTRB(_tpr.L, _tpr.T, _tpr.R, _tpr.B), _ck);
          _rootCanvas->clipRRect(rr, true);
        }
        else
        {
          _rootCanvas->clipRect(SkRect::MakeLTRB(_tpr.L, _tpr.T, _tpr.R, _tpr.B));
        }
        const SkRect _blendRect = SkRect::MakeLTRB(_tpr.L, _tpr.T, _tpr.R, _tpr.B);
        SkCanvas::SaveLayerRec _blendBackdropRec(&_blendRect, nullptr,
            SkCanvas::kInitWithPrevious_SaveLayerFlag);
        _rootCanvas->saveLayer(_blendBackdropRec);

        SkPaint _blendPaint;
        _blendPaint.setAlphaf(_selfOpacity);
        _blendPaint.setBlendMode(_selfBlendMode);
        _rootCanvas->saveLayer(nullptr, &_blendPaint);
        _drawToCanvasImpl(_rootCanvas, false, true);
        _rootCanvas->restore();
        _rootCanvas->restore();
        _rootCanvas->restore();
        return;
      }

      SkPaint _compPaint;
      _compPaint.setAlphaf(_selfOpacity);
      _compPaint.setBlendMode(_selfBlendMode);
      _rootCanvas->saveLayer(nullptr, &_compPaint);
      _drawToCanvasImpl(_rootCanvas, false, true);
      _rootCanvas->restore();
      return;
    }

    EnsureFilterPad();  // re-run each draw frame; reads computedStyle (updated by tickTransitions)
    glint_rect _expandedRECT = mRect;
    if (mFilterPad > 0.f) mRect = mPaintRECT;

    const bool _hasMask = !computedStyle.mask.empty() && computedStyle.mask != "none";

    _ShaderParseResult _bdParsed, _fParsed;
    if (!computedStyle.backdropFilter.empty() && computedStyle.backdropFilter != "none")
      _bdParsed = _parseFilter(computedStyle.backdropFilter);
    if (!computedStyle.filter.empty() && computedStyle.filter != "none")
      _fParsed  = _parseFilter(computedStyle.filter);

    SkCanvas* _shCanvas = static_cast<SkCanvas*>(g.GetDrawContext());
    for (auto& _shId : _bdParsed.shaderIds) {
      auto _shIt = shaders.find(_shId);
      if (_shIt != shaders.end() && _shIt->second->isBackdrop && _shCanvas) {
        _shIt->second->mDpr = _getRootDpr();
        _shIt->second->beginBackdropLayer(_shCanvas, GetPaintRECT(), computedStyle);
      }
    }
    const bool _hasBackdropFilter = !_bdParsed.css.empty();
    if (_hasBackdropFilter) glint_filter::BeginBackdropLayer(g, GetPaintRECT(), computedStyle, _bdParsed.css);

    if (_hasMask && _rootCanvas)
      _rootCanvas->saveLayer(nullptr, nullptr);

    const bool _hasFilter = !_fParsed.css.empty();
    if (_hasFilter) glint_filter::BeginLayer(g, mRect, _fParsed.css);

    // Draw non-backdrop (bg) shaders as the background layer, before content.
    if (_shCanvas)
      for (auto& _shId : _fParsed.shaderIds) {
        auto _shIt = shaders.find(_shId);
        if (_shIt != shaders.end() && !_shIt->second->isBackdrop)
          _shIt->second->drawDirect(_shCanvas, GetPaintRECT());
      }

    DrawBackground(g, computedStyle);

    // Determine clip / scroll behaviour from overflow-x / overflow-y.
    const bool _clipContent     = (computedStyle.overflowX != "visible" || computedStyle.overflowY != "visible");
    const bool _hasScrollBars   = (mScrollbarV || mScrollbarH || mScrollCorner);
    SkCanvas*  _canvas          = _clipContent
                                  ? static_cast<SkCanvas*>(g.GetDrawContext()) : nullptr;
    if (_canvas)
    {
      _canvas->save();

      // Clip rect: full paint rect, or content area when scrollbars are present.
      const glint_rect clip = _hasScrollBars ? _getContentClipRect() : GetPaintRECT();

      {
        auto _cr = computedStyle.resolveCornerRadii(clip.W(), clip.H());
        if (_cr[0]>0.f || _cr[1]>0.f || _cr[2]>0.f || _cr[3]>0.f)
        {
          SkVector _ck[4] = {{_cr[0],_cr[0]},{_cr[1],_cr[1]},{_cr[2],_cr[2]},{_cr[3],_cr[3]}};
          SkRRect rr; rr.setRectRadii(SkRect::MakeLTRB(clip.L, clip.T, clip.R, clip.B), _ck);
          _canvas->clipRRect(rr, true);
        }
        else
        {
          _canvas->clipRect(SkRect::MakeLTRB(clip.L, clip.T, clip.R, clip.B));
        }
      }

      // Apply scroll translation so children render at their scroll-offset positions.
      if (mScrollLeft != 0.f || mScrollTop != 0.f)
        _canvas->translate(-mScrollLeft, -mScrollTop);
    }

    // Most parents have all children at zIndex 0 (the text page is hundreds of
    // such labels). Detect this fast path and skip the sort vector entirely.
    // computedStyle.zIndex is kept fresh by tickTransitionsAll() running before
    // Draw, so reading it here avoids the heavy _mergedStyle() copy.
    bool _anyNonZeroZ = false;
    for (auto& child : mChildren)
    {
      auto* c = child.get();
      if (c == mScrollbarV || c == mScrollbarH || c == mScrollCorner) continue;
      if (c->computedStyle.zIndex != 0) { _anyNonZeroZ = true; break; }
    }

    if (!_anyNonZeroZ)
    {
      drawContent(g);

      for (auto& child : mChildren)
      {
        auto* c = child.get();
        if (c == mScrollbarV || c == mScrollbarH || c == mScrollCorner) continue;
        c->Draw(g);
      }
    }
    else
    {
      struct _ChildDrawOrder { glint_element* node; int zIndex; };
      std::vector<_ChildDrawOrder> _drawOrder;
      _drawOrder.reserve(mChildren.size());
      for (auto& child : mChildren)
      {
        auto* c = child.get();
        if (c == mScrollbarV || c == mScrollbarH || c == mScrollCorner) continue;
        _drawOrder.push_back({ c, c->computedStyle.zIndex });
      }
      std::stable_sort(_drawOrder.begin(), _drawOrder.end(),
        [](const _ChildDrawOrder& a, const _ChildDrawOrder& b) {
          return a.zIndex < b.zIndex;
        });

      // Negative z-index children paint before the parent's content, which brings
      // the active standalone path closer to Chrome's stacking order.
      for (const auto& entry : _drawOrder)
      {
        if (entry.zIndex >= 0) break;
        entry.node->Draw(g);
      }

      drawContent(g);

      // Non-negative children continue to paint after the parent's content.
      for (const auto& entry : _drawOrder)
      {
        if (entry.zIndex < 0) continue;
        entry.node->Draw(g);
      }
    }

    // Restore scroll clip + translation before drawing scrollbar overlays.
    if (_canvas) _canvas->restore();

  // Border paints on top of content and children but is part of the same
  // rounded source image that filter operates on.
  if (_rootCanvas) _drawBorderSkia(_rootCanvas, computedStyle, mRect);

    if (_hasBackdropFilter) glint_filter::EndBackdropLayer(g);
    // Close backdrop shader layers in REVERSE order.
    for (auto _shIt2 = _bdParsed.shaderIds.rbegin(); _shIt2 != _bdParsed.shaderIds.rend(); ++_shIt2) {
      auto _shSit = shaders.find(*_shIt2);
      if (_shSit != shaders.end() && _shSit->second->isBackdrop && _shCanvas)
        _shSit->second->endBackdropLayer(_shCanvas);
    }
    if (_hasFilter) glint_filter::EndLayer(g);
    // Keep redraws going while any shader is animated.
    for (auto& [_sid, _s] : shaders)
      if (_s->animated) { setDirty(false); break; }

    // CSS parity: border-radius shapes the source paint, filter composites that
    // rounded source, and mask applies afterward to the filtered result.
    if (_hasMask && _rootCanvas)
    {
      auto _maskLayers = glint_parse_mask_layers(computedStyle);

      // Open mask accumulation surface (transparent start).
      // On restore it is composited into the content layer via DST_IN:
      //   content.alpha *= mask.alpha — clean, no kDecal bilinear fringe.
      SkPaint _dstInPaint;
      _dstInPaint.setBlendMode(SkBlendMode::kDstIn);
      _rootCanvas->saveLayer(nullptr, &_dstInPaint);  // L2 — mask accumulation surface

      bool _firstMaskLayer = true;
      for (const auto& _ml : _maskLayers)
      {
        const glint_rect _maskOriginBox = _maskBoxRect(_ml.origin);
        sk_sp<SkShader> _mShader;
        sk_sp<SkImage>  _mImg;

        if (_ml.type == glint_mask_layer::GRADIENT)
        {
          _mShader = glint_mask_gradient_shader(_ml, _maskOriginBox);
        }
        else if (_ml.type == glint_mask_layer::URL_ELEMENT_ID)
        {
          glint_element* _mSrc = findMaskSourceElement(_ml.urlTarget);
          if (_mSrc && _mSrc != this)
          {
            const int _mW = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.W())));
            const int _mH = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.H())));
            auto _mSurf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(_mW, _mH));
            if (_mSurf)
            {
              SkCanvas* _mc = _mSurf->getCanvas();
              _mc->clear(SK_ColorTRANSPARENT);
              _mc->translate(-_maskOriginBox.L, -_maskOriginBox.T);
              _mSrc->DrawToCanvas(_mc);
              _mImg = _mSurf->makeImageSnapshot();
            }
          }
        }
        else if (_ml.type == glint_mask_layer::URL_SVG_FILE ||
                 _ml.type == glint_mask_layer::URL_SVG_FILE_ID)
        {
          auto _dom = glint_load_svg_dom(_ml.urlTarget, _getOnRequest(), this, _getNetworkLog());
          if (_dom)
          {
            const int _mW = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.W())));
            const int _mH = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.H())));
            const char* _fId = (_ml.type == glint_mask_layer::URL_SVG_FILE_ID)
                                ? _ml.urlFragId.c_str() : nullptr;
            _mImg = glint_rasterize_svg(_dom, _mW, _mH, _fId);
          }
        }
        else if (_ml.type == glint_mask_layer::URL_IMAGE)
        {
          _mImg = glint_load_image(_ml.urlTarget, _getOnRequest(), this, _getNetworkLog());
        }

        // Blend mode for compositing into the accumulation surface (not onto content).
        SkPaint _mp;
        _mp.setBlendMode(glint_mask_accum_blend_mode(_ml.composite, _firstMaskLayer));
        if (_ml.mode == "luminance")
          _mp.setColorFilter(glint_mask_luma_color_filter());

        _rootCanvas->save();
        _clipToMaskBox(_rootCanvas, _ml.clip);

        if (_mImg)
        {
          // kStrict_SrcRectConstraint prevents bilinear sampling from reading outside
          // the source rect boundary — eliminates the half-pixel alpha fringe at image edges.
          const SkRect _srcR = SkRect::MakeWH(static_cast<float>(_mImg->width()),
                                              static_cast<float>(_mImg->height()));
          const SkRect _dstR = glint_mask_image_dst_rect(_mImg, _maskOriginBox, _ml);
          _rootCanvas->drawImageRect(_mImg, _srcR, _dstR,
                                     SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear),
                                     &_mp,
                                     SkCanvas::kStrict_SrcRectConstraint);
        }
        else if (_mShader)
        {
          // Gradient / element-source masks: shader-based, no edge artifact.
          _mp.setShader(_mShader);
          _rootCanvas->drawPaint(_mp);
        }

        _rootCanvas->restore();
        _firstMaskLayer = false;
      }
      _rootCanvas->restore();  // L2 restores: DST_IN applied — content.a *= mask.a
      _rootCanvas->restore();  // L1 restores: masked element composited into parent
    }

    // Draw scrollbar overlays in screen space after the element's mask/filter
    // stack has fully resolved back into the parent canvas.
    if (mScrollbarV) mScrollbarV->Draw(g);
    if (mScrollbarH) mScrollbarH->Draw(g);
    if (mScrollCorner) mScrollCorner->Draw(g);

    if (mFilterPad > 0.f) mRect = _expandedRECT;

    // Focus ring — drawn inline at this element's z-order position so it never
    // paints above scene siblings with higher stacking order (e.g. open dropdowns).
    // Drawn after border, filter, mask, and scrollbars, but before siblings drawn later.
    if (mIsFocused && mAcceptsFocus && mTabStop && _isFocusViaKeyboard() && _rootCanvas)
    {
      const glint_rect _fr = GetPaintRECT();
      SkPaint _fp;
      _fp.setStyle(SkPaint::kStroke_Style);
      _fp.setColor(SkColorSetARGB(210, 74, 158, 255));
      _fp.setStrokeWidth(2.f);
      _fp.setAntiAlias(true);
      _rootCanvas->drawRect(SkRect::MakeLTRB(_fr.L, _fr.T, _fr.R, _fr.B), _fp);
    }

    if (glint_debug::colorizedBorders)
      g.DrawRect(glint_debug::borderColorFor(this),
                 mFilterPad > 0.f ? mPaintRECT : mRect, nullptr, 1.0f);

    // (transform + opacity: handled at top of _drawImpl via early-return layer path)
  }

  virtual void Draw(glint_canvas& g)
  {
    _drawImpl(g, true);
  }

  // ── Layout pass ──────────────────────────────────────────────────────────────
  //
  // Called top-down by glint_document::Draw() before the visual draw traversal.
  // Re-computes mRect for every direct child from their current style values
  // (flexGrow, width, height, left, top), then recurses into each child.
  //
  // This makes style mutations reactive: change style.flexGrow + setDirty()
  // and the next frame re-flows the layout automatically — no manual mRect sync.
  //
  // Flex containers (style.display == "flex") run the full flex algorithm.
  // Non-flex containers run cursor-based block flow (matching existing builder).
  //
  virtual void Layout(glint_canvas* g)
  {
    if (mChildren.empty()) return;

    const bool scrollY = (computedStyle.overflowY == "scroll" || computedStyle.overflowY == "auto");
    const bool scrollX = (computedStyle.overflowX == "scroll" || computedStyle.overflowX == "auto");

    if (scrollY || scrollX)
    {
      _layoutScroll(g, scrollX, scrollY);
      return;
    }

    if (computedStyle.display == "flex")
    {
      const glint_rect c = getContent();
      layoutFlex(g, c, c.W(), c.H());
    }
    else if (computedStyle.display == "table")
    {
      const glint_rect c = getContent();
      layoutTable(g, c, c.W(), c.H());
    }
    else
    {
      const glint_rect c = getContent();
      layoutBlock(g, c, c.W(), c.H());
    }
  }

  // ── Mouse events (routed by glint_document) ───────────────────────────────────

  virtual void OnMouseDown(float x, float y, const glint_mouse_mod& mod)
  {
    if (innerText.empty()) return;
    const std::string& us = computedStyle.userSelect;
    if (us == "none") return;
    if (mod.R) { _showContextMenu(x, y); return; }
    if (us == "all")
    {
      mSelStart = 0; mSelEnd = static_cast<int>(innerText.size());
      mDragAnchor = 0; mCursorPos = mSelEnd;
      setDirty(false); return;
    }
    const int pos = _txtHitTestPos(x, y);
    if (mod.S)
    {
      if (mDragAnchor < 0) mDragAnchor = mCursorPos;
      mCursorPos = pos;
      mSelStart  = std::min(mDragAnchor, pos);
      mSelEnd    = std::max(mDragAnchor, pos);
      if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
      setDirty(false); return;
    }
    const auto  now     = std::chrono::steady_clock::now();
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - mTxtLastClickTime).count();
#ifdef _WIN32
    const long long threshold = static_cast<long long>(::GetDoubleClickTime());
#else
    const long long threshold = 500LL;
#endif
    if (elapsed < threshold && std::abs(x - mTxtLastClickX) < 4.f) ++mTxtClickCount;
    else mTxtClickCount = 1;
    mTxtLastClickTime = now;
    mTxtLastClickX    = x;

    if (mTxtClickCount >= 3)
    {
      mSelStart = 0; mSelEnd = static_cast<int>(innerText.size());
      mCursorPos = mSelEnd; mDragAnchor = 0;
      mTxtDragMode = TxtDragMode::ALL;
    }
    else if (mTxtClickCount == 2)
    {
      auto [ws, we]       = _txtWordBoundary(innerText, pos);
      mSelStart           = ws; mSelEnd = we; mCursorPos = we;
      mDragAnchor         = pos;
      mTxtWordAnchorStart = ws; mTxtWordAnchorEnd = we;
      mTxtDragMode        = TxtDragMode::WORD;
    }
    else
    {
      mSelStart = mSelEnd = mDragAnchor = pos;
      mCursorPos   = pos;
      mTxtDragMode = TxtDragMode::CHAR;
    }
    setDirty(false);
  }

  virtual void OnMouseUp  (float /*x*/, float /*y*/, const glint_mouse_mod& /*mod*/) {}

  virtual void OnMouseDrag(float x, float y, float /*dX*/, float /*dY*/,
                            const glint_mouse_mod& /*mod*/)
  {
  if (innerText.empty() || computedStyle.userSelect == "none" ||
    computedStyle.userSelect == "all" || mDragAnchor < 0) return;
    const int pos = _txtHitTestPos(x, y);
    if (mTxtDragMode == TxtDragMode::WORD)
    {
      if (pos <= mTxtWordAnchorStart)
      {
        auto [ws, we] = _txtWordBoundary(innerText, pos);
        mSelStart = ws; mSelEnd = mTxtWordAnchorEnd; mCursorPos = ws;
      }
      else if (pos >= mTxtWordAnchorEnd)
      {
        auto [ws, we] = _txtWordBoundary(innerText, pos);
        mSelStart = mTxtWordAnchorStart; mSelEnd = we; mCursorPos = we;
      }
      else
      {
        mSelStart = mTxtWordAnchorStart; mSelEnd = mTxtWordAnchorEnd;
        mCursorPos = mTxtWordAnchorEnd;
      }
    }
    else
    {
      mSelStart = std::min(mDragAnchor, pos);
      mSelEnd   = std::max(mDragAnchor, pos);
      mCursorPos = pos;
    }
    setDirty(false);
  }

  virtual void OnMouseOver(float /*x*/, float /*y*/, const glint_mouse_mod& /*mod*/) {}
  virtual void OnMouseOut () {}

  /**
   * Called by glint_document::OnMouseWheel after routing to the nearest scrollable
   * ancestor.  Override to intercept or customise wheel behaviour.
   * @param deltaX  horizontal scroll in pixels (positive = right)
   * @param deltaY  vertical scroll in pixels   (positive = down)
   */
  virtual void OnMouseWheel(float /*deltaX*/, float /*deltaY*/, const glint_mouse_mod& /*mod*/) {}

  // ── Keyboard events (routed by glint_document to the focused node) ────────────
  // Only reached when mAcceptsFocus is true and this node currently has focus.
  // Return true to signal the event was handled and suppress default host handling.
  virtual bool OnKeyDown(const glint_key_press& key)
  {
    if (innerText.empty() || computedStyle.userSelect == "none") return false;
    const int n = static_cast<int>(innerText.size());

    if (key.ctrl)
    {
      if (key.vk == 'A')
      {
        mSelStart = 0; mSelEnd = n; mCursorPos = n;
        setDirty(false); return true;
      }
      if (key.vk == 'C' && mSelStart >= 0 && mSelStart != mSelEnd)
      {
        const int lo = std::min(mSelStart, mSelEnd);
        const int hi = std::max(mSelStart, mSelEnd);
        _txtCopyToClipboard(innerText.substr(static_cast<size_t>(lo),
                                             static_cast<size_t>(hi - lo)));
        return true;
      }
      if (key.vk == 0x25 || key.vk == 0x27)  // Ctrl+Left/Right
      {
        const bool goLeft = (key.vk == 0x25);
        if (key.shift && mSelStart == -1) mSelStart = mCursorPos;
        mCursorPos = goLeft ? _txtWordLeft(mCursorPos) : _txtWordRight(mCursorPos);
        if (key.shift) { mSelEnd = mCursorPos; if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1; }
        else { mSelStart = mSelEnd = -1; }
        setDirty(false); return true;
      }
      if (key.vk == 0x24 || key.vk == 0x23)  // Ctrl+Home/End
      {
        if (key.shift && mSelStart == -1) mSelStart = mCursorPos;
        mCursorPos = (key.vk == 0x24) ? 0 : n;
        if (key.shift) { mSelEnd = mCursorPos; if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1; }
        else { mSelStart = mSelEnd = -1; }
        setDirty(false); return true;
      }
      return false;
    }

    if (key.shift)
    {
      if (key.vk == 0x25 || key.vk == 0x27)  // Shift+Left/Right
      {
        if (mSelStart == -1) mSelStart = mCursorPos;
        if (key.vk == 0x25) mCursorPos = _txtPrevCP(innerText, mCursorPos);
        else mCursorPos = std::min(_txtNextCP(innerText, mCursorPos), n);
        mSelEnd = mCursorPos;
        if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
        setDirty(false); return true;
      }
      if (key.vk == 0x24)  // Shift+Home
      {
        if (mSelStart == -1) mSelStart = mCursorPos;
        mCursorPos = mSelEnd = 0;
        if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
        setDirty(false); return true;
      }
      if (key.vk == 0x23)  // Shift+End
      {
        if (mSelStart == -1) mSelStart = mCursorPos;
        mCursorPos = mSelEnd = n;
        if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
        setDirty(false); return true;
      }
    }
    return false;
  }
  virtual bool OnKeyUp(const glint_key_press& /*key*/) { return false; }

  // ── Focus lifecycle (called by glint_document::SetFocus) ──────────────────────
  // Override to react when focus is gained or lost.  The DOM "focus"/"blur"
  // events are also dispatched on element for addEventListener-based handling.
  virtual void onFocusGained() {}
  virtual void onFocusLost  () {}

  // ── Declarative child adder ──────────────────────────────────────────────────
  // Use this->add.div / button / svg / component etc. inside constructors.
  // Method bodies are defined in glint_builder.hpp once all style types are known.
  // You must include glint/glint.hpp (not glint_element.hpp directly) to
  // get the full method set.
  // Forward declarations for template parameter types used below.
  // (Full definitions are in their respective component headers.)
  struct ComponentAdd
  {
    glint_element* _owner;
    // Block-flow cursor: tracks the Y offset of the next auto-positioned child,
    // relative to the component's top edge. Advances after each non-absolute child
    // is placed — mirroring browser block-flow layout.
    float mCursorY = 0.f;

    template<typename S> auto button     (S&& setup);
    template<typename S> auto image      (S&& setup);
    template<typename S> auto input      (S&& setup);
    template<typename S> auto list       (S&& setup);
    template<typename T, typename S> T* custom(S&& setup, T** out = nullptr);
    template<typename T, typename S> T* fromClass(S&& setup, T** out = nullptr)
    {
      return custom<T>(std::forward<S>(setup), out);
    }
    template<typename S> auto colorpicker   (S&& setup, glint_colorpicker**          out = nullptr);
    template<typename S> auto gradientEditor(S&& setup, glint_gradient_editor** out = nullptr);
    template<typename S> auto dial           (S&& setup, glint_dial**                out = nullptr);
    template<typename S> glint_element* div      (S&& setup, glint_element** out = nullptr);
    template<typename S> glint_element* component(S&& setup, glint_element** out = nullptr); // backward-compat → div()
    template<typename T> auto attach   (T* ctrl, int tag = kNoTag);
    template<typename F> auto make     (float w, float h, F factory, int tag = kNoTag);
    glint_element*  spacer();  // defined in glint_builder.hpp — flex-grow:1 invisible node
  } add{this};

  /** Tick transitions on this component and all descendants.
   *  Called by glint_document before Layout() so animated width/height values
   *  are available to childPrefH/W during the layout pass.
   *
   *  Virtual hook for composite components that lazily sync child `style`
   *  from public fields (e.g. glint_checkbox::_syncFromProps): override this
   *  to do the sync BEFORE base recursion, so descendants snapshot
   *  computedStyle from the freshly-synced style this frame. Without that
   *  hook the sync would have to wait until Layout(), by which point the
   *  parent has already queried childPrefH/W with stale computedStyle. */
  virtual void tickTransitionsAll()
  {
    tickTransitions();
    mSkipNextTick_ = true;
    for (auto& ch : mChildren)
      ch->tickTransitionsAll();
  }

  /** Force-refresh computedStyle for this element from the current `style` and
   *  cssStyle_ — bypassing the once-per-frame skip flag set by the root
   *  tickTransitionsAll() pre-pass. Use this when a component mutates a child's
   *  `style` lazily during its own Layout()/drawContent() override and needs
   *  the change to be visible *this* frame (otherwise tickTransitions() consumes
   *  the skip flag and the child renders with stale computedStyle for one frame).
   *  Recurses into descendants. */
protected:
  static std::string _describeRenderNode(const glint_element* node)
  {
    if (!node) return "unknown";

    const auto describeLocal = [](const glint_element* current) {
      std::ostringstream oss;
      const char* typeName = current->typeName();
      oss << ((typeName && typeName[0] != '\0') ? typeName : "div");

      if (!current->id.empty())
        oss << '#' << current->id;

      if (!current->className.empty())
      {
        std::istringstream classes(current->className);
        std::string firstClass;
        if (classes >> firstClass)
          oss << '.' << firstClass;
      }

      return oss.str();
    };

    std::ostringstream oss;
    oss << describeLocal(node);

    std::vector<std::string> contextLabels;
    contextLabels.reserve(2);
    for (const glint_element* ancestor = node->mParent;
         ancestor && contextLabels.size() < 2;
         ancestor = ancestor->mParent)
    {
      if (ancestor->id.empty() && ancestor->className.empty())
        continue;
      contextLabels.push_back(describeLocal(ancestor));
    }

    if (!contextLabels.empty())
    {
      oss << '@' << contextLabels[0];
      if (contextLabels.size() > 1)
        oss << "<-" << contextLabels[1];
    }

    return oss.str();
  }

  enum class render_timing_bucket
  {
    transform_direct,
    transform_offscreen,
    filter_in_place,
    filter_offscreen,
    backdrop,
    self_paint,
    content,
    children,
    mask
  };

  struct render_timing_scope
  {
    render_timing_bucket bucket;
    std::chrono::steady_clock::time_point start;

    explicit render_timing_scope(render_timing_bucket timingBucket)
      : bucket(timingBucket), start(std::chrono::steady_clock::now())
    {
    }

    ~render_timing_scope()
    {
      const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
      _recordRenderTiming(bucket, elapsedMs);
    }
  };

  static void _recordRenderTiming(render_timing_bucket bucket, double elapsedMs)
  {
    switch (bucket)
    {
      case render_timing_bucket::transform_direct:
        sRenderTimingProfile_.transformDirectMs += elapsedMs;
        break;
      case render_timing_bucket::transform_offscreen:
        sRenderTimingProfile_.transformOffscreenMs += elapsedMs;
        break;
      case render_timing_bucket::filter_in_place:
        sRenderTimingProfile_.filterInPlaceMs += elapsedMs;
        break;
      case render_timing_bucket::filter_offscreen:
        sRenderTimingProfile_.filterOffscreenMs += elapsedMs;
        break;
      case render_timing_bucket::backdrop:
        sRenderTimingProfile_.backdropMs += elapsedMs;
        break;
      case render_timing_bucket::self_paint:
        sRenderTimingProfile_.selfPaintMs += elapsedMs;
        break;
      case render_timing_bucket::content:
        sRenderTimingProfile_.contentMs += elapsedMs;
        break;
      case render_timing_bucket::children:
        sRenderTimingProfile_.childrenMs += elapsedMs;
        break;
      case render_timing_bucket::mask:
        sRenderTimingProfile_.maskMs += elapsedMs;
        break;
    }
  }

  static void _recordChildSubtreeTiming(const glint_element* node, double elapsedMs)
  {
    if (!node || elapsedMs <= 0.0) return;
    sRenderTimingProfile_.childSubtreeMs[_describeRenderNode(node)] += elapsedMs;
  }

  #include "element/glint_element_style.hpp"
  #include "element/glint_element_layout.hpp"
  #include "element/glint_element_render.hpp"
  // Called after addChild() and notifyStyleChanged() respectively.
  // Implemented as out-of-line inlines at the bottom of glint_document.hpp so
  // they can dereference mRoot without needing the full definition here.
  void callRootTreeChanged();
  void callRootStyleChanged(uint64_t id);

  // Returns a pointer to mRoot->onRequest (or nullptr when mRoot is null).
  // Defined at the bottom of glint_document.hpp after the full document definition.
  // Used by load helpers so they receive the callback without a circular include.
  const std::function<void(glint_resource_request&)>* _getOnRequest() const;

  // Returns a pointer to mRoot->networkLog (or nullptr when mRoot is null).
  // Defined at the bottom of glint_document.hpp after the full document definition.
  glint_network_log* _getNetworkLog() const;

  // Find a DOM element by its string `id` (element.id).  Walks up to mRoot
  // and does a DFS search.  Returns nullptr when not found or mRoot is null.
  // Implemented at the bottom of glint_document.hpp (after the full document
  // definition) following the same pattern as callRootTreeChanged.
  glint_element* findMaskSourceElement(const std::string& strId) const;

  // Returns true when the owning document's current focus arrived via Tab/Shift+Tab
  // keyboard navigation (:focus-visible).  Defined at the bottom of glint_document.hpp
  // after the full document definition.
  bool _isFocusViaKeyboard() const;

  // Sets `mRoot->mLayoutDirty = true`. Defined at the bottom of glint_document.hpp
  // after the full document definition (we only have a forward-decl here).
  void _markRootLayoutDirty();

  // Returns mRoot->devicePixelRatio (or 1.f when mRoot is null).
  // Defined at the bottom of glint_document.hpp after the full document definition.
  float _getRootDpr() const;

  // ── Internal: createElement registry ─────────────────────────────────────
  static std::map<std::string, std::function<glint_element*()>>& _elementFactories()
  {
    static std::map<std::string, std::function<glint_element*()>> s;
    return s;
  }

  static inline thread_local glint_render_timing_profile sRenderTimingProfile_;

  // ── Text-selection private state ──────────────────────────────────────────
  // Active when innerText is non-empty and style.userSelect != "none".
  int mSelStart   = -1;   // byte index, -1 = no selection
  int mSelEnd     = -1;
  int mDragAnchor = -1;
  int mCursorPos  = 0;

  enum class TxtDragMode { CHAR, WORD, ALL };
  TxtDragMode mTxtDragMode        = TxtDragMode::CHAR;
  int         mTxtClickCount      = 0;
  float       mTxtLastClickX      = -9999.f;
  std::chrono::steady_clock::time_point mTxtLastClickTime{};
  int         mTxtWordAnchorStart = 0;
  int         mTxtWordAnchorEnd   = 0;

  mutable std::vector<_TxtRenderLine> mInlineTextRenderLines;

  // Cached inputs that produced `mInlineTextRenderLines`. _buildRenderLines()
  // reuses the cache when these all match the current style/text/content rect,
  // avoiding per-frame reshape on static text. Mutable because _buildRenderLines
  // is logically const but populates the cache.
  mutable bool             mTxtCacheValid       = false;
  mutable std::string      mTxtCacheText;
  mutable std::string      mTxtCacheFontFamily;
  mutable std::string      mTxtCacheFontStyle;
  mutable float            mTxtCacheWidth       = 0.f;
  mutable float            mTxtCacheLeft        = 0.f;
  mutable float            mTxtCacheTop         = 0.f;
  mutable float            mTxtCacheFontSize    = 0.f;
  mutable float            mTxtCacheFontWeight  = 0.f;
  mutable float            mTxtCacheLineHeight  = 0.f;
  mutable glint_text_align mTxtCacheTextAlign   = EAlign::Near;

  // Memoized intrinsic-size results (preferredW / preferredH). Both are called
  // multiple times per Layout() pass per child for flex/block/table layouts;
  // their inputs change far less often than they're queried.
  mutable bool        mPrefWValid      = false;
  mutable float       mPrefWValue      = 0.f;
  mutable std::string mPrefWText;
  mutable std::string mPrefWFontFamily;
  mutable std::string mPrefWFontStyle;
  mutable float       mPrefWFontSize   = 0.f;
  mutable float       mPrefWFontWeight = 0.f;

  mutable bool        mPrefHValid      = false;
  mutable float       mPrefHValue      = 0.f;
  mutable float       mPrefHAvailW     = 0.f;
  mutable std::string mPrefHText;
  mutable std::string mPrefHFontFamily;
  mutable std::string mPrefHFontStyle;
  mutable float       mPrefHFontSize   = 0.f;
  mutable float       mPrefHFontWeight = 0.f;
  mutable float       mPrefHLineHeight = 0.f;

  // ── Mask cache ───────────────────────────────────────────────────────────
  // Cached output of glint_parse_mask_layers(computedStyle) and the per-layer
  // gradient SkShader built by glint_mask_gradient_shader(layer, originBox).
  // _drawToCanvasImpl walks every mask-bearing element each frame; without
  // this cache the masks page rebuilds ~25 SkShaders + parses ~25 mask
  // strings per frame, a multi-millisecond cost in Debug and a known cause
  // of GPU-program-cache thrash on the first paint.
  //
  // Invalidation: any change to one of the mask-* style strings OR to the
  // origin box geometry (L/T/W/H) forces a rebuild. style.* writes go
  // through computedStyle each frame, so we can compare cheaply.
  mutable bool        mMaskCacheValid       = false;
  mutable std::string mMaskCacheMask;
  mutable std::string mMaskCacheMode;
  mutable std::string mMaskCachePosition;
  mutable std::string mMaskCacheSize;
  mutable std::string mMaskCacheRepeat;
  mutable std::string mMaskCacheOrigin;
  mutable std::string mMaskCacheClip;
  mutable std::string mMaskCacheComposite;
  mutable float       mMaskCacheBoxL = 0.f;
  mutable float       mMaskCacheBoxT = 0.f;
  mutable float       mMaskCacheBoxR = 0.f;
  mutable float       mMaskCacheBoxB = 0.f;
  mutable std::vector<glint_mask_layer> mMaskCacheLayers;
  mutable std::vector<sk_sp<SkShader>>  mMaskCacheShaders;  // parallel; nullptr for non-GRADIENT layers

  // ── Background-image shader cache ─────────────────────────────────────────
  // _drawBackgroundSkia rebuilds an SkShader from the loaded SkImage on every
  // paint via glint_mask_image_shader(). Caching the resulting shader lets the
  // GPU program cache hit on subsequent paints (same root cause as the mask
  // cache fix — first-switch stalls go away when shader instances are stable).
  // Invalidates on backgroundImage path / size / position / repeat / opacity
  // change OR when the rect geometry (L/T/W/H) changes.
  mutable bool        mBgImgCacheValid    = false;
  mutable std::string mBgImgCachePath;
  mutable std::string mBgImgCacheSize;
  mutable std::string mBgImgCachePosition;
  mutable std::string mBgImgCacheRepeat;
  mutable float       mBgImgCacheRectL    = 0.f;
  mutable float       mBgImgCacheRectT    = 0.f;
  mutable float       mBgImgCacheRectR    = 0.f;
  mutable float       mBgImgCacheRectB    = 0.f;
  mutable sk_sp<SkImage>  mBgImgCacheImg;
  mutable sk_sp<SkShader> mBgImgCacheShader;

  // ── Border dash-path-effect cache ────────────────────────────────────────
  // SkDashPathEffect::Make is a non-trivial allocation; caching by exact dash
  // string + offset prevents per-frame rebuilds on dashed borders.
  mutable bool        mDashCacheValid     = false;
  mutable std::string mDashCacheStr;
  mutable float       mDashCacheOffset    = 0.f;
  mutable sk_sp<SkPathEffect> mDashCacheEffect;

  // Returns the byte index into innerText nearest to canvas-space (lx, ly).
  int _txtHitTestPos(float lx, float ly) const
  {
    if (innerText.empty()) return 0;
    const float sz   = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 12.f;
    SkFont      font = skFont(sz,
                               computedStyle.fontFamily.c_str(),
                               computedStyle.fontWeight,
                               computedStyle.fontStyle.c_str());
    std::vector<_TxtRenderLine> lines = _buildRenderLines(font);
    if (lines.empty()) return 0;

    int li = 0;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
    {
      const auto& line = lines[static_cast<std::size_t>(i)];
      if (ly < line.lineBoxBottom || i == static_cast<int>(lines.size()) - 1)
      {
        li = i;
        break;
      }
    }
    const _TxtRenderLine& ln = lines[static_cast<std::size_t>(li)];
    const int n = static_cast<int>(ln.text.size());

    const float localX = lx - ln.x;
    if (localX <= 0.f) return ln.byteStart;
    for (int i = 0; i < n; )
    {
      int next = i + 1;
      while (next < n && (static_cast<unsigned char>(ln.text[next]) & 0xC0u) == 0x80u) ++next;
      SkRect b0, b1;
      const float adv0 = font.measureText(ln.text.c_str(), static_cast<std::size_t>(i),    SkTextEncoding::kUTF8, &b0);
      const float adv1 = font.measureText(ln.text.c_str(), static_cast<std::size_t>(next), SkTextEncoding::kUTF8, &b1);
      if (localX < (adv0 + adv1) * 0.5f) return ln.byteStart + i;
      i = next;
    }
    return ln.byteEnd;
  }

  // Draws selection highlight rects behind text.
  void _txtDrawSelectionHighlights(SkCanvas* canvas) const
  {
    if (mSelStart < 0 || mSelStart == mSelEnd || innerText.empty()) return;
    const int selLo = std::min(mSelStart, mSelEnd);
    const int selHi = std::max(mSelStart, mSelEnd);
    const float sz   = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 12.f;
    SkFont      font = skFont(sz,
                               computedStyle.fontFamily.c_str(),
                               computedStyle.fontWeight,
                               computedStyle.fontStyle.c_str());
    std::vector<_TxtRenderLine> lines = _buildRenderLines(font);
    if (lines.empty()) return;

    const glint_color& sc = computedStyle.selectionColor.value;
    SkPaint selPaint;
    selPaint.setColor(SkColorSetARGB(sc.A, sc.R, sc.G, sc.B));

    for (std::size_t li = 0; li < lines.size(); ++li)
    {
      const _TxtRenderLine& ln   = lines[li];
      const int       selS       = std::max(selLo, ln.byteStart);
      const int       selE       = std::min(selHi, ln.byteEnd);
      if (selS >= selE) continue;

      const int localS = std::min(selS - ln.byteStart, (int)ln.text.size());
      const int localE = std::min(selE - ln.byteStart, (int)ln.text.size());

      SkRect bS, bE;
      const float advS = font.measureText(ln.text.c_str(), static_cast<std::size_t>(localS), SkTextEncoding::kUTF8, &bS);
      const float advE = font.measureText(ln.text.c_str(), static_cast<std::size_t>(localE), SkTextEncoding::kUTF8, &bE);
      const float selTop = ln.lineBoxTop;
      const float selBottom = std::max(ln.lineBoxTop, ln.lineBoxBottom);
      canvas->drawRect(SkRect::MakeLTRB(ln.x + advS, selTop,
                                        ln.x + advE, selBottom), selPaint);
    }
  }

  // Right-click context menu (Win32 synchronous TrackPopupMenu).
  void _showContextMenu(float x, float y)
  {
#ifdef _WIN32
    const bool hasSelection = (mSelStart != -1 && mSelStart != mSelEnd);
    float wx = x, wy = y;
    for (glint_element* p = mParent; p; p = p->mParent) { wx -= p->mScrollLeft; wy -= p->mScrollTop; }
    HWND hwnd = mpG ? static_cast<HWND>(mpG->GetWindow()) : nullptr;
    if (!hwnd) return;

    auto sysStr = [](UINT id, const wchar_t* fb) -> std::wstring {
      wchar_t buf[256] = {};
      int n = ::LoadStringW(::GetModuleHandleW(L"user32.dll"), id, buf, 256);
      return n > 0 ? std::wstring(buf, static_cast<std::size_t>(n)) : fb;
    };
    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenuW(hMenu, MF_STRING | (hasSelection ? 0u : MF_GRAYED), 1, sysStr(31962, L"&Copy").c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hMenu, MF_STRING | MF_ENABLED, 3, sysStr(31965, L"Select &All").c_str());

    POINT pt = { static_cast<LONG>(wx), static_cast<LONG>(wy) };
    ::ClientToScreen(hwnd, &pt);
    const int result = static_cast<int>(::TrackPopupMenu(
      hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr));
    ::DestroyMenu(hMenu);
    if (result == 1 && hasSelection)
    {
      const int lo = std::min(mSelStart, mSelEnd);
      const int hi = std::max(mSelStart, mSelEnd);
      _txtCopyToClipboard(innerText.substr(static_cast<std::size_t>(lo),
                                           static_cast<std::size_t>(hi - lo)));
    }
    else if (result == 3)
    {
      mSelStart = 0; mSelEnd = static_cast<int>(innerText.size());
      mCursorPos = mSelEnd; mDragAnchor = 0;
      setDirty(false);
    }
#elif defined(__APPLE__)
    (void)x; (void)y;
    const bool hasSelection = (mSelStart != -1 && mSelStart != mSelEnd);
    const std::vector<std::pair<int, std::string>> menuItems = {
      {1, "Copy"}, {0, "-"}, {3, "Select All"}
    };
    const std::vector<int> grayed = hasSelection
      ? std::vector<int>{}
      : std::vector<int>{1};
    const int result = glint_platform::showContextMenu(0, 0, menuItems, grayed);
    if (result == 1 && hasSelection)
    {
      const int lo = std::min(mSelStart, mSelEnd);
      const int hi = std::max(mSelStart, mSelEnd);
      _txtCopyToClipboard(innerText.substr(static_cast<std::size_t>(lo),
                                           static_cast<std::size_t>(hi - lo)));
    }
    else if (result == 3)
    {
      mSelStart = 0; mSelEnd = static_cast<int>(innerText.size());
      mCursorPos = mSelEnd; mDragAnchor = 0;
      setDirty(false);
    }
#endif
  }

  static void _txtCopyToClipboard(const std::string& s)
  {
#ifdef _WIN32
    if (s.empty() || !OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (wlen > 0)
    {
      HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(WCHAR));
      if (hg)
      {
        WCHAR* p = static_cast<WCHAR*>(GlobalLock(hg));
        if (p) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, p, wlen); GlobalUnlock(hg); }
        SetClipboardData(CF_UNICODETEXT, hg);
      }
    }
    CloseClipboard();
#elif defined(__APPLE__)
    if (!s.empty()) glint_platform::setClipboardText(s);
#endif
  }

  // ── UTF-8 / word-navigation helpers ──────────────────────────────────────
  static bool _txtIsWordChar(unsigned char c)
  {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
  }
  static bool _txtIsSpaceChar(unsigned char c) { return c == ' ' || c == '\t'; }

  static int _txtNextCP(const std::string& s, int pos)
  {
    if (pos >= static_cast<int>(s.size())) return pos;
    const auto c = static_cast<unsigned char>(s[pos]);
    if      (c < 0x80) return pos + 1;
    else if (c < 0xE0) return pos + 2;
    else if (c < 0xF0) return pos + 3;
    else               return pos + 4;
  }
  static int _txtPrevCP(const std::string& s, int pos)
  {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return p;
  }

  static std::pair<int,int> _txtWordBoundary(const std::string& t, int bytePos)
  {
    if (t.empty()) return {0, 0};
    bytePos = std::max(0, std::min(bytePos, static_cast<int>(t.size())));
    int cp = bytePos;
    if (cp >= static_cast<int>(t.size())) cp = _txtPrevCP(t, static_cast<int>(t.size()));
    const unsigned char c = static_cast<unsigned char>(t[cp]);
    if (!_txtIsWordChar(c) && !_txtIsSpaceChar(c)) return { cp, _txtNextCP(t, cp) };
    int start = cp;
    while (start > 0)
    {
      const int  prev = _txtPrevCP(t, start);
      const auto pc   = static_cast<unsigned char>(t[prev]);
      if (!(_txtIsWordChar(c) ? _txtIsWordChar(pc) : _txtIsSpaceChar(pc))) break;
      start = prev;
    }
    int end = _txtNextCP(t, cp);
    while (end < static_cast<int>(t.size()))
    {
      const auto ec = static_cast<unsigned char>(t[end]);
      if (!(_txtIsWordChar(c) ? _txtIsWordChar(ec) : _txtIsSpaceChar(ec))) break;
      end = _txtNextCP(t, end);
    }
    return { start, end };
  }

  int _txtWordLeft(int pos) const
  {
    if (pos <= 0) return 0;
    pos = _txtPrevCP(innerText, pos);
    while (pos > 0 && _txtIsSpaceChar(static_cast<unsigned char>(innerText[pos])))
      pos = _txtPrevCP(innerText, pos);
    if (pos > 0 && _txtIsWordChar(static_cast<unsigned char>(innerText[pos])))
      while (pos > 0 && _txtIsWordChar(static_cast<unsigned char>(
               innerText[_txtPrevCP(innerText, pos)])))
        pos = _txtPrevCP(innerText, pos);
    return pos;
  }

  int _txtWordRight(int pos) const
  {
    const int n = static_cast<int>(innerText.size());
    if (pos >= n) return n;
    while (pos < n && _txtIsSpaceChar(static_cast<unsigned char>(innerText[pos])))
      pos = _txtNextCP(innerText, pos);
    if (pos >= n) return n;
    if (_txtIsWordChar(static_cast<unsigned char>(innerText[pos])))
      while (pos < n && _txtIsWordChar(static_cast<unsigned char>(innerText[pos])))
        pos = _txtNextCP(innerText, pos);
    else
      pos = _txtNextCP(innerText, pos);
    return pos;
  }
};

// ── Planned implementation split sub-headers (currently stubs) ──────────────
// When glint_element is fully split, these will own the method bodies.
// See web-refactor.md step 3 for the migration plan.
#include "element/glint_element_tree.hpp"    // appendChild, removeChild, parentElement, tree linkage
#include "element/glint_element_events.hpp"  // addEventListener, removeEventListener, dispatchEvent
#include "element/glint_element_style.hpp"   // style, computedStyle, id, tickTransitions
#include "element/glint_element_layout.hpp"  // Layout, HitTest, scroll, _ensureScrollbars
#include "element/glint_element_render.hpp"  // Draw, DrawBackground, DrawToCanvas, shaders

// ── Backward-compat alias ─────────────────────────────────────────────────────
// glint_element is the primary name; sk_ui_component remains valid in existing code.

