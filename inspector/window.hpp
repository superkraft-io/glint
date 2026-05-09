#pragma once

/**
 * inspector/window.hpp
 * glint Inspector � a native Win32 window with its own glint scene graph.
 *
 * Style utilities and the editable style panel are in style_editor.hpp.
 *
 * Rendered via the SkCanvas draw path (glint_document::DrawToCanvas) into a CPU
 * SkBitmap, then blitted to the HWND with StretchDIBits.  No glint_canvas
 * dependency in the inspector's rendering path.
 *
 * Extends glint_window (glint_window_win32 on Windows) � all Win32 plumbing,
 * mouse/keyboard routing, surface management and thread lifecycle are handled
 * by the base class.  This file contains only inspector-specific logic.
 *
 * Root-change notifications are received via glint_bus subscriptions
 * (replaces the direct onTreeChanged / onNodeStyleChanged etc. callbacks on
 * glint_document).  Each subscription is registered in onThreadStarted() and
 * removed in onThreadEnded() so multiple inspectors can coexist safely.
 *
 * API (per-root � each glint_document gets its own independent inspector):
 *   glint_inspector_window::open(root);    // create + show for that root
 *   glint_inspector_window::close(root);   // teardown the inspector for that root
 *   glint_inspector_window::isOpen(root);  // query
 *
 * Thread model:
 *   - Inspector HWND + message loop run on a background thread (base class).
 *   - mainRoot is only read (snapshot via getUITree) on that thread.
 *   - The glint_document for the inspector UI is only touched from that thread.
 *   - inspectedNode (atomic) is safe for cross-thread writes.
 */

#include "../platform/glint_window.hpp"   // glint_window base (platform-dispatching umbrella)
#include "../components/glint_image.hpp"
#include "../components/glint_input.hpp"
#include "../components/glint_tree.hpp"
#include "style_editor.hpp"      // glint_style_set_by_name, glint_style_is_valid_by_name, InspStylePanel
#include "image_preview_popup.hpp" // InspImagePreviewPopup hover thumbnail overlay
#include "../utils/glint_path.hpp"
#if defined(__APPLE__)
#  include <CoreGraphics/CoreGraphics.h>
#  include <CoreVideo/CVDisplayLink.h>
#endif

#include <cctype>
#include <limits>
#include <map>
#include <vector>
#include <filesystem>
#include <ctime>
#include "include/core/SkSurface.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#if defined(_WIN32)
#  include <shlobj.h>    // SHGetFolderPathA / CSIDL_DESKTOP
#  include <shellapi.h>  // ShellExecuteA
#  include <commdlg.h>   // GetSaveFileNameW (screenshot export)
#endif

// -- Custom messages (inspector-private) --------------------------------------
static constexpr UINT WM_INSP_TREE_CHANGED    = WM_USER + 100;
static constexpr UINT WM_INSP_STYLE_CHANGED   = WM_USER + 101;
static constexpr UINT WM_INSP_HOVER_CHANGED   = WM_USER + 102;
static constexpr UINT WM_INSP_SELECT_CHANGED  = WM_USER + 103;
static constexpr UINT WM_INSP_ENABLE_INSPECT  = WM_USER + 104;  // activate crosshair mode
static constexpr UINT WM_INSP_TIMER_ID        = 2;              // ::SetTimer id for stats refresh (must not collide with glint_window_win32::SKUI_ANIM_TIMER=1)
static constexpr UINT WM_INSP_PREVIEW_SHOW_TIMER = 3;           // 150 ms show-delay for img preview popup
static constexpr UINT WM_INSP_PREVIEW_HIDE_TIMER = 4;           // 100 ms hide-delay for img preview popup
static constexpr UINT WM_INSP_TIMER_MS_NORMAL   = 500;          // default inspector refresh cadence
static constexpr UINT WM_INSP_TIMER_MS_REALTIME = 150;          // faster polling when Realtime is enabled

// -- CrosshairButton ----------------------------------------------------------
// Square icon button that draws a ? (circle with plus) symbol instead of text.
// Subclasses glint_button so hover/pressed/onClick behaviour is inherited.

class CrosshairButton : public glint_button
{
public:
  bool toggled = false;   // true while inspect mode is active

protected:
  // Resolved colors: toggled state overrides the normal dark style with a blue fill.
  struct ActiveColors { glint_color bg; glint_color icon; float brdW; glint_color brd; float radius; };

  ActiveColors resolveColors() const
  {
    if (toggled)
    {
      // Active (toggled on): blue fill, lightened on hover/press.
      const glint_color bg  = mIsPressed ? glint_color(255, 80, 130, 255)
                       : mIsHovered ? glint_color(255, 90, 150, 255)
                                    : glint_color(255, 58, 123, 255);
      const glint_color brd = glint_color(255, 140, 180, 255);
      return { bg, glint_color(255, 255, 255, 255), 1.f, brd, style.borderRadius.resolve(std::min(mRect.W(), mRect.H())) };
    }
    // Normal: use standard hover/pressed/normal style cascade.
    const glint_style& s = mIsPressed ? pressed : mIsHovered ? hover : style;
    const float refSz    = std::min(mRect.W(), mRect.H());
    return {
      ApplyOpacity(s.backgroundColor.value, s.opacity),
      s.color.value,
      s.borderWidth,
      ApplyOpacity(s.borderColor.value, s.opacity),
      s.borderRadius.resolve(refSz)
    };
  }

  void DrawToCanvas(SkCanvas* canvas) override
  {
    const auto  c   = resolveColors();
    const float rrr = c.radius;

    if (c.bg.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.bg));  p.setAntiAlias(true);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }
    if (c.brdW > 0.f && c.brd.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.brd));  p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style);  p.setStrokeWidth(c.brdW);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }

    const glint_rect  r   = getContent();
    const float  cx  = r.L + r.W() * 0.5f;
    const float  cy  = r.T + r.H() * 0.5f;
    const float  rad = std::min(r.W(), r.H()) * 0.5f - 6.f;
    const float  arm = rad - 1.5f;

    SkPaint icon;
    icon.setColor(skColor(c.icon));
    icon.setAntiAlias(true);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.5f);
    icon.setStrokeCap(SkPaint::kRound_Cap);

    canvas->drawCircle(cx, cy, rad, icon);
    canvas->drawLine(cx - arm, cy,  cx + arm, cy,  icon);
    canvas->drawLine(cx, cy - arm,  cx, cy + arm,  icon);

    for (auto& child : mChildren)
      child->DrawToCanvas(canvas);
  }
};

// -- OverlappingRectsButton --------------------------------------------------
// Square icon button that draws two overlapping rectangle outlines.
// Represents "colorize borders" � toggle to paint debug borders on all
// components in the inspected window.

class OverlappingRectsButton : public glint_button
{
public:
  bool toggled = false;   // matches glint_debug::colorizedBorders

protected:
  struct ActiveColors { glint_color bg; glint_color icon; float brdW; glint_color brd; float radius; };

  ActiveColors resolveColors() const
  {
    if (toggled)
    {
      const glint_color bg  = mIsPressed ? glint_color(255, 80, 130, 255)
                       : mIsHovered ? glint_color(255, 90, 150, 255)
                                    : glint_color(255, 58, 123, 255);
      const glint_color brd = glint_color(255, 140, 180, 255);
      return { bg, glint_color(255, 255, 255, 255), 1.f, brd, style.borderRadius.resolve(std::min(mRect.W(), mRect.H())) };
    }
    const glint_style& s = mIsPressed ? pressed : mIsHovered ? hover : style;
    const float refSz    = std::min(mRect.W(), mRect.H());
    return {
      ApplyOpacity(s.backgroundColor.value, s.opacity),
      s.color.value,
      s.borderWidth,
      ApplyOpacity(s.borderColor.value, s.opacity),
      s.borderRadius.resolve(refSz)
    };
  }

  void DrawToCanvas(SkCanvas* canvas) override
  {
    const auto  c   = resolveColors();
    const float rrr = c.radius;

    if (c.bg.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.bg));  p.setAntiAlias(true);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }
    if (c.brdW > 0.f && c.brd.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.brd));  p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style);  p.setStrokeWidth(c.brdW);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }

    // Two overlapping rect outlines � back rect (upper-left) + front rect (lower-right)
    const glint_rect  r   = getContent();
    const float  cx  = r.L + r.W() * 0.5f;
    const float  cy  = r.T + r.H() * 0.5f;
    const float  hw  = 5.f;    // half-width of each rect
    const float  hh  = 4.f;    // half-height of each rect
    const float  off = 2.5f;   // offset between the two rects

    SkPaint icon;
    icon.setColor(skColor(c.icon));
    icon.setAntiAlias(true);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.f);

    canvas->drawRect(SkRect::MakeLTRB(cx - off - hw, cy - off - hh,
                                      cx - off + hw, cy - off + hh), icon);
    canvas->drawRect(SkRect::MakeLTRB(cx + off - hw, cy + off - hh,
                                      cx + off + hw, cy + off + hh), icon);

    for (auto& child : mChildren)
      child->DrawToCanvas(canvas);
  }
};

// -- TrashCanButton ----------------------------------------------------------
// Square icon button that draws a trash-can symbol.
// Used for the "Clear" action in the Network tab toolbar.

class TrashCanButton : public glint_button
{
public:
  bool disabled = false;

protected:
  struct ActiveColors { glint_color bg; glint_color icon; float brdW; glint_color brd; float radius; };

  ActiveColors resolveColors() const
  {
    if (disabled)
    {
      const float refSz = std::min(mRect.W(), mRect.H());
      return {
        glint_color(255, 34, 34, 34),
        glint_color(255, 92, 92, 92),
        1.f,
        glint_color(255, 60, 60, 60),
        style.borderRadius.resolve(refSz)
      };
    }
    const glint_style& s = mIsPressed ? pressed : mIsHovered ? hover : style;
    const float refSz   = std::min(mRect.W(), mRect.H());
    return {
      ApplyOpacity(s.backgroundColor.value, s.opacity),
      s.color.value,
      s.borderWidth,
      ApplyOpacity(s.borderColor.value, s.opacity),
      s.borderRadius.resolve(refSz)
    };
  }

  void DrawToCanvas(SkCanvas* canvas) override
  {
    const auto  c   = resolveColors();
    const float rrr = c.radius;

    if (c.bg.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.bg));  p.setAntiAlias(true);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }
    if (c.brdW > 0.f && c.brd.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.brd));  p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style);  p.setStrokeWidth(c.brdW);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }

    const glint_rect  r  = getContent();
    const float  cx = r.L + r.W() * 0.5f;
    const float  cy = r.T + r.H() * 0.5f;

    SkPaint icon;
    icon.setColor(skColor(c.icon));
    icon.setAntiAlias(true);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.4f);
    icon.setStrokeCap(SkPaint::kRound_Cap);
    icon.setStrokeJoin(SkPaint::kRound_Join);

    // Measurements (centred on cx, cy)
    const float bw   = 4.5f;  // body half-width at lid
    const float bh   = 7.5f;  // body height
    const float lidY = cy - 2.5f;
    const float botY = lidY + bh;
    const float hw   = 2.f;   // handle half-width
    const float hh   = 2.5f;  // handle height above lid
    const float inset = 1.f;  // how much each side tapers inward at bottom

    // Handle (small ⊓ above the lid)
    SkPath handle;
    handle.moveTo(cx - hw, lidY);
    handle.lineTo(cx - hw, lidY - hh);
    handle.lineTo(cx + hw, lidY - hh);
    handle.lineTo(cx + hw, lidY);
    canvas->drawPath(handle, icon);

    // Lid (horizontal line, slightly wider than body)
    canvas->drawLine(cx - bw - 1.f, lidY, cx + bw + 1.f, lidY, icon);

    // Body: open-top trapezoid (sides taper inward slightly toward the bottom)
    SkPath body;
    body.moveTo(cx - bw,         lidY);
    body.lineTo(cx - bw + inset, botY);
    body.lineTo(cx + bw - inset, botY);
    body.lineTo(cx + bw,         lidY);
    canvas->drawPath(body, icon);
    canvas->drawLine(cx - bw + inset, botY, cx + bw - inset, botY, icon); // bottom cap

    // Three vertical slat lines inside the body
    SkPaint slat = icon;
    slat.setStrokeWidth(1.1f);
    const float slatTop = lidY + 2.f;
    const float slatBot = botY - 1.5f;
    for (int s = -1; s <= 1; ++s)
      canvas->drawLine(cx + s * 2.f, slatTop, cx + s * 2.f, slatBot, slat);

    for (auto& child : mChildren)
      child->DrawToCanvas(canvas);
  }
};

// -- CameraButton ------------------------------------------------------------
// Square icon button that draws a camera symbol.
// Used for the "Screenshot" action in the inspector header.

class CameraButton : public glint_button
{
protected:
  struct ActiveColors { glint_color bg; glint_color icon; float brdW; glint_color brd; float radius; };

  ActiveColors resolveColors() const
  {
    const glint_style& s = mIsPressed ? pressed : mIsHovered ? hover : style;
    const float refSz    = std::min(mRect.W(), mRect.H());
    return {
      ApplyOpacity(s.backgroundColor.value, s.opacity),
      s.color.value,
      s.borderWidth,
      ApplyOpacity(s.borderColor.value, s.opacity),
      s.borderRadius.resolve(refSz)
    };
  }

  void DrawToCanvas(SkCanvas* canvas) override
  {
    const auto  c   = resolveColors();
    const float rrr = c.radius;

    if (c.bg.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.bg));  p.setAntiAlias(true);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }
    if (c.brdW > 0.f && c.brd.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.brd));  p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style);  p.setStrokeWidth(c.brdW);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }

    const glint_rect  r  = getContent();
    const float  cx = r.L + r.W() * 0.5f;
    const float  cy = r.T + r.H() * 0.5f;

    SkPaint icon;
    icon.setColor(skColor(c.icon));
    icon.setAntiAlias(true);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.3f);
    icon.setStrokeCap(SkPaint::kRound_Cap);
    icon.setStrokeJoin(SkPaint::kRound_Join);

    // Camera body: rounded rectangle centred on (cx, cy)
    const float bw   = 7.f;   // body half-width
    const float bh   = 5.f;   // body half-height
    const float rad  = 1.2f;  // body corner radius
    const SkRect body = SkRect::MakeLTRB(cx - bw, cy - bh + 0.5f, cx + bw, cy + bh + 0.5f);
    canvas->drawRoundRect(body, rad, rad, icon);

    // Viewfinder bump on top (small notch indicating the prism / hot shoe)
    const float nw = 2.5f;  // notch half-width
    const float nh = 1.6f;  // notch height above body
    SkPath notch;
    notch.moveTo(cx - nw - 1.f, cy - bh + 0.5f);
    notch.lineTo(cx - nw,       cy - bh + 0.5f - nh);
    notch.lineTo(cx + nw,       cy - bh + 0.5f - nh);
    notch.lineTo(cx + nw + 1.f, cy - bh + 0.5f);
    canvas->drawPath(notch, icon);

    // Lens (centred circle)
    canvas->drawCircle(cx, cy + 0.7f, 2.6f, icon);

    // Shutter dot in the top-right of the body
    SkPaint dot;
    dot.setColor(skColor(c.icon));
    dot.setAntiAlias(true);
    dot.setStyle(SkPaint::kFill_Style);
    canvas->drawCircle(cx + bw - 1.6f, cy - bh + 2.0f, 0.8f, dot);

    for (auto& child : mChildren)
      child->DrawToCanvas(canvas);
  }
};

// -- HtmlDocButton -----------------------------------------------------------
// Square icon button that draws a document outline with a folded corner and a
// small "< >" glyph centred inside.  Used for the "Export HTML" action.

class HtmlDocButton : public glint_button
{
protected:
  struct ActiveColors { glint_color bg; glint_color icon; float brdW; glint_color brd; float radius; };

  ActiveColors resolveColors() const
  {
    const glint_style& s = mIsPressed ? pressed : mIsHovered ? hover : style;
    const float refSz    = std::min(mRect.W(), mRect.H());
    return {
      ApplyOpacity(s.backgroundColor.value, s.opacity),
      s.color.value,
      s.borderWidth,
      ApplyOpacity(s.borderColor.value, s.opacity),
      s.borderRadius.resolve(refSz)
    };
  }

  void DrawToCanvas(SkCanvas* canvas) override
  {
    const auto  c   = resolveColors();
    const float rrr = c.radius;

    if (c.bg.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.bg));  p.setAntiAlias(true);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }
    if (c.brdW > 0.f && c.brd.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.brd));  p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style);  p.setStrokeWidth(c.brdW);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }

    const glint_rect  r  = getContent();
    const float  cx = r.L + r.W() * 0.5f;
    const float  cy = r.T + r.H() * 0.5f;

    SkPaint icon;
    icon.setColor(skColor(c.icon));
    icon.setAntiAlias(true);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.3f);
    icon.setStrokeCap(SkPaint::kRound_Cap);
    icon.setStrokeJoin(SkPaint::kRound_Join);

    // Document outline: vertical rect with the top-right corner folded over.
    const float pw   = 6.f;   // page half-width
    const float ph   = 7.5f;  // page half-height
    const float fold = 2.5f;  // size of the folded corner
    const float pl   = cx - pw;
    const float pr   = cx + pw;
    const float pt   = cy - ph;
    const float pb   = cy + ph;

    SkPath page;
    page.moveTo(pl,         pt);
    page.lineTo(pr - fold,  pt);
    page.lineTo(pr,         pt + fold);
    page.lineTo(pr,         pb);
    page.lineTo(pl,         pb);
    page.close();
    canvas->drawPath(page, icon);

    // Folded corner indicator (small triangle in the top-right).
    SkPath fc;
    fc.moveTo(pr - fold, pt);
    fc.lineTo(pr - fold, pt + fold);
    fc.lineTo(pr,        pt + fold);
    canvas->drawPath(fc, icon);

    // "< >" glyph centred in the lower body of the page.
    SkPaint glyph = icon;
    glyph.setStrokeWidth(1.1f);
    const float gy   = cy + 1.8f;   // vertical centre of the glyph
    const float gh   = 2.2f;        // glyph half-height
    const float gx   = 2.4f;        // horizontal offset of each chevron tip from cx
    const float gtip = 1.6f;        // how far each chevron tip extends outward

    // Left chevron "<"
    SkPath lt;
    lt.moveTo(cx - gx,         gy - gh);
    lt.lineTo(cx - gx - gtip,  gy);
    lt.lineTo(cx - gx,         gy + gh);
    canvas->drawPath(lt, glyph);

    // Right chevron ">"
    SkPath rt;
    rt.moveTo(cx + gx,         gy - gh);
    rt.lineTo(cx + gx + gtip,  gy);
    rt.lineTo(cx + gx,         gy + gh);
    canvas->drawPath(rt, glyph);

    for (auto& child : mChildren)
      child->DrawToCanvas(canvas);
  }
};

