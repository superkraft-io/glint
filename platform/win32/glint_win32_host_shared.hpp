#pragma once

#include "../../glint_core.hpp"
#include "glint_win32_cursors_embedded.hpp"

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
  modifiers.L   = (wp & MK_LBUTTON) != 0;
  modifiers.R   = (wp & MK_RBUTTON) != 0;
  modifiers.Mid = (wp & MK_MBUTTON) != 0;
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

inline void routeMiddleButtonDown(glint_document* document, float& prevX, float& prevY, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(GET_X_LPARAM(lp)) * invScale;
  const float y = static_cast<float>(GET_Y_LPARAM(lp)) * invScale;
  prevX = x;
  prevY = y;

  glint_mouse_mod modifiers = modifierKeysFromWParam(wp);
  modifiers.Mid = true;
  if (document)
    document->OnMouseDown(x, y, modifiers);
}

inline void routeMiddleButtonUp(glint_document* document, WPARAM wp, LPARAM lp, float scale = 1.f)
{
  const float invScale = scale > 0.f ? 1.f / scale : 1.f;
  const float x = static_cast<float>(GET_X_LPARAM(lp)) * invScale;
  const float y = static_cast<float>(GET_Y_LPARAM(lp)) * invScale;

  glint_mouse_mod modifiers = modifierKeysFromWParam(wp);
  if (document)
    document->OnMouseUp(x, y, modifiers);
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
    if (modifiers.L || modifiers.Mid)
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
  const auto  mod   = modifierKeysFromWParam(wp);
  // Shift+wheel → horizontal scroll (common browser/app convention).
  if (mod.S) document->OnMouseWheel(x, y, -delta, 0.f, mod);
  else       document->OnMouseWheel(x, y, 0.f,   -delta, mod);
}

// WM_MOUSEHWHEEL — native horizontal tilt-wheel event.
// Positive delta = wheel tilted right = scroll right (positive deltaX).
inline void routeMouseWheelH(HWND hwnd, glint_document* document, WPARAM wp, LPARAM lp, float scale = 1.f)
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
  document->OnMouseWheel(x, y, delta, 0.f, modifierKeysFromWParam(wp));
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

// ── Custom cursor creation (matching Chrome's win_cursor_factory.cc) ──────
//
// Creates a 32×32 RGBA HCURSOR from a pixel-marking callback.
// DrawFn receives a `set(x,y)` callable that marks black pixels; a 1-pixel
// white halo is added automatically around every black pixel, matching the
// Windows system cursor style.
//
// Reference: https://source.chromium.org/chromium/chromium/src/+/main:ui/base/win/win_cursor_factory.cc

template<typename DrawFn>
static HCURSOR glint_make_cursor32(DrawFn draw, int hotX, int hotY)
{
  uint32_t bm[32] = {};   // bit(31-x) of bm[y] == pixel(x,y) is black
  draw([&](int x, int y) {
    if ((unsigned)x < 32u && (unsigned)y < 32u)
      bm[y] |= (1u << (31 - x));
  });

  uint32_t px[32 * 32] = {};
  // White 8-neighbour halo around every black pixel
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x) {
      if (!((bm[y] >> (31 - x)) & 1u)) continue;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (!dx && !dy) continue;
          int nx = x + dx, ny = y + dy;
          if ((unsigned)nx < 32u && (unsigned)ny < 32u
              && !((bm[ny] >> (31 - nx)) & 1u))
            px[ny * 32 + nx] = 0xFFFFFFFFu;
        }
    }
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x)
      if ((bm[y] >> (31 - x)) & 1u)
        px[y * 32 + x] = 0xFF000000u;

  // 32-bit top-down DIB — same colour masks used by Chrome's WinCursorFactory
  BITMAPV5HEADER bi = {};
  bi.bV5Size        = sizeof(bi);
  bi.bV5Width       = 32;
  bi.bV5Height      = -32;          // top-down
  bi.bV5Planes      = 1;
  bi.bV5BitCount    = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask     = 0x00FF0000u;
  bi.bV5GreenMask   = 0x0000FF00u;
  bi.bV5BlueMask    = 0x000000FFu;
  bi.bV5AlphaMask   = 0xFF000000u;

  HDC     hdc    = ::GetDC(nullptr);
  void*   bits   = nullptr;
  HBITMAP hColor = ::CreateDIBSection(
                       hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                       DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, hdc);
  if (!hColor) return nullptr;
  memcpy(bits, px, sizeof(px));

  HBITMAP hMask = ::CreateBitmap(32, 32, 1, 1, nullptr);
  ICONINFO ii   = {};
  ii.fIcon      = FALSE;
  ii.xHotspot   = static_cast<DWORD>(hotX);
  ii.yHotspot   = static_cast<DWORD>(hotY);
  ii.hbmMask    = hMask;
  ii.hbmColor   = hColor;

  HCURSOR hc = reinterpret_cast<HCURSOR>(::CreateIconIndirect(&ii));
  ::DeleteObject(hColor);
  ::DeleteObject(hMask);
  return hc;
}

