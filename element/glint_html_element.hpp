#pragma once

/**
 * glint_html_element.hpp
 * DOM-compatible event listener registry for glint components.
 *
 * Every glint_element exposes a public `element` member of this type.
 * The API mirrors the DOM EventTarget interface 1:1 in naming and semantics:
 *   https://developer.mozilla.org/en-US/docs/Web/API/EventTarget
 *
 * Usage (identical to JS):
 *
 *   // Register a persistent listener:
 *   myComp->element.addEventListener("click", [](glint_event& e) {
 *       auto& me = static_cast<glint_mouse_event&>(e);
 *       // handle click
 *   });
 *
 *   // Register a one-shot listener (auto-removed after first fire):
 *   myComp->element.addEventListener("mousedown", handler, { .once = true });
 *
 *   // Remove a listener by ID (C++ divergence from DOM — lambdas have no
 *   // identity in C++ so we use the int ID returned by addEventListener):
 *   int id = myComp->element.addEventListener("mousemove", handler);
 *   myComp->element.removeEventListener(id);
 *
 * Supported event types (fired by glint_document):
 *   "mousedown"   — button pressed (bubbles)
 *   "mouseup"     — button released (bubbles)
 *   "click"       — mousedown + mouseup on same element (bubbles)
 *   "mousemove"   — pointer moved over the current hover/drag target (bubbles)
 *   "mouseover"   — pointer entered node or descendant (bubbles)
 *   "mouseout"    — pointer left node or descendant (bubbles)
 *   "mouseenter"  — pointer entered this exact node (does NOT bubble)
 *   "mouseleave"  — pointer left this exact node (does NOT bubble)
 */

#include "../events/glint_event.hpp"
#include "../glint_style.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using glint_event_listener = std::function<void(glint_event&)>;

// ── glint_event_listener_options ─────────────────────────────────────────────
// Mirrors the DOM AddEventListenerOptions dictionary.

struct glint_event_listener_options
{
    bool once    = false;   // auto-remove this listener after it fires once
    bool capture = false;   // reserved — capture phase not yet implemented
};

// ── glint_html_element ────────────────────────────────────────────────────────

class glint_html_element
{
public:

    // ── Style ─────────────────────────────────────────────────────────────────
    // CSS-inspired styling, accessible as both element.style and via the
    // glint_element::style reference alias that points here.
    glint_style style;

    // ── addEventListener ───────────────────────────────────────────────────────

    /**
     * Register an event listener for the given type.
     * Returns an int listener ID — pass it to removeEventListener to unregister.
     *
     * Mirrors: element.addEventListener(type, listener, options)
     *
     * @param type     DOM event type string, e.g. "click", "mousedown".
     * @param listener Callback receiving glint_event& (cast to glint_mouse_event&
     *                 for mouse events).
     * @param options  { .once = true } auto-removes after first fire.
     */
    int addEventListener(const std::string&            type,
                         glint_event_listener           listener,
                         glint_event_listener_options   options = {})
    {
        const int id = mNextId++;
        mListeners.push_back({ id, type, std::move(listener), options.once });
        return id;
    }

    /**
     * Boolean useCapture overload — mirrors the older DOM addEventListener signature:
     *   element.addEventListener(type, listener, useCapture)
     */
    int addEventListener(const std::string&   type,
                         glint_event_listener  listener,
                         bool                  useCapture)
    {
        return addEventListener(type, std::move(listener),
                                glint_event_listener_options{ false, useCapture });
    }

    // ── removeEventListener ───────────────────────────────────────────────────

    /**
     * Remove a previously registered listener by ID.
     *
     * @param id  The value returned by addEventListener.
     */
    void removeEventListener(int id)
    {
        mListeners.erase(
            std::remove_if(mListeners.begin(), mListeners.end(),
                           [id](const Entry& e) { return e.id == id; }),
            mListeners.end());
    }

    // ── Internal ──────────────────────────────────────────────────────────────