// -- PulseButton -------------------------------------------------------------
// Square icon button that draws an ECG-style pulse waveform: flat baseline
// with a single QRS-complex spike (small upward bump, sharp R-wave up,
// sharp S-wave down, recovery bump, then baseline).  Used for the inspector's
// "Realtime" polling toggle; active/inactive coloring is driven externally
// via applyRealtimeMode().

class PulseButton : public glint_button
{
protected:
  struct ActiveColors { glint_color bg; glint_color icon; float brdW; glint_color brd; float radius; };

  ActiveColors resolveColors() const
  {
    const glint_style& s = mIsPressed ? pressed : mIsHovered ? hover : style;
    const float refSz    = std::min(mRect.W(), mRect.H());
    return {
      ApplyOpacity(s.backgroundColor.value, s.opacity),
      s.color.value,
      s.borderWidth,
      ApplyOpacity(s.borderColor.value, s.opacity),
      s.borderRadius.resolve(refSz)
    };
  }

  void DrawToCanvas(SkCanvas* canvas) override
  {
    const auto  c   = resolveColors();
    const float rrr = c.radius;

    if (c.bg.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.bg));  p.setAntiAlias(true);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }
    if (c.brdW > 0.f && c.brd.A > 0)
    {
      SkPaint p;  p.setColor(skColor(c.brd));  p.setAntiAlias(true);
      p.setStyle(SkPaint::kStroke_Style);  p.setStrokeWidth(c.brdW);
      if (rrr > 0.f) canvas->drawRoundRect(skRect(mRect), rrr, rrr, p);
      else           canvas->drawRect(skRect(mRect), p);
    }

    const glint_rect  r  = getContent();
    const float  cx = r.L + r.W() * 0.5f;
    const float  cy = r.T + r.H() * 0.5f;

    SkPaint icon;
    icon.setColor(skColor(c.icon));
    icon.setAntiAlias(true);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.4f);
    icon.setStrokeCap(SkPaint::kRound_Cap);
    icon.setStrokeJoin(SkPaint::kRound_Join);

    // ECG pulse: flat baseline -> small P-wave bump -> sharp R-wave spike up
    // -> sharp S-wave dip down -> small T-wave bump -> flat baseline.
    SkPath wave;
    wave.moveTo(cx - 8.0f, cy);
    wave.lineTo(cx - 5.0f, cy);              // baseline left
    wave.lineTo(cx - 3.5f, cy - 1.2f);       // P-wave up
    wave.lineTo(cx - 2.0f, cy);              // back to baseline
    wave.lineTo(cx - 1.0f, cy - 5.0f);       // R-wave up (sharp)
    wave.lineTo(cx + 0.5f, cy + 4.0f);       // S-wave down (sharp)
    wave.lineTo(cx + 1.5f, cy);              // back to baseline
    wave.lineTo(cx + 3.0f, cy - 1.5f);       // T-wave up
    wave.lineTo(cx + 4.5f, cy);              // back to baseline
    wave.lineTo(cx + 8.0f, cy);              // baseline right
    canvas->drawPath(wave, icon);

    for (auto& child : mChildren)
      child->DrawToCanvas(canvas);
  }
};

// -- InspTabBtn ---------------------------------------------------------------
// Bottom-tab-bar button for the inspector window.  Draws its own label text
// and a 2 px blue top-line indicator when active.  Background color is managed
// externally (switchTab updates style.backgroundColor so the base class draws
// it for us).

class InspTabBtn : public glint_element
{
public:
  std::string label;
  bool        active = false;

protected:
  void OnMouseDown(float, float, const glint_mouse_mod&) override { /* click via DOM */ }

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (label.empty()) return;

    // Text
    const glint_color col = active ? glint_color(255, 220, 220, 220) : glint_color(255, 95, 95, 95);
    SkPaint p;
    p.setColor(skColor(col));
    p.setAntiAlias(true);
    SkFont font = skFont(12.f);
    const glint_rect  r     = getContent();
    const float  tw    = font.measureText(label.c_str(), label.size(), SkTextEncoding::kUTF8);
    const float  tx    = r.L + (r.W() - tw) * 0.5f;
    const float  ty    = r.T + (r.H() + 10.f) * 0.5f;
    canvas->drawString(label.c_str(), tx, ty, font, p);

    // Active indicator: 2 px blue line along top edge
    if (active)
    {
      SkPaint line;
      line.setColor(SkColorSetARGB(255, 74, 158, 255));
      line.setStrokeWidth(2.f);
      line.setAntiAlias(false);
      canvas->drawLine(mRect.L, mRect.T + 1.f, mRect.R, mRect.T + 1.f, line);
    }
  }
};

// -- InspSectionLabel ---------------------------------------------------------
// Uppercase 10 px grey section heading with a hairline divider to the right.

class InspSectionLabel : public glint_element
{
public:
  std::string text;

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (text.empty()) return;
    SkFont  font = skFont(10.f);
    SkPaint tp;
    tp.setColor(SkColorSetARGB(255, 110, 110, 110));
    tp.setAntiAlias(true);
    const glint_rect r  = getContent();
    const float tw = font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8);
    canvas->drawString(text.c_str(), r.L, r.T + r.H() * 0.5f + 4.f, font, tp);
    // Hairline after text
    const float lx = r.L + tw + 8.f;
    SkPaint lp;
    lp.setColor(SkColorSetARGB(80, 255, 255, 255));
    lp.setStrokeWidth(1.f);
    canvas->drawLine(lx, r.T + r.H() * 0.5f, r.R, r.T + r.H() * 0.5f, lp);
  }
};

// -- InspFpsChart -------------------------------------------------------------
// Rolling FPS line chart for the Rendering tab.
// Set mMainRoot after construction.  Reads getFrameSamples() each repaint.
// Fetches the screen refresh rate via Win32 EnumDisplaySettingsW in the constructor.

class InspFpsChart : public glint_element
{
public:
  glint_document* mMainRoot    = nullptr;
  int         mRefreshRate = 60;     // screen Hz fetched in the constructor

  const char* typeName() const override { return "fps-chart"; }

  InspFpsChart()
  {
#if defined(OS_WIN) || defined(_WIN32)
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
      mRefreshRate = static_cast<int>(dm.dmDisplayFrequency);
#elif defined(__APPLE__)
    CGDirectDisplayID display = CGMainDisplayID();
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (mode)
    {
      double hz = CGDisplayModeGetRefreshRate(mode);
      CGDisplayModeRelease(mode);
      // ProMotion / high-refresh displays report 0 via CGDisplayModeGetRefreshRate;
      // fall back to CVDisplayLink in that case.
      if (hz < 1.0)
      {
        CVDisplayLinkRef link = nullptr;
        if (CVDisplayLinkCreateWithCGDisplay(display, &link) == kCVReturnSuccess)
        {
          CVTime t = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
          if (t.timeValue > 0)
            hz = static_cast<double>(t.timeScale) / static_cast<double>(t.timeValue);
          CVDisplayLinkRelease(link);
        }
      }
      if (hz >= 1.0)
        mRefreshRate = static_cast<int>(std::round(hz));
    }
#endif
  }

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    const glint_rect  r = getContent();
    const float  W = r.W();
    const float  H = r.H();
    if (W <= 0.f || H <= 0.f) return;

    std::vector<float> samples;
    if (mMainRoot) samples = mMainRoot->getFrameSamples();

    // -- Y-axis auto-scale --------------------------------------------------
    // Ceiling = max(samples, refHz) * 1.15, snapped up to the nearest 10.
    float maxSample = static_cast<float>(mRefreshRate);
    for (float v : samples) if (v > maxSample) maxSample = v;
    const float yMax = std::ceil((maxSample * 1.15f) / 10.f) * 10.f;

    // fps value -> Y pixel in content rect (bottom of rect = 0 fps).
    auto fpsToY = [&](float fps) {
      return r.T + H - (fps / yMax) * H;
    };

    const int   n    = static_cast<int>(samples.size());
    const float refY = fpsToY(static_cast<float>(mRefreshRate));

    // -- Reference line at screen refresh rate (manual dashes) -------------
    {
      SkPaint p;
      p.setColor(SkColorSetARGB(80, 255, 255, 255));
      p.setStrokeWidth(1.f);
      p.setAntiAlias(false);
      constexpr float kDash = 6.f, kGap = 4.f;
      float x = r.L;
      bool  on = true;
      while (x < r.R)
      {
        const float end = std::min(x + (on ? kDash : kGap), r.R);
        if (on) canvas->drawLine(x, refY, end, refY, p);
        x  = end;
        on = !on;
      }
    }

    // -- FPS filled area + line path ----------------------------------------
    if (n >= 2)
    {
      auto xAt = [&](int i) {
        return r.L + (static_cast<float>(i) / static_cast<float>(n - 1)) * W;
      };

      SkPath linePath, areaPath;
      areaPath.moveTo(xAt(0), r.T + H);
      areaPath.lineTo(xAt(0), fpsToY(samples[0]));
      linePath.moveTo(xAt(0), fpsToY(samples[0]));

      for (int i = 1; i < n; ++i)
      {
        const float y = fpsToY(samples[i]);
        linePath.lineTo(xAt(i), y);
        areaPath.lineTo(xAt(i), y);
      }
      areaPath.lineTo(xAt(n - 1), r.T + H);
      areaPath.close();

      // Filled area under the curve.
      SkPaint fill;
      fill.setColor(SkColorSetARGB(55, 80, 200, 120));
      fill.setStyle(SkPaint::kFill_Style);
      fill.setAntiAlias(true);
      canvas->drawPath(areaPath, fill);

      // Line.
      SkPaint line;
      line.setColor(SkColorSetARGB(220, 80, 200, 120));
      line.setStyle(SkPaint::kStroke_Style);
      line.setStrokeWidth(1.5f);
      line.setStrokeCap(SkPaint::kRound_Cap);
      line.setStrokeJoin(SkPaint::kRound_Join);
      line.setAntiAlias(true);
      canvas->drawPath(linePath, line);
    }
    else
    {
      SkPaint tp;
      tp.setColor(SkColorSetARGB(100, 180, 180, 180));
      tp.setAntiAlias(true);
      canvas->drawString("No data", r.L + 4.f, r.T + H * 0.5f + 4.f, skFont(11.f), tp);
    }

    // -- Y-axis labels ------------------------------------------------------
    {
      SkFont  font = skFont(9.f);
      SkPaint tp;
      tp.setColor(SkColorSetARGB(130, 180, 180, 180));
      tp.setAntiAlias(true);

      char buf[24];

      // Max label (top left).
      snprintf(buf, sizeof(buf), "%.0f", yMax);
      canvas->drawString(buf, r.L + 2.f, r.T + 9.f, font, tp);

      // Reference line label – drawn above the dashed line, left edge.
      // Only draw if it won't overlap the max label above (need >= 12 px gap).
      const float kMinLabelGap = 12.f;
      const float hzLabelY = refY - 3.f;
      const float maxLabelY = r.T + 9.f;
      if (hzLabelY - maxLabelY >= kMinLabelGap)
      {
        snprintf(buf, sizeof(buf), "%d Hz", mRefreshRate);
        canvas->drawString(buf, r.L + 2.f, hzLabelY, font, tp);
      }
    }
  }
};

// -- glint_inspector_window ----------------------------------------------------

class glint_inspector_window : public glint_window
{
public:
  // -- Per-root API ----------------------------------------------------------
  //  Each glint_document can have its own independent inspector window.

  /** Create and show an inspector window for mainRoot. No-op if already open for that root,
   *  or if mainRoot->skipInspectMode is true (that root has opted out of being inspected). */
  static void open(glint_document* mainRoot)
  {
    if (mainRoot->skipInspectMode) return;   // root has opted out of inspection
    auto it = sInstances.find(mainRoot);
    if (it != sInstances.end() && it->second->isRunning()) return;
    auto* inst = new glint_inspector_window(mainRoot);
    sInstances[mainRoot] = inst;
    inst->startThread();
  }

  /** Open the inspector for mainRoot and immediately activate crosshair (element-picker) mode.
   *  If the inspector is already open the crosshair is toggled on.
   *  No-op if mainRoot->skipInspectMode is true. */
  static void openAndEnableInspect(glint_document* mainRoot)
  {
    if (mainRoot->skipInspectMode) return;
    auto it = sInstances.find(mainRoot);
    if (it != sInstances.end() && it->second->isRunning())
    {
      // Already running � post the enable message to the inspector's HWND.
#if defined(_WIN32)
      if (HWND h = it->second->mHWNDAtom.load())
        ::PostMessage(h, WM_INSP_ENABLE_INSPECT, 0, 0);
#elif defined(__APPLE__)
      glint_inspector_window* inst = it->second;
      auto alive = inst->mAlive;
      glint_window_mac::_dispatchMain([inst, alive]{ if (alive && alive->load()) inst->applyInspectMode(true); });
#endif
      return;
    }
    // Not yet open: open it and set a pending flag so onCreated() activates inspect mode.
    auto* inst = new glint_inspector_window(mainRoot);
    inst->mPendingEnableInspect = true;
    sInstances[mainRoot] = inst;
    inst->startThread();
  }

  /** Close the inspector for mainRoot. */
  static void close(glint_document* mainRoot)
  {
    auto it = sInstances.find(mainRoot);
    if (it == sInstances.end()) return;
    it->second->stopThread();
  }

  /** True while the inspector for mainRoot is alive. */
  static bool isOpen(glint_document* mainRoot)
  {
    auto it = sInstances.find(mainRoot);
    return it != sInstances.end() && it->second->isRunning();
  }

private:
  explicit glint_inspector_window(glint_document* mainRoot)
    : mMainRoot(mainRoot)
  {
    if (!mainRoot->name.empty()) {
#if defined(_WIN32)
      // Convert UTF-8 name to wide string for the Win32 title bar.
      const int len = ::MultiByteToWideChar(CP_UTF8, 0,
                        mainRoot->name.c_str(), -1, nullptr, 0);
      std::wstring wide(len - 1, L'\0');
      ::MultiByteToWideChar(CP_UTF8, 0, mainRoot->name.c_str(), -1, wide.data(), len);
#else
      // ASCII / basic UTF-8 pass-through (doc names are typically ASCII identifiers).
      const std::string& ns = mainRoot->name;
      std::wstring wide(ns.begin(), ns.end());
#endif
      mDynTitle = L"Inspecting " + wide;
    } else {
      mDynTitle = L"glint Inspector";
    }
  }

  // -- Layout constants -------------------------------------------------------
  static constexpr int kHeaderH  = 36;
  static constexpr int kStyleW   = 500;   // style panel fixed width (right column)
  static constexpr int kTabH     = 32;    // bottom tab bar height
  static constexpr const char* kStyleWResponsive = "61%"; // preserves ~500 px at the default 820 px window width

  // -- State ------------------------------------------------------------------
  glint_document*      mMainRoot     = nullptr;   // not owned
  glint_element* mSelectedComp = nullptr;   // last persistently selected component
  uint64_t      mSelectedNodeId = 0;       // stable id for refresh / deletion fallback
  std::vector<uint64_t> mRemovedNodeUndoStack;    // most recently removed node IDs (LIFO)
  CrosshairButton*        mInspectBtn   = nullptr;   // Inspect toggle (header)
  glint_button*            mRealtimeBtn  = nullptr;   // Realtime polling toggle (header)
  TrashCanButton*         mRemoveNodeBtn = nullptr;  // Remove selected tree node (header)
  OverlappingRectsButton* mColorizeBtn  = nullptr;   // Colorize borders (Rendering tab)
  glint_tree*      mTree                = nullptr;   // inside mOwnRoot (not owned)
  InspStylePanel*  mStylePanel          = nullptr;   // inside mOwnRoot (not owned)

  // The inspector always uses the shared base-class CPU raster surface even in
  // GPU-enabled builds. That keeps it on the same bitmap/canvas path the base
  // Win32 presenter actually blits to the HWND.

