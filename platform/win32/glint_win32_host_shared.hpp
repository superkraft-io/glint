#pragma once

#include "../../glint_core.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <chrono>
#include <vector>

namespace glint_win32_host
{
inline void invalidateWindow(HWND hwnd)
{
  if (hwnd)
    ::InvalidateRect(hwnd, nullptr, FALSE);
}

inline void scheduleRedraw(HWND hwnd, bool useTransparency = false, UINT redrawMessage = 0)
{
  if (!hwnd)
    return;

  if (useTransparency && redrawMessage != 0)
    ::PostMessage(hwnd, redrawMessage, 0, 0);
  else
    invalidateWindow(hwnd);
}

inline glint_mouse_mod modifierKeysFromWParam(WPARAM wp)
{
  glint_mouse_mod modifiers = {};
  modifiers.S = (wp & MK_SHIFT) != 0;
  modifiers.C = (wp & MK_CONTROL) != 0;
  return modifiers;
}

inline glint_mouse_mod mouseButtonsFromWParam(WPARAM wp)
{
  glint_mouse_mod modifiers = modifierKeysFromWParam(wp);
  modifiers.L = (wp & MK_LBUTTON) != 0;
  modifiers.R = (wp & MK_RBUTTON) != 0;
  return modifiers;
}

inline void routeLeftButtonDown(glint_document* document, float& prevX, float& prevY, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(GET_X_LPARAM(lp)) * invScale;
  const float y = static_cast<float>(GET_Y_LPARAM(lp)) * invScale;
  prevX = x;
  prevY = y;

  glint_mouse_mod modifiers = modifierKeysFromWParam(wp);
  modifiers.L = true;
  if (document)
    document->OnMouseDown(x, y, modifiers);
}

inline void routeLeftButtonUp(glint_document* document, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(GET_X_LPARAM(lp)) * invScale;
  const float y = static_cast<float>(GET_Y_LPARAM(lp)) * invScale;

  if (document)
    document->OnMouseUp(x, y, modifierKeysFromWParam(wp));
}

inline void routeRightButtonDown(glint_document* document, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(GET_X_LPARAM(lp)) * invScale;
  const float y = static_cast<float>(GET_Y_LPARAM(lp)) * invScale;

  glint_mouse_mod modifiers = modifierKeysFromWParam(wp);
  modifiers.R = true;
  if (document)
    document->OnMouseDown(x, y, modifiers);
}

inline void routeRightButtonUp(glint_document* document, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(GET_X_LPARAM(lp)) * invScale;
  const float y = static_cast<float>(GET_Y_LPARAM(lp)) * invScale;

  if (document)
    document->OnMouseUp(x, y, modifierKeysFromWParam(wp));
}

inline void routeMouseMove(HWND hwnd, glint_document* document, float& prevX, float& prevY, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(GET_X_LPARAM(lp)) * invScale;
  const float y = static_cast<float>(GET_Y_LPARAM(lp)) * invScale;
  const float deltaX = x - prevX;
  const float deltaY = y - prevY;
  prevX = x;
  prevY = y;

  if (document)
  {
    const glint_mouse_mod modifiers = mouseButtonsFromWParam(wp);
    if (modifiers.L)
      document->OnMouseDrag(x, y, deltaX, deltaY, modifiers);
    else
      document->OnMouseOver(x, y, modifiers, deltaX, deltaY);
  }

  if (hwnd)
  {
    TRACKMOUSEEVENT trackMouseEvent = { sizeof(trackMouseEvent), TME_LEAVE, hwnd, 0 };
    ::TrackMouseEvent(&trackMouseEvent);
  }
}

inline void routeMouseLeave(glint_document* document)
{
  if (document)
    document->OnMouseOut();
}

inline void routeMouseWheel(HWND hwnd, glint_document* document, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  if (!document)
    return;

  POINT point = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
  if (hwnd)
    ::ScreenToClient(hwnd, &point);

  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(point.x) * invScale;
  const float y = static_cast<float>(point.y) * invScale;
  const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 40.f;
  document->OnMouseWheel(x, y, 0.f, -delta, modifierKeysFromWParam(wp));
}

inline void routeChar(glint_document* document, WPARAM wp)
{
  if (!document || wp < 0x20)
    return;

  wchar_t wideChar = static_cast<wchar_t>(wp);
  char utf8[5] = {};
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, &wideChar, 1, utf8, 4, nullptr, nullptr);
  if (length <= 0)
    return;

  glint_key_press keyPress = {};
  keyPress.utf8[0] = utf8[0];
  keyPress.utf8[1] = utf8[1];
  keyPress.utf8[2] = utf8[2];
  keyPress.utf8[3] = utf8[3];
  document->OnKeyDown(keyPress);
}