    /**
     * Invoke all listeners registered for e.type on this element.
     * Handles `once` auto-removal and stopImmediatePropagation.
     * Called once per node in the bubble chain by glint_element::dispatchDOMEvent.
     */
    void _dispatchToListeners(glint_event& e)
    {
        // ── Snapshot the listener list before iteration ───────────────────────
        // A callback (entry.fn) may trigger a UI rebuild that destroys this
        // element, freeing mListeners.  Iterating the member vector after fn()
        // returns is therefore UB (classic use-after-free / dangling iterator).
        // We iterate a value-copy instead; after fn() we only touch local vars.
        //
        // "once" removal: the DOM spec says the listener is removed BEFORE it
        // fires.  Removing from the live mListeners BEFORE calling fn() means we
        // never access mListeners after fn(), regardless of whether fn() destroys
        // this element.
        const std::vector<Entry> snap = mListeners;

        for (const Entry& entry : snap)
        {
            if (entry.type != e.type) continue;

            // Remove once-listeners from the live list before invoking fn so
            // that even if fn destroys this element we have not left a stale entry.
            if (entry.once) removeEventListener(entry.id);

            entry.fn(e);

            // After fn() this element may be destroyed — do NOT access any member.
            // `e` lives on the caller's stack, `entry` is from our local snap: both safe.
            if (e._stopImmediatePropagation) return;
        }
    }

    // ── Scroll DOM properties ─────────────────────────────────────────────────
    // Mirrors the DOM Element scroll interface (read/write with side effects).
    //
    //   element.scrollTop  = 200.f;          // reactive: clamps, fires "scroll", redraws
    //   float pos = element.scrollTop;       // getter
    //   float h   = element.scrollHeight;    // read-only: total content height
    //
    // These are wired up by glint_element::_initScrollElement() during subtree finalization.

    struct sk_scroll_prop
    {
        std::function<float()>     _getter;
        std::function<void(float)> _setter;

        operator float() const { return _getter ? _getter() : 0.f; }  // NOLINT
        sk_scroll_prop& operator=(float v)  { if (_setter) _setter(v); return *this; }
        sk_scroll_prop& operator=(double v) { return operator=(static_cast<float>(v)); }
        sk_scroll_prop& operator=(int v)    { return operator=(static_cast<float>(v)); }
    };

    sk_scroll_prop scrollTop;    // element.scrollTop  (read/write)
    sk_scroll_prop scrollLeft;   // element.scrollLeft (read/write)
    float scrollWidth  = 0.f;   // element.scrollWidth  (read-only, set by Layout)
    float scrollHeight = 0.f;   // element.scrollHeight (read-only, set by Layout)

    // ── id ─────────────────────────────────────────────────────────────────────
    // Optional human-readable identifier for this element. Mirrors the DOM
    // `id` attribute. When set, the inspector tree displays it as
    // "typeName#id" instead of just "typeName".
    //
    //   myComp->element.id = "main-panel";
    std::string id;

    // ── scrollCornerBox ───────────────────────────────────────────────────────
    // The glint_element at the bottom-right intersection of two active
    // scrollbars. Null if only one axis scrolls. Exposed on element so
    // devs can style or add children to this corner area.
    class glint_element* scrollCornerBox = nullptr;

    // ── Internal bind helper ──────────────────────────────────────────────────
    // Called once from glint_element::_initScrollElement().
    void _bindScroll(std::function<float()>     getTop,
                     std::function<void(float)> setTop,
                     std::function<float()>     getLeft,
                     std::function<void(float)> setLeft)
    {
        scrollTop._getter  = std::move(getTop);
        scrollTop._setter  = std::move(setTop);
        scrollLeft._getter = std::move(getLeft);
        scrollLeft._setter = std::move(setLeft);
    }

private:
    struct Entry
    {
        int                  id;
        std::string          type;
        glint_event_listener fn;
        bool                 once;
    };

    int              mNextId = 1;
    std::vector<Entry> mListeners;
};