  // -- Tab state --------------------------------------------------------------
  int              mActiveTab    = 0;                // 0 = Inspector, 1 = Rendering, 2 = Network
  glint_element* mInspBody     = nullptr;          // inspector tab body panel
  glint_element* mRenderBody   = nullptr;          // rendering tab body panel
  glint_element* mNetworkBody  = nullptr;          // network tab body panel
  InspTabBtn*      mTabBtns[3]   = { nullptr, nullptr, nullptr };
  InspFpsChart*    mFpsChart     = nullptr;          // FPS chart in Rendering tab
  // -- Network tab state ------------------------------------------------------
  glint_element*    mNetworkListContainer = nullptr;  // scrollable row container
  glint_element*    mNetworkCountLabel    = nullptr;  // "N requests" label in toolbar
  int              mNetworkEntryCount    = -1;       // last snapshot size (-1 = force rebuild)
  // -- Right sidebar tab state -----------------------------------------------
  InspComputedPanel* mComputedPanel    = nullptr;    // inside mOwnRoot (not owned)
  InspImagePreviewPopup* mPreviewPopup = nullptr;    // inside mOwnRoot canvas (not owned)
  int              mActiveRightTab  = 0;              // 0 = Style, 1 = Computed
  InspTabBtn*      mRightTabBtns[2] = { nullptr, nullptr };
  // Rendering tab labels (text updated by WM_TIMER every 500 ms)
  glint_element*     mFpsLabel       = nullptr;
  glint_element*     mFrameTimeLabel = nullptr;
  glint_element*     mDrawCountLabel = nullptr;

  std::vector<int> mSubIds;                               // glint_bus subscription IDs
  std::wstring     mDynTitle;                             // "Inspecting <name>" � built in constructor
  bool             mPendingEnableInspect = false;         // activate crosshair on first onCreated()
  bool             mRealtimeMode         = false;         // poll selected node panels on timer
#if defined(__APPLE__)
  // Live token passed (by value) into bus-callback dispatch blocks.
  // Set in onThreadStarted(); cleared in onThreadEnded() before self-destruct.
  std::shared_ptr<std::atomic<bool>> mAlive;
#endif

  static std::map<glint_document*, glint_inspector_window*> sInstances;

  // -- Thread entry ----------------------------------------------------------
#if 0 // dead run() and createWindow() � superseded by glint_window base
  void run(glint_document* mainRoot)
  {
    mRunning  = true;
    mMainRoot = mainRoot;

    if (!createWindow())
    {
      mRunning = false;
      return;
    }

    // Subscribe to bus events for this root.
    mSubIds.push_back(
      glint_bus::subscribe<glint_tree_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_TREE_CHANGED, 0, 0);
      })
    );
    mSubIds.push_back(
      glint_bus::subscribe<glint_node_style_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_STYLE_CHANGED,
                        static_cast<WPARAM>(e.id & 0xFFFFFFFF),
                        static_cast<LPARAM>(e.id >> 32));
      })
    );
    mSubIds.push_back(
      glint_bus::subscribe<glint_hovered_node_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
        if (!glint_debug::inspectMode.load()) return;
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_HOVER_CHANGED,
                        static_cast<WPARAM>(e.id & 0xFFFFFFFF),
                        static_cast<LPARAM>(e.id >> 32));
      })
    );
    mSubIds.push_back(
      glint_bus::subscribe<glint_selected_node_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_SELECT_CHANGED,
                        static_cast<WPARAM>(e.id & 0xFFFFFFFF),
                        static_cast<LPARAM>(e.id >> 32));
      })
    );

    // Initial tree snapshot
    refreshTree();

    // Message loop
    MSG msg;
    while (::GetMessage(&msg, nullptr, 0, 0) > 0)
    {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
    }

    // Unsubscribe from bus events.
    for (int id : mSubIds) glint_bus::unsubscribe(id);
    mSubIds.clear();
    // Reset inspect mode and both highlights so the main UI returns to normal.
    glint_debug::inspectMode.store(false);
    glint_debug::hoveredNode.store(nullptr);
    glint_debug::inspectedNode.store(nullptr);
    glint_debug::pinnedNode.store(nullptr);
    mHWNDAtom = nullptr;
    mRunning  = false;
    // Remove from the per-root registry and self-destruct.
    sInstances.erase(mMainRoot);
    delete this;
  }

  // -- Window creation --------------------------------------------------------
  bool createWindow()
  {
    static bool sRegistered = false;
    if (!sRegistered)
    {
      WNDCLASSEXW wc    = {};
      wc.cbSize         = sizeof(wc);
      wc.style          = CS_HREDRAW | CS_VREDRAW;
      wc.lpfnWndProc    = WndProc;
      wc.hInstance      = ::GetModuleHandleW(nullptr);
      wc.hCursor        = ::LoadCursor(nullptr, IDC_ARROW);
      wc.hbrBackground  = reinterpret_cast<HBRUSH>(::CreateSolidBrush(RGB(26, 26, 26)));
      wc.lpszClassName  = L"glint_inspector";
      if (!::RegisterClassExW(&wc)) return false;
      sRegistered = true;
    }

    mHWND = ::CreateWindowExW(
      WS_EX_TOOLWINDOW,
      L"glint_inspector",
      L"glint Inspector",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT,
      mW, mH,
      nullptr, nullptr,
      ::GetModuleHandleW(nullptr),
      this   // passed to WM_NCCREATE as lpCreateParams
    );
    if (!mHWND) return false;

    ::ShowWindow(mHWND, SW_SHOW);
    ::UpdateWindow(mHWND);
    mHWNDAtom = mHWND;
    return true;
  }
#endif

  // -- Win32 identity --------------------------------------------------------
  const wchar_t* windowClassName() const override { return L"glint_inspector"; }
  const wchar_t* windowTitle()     const override { return mDynTitle.c_str(); }
#if defined(__APPLE__)
  // macOS title bar uses UTF-8 via macTitleUTF8(); convert mDynTitle once and cache.
  const char* macTitleUTF8() const override
  {
    if (mDynTitleUTF8.empty() && !mDynTitle.empty())
    {
      for (wchar_t wc : mDynTitle)
        mDynTitleUTF8 += (wc < 0x80) ? static_cast<char>(wc) : '?';
    }
    return mDynTitleUTF8.c_str();
  }
  mutable std::string mDynTitleUTF8;
#endif
  int  defaultWidth()  const override { return 820; }
  int  defaultHeight() const override { return 650; }
#if defined(_WIN32)
  COLORREF bgColor() const override { return RGB(26, 26, 26); }
#endif
  SkColor clearColor() const override { return SkColorSetARGB(255, 26, 26, 26); }
  bool useGpu() const override { return false; }

  // -- Lifecycle overrides ---------------------------------------------------
  void onCreated() override
  {
    refreshTree();
    if (mPendingEnableInspect)
    {
      mPendingEnableInspect = false;
      applyInspectMode(true);
    }
    applyRealtimeMode(mRealtimeMode);
  }

  void onThreadStarted() override
  {
#if defined(__APPLE__)
    mAlive = std::make_shared<std::atomic<bool>>(true);
#endif
    mSubIds.push_back(
      glint_bus::subscribe<glint_tree_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
#if defined(_WIN32)
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_TREE_CHANGED, 0, 0);
#elif defined(__APPLE__)
        auto alive = mAlive;
        glint_window_mac::_dispatchMain([this, alive]{ if (alive->load()) refreshTree(); });
#endif
      })
    );
    mSubIds.push_back(
      glint_bus::subscribe<glint_node_style_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
#if defined(_WIN32)
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_STYLE_CHANGED,
                        static_cast<WPARAM>(e.id & 0xFFFFFFFF),
                        static_cast<LPARAM>(e.id >> 32));
#elif defined(__APPLE__)
        const uint64_t id = e.id;
        auto alive = mAlive;
        glint_window_mac::_dispatchMain([this, alive, id]{ if (alive->load()) refreshStyle(id); });
#endif
      })
    );
    mSubIds.push_back(
      glint_bus::subscribe<glint_hovered_node_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
        if (!glint_debug::inspectMode.load()) return;
#if defined(_WIN32)
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_HOVER_CHANGED,
                        static_cast<WPARAM>(e.id & 0xFFFFFFFF),
                        static_cast<LPARAM>(e.id >> 32));
#elif defined(__APPLE__)
        const uint64_t id = e.id;
        auto alive = mAlive;
        glint_window_mac::_dispatchMain([this, alive, id]{
          if (!alive->load()) return;
          if (mTree) mTree->hoverById(id);
          requestRedraw();
        });
#endif
      })
    );
    mSubIds.push_back(
      glint_bus::subscribe<glint_selected_node_changed_event>([this](const auto& e) {
        if (e.root != mMainRoot) return;
#if defined(_WIN32)
        if (HWND h = mHWNDAtom.load())
          ::PostMessage(h, WM_INSP_SELECT_CHANGED,
                        static_cast<WPARAM>(e.id & 0xFFFFFFFF),
                        static_cast<LPARAM>(e.id >> 32));
#elif defined(__APPLE__)
        const uint64_t id = e.id;
        auto alive = mAlive;
        glint_window_mac::_dispatchMain([this, alive, id]{
          if (!alive->load()) return;
          selectInspectorNodeById(id);
          applyInspectMode(false);
          if (mPreviewPopup) {
            killTimer(WM_INSP_PREVIEW_SHOW_TIMER);
            killTimer(WM_INSP_PREVIEW_HIDE_TIMER);
            mPreviewPopup->dismiss();
          }
          requestRedraw();
        });
#endif
      })
    );
  }

  void onThreadEnded() override
  {
#if defined(_WIN32)
    if (mHWND) ::KillTimer(mHWND, WM_INSP_TIMER_ID);
#elif defined(__APPLE__)
    if (mAlive) mAlive->store(false);  // prevent queued dispatch blocks from running
    killTimer(WM_INSP_TIMER_ID);
#endif
    for (int id : mSubIds) glint_bus::unsubscribe(id);
    mSubIds.clear();
    glint_debug::inspectMode.store(false);
    glint_debug::hoveredNode.store(nullptr);
    glint_debug::inspectedNode.store(nullptr);
    glint_debug::pinnedNode.store(nullptr);
    glint_debug::inspectorDoc = nullptr;
  }

  void afterRun() override
  {
    sInstances.erase(mMainRoot);
    delete this;
  }

  static uint64_t unpackId(WPARAM wp, LPARAM lp)
  {
    return (static_cast<uint64_t>(static_cast<uint32_t>(lp)) << 32)
         |  static_cast<uint64_t>(static_cast<uint32_t>(wp));
  }

  static std::string _exportFilePath(const char* ext)
  {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    ::GetTempPathA(MAX_PATH, buf);
    std::string p = buf;
    if (!p.empty() && p.back() != '\\') p += '\\';
#else
    std::string p = "/tmp/";
#endif
    p += "glint_export.";
    p += ext;
    return p;
  }

  // ── HTML export — semantic CSS render ────────────────────────────────────
  void exportDOMTreeHTML() const
  {
    if (!mMainRoot) return;

    // Ask the user where to save the exported HTML.  Falls back to the
    // %TEMP%/tmp path only on platforms without a native dialog implemented
    // here, matching the pattern used by exportScreenshot().
    std::string path;
#if defined(_WIN32)
    {
      wchar_t buf[MAX_PATH] = L"glint_export.html";
      OPENFILENAMEW ofn = {};
      ofn.lStructSize   = sizeof(ofn);
      ofn.hwndOwner     = mHWNDAtom.load();
      ofn.lpstrFilter   = L"HTML Document\0*.html;*.htm\0All Files\0*.*\0";
      ofn.lpstrDefExt   = L"html";
      ofn.lpstrFile     = buf;
      ofn.nMaxFile      = MAX_PATH;
      ofn.lpstrTitle    = L"Export DOM as HTML";
      ofn.Flags         = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      if (!::GetSaveFileNameW(&ofn)) return;  // user cancelled

      // Convert wide path → UTF-8 std::string for the rest of the function.
      const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                            nullptr, 0, nullptr, nullptr);
      if (len > 1)
      {
        path.resize(static_cast<size_t>(len - 1));
        ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), len,
                              nullptr, nullptr);
      }
      if (path.empty()) return;
    }
#elif defined(__APPLE__) && defined(__OBJC__)
    {
      NSSavePanel* panel = [NSSavePanel savePanel];
      [panel setAllowedFileTypes:@[@"html", @"htm"]];
      [panel setNameFieldStringValue:@"glint_export.html"];
      [panel setTitle:@"Export DOM as HTML"];
      if ([panel runModal] != NSModalResponseOK) return;  // user cancelled
      NSString* nsPath = [[panel URL] path];
      if (!nsPath) return;
      path = [nsPath UTF8String];
    }
#else
    path = _exportFilePath("html");
#endif

    std::ofstream f(path);
    if (!f.is_open())
    {
#if defined(_WIN32)
      OutputDebugStringA("[glint export] ERROR: could not open HTML file for writing\n");
#else
      fprintf(stderr, "[glint export] ERROR: could not open HTML file for writing\n");
#endif
      return;
    }

    char hdr[512];
    std::snprintf(hdr, sizeof(hdr), "[glint export] Writing semantic HTML to: %s\n", path.c_str());
#if defined(_WIN32)
    OutputDebugStringA(hdr);
#else
    fprintf(stderr, "%s", hdr);
