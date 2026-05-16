#pragma once

/**
 * glint_event.hpp
 * DOM-compatible Event and MouseEvent classes for glint.
 *
 * These mirror the W3C DOM interfaces 1:1 in naming and semantics:
 *   https://developer.mozilla.org/en-US/docs/Web/API/Event
 *   https://developer.mozilla.org/en-US/docs/Web/API/MouseEvent
 *
 * Usage (identical to JS):
 *   component->element.addEventListener("click", [](glint_event& e) {
 *       auto& me = static_cast<glint_mouse_event&>(e);
 *       // me.clientX, me.clientY, me.shiftKey, me.button, etc.
 *       me.preventDefault();         // suppress default behaviour
 *       me.stopPropagation();        // stop bubbling
 *   });
 */

#include <string>
#include <chrono>

// Forward declaration — glint_element is defined in glint_element.hpp.
class glint_element;

enum class glint_input_phase
{
    none = 0,
    may_begin,
    began,
    changed,
    ended,
    cancelled,
};

enum class glint_gesture_kind
{
    none = 0,
    pinch,
    rotate,
    swipe,
    smart_zoom,
};

// ── glint_event ───────────────────────────────────────────────────────────────
// Mirrors the DOM Event interface.

class glint_event
{
public:
    // ── Core fields ────────────────────────────────────────────────────────────

    std::string      type;
    glint_element* target        = nullptr;  // node the event was originally fired on
    glint_element* currentTarget = nullptr;  // node whose listener is currently running
    bool             bubbles       = false;
    bool             cancelable    = false;
    bool             defaultPrevented = false;
    double           timeStamp     = 0.0;      // ms since first Event construction

    // ── Propagation control ────────────────────────────────────────────────────

    /** Prevent the default action associated with this event (if cancelable). */
    void preventDefault()
    {
        if (cancelable) defaultPrevented = true;
    }

    /** Stop the event from bubbling further up the component tree. */
    void stopPropagation()
    {
        _stopPropagation = true;
    }

    /**
     * Stop bubbling AND prevent any further listeners on the current node
     * from receiving this event (mirrors DOM stopImmediatePropagation).
     */
    void stopImmediatePropagation()
    {
        _stopPropagation          = true;
        _stopImmediatePropagation = true;
    }

    // ── Internal propagation flags — read by dispatchDOMEvent / element ────────
    bool _stopPropagation          = false;
    bool _stopImmediatePropagation = false;
};

// ── glint_mouse_event ─────────────────────────────────────────────────────────
// Mirrors the DOM MouseEvent interface.
// Inherits all Event fields; adds mouse-specific position, modifier, and button data.

class glint_mouse_event : public glint_event
{
public:
    // ── Position ───────────────────────────────────────────────────────────────

    float clientX   = 0.f;   // x coordinate relative to the canvas origin
    float clientY   = 0.f;   // y coordinate relative to the canvas origin
    float movementX = 0.f;   // x delta since last mousemove event
    float movementY = 0.f;   // y delta since last mousemove event

    // ── Modifier keys ─────────────────────────────────────────────────────────

    bool shiftKey = false;
    bool ctrlKey  = false;
    bool altKey   = false;
    bool metaKey  = false;   // Cmd (macOS) / Windows key

    // ── Button state ──────────────────────────────────────────────────────────

    int button  = 0;   // button that changed: 0=left, 1=middle, 2=right
    int buttons = 0;   // bitmask of currently held buttons: 1=left, 2=right, 4=middle
};

// ── glint_wheel_event ─────────────────────────────────────────────────────────
// Mirrors the DOM WheelEvent interface.
// Dispatched by glint_document::OnMouseWheel; can be stopped via preventDefault()
// to suppress the default scroll behaviour.
//
//   component->element.addEventListener("wheel", [](glint_event& e) {
//       auto& we = static_cast<glint_wheel_event&>(e);
//       we.preventDefault();   // suppress scrolling
//       // we.deltaX, we.deltaY in pixels (deltaMode == 0)
//   });

class glint_wheel_event : public glint_mouse_event
{
public:
    float deltaX    = 0.f;   // horizontal scroll distance (pixels, positive = right)
    float deltaY    = 0.f;   // vertical scroll distance   (pixels, positive = down)
    float deltaZ    = 0.f;   // z-axis scroll (rarely used)
    int   deltaMode = 0;     // 0=pixel (DOM_DELTA_PIXEL), 1=line, 2=page
    bool  hasPreciseDeltas = false;
    bool  isMomentum       = false;
    glint_input_phase phase         = glint_input_phase::none;
    glint_input_phase momentumPhase = glint_input_phase::none;
};

// ── glint_gesture_event ──────────────────────────────────────────────────────
// DOM-like trackpad gesture payload for platform-native gestures such as
// pinch, rotate, swipe, and smart zoom.

class glint_gesture_event : public glint_mouse_event
{
public:
    glint_gesture_kind kind = glint_gesture_kind::none;
    glint_input_phase  phase = glint_input_phase::none;
    float deltaX        = 0.f;
    float deltaY        = 0.f;
    float magnification = 0.f;
    float scale         = 1.f;
    float rotation      = 0.f;
    bool  isInertial    = false;
    bool  hasPreciseDeltas = false;
};

// ── glint_* aliases ───────────────────────────────────────────────────────────────
// New API names — both refer to the same classes; use glint_* in new code.
using glint_event       = glint_event;
