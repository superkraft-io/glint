#pragma once

/**
 * glint_keyboard_event.hpp
 * DOM-compatible KeyboardEvent for glint.
 *
 * glint_keyboard_event mirrors the W3C KeyboardEvent interface:
 *   https://developer.mozilla.org/en-US/docs/Web/API/KeyboardEvent
 *
 * Dispatched by glint_document::OnKeyDown / OnKeyUp to the focused node.
 * Also used internally for "focus" / "blur" events (key field unused).
 *
 * Usage:
 *   component->element.addEventListener("keydown", [](glint_event& e) {
 *       auto& ke = static_cast<glint_keyboard_event&>(e);
 *       if (ke.key.vk == kVK_RETURN) { ... }
 *       ke.preventDefault(); // suppress default action
 *   });
 *
 * Event types fired by glint_document:
 *   "keydown"  — key pressed  (bubbles, target = focused node)
 *   "keyup"    — key released (bubbles, target = focused node)
 *   "focus"    — node gained input focus (does NOT bubble, DOM-compliant)
 *   "blur"     — node lost input focus   (does NOT bubble, DOM-compliant)
 */

#include "glint_event.hpp"    // glint_event base
#include "../glint_types.hpp"    // glint_key_press

// ── glint_keyboard_event ─────────────────────────────────────────────────────
// Extends glint_event with key-specific fields.
// For "focus" / "blur" events, `key` is default-initialised and unused.

class glint_keyboard_event : public glint_event
{
public:
    glint_key_press key;       // key that triggered this event (keydown/keyup only)
    bool            repeat = false;  // true if the key is being held (auto-repeat)
};

// ── glint_keyboard_event ─────────────────────────────────────────────────────────
// New API name for glint_keyboard_event.