#endif

    // ── Helpers ──────────────────────────────────────────────────────────

    // glint_color (A,R,G,B) → "rgba(R,G,B,a)" where a is 0..1
    auto icolorToCss = [](const glint_color& c) -> std::string {
      char tmp[64];
      std::snprintf(tmp, sizeof(tmp), "rgba(%d,%d,%d,%.4f)",
        c.R, c.G, c.B, c.A / 255.f);
      return tmp;
    };

    // glint_length::raw → CSS value string.
    // Bare numbers (no unit) get "px" appended.  Empty/unset → "".
    auto lenToCss = [](const glint_length& l) -> std::string {
      const std::string& r = l.raw;
      if (r.empty()) return "";
      if (r == "0")  return "0";
      if (r == "auto") return "auto";
      // Already has a unit ('%', 'px', 'em', 'rem', 'vh', 'vw' …)
      if (!std::isdigit(static_cast<unsigned char>(r.back()))) return r;
      // Bare numeric string → append px
      return r + "px";
    };

    // sk_side_proxy → CSS value string (percentage raw or float px fallback).
    auto sideToCss = [](const sk_side_proxy& sp) -> std::string {
      if (sp._rawp && !sp._rawp->empty())
      {
        const std::string& r = *sp._rawp;
        if (!std::isdigit(static_cast<unsigned char>(r.back()))) return r; // has unit/% already
        return r + "px"; // bare number
      }
      if (!sp._p || *sp._p == 0.f) return "";
      char tmp[32];
      std::snprintf(tmp, sizeof(tmp), "%.4gpx", *sp._p);
      return tmp;
    };

    auto percentEncodeUri = [](const std::string& raw) -> std::string {
      static const char kHex[] = "0123456789ABCDEF";
      std::string out;
      out.reserve(raw.size() + 16);
      for (unsigned char c : raw)
      {
        const bool safe = std::isalnum(c) || c == '/' || c == ':' || c == '-' ||
                          c == '_' || c == '.' || c == '~';
        if (safe)
        {
          out += static_cast<char>(c);
        }
        else
        {
          out += '%';
          out += kHex[(c >> 4) & 0xF];
          out += kHex[c & 0xF];
        }
      }
      return out;
    };

    auto trimQuoted = [](std::string s) -> std::string {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
      if (s.size() >= 2)
      {
        const char q0 = s.front();
        const char q1 = s.back();
        if ((q0 == '"' && q1 == '"') || (q0 == '\'' && q1 == '\''))
          return s.substr(1, s.size() - 2);
      }
      return s;
    };

    auto resolveAbsolutePath = [&](const std::string& path_) -> std::filesystem::path {
      const std::string raw = trimQuoted(path_);
      if (raw.empty() || raw.front() == '#') return {};

      if (raw.rfind("file://", 0) == 0)
      {
        std::string pathPart = raw.substr(7);
        while (pathPart.size() >= 3 && pathPart[0] == '/' && pathPart[1] == '/' && pathPart[2] == '/')
          pathPart.erase(pathPart.begin());
        if (!pathPart.empty() && pathPart[0] == '/' && pathPart.size() >= 3 && pathPart[2] == ':')
          pathPart.erase(pathPart.begin());
        return std::filesystem::path(pathPart).make_preferred().lexically_normal();
      }

      if (raw.find("://") != std::string::npos || raw.rfind("data:", 0) == 0)
        return {};

      // First attempt: route through the host application's onRequest handler
      // exactly like the runtime does.  This is the only way to honour custom
      // asset roots like glint_user_code/web/, which the engine has no
      // built-in knowledge of.  glint_assets_path() (used as fallback below)
      // only finds a sibling "glint_assets" directory in the source tree and
      // returns an empty / cwd-relative path when no such directory exists,
      // which would point exported HTML at the wrong location for fonts and
      // images.
      if (mMainRoot && mMainRoot->onRequest)
      {
        glint_resource_request req;
        req.url = raw;
        req.parseUrl();
        mMainRoot->onRequest(req);
        if (!req.resolvedFilePath.empty())
          return std::filesystem::path(req.resolvedFilePath)
                   .make_preferred().lexically_normal();
      }

      std::filesystem::path p(raw);
      if (p.is_absolute())
        return p.make_preferred().lexically_normal();

      if (!raw.empty() && (raw.front() == '/' || raw.front() == '\\'))
        return glint_assets_path(raw).make_preferred().lexically_normal();

      const std::filesystem::path assetPath = glint_assets_path(raw);
      std::error_code existsEc;
      if (!glint_assets_dir().empty() && std::filesystem::exists(assetPath, existsEc))
      {
        auto out = assetPath;
        return out.make_preferred().lexically_normal();
      }

      std::error_code absEc;
      return std::filesystem::absolute(p, absEc).make_preferred().lexically_normal();
    };

    auto fileUri = [&](const std::string& path_) -> std::string {
      const std::string raw = trimQuoted(path_);
      if (raw.empty()) return {};
      if (raw.front() == '#') return raw;

      auto toFileUri = [&](std::filesystem::path p) -> std::string {
        std::error_code ec;
        p = p.make_preferred().lexically_normal();
        if (!p.is_absolute())
          p = std::filesystem::absolute(p, ec);
        std::string uriPath = p.generic_string();
        if (uriPath.size() >= 2 && std::isalpha(static_cast<unsigned char>(uriPath[0])) && uriPath[1] == ':')
          uriPath.insert(uriPath.begin(), '/');
        return "file://" + percentEncodeUri(uriPath);
      };

      if (raw.rfind("file://", 0) == 0)
      {
        std::string pathPart = raw.substr(7);
        while (pathPart.size() >= 3 && pathPart[0] == '/' && pathPart[1] == '/' && pathPart[2] == '/')
          pathPart.erase(pathPart.begin());
        if (!pathPart.empty() && pathPart[0] == '/' && pathPart.size() >= 3 && pathPart[2] == ':')
          pathPart.erase(pathPart.begin());
        return toFileUri(std::filesystem::path(pathPart));
      }

      if (raw.find("://") != std::string::npos || raw.rfind("data:", 0) == 0)
        return raw;

      const std::filesystem::path resolved = resolveAbsolutePath(raw);
      return resolved.empty() ? std::string{} : toFileUri(resolved);
    };

    auto absolutizeCssUrls = [&](const std::string& cssValue) -> std::string {
      if (cssValue.empty()) return {};
      std::string out;
      size_t pos = 0;
      while (pos < cssValue.size())
      {
        const size_t urlPos = cssValue.find("url(", pos);
        if (urlPos == std::string::npos)
        {
          out += cssValue.substr(pos);
          break;
        }

        out += cssValue.substr(pos, urlPos - pos);

        size_t cur = urlPos + 4;
        while (cur < cssValue.size() && std::isspace(static_cast<unsigned char>(cssValue[cur]))) ++cur;

        char quote = 0;
        if (cur < cssValue.size() && (cssValue[cur] == '"' || cssValue[cur] == '\''))
          quote = cssValue[cur++];

        const size_t valueBeg = cur;
        size_t valueEnd = std::string::npos;
        size_t closePos = std::string::npos;

        if (quote)
        {
          valueEnd = cssValue.find(quote, valueBeg);
          if (valueEnd == std::string::npos) { out += cssValue.substr(urlPos); break; }
          closePos = cssValue.find(')', valueEnd + 1);
          if (closePos == std::string::npos) { out += cssValue.substr(urlPos); break; }
        }
        else
        {
          closePos = cssValue.find(')', valueBeg);
          if (closePos == std::string::npos) { out += cssValue.substr(urlPos); break; }
          valueEnd = closePos;
          while (valueEnd > valueBeg && std::isspace(static_cast<unsigned char>(cssValue[valueEnd - 1]))) --valueEnd;
        }

        const std::string rawUrl = cssValue.substr(valueBeg, valueEnd - valueBeg);
        const std::string resolved = fileUri(rawUrl);
        out += "url(\"" + (resolved.empty() ? trimQuoted(rawUrl) : resolved) + "\")";
        pos = closePos + 1;
      }
      return out;
    };

    auto buildBackgroundFill = [&](const glint_style& s) -> std::string {
      if (!s.backgroundGradient.empty())
      {
        std::string grad;
        if (s.backgroundGradientType == "radial")
        {
          char tmp[256];
          std::snprintf(tmp, sizeof(tmp), "radial-gradient(circle %.1f%% at %.1f%% %.1f%%",
            s.backgroundGradientRadius * 100.f,
            s.backgroundGradientCX    * 100.f,
            s.backgroundGradientCY    * 100.f);
          grad = tmp;
        }
        else
        {
          if (!s.backgroundGradientDirection.empty())
          {
            grad = "linear-gradient(" + s.backgroundGradientDirection;
          }
          else
          {
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), "linear-gradient(%.1fdeg", s.backgroundGradientAngle);
            grad = tmp;
          }
        }
        for (const auto& stop : s.backgroundGradient)
        {
          char pct[64];
          std::snprintf(pct, sizeof(pct), " %.2f%%", stop.position * 100.f);
          grad += ", " + icolorToCss(stop.color) + pct;
        }
        grad += ")";
        return grad;
      }
      if (s.backgroundColor.value.A == 0) return "transparent";
      return icolorToCss(s.backgroundColor.value);
    };

    auto buildBackgroundImage = [&](const glint_style& s) -> std::string {
      if (!s.backgroundImage.empty())
      {
        const std::string uri = fileUri(s.backgroundImage);
        if (!uri.empty())
          return "url(\"" + uri + "\")";
      }
      return {};
    };

    auto escapeHtml = [](const std::string& s) -> std::string {
      std::string out;
      out.reserve(s.size() + 16);
      for (char c : s)
      {
        switch (c)
        {
          case '&':  out += "&amp;";  break;
          case '<':  out += "&lt;";   break;
          case '>':  out += "&gt;";   break;
          case '"': out += "&quot;"; break;
          default:   out += c;         break;
        }
      }
      return out;
    };

    auto escapeAttr = [&](const std::string& s) -> std::string {
      std::string out = escapeHtml(s);
      for (char& c : out) if (c == '\n' || c == '\r') c = ' ';
      return out;
    };

    auto serializeCssDeclaration = [&absolutizeCssUrls](const GlintCssDeclaration& decl) -> std::string {
      if (decl.disabled || decl.property.empty()) return {};
      // Rewrite root-relative url(/foo/bar.ttf) and other relative URLs to
      // absolute file:// URIs so the exported HTML can resolve assets when
      // opened from disk. Without this, @font-face src urls like
      // "/fonts/roboto/Roboto-Regular.ttf" resolve to file:///fonts/... in
      // the browser (filesystem root), fonts fail to load, and pages fall
      // back to the default sans-serif. Same logic applies to background
      // images and other url(...) references inside CSS rules.
      const std::string val = absolutizeCssUrls(decl.value);
      std::string out = decl.property + ':' + (val.empty() ? decl.value : val);
      if (decl.important) out += " !important";
      out += ';';
      return out;
    };

    std::function<std::string(const GlintCssQualifiedRule&)> serializeCssQualifiedRule;
    std::function<std::string(const GlintCssAtRule&)>        serializeCssAtRule;

    serializeCssQualifiedRule = [&](const GlintCssQualifiedRule& rule) -> std::string {
      std::string body;
      for (const auto& decl : rule.declarations)
        body += serializeCssDeclaration(decl);
      if (body.empty()) return {};

      const std::string selector = !rule.prelude.empty()
        ? rule.prelude
        : [&]() {
            std::string out;
            for (const auto& tok : rule.prelToks)
            {
              switch (tok.type)
              {
                case GlintCssTokenType::WHITESPACE: out += ' '; break;
                case GlintCssTokenType::STRING:     out += '"' + tok.value + '"'; break;
                case GlintCssTokenType::IDENT:
                case GlintCssTokenType::DELIM:
                case GlintCssTokenType::AT_KEYWORD: out += tok.value; break;
                case GlintCssTokenType::FUNCTION:   out += tok.value + '('; break;
                case GlintCssTokenType::HASH:       out += '#' + tok.value; break;
                case GlintCssTokenType::COLON:      out += ':'; break;
                case GlintCssTokenType::SEMICOLON:  out += ';'; break;
                case GlintCssTokenType::COMMA:      out += ','; break;
                case GlintCssTokenType::OPEN_PAREN: out += '('; break;
                case GlintCssTokenType::CLOSE_PAREN: out += ')'; break;
                case GlintCssTokenType::OPEN_SQUARE: out += '['; break;
                case GlintCssTokenType::CLOSE_SQUARE: out += ']'; break;
                case GlintCssTokenType::OPEN_CURLY: out += '{'; break;
                case GlintCssTokenType::CLOSE_CURLY: out += '}'; break;
                case GlintCssTokenType::COMMENT:    out += "/*" + tok.value + "*/"; break;
                case GlintCssTokenType::NUMBER:     out += tok.value; break;
                case GlintCssTokenType::PERCENTAGE:
                {
                  char buf[32];
                  std::snprintf(buf, sizeof(buf), "%g", tok.numericValue);
                  out += buf;
                  out += '%';
                  break;
                }
                case GlintCssTokenType::DIMENSION:
                {
                  char buf[32];
                  std::snprintf(buf, sizeof(buf), "%g", tok.numericValue);
                  out += buf;
                  out += tok.value;
                  break;
                }
                case GlintCssTokenType::URL:        out += "url(" + tok.value + ')'; break;
                default: break;
              }
            }
            return out;
          }();

      if (selector.empty()) return {};
      return selector + '{' + body + '}';
    };

    serializeCssAtRule = [&](const GlintCssAtRule& rule) -> std::string {
      std::string out = "@" + rule.name;
      if (!rule.prelude.empty()) out += ' ' + rule.prelude;

      std::string body;
      if (!rule.declarations.empty())
      {
        for (const auto& decl : rule.declarations)
          body += serializeCssDeclaration(decl);
      }
      else if (!rule.children.empty())
      {
        for (const auto& child : rule.children)
        {
          if (child.kind == GlintCssAtRule::ChildRule::Kind::QUALIFIED && child.qualified)
            body += serializeCssQualifiedRule(*child.qualified);
          else if (child.kind == GlintCssAtRule::ChildRule::Kind::AT && child.atRule)
            body += serializeCssAtRule(*child.atRule);
        }
      }

      if (body.empty())
      {
        out += ';';
        return out;
      }

      out += '{';
      out += body;
      out += '}';
      return out;
    };

    auto serializeStylesheet = [&](const GlintCssStylesheet& sheet) -> std::string {
      std::string out;
      for (const auto& rule : sheet.rules)
      {
        if (rule.kind == GlintCssStylesheet::Rule::Kind::QUALIFIED && rule.qualified)
          out += serializeCssQualifiedRule(*rule.qualified);
        else if (rule.kind == GlintCssStylesheet::Rule::Kind::AT && rule.atRule)
          out += serializeCssAtRule(*rule.atRule);
        out += '\n';
      }
      return out;
    };

    auto exportText = [](glint_element* node) -> std::string {
      return node ? node->innerText : std::string{};
    };

    // ── Canvas viewport size (px) — the one hard pixel anchor ────────────
    const glint_rect rootRect = mMainRoot->mCanvas.GetPaintRECT();
    const float vw       = rootRect.W();
    const float vh       = rootRect.H();

    // ── HTML head ────────────────────────────────────────────────────────
    f << R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>glint Export</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { background: #0d0d0d; display: flex; flex-direction: column;
               align-items: center; padding: 32px;
               font-family: 'Roboto', sans-serif; gap: 16px; }
  .glint-scrollable {
    scrollbar-width: var(--glint-scrollbar-width, auto);
    scrollbar-color: var(--glint-scrollbar-thumb, auto) var(--glint-scrollbar-track, auto);
  }
  .glint-scrollable::-webkit-scrollbar {
    width: var(--glint-scrollbar-size, 12px);
    height: var(--glint-scrollbar-size, 12px);
  }
  .glint-scrollable::-webkit-scrollbar-thumb {
    background: var(--glint-scrollbar-thumb, rgba(110,110,110,1));
    border: max(0px, calc(var(--glint-scrollbar-size, 12px) * 0.1667)) solid var(--glint-scrollbar-track, transparent);
    border-radius: 999px;
  }
  .glint-scrollable::-webkit-scrollbar-track,
  .glint-scrollable::-webkit-scrollbar-corner {
    background: var(--glint-scrollbar-track, rgba(40,40,40,1));
  }
  .glint-scrollable::-webkit-scrollbar-button {
    background: var(--glint-scrollbar-button, rgba(65,65,65,1));
  }
  .glint-toolbar { display: flex; gap: 8px; align-items: center; width: 100%;
                  justify-content: center; }
  .glint-btn { background: #1e2a3a; color: #7df; border: 1px solid #2a4a6a;
              padding: 7px 16px; border-radius: 6px; cursor: pointer;
              font-size: 13px; font-family: inherit; transition: background .15s; }
  .glint-btn:hover { background: #263545; }
</style>
)HTML";
    for (const auto& sheet : mMainRoot->stylesheets())
    {
      // Always inline the parsed stylesheet (instead of emitting a <link>
      // to the on-disk file) so url(...) references inside it have already
      // been rewritten to absolute file:// URIs by serializeCssDeclaration.
      // Linking the raw .css would leave root-relative urls (e.g.
      // "/fonts/...") resolving against the filesystem root in the browser
      // and silently break @font-face loading.
      const std::string cssText = serializeStylesheet(sheet);
      if (!cssText.empty())
      {
        f << "<style data-glint-source=\""
          << escapeAttr(sheet.sourceUrl)
          << "\">\n"
          << cssText
          << "</style>\n";
      }
    }
    f << R"HTML(<script>
function exportAbsoluteJSON() {
  var vp = document.getElementById('glint-viewport');
  var vpRect = vp.getBoundingClientRect();
  function r2(v) { return Math.round(v * 100) / 100; }
  function walkEl(el) {
    var r = el.getBoundingClientRect();
    var cs = window.getComputedStyle(el);
    var node = {
      tag: el.tagName.toLowerCase(),
      rect: { left:   r2(r.left   - vpRect.left),
               top:    r2(r.top    - vpRect.top),
               right:  r2(r.right  - vpRect.left),
               bottom: r2(r.bottom - vpRect.top),
               width:  r2(r.width),
               height: r2(r.height) },
      margin:  { top: r2(parseFloat(cs.marginTop)||0),
                 right: r2(parseFloat(cs.marginRight)||0),
                 bottom: r2(parseFloat(cs.marginBottom)||0),
                 left: r2(parseFloat(cs.marginLeft)||0) },
      padding: { top: r2(parseFloat(cs.paddingTop)||0),
                 right: r2(parseFloat(cs.paddingRight)||0),
                 bottom: r2(parseFloat(cs.paddingBottom)||0),
                 left: r2(parseFloat(cs.paddingLeft)||0) },
      border:  { top: r2(parseFloat(cs.borderTopWidth)||0),
                 right: r2(parseFloat(cs.borderRightWidth)||0),
                 bottom: r2(parseFloat(cs.borderBottomWidth)||0),
                 left: r2(parseFloat(cs.borderLeftWidth)||0) },
      children: []
    };
    if (el.id) node.id = el.id;
    for (var i = 0; i < el.children.length; i++)
      node.children.push(walkEl(el.children[i]));
    if (node.children.length === 0) delete node.children;
    return node;
  }
  var json = JSON.stringify(walkEl(vp), null, 2);
  var ts   = new Date().toISOString().replace(/[:.]/g,'-').slice(0,19);
  var blob = new Blob([json], { type: 'application/json' });
  var a    = document.createElement('a');
  a.href   = URL.createObjectURL(blob);
  a.download = 'glint_absolute_' + ts + '.json';
  document.body.appendChild(a); a.click(); document.body.removeChild(a);
}
</script>
</head>
<body>
<div class="glint-toolbar">
  <button class="glint-btn" onclick="exportAbsoluteJSON()">Export Absolute JSON</button>
