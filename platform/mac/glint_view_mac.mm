/**
 * glint_view_mac.mm
 * macOS embedded view host — ObjC++ implementation.
 *
 * Provides GlintMacView (an NSView subclass) and the glint_view_mac C++
 * method bodies.  All Cocoa / ObjC types are confined to this file.
 */

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CVDisplayLink.h>

#include "glint_view_mac.hpp"
#include "../../glint_standalone.hpp"       // pulls in glint_colorpicker, glint_list, all components
#include "../../inspector/glint_inspector_support.hpp"

// Rename alias: .mm files were written against sk_mouse_mod; headers now use glint_mouse_mod.
using sk_mouse_mod = glint_mouse_mod;
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/utils/mac/SkCGUtils.h"
#include "include/gpu/ganesh/GrTypes.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// ---------------------------------------------------------------------------
// Cursor helpers
// ---------------------------------------------------------------------------
#import "glint_cursor_mac.h"

// Maps a CSS cursor keyword to the closest available NSCursor and sets it.
static void glint_view_mac_set_cursor(const std::string& css)
{
    [glint_css_to_nscursor(css) set];
}

namespace {

NSView* glint_resolve_parent_view(void* parent)
{
  id candidate = (id) parent;
  if (candidate == nil)
    return nil;

  if ([candidate isKindOfClass:[NSView class]])
    return (NSView*) candidate;

  if ([candidate isKindOfClass:[NSWindow class]])
    return [(NSWindow*) candidate contentView];

  return nil;
}

sk_mouse_mod glint_modifiers_from_event(NSEvent* event)
{
  sk_mouse_mod mod = {};
  const NSEventModifierFlags flags = [event modifierFlags];
  mod.S = (flags & NSEventModifierFlagShift)   != 0;
  mod.C = (flags & NSEventModifierFlagControl) != 0;
  mod.A = (flags & NSEventModifierFlagOption)  != 0;
  mod.M = (flags & NSEventModifierFlagCommand) != 0;
  return mod;
}

sk_mouse_mod glint_buttons_from_event(NSEvent* event)
{
  sk_mouse_mod mod = glint_modifiers_from_event(event);
  const NSUInteger buttons = [NSEvent pressedMouseButtons];
  mod.L = (buttons & 1U) != 0;
  mod.R = (buttons & 2U) != 0;
  return mod;
}

int glint_virtual_key_from_event(NSEvent* event)
{
  switch ([event keyCode])
  {
    case 123: return 0x25;  // left
    case 124: return 0x27;  // right
    case 125: return 0x28;  // down
    case 126: return 0x26;  // up
    case 115: return 0x24;  // Home
    case 119: return 0x23;  // End
    case  51: return 0x08;  // Backspace
    case 117: return 0x2E;  // Delete
    case  36:
    case  76: return 0x0D;  // Return
    case  53: return 0x1B;  // Escape
    case  48: return 0x09;  // Tab
    // Number row — always map to the digit VK regardless of shift state.
    // [event charactersIgnoringModifiers] retains Shift, so e.g. Shift+5
    // returns '%' (0x25) which collides with VK_LEFT, Shift+4 returns '$'
    // (0x24 = VK_HOME), '.' returns 0x2E (= VK_DEL), etc.
    case 29: return '0';
    case 18: return '1';
    case 19: return '2';
    case 20: return '3';
    case 21: return '4';
    case 23: return '5';
    case 22: return '6';
    case 26: return '7';
    case 28: return '8';
    case 25: return '9';
    // Punctuation keys whose unshifted ASCII collides with a navigation VK.
    case 39: return 0xDE;  // apostrophe/quote  (0x27 = VK_RIGHT collision)
    case 47: return 0xBE;  // period/full-stop  (0x2E = VK_DEL   collision)
    default:  break;
  }

  NSString* chars = [event charactersIgnoringModifiers];
  if ([chars length] == 0)
    return 0;

  const unichar ch = [chars characterAtIndex:0];
  if (ch >= 0x20 && ch <= 0x7E)
    return std::toupper(static_cast<unsigned char>(ch));

  // Ctrl+letter arrives as control character (0x01–0x1A). Map back to the
  // uppercase letter so key handlers see e.g. 'Z' for Ctrl+Z.
  if (ch >= 0x01 && ch <= 0x1A)
    return static_cast<int>(ch + 0x40);

  return 0;
}

glint_key_press glint_key_press_from_event(NSEvent* event, bool includeText)
{
  glint_key_press kp = {};
  kp.vk = glint_virtual_key_from_event(event);

  const NSEventModifierFlags flags = [event modifierFlags];
  kp.shift = (flags & NSEventModifierFlagShift) != 0;
  kp.ctrl  = (flags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0;
  kp.alt   = (flags & NSEventModifierFlagOption) != 0;

  if (!includeText)
    return kp;

  NSString* chars = [event characters];
  if ([chars length] == 0)
    return kp;

  NSData* utf8 = [chars dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
  if (!utf8)
    return kp;

  const NSUInteger len = std::min<NSUInteger>([utf8 length], 4);
  std::memcpy(kp.utf8, [utf8 bytes], len);
  kp.utf8[len] = '\0';
  return kp;
}

bool glint_should_schedule_redraw(glint_document* doc, bool redrawRequested)
{
  if (redrawRequested)
    return true;

  glint_element* focused = doc ? doc->getFocusedNode() : nullptr;
  if (!focused || !focused->wantsPeriodicRedraw())
    return false;

  return std::chrono::steady_clock::now() >= focused->nextPeriodicRedrawTime();
}

} // namespace

// ---------------------------------------------------------------------------
// GlintMacView — NSView subclass
// ---------------------------------------------------------------------------

@interface GlintMacView : NSView <NSTextInputClient>
{
@public
  glint_view_mac* cppView;
  NSTrackingArea* trackingArea;
}
- (instancetype) initWithView:(glint_view_mac*) view frame:(NSRect) frame;
- (void) handleAnimationTimer:(NSTimer*) timer;
@end

@implementation GlintMacView

- (instancetype) initWithView:(glint_view_mac*) view frame:(NSRect) frame
{
  self = [super initWithFrame:frame];
  if (self)
    cppView = view;
  return self;
}

- (void) dealloc
{
  if (trackingArea)
  {
    [self removeTrackingArea:trackingArea];
    [trackingArea release];
    trackingArea = nil;
  }
  [super dealloc];
}

- (BOOL) isFlipped                      { return YES; }
- (BOOL) acceptsFirstResponder          { return YES; }
- (BOOL) acceptsFirstMouse:(NSEvent*) e { (void) e; return YES; }

- (BOOL) wantsLayer { return cppView && cppView->_metalEnabled() ? YES : [super wantsLayer]; }
- (CALayer*) makeBackingLayer
{
  if (cppView && cppView->_metalEnabled())
    return [CAMetalLayer layer];
  return [super makeBackingLayer];
}

- (void) viewDidMoveToWindow
{
  [super viewDidMoveToWindow];
  [[self window] setAcceptsMouseMovedEvents:YES];
  if (cppView)
    cppView->_viewDidMoveToWindow((__bridge void*)[self window]);
}

- (void) resetCursorRects
{
  [super resetCursorRects];
  // Cursor shape is driven dynamically by glint_view_mac_set_cursor in the
  // mouseMoved / mouseEntered handlers; no static cursor rects are needed.
}

- (void) updateTrackingAreas
{
  [super updateTrackingAreas];

  if (trackingArea)
  {
    [self removeTrackingArea:trackingArea];
    [trackingArea release];
    trackingArea = nil;
  }

  const NSTrackingAreaOptions opts =
    NSTrackingMouseEnteredAndExited |
    NSTrackingMouseMoved            |
    NSTrackingActiveInKeyWindow     |
    NSTrackingInVisibleRect;

  trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                              options:opts
                                                owner:self
                                             userInfo:nil];
  [self addTrackingArea:trackingArea];
}

- (void) drawRect:(NSRect) dirtyRect
{
  (void) dirtyRect;
  if (cppView)
  {
    // Metal renders directly from the animation timer; drawRect is a no-op.
    if (cppView->_metalEnabled())
      return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    cppView->_paint((void*) ctx, (__bridge void*) self);
  }
}

- (void) mouseDown:(NSEvent*) event
{
  [[self window] makeFirstResponder:self];
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    sk_mouse_mod mod = glint_modifiers_from_event(event);
    mod.L = true;
    cppView->_handleMouseDown(static_cast<float>(pt.x), static_cast<float>(pt.y), mod);
  }
}