// Circle outline — midpoint (Bresenham) algorithm
template<typename S>
static void glint_cur_ring(S s, int cx, int cy, int r)
{
  int x = r, y = 0, d = 1 - r;
  while (y <= x) {
    s(cx+x,cy+y); s(cx+y,cy+x); s(cx-y,cy+x); s(cx-x,cy+y);
    s(cx-x,cy-y); s(cx-y,cy-x); s(cx+y,cy-x); s(cx+x,cy-y);
    ++y;
    if (d <= 0) d += 2*y + 1;
    else { --x; d += 2*(y - x) + 1; }
  }
}

// Standard NW-pointing filled-triangle arrow, tip at (tx, ty), ~15 px tall
template<typename S>
static void glint_cur_arrow(S s, int tx, int ty)
{
  for (int r = 0; r <= 14; ++r)
    for (int c = 0; c <= r; ++c)
      s(tx + c, ty + r);
}

// ── Per-cursor cached factories (lazy-init, created once per process) ─────

// cursor: cell  →  thick plus / cell-selector  (Chrome: IDC_CELL from resource DLL)
inline HCURSOR glint_cursor_cell()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_cell, kCur_cell_size);
  return h;
}

// cursor: grab  →  loaded from embedded grab.cur
inline HCURSOR glint_cursor_grab()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_grab, kCur_grab_size);
  return h;
}

// cursor: grabbing  →  loaded from embedded grabbing.cur
inline HCURSOR glint_cursor_grabbing()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_grabbing, kCur_grabbing_size);
  return h;
}

// cursor: zoom-in  →  loaded from embedded zoom-in.cur
inline HCURSOR glint_cursor_zoom_in()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_zoom_in, kCur_zoom_in_size);
  return h;
}

// cursor: zoom-out  →  loaded from embedded zoom-out.cur
inline HCURSOR glint_cursor_zoom_out()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_zoom_out, kCur_zoom_out_size);
  return h;
}

// cursor: col-resize  →  loaded from embedded col-resize.cur
inline HCURSOR glint_cursor_col_resize()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_col_resize, kCur_col_resize_size);
  return h;
}

// cursor: row-resize  →  loaded from embedded row-resize.cur
inline HCURSOR glint_cursor_row_resize()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_row_resize, kCur_row_resize_size);
  return h;
}

// cursor: copy  →  loaded from embedded copy.cur
inline HCURSOR glint_cursor_copy()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_copy, kCur_copy_size);
  return h;
}

// cursor: alias  →  loaded from embedded alias.cur
inline HCURSOR glint_cursor_alias()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_alias, kCur_alias_size);
  return h;
}

// cursor: vertical-text  →  loaded from embedded vertical-text.cur
inline HCURSOR glint_cursor_vertical_text()
{
  static HCURSOR h = glint_load_cursor_from_memory(kCur_vertical_text, kCur_vertical_text_size);
  return h;
}

