#pragma once

/**
 * glint_window.hpp
 * Platform-dispatching umbrella for glint_window.
 *
 * Includes the correct platform-specific implementation and exposes
 * `glint_window` as an alias for it.  Client code includes only this header.
 *
 * Subclass `glint_window` and implement the required virtual methods:
 *   virtual const wchar_t* windowClassName() const = 0;
 *   virtual const wchar_t* windowTitle()     const = 0;
 *   virtual void           buildUI()               = 0;
 *
 * Optional overrides: defaultWidth/Height, clearColor, bgColor (Win32),
 *   onCreated, onThreadStarted, onThreadEnded, onDestroyed, afterRun,
 *   handleMessage (Win32 extension point for custom WM_ messages).
 *
 * Platform status:
 *   Win32   — implemented (glint_window_win32.hpp)
 *   macOS   — not yet implemented
 *   Linux   — not yet implemented
 */

#include "platform/glint_window.hpp"