- (void) mouseDragged:(NSEvent*) event
{
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    cppView->_handleMouseMove(static_cast<float>(pt.x), static_cast<float>(pt.y),
                              glint_buttons_from_event(event));
  }
}

- (void) mouseUp:(NSEvent*) event
{
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    sk_mouse_mod mod = glint_modifiers_from_event(event);
    mod.L = false;
    cppView->_handleMouseUp(static_cast<float>(pt.x), static_cast<float>(pt.y), mod);
  }
}

- (void) rightMouseDown:(NSEvent*) event
{
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    sk_mouse_mod mod = glint_modifiers_from_event(event);
    mod.R = true;
    cppView->_handleMouseDown(static_cast<float>(pt.x), static_cast<float>(pt.y), mod);
  }
}

- (void) rightMouseUp:(NSEvent*) event
{
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    sk_mouse_mod mod = glint_modifiers_from_event(event);
    mod.R = false;
    cppView->_handleMouseUp(static_cast<float>(pt.x), static_cast<float>(pt.y), mod);
  }
}

- (void) mouseMoved:(NSEvent*) event
{
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    cppView->_handleMouseMove(static_cast<float>(pt.x), static_cast<float>(pt.y),
                              glint_buttons_from_event(event));
  }
}

- (void) mouseEntered:(NSEvent*) event
{
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    cppView->_handleMouseMove(static_cast<float>(pt.x), static_cast<float>(pt.y),
                              glint_buttons_from_event(event));
    // Cursor is set by _handleMouseMove → glint_view_mac_set_cursor.
  }
}

