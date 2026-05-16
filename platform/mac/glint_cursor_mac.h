/**
 * glint_cursor_mac.h
 * CSS cursor → NSCursor mapping using CoreCursor private type IDs.
 *
 * Mirrors Chrome's CrCoreCursor approach:
 *   https://source.chromium.org/chromium/chromium/src/+/main:ui/base/cocoa/cursor_utils.mm
 *
 * The key insight: NSCursor subclasses can override the private property
 * `_coreCursorType` to tell CoreCursor which system cursor glyph to render.
 * This gives accurate CSS cursor parity on macOS 11–14 without drawing
 * custom bitmaps.  The integer IDs come from CoreCursor's internal enum.
 *
 * Usage:
 *   NSCursor* cursor = glint_css_to_nscursor(css_keyword);
 *   [cursor set];
 *
 * IMPORTANT: GlintCoreCursor must be compiled into the binary exactly once.
 * Include this header in multiple .mm files freely; the @implementation is
 * in glint_cursor_mac.mm.
 */

#pragma once

#import <Cocoa/Cocoa.h>
#include "glint_cursor_mac_shared.hpp"
#include <string>

// ---------------------------------------------------------------------------
// CoreCursor type IDs (from Chrome's CrCoreCursorType enum).
// column/row resize cursors have a bar (←|→), frame resize cursors do not (←→).
// ---------------------------------------------------------------------------
enum GlintCoreCursorType : int32_t {
    kGlintCursorBusyButClickable      =  4,  // spinning indicator + arrow (wait/progress)
    kGlintCursorColumnResizeLeftRight = 19,  // col-resize   ←|→  (with bar)
    kGlintCursorRowResizeUpDown       = 23,  // row-resize   ↑|↓  (with bar)
    kGlintCursorFrameResizeEast       = 27,  // e-resize     →
    kGlintCursorFrameResizeEastWest   = 28,  // ew-resize    ←→
    kGlintCursorFrameResizeNE         = 29,  // ne-resize    ↗
    kGlintCursorFrameResizeNESW       = 30,  // nesw-resize  ↗↙
    kGlintCursorFrameResizeNorth      = 31,  // n-resize     ↑
    kGlintCursorFrameResizeNS         = 32,  // ns-resize    ↑↓
    kGlintCursorFrameResizeNW         = 33,  // nw-resize    ↖
    kGlintCursorFrameResizeNWSE       = 34,  // nwse-resize  ↖↘
    kGlintCursorFrameResizeSE         = 35,  // se-resize    ↘
    kGlintCursorFrameResizeSouth      = 36,  // s-resize     ↓
    kGlintCursorFrameResizeSW         = 37,  // sw-resize    ↙
    kGlintCursorFrameResizeWest       = 38,  // w-resize     ←
    kGlintCursorMove                  = 39,  // move / all-scroll  ✥
    kGlintCursorHelp                  = 40,  // help  arrow + ?
    kGlintCursorZoomIn                = 42,  // zoom-in   🔍+
    kGlintCursorZoomOut               = 43,  // zoom-out  🔍-
};

// ---------------------------------------------------------------------------
// GlintCoreCursor — NSCursor subclass that exposes the private _coreCursorType
// property so macOS CoreCursor picks up the correct system glyph.
// ---------------------------------------------------------------------------
@interface GlintCoreCursor : NSCursor
+ (instancetype)cursorWithType:(GlintCoreCursorType)type;
@property(readonly, nonatomic) GlintCoreCursorType _coreCursorType;
@end

// ---------------------------------------------------------------------------
// glint_css_to_nscursor — maps a CSS cursor keyword to an NSCursor.
// Returns [NSCursor arrowCursor] for unknown / "default" / "auto".
// ---------------------------------------------------------------------------
static inline NSCursor* glint_css_to_nscursor(const std::string& css)
{
    if (void* custom = glint_mac_cursor::findCustomCursor(css))
        return (NSCursor*) custom;

    // --- Public NSCursor API ---
    if (css == "pointer")                         return [NSCursor pointingHandCursor];
    if (css == "text")                            return [NSCursor IBeamCursor];
    if (css == "vertical-text")                   return [NSCursor IBeamCursorForVerticalLayout];
    if (css == "crosshair" || css == "cell")      return [NSCursor crosshairCursor];
    if (css == "grab")                            return [NSCursor openHandCursor];
    if (css == "grabbing")                        return [NSCursor closedHandCursor];
    if (css == "not-allowed" || css == "no-drop") return [NSCursor operationNotAllowedCursor];
    if (css == "copy")                            return [NSCursor dragCopyCursor];
    if (css == "alias")                           return [NSCursor dragLinkCursor];
    if (css == "context-menu")                    return [NSCursor contextualMenuCursor];

    // --- CoreCursor private type IDs (via GlintCoreCursor) ---
    if (css == "move" || css == "all-scroll")
        return [GlintCoreCursor cursorWithType:kGlintCursorMove];
    if (css == "help")
        return [GlintCoreCursor cursorWithType:kGlintCursorHelp];
    if (css == "wait" || css == "progress")
        return [GlintCoreCursor cursorWithType:kGlintCursorBusyButClickable];
    if (css == "zoom-in")
        return [GlintCoreCursor cursorWithType:kGlintCursorZoomIn];
    if (css == "zoom-out")
        return [GlintCoreCursor cursorWithType:kGlintCursorZoomOut];

    // Cardinal frame-resize (no bar):
    if (css == "n-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeNorth];
    if (css == "s-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeSouth];
    if (css == "e-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeEast];
    if (css == "w-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeWest];
    if (css == "ns-resize")  return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeNS];
    if (css == "ew-resize")  return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeEastWest];

    // Column / row resize (with bar):
    if (css == "col-resize") return [GlintCoreCursor cursorWithType:kGlintCursorColumnResizeLeftRight];
    if (css == "row-resize") return [GlintCoreCursor cursorWithType:kGlintCursorRowResizeUpDown];

    // Diagonal frame-resize (no bar):
    if (css == "ne-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeNE];
    if (css == "sw-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeSW];
    if (css == "nw-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeNW];
    if (css == "se-resize")   return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeSE];
    if (css == "nesw-resize") return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeNESW];
    if (css == "nwse-resize") return [GlintCoreCursor cursorWithType:kGlintCursorFrameResizeNWSE];

    // Invisible cursor:
    if (css == "none") {
        static NSCursor* blank = []() -> NSCursor* {
            NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
            NSCursor* c  = [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(0, 0)];
            [img release];
            return c;
        }();
        return blank;
    }

    // default / auto / unknown → system arrow
    return [NSCursor arrowCursor];
}