</div>
)HTML";
    {
      char vpDiv[128];
      std::snprintf(vpDiv, sizeof(vpDiv),
        "<div id=\"glint-viewport\" style=\"position:relative;width:%.0fpx;height:%.0fpx;flex-shrink:0;overflow:hidden;\">\n",
        vw, vh);
      f << vpDiv;
    }

    // ── Recursive walk ────────────────────────────────────────────────────
    // Export authored DOM structure plus authored inline style only.
    // Class-based styling is carried by linked/inlined document stylesheets.
    std::function<void(glint_element*)> walk = [&](glint_element* node)
    {
      const glint_style& authored = node->style;
      const glint_style& computed = node->computedStyle;
      const glint_style  defaults;
      const glint_rect       paintRect = node->GetPaintRECT();

      const bool policyScrollY = (computed.overflowY == "auto" || computed.overflowY == "scroll");
      const bool policyScrollX = (computed.overflowX == "auto" || computed.overflowX == "scroll");
      const bool scrollableY   = policyScrollY &&
                                 (computed.overflowY == "scroll" || node->mScrollHeight > paintRect.H());
      const bool scrollableX   = policyScrollX &&
                                 (computed.overflowX == "scroll" || node->mScrollWidth > paintRect.W());

      std::string styleStr;
      styleStr.reserve(512);

      // ── Layout — from authored style ──────────────────────────────────

      // position
      if (!authored.position.empty())
        styleStr += "position:" + authored.position + ";";

      // width / height / constraints
      auto emitLen = [&](const char* prop, const glint_length& l) {
        const std::string v = lenToCss(l);
        if (!v.empty()) { styleStr += prop; styleStr += v; styleStr += ";"; }
      };
      emitLen("width:",      authored.width);
      emitLen("height:",     authored.height);
      emitLen("min-width:",  authored.minWidth);
      emitLen("max-width:",  authored.maxWidth);
      emitLen("min-height:", authored.minHeight);
      emitLen("max-height:", authored.maxHeight);

      // inset (left / top / right / bottom)
      emitLen("left:",   authored.left);
      emitLen("top:",    authored.top);
      emitLen("right:",  authored.right);
      emitLen("bottom:", authored.bottom);

      // margin — per side
      {
        auto emitSide = [&](const char* prop, const sk_side_proxy& sp) {
          const std::string v = sideToCss(sp);
          if (!v.empty()) { styleStr += prop; styleStr += v; styleStr += ";"; }
        };
        emitSide("margin-top:",    authored.marginTop);
        emitSide("margin-right:",  authored.marginRight);
        emitSide("margin-bottom:", authored.marginBottom);
        emitSide("margin-left:",   authored.marginLeft);
      }

      // padding — per side
      {
        auto emitSide = [&](const char* prop, const sk_side_proxy& sp) {
          const std::string v = sideToCss(sp);
          if (!v.empty()) { styleStr += prop; styleStr += v; styleStr += ";"; }
        };
        emitSide("padding-top:",    authored.paddingTop);
        emitSide("padding-right:",  authored.paddingRight);
        emitSide("padding-bottom:", authored.paddingBottom);
        emitSide("padding-left:",   authored.paddingLeft);
      }

      // display + flex properties
      if (!authored.display.empty())
      {
        styleStr += "display:" + authored.display + ";";
        if (authored.display == "flex" || authored.display == "inline-flex")
        {
          if (!authored.flexDirection.empty() && authored.flexDirection != "row")
            styleStr += "flex-direction:" + authored.flexDirection + ";";
          if (!authored.justifyContent.empty() && authored.justifyContent != "flex-start")
            styleStr += "justify-content:" + authored.justifyContent + ";";
          if (!authored.alignItems.empty() && authored.alignItems != "flex-start")
            styleStr += "align-items:" + authored.alignItems + ";";
        }
      }

      // gap
      {
        const std::string gv = lenToCss(authored.gap);
        if (!gv.empty()) { styleStr += "gap:"; styleStr += gv; styleStr += ";"; }
      }

      // flex-grow
      if (authored.flexGrow > 0.f)
      {
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "flex-grow:%.4g;min-width:0;min-height:0;", authored.flexGrow);
        styleStr += tmp;
      }

      if (authored.display == "flex" || authored.display == "inline-flex")
        styleStr += "min-width:0;min-height:0;";

      // ── Visual — from authored inline style ───────────────────────────

      // Background
      {
        const std::string bgFill = buildBackgroundFill(authored);
        const std::string bgImage = buildBackgroundImage(authored);
        if (!authored.backgroundImage.empty())
        {
          if (!authored.backgroundGradient.empty())
          {
            styleStr += "background-img:" + bgFill + "," + bgImage + ";";
            styleStr += "background-repeat:no-repeat," + authored.backgroundRepeat + ";";
            styleStr += "background-position:0% 0%," + authored.backgroundPosition + ";";
            styleStr += "background-size:auto," + authored.backgroundSize + ";";
            if (authored.backgroundColor.value.A > 0)
              styleStr += "background-color:" + icolorToCss(authored.backgroundColor.value) + ";";
          }
          else
          {
            if (authored.backgroundColor.value.A > 0)
              styleStr += "background-color:" + icolorToCss(authored.backgroundColor.value) + ";";
            styleStr += "background-img:" + bgImage + ";";
            if (!authored.backgroundSize.empty())
              styleStr += "background-size:" + authored.backgroundSize + ";";
            if (!authored.backgroundPosition.empty())
              styleStr += "background-position:" + authored.backgroundPosition + ";";
            if (!authored.backgroundRepeat.empty())
              styleStr += "background-repeat:" + authored.backgroundRepeat + ";";
          }
        }
        else if (!authored.backgroundGradient.empty() || authored.backgroundColor.value.A > 0)
        {
          // When both a gradient and a solid backgroundColor are present, emit them as
          // separate background-img + background-color properties.  The `background`
          // shorthand resets background-color to transparent, which destroys the base
          // layer that background-blend-mode blends the gradient against in the browser.
          if (!authored.backgroundGradient.empty() && authored.backgroundColor.value.A > 0)
          {
            styleStr += "background-img:" + bgFill + ";";
            styleStr += "background-color:" + icolorToCss(authored.backgroundColor.value) + ";";
          }
          else
          {
            styleStr += "background:" + bgFill + ";";
          }
        }

        if (authored.boxShadow.isSet)
          styleStr += "box-shadow:" + static_cast<std::string>(authored.boxShadow) + ";";
      }

      // Opacity
      if (authored.opacity.isSet)
      {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "opacity:%.4f;", authored.opacity.value);
        styleStr += tmp;
      }

      // Borders — per side
      {
        struct Side { const char* name; int idx; };
        const Side sides[4] = {
          { "top",    0 },
          { "right",  1 },
          { "bottom", 2 },
          { "left",   3 },
        };
        for (const auto& side : sides)
        {
          const float bw = authored.resolvedBorderWidth(side.idx);
          if (bw <= 0.f) continue;
          const std::string bc = icolorToCss(authored.resolvedBorderColor(side.idx));
          const std::string bs = authored.resolvedBorderStyle(side.idx).empty()
            ? (authored.borderStyle.empty() ? "solid" : authored.borderStyle)
            : authored.resolvedBorderStyle(side.idx);
          char tmp[160];
          std::snprintf(tmp, sizeof(tmp), "border-%s:%.4gpx %s %s;",
            side.name, bw, bs.c_str(), bc.c_str());
          styleStr += tmp;
        }
      }

      // Border radius
      {
        const std::string tl = lenToCss(authored.borderTopLeftRadius);
        const std::string tr = lenToCss(authored.borderTopRightRadius);
        const std::string br = lenToCss(authored.borderBottomRightRadius);
        const std::string bl = lenToCss(authored.borderBottomLeftRadius);
        if (!tl.empty() || !tr.empty() || !br.empty() || !bl.empty())
        {
          styleStr += "border-top-left-radius:" + (!tl.empty() ? tl : lenToCss(authored.borderRadius)) + ";";
          styleStr += "border-top-right-radius:" + (!tr.empty() ? tr : lenToCss(authored.borderRadius)) + ";";
          styleStr += "border-bottom-right-radius:" + (!br.empty() ? br : lenToCss(authored.borderRadius)) + ";";
          styleStr += "border-bottom-left-radius:" + (!bl.empty() ? bl : lenToCss(authored.borderRadius)) + ";";
        }
        else if (authored.borderRadius.toFloat() > 0.f)
        {
          styleStr += "border-radius:" + lenToCss(authored.borderRadius) + ";";
        }
      }

      // SVG-style fill / stroke
      if (authored.fill.isSet)
        styleStr += "fill:" + icolorToCss(authored.fill.value) + ";";
      if (authored.strokeColor.isSet)
        styleStr += "stroke:" + icolorToCss(authored.strokeColor.value) + ";";
      if (authored.strokeWidth > 0.f)
      {
        char tmp[96];
        std::snprintf(tmp, sizeof(tmp), "stroke-width:%.4gpx;stroke-miterlimit:%.4g;stroke-dashoffset:%.4g;stroke-opacity:%.4g;",
          authored.strokeWidth,
          authored.strokeMiterlimit,
          authored.strokeDashoffset,
          authored.strokeOpacity.isSet ? authored.strokeOpacity.value : 1.f);
        styleStr += tmp;
      }
      if (authored.strokeLinecap != defaults.strokeLinecap)
        styleStr += "stroke-linecap:" + authored.strokeLinecap + ";";
      if (authored.strokeLinejoin != defaults.strokeLinejoin)
        styleStr += "stroke-linejoin:" + authored.strokeLinejoin + ";";
      if (!authored.strokeDasharray.empty())
        styleStr += "stroke-dasharray:" + authored.strokeDasharray + ";";

      // Overflow
      {
        auto emitOverflowAxis = [&](const char* prop, const std::string& value) {
          if (value.empty() || value == "visible") return;
          if (value == "clip")
          {
            styleStr += prop;
            styleStr += "hidden;";
            return;
          }
          styleStr += prop;
          styleStr += value;
          styleStr += ';';
        };
        emitOverflowAxis("overflow-x:", authored.overflowX);
        emitOverflowAxis("overflow-y:", authored.overflowY);
      }

      if (!authored.pointerEvents.empty())
        styleStr += "pointer-events:" + authored.pointerEvents + ";";
      if (!authored.userSelect.empty())
      {
        styleStr += "user-select:" + authored.userSelect + ";";
        styleStr += "-webkit-user-select:" + authored.userSelect + ";";
      }
      if (!authored.whiteSpace.empty())
        styleStr += "white-space:" + authored.whiteSpace + ";";
      if (authored.zIndex != 0)
      {
        char tmp[48];
        std::snprintf(tmp, sizeof(tmp), "z-index:%d;", authored.zIndex);
        styleStr += tmp;
      }

      if (!authored.transform.empty() && authored.transform != "none")
        styleStr += "transform:" + authored.transform + ";";
      if (!authored.filter.empty() && authored.filter != "none")
        styleStr += "filter:" + std::string(authored.filter) + ";";
      if (!authored.backdropFilter.empty() && authored.backdropFilter != "none")
      {
        styleStr += "backdrop-filter:" + std::string(authored.backdropFilter) + ";";
        styleStr += "-webkit-backdrop-filter:" + std::string(authored.backdropFilter) + ";";
      }
      if (!authored.mixBlendMode.empty() && authored.mixBlendMode != "normal")
        styleStr += "mix-blend-mode:" + authored.mixBlendMode + ";";
      // Use computed (not just authored) so that a CSS-class blend mode is emitted
      // inline and survives the inline `background` shorthand resetting it per spec.
      {
        const std::string& bbm = computed.backgroundBlendMode.empty()
          ? authored.backgroundBlendMode
          : computed.backgroundBlendMode;
        if (!bbm.empty() && bbm != "normal")
          styleStr += "background-blend-mode:" + bbm + ";";
      }
      if (authored.isolation == "isolate")
        styleStr += "isolation:isolate;";

      if (!authored.mask.empty() && authored.mask != "none")
      {
        const std::string maskImage = absolutizeCssUrls(authored.mask);
        styleStr += "mask-img:" + maskImage + ";";
        styleStr += "-webkit-mask-img:" + maskImage + ";";
        if (!authored.maskMode.empty())      styleStr += "mask-mode:" + authored.maskMode + ";";
        if (!authored.maskPosition.empty())
        {
          styleStr += "mask-position:" + authored.maskPosition + ";";
          styleStr += "-webkit-mask-position:" + authored.maskPosition + ";";
        }
        if (!authored.maskSize.empty())
        {
          styleStr += "mask-size:" + authored.maskSize + ";";
          styleStr += "-webkit-mask-size:" + authored.maskSize + ";";
        }
        if (!authored.maskRepeat.empty())
        {
          styleStr += "mask-repeat:" + authored.maskRepeat + ";";
          styleStr += "-webkit-mask-repeat:" + authored.maskRepeat + ";";
        }
        if (!authored.maskOrigin.empty())
        {
          styleStr += "mask-origin:" + authored.maskOrigin + ";";
          styleStr += "-webkit-mask-origin:" + authored.maskOrigin + ";";
        }
        if (!authored.maskClip.empty())
        {
          styleStr += "mask-clip:" + authored.maskClip + ";";
          styleStr += "-webkit-mask-clip:" + authored.maskClip + ";";
        }
        if (!authored.maskComposite.empty()) styleStr += "mask-composite:" + authored.maskComposite + ";";
      }

      // Text color
      if (authored.color.value.A > 0)
        styleStr += "color:" + icolorToCss(authored.color.value) + ";";

      // Font / text
      {
        const std::string fsz = lenToCss(authored.fontSize);
        if (!fsz.empty()) { styleStr += "font-size:"; styleStr += fsz; styleStr += ";"; }
        if (authored.lineHeight > 0.f)
        {
          char tmp[48];
          std::snprintf(tmp, sizeof(tmp), "line-height:%.4g;", authored.lineHeight);
          styleStr += tmp;
        }
        if (!authored.fontFamily.empty())
          styleStr += "font-family:" + authored.fontFamily + ";";
        if (!authored.fontStyle.empty())
          styleStr += "font-style:" + authored.fontStyle + ";";
        if (authored.fontWeight.isSet)
        {
          char tmp[48];
          std::snprintf(tmp, sizeof(tmp), "font-weight:%.4g;", authored.fontWeight.value);
          styleStr += tmp;
        }
        if (authored.textAlign != defaults.textAlign)
        {
          switch (authored.textAlign)
          {
            case EAlign::Center: styleStr += "text-align:center;"; break;
            case EAlign::Far:    styleStr += "text-align:right;"; break;
            default:             styleStr += "text-align:left;"; break;
          }
        }
        if (authored.verticalAlign != defaults.verticalAlign)
          styleStr += "vertical-align:" + authored.verticalAlign + ";";
        if (!authored.textDecoration.empty())
          styleStr += "text-decoration:" + authored.textDecoration + ";";
      }

        if (scrollableX || scrollableY)
        {
          char tmp[96];
          std::snprintf(tmp, sizeof(tmp), "width:%.4gpx;height:%.4gpx;min-width:0;min-height:0;",
            paintRect.W(), paintRect.H());
          styleStr += tmp;

          char sbTmp[320];
          const std::string sbThumb  = icolorToCss(computed.scrollbarThumbColor.value);
          const std::string sbTrack  = icolorToCss(computed.scrollbarTrackColor.value);
          const std::string sbButton = icolorToCss(computed.scrollbarButtonColor.value);
          std::snprintf(sbTmp, sizeof(sbTmp),
            "--glint-scrollbar-size:%.4gpx;--glint-scrollbar-width:%s;--glint-scrollbar-thumb:%s;--glint-scrollbar-track:%s;--glint-scrollbar-button:%s;",
            computed.scrollbarWidth,
            computed.scrollbarWidth <= 0.f ? "none" : "auto",
            sbThumb.c_str(),
            sbTrack.c_str(),
            sbButton.c_str());
          styleStr += sbTmp;
        }

      // ── Determine HTML tag and extra attributes ───────────────────────
      const char* tn = node->typeName() ? node->typeName() : "div";
      const char* tg = node->tagName()  ? node->tagName()  : "div";
      std::string tag = tg;
      std::string extraAttribs;
      std::string classNames = node->className;
      const std::string textContent = exportText(node);
      std::string rawHtmlContent;

      if (tag.empty()) tag = "div";

      if (tag != "img" && std::strcmp(tn, "img") == 0)
      {
        tag = "img";
        auto* imgNode = dynamic_cast<glint_image*>(node);
        if (imgNode && !imgNode->src.empty())
          extraAttribs += " src=\"" + escapeAttr(fileUri(imgNode->src)) + "\"";
        if (!authored.objectFit.empty())
          styleStr += "object-fit:" + authored.objectFit + ";";
        if (!authored.objectPosition.empty())
          styleStr += "object-position:" + authored.objectPosition + ";";
      }
      else if (tag != "img" && std::strcmp(tn, "input") == 0)
      {
        tag = "input";
        std::string inputType = "text";
        std::string inputValue;
        if (auto* inputNode = dynamic_cast<glint_input*>(node))
        {
          if (!inputNode->type.empty()) inputType = inputNode->type;
          if (!inputNode->placeholder.empty())
            extraAttribs += " placeholder=\"" + escapeAttr(inputNode->placeholder) + "\"";
          if (inputNode->readonly)
            extraAttribs += " readonly";
          if (inputNode->disabled)
            extraAttribs += " disabled";
          if (inputType == "number" || inputType == "range")
          {
            if (inputNode->min != std::numeric_limits<float>::lowest())
            {
              char tmp[48];
              std::snprintf(tmp, sizeof(tmp), "%.4g", inputNode->min);
              extraAttribs += " min=\"" + std::string(tmp) + "\"";
            }
            if (inputNode->max != std::numeric_limits<float>::max())
            {
              char tmp[48];
              std::snprintf(tmp, sizeof(tmp), "%.4g", inputNode->max);
              extraAttribs += " max=\"" + std::string(tmp) + "\"";
            }
          }
          inputValue = inputNode->getValue();
        }
        extraAttribs += " type=\"" + escapeAttr(inputType) + "\"";
        const std::string valToExport = !inputValue.empty() ? inputValue : textContent;
        if (!valToExport.empty())
          extraAttribs += " value=\"" + escapeAttr(valToExport) + "\"";
      }

      if (tag == "ul") styleStr += "list-style:none;margin:0;padding:0;";
      if (tag == "li") styleStr += "list-style:none;";

      if (scrollableX || scrollableY)
      {
        if (!classNames.empty()) classNames += ' ';
        classNames += "glint-scrollable";
      }

      if (!node->element.id.empty())
      {
        extraAttribs += " id=\"" + escapeAttr(node->element.id) + "\"";
      }

      if (!classNames.empty())
        extraAttribs += " class=\"" + escapeAttr(classNames) + "\"";

      // ── Emit ─────────────────────────────────────────────────────────
      f << "<" << tag << extraAttribs;
      if (!styleStr.empty())
        f << " style=\"" << escapeAttr(styleStr) << "\"";
      f << ">";

      // Text content
      if (!rawHtmlContent.empty())
        f << rawHtmlContent;
      else if (tag != "img" && tag != "input" && !textContent.empty())
        f << escapeHtml(textContent);

      // Children
      if (tag != "img" && tag != "input" && rawHtmlContent.empty())
      {
        for (auto& child : node->mChildren)
          walk(child.get());
        f << "</" << tag << ">\n";
      }
      else
      {
        f << "\n";
      }
    };

    walk(&mMainRoot->mCanvas);

    f << "</div>\n</body>\n</html>\n"; // close #glint-viewport, body, html
    f.close();

#if defined(_WIN32)
    ::ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    glint_window_mac::openFileInDefaultApp(path);
#endif

    char done[512];
    std::snprintf(done, sizeof(done), "[glint export] Semantic HTML wrote: %s\n", path.c_str());
#if defined(_WIN32)
    OutputDebugStringA(done);
#else
    fprintf(stderr, "%s", done);