inline glint_key_press virtualKeyPress(WPARAM wp)
{
  glint_key_press keyPress = {};
  keyPress.vk = static_cast<int>(wp);
  keyPress.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  keyPress.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  keyPress.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
  return keyPress;
}

inline bool shouldScheduleTimerRedraw(glint_document* document, bool redrawRequested)
{
  if (redrawRequested)
    return true;

  glint_element* focused = document ? document->getFocusedNode() : nullptr;
  if (!focused || !focused->wantsPeriodicRedraw())
    return false;

  return std::chrono::steady_clock::now() >= focused->nextPeriodicRedrawTime();
}

// ── Cursor support ────────────────────────────────────────────────────────

// Returns the HCURSOR corresponding to a CSS cursor keyword.
// Maps the full Chrome cursor vocabulary to the closest Win32 system cursor.
inline HCURSOR cssToHCursor(const std::string& css)
{
  // "none" — 1x1 fully transparent cursor (AND=0xFF XOR=0x00 → invisible).
  if (css == "none")
  {
    static HCURSOR blank = []() -> HCURSOR {
      // AND plane 0xFF = transparent, XOR plane 0x00 = no colour.
      // Win32 cursor planes must be 32-bit aligned; 4 bytes covers 1x32px.
      static const BYTE andPlane[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
      static const BYTE xorPlane[4] = { 0x00, 0x00, 0x00, 0x00 };
      return ::CreateCursor(::GetModuleHandleW(nullptr), 0, 0, 1, 1,
                            andPlane, xorPlane);
    }();
    return blank;
  }

  // Use integer OCR_ IDs so the code compiles in both UNICODE and ANSI builds.
  // Reference: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadcursorw
  static constexpr WORD OCR_NORMAL     = 32512;  // IDC_ARROW
  static constexpr WORD OCR_IBEAM      = 32513;  // IDC_IBEAM
  static constexpr WORD OCR_WAIT       = 32514;  // IDC_WAIT
  static constexpr WORD OCR_CROSS      = 32515;  // IDC_CROSS
  static constexpr WORD OCR_SIZENWSE   = 32642;  // IDC_SIZENWSE
  static constexpr WORD OCR_SIZENESW   = 32643;  // IDC_SIZENESW
  static constexpr WORD OCR_SIZEWE     = 32644;  // IDC_SIZEWE
  static constexpr WORD OCR_SIZENS     = 32645;  // IDC_SIZENS
  static constexpr WORD OCR_SIZEALL    = 32646;  // IDC_SIZEALL
  static constexpr WORD OCR_NO         = 32648;  // IDC_NO
  static constexpr WORD OCR_HAND       = 32649;  // IDC_HAND
  static constexpr WORD OCR_APPSTART   = 32650;  // IDC_APPSTARTING
  static constexpr WORD OCR_HELP       = 32651;  // IDC_HELP

  WORD id = OCR_NORMAL;

  if      (css == "pointer")                                          id = OCR_HAND;
  else if (css == "text" || css == "vertical-text")                  id = OCR_IBEAM;
  else if (css == "crosshair" || css == "cell")                      id = OCR_CROSS;
  else if (css == "move" || css == "all-scroll" ||
           css == "grab" || css == "grabbing")                       id = OCR_SIZEALL;
  else if (css == "not-allowed" || css == "no-drop")                 id = OCR_NO;
  else if (css == "wait")                                            id = OCR_WAIT;
  else if (css == "progress")                                        id = OCR_APPSTART;
  else if (css == "help")                                            id = OCR_HELP;
  else if (css == "n-resize"   || css == "s-resize"  ||
           css == "ns-resize"  || css == "row-resize")               id = OCR_SIZENS;
  else if (css == "e-resize"   || css == "w-resize"  ||
           css == "ew-resize"  || css == "col-resize")               id = OCR_SIZEWE;
  else if (css == "ne-resize"  || css == "sw-resize" ||
           css == "nesw-resize")                                      id = OCR_SIZENESW;
  else if (css == "nw-resize"  || css == "se-resize" ||
           css == "nwse-resize")                                      id = OCR_SIZENWSE;
  // default / auto / alias / copy / zoom-in / zoom-out / context-menu → OCR_NORMAL

  return ::LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
}

// Call from WM_SETCURSOR when LOWORD(lp) == HTCLIENT.
// Queries the document for the cursor under the last known mouse position,
// sets it, and returns TRUE so DefWindowProc does not override it.
inline LRESULT routeSetCursor(glint_document* document, float mouseX, float mouseY)
{
  const std::string css = document
    ? document->getCursorAtPoint(mouseX, mouseY)
    : "default";
  HCURSOR hc = cssToHCursor(css);
  if (hc)
    ::SetCursor(hc);
  return TRUE;
}

} // namespace glint_win32_host