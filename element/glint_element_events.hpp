#pragma once

/**
 * glint_element_events.hpp
 * Event system implementation for glint_element.
 *
 * Included by glint_element.hpp after the full class declaration.
 * This file will eventually own the out-of-line implementations of:
 *
 *   addEventListener(type, listener, options)   — delegates to element bus
 *   addEventListener(type, listener, useCapture) — overload
 *   removeEventListener(listenerId)             — removes by ID
 *   dispatchDOMEvent(event)                     — walks parent chain (bubbling)
 *
 * The underlying glint_html_element event bus remains on the `element` member;
 * these methods are forwarding wrappers providing the DOM direct-on-element API.
 *
 * STATUS: STUB — implementation currently in glint_element.hpp.
 *         Migration pending (see web-refactor.md step 3).
 */