#endif
  }

  // Enable or disable crosshair / element-picker mode.
  // Safe to call from any code that runs on the inspector thread
  // (onCreated, handleMessage, button onClick).
  void applyInspectMode(bool newMode)
  {
    glint_debug::inspectMode.store(newMode);
    if (!newMode)
    {
      // Leaving inspect mode � clear the transient hover highlight.
      glint_debug::hoveredNode.store(nullptr);
      if (mMainRoot) mMainRoot->setDirty(false);
    }
    if (mInspectBtn)
    {
      mInspectBtn->toggled = newMode;
      mInspectBtn->setDirty(false);
    }
#if defined(_WIN32)
    if (HWND h = mHWNDAtom.load())
      ::InvalidateRect(h, nullptr, FALSE);
#elif defined(__APPLE__)
    requestRedraw();
#endif
  }

  void applyRealtimeMode(bool newMode)
  {
    mRealtimeMode = newMode;
    if (mRealtimeBtn)
    {
      mRealtimeBtn->style.backgroundColor   = newMode ? glint_color(255, 58, 123, 255) : glint_color(255, 44, 44, 44);
      mRealtimeBtn->style.color             = newMode ? glint_color(255, 255, 255, 255) : glint_color(255, 187, 187, 187);
      mRealtimeBtn->style.borderColor       = newMode ? glint_color(255, 140, 180, 255) : glint_color(255, 72, 72, 72);
      mRealtimeBtn->hover.backgroundColor   = newMode ? glint_color(255, 90, 150, 255) : glint_color(255, 58, 58, 58);
      mRealtimeBtn->hover.color             = glint_color(255, 255, 255, 255);
      mRealtimeBtn->hover.borderColor       = newMode ? glint_color(255, 160, 195, 255) : glint_color(255, 102, 102, 102);
      mRealtimeBtn->pressed.backgroundColor = newMode ? glint_color(255, 80, 130, 255) : glint_color(255, 74, 74, 74);
      mRealtimeBtn->pressed.color           = glint_color(255, 255, 255, 255);
      mRealtimeBtn->pressed.borderColor     = newMode ? glint_color(255, 140, 180, 255) : glint_color(255, 119, 119, 119);
      mRealtimeBtn->setDirty(false);
    }

#if defined(_WIN32)
    if (mHWND)
      ::SetTimer(mHWND, WM_INSP_TIMER_ID,
                 newMode ? WM_INSP_TIMER_MS_REALTIME : WM_INSP_TIMER_MS_NORMAL,
                 nullptr);
#elif defined(__APPLE__)
    setTimer(WM_INSP_TIMER_ID,
             (newMode ? WM_INSP_TIMER_MS_REALTIME : WM_INSP_TIMER_MS_NORMAL) / 1000.0);
#endif
  }

  bool canRemoveSelectedNode() const
  {
    return mMainRoot && mSelectedComp && mSelectedNodeId != 0
           && mSelectedComp != &mMainRoot->mCanvas;
  }

  void updateRemoveNodeButtonState()
  {
    if (!mRemoveNodeBtn) return;
    mRemoveNodeBtn->disabled = !canRemoveSelectedNode();
    mRemoveNodeBtn->setDirty(false);
  }

  void clearSelectionState(bool clearTreeSelection)
  {
    mSelectedComp   = nullptr;
    mSelectedNodeId = 0;
    glint_debug::hoveredNode.store(nullptr);
    glint_debug::inspectedNode.store(nullptr);
    if (mStylePanel)    mStylePanel->clear();
    if (mComputedPanel) mComputedPanel->clear();
    if (clearTreeSelection && mTree) mTree->selectById(0);
    updateRemoveNodeButtonState();
  }

  void refreshActiveSidebarPanel(glint_element* comp)
  {
    if (!comp) return;

    if (mActiveRightTab == 0)
    {
      if (mStylePanel) mStylePanel->show(comp);
      return;
    }

    if (mComputedPanel) mComputedPanel->show(comp);
  }

  void selectInspectorNodeById(uint64_t id)
  {
    if (!mMainRoot || id == 0)
    {
      clearSelectionState(true);
      return;
    }

    if (auto* comp = mMainRoot->getNodeById(id))
    {
      mSelectedComp   = comp;
      mSelectedNodeId = id;
      glint_debug::inspectedNode.store(comp);
      if (mTree && mTree->selectedId() != id) mTree->selectById(id);
      refreshActiveSidebarPanel(comp);
      if (mMainRoot) mMainRoot->setDirty(false);
      updateRemoveNodeButtonState();
      return;
    }

    clearSelectionState(true);
  }

  void removeSelectedInspectorNode()
  {
    if (!canRemoveSelectedNode()) return;

    const uint64_t removedId = mSelectedNodeId;
    uint64_t fallbackId = 0;
    if (!mMainRoot->debugRemoveNodeById(removedId, &fallbackId)) return;

    mRemovedNodeUndoStack.push_back(removedId);

    applyInspectMode(false);
    if (mPreviewPopup)
    {
#if defined(_WIN32)
      ::KillTimer(mHWND, WM_INSP_PREVIEW_SHOW_TIMER);
      ::KillTimer(mHWND, WM_INSP_PREVIEW_HIDE_TIMER);
#elif defined(__APPLE__)
      killTimer(WM_INSP_PREVIEW_SHOW_TIMER);
      killTimer(WM_INSP_PREVIEW_HIDE_TIMER);
#endif
      mPreviewPopup->dismiss();
    }

    mSelectedNodeId = fallbackId;
    refreshTree();
  }

  void undoLastRemovedInspectorNode()
  {
    if (!mMainRoot) return;

    while (!mRemovedNodeUndoStack.empty())
    {
      const uint64_t restoreId = mRemovedNodeUndoStack.back();
      mRemovedNodeUndoStack.pop_back();
      if (!mMainRoot->debugRestoreNodeById(restoreId)) continue;

      mSelectedNodeId = restoreId;
      refreshTree();
      return;
    }
  }

  // Shared timer handler — called from handleMessage(WM_TIMER) on Win32
  // and from onTimerFired() on macOS.
  void handleTimerFired(int timerId)
  {
    if (timerId == static_cast<int>(WM_INSP_PREVIEW_SHOW_TIMER))
    {
#if defined(_WIN32)
      ::KillTimer(mHWND, WM_INSP_PREVIEW_SHOW_TIMER);
#elif defined(__APPLE__)
      killTimer(WM_INSP_PREVIEW_SHOW_TIMER);
#endif
      if (mPreviewPopup)
      {
        mPreviewPopup->commitShow();
        if (mOwnRoot) mOwnRoot->setDirty(false);
#if defined(_WIN32)
        ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
        requestRedraw();
#endif
      }
      return;
    }
    if (timerId == static_cast<int>(WM_INSP_PREVIEW_HIDE_TIMER))
    {
#if defined(_WIN32)
      ::KillTimer(mHWND, WM_INSP_PREVIEW_HIDE_TIMER);
#elif defined(__APPLE__)
      killTimer(WM_INSP_PREVIEW_HIDE_TIMER);
#endif
      if (mPreviewPopup)
      {
        mPreviewPopup->commitHide();
        if (mOwnRoot) mOwnRoot->setDirty(false);
#if defined(_WIN32)
        ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
        requestRedraw();
#endif
      }
      return;
    }
    if (timerId == static_cast<int>(WM_INSP_TIMER_ID) && mMainRoot)
    {
      bool dirty = false;

      // Rendering tab stats (only when that tab is visible)
      if (mActiveTab == 1)
      {
        char buf[48];
        if (mFpsLabel) {
          snprintf(buf, sizeof(buf), "%.1f fps", mMainRoot->getFPS());
          mFpsLabel->innerText = buf;
        }
        if (mFrameTimeLabel) {
          snprintf(buf, sizeof(buf), "%.1f ms", mMainRoot->getFrameTimeMs());
          mFrameTimeLabel->innerText = buf;
        }
        if (mDrawCountLabel) {
          snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(mMainRoot->getDrawCount()));
          mDrawCountLabel->innerText = buf;
        }
        dirty = true;
      }

      // Network tab live refresh (only when visible).
      if (mActiveTab == 2)
      {
        refreshNetworkTab();
        dirty = true;
      }

      // Realtime style-panel polling.
      if (mActiveTab == 0 && mRealtimeMode && mActiveRightTab == 0 &&
          mStylePanel && mSelectedComp && mStylePanel->canLiveRefresh())
      {
        mStylePanel->liveRefresh(mSelectedComp);
        dirty = true;
      }

      // Computed panel live refresh.
      if (mActiveRightTab == 1 && mComputedPanel && mSelectedComp)
      {
        mComputedPanel->liveRefresh(mSelectedComp, mRealtimeMode);
        dirty = true;
      }

      if (dirty)
      {
        if (mOwnRoot) mOwnRoot->setDirty(false);
#if defined(_WIN32)
        ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
        requestRedraw();
#endif
      }
    }
  }

#if defined(__APPLE__)
  void onTimerFired(int id) override { handleTimerFired(id); }
#endif

#if defined(_WIN32)
  LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp) override
  {
    switch (msg)
    {
    case WM_INSP_TREE_CHANGED:
      refreshTree();
      return 0;
    case WM_INSP_STYLE_CHANGED:
      refreshStyle(unpackId(wp, lp));
      return 0;
    case WM_INSP_HOVER_CHANGED:
      if (mTree) mTree->hoverById(unpackId(wp, lp));
      ::InvalidateRect(mHWND, nullptr, FALSE);
      return 0;
    case WM_INSP_SELECT_CHANGED:
    {
      selectInspectorNodeById(unpackId(wp, lp));
      applyInspectMode(false);   // exit inspect mode after element was picked
      // Dismiss any open preview popup (selected element changed).
      if (mPreviewPopup && mHWND)
      {
        ::KillTimer(mHWND, WM_INSP_PREVIEW_SHOW_TIMER);
        ::KillTimer(mHWND, WM_INSP_PREVIEW_HIDE_TIMER);
        mPreviewPopup->dismiss();
      }
      ::InvalidateRect(mHWND, nullptr, FALSE);
      return 0;
    }
    case WM_INSP_ENABLE_INSPECT:
      applyInspectMode(true);
      return 0;
    case WM_TIMER:
      handleTimerFired(static_cast<int>(wp));
      return 0;
    case WM_INSP_CP_CHANGED:
    {
      const uint32_t rgba = static_cast<uint32_t>(wp);
      glint_color c;
      c.A = (rgba >> 24) & 0xFF;  c.R = (rgba >> 16) & 0xFF;
      c.G = (rgba >>  8) & 0xFF;  c.B =  rgba        & 0xFF;
      if (mStylePanel) mStylePanel->updateActiveSwatch(c);
      ::InvalidateRect(mHWND, nullptr, FALSE);
      return 0;
    }
    case WM_INSP_CP_CLOSED:
      if (mStylePanel) mStylePanel->onPickerClosed(static_cast<int>(lp));
      ::InvalidateRect(mHWND, nullptr, FALSE);
      return 0;
    case WM_INSP_ATTR_PICKED:
    {
      // lp = heap-allocated std::string* posted by glint_attributes_list_window::_pick()
      auto* key = reinterpret_cast<std::string*>(lp);
      if (mStylePanel) mStylePanel->commitAddProperty(*key);
      delete key;
      ::InvalidateRect(mHWND, nullptr, FALSE);
      return 0;
    }
    default:
      return -1;
    }
  }

#endif  // defined(_WIN32)

  void onKeyDown(const glint_key_press& kp) override
  {
    // Ctrl+C (Cmd+C on macOS) — copy selected element's innerText when no
    // text field in the inspector owns keyboard focus.
    if (kp.ctrl && !kp.alt && !kp.shift && kp.vk == 'C')
    {
      glint_element* focused = mOwnRoot ? mOwnRoot->getFocusedNode() : nullptr;
      if (!mOwnRoot || !focused || !focused->consumesCtrlA())
      {
        if (mSelectedComp)
        {
          const std::string text = mSelectedComp->innerText;
#if defined(_WIN32)
          if (!text.empty() && ::OpenClipboard(nullptr))
          {
            ::EmptyClipboard();
            const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            if (wlen > 0)
            {
              HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(WCHAR));
              if (hg) {
                WCHAR* p = static_cast<WCHAR*>(::GlobalLock(hg));
                if (p) { ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p, wlen); ::GlobalUnlock(hg); }
                ::SetClipboardData(CF_UNICODETEXT, hg);
              }
            }
            ::CloseClipboard();
          }
          ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
          glint_platform::setClipboardText(text);
          requestRedraw();
#endif
        }
        return;
      }
    }

    // Ctrl+S — save all CSS rules with pending inspector edits.
    if (kp.ctrl && kp.vk == 'S')
    {
      if (mStylePanel) mStylePanel->saveAllDirtyRules();
#if defined(_WIN32)
      ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
      requestRedraw();
#endif
      return;
    }

    // Delete — remove the selected tree node when no text field currently owns focus.
    if (!kp.ctrl && !kp.alt && !kp.shift && kp.vk == VK_DELETE)
    {
      glint_element* focused = mOwnRoot ? mOwnRoot->getFocusedNode() : nullptr;
      if (!mOwnRoot || !focused || !focused->consumesCtrlA())
      {
        removeSelectedInspectorNode();
#if defined(_WIN32)
        ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
        requestRedraw();
#endif
      }
      return;
    }

    // Ctrl+Z — undo the most recent inspector tree removal when no text field owns focus.
    if (kp.ctrl && !kp.alt && !kp.shift && kp.vk == 'Z')
    {
      glint_element* focused = mOwnRoot ? mOwnRoot->getFocusedNode() : nullptr;
      if (!mOwnRoot || !focused || !focused->consumesCtrlA())
      {
        undoLastRemovedInspectorNode();
#if defined(_WIN32)
        ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
        requestRedraw();
#endif
      }
      return;
    }
  }

  // Switch the active bottom tab (0 = Inspector, 1 = Rendering, 2 = Network).
  void switchTab(int idx)
  {
    if (mActiveTab == idx) return;
    mActiveTab = idx;

    // Toggle body visibility.
    if (mInspBody)    mInspBody->style.display    = (idx == 0) ? "flex" : "none";
    if (mRenderBody)  mRenderBody->style.display   = (idx == 1) ? "flex" : "none";
    if (mNetworkBody) mNetworkBody->style.display  = (idx == 2) ? "flex" : "none";

    // Force a full network list rebuild when switching to the Network tab.
    if (idx == 2) { mNetworkEntryCount = -1; refreshNetworkTab(); }

    // Update tab button active states and backgrounds.
    for (int i = 0; i < 3; ++i)
    {
      if (!mTabBtns[i]) continue;
      const bool a = (i == idx);
      mTabBtns[i]->active                  = a;
      mTabBtns[i]->style.backgroundColor   = a ? "#252525" : "#1e1e1e";
    }

    if (mOwnRoot) mOwnRoot->setDirty(false);
  }

  // Switch the active right-sidebar tab (0 = Style, 1 = Computed).
  void switchRightTab(int idx)
  {
    // Dismiss popup whenever we leave the Computed tab (or switch to it, since
    // it will be showing a fresh node anyway).
    if (mPreviewPopup)
    {
#if defined(_WIN32)
      ::KillTimer(mHWND, WM_INSP_PREVIEW_SHOW_TIMER);
      ::KillTimer(mHWND, WM_INSP_PREVIEW_HIDE_TIMER);
#elif defined(__APPLE__)
      killTimer(WM_INSP_PREVIEW_SHOW_TIMER);
      killTimer(WM_INSP_PREVIEW_HIDE_TIMER);
#endif
      mPreviewPopup->dismiss();
    }

    if (mActiveRightTab == idx) return;
    mActiveRightTab = idx;

    if (mStylePanel)    mStylePanel->style.display    = (idx == 0) ? "flex" : "none";
    if (mComputedPanel) mComputedPanel->style.display = (idx == 1) ? "flex" : "none";

    for (int i = 0; i < 2; ++i)
    {
      if (!mRightTabBtns[i]) continue;
      const bool a = (i == idx);
      mRightTabBtns[i]->active                = a;
      mRightTabBtns[i]->style.backgroundColor = a ? "#252525" : "#191919";
    }

    if (mSelectedComp)
      refreshActiveSidebarPanel(mSelectedComp);

    if (mOwnRoot) mOwnRoot->setDirty(false);
  }

  // -- Screenshot export -------------------------------------------------------
  // Renders mMainRoot to a CPU SkBitmap, encodes as PNG, and shows a Save As
  // dialog.  Cross-platform: Win32 GetSaveFileNameW on Windows, NSSavePanel on
  // macOS (compiled as ObjC++ via glint_window_mac.mm).
  void exportScreenshot()
  {
    if (!mMainRoot) return;

    const glint_rect& bounds = mMainRoot->mCanvas.mRect;
    const int w = static_cast<int>(std::round(bounds.W()));
    const int h = static_cast<int>(std::round(bounds.H()));
    if (w <= 0 || h <= 0) return;

    // Render into an off-screen CPU bitmap.
    SkBitmap bmp;
    if (!bmp.tryAllocPixels(SkImageInfo::MakeN32Premul(w, h))) return;
    bmp.eraseColor(SK_ColorTRANSPARENT);
    {
      SkCanvas c(bmp);
      mMainRoot->DrawToCanvas(c);
    }

    // Encode to PNG in memory.
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, bmp.pixmap(), {})) return;
    sk_sp<SkData> pngData = stream.detachAsData();
    if (!pngData || pngData->isEmpty()) return;

    // -- Platform Save As dialog + write -------------------------------------