// ── Cursor support ────────────────────────────────────────────────────────

// Returns the HCURSOR corresponding to a CSS cursor keyword.
// Maps Chrome's cursor vocabulary to Win32 system cursors (standard types)
// and programmatically-drawn cursors (custom types), matching Chrome's
// GetCursorId() switch in win_cursor_factory.cc:
//   https://source.chromium.org/chromium/chromium/src/+/main:ui/base/win/win_cursor_factory.cc
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

  // Custom cursors — Chrome loads these from its resource DLL; we create them
  // programmatically so no external resource file is needed.
  if (css == "cell")          return glint_cursor_cell();
  if (css == "grab")          return glint_cursor_grab();
  if (css == "grabbing")      return glint_cursor_grabbing();
  if (css == "zoom-in")       return glint_cursor_zoom_in();
  if (css == "zoom-out")      return glint_cursor_zoom_out();
  if (css == "col-resize")    return glint_cursor_col_resize();
  if (css == "row-resize")    return glint_cursor_row_resize();
  if (css == "copy")          return glint_cursor_copy();
  if (css == "alias")         return glint_cursor_alias();
  if (css == "vertical-text") return glint_cursor_vertical_text();

  // Standard Win32 system cursors — exact mapping from Chrome's GetCursorId().
  // Integer OCR_ IDs compile in both UNICODE and ANSI builds.
  static constexpr WORD OCR_NORMAL   = 32512;  // IDC_ARROW
  static constexpr WORD OCR_IBEAM    = 32513;  // IDC_IBEAM
  static constexpr WORD OCR_WAIT     = 32514;  // IDC_WAIT
  static constexpr WORD OCR_CROSS    = 32515;  // IDC_CROSS
  static constexpr WORD OCR_SIZENWSE = 32642;  // IDC_SIZENWSE
  static constexpr WORD OCR_SIZENESW = 32643;  // IDC_SIZENESW
  static constexpr WORD OCR_SIZEWE   = 32644;  // IDC_SIZEWE
  static constexpr WORD OCR_SIZENS   = 32645;  // IDC_SIZENS
  static constexpr WORD OCR_SIZEALL  = 32646;  // IDC_SIZEALL
  static constexpr WORD OCR_NO       = 32648;  // IDC_NO
  static constexpr WORD OCR_HAND     = 32649;  // IDC_HAND
  static constexpr WORD OCR_APPSTART = 32650;  // IDC_APPSTARTING
  static constexpr WORD OCR_HELP     = 32651;  // IDC_HELP

  WORD id = OCR_NORMAL;

  if      (css == "pointer")                               id = OCR_HAND;
  else if (css == "text")                                  id = OCR_IBEAM;
  else if (css == "crosshair")                                id = OCR_CROSS;
  else if (css == "move" || css == "all-scroll")           id = OCR_SIZEALL;
  else if (css == "not-allowed" || css == "no-drop")       id = OCR_NO;
  else if (css == "wait")                                  id = OCR_WAIT;
  else if (css == "progress")                              id = OCR_APPSTART;
  else if (css == "help")                                  id = OCR_HELP;
  else if (css == "n-resize"  || css == "s-resize"  ||
           css == "ns-resize")                             id = OCR_SIZENS;
  else if (css == "e-resize"  || css == "w-resize"  ||
           css == "ew-resize")                             id = OCR_SIZEWE;
  else if (css == "ne-resize" || css == "sw-resize" ||
           css == "nesw-resize")                           id = OCR_SIZENESW;
  else if (css == "nw-resize" || css == "se-resize" ||
           css == "nwse-resize")                           id = OCR_SIZENWSE;
  // default / auto / context-menu / unknown → OCR_NORMAL
  // (Chrome: kContextMenu → NOTIMPLEMENTED() → IDC_ARROW)

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