- (void) mouseExited:(NSEvent*) event
{
  (void) event;
  if (cppView)
    cppView->_handleMouseLeave();
}

- (void) scrollWheel:(NSEvent*) event
{
  if (cppView)
  {
    const NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    cppView->_handleScrollWheel(
      static_cast<float>(pt.x),
      static_cast<float>(pt.y),
      static_cast<float>([event scrollingDeltaX]),
      static_cast<float>([event scrollingDeltaY]),
      glint_modifiers_from_event(event));
  }
}

- (void) keyDown:(NSEvent*) event
{
  if (cppView)
    cppView->_handleKeyDown(glint_key_press_from_event(event, false));
  // Route through AppKit so dead-key composition is resolved.
  [self interpretKeyEvents:@[event]];
}

// ── NSTextInputClient ───────────────────────────────────────────────────────
// Called by AppKit after dead-key composition completes.  This is the only
// path that delivers dead-key characters (` ´ ¨ ' ^ ~ …) and Option+key
// combos that produce non-ASCII output.
- (void) insertText:(id) aString replacementRange:(NSRange) replacementRange
{
  (void) replacementRange;
  if (!cppView) return;
  NSString* str = [aString isKindOfClass:[NSAttributedString class]]
                    ? [(NSAttributedString*) aString string]
                    : (NSString*) aString;
  if ([str length] == 0) return;
  NSData* utf8data = [str dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
  if (!utf8data || [utf8data length] == 0) return;
  glint_key_press kp = {};
  const NSUInteger len = std::min<NSUInteger>([utf8data length], 32u);
  std::memcpy(kp.utf8, [utf8data bytes], len);
  kp.utf8[len] = '\0';
  cppView->_handleKeyDown(kp);
}

// Command keys are handled via VK in the keyDown: path above.
- (void) doCommandBySelector:(SEL) aSelector { (void) aSelector; }

- (void) setMarkedText:(id) aString selectedRange:(NSRange) sel replacementRange:(NSRange) rep
  { (void) aString; (void) sel; (void) rep; }
- (void)     unmarkText                                           {}
- (NSRange)  selectedRange                                        { return NSMakeRange(NSNotFound, 0); }
- (NSRange)  markedRange                                          { return NSMakeRange(NSNotFound, 0); }
- (BOOL)     hasMarkedText                                        { return NO; }
- (nullable NSAttributedString*) attributedSubstringForProposedRange:(NSRange) r
                                                         actualRange:(NSRangePointer) ar
  { (void) r; if (ar) *ar = NSMakeRange(NSNotFound, 0); return nil; }
- (NSArray<NSAttributedStringKey>*) validAttributesForMarkedText  { return @[]; }
- (NSRect) firstRectForCharacterRange:(NSRange) r actualRange:(NSRangePointer) ar
  { (void) r; if (ar) *ar = NSMakeRange(NSNotFound, 0); return NSZeroRect; }
- (NSUInteger) characterIndexForPoint:(NSPoint) p                 { (void) p; return 0; }

- (void) keyUp:(NSEvent*) event
{
  if (cppView)
    cppView->_handleKeyUp(glint_key_press_from_event(event, false));
}

- (void) handleAnimationTimer:(NSTimer*) timer
{
  (void) timer;
  if (cppView)
    cppView->_handleAnimationTimer();
}

@end

// ---------------------------------------------------------------------------
// CVDisplayLink callback (fires on a private CoreVideo thread)
// ---------------------------------------------------------------------------

static CVReturn GlintViewMacDisplayLinkCB(CVDisplayLinkRef,
                                          const CVTimeStamp*,
                                          const CVTimeStamp*,
                                          CVOptionFlags,
                                          CVOptionFlags*,
                                          void* ctx)
{
  static_cast<glint_view_mac*>(ctx)->_cvDisplayLinkFired();
  return kCVReturnSuccess;
}

// ---------------------------------------------------------------------------
// glint_view_mac C++ method bodies
// ---------------------------------------------------------------------------

void glint_view_mac::_cvDisplayLinkFired()
{
  bool expected = false;
  if (!mFramePending.compare_exchange_strong(expected, true))
    return;  // already have a pending dispatch
  dispatch_async(dispatch_get_main_queue(), ^{
    mFramePending.store(false);  // clear BEFORE so the next tick can queue back-to-back
    _handleAnimationTimer();
  });
}

bool glint_view_mac::open()
{
  NSView* parent = glint_resolve_parent_view(mOptions.parent);
  // parent == nil is valid for AU: the host embeds the returned NSView* itself.

  if (parent)
    mParentHandle = (void*) parent;  // unretained weak ref — parent outlives us

  // Attempt full Metal init (device + queue + GrContext + CAMetalLayer) FIRST,
  // before the view exists.  Only if it succeeds do we enter layer-hosting mode.
  // If it fails, mActiveBackend stays CPU and drawRect: works normally.
  if (shouldUseGpu())
    setupMetal(nullptr);

  GlintMacView* view = [[GlintMacView alloc] initWithView:this
                                                    frame:NSMakeRect(0.0, 0.0, mW, mH)];
  // Fixed-size plugin UI: do NOT autoresize with the host window.
  // resize() is the only correct way to change the view size.
  [view setAutoresizingMask:NSViewNotSizable];

  if (mActiveBackend == glint_backend::Metal)
  {
    // Layer-hosting mode: assign our pre-configured CAMetalLayer before
    // setWantsLayer:YES and before addSubview: so AppKit never creates a
    // backing layer of the wrong type.
    [view setLayer:(__bridge CAMetalLayer*) mMetalLayer];
    [view setWantsLayer:YES];
  }

  if (parent)
    [parent addSubview:view];
  mViewHandle = (void*) CFBridgingRetain(view);
  [view release];  // CFBridgingRetain took ownership

  // Now that the view is in the hierarchy and has a window, correct the Metal
  // layer's contentsScale and drawableSize to match the actual HiDPI scale.
  // This must happen before the first paint so the compositor displays the
  // layer at the right point size (contentsScale=1 with a 2x drawable looks
  // twice as large / stretched).
  if (mActiveBackend == glint_backend::Metal && mMetalLayer)
  {
    CAMetalLayer* layer = (__bridge CAMetalLayer*) mMetalLayer;
    CGFloat scale = 1.0;
    if ([view window])
      scale = std::max<CGFloat>([[view window] backingScaleFactor], 1.0);
    else if ([parent window])
      scale = std::max<CGFloat>([[parent window] backingScaleFactor], 1.0);
    layer.contentsScale = scale;
    layer.drawableSize  = CGSizeMake(mW * scale, mH * scale);
    NSLog(@"[glint] open: layer contentsScale=%.1f drawableSize=%.0fx%.0f",
          scale, layer.drawableSize.width, layer.drawableSize.height);
  }

  initDocument();

  // Drive repaints from the display's actual refresh rate via CVDisplayLink.
  // The callback fires on a dedicated CV thread and dispatches to the main
  // thread, replacing the old fixed 60 Hz NSTimer.
  CVDisplayLinkCreateWithActiveCGDisplays(&mDisplayLink);
  CVDisplayLinkSetOutputCallback(mDisplayLink, GlintViewMacDisplayLinkCB, this);
  CVDisplayLinkStart(mDisplayLink);
  mFramePending.store(false);

  requestRedraw();
  return true;
}

void glint_view_mac::_viewDidMoveToWindow(void* nsWindowRef)
{
  // Correct the Metal layer's pixel scale now that the view has been embedded
  // by the host (relevant for the AU case where open() had no parent window).
  if (mActiveBackend != glint_backend::Metal || !mMetalLayer || !nsWindowRef)
    return;

  NSWindow* window = (__bridge NSWindow*) nsWindowRef;
  CAMetalLayer* layer = (__bridge CAMetalLayer*) mMetalLayer;
  const CGFloat scale = std::max<CGFloat>([window backingScaleFactor], 1.0);
  layer.contentsScale = scale;
  layer.drawableSize  = CGSizeMake(mW * scale, mH * scale);
  NSLog(@"[glint] viewDidMoveToWindow: layer contentsScale=%.1f drawableSize=%.0fx%.0f",
        scale, layer.drawableSize.width, layer.drawableSize.height);
  requestRedraw();
}

void glint_view_mac::close()
{
  if (mDisplayLink)
  {
    CVDisplayLinkStop(mDisplayLink);
    CVDisplayLinkRelease(mDisplayLink);
    mDisplayLink = nullptr;
  }

  if (mViewHandle)
  {
    GlintMacView* view = (__bridge GlintMacView*) mViewHandle;
    view->cppView = nullptr;  // prevent any in-flight callbacks from reaching us
    [view removeFromSuperview];
    CFRelease(mViewHandle);
    mViewHandle = nullptr;
  }

  mParentHandle = nullptr;
  mDocument.reset();
  mRedrawRequested = false;

  teardownMetal();
}

void glint_view_mac::initDocument()
{
  createDocument([this]() { requestRedraw(); });

  mDocument->mCanvas.style.display       = "flex";
  mDocument->mCanvas.style.flexDirection = "column";
  updateDocumentBounds();

  if (mOptions.onDocumentCreated)
    mOptions.onDocumentCreated(*mDocument);

#ifndef GLINT_INSPECTOR_DISABLED
  // Ctrl+Shift+I — toggle the inspector for this embedded view's document.
  // Ctrl+Shift+C — open and immediately activate element-picker mode.
  // Mirrors the onGlobalKeyDown setup in glint_window_mac/_createPanelAndView.
  mDocument->onGlobalKeyDown = [this](const glint_key_press& k) -> bool {
    if (k.ctrl && k.shift && k.vk == 'I') {
      glint_document* root = mDocument.get();
      if (glint_insp_bridge::isOpen(root))
        glint_insp_bridge::close(root);
      else
        glint_insp_bridge::open(root);
      return true;
    }
    if (k.ctrl && k.shift && k.vk == 'C') {
      glint_insp_bridge::openAndEnableInspect(mDocument.get());
      return true;
    }
    return false;
  };
#endif
}

void glint_view_mac::resize(int width, int height)
{
  mW = std::max(width,  1);
  mH = std::max(height, 1);
  updateDocumentBounds();

  if (mViewHandle)
  {
    GlintMacView* view = (__bridge GlintMacView*) mViewHandle;
    if (mActiveBackend == glint_backend::Metal && mMetalLayer)
    {
      CAMetalLayer* layer = (__bridge CAMetalLayer*) mMetalLayer;
      const CGFloat scale = layer.contentsScale;
      layer.drawableSize = CGSizeMake(mW * scale, mH * scale);
    }
    if ([NSThread isMainThread])
      [view setFrame:NSMakeRect(0.0, 0.0, mW, mH)];
    else {
      [view retain];  // keep view alive for the duration of the block
      dispatch_async(dispatch_get_main_queue(), ^{
        [view setFrame:NSMakeRect(0.0, 0.0, mW, mH)];
        [view release];
      });
    }
  }

  requestRedraw();
}

void glint_view_mac::requestRedraw()
{
  mRedrawRequested = true;
  if (!mViewHandle)
    return;

  GlintMacView* view = (__bridge GlintMacView*) mViewHandle;
  if ([NSThread isMainThread])
    [view setNeedsDisplay:YES];
  else {
    [view retain];  // keep view alive for the duration of the block
    dispatch_async(dispatch_get_main_queue(), ^{
      [view setNeedsDisplay:YES];  // drawRect: guards cppView == nullptr; safe if cleaned up
      [view release];
    });
  }
}

void glint_view_mac::_paint(void* cgContextRef, void* nsViewRef)
{
  CGContextRef ctx = static_cast<CGContextRef>(cgContextRef);
  if (!ctx || !mDocument || mW <= 0 || mH <= 0)
    return;

  // Resolve backing scale factor for HiDPI rendering.
  CGFloat scale = 1.0;
  if (nsViewRef)
  {
    NSView* view = (__bridge NSView*) nsViewRef;
    if ([view window])
      scale = std::max<CGFloat>([[view window] backingScaleFactor], 1.0);
  }

  const int pw = std::max(1, static_cast<int>(std::lround(static_cast<double>(mW) * scale)));
  const int ph = std::max(1, static_cast<int>(std::lround(static_cast<double>(mH) * scale)));

  SkBitmap bitmap;
  bitmap.allocN32Pixels(pw, ph);

  SkCanvas canvas(bitmap);
  canvas.scale(static_cast<SkScalar>(scale), static_cast<SkScalar>(scale));

  mRedrawRequested = false;
  canvas.clear(mOptions.clearColor);
  mDocument->devicePixelRatio = static_cast<float>(scale);
  mDocument->DrawToCanvas(canvas);

  CGContextSaveGState(ctx);
  CGContextScaleCTM(ctx, 1.0 / scale, 1.0 / scale);
  SkCGDrawBitmap(ctx, bitmap, 0, 0);
  CGContextRestoreGState(ctx);
}

void glint_view_mac::_handleMouseDown(float x, float y, const sk_mouse_mod& mod)
{
  if (!mDocument) return;
  mPrevX = x;
  mPrevY = y;
  mDocument->OnMouseDown(x, y, mod);
  requestRedraw();
}

void glint_view_mac::_handleMouseUp(float x, float y, const sk_mouse_mod& mod)
{
  if (!mDocument) return;
  mDocument->OnMouseUp(x, y, mod);
  requestRedraw();
}

void glint_view_mac::_handleMouseMove(float x, float y, const sk_mouse_mod& mod)
{
  if (!mDocument) return;
  const float dx = x - mPrevX;
  const float dy = y - mPrevY;
  mPrevX = x;
  mPrevY = y;

  if (mod.L)
    mDocument->OnMouseDrag(x, y, dx, dy, mod);
  else
    mDocument->OnMouseOver(x, y, mod, dx, dy);

  // Update the platform cursor to match the element under the pointer.
  glint_view_mac_set_cursor(mDocument->getCursorAtPoint(x, y));

  requestRedraw();
}

void glint_view_mac::_handleMouseLeave()
{
  if (!mDocument) return;
  mDocument->OnMouseOut();
  requestRedraw();
}

void glint_view_mac::_handleScrollWheel(float x, float y, float dx, float dy,
                                        const sk_mouse_mod& mod)
{
  if (!mDocument) return;
  // macOS scrollingDeltaY is positive-up (opposite of DOM deltaY convention).
  // Negate so positive deltaY == scroll down, matching Chrome/DOM behaviour.
  mDocument->OnMouseWheel(x, y, dx, -dy, mod);
  requestRedraw();
}

void glint_view_mac::_handleKeyDown(const glint_key_press& kp)
{
  if (!mDocument) return;
  mDocument->OnKeyDown(kp);
  requestRedraw();
}

void glint_view_mac::_handleKeyUp(const glint_key_press& kp)
{
  if (!mDocument) return;
  mDocument->OnKeyUp(kp);
}

void glint_view_mac::_handleAnimationTimer()
{
  if (mActiveBackend == glint_backend::Metal)
  {
    // Only paint when a redraw has been requested or an animation is running.
    // CSS transitions self-perpetuate: setDirty() called during DrawToCanvas
    // sets mRedrawRequested = true before we clear it, so the loop continues
    // automatically for as long as any transition is in flight.
    if (glint_should_schedule_redraw(mDocument.get(), mRedrawRequested))
      _paintMetal();
    return;
  }
  if (glint_should_schedule_redraw(mDocument.get(), mRedrawRequested))
    requestRedraw();
}

// ---------------------------------------------------------------------------
// Metal backend
// ---------------------------------------------------------------------------

void glint_view_mac::setupMetal(void* /*unused*/)
{
  @autoreleasepool
  {
    // Create Metal device and command queue.
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
    {
      NSLog(@"[glint] setupMetal: MTLCreateSystemDefaultDevice failed");
      return;
    }
    NSLog(@"[glint] setupMetal: device=%@", device.name);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue)
    {
      NSLog(@"[glint] setupMetal: newCommandQueue failed");
      [device release];
      return;
    }

    // Build the Skia GrDirectContext first so we know Metal is actually usable
    // before committing to layer-hosting mode in open().
    GrMtlBackendContext grCtx;
    grCtx.fDevice.retain((__bridge GrMTLHandle) device);
    grCtx.fQueue.retain((__bridge GrMTLHandle) queue);
    mGrContext = GrDirectContexts::MakeMetal(grCtx);

    if (!mGrContext)
    {
      NSLog(@"[glint] setupMetal: GrDirectContexts::MakeMetal failed");
      [queue release];
      [device release];
      return;
    }
    NSLog(@"[glint] setupMetal: GrDirectContext OK");

    // Create and configure the CAMetalLayer.  The view doesn't exist yet so
    // contentsScale defaults to 1; _paintMetal syncs it on the first frame.
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device          = device;
    layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;   // Skia needs full texture access
    layer.opaque          = YES;
    layer.contentsScale   = 1.0;  // corrected in _paintMetal once we have a window
    layer.drawableSize    = CGSizeMake(mW, mH);

    // Retain the layer manually — it will also be retained by [view setLayer:]
    // in open().  teardownMetal() releases this manual retain.
    CFRetain((__bridge CFTypeRef) layer);

    // Transfer ownership of device/queue to the member variables.
    mMetalDevice   = (void*) device;  // +1 from MTLCreateSystemDefaultDevice
    mMetalQueue    = (void*) queue;   // +1 from newCommandQueue
    mMetalLayer    = (__bridge void*) layer;  // +1 from CFRetain above
    mActiveBackend = glint_backend::Metal;
    NSLog(@"[glint] setupMetal: SUCCESS — Metal backend active, drawableSize=%dx%d", mW, mH);
  }
}