#if defined(_WIN32)
    wchar_t path[MAX_PATH] = L"screenshot.png";
    OPENFILENAMEW ofn    = {};
    ofn.lStructSize      = sizeof(ofn);
    ofn.hwndOwner        = mHWNDAtom.load();
    ofn.lpstrFilter      = L"PNG Image\0*.png\0All Files\0*.*\0";
    ofn.lpstrDefExt      = L"png";
    ofn.lpstrFile        = path;
    ofn.nMaxFile         = MAX_PATH;
    ofn.lpstrTitle       = L"Export Screenshot";
    ofn.Flags            = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetSaveFileNameW(&ofn)) return;  // user cancelled

    HANDLE hFile = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(hFile, pngData->data(), static_cast<DWORD>(pngData->size()),
                &written, nullptr);
    ::CloseHandle(hFile);

#elif defined(__APPLE__) && defined(__OBJC__)
    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setAllowedFileTypes:@[@"png"]];
    [panel setNameFieldStringValue:@"screenshot.png"];
    [panel setTitle:@"Export Screenshot"];
    if ([panel runModal] != NSModalResponseOK) return;  // user cancelled

    NSString* nsPath = [[panel URL] path];
    if (!nsPath) return;
    const char* cPath = [nsPath UTF8String];
    FILE* f = fopen(cPath, "wb");
    if (!f) return;
    fwrite(pngData->data(), 1, pngData->size(), f);
    fclose(f);
#endif
  }

  void buildUI() override
  {
    // Prevent the inspector from being inspected recursively.
    if (mOwnRoot) mOwnRoot->skipInspectMode = true;
    // Register this document so DrawToCanvas skips the debug border overlay.
    glint_debug::inspectorDoc = mOwnRoot.get();

    mTree             = nullptr;
    mStylePanel       = nullptr;
    mRealtimeBtn      = nullptr;
    mInspBody         = nullptr;
    mRenderBody       = nullptr;
    mNetworkBody      = nullptr;
    mNetworkListContainer = nullptr;
    mNetworkCountLabel    = nullptr;
    mNetworkEntryCount    = -1;
    mFpsChart         = nullptr;
    mFpsLabel         = nullptr;
    mFrameTimeLabel   = nullptr;
    mDrawCountLabel   = nullptr;
    mTabBtns[0] = mTabBtns[1] = mTabBtns[2] = nullptr;
    mComputedPanel             = nullptr;
    mPreviewPopup              = nullptr;
    mRightTabBtns[0] = mRightTabBtns[1] = nullptr;
    mActiveRightTab   = 0;

    // -- Header (HTML export + crosshair inspect-mode toggle) -----------------
    {
      auto* header = new glint_element();
      header->style.backgroundColor = "#252525";
      header->style.height          = static_cast<float>(kHeaderH);
      header->style.width           = "100%";
      header->style.display         = "flex";
      header->style.alignItems      = "center";
      header->style.justifyContent  = "flex-end";
      header->style.padding         = "0 8";
      mOwnRoot->mCanvas.addChild(header);

      auto* removeBtn = new TrashCanButton();
      removeBtn->style.width             = 28.f;
      removeBtn->style.height            = 28.f;
      removeBtn->style.marginRight       = 8.f;
      removeBtn->style.backgroundColor   = glint_color(255, 44, 32, 32);
      removeBtn->style.color             = glint_color(255, 176, 126, 126);
      removeBtn->style.borderColor       = glint_color(255, 96, 56, 56);
      removeBtn->style.borderWidth       = 1.f;
      removeBtn->style.borderRadius      = 4.f;
      removeBtn->hover.backgroundColor   = glint_color(255, 68, 36, 36);
      removeBtn->hover.color             = glint_color(255, 232, 182, 182);
      removeBtn->hover.borderColor       = glint_color(255, 164, 74, 74);
      removeBtn->hover.borderWidth       = 1.f;
      removeBtn->hover.borderRadius      = 4.f;
      removeBtn->pressed.backgroundColor = glint_color(255, 90, 30, 30);
      removeBtn->pressed.color           = glint_color(255, 255, 210, 210);
      removeBtn->pressed.borderColor     = glint_color(255, 190, 86, 86);
      removeBtn->pressed.borderWidth     = 1.f;
      removeBtn->pressed.borderRadius    = 4.f;
      removeBtn->disabled                = true;
      removeBtn->onClick = [this] {
        if (mRemoveNodeBtn && !mRemoveNodeBtn->disabled)
          removeSelectedInspectorNode();
      };
      mRemoveNodeBtn = removeBtn;
      header->addChild(removeBtn);

      // Spacer: pushes the remaining toolbar buttons to the right edge while
      // keeping the trash icon anchored to the left.
      auto* headerSpacer = new glint_element();
      headerSpacer->style.flexGrow = 1.f;
      headerSpacer->style.height   = "100%";
      header->addChild(headerSpacer);

      auto* screenshotBtn = new CameraButton();
      screenshotBtn->style.width            = 28.f;
      screenshotBtn->style.height           = 28.f;
      screenshotBtn->style.marginRight      = 8.f;
      screenshotBtn->style.backgroundColor  = "#2c2c2c";
      screenshotBtn->style.color            = "#bbb";
      screenshotBtn->style.borderColor      = "#484848";
      screenshotBtn->style.borderWidth      = 1.f;
      screenshotBtn->style.borderRadius     = 4.f;
      screenshotBtn->hover.backgroundColor  = "#3a3a3a";
      screenshotBtn->hover.color            = "#fff";
      screenshotBtn->hover.borderColor      = "#666";
      screenshotBtn->hover.borderWidth      = 1.f;
      screenshotBtn->hover.borderRadius     = 4.f;
      screenshotBtn->pressed.backgroundColor = "#4a4a4a";
      screenshotBtn->pressed.color           = "#fff";
      screenshotBtn->pressed.borderColor     = "#777";
      screenshotBtn->pressed.borderWidth     = 1.f;
      screenshotBtn->pressed.borderRadius    = 4.f;
      screenshotBtn->onClick = [this] { exportScreenshot(); };
      header->addChild(screenshotBtn);

      auto* exportBtn = new HtmlDocButton();
      exportBtn->style.width            = 28.f;
      exportBtn->style.height           = 28.f;
      exportBtn->style.marginRight      = 8.f;
      exportBtn->style.backgroundColor  = "#2c2c2c";
      exportBtn->style.color            = "#bbb";
      exportBtn->style.borderColor      = "#484848";
      exportBtn->style.borderWidth      = 1.f;
      exportBtn->style.borderRadius     = 4.f;
      exportBtn->hover.backgroundColor  = "#3a3a3a";
      exportBtn->hover.color            = "#fff";
      exportBtn->hover.borderColor      = "#666";
      exportBtn->hover.borderWidth      = 1.f;
      exportBtn->hover.borderRadius     = 4.f;
      exportBtn->pressed.backgroundColor = "#4a4a4a";
      exportBtn->pressed.color           = "#fff";
      exportBtn->pressed.borderColor     = "#777";
      exportBtn->pressed.borderWidth     = 1.f;
      exportBtn->pressed.borderRadius    = 4.f;
      exportBtn->onClick = [this] { exportDOMTreeHTML(); };
      header->addChild(exportBtn);

      auto* realtimeBtn = new PulseButton();
      realtimeBtn->style.width          = 28.f;
      realtimeBtn->style.height         = 28.f;
      realtimeBtn->style.marginRight    = 8.f;
      realtimeBtn->style.borderWidth    = 1.f;
      realtimeBtn->style.borderRadius   = 4.f;
      realtimeBtn->hover.borderWidth    = 1.f;
      realtimeBtn->hover.borderRadius   = 4.f;
      realtimeBtn->pressed.borderWidth  = 1.f;
      realtimeBtn->pressed.borderRadius = 4.f;
      realtimeBtn->onClick = [this] { applyRealtimeMode(!mRealtimeMode); };
      mRealtimeBtn = realtimeBtn;
      applyRealtimeMode(mRealtimeMode);
      header->addChild(realtimeBtn);

      auto* btn = new CrosshairButton();
      btn->style.width             = 28.f;
      btn->style.height            = 28.f;
      btn->style.backgroundColor   = "#2c2c2c";
      btn->style.color             = "#bbb";
      btn->style.borderColor       = "#484848";
      btn->style.borderWidth       = 1.f;
      btn->style.borderRadius      = 4.f;
      btn->hover.backgroundColor   = "#3a3a3a";  btn->hover.color = "#fff";
      btn->hover.borderColor       = "#666";     btn->hover.borderWidth = 1.f;
      btn->hover.borderRadius      = 4.f;
      btn->pressed.backgroundColor = "#4a4a4a";  btn->pressed.color = "#fff";
      btn->pressed.borderColor     = "#777";     btn->pressed.borderWidth = 1.f;
      btn->pressed.borderRadius    = 4.f;
      mInspectBtn = btn;
      btn->onClick = [this] { applyInspectMode(!glint_debug::inspectMode.load()); };

      header->addChild(btn);
    }

    // -- Inspector body � Tab 0 ------------------------------------------------
    {
      auto* body = new glint_element();
      body->style.display        = "flex";      // visible initially
      body->style.flexDirection  = "row";
      body->style.flexGrow       = 1.f;
      body->style.width          = "100%";
      body->style.minWidth       = 0.f;
      body->style.overflow       = "hidden";
      mInspBody = body;
      mOwnRoot->mCanvas.addChild(body);

      // Tree (left column)
      {
        auto* tree = new glint_tree();
        tree->style.flexGrow        = 1.f;
        tree->style.minWidth        = 0.f;
        tree->style.height          = "100%";
        tree->style.backgroundColor = "#1c1c1c";
        tree->onSelect = [this](const glint_tree_node& node) {
          selectInspectorNodeById(node.id);
        };
        tree->onHover = [this](const glint_tree_node* node) {
          if (!mMainRoot) return;
          if (node) {
            if (auto* comp = mMainRoot->getNodeById(node->id))
              glint_debug::hoveredNode.store(comp);
          } else {
            glint_debug::hoveredNode.store(nullptr);
          }
          mMainRoot->setDirty(false);
        };
        tree->onEyeToggle = [this](uint64_t id, bool active) {
          if (!mMainRoot) return;
          if (active && id != 0) {
            if (auto* comp = mMainRoot->getNodeById(id))
              glint_debug::pinnedNode.store(comp);
          } else {
            glint_debug::pinnedNode.store(nullptr);
          }
          mMainRoot->setDirty(false);
        };
        mTree = tree;
        body->addChild(tree);
      }

      // Vertical divider
      {
        auto* div = new glint_element();
        div->style.width           = 1.f;
        div->style.height          = "100%";
        div->style.backgroundColor = "#383838";
        body->addChild(div);
      }

      // Right column: right-tab-bar + Style panel + Computed panel
      {
        auto* rightCol = new glint_element();
        rightCol->style.width         = kStyleWResponsive;
        rightCol->style.maxWidth      = static_cast<float>(kStyleW);
        rightCol->style.minWidth      = 0.f;
        rightCol->style.height        = "100%";
        rightCol->style.display       = "flex";
        rightCol->style.flexDirection = "column";
        rightCol->style.backgroundColor = "#191919";
        body->addChild(rightCol);

        // Right tab bar: Style | Computed
        {
          auto* tabBar = new glint_element();
          tabBar->style.height           = static_cast<float>(kTabH);
          tabBar->style.width            = "100%";
          tabBar->style.display          = "flex";
          tabBar->style.flexDirection    = "row";
          tabBar->style.backgroundColor  = "#191919";
          tabBar->style.borderBottomWidth = 1.f;
          tabBar->style.borderBottomColor = "#333";
          rightCol->addChild(tabBar);

          const char* rNames[] = { "Style", "Computed" };
          for (int i = 0; i < 2; ++i)
          {
            auto* tab = new InspTabBtn();
            tab->label                 = rNames[i];
            tab->active                = (i == 0);
            tab->style.flexGrow        = 1.f;
            tab->style.height          = "100%";
            tab->style.backgroundColor = (i == 0) ? "#252525" : "#191919";
            mRightTabBtns[i] = tab;
            const int idx = i;
            tab->element.addEventListener("click", [this, idx](glint_event&) {
              switchRightTab(idx);
            });
            tabBar->addChild(tab);
          }
        }

        // Style panel (shown by default)
        {
          auto* panel = new InspStylePanel();
          panel->style.flexGrow        = 1.f;
          panel->style.width           = "100%";
          panel->style.backgroundColor = "#191919";
          mStylePanel        = panel;
#if defined(_WIN32)
          panel->mOwnerHWND  = mHWND;  // enables color picker & attr list popups
#elif defined(__APPLE__)
          panel->mOwnerHWND  = this;   // glint_window_mac* stored as void*; enables attr list popup
#endif
          panel->mDocument   = mMainRoot;
          panel->prewarmPicker();  // spin up hidden picker thread now to avoid first-open delay
          rightCol->addChild(panel);
        }

        // Computed panel (hidden by default)
        {
          auto* panel = new InspComputedPanel();
          panel->style.display         = "none";
          panel->style.flexGrow        = 1.f;
          panel->style.width           = "100%";
          panel->style.backgroundColor = "#191919";
          mComputedPanel = panel;
          rightCol->addChild(panel);
        }

        // Image preview popup — absolute-positioned overlay on the root canvas.
        // Must be added AFTER rightCol so it paints on top (higher sibling order).
        // We wire the computed panel callbacks here so the popup's helper functions
        // (_inspExtractUrlPath etc.) are in scope (image_preview_popup.hpp is included
        // before style_editor.hpp in this translation unit).
        {
          auto* popup = new InspImagePreviewPopup();
          popup->mRootW = static_cast<float>(defaultWidth());
          popup->mRootH = static_cast<float>(defaultHeight());
          mPreviewPopup = popup;
          mOwnRoot->mCanvas.addChild(popup);

          // Shared callbacks — both the Style panel and Computed panel fire these
          // on mouseenter/mouseleave for img-bearing property rows.
          // Defined once here; _inspExtractUrlPath is in scope via image_preview_popup.hpp.
          auto onRowEnter = [this](const std::string& key, const std::string& val,
                                   float rowLeft, float rowTop, float rowBot)
          {
            if (!mPreviewPopup) return;
            std::string path = _inspExtractUrlPath(val);
            if (path.empty() && key == "src")
            {
              path = val;
              while (!path.empty() && std::isspace(static_cast<unsigned char>(path.front()))) path.erase(path.begin());
              while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) path.pop_back();
            }
            if (path.empty()) return;
#if defined(_WIN32)
            if (mHWND) ::KillTimer(mHWND, WM_INSP_PREVIEW_HIDE_TIMER);
            mPreviewPopup->prepareShow(path, rowLeft, rowTop, rowBot);
            if (mHWND) ::SetTimer(mHWND, WM_INSP_PREVIEW_SHOW_TIMER, 150, nullptr);
#elif defined(__APPLE__)
            killTimer(WM_INSP_PREVIEW_HIDE_TIMER);
            mPreviewPopup->prepareShow(path, rowLeft, rowTop, rowBot);
            setTimer(WM_INSP_PREVIEW_SHOW_TIMER, 0.15);
#endif
          };
          auto onRowLeave = [this]()
          {
            if (!mPreviewPopup) return;
#if defined(_WIN32)
            if (mHWND) ::KillTimer(mHWND, WM_INSP_PREVIEW_SHOW_TIMER);
#elif defined(__APPLE__)
            killTimer(WM_INSP_PREVIEW_SHOW_TIMER);
#endif
            if (mPreviewPopup->mState == InspImagePreviewPopup::State::Visible)
            {
              mPreviewPopup->prepareHide();
#if defined(_WIN32)
              if (mHWND) ::SetTimer(mHWND, WM_INSP_PREVIEW_HIDE_TIMER, 100, nullptr);
#elif defined(__APPLE__)
              setTimer(WM_INSP_PREVIEW_HIDE_TIMER, 0.10);
#endif
            }
            else
            {
              mPreviewPopup->mState = InspImagePreviewPopup::State::Idle;
            }
          };

          if (mStylePanel)    mStylePanel->setPreviewCallbacks(onRowEnter, onRowLeave);
          if (mComputedPanel) mComputedPanel->setPreviewCallbacks(onRowEnter, onRowLeave);
        }
      }  // right column
    }    // inspector body (mInspBody)

    // -- Rendering body � Tab 1 (hidden initially) -----------------------------
    {
      auto* body = new glint_element();
      body->style.display        = "none";   // hidden until tab 1 is selected
      body->style.flexDirection  = "column";
      body->style.flexGrow       = 1.f;
      body->style.width          = "100%";
      body->style.overflow       = "hidden";
      body->style.padding        = "16 20";
      body->style.gap            = 12.f;
      mRenderBody = body;
      mOwnRoot->mCanvas.addChild(body);

      // Section: Performance
      {
        auto* lbl = new InspSectionLabel();
        lbl->innerText = "PERFORMANCE";
        lbl->style.width  = "100%";
        lbl->style.height = 16.f;
        body->addChild(lbl);
      }

      // Stats cards row: FPS + Frame Time
      {
        auto* row = new glint_element();
        row->style.display       = "flex";
        row->style.flexDirection = "row";
        row->style.gap           = 12.f;
        body->addChild(row);

        // Helper: build a stat card with a title + big value label.
        auto makeCard = [&](const char* title, glint_element*& outVal)
        {
          auto* card = new glint_element();
          card->style.display        = "flex";
          card->style.flexDirection  = "column";
          card->style.backgroundColor = "#252525";
          card->style.borderRadius   = 6.f;
          card->style.borderColor    = "#383838";
          card->style.borderWidth    = 1.f;
          card->style.padding        = "8 12";
          card->style.gap            = 4.f;
          row->addChild(card);

          auto* tl = new glint_element();
          tl->innerText        = title;
          tl->style.fontSize = 10.f;
          tl->style.color = glint_color(255, 110, 110, 110);
          card->addChild(tl);

          auto* vl = new glint_element();
          vl->innerText        = "\xe2\x80\x94";   // em-dash placeholder
          vl->style.fontSize = 22.f;
          vl->style.color = glint_color(255, 215, 215, 215);
          card->addChild(vl);
          outVal = vl;
        };

        makeCard("FPS",        mFpsLabel);
        makeCard("FRAME TIME", mFrameTimeLabel);
      }

      // Total draw calls card
      {
        auto* card = new glint_element();
        card->style.display        = "flex";
        card->style.flexDirection  = "column";
        card->style.backgroundColor = "#252525";
        card->style.borderRadius   = 6.f;
        card->style.borderColor    = "#383838";
        card->style.borderWidth    = 1.f;
        card->style.padding        = "8 12";
        card->style.gap            = 4.f;
        card->style.width          = 200.f;
        body->addChild(card);

        auto* tl = new glint_element();
        tl->innerText        = "TOTAL DRAW CALLS";
        tl->style.fontSize = 10.f;
        tl->style.color = glint_color(255, 110, 110, 110);
        card->addChild(tl);

        auto* vl = new glint_element();
        vl->innerText        = "\xe2\x80\x94";
        vl->style.fontSize = 22.f;
        vl->style.color = glint_color(255, 215, 215, 215);
        card->addChild(vl);
        mDrawCountLabel = vl;
      }

      // FPS chart
      {
        auto* chart = new InspFpsChart();
        chart->mMainRoot           = mMainRoot;
        chart->style.width         = 300.f;
        chart->style.height        = 100.f;
        chart->style.backgroundColor = "#1c1c1c";
        chart->style.borderRadius  = 6.f;
        chart->style.borderColor   = "#383838";
        chart->style.borderWidth   = 1.f;
        chart->style.padding       = "8 10";
        mFpsChart = chart;
        body->addChild(chart);
      }

      // Divider
      {
        auto* div = new glint_element();
        div->style.width           = "100%";
        div->style.height          = 1.f;
        div->style.backgroundColor = "#333";
        body->addChild(div);
      }

      // Section: Debug Overlays
      {
        auto* lbl = new InspSectionLabel();
        lbl->innerText = "DEBUG OVERLAYS";
        lbl->style.width  = "100%";
        lbl->style.height = 16.f;
        body->addChild(lbl);
      }

      // Colorize borders row: icon btn + label
      {
        auto* row = new glint_element();
        row->style.display       = "flex";
        row->style.flexDirection = "row";
        row->style.alignItems    = "center";
        row->style.gap           = 8.f;
        body->addChild(row);

        auto* btn = new OverlappingRectsButton();
        btn->style.width             = 28.f;
        btn->style.height            = 28.f;
        btn->style.backgroundColor   = "#2c2c2c";
        btn->style.color             = "#bbb";
        btn->style.borderColor       = "#484848";
        btn->style.borderWidth       = 1.f;
        btn->style.borderRadius      = 4.f;
        btn->hover.backgroundColor   = "#3a3a3a";  btn->hover.color = "#fff";
        btn->hover.borderColor       = "#666";     btn->hover.borderWidth = 1.f;
        btn->hover.borderRadius      = 4.f;
        btn->pressed.backgroundColor = "#4a4a4a";  btn->pressed.color = "#fff";
        btn->pressed.borderColor     = "#777";     btn->pressed.borderWidth = 1.f;
        btn->pressed.borderRadius    = 4.f;
        btn->toggled = glint_debug::colorizedBorders;
        mColorizeBtn = btn;
        btn->onClick = [this] {
          glint_debug::colorizedBorders = !glint_debug::colorizedBorders;
          if (mColorizeBtn) {
            mColorizeBtn->toggled = glint_debug::colorizedBorders;
            mColorizeBtn->setDirty(false);
          }
          if (mMainRoot) mMainRoot->setDirty(false);
        };
        row->addChild(btn);

        auto* lbl = new glint_element();
        lbl->innerText        = "Colorize Borders";
        lbl->style.fontSize = 13.f;
        lbl->style.color = glint_color(255, 180, 180, 180);
        row->addChild(lbl);
      }
    }

    // -- Network body ─ Tab 2 (hidden initially) --------------------------------
    {
      auto* body = new glint_element();
      body->style.display        = "none";
      body->style.flexDirection  = "column";
      body->style.flexGrow       = 1.f;
      body->style.width          = "100%";
      body->style.overflow       = "hidden";
      mNetworkBody = body;
      mOwnRoot->mCanvas.addChild(body);

      // Toolbar: title + count + Clear button
      {
        auto* toolbar = new glint_element();
        toolbar->style.display         = "flex";
        toolbar->style.flexDirection   = "row";
        toolbar->style.alignItems      = "center";
        toolbar->style.height          = 36.f;
        toolbar->style.width           = "100%";
        toolbar->style.padding         = "0 12";
        toolbar->style.gap             = 8.f;
        toolbar->style.backgroundColor = "#252525";
        toolbar->style.borderBottomWidth = 1.f;
        toolbar->style.borderBottomColor = "#383838";
        body->addChild(toolbar);

        auto* title = new glint_element();
        title->innerText       = "Network Requests";
        title->style.fontSize  = 12.f;
        title->style.color     = glint_color(255, 160, 160, 160);
        toolbar->addChild(title);

        auto* countLbl = new glint_element();
        countLbl->innerText       = "";
        countLbl->style.fontSize  = 11.f;
        countLbl->style.color     = glint_color(255, 100, 100, 100);
        countLbl->style.flexGrow  = 1.f;
        mNetworkCountLabel = countLbl;
        toolbar->addChild(countLbl);

        // Clear (trash can icon) button
        auto* clearBtn = new TrashCanButton();
        clearBtn->style.width           = 26.f;
        clearBtn->style.height          = 26.f;
        clearBtn->style.backgroundColor = "#2c2c2c";
        clearBtn->style.color           = glint_color(255, 160, 160, 160);
        clearBtn->style.borderColor     = "#484848";
        clearBtn->style.borderWidth     = 1.f;
        clearBtn->style.borderRadius    = 4.f;
        clearBtn->hover.backgroundColor = glint_color(255, 58, 38, 38);
        clearBtn->hover.color           = glint_color(255, 220, 100, 100);
        clearBtn->hover.borderColor     = glint_color(255, 150, 60, 60);
        clearBtn->hover.borderWidth     = 1.f;
        clearBtn->hover.borderRadius    = 4.f;
        clearBtn->pressed.backgroundColor = glint_color(255, 80, 30, 30);
        clearBtn->pressed.color           = glint_color(255, 240, 120, 120);
        clearBtn->pressed.borderColor     = glint_color(255, 180, 70, 70);
        clearBtn->pressed.borderWidth     = 1.f;
        clearBtn->pressed.borderRadius    = 4.f;
        clearBtn->onClick = [this] {
          if (mMainRoot) mMainRoot->networkLog.clear();
          mNetworkEntryCount = -1;
          refreshNetworkTab();
        };
        toolbar->addChild(clearBtn);
      }

      // Column header row
      {
        auto* hdr = new glint_element();
        hdr->style.display         = "flex";
        hdr->style.flexDirection   = "row";
        hdr->style.alignItems      = "center";
        hdr->style.height          = 24.f;
        hdr->style.width           = "100%";
        hdr->style.padding         = "0 12";
        hdr->style.gap             = 8.f;
        hdr->style.backgroundColor = "#1e1e1e";
        hdr->style.borderBottomWidth = 1.f;
        hdr->style.borderBottomColor = "#2e2e2e";
        body->addChild(hdr);

        auto makeHdrCell = [&](const char* text, float w, float grow = 0.f) {
          auto* c = new glint_element();
          c->innerText       = text;
          c->style.fontSize  = 10.f;
          c->style.color     = glint_color(255, 100, 100, 100);
          if (w > 0.f) c->style.width   = w;
          if (grow > 0.f) c->style.flexGrow = grow;
          hdr->addChild(c);
        };
        makeHdrCell("STATUS",  70.f);
        makeHdrCell("TYPE",    54.f);
        makeHdrCell("URL",      0.f, 1.f);
        makeHdrCell("SIZE",    60.f);
      }

      // Scrollable row list
      {
        auto* list = new glint_element();
        list->style.display       = "flex";
        list->style.flexDirection = "column";
        list->style.flexGrow      = 1.f;
        list->style.width         = "100%";
        list->style.overflowY     = "scroll";
        mNetworkListContainer = list;
        body->addChild(list);
      }
    }

    // -- Tab bar (bottom, fixed height) ----------------------------------------
    {
      auto* bar = new glint_element();
      bar->style.height          = static_cast<float>(kTabH);
      bar->style.width           = "100%";
      bar->style.display         = "flex";
      bar->style.flexDirection   = "row";
      bar->style.backgroundColor = "#1e1e1e";
      bar->style.borderTopWidth  = 1.f;
      bar->style.borderTopColor  = "#333";
      mOwnRoot->mCanvas.addChild(bar);

      const char* names[] = { "Inspector", "Rendering", "Network" };
      for (int i = 0; i < 3; ++i)
      {
        auto* tab = new InspTabBtn();
        tab->label                  = names[i];
        tab->active                 = (i == 0);
        tab->style.flexGrow         = 1.f;
        tab->style.height           = "100%";
        tab->style.backgroundColor  = (i == 0) ? "#252525" : "#1e1e1e";
        mTabBtns[i] = tab;
        const int idx = i;
        tab->element.addEventListener("click", [this, idx](glint_event&) {
          switchTab(idx);
        });
        bar->addChild(tab);
      }
    }
  }

  // -- Rendering -------------------------------------------------------------
  void recreateSurface()
  {
#if defined(_WIN32)
    if (mRenderer)
      mRenderer->resize(mWpx, mHpx);
    else
      recreateCpuSurface();
#else
    recreateCpuSurface();
#endif

    if (mPreviewPopup)
    {
      mPreviewPopup->mRootW = static_cast<float>(mW);
      mPreviewPopup->mRootH = static_cast<float>(mH);
      if (mPreviewPopup->mState == InspImagePreviewPopup::State::Visible)
        mPreviewPopup->commitShow();
    }
  }

  // -- Tree / style refresh --------------------------------------------------
  void refreshNetworkTab()
  {
    if (!mNetworkListContainer || !mMainRoot) return;

    auto snapshot = mMainRoot->networkLog.snapshot();
    const int newCount = static_cast<int>(snapshot.size());
    if (newCount == mNetworkEntryCount) return;  // nothing changed
    mNetworkEntryCount = newCount;

    // Update count label
    if (mNetworkCountLabel)
    {
      if (newCount == 0)
        mNetworkCountLabel->innerText = "";
      else
      {
        char buf[40];
        snprintf(buf, sizeof(buf), "%d request%s", newCount, newCount == 1 ? "" : "s");
        mNetworkCountLabel->innerText = buf;
      }
    }

    // Rebuild the row list (oldest first — matches actual load/request order)
    mNetworkListContainer->clearChildren();

    for (int i = 0; i < newCount; ++i)
    {
      const glint_network_log_entry& e = snapshot[static_cast<size_t>(i)];
      const bool alt = (i % 2) == 0;

      auto* row = new glint_element();
      row->style.display         = "flex";
      row->style.flexDirection   = "row";
      row->style.alignItems      = "center";
      row->style.width           = "100%";
      row->style.minHeight       = 22.f;
      row->style.padding         = "0 12";
      row->style.gap             = 8.f;
      row->style.backgroundColor = alt ? glint_color(255, 30, 30, 30) : glint_color(255, 26, 26, 26);
      mNetworkListContainer->addChild(row);

      // Status dot + code
      {
        auto* cell = new glint_element();
        cell->style.display      = "flex";
        cell->style.flexDirection = "row";
        cell->style.alignItems   = "center";
        cell->style.width        = 70.f;
        cell->style.gap          = 5.f;
        row->addChild(cell);

        // Colored dot
        auto* dot = new glint_element();
        dot->style.width        = 7.f;
        dot->style.height       = 7.f;
        dot->style.borderRadius = 4.f;
        if (e.statusCode == 200)
          dot->style.backgroundColor = glint_color(255, 60, 180, 110);
        else if (e.statusCode >= 400 && e.statusCode < 500)
          dot->style.backgroundColor = glint_color(255, 230, 150,  50);
        else if (e.statusCode >= 500)
          dot->style.backgroundColor = glint_color(255, 210,  60,  60);
        else
          dot->style.backgroundColor = glint_color(255, 100, 100, 100);
        cell->addChild(dot);

        // Status code / disk label
        auto* code = new glint_element();
        char codeBuf[16];
        if (e.statusCode == 0)
          snprintf(codeBuf, sizeof(codeBuf), "disk");
        else
          snprintf(codeBuf, sizeof(codeBuf), "%d", e.statusCode);
        code->innerText      = codeBuf;
        code->style.fontSize = 11.f;
        code->style.color    = (e.statusCode == 200) ? glint_color(255, 80, 200, 130)
                             : (e.statusCode  ==  0) ? glint_color(255, 110,110,110)
                             : (e.statusCode  >= 400 && e.statusCode < 500)
                                                      ? glint_color(255, 220, 150, 60)
                                                      : glint_color(255, 200, 80, 80);
        cell->addChild(code);
      }

      // Type
      {
        auto* cell = new glint_element();
        cell->style.width    = 54.f;
        cell->style.fontSize = 11.f;
        cell->style.color    = glint_color(255, 120, 120, 120);
        switch (e.type)
        {
          case glint_resource_request::Type::Image:      cell->innerText = "img";  break;
          case glint_resource_request::Type::SVG:        cell->innerText = "svg";  break;
          case glint_resource_request::Type::Stylesheet: cell->innerText = "css";  break;
          default:                                       cell->innerText = "?";    break;
        }
        row->addChild(cell);
      }

      // URL (pathname preferred, truncated)
      {
        auto* cell = new glint_element();
        cell->style.flexGrow   = 1.f;
        cell->style.fontSize   = 11.f;
        cell->style.color      = glint_color(255, 200, 200, 200);
        cell->style.overflow   = "hidden";
        cell->innerText = e.pathname.empty() ? e.url : e.pathname;
        row->addChild(cell);
      }

      // Size
      {
        auto* cell = new glint_element();
        cell->style.width    = 60.f;
        cell->style.fontSize = 11.f;
        cell->style.color    = glint_color(255, 110, 110, 110);
        if (e.byteSize == 0)
        {
          cell->innerText = e.statusCode == 0 ? "" : "—";
        }
        else
        {
          char sizeBuf[24];
          if (e.byteSize >= 1024 * 1024)
            snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", e.byteSize / (1024.0 * 1024.0));
          else if (e.byteSize >= 1024)
            snprintf(sizeBuf, sizeof(sizeBuf), "%.1f KB", e.byteSize / 1024.0);
          else
            snprintf(sizeBuf, sizeof(sizeBuf), "%zu B", e.byteSize);
          cell->innerText = sizeBuf;
        }
        row->addChild(cell);
      }
    }

    if (mOwnRoot) mOwnRoot->setDirty(false);
  }

  void refreshTree()
  {
    if (!mTree || !mMainRoot) return;
    const uint64_t selectedId = mSelectedNodeId;
    // Tree rebuild invalidates all component pointers from the previous snapshot.
    mSelectedComp = nullptr;
    glint_debug::hoveredNode.store(nullptr);
    glint_debug::inspectedNode.store(nullptr);
    if (mStylePanel)    mStylePanel->clear();     // pointers are stale after tree rebuild
    if (mComputedPanel) mComputedPanel->clear(); // same
    mTree->setTree(mMainRoot->getUITree());
    mTree->expandToDepth(2);
    if (selectedId != 0)
      selectInspectorNodeById(selectedId);
    else
      updateRemoveNodeButtonState();
#if defined(_WIN32)
    if (mHWND) ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
    requestRedraw();
#endif
  }

  void refreshStyle(uint64_t id)
  {
    if (!mStylePanel || !mMainRoot) return;
    if (mTree && mTree->selectedId() != id) return;
    if (auto* comp = mMainRoot->getNodeById(id))
    {
      mSelectedComp   = comp;
      mSelectedNodeId = id;
      refreshActiveSidebarPanel(comp);
      updateRemoveNodeButtonState();
    }
#if defined(_WIN32)
    if (mHWND) ::InvalidateRect(mHWND, nullptr, FALSE);
#elif defined(__APPLE__)
    requestRedraw();
#endif
  }
};

inline std::map<glint_document*, glint_inspector_window*> glint_inspector_window::sInstances;

// glint_insp_bridge � defined here after glint_inspector_window is complete.
inline void glint_insp_bridge::open (glint_document* r) { glint_inspector_window::open(r);                  }
inline void glint_insp_bridge::close(glint_document* r) { glint_inspector_window::close(r);                 }
inline bool glint_insp_bridge::isOpen(glint_document* r){ return glint_inspector_window::isOpen(r);         }
inline void glint_insp_bridge::openAndEnableInspect(glint_document* r) { glint_inspector_window::openAndEnableInspect(r); }

// glint_document::showInspector / isInspectorOpen / openInspectorWithPicker
// Defined here after glint_inspector_window is complete.
inline void glint_document::showInspector(bool open)
{
  if (open) glint_inspector_window::open(this);
  else      glint_inspector_window::close(this);
}
inline bool glint_document::isInspectorOpen() const
{
  return glint_inspector_window::isOpen(const_cast<glint_document*>(this));
}
inline void glint_document::openInspectorWithPicker()
{
  glint_inspector_window::openAndEnableInspect(this);
}