void glint_view_mac::teardownMetal()
{
  mGrContext.reset();

  if (mMetalLayer)
  {
    CFRelease(mMetalLayer);  // release the manual retain acquired in setupMetal
    mMetalLayer = nullptr;
  }

  if (mMetalQueue)
  {
    [(id<MTLCommandQueue>) mMetalQueue release];
    mMetalQueue = nullptr;
  }
  if (mMetalDevice)
  {
    [(id<MTLDevice>) mMetalDevice release];
    mMetalDevice = nullptr;
  }

  if (mActiveBackend == glint_backend::Metal)
    mActiveBackend = glint_backend::CPU;
}

void glint_view_mac::_paintMetal()
{
  if (!mGrContext || !mDocument || !mMetalLayer || mW <= 0 || mH <= 0)
    return;

  @autoreleasepool
  {
    CAMetalLayer* layer = (__bridge CAMetalLayer*) mMetalLayer;

    // Sync mW/mH from the layer's actual bounds — AppKit may have autoresized
    // the view (and therefore the layer) after addSubview: or on window resize.
    // drawableSize must exactly cover layer.bounds * contentsScale or the
    // compositor will stretch/compress the drawable to fill the visible area.
    CGFloat scale = layer.contentsScale;
    if ([(__bridge GlintMacView*) mViewHandle window])
    {
      const CGFloat ws = std::max<CGFloat>([[(__bridge GlintMacView*) mViewHandle window] backingScaleFactor], 1.0);
      if (std::fabs(ws - scale) > 0.001)
      {
        layer.contentsScale = ws;
        scale = ws;
      }
    }

    // drawableSize must exactly cover layer.bounds × scale to prevent the
    // compositor from stretching the drawable.  mW/mH are NOT updated here —
    // they hold the declared plugin size (options.width × options.height) and
    // drive document layout.  The view has NSViewNotSizable so layer.bounds
    // should equal mW × mH; this guard handles any edge-case divergence.
    const CGSize needed = CGSizeMake(
        std::lround(layer.bounds.size.width  * scale),
        std::lround(layer.bounds.size.height * scale));
    if (std::fabs(layer.drawableSize.width  - needed.width)  > 0.5 ||
        std::fabs(layer.drawableSize.height - needed.height) > 0.5)
      layer.drawableSize = needed;

    // Grab the current Metal drawable via Skia's WrapCAMetalLayer.
    GrMTLHandle drawableHandle = nullptr;
    auto surface = SkSurfaces::WrapCAMetalLayer(
        mGrContext.get(),
        (__bridge GrMTLHandle) layer,
        kTopLeft_GrSurfaceOrigin,
        1,                      // 1 sample per pixel (Metal requires >= 1; no MSAA)
        kBGRA_8888_SkColorType,
        nullptr,                // color space
        nullptr,                // surface props
        &drawableHandle);

    if (!surface)
      return;

    // Clear the flag BEFORE DrawToCanvas so that any setDirty() call issued
    // during the draw (e.g. from an in-flight CSS transition) re-arms it and
    // keeps the render loop alive for the next frame.
    mRedrawRequested = false;

    // Draw the document into the Metal-backed surface.
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(mOptions.clearColor);
    canvas->save();
    canvas->scale(static_cast<SkScalar>(scale), static_cast<SkScalar>(scale));
    mDocument->devicePixelRatio = static_cast<float>(scale);
    mDocument->DrawToCanvas(*canvas);
    canvas->restore();

    // Flush Skia's pending GPU commands and mark the surface for presentation.
    GrFlushInfo flushInfo{};
    mGrContext->flush(surface.get(), SkSurfaces::BackendSurfaceAccess::kPresent, flushInfo);
    mGrContext->submit();

    // Present the Metal drawable via a new command buffer.
    id<CAMetalDrawable>   drawable      = (__bridge id<CAMetalDrawable>) drawableHandle;
    id<MTLCommandBuffer>  commandBuffer = [(id<MTLCommandQueue>) mMetalQueue commandBuffer];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
  }
}


