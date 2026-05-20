/**
 * glint_window_mac.mm
 * macOS Objective-C++ implementation of glint_window_mac.
 *
 * Provides:
 *   - GlintWindowMacNSView   : NSView subclass (drawing, input)
 *   - GlintWindowMacDelegate : NSWindowDelegate (window-close notification)
 *   - GlintWindowMacTimer    : NSTimer target
 *   - glint_window_mac member implementations
 *
 * Rendering follows the same CPU-raster pattern used by VCUGlintEditorMac.mm:
 *   SkBitmap → SkCanvas → DrawToCanvas → SkCGDrawBitmap
 */

#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CVDisplayLink.h>
#include <atomic>

#include "glint_window_mac.hpp"
#include "../../glint_bus.hpp"        // glint_insp_bridge declarations
#ifndef GLINT_INSPECTOR_DISABLED
#  include "../../components/glint_builder.hpp"  // glint_component_style + full builder (needed by inspector headers)
#  include "../../inspector/window.hpp"  // real glint_insp_bridge + glint_inspector_window
#endif

// Rename alias: .mm files were written against sk_mouse_mod; headers now use glint_mouse_mod.
using sk_mouse_mod = glint_mouse_mod;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/utils/mac/SkCGUtils.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "include/gpu/ganesh/mtl/SkSurfaceMetal.h"

// ── Forward declarations of ObjC helpers ─────────────────────────────────────
@class GlintWindowMacNSView;
@class GlintWindowMacDelegate;

// ── Cursor helpers ───────────────────────────────────────────────────────────
#import "glint_cursor_mac.h"

static void glint_window_mac_set_cursor(const std::string& css)
{
    [glint_css_to_nscursor(css) set];
}

static sk_mouse_mod modifiersFromFlags(NSEventModifierFlags flags,
                                       bool leftDown, bool rightDown)
{
  sk_mouse_mod m = {};
  m.L = leftDown;
  m.R = rightDown;
  m.S = (flags & NSEventModifierFlagShift)   != 0;
  m.C = (flags & NSEventModifierFlagControl) != 0;
  m.A = (flags & NSEventModifierFlagOption)  != 0;
  m.M = (flags & NSEventModifierFlagCommand) != 0;
  return m;
}

static glint_input_phase phaseFromNSEventPhase(NSEventPhase phase)
{
  if (phase & NSEventPhaseMayBegin) return glint_input_phase::may_begin;
  if (phase & NSEventPhaseBegan)     return glint_input_phase::began;
  if (phase & NSEventPhaseChanged)   return glint_input_phase::changed;
  if (phase & NSEventPhaseEnded)     return glint_input_phase::ended;
  if (phase & NSEventPhaseCancelled) return glint_input_phase::cancelled;
  return glint_input_phase::none;
}

static int virtualKeyFromNSEvent(NSEvent* event)
{
  switch ([event keyCode])
  {
    case 122: return 0x70;  // F1
    case 120: return 0x71;  // F2
    case  99: return 0x72;  // F3
    case 118: return 0x73;  // F4
    case  96: return 0x74;  // F5
    case  97: return 0x75;  // F6
    case  98: return 0x76;  // F7
    case 100: return 0x77;  // F8
    case 101: return 0x78;  // F9
    case 109: return 0x79;  // F10
    case 103: return 0x7A;  // F11
    case 111: return 0x7B;  // F12
    case 123: return 0x25;  // Left
    case 124: return 0x27;  // Right
    case 125: return 0x28;  // Down
    case 126: return 0x26;  // Up
    case 115: return 0x24;  // Home
    case 119: return 0x23;  // End
    case  51: return 0x08;  // Backspace
    case 117: return 0x2E;  // Delete
    case  36:
    case  76: return 0x0D;  // Return / Enter
    case  53: return 0x1B;  // Escape
    case  48: return 0x09;  // Tab
    default: break;
  }
  NSString* chars = [event charactersIgnoringModifiers];
  if ([chars length] == 0) return 0;
  const unichar ch = [chars characterAtIndex:0];
  if (ch >= 0x20 && ch <= 0x7E) return std::toupper(static_cast<unsigned char>(ch));
  if (ch >= 0x01 && ch <= 0x1A) return static_cast<int>(ch + 0x40);
  return 0;
}

static glint_key_press keyPressFromNSEvent(NSEvent* event, bool includeText)
{
  glint_key_press kp = {};
  kp.vk = virtualKeyFromNSEvent(event);
  const NSEventModifierFlags flags = [event modifierFlags];
  kp.shift = (flags & NSEventModifierFlagShift)   != 0;
  kp.ctrl  = (flags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)) != 0;
  kp.alt   = (flags & NSEventModifierFlagOption)  != 0;
  if (!includeText) return kp;
  NSString* chars = [event characters];
  if ([chars length] == 0) return kp;
  NSData* utf8 = [chars dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
  if (!utf8) return kp;
  const NSUInteger len = std::min<NSUInteger>([utf8 length], 4u);
  std::memcpy(kp.utf8, [utf8 bytes], len);
  kp.utf8[len] = '\0';
  return kp;
}

// ── NSTimer target ────────────────────────────────────────────────────────────
@interface GlintWindowMacTimerTarget : NSObject
{
@public
  glint_window_mac* _cpp;
  int               _timerId;
}
- (instancetype)initWithCpp:(glint_window_mac*)cpp timerId:(int)tid;
- (void)timerFired:(NSTimer*)timer;
@end

@implementation GlintWindowMacTimerTarget
- (instancetype)initWithCpp:(glint_window_mac*)cpp timerId:(int)tid
{
  self = [super init];
  _cpp     = cpp;
  _timerId = tid;
  return self;
}
- (void)timerFired:(NSTimer*)timer
{
  (void)timer;
  if (_cpp && _cpp->isRunning())
    _cpp->onTimerFired(_timerId);
}
@end

// ── Popup-style panel subclass ────────────────────────────────────────────────
// NSWindowStyleMaskBorderless panels must override canBecomeKeyWindow so that
// makeKeyAndOrderFront: works and keyboard events reach the content view.
@interface GlintPopupPanel : NSPanel
@end
@implementation GlintPopupPanel
- (BOOL)canBecomeKeyWindow  { return YES; }
- (BOOL)canBecomeMainWindow { return NO;  }
@end

// ── NSView subclass ───────────────────────────────────────────────────────────
@interface GlintWindowMacNSView : NSView <NSTextInputClient>
{
@public
  glint_window_mac*    _cpp;
  NSTrackingArea*      _tracking;
  CVDisplayLinkRef     _displayLink;
  std::atomic<bool>    _framePending;
}
- (instancetype)initWithCpp:(glint_window_mac*)cpp frame:(NSRect)frame;
@end

static CVReturn GlintWindowMacDisplayLinkCB(CVDisplayLinkRef,
                                            const CVTimeStamp*,
                                            const CVTimeStamp*,
                                            CVOptionFlags,
                                            CVOptionFlags*,
                                            void* ctx)
{
  GlintWindowMacNSView* view = (__bridge GlintWindowMacNSView*)ctx;
  if (!view->_cpp || !view->_cpp->isRunning()) return kCVReturnSuccess;
  bool expected = false;
  if (!view->_framePending.compare_exchange_strong(expected, true))
    return kCVReturnSuccess;
  dispatch_async(dispatch_get_main_queue(), ^{
    view->_framePending.store(false);  // clear BEFORE paint so the next tick can queue immediately
    if (!view->_cpp || !view->_cpp->isRunning()) return;
    if (view->_cpp->metalEnabled())
      view->_cpp->paintMetal();
    else
      [view setNeedsDisplay:YES];
  });
  return kCVReturnSuccess;
}

@implementation GlintWindowMacNSView

- (instancetype)initWithCpp:(glint_window_mac*)cpp frame:(NSRect)frame
{
  self = [super initWithFrame:frame];
  if (self) {
    _cpp = cpp;
    _framePending.store(false);
    CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink);
    CVDisplayLinkSetOutputCallback(_displayLink, GlintWindowMacDisplayLinkCB, (__bridge void*)self);
    CVDisplayLinkStart(_displayLink);
  }
  return self;
}

- (void)dealloc
{
  if (_displayLink) {
    CVDisplayLinkStop(_displayLink);
    CVDisplayLinkRelease(_displayLink);
    _displayLink = nullptr;
  }
  if (_tracking) {
    [self removeTrackingArea:_tracking];
    [_tracking release];
    _tracking = nil;
  }
  [super dealloc];
}

- (BOOL)isFlipped           { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void)e; return YES; }
- (BOOL)isOpaque { return _cpp ? (_cpp->clearColorAlpha() == 255) : YES; }

- (BOOL)wantsLayer { return _cpp && _cpp->metalEnabled() ? YES : [super wantsLayer]; }
- (CALayer*)makeBackingLayer
{
  if (_cpp && _cpp->metalEnabled())
    return [CAMetalLayer layer];
  return [super makeBackingLayer];
}

- (void)viewDidMoveToWindow
{
  [super viewDidMoveToWindow];
  if ([self window])
    [[self window] setAcceptsMouseMovedEvents:YES];
}

- (void)updateTrackingAreas
{
  [super updateTrackingAreas];
  if (_tracking) {
    [self removeTrackingArea:_tracking];
    [_tracking release];
    _tracking = nil;
  }
  const NSTrackingAreaOptions opts =
    NSTrackingMouseEnteredAndExited |
    NSTrackingMouseMoved            |
    NSTrackingActiveAlways          |
    NSTrackingInVisibleRect;
  _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                           options:opts
                                             owner:self
                                          userInfo:nil];
  [self addTrackingArea:_tracking];
}

- (void)drawRect:(NSRect)dirtyRect
{
  (void)dirtyRect;
  if (!_cpp) return;
  // Metal renders directly from the CVDisplayLink callback; drawRect: is a no-op in that mode.
  if (_cpp->metalEnabled()) return;
  CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
  CGFloat scale = 1.0;
  if ([self window]) scale = std::max<CGFloat>([[self window] backingScaleFactor], 1.0);
  const NSRect bounds = [self bounds];
  const int pw = std::max(1, static_cast<int>(std::lround(NSWidth(bounds)  * scale)));
  const int ph = std::max(1, static_cast<int>(std::lround(NSHeight(bounds) * scale)));
  _cpp->routeDraw(ctx, pw, ph, static_cast<float>(scale));
}

// ── Mouse ────────────────────────────────────────────────────────────────────
- (void)mouseDown:(NSEvent*)e
{
  [[self window] makeFirstResponder:self];
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeMouseDown((float)p.x, (float)p.y, false,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0);
}
- (void)rightMouseDown:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeMouseDown((float)p.x, (float)p.y, true,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0);
}
- (void)mouseUp:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeMouseUp((float)p.x, (float)p.y, false,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0);
}
- (void)rightMouseUp:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeMouseUp((float)p.x, (float)p.y, true,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0);
}
- (void)mouseDragged:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSUInteger btns = [NSEvent pressedMouseButtons];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeMouseMove((float)p.x, (float)p.y,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0,
    (btns & 1) != 0, (btns & 2) != 0);
}
- (void)mouseMoved:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeMouseMove((float)p.x, (float)p.y,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0,
    false, false);
}
- (void)mouseEntered:(NSEvent*)e
{
  // Re-use mouseMoved to fire OnMouseOver when entering the view
  [self mouseMoved:e];
}
- (void)mouseExited:(NSEvent*)e
{
  (void)e;
  if (_cpp) _cpp->routeMouseLeave();
}
- (void)scrollWheel:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeMouseWheel((float)p.x, (float)p.y,
    (float)([e scrollingDeltaX]),
    (float)([e scrollingDeltaY]),
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0,
    [e hasPreciseScrollingDeltas],
    phaseFromNSEventPhase([e phase]),
    phaseFromNSEventPhase([e momentumPhase]));
}

- (void)beginGestureWithEvent:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeGesture((float)p.x, (float)p.y,
    glint_gesture_kind::none,
    glint_input_phase::began,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0);
}

- (void)endGestureWithEvent:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeGesture((float)p.x, (float)p.y,
    glint_gesture_kind::none,
    glint_input_phase::ended,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0);
}

- (void)magnifyWithEvent:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeGesture((float)p.x, (float)p.y,
    glint_gesture_kind::pinch,
    phaseFromNSEventPhase([e phase]),
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0,
    0.f,
    0.f,
    (float)([e magnification]),
    0.f,
    [e momentumPhase] != NSEventPhaseNone,
    [e hasPreciseScrollingDeltas]);
}

- (void)rotateWithEvent:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeGesture((float)p.x, (float)p.y,
    glint_gesture_kind::rotate,
    phaseFromNSEventPhase([e phase]),
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0,
    0.f,
    0.f,
    0.f,
    (float)([e rotation]));
}

- (void)swipeWithEvent:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeGesture((float)p.x, (float)p.y,
    glint_gesture_kind::swipe,
    glint_input_phase::changed,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0,
    (float)([e deltaX]),
    (float)([e deltaY]));
}

- (void)smartMagnifyWithEvent:(NSEvent*)e
{
  if (!_cpp) return;
  NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
  const NSEventModifierFlags f = [e modifierFlags];
  _cpp->routeGesture((float)p.x, (float)p.y,
    glint_gesture_kind::smart_zoom,
    glint_input_phase::changed,
    (f & NSEventModifierFlagShift) != 0,
    (f & NSEventModifierFlagControl) != 0,
    (f & NSEventModifierFlagOption) != 0,
    (f & NSEventModifierFlagCommand) != 0);
}

// ── Keyboard ─────────────────────────────────────────────────────────────────
- (void)keyDown:(NSEvent*)e
{
  if (!_cpp) return;
  // VK dispatch first (navigation, shortcuts, backspace, etc.).
  _cpp->routeKeyDown(keyPressFromNSEvent(e, false));
  // Then let AppKit resolve any dead-key composition state.
  [self interpretKeyEvents:@[e]];
}
- (void)keyUp:(NSEvent*)e
{
  if (!_cpp) return;
  _cpp->routeKeyUp(virtualKeyFromNSEvent(e));
}

// ── NSTextInputClient ─────────────────────────────────────────────────────────
// Delivers the fully-composed character after AppKit resolves dead-key state.
- (void)insertText:(id)aString replacementRange:(NSRange)rep
{
  (void) rep;
  if (!_cpp) return;
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
  _cpp->routeKeyDown(kp);
}

// Command keys are handled via VK in keyDown: above.
- (void)doCommandBySelector:(SEL)aSelector { (void) aSelector; }

- (void)setMarkedText:(id)s selectedRange:(NSRange)sel replacementRange:(NSRange)rep
  { (void) s; (void) sel; (void) rep; }
- (void)    unmarkText                                          {}
- (NSRange) selectedRange                                       { return NSMakeRange(NSNotFound, 0); }
- (NSRange) markedRange                                         { return NSMakeRange(NSNotFound, 0); }
- (BOOL)    hasMarkedText                                       { return NO; }
- (nullable NSAttributedString*) attributedSubstringForProposedRange:(NSRange) r
                                                        actualRange:(NSRangePointer) ar
  { (void) r; if (ar) *ar = NSMakeRange(NSNotFound, 0); return nil; }
- (NSArray<NSAttributedStringKey>*) validAttributesForMarkedText { return @[]; }
- (NSRect) firstRectForCharacterRange:(NSRange) r actualRange:(NSRangePointer) ar
  { (void) r; if (ar) *ar = NSMakeRange(NSNotFound, 0); return NSZeroRect; }
- (NSUInteger) characterIndexForPoint:(NSPoint) p               { (void) p; return 0; }

@end

// ── Window delegate ───────────────────────────────────────────────────────────
@interface GlintWindowMacDelegate : NSObject <NSWindowDelegate>
{
@public
  glint_window_mac* _cpp;
}
- (instancetype)initWithCpp:(glint_window_mac*)cpp;
@end

@implementation GlintWindowMacDelegate
- (instancetype)initWithCpp:(glint_window_mac*)cpp
{
  self = [super init];
  _cpp = cpp;
  return self;
}
- (void)windowWillClose:(NSNotification*)notification
{
  (void)notification;
  if (_cpp) _cpp->_panelWillClose();
}
- (void)windowDidResize:(NSNotification*)notification
{
  if (!_cpp) return;
  NSWindow* win = (NSWindow*)notification.object;
  const NSRect contentRect = [[win contentView] bounds];
  const int newW = std::max(1, static_cast<int>(std::round(contentRect.size.width)));
  const int newH = std::max(1, static_cast<int>(std::round(contentRect.size.height)));
  _cpp->routeResize(newW, newH);
}
// Auto-dismiss popup-style windows when they lose key status
// (e.g. the user clicks somewhere else).
- (void)windowDidResignKey:(NSNotification*)notification
{
  (void)notification;
  if (_cpp && _cpp->usePopupStyle() && !_cpp->mSuppressAutoClose)
    _cpp->onOutsideClick();
}
@end

// =============================================================================
// glint_window_mac C++ member implementations
// =============================================================================

glint_window_mac::~glint_window_mac()
{
  if (mRunning.load())
    _closePanelAndCleanup();
}

void glint_window_mac::startThread()
{
  if (mRunning.load()) return;
  mW = defaultWidth();
  mH = defaultHeight();
  mRunning.store(true);

  if ([NSThread isMainThread]) {
    _createPanelAndView();
  } else {
    dispatch_sync(dispatch_get_main_queue(), ^{ _createPanelAndView(); });
  }
}

void glint_window_mac::stopThread()
{
  if (!mRunning.load()) return;

  if ([NSThread isMainThread]) {
    _closePanelAndCleanup();
  } else {
    dispatch_async(dispatch_get_main_queue(), ^{ _closePanelAndCleanup(); });
  }
}

void glint_window_mac::requestRedraw()
{
  // Arm the dirty flag so paintMetal() renders on the next CVDisplayLink tick.
  // (setNeedsDisplay: is only used by the CPU-raster fallback path.)
  mRedrawRequested = true;
  GlintWindowMacNSView* view = (__bridge GlintWindowMacNSView*)mViewHandle;
  if (!view) return;
  if ([NSThread isMainThread]) {
    [view setNeedsDisplay:YES];
  } else {
    [view retain];  // keep the view object alive for the duration of the block
    dispatch_async(dispatch_get_main_queue(), ^{
      [view setNeedsDisplay:YES];  // drawRect: guards _cpp == nullptr; safe if cleaned up
      [view release];
    });
  }
}

void glint_window_mac::refreshWindowTitle()
{
  if (usePopupStyle()) return;

  _dispatchMain([this]() {
    NSPanel* panel = (__bridge NSPanel*)mPanelHandle;
    if (!panel) return;

    const char* title = macTitleUTF8();
    NSString* nsTitle = [NSString stringWithUTF8String:(title ? title : "")];
    if (!nsTitle) nsTitle = @"";
    [panel setTitle:nsTitle];
  });
}

void glint_window_mac::setTimer(int timerId, double intervalSec, bool oneShot)
{
  if ([NSThread isMainThread]) {
    _scheduleTimerOnMainThread(timerId, intervalSec, oneShot);
  } else {
    dispatch_async(dispatch_get_main_queue(), ^{
      _scheduleTimerOnMainThread(timerId, intervalSec, oneShot);
    });
  }
}

void glint_window_mac::killTimer(int timerId)
{
  if ([NSThread isMainThread]) {
    _killTimerOnMainThread(timerId);
  } else {
    dispatch_async(dispatch_get_main_queue(), ^{
      _killTimerOnMainThread(timerId);
    });
  }
}

/*static*/
void glint_window_mac::openFileInDefaultApp(const std::string& path)
{
  // Copy the path string before dispatching: the block captures it by value
  // (copy-constructed into the block's closure), so it remains valid even
  // after the caller's local std::string is destroyed.
  std::string pathCopy = path;
  dispatch_async(dispatch_get_main_queue(), ^{
    NSString* nsPath = [NSString stringWithUTF8String:pathCopy.c_str()];
    NSURL* url = [NSURL fileURLWithPath:nsPath];
    [[NSWorkspace sharedWorkspace] openURL:url];
  });
}

void glint_window_mac::_dispatchMain(std::function<void()> fn)
{
  // Heap-allocate the function so it survives across the dispatch boundary.
  auto* heap = new std::function<void()>(std::move(fn));
  dispatch_async(dispatch_get_main_queue(), ^{
    (*heap)();
    delete heap;
  });
}

// ── Screen coordinate helpers ─────────────────────────────────────────────────

// Primary-screen height in points: the denominator for all y-flip conversions.
static CGFloat _primaryScreenHeight()
{
  NSArray<NSScreen*>* screens = [NSScreen screens];
  if (screens.count == 0) return 0.0;
  return screens[0].frame.size.height;
}

RECT glint_window_mac::contentRectToScreen(float x, float y, float w, float h) const
{
  NSView* view = (__bridge NSView*)mViewHandle;
  NSWindow* win = view ? [view window] : nil;
  if (!view || !win) return {};

  // NSView is flipped (isFlipped = YES) so NSView coords are already y-down.
  const NSRect viewRect   = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
  const NSRect winRect    = [view convertRect:viewRect toView:nil];
  const NSRect screenRect = [win convertRectToScreen:winRect];

  // macOS screen: y-up from bottom.  Win32-style: y-down from top of primary screen.
  const CGFloat sh = _primaryScreenHeight();
  const int left   = (int)screenRect.origin.x;
  const int top    = (int)(sh - screenRect.origin.y - screenRect.size.height);
  const int right  = left + (int)screenRect.size.width;
  const int bottom = top  + (int)screenRect.size.height;
  return RECT{left, top, right, bottom};
}

void glint_window_mac::setPanelFrameOrigin(int screenX, int screenYFromTop)
{
  NSPanel* panel = (__bridge NSPanel*)mPanelHandle;
  if (!panel) return;
  const CGFloat sh      = _primaryScreenHeight();
  const CGFloat panelH  = [panel frame].size.height;
  // Win32 yFromTop → macOS y-up: macOS.y = sh - yFromTop - panelHeight
  const CGFloat macY    = sh - (CGFloat)screenYFromTop - panelH;
  [panel setFrameOrigin:NSMakePoint((CGFloat)screenX, macY)];
}

/*static*/
RECT glint_window_mac::screenWorkArea()
{
  NSArray<NSScreen*>* screens = [NSScreen screens];
  if (!screens.count) return {};
  const NSRect wa  = screens[0].visibleFrame;
  const CGFloat sh = screens[0].frame.size.height;
  const int left   = (int)wa.origin.x;
  const int top    = (int)(sh - wa.origin.y - wa.size.height);
  const int right  = (int)(wa.origin.x + wa.size.width);
  const int bottom = (int)(sh - wa.origin.y);
  return RECT{left, top, right, bottom};
}

// ── Input routing (C++ → glint_document) ─────────────────────────────────────

void glint_window_mac::routeMouseDown(float x, float y, bool rightButton,
                                      bool shift, bool ctrl, bool alt, bool cmd)
{
  if (!mOwnRoot) return;
  const bool c = ctrl || cmd;
  sk_mouse_mod m = {};
  m.L = !rightButton; m.R = rightButton;
  m.S = shift; m.C = c; m.A = alt; m.M = cmd;
  mOwnRoot->OnMouseDown(x, y, m);
  requestRedraw();
}

void glint_window_mac::routeMouseUp(float x, float y, bool rightButton,
                                    bool shift, bool ctrl, bool alt, bool cmd)
{
  if (!mOwnRoot) return;
  const bool c = ctrl || cmd;
  sk_mouse_mod m = {};
  m.L = !rightButton; m.R = rightButton;
  m.S = shift; m.C = c; m.A = alt; m.M = cmd;
  mOwnRoot->OnMouseUp(x, y, m);
  requestRedraw();
}

void glint_window_mac::routeMouseMove(float x, float y,
                                      bool shift, bool ctrl, bool alt, bool cmd,
                                      bool leftDown, bool rightDown)
{
  if (!mOwnRoot) return;
  const bool c = ctrl || cmd;
  sk_mouse_mod m = {};
  m.L = leftDown; m.R = rightDown;
  m.S = shift; m.C = c; m.A = alt; m.M = cmd;
  const float dx = x - mPrevX;
  const float dy = y - mPrevY;
  mPrevX = x; mPrevY = y;
  if (leftDown)
    mOwnRoot->OnMouseDrag(x, y, dx, dy, m);
  else
    mOwnRoot->OnMouseOver(x, y, m, dx, dy);
  // Update the platform cursor to match the element under the pointer.
  glint_window_mac_set_cursor(mOwnRoot->getCursorAtPoint(x, y));
  requestRedraw();
}

void glint_window_mac::routeMouseLeave()
{
  if (!mOwnRoot) return;
  mOwnRoot->OnMouseOut();
  requestRedraw();
}

void glint_window_mac::routeMouseWheel(float x, float y, float deltaX, float deltaY,
                                       bool shift, bool ctrl, bool alt, bool cmd,
                                       bool hasPreciseDeltas,
                                       glint_input_phase phase,
                                       glint_input_phase momentumPhase)
{
  if (!mOwnRoot) return;
  const bool c = ctrl || cmd;
  sk_mouse_mod m = {};
  m.S = shift; m.C = c; m.A = alt; m.M = cmd;
  // macOS scrollingDeltaY is positive-up (opposite of DOM deltaY convention).
  // Negate so positive deltaY == scroll down, matching Chrome/DOM behaviour.
  mOwnRoot->OnMouseWheel(x, y, deltaX, -deltaY, m,
                         hasPreciseDeltas, phase, momentumPhase);
  requestRedraw();
}

void glint_window_mac::routeGesture(float x, float y, glint_gesture_kind kind,
                                    glint_input_phase phase,
                                    bool shift, bool ctrl, bool alt, bool cmd,
                                    float deltaX, float deltaY,
                                    float magnification, float rotation,
                                    bool isInertial, bool hasPreciseDeltas)
{
  if (!mOwnRoot) return;
  const bool c = ctrl || cmd;
  sk_mouse_mod m = {};
  m.S = shift; m.C = c; m.A = alt; m.M = cmd;
  mOwnRoot->OnGesture(x, y, kind, phase, m,
                      deltaX, deltaY, magnification, rotation,
                      isInertial, hasPreciseDeltas);
  requestRedraw();
}

void glint_window_mac::routeKeyDown(const glint_key_press& kp)
{
  if (!mOwnRoot) return;

  // Ctrl+Shift+I / Ctrl+Shift+C are handled via mOwnRoot->onGlobalKeyDown
  // (set at document creation time) so no special handling is needed here.

  mOwnRoot->OnKeyDown(kp);
  onKeyDown(kp);   // subclass accelerator hook (e.g. inspector saves, delete, undo)
  requestRedraw();
}

void glint_window_mac::routeKeyUp(int vk)
{
  if (!mOwnRoot) return;
  glint_key_press kp = {};
  kp.vk = vk;
  mOwnRoot->OnKeyUp(kp);
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void glint_window_mac::routeDraw(void* cgCtxRef, int pixelW, int pixelH, float scale)
{
  CGContextRef ctx = static_cast<CGContextRef>(cgCtxRef);
  if (!ctx || !mOwnRoot || pixelW <= 0 || pixelH <= 0) return;

  SkBitmap bitmap;
  bitmap.allocN32Pixels(pixelW, pixelH);
  SkCanvas canvas(bitmap);
  canvas.scale(scale, scale);
  canvas.clear(clearColor());
  mOwnRoot->devicePixelRatio = scale;
  mOwnRoot->DrawToCanvas(canvas);

  CGContextSaveGState(ctx);
  CGContextScaleCTM(ctx, 1.0 / scale, 1.0 / scale);
  SkCGDrawBitmap(ctx, bitmap, 0, 0);
  CGContextRestoreGState(ctx);
}

// ── Resize ────────────────────────────────────────────────────────────────────

void glint_window_mac::routeResize(int newW, int newH)
{
  mW = std::max(newW, 1);
  mH = std::max(newH, 1);
  if (mOwnRoot) {
    const glint_rect bounds(0.f, 0.f, static_cast<float>(mW), static_cast<float>(mH));
    mOwnRoot->mCanvas.mRect       = bounds;
    mOwnRoot->mCanvas.mPaintRECT  = bounds;
    mOwnRoot->mCanvas.mParentW    = static_cast<float>(mW);
    mOwnRoot->mCanvas.mParentH    = static_cast<float>(mH);
    mOwnRoot->setDirty(false);
  }
  requestRedraw();
}

// ── Internal helpers ──────────────────────────────────────────────────────────

void glint_window_mac::_createPanelAndView()
{
  // Create document
  const glint_rect bounds(0.f, 0.f, static_cast<float>(mW), static_cast<float>(mH));
  mOwnRoot = std::make_unique<glint_document>(
    bounds,
    nullptr,
    [this]() { requestRedraw(); });
  mOwnRoot->macWindow = this;
  mOwnRoot->mCanvas.style.display       = "flex";
  mOwnRoot->mCanvas.style.flexDirection = "column";
  mOwnRoot->mCanvas.mRect       = bounds;
  mOwnRoot->mCanvas.mPaintRECT  = bounds;
  mOwnRoot->mCanvas.mParentW    = static_cast<float>(mW);
  mOwnRoot->mCanvas.mParentH    = static_cast<float>(mH);

#ifndef GLINT_INSPECTOR_DISABLED
  // Ctrl+Shift+I — toggle the inspector for this window's root.
  // Ctrl+Shift+C — open and immediately activate element-picker mode.
  // Mirrors the Win32 onGlobalKeyDown setup in glint_window_win32.
  mOwnRoot->onGlobalKeyDown = [this](const glint_key_press& k) -> bool {
    if (k.ctrl && k.shift && k.vk == 'I') {
      glint_document* root = mOwnRoot.get();
      if (glint_insp_bridge::isOpen(root))
        glint_insp_bridge::close(root);
      else
        glint_insp_bridge::open(root);
      return true;
    }
    if (k.ctrl && k.shift && k.vk == 'C') {
      glint_insp_bridge::openAndEnableInspect(mOwnRoot.get());
      return true;
    }
    return false;
  };
#endif

  // Build the subclass UI
  buildUI();

  // Attempt Metal init before the NSView and panel exist.  On failure the
  // CPU raster path is used transparently (mMetalEnabled stays false).
  if (useGpu())
    setupMetal();

  // Create NSPanel
  NSRect frame = NSMakeRect(0, 0, mW, mH);
  NSUInteger style;
  if (usePopupStyle())
    style = NSWindowStyleMaskBorderless;
  else
    style = NSWindowStyleMaskTitled |
            NSWindowStyleMaskClosable |
            NSWindowStyleMaskResizable |
            NSWindowStyleMaskMiniaturizable;
  // Use GlintPopupPanel for borderless popups so canBecomeKeyWindow returns YES
  // (NSWindowStyleMaskBorderless otherwise prevents the window from becoming key,
  // which breaks keyboard input and windowDidResignKey: dismiss).
  NSPanel* panel = usePopupStyle()
    ? [[GlintPopupPanel alloc] initWithContentRect:frame
                                         styleMask:style
                                           backing:NSBackingStoreBuffered
                                             defer:NO]
    : [[NSPanel alloc] initWithContentRect:frame
                                 styleMask:style
                                   backing:NSBackingStoreBuffered
                                     defer:NO];
  if (!usePopupStyle())
    [panel setTitle:[NSString stringWithUTF8String:macTitleUTF8()]];
  [panel setFloatingPanel:YES];
  [panel setBecomesKeyOnlyIfNeeded:NO];
  [panel setHidesOnDeactivate:NO];
  [panel setReleasedWhenClosed:NO];  // we manage lifetime via mPanelHandle
  if (usePopupStyle()) {
    // Ensure the panel sits above the inspector window and accepts key events
    // so keyboard navigation and resign-key dismiss both work.
    [panel setLevel:NSPopUpMenuWindowLevel];
    [panel setAcceptsMouseMovedEvents:YES];

    // Install outside-click monitors so the popup dismisses when the user
    // clicks anywhere outside it.  Both local (same-app) and global (other
    // apps) are needed for full coverage.
    // NOTE: called directly (no dispatch_async) because we are on the main
    // thread and direct calls avoid any dangling-pointer risks from deferred
    // execution after `delete this`.
    glint_window_mac* cppSelf = this;
    id localMon = [NSEvent
        addLocalMonitorForEventsMatchingMask:
            (NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown)
        handler:^NSEvent*(NSEvent* event) {
          NSPanel* p = (__bridge NSPanel*)cppSelf->mPanelHandle;
          if (!p || event.window != p)
            if (cppSelf->isRunning() && !cppSelf->mSuppressAutoClose) cppSelf->onOutsideClick();
          return event;  // always pass the event through
        }];
    mEventMonitor = (void*)CFBridgingRetain(localMon);

    id globalMon = [NSEvent
        addGlobalMonitorForEventsMatchingMask:
            (NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown)
        handler:^(NSEvent*) {
          if (cppSelf->isRunning() && !cppSelf->mSuppressAutoClose) cppSelf->onOutsideClick();
        }];
    mEventMonitorGlobal = (void*)CFBridgingRetain(globalMon);
  }

  // Delegate
  GlintWindowMacDelegate* delegate =
    [[GlintWindowMacDelegate alloc] initWithCpp:this];
  [panel setDelegate:delegate];
  mDelegateHandle = (void*)CFBridgingRetain(delegate);
  [delegate release];  // balance alloc; mDelegateHandle owns the retain

  // Content view
  GlintWindowMacNSView* view =
    [[GlintWindowMacNSView alloc] initWithCpp:this frame:frame];
  [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

  // Layer-hosting mode: assign our pre-configured CAMetalLayer before
  // setWantsLayer:YES and before setContentView: so AppKit never creates a
  // backing layer of the wrong type.
  if (mMetalEnabled)
  {
    [view setLayer:(__bridge CAMetalLayer*) mMetalLayer];
    [view setWantsLayer:YES];
  }

  // If the Skia clear-colour has any transparency (e.g. popup windows with
  // rounded corners), the NSPanel must be non-opaque so transparent pixels
  // composite correctly against whatever is behind the window.
  if (SkColorGetA(clearColor()) < 255) {
    [panel setOpaque:NO];
    [panel setBackgroundColor:[NSColor clearColor]];
  }

  [panel setContentView:view];
  mViewHandle  = (void*)CFBridgingRetain(view);
  [view release];    // balance alloc; mViewHandle owns the retain
  mPanelHandle = (void*)CFBridgingRetain(panel);
  [panel release];   // balance alloc; mPanelHandle owns the retain

  // Position: centred on the primary screen
  [panel center];
  // Popup-style panels must become key so windowDidResignKey: fires when
  // the user clicks elsewhere (enabling auto-dismiss).  Framed inspector
  // windows also benefit from makeKeyAndOrderFront:.
  [panel makeKeyAndOrderFront:nil];

  // Now that the panel is on screen, correct the Metal layer's contentsScale
  // and drawableSize to match the actual HiDPI backing scale factor.
  if (mMetalEnabled && mMetalLayer)
  {
    CAMetalLayer* metalLayer = (__bridge CAMetalLayer*) mMetalLayer;
    CGFloat scale = 1.0;
    if ([panel screen])
      scale = std::max<CGFloat>([[panel screen] backingScaleFactor], 1.0);
    metalLayer.contentsScale = scale;
    metalLayer.drawableSize  = CGSizeMake(mW * scale, mH * scale);
    NSLog(@"[glint] _createPanelAndView: Metal layer contentsScale=%.1f drawableSize=%.0fx%.0f",
          scale, metalLayer.drawableSize.width, metalLayer.drawableSize.height);
  }

  onCreated();
  onThreadStarted();
  requestRedraw();
}

void glint_window_mac::_closePanelAndCleanup()
{
  // Remove outside-click event monitors first (popup only; no-op if nullptr).
  // Must happen before anything else so no new stopThread() calls can be
  // queued from the monitor callbacks during teardown.
  if (mEventMonitor) {
    [NSEvent removeMonitor:(__bridge id)mEventMonitor];
    CFRelease(mEventMonitor);  // balance __bridge_retained
    mEventMonitor = nullptr;
  }
  if (mEventMonitorGlobal) {
    [NSEvent removeMonitor:(__bridge id)mEventMonitorGlobal];
    CFRelease(mEventMonitorGlobal);  // balance __bridge_retained
    mEventMonitorGlobal = nullptr;
  }

  onDestroyed();
  onThreadEnded();

  // Invalidate all glint timers
  for (auto& [timerId, timerPtr] : mTimers) {
    if (timerPtr) {
      NSTimer* t = (__bridge NSTimer*)timerPtr;
      [t invalidate];
      CFRelease(timerPtr);
    }
  }
  mTimers.clear();

  // Close panel
  if (mPanelHandle) {
    NSPanel* panel = (__bridge NSPanel*)mPanelHandle;
    [panel setDelegate:nil];
    [panel close];
    CFRelease(mPanelHandle);  // balance __bridge_retained
    mPanelHandle = nullptr;
  }

  // Release view
  if (mViewHandle) {
    GlintWindowMacNSView* view = (__bridge GlintWindowMacNSView*)mViewHandle;
    if (view->_displayLink) {
      CVDisplayLinkStop(view->_displayLink);
      CVDisplayLinkRelease(view->_displayLink);
      view->_displayLink = nullptr;
    }
    view->_cpp = nullptr;
    CFRelease(mViewHandle);  // balance __bridge_retained
    mViewHandle = nullptr;
  }

  // Release delegate
  if (mDelegateHandle) {
    GlintWindowMacDelegate* del = (__bridge GlintWindowMacDelegate*)mDelegateHandle;
    del->_cpp = nullptr;
    CFRelease(mDelegateHandle);  // balance __bridge_retained
    mDelegateHandle = nullptr;
  }

  mOwnRoot.reset();
  teardownMetal();
  mRunning.store(false);
  afterRun();
}

void glint_window_mac::_panelWillClose()
{
  // NSPanel is closing (user clicked the red button).
  // Trigger the same cleanup sequence as stopThread().
  if (!mRunning.load()) return;
  // Detach panel so _closePanelAndCleanup doesn't call [panel close] again.
  if (mPanelHandle) {
    NSPanel* panel = (__bridge NSPanel*)mPanelHandle;
    [panel setDelegate:nil];
    CFRelease(mPanelHandle);  // balance __bridge_retained
    mPanelHandle = nullptr;
  }
  onDestroyed();
  onThreadEnded();

  // Invalidate glint timers
  for (auto& [timerId, timerPtr] : mTimers) {
    if (timerPtr) {
      NSTimer* t = (__bridge NSTimer*)timerPtr;
      [t invalidate];
      CFRelease(timerPtr);
    }
  }
  mTimers.clear();

  // Release view
  if (mViewHandle) {
    GlintWindowMacNSView* view = (__bridge GlintWindowMacNSView*)mViewHandle;
    if (view->_displayLink) {
      CVDisplayLinkStop(view->_displayLink);
      CVDisplayLinkRelease(view->_displayLink);
      view->_displayLink = nullptr;
    }
    view->_cpp = nullptr;
    CFRelease(mViewHandle);  // balance __bridge_retained
    mViewHandle = nullptr;
  }

  // Release delegate
  if (mDelegateHandle) {
    GlintWindowMacDelegate* del = (__bridge GlintWindowMacDelegate*)mDelegateHandle;
    del->_cpp = nullptr;
    CFRelease(mDelegateHandle);  // balance __bridge_retained
    mDelegateHandle = nullptr;
  }

  mOwnRoot.reset();
  teardownMetal();
  mRunning.store(false);
  afterRun();
}

void glint_window_mac::_scheduleTimerOnMainThread(int timerId, double intervalSec, bool oneShot)
{
  // Invalidate any existing timer with this ID
  _killTimerOnMainThread(timerId);

  GlintWindowMacTimerTarget* target =
    [[GlintWindowMacTimerTarget alloc] initWithCpp:this timerId:timerId];
  NSTimer* t = [NSTimer timerWithTimeInterval:intervalSec
                                       target:target
                                     selector:@selector(timerFired:)
                                     userInfo:nil
                                      repeats:!oneShot];
  [target release];
  [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
  mTimers[timerId] = (void*)CFBridgingRetain(t);   // cast away const from CFTypeRef
}

void glint_window_mac::_killTimerOnMainThread(int timerId)
{
  auto it = mTimers.find(timerId);
  if (it == mTimers.end()) return;
  if (it->second) {
    NSTimer* t = (__bridge NSTimer*)it->second;
    [t invalidate];
    CFRelease(it->second);
  }
  mTimers.erase(it);
}

// ── Panel show / hide ─────────────────────────────────────────────────────────

void glint_window_mac::showPanel()
{
  if ([NSThread isMainThread]) {
    mSuppressAutoClose = false;   // re-enable outside-click auto-dismiss
    if (mPanelHandle)
      [(__bridge NSWindow*)mPanelHandle makeKeyAndOrderFront:nil];
  } else {
    _dispatchMain([this] {
      mSuppressAutoClose = false;
      if (mPanelHandle)
        [(__bridge NSWindow*)mPanelHandle makeKeyAndOrderFront:nil];
    });
  }
}

void glint_window_mac::hidePanel()
{
  // Must be called from the main thread.
  // Leave mSuppressAutoClose = true after orderOut: so subsequent outside-click
  // monitor firings (while the panel is hidden) don't trigger stopThread().
  // showPanel() clears the flag.
  mSuppressAutoClose = true;
  if (mPanelHandle)
    [(__bridge NSWindow*)mPanelHandle orderOut:nil];
}

// ── Platform services ─────────────────────────────────────────────────────────

// ObjC helper for showContextMenu — defined at file scope (outside any namespace).
@interface _GlintMenuTracker : NSObject
@property (atomic) int selectedId;
- (void)onItem:(NSMenuItem*)item;
@end
@implementation _GlintMenuTracker
- (void)onItem:(NSMenuItem*)item { self.selectedId = (int)item.tag; }
@end

namespace glint_platform {

static NSString* _glintNSStringFromUtf8(const std::string& utf8)
{
  return utf8.empty() ? nil : [NSString stringWithUTF8String:utf8.c_str()];
}

static NSArray<NSString*>* _glintAllowedFileTypes(const std::vector<std::string>& extensions)
{
  if (extensions.empty()) return nil;

  NSMutableArray<NSString*>* types = [NSMutableArray arrayWithCapacity:extensions.size()];
  for (const std::string& ext : extensions) {
    if (ext.empty()) continue;
    std::string normalized = ext;
    if (!normalized.empty() && normalized.front() == '.')
      normalized.erase(normalized.begin());
    NSString* nsExt = _glintNSStringFromUtf8(normalized);
    if (nsExt && [nsExt length] > 0)
      [types addObject:nsExt];
  }
  return [types count] > 0 ? types : nil;
}

static NSWindow* _glintDialogAnchorWindow()
{
  NSWindow* window = [NSApp keyWindow];
  if (!window)
    window = [NSApp mainWindow];
  if (!window) {
    NSEvent* event = [NSApp currentEvent];
    window = event ? [event window] : nil;
  }
  return window;
}

static void _glintPrepareForModalDialog()
{
  [NSApp activateIgnoringOtherApps:YES];
  if (NSWindow* window = _glintDialogAnchorWindow())
    [window makeKeyAndOrderFront:nil];
}

static std::string _glintRunOpenPanel(bool chooseFiles,
                                      bool chooseDirectories,
                                      const std::vector<std::string>& extensions,
                                      const std::string& title)
{
  __block std::string result;
  auto run = ^{
    _glintPrepareForModalDialog();

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:chooseFiles ? YES : NO];
    [panel setCanChooseDirectories:chooseDirectories ? YES : NO];
    [panel setAllowsMultipleSelection:NO];
    [panel setCanCreateDirectories:chooseDirectories ? YES : NO];
    if (NSString* nsTitle = _glintNSStringFromUtf8(title))
      [panel setTitle:nsTitle];
    if (NSArray<NSString*>* fileTypes = _glintAllowedFileTypes(extensions))
      [panel setAllowedFileTypes:fileTypes];

    if ([panel runModal] != NSModalResponseOK) return;
    NSString* path = [[panel URL] path];
    if (!path) return;
    const char* utf8 = [path UTF8String];
    if (utf8) result = utf8;
  };

  if ([NSThread isMainThread]) run();
  else dispatch_sync(dispatch_get_main_queue(), run);
  return result;
}

std::string showOpenFileDialog(const std::vector<std::string>& extensions,
                               const std::string& title,
                               bool allowDirectories)
{
  return _glintRunOpenPanel(true, allowDirectories, extensions, title);
}

std::string showSaveFileDialog(const std::vector<std::string>& extensions,
                               const std::string& defaultExtension,
                               const std::string& title,
                               const std::string& suggestedPath)
{
  __block std::string result;
  auto run = ^{
    _glintPrepareForModalDialog();

    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setCanCreateDirectories:YES];
    [panel setAllowsOtherFileTypes:YES];
    [panel setCanSelectHiddenExtension:YES];
    [panel setExtensionHidden:NO];
    if (NSString* nsTitle = _glintNSStringFromUtf8(title))
      [panel setTitle:nsTitle];
    if (!suggestedPath.empty()) {
      std::filesystem::path fsPath(suggestedPath);
      if (NSString* dir = _glintNSStringFromUtf8(fsPath.parent_path().string())) {
        NSURL* dirURL = [NSURL fileURLWithPath:dir isDirectory:YES];
        if (dirURL)
          [panel setDirectoryURL:dirURL];
      }
      if (NSString* fileName = _glintNSStringFromUtf8(fsPath.filename().string())) {
        if ([fileName length] > 0)
          [panel setNameFieldStringValue:fileName];
      }
    }
    if (!defaultExtension.empty()) {
      std::string normalized = defaultExtension;
      if (!normalized.empty() && normalized.front() == '.')
        normalized.erase(normalized.begin());
      if (NSString* ext = _glintNSStringFromUtf8(normalized))
        if ([[panel nameFieldStringValue] length] == 0)
        [panel setNameFieldStringValue:[@"Untitled." stringByAppendingString:ext]];
    }

    if ([panel runModal] != NSModalResponseOK) return;
    NSString* path = [[panel URL] path];
    if (!path) return;
    std::string chosenPath;
    if (const char* utf8 = [path UTF8String])
      chosenPath = utf8;

    if (!chosenPath.empty() && !defaultExtension.empty()) {
      std::string normalized = defaultExtension;
      if (!normalized.empty() && normalized.front() == '.')
        normalized.erase(normalized.begin());

      std::filesystem::path fsPath(chosenPath);
      std::string currentExt = fsPath.extension().string();
      std::transform(currentExt.begin(), currentExt.end(), currentExt.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      const std::string wantedExt = "." + normalized;
      bool hasAllowedExt = false;
      for (const std::string& ext : extensions) {
        std::string normalizedExt = ext;
        if (!normalizedExt.empty() && normalizedExt.front() != '.')
          normalizedExt.insert(normalizedExt.begin(), '.');
        std::transform(normalizedExt.begin(), normalizedExt.end(), normalizedExt.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (currentExt == normalizedExt) {
          hasAllowedExt = true;
          break;
        }
      }

      if (currentExt.empty() || !hasAllowedExt) {
        fsPath.replace_extension(wantedExt);
        chosenPath = fsPath.string();
      }
    }

    result = std::move(chosenPath);
  };

  if ([NSThread isMainThread]) run();
  else dispatch_sync(dispatch_get_main_queue(), run);
  return result;
}

std::string showOpenFolderDialog(const std::string& title)
{
  return _glintRunOpenPanel(false, true, {}, title);
}

void showAlertDialog(const std::string& title, const std::string& message)
{
  auto run = ^{
    _glintPrepareForModalDialog();

    NSAlert* alert = [[NSAlert alloc] init];
    if (NSString* nsTitle = _glintNSStringFromUtf8(title))
      [alert setMessageText:nsTitle];
    if (NSString* nsMessage = _glintNSStringFromUtf8(message))
      [alert setInformativeText:nsMessage];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
  };

  if ([NSThread isMainThread]) run();
  else dispatch_sync(dispatch_get_main_queue(), run);
}

confirm_dialog_result showConfirmDialog(const std::string& title,
                                        const std::string& message,
                                        const std::string& primaryButton,
                                        const std::string& secondaryButton,
                                        const std::string& cancelButton)
{
  __block confirm_dialog_result result = confirm_dialog_result::cancel;
  auto run = ^{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    if (NSString* nsTitle = _glintNSStringFromUtf8(title))
      [alert setMessageText:nsTitle];
    if (NSString* nsMessage = _glintNSStringFromUtf8(message))
      [alert setInformativeText:nsMessage];

    NSString* primary = _glintNSStringFromUtf8(primaryButton);
    NSString* secondary = _glintNSStringFromUtf8(secondaryButton);
    NSString* cancel = _glintNSStringFromUtf8(cancelButton);
    [alert addButtonWithTitle:primary ? primary : @"OK"];
    [alert addButtonWithTitle:secondary ? secondary : @"No"];
    [alert addButtonWithTitle:cancel ? cancel : @"Cancel"];

    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn)
      result = confirm_dialog_result::primary;
    else if (response == NSAlertSecondButtonReturn)
      result = confirm_dialog_result::secondary;
    else
      result = confirm_dialog_result::cancel;
  };

  if ([NSThread isMainThread]) run();
  else dispatch_sync(dispatch_get_main_queue(), run);
  return result;
}

void setClipboardText(const std::string& utf8)
{
  NSPasteboard* pb = [NSPasteboard generalPasteboard];
  [pb clearContents];
  NSString* str = [NSString stringWithUTF8String:utf8.c_str()];
  if (str) [pb setString:str forType:NSPasteboardTypeString];
}

std::string getClipboardText()
{
  NSString* str = [[NSPasteboard generalPasteboard]
                    stringForType:NSPasteboardTypeString];
  if (!str) return {};
  const char* u = [str UTF8String];
  return u ? u : std::string{};
}

int showContextMenu(int screenX, int screenY,
                    const std::vector<std::pair<int, std::string>>& items,
                    const std::vector<int>& disabledIds,
                    const std::vector<int>& checkedIds)
{
  int selected = 0;
  auto run = [&]() {
    _GlintMenuTracker* tracker = [[_GlintMenuTracker alloc] init];
    tracker.selectedId = 0;

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    [menu setAutoenablesItems:NO];
    for (auto& [itemId, label] : items) {
      if (itemId == 0 && label == "-") {
        [menu addItem:[NSMenuItem separatorItem]];
      } else {
        NSString* title = [NSString stringWithUTF8String:label.c_str()];
        NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:title
                                                    action:@selector(onItem:)
                                             keyEquivalent:@""];
        mi.tag    = itemId;
        mi.target = tracker;
        const bool disabled =
          std::find(disabledIds.begin(), disabledIds.end(), itemId) != disabledIds.end();
        if (disabled) { [mi setEnabled:NO]; mi.action = nil; }
        const bool checked =
          std::find(checkedIds.begin(), checkedIds.end(), itemId) != checkedIds.end();
        if (checked) mi.state = NSControlStateValueOn;
        [menu addItem:mi];
      }
    }

    NSWindow* kw   = [NSApp keyWindow];
    // When the Glint view is embedded in a foreign host (e.g. Reaper AU),
    // [NSApp keyWindow] may return nil because the host window is not the
    // AppKit-tracked key window.  Fall back to the window of the current
    // event (the right-click that triggered this menu) which is always
    // the correct window regardless of key-window state.
    if (!kw) {
      NSEvent* ev = [NSApp currentEvent];
      kw = ev ? [ev window] : nil;
    }
    NSView*   view = kw ? [kw contentView] : nil;
    if (!view) return;

    // Prefer the current NSEvent (right-click) so the menu appears at the
    // exact cursor position.  Fall back to explicit screen coordinates if no
    // current event is available (e.g. programmatic open).
    NSEvent* event = [NSApp currentEvent];
    if (event && (event.type == NSEventTypeRightMouseDown ||
                  event.type == NSEventTypeOtherMouseDown)) {
      [NSMenu popUpContextMenu:menu withEvent:event forView:view];
    } else {
      // Coordinate-based fallback.
      // If no explicit screen position was supplied (0,0), use the current
      // cursor location so programmatic opens (e.g. left-click hamburger button)
      // appear at the pointer rather than the top-left corner of the screen.
      NSPoint screenPt;
      if (screenX == 0 && screenY == 0) {
        screenPt = [NSEvent mouseLocation];
      } else {
        NSArray<NSScreen*>* screens = [NSScreen screens];
        const CGFloat sh = screens.count > 0 ? screens[0].frame.size.height : 0.0;
        screenPt = NSMakePoint((CGFloat)screenX, sh - (CGFloat)screenY);
      }
      NSPoint winPt  = [kw convertPointFromScreen:screenPt];
      NSPoint viewPt = [view convertPoint:winPt fromView:nil];
      [menu popUpMenuPositioningItem:nil atLocation:viewPt inView:view];
    }
    selected = tracker.selectedId;
  };

  if ([NSThread isMainThread]) run();
  else dispatch_sync(dispatch_get_main_queue(), ^{ run(); });
  return selected;
}

int showSelectMenu(int screenX, int screenY,
                   const std::vector<std::pair<int, std::string>>& items,
                   int selectedId,
                   const std::vector<int>& disabledIds)
{
  const std::vector<int> checkedIds = selectedId > 0
    ? std::vector<int>{selectedId}
    : std::vector<int>{};
  return showContextMenu(screenX, screenY, items, disabledIds, checkedIds);
}

} // namespace glint_platform

// =============================================================================
// Metal rendering
// =============================================================================

void glint_window_mac::setupMetal()
{
  @autoreleasepool
  {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
    {
      NSLog(@"[glint] glint_window_mac::setupMetal: MTLCreateSystemDefaultDevice failed");
      return;
    }
    NSLog(@"[glint] glint_window_mac::setupMetal: device=%@", device.name);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue)
    {
      NSLog(@"[glint] glint_window_mac::setupMetal: newCommandQueue failed");
      [device release];
      return;
    }

    GrMtlBackendContext grCtx;
    grCtx.fDevice.retain((__bridge GrMTLHandle) device);
    grCtx.fQueue.retain((__bridge GrMTLHandle) queue);
    mGrContext = GrDirectContexts::MakeMetal(grCtx);

    if (!mGrContext)
    {
      NSLog(@"[glint] glint_window_mac::setupMetal: GrDirectContexts::MakeMetal failed");
      [queue release];
      [device release];
      return;
    }
    NSLog(@"[glint] glint_window_mac::setupMetal: GrDirectContext OK");

    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device          = device;
    layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;   // Skia needs full texture access
    layer.opaque          = (clearColorAlpha() == 255) ? YES : NO;
    layer.contentsScale   = 1.0;  // corrected after makeKeyAndOrderFront:
    layer.drawableSize    = CGSizeMake(mW, mH);

    CFRetain((__bridge CFTypeRef) layer);  // released in teardownMetal()

    mMetalDevice  = (void*) device;   // +1 from MTLCreateSystemDefaultDevice
    mMetalQueue   = (void*) queue;    // +1 from newCommandQueue
    mMetalLayer   = (__bridge void*) layer;  // +1 from CFRetain
    mMetalEnabled = true;
    NSLog(@"[glint] glint_window_mac::setupMetal: SUCCESS — Metal backend active, drawableSize=%dx%d",
          mW, mH);
  }
}

void glint_window_mac::teardownMetal()
{
  mGrContext.reset();

  if (mMetalLayer)
  {
    CFRelease(mMetalLayer);
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
  mMetalEnabled = false;
}

// Returns true when a new frame should be rendered.  Mirrors the logic in
// glint_view_mac::_handleAnimationTimer() / glint_should_schedule_redraw().
static bool glint_window_should_redraw(glint_document* doc, bool redrawRequested)
{
  if (redrawRequested) return true;
  glint_element* focused = doc ? doc->getFocusedNode() : nullptr;
  if (!focused || !focused->wantsPeriodicRedraw()) return false;
  return std::chrono::steady_clock::now() >= focused->nextPeriodicRedrawTime();
}

void glint_window_mac::paintMetal()
{
  if (!mGrContext || !mOwnRoot || !mMetalLayer || mW <= 0 || mH <= 0)
    return;

  // Skip the render when nothing has changed — avoids drawing the entire scene
  // graph every tick when the UI is idle.  The previous frame remains on the
  // CAMetalLayer, so the display stays correct.
  if (!glint_window_should_redraw(mOwnRoot.get(), mRedrawRequested))
    return;

  @autoreleasepool
  {
    CAMetalLayer* layer = (__bridge CAMetalLayer*) mMetalLayer;

    // Sync contentsScale with the window's current backing scale factor.
    GlintWindowMacNSView* view = (__bridge GlintWindowMacNSView*) mViewHandle;
    CGFloat scale = layer.contentsScale;
    if (view && [view window])
    {
      const CGFloat ws = std::max<CGFloat>([[view window] backingScaleFactor], 1.0);
      if (std::fabs(ws - scale) > 0.001)
      {
        layer.contentsScale = ws;
        scale = ws;
      }
    }

    // drawableSize must exactly cover layer.bounds × scale.
    const CGSize needed = CGSizeMake(
        std::lround(layer.bounds.size.width  * scale),
        std::lround(layer.bounds.size.height * scale));
    if (std::fabs(layer.drawableSize.width  - needed.width)  > 0.5 ||
        std::fabs(layer.drawableSize.height - needed.height) > 0.5)
      layer.drawableSize = needed;

    GrMTLHandle drawableHandle = nullptr;
    auto surface = SkSurfaces::WrapCAMetalLayer(
        mGrContext.get(),
        (__bridge GrMTLHandle) layer,
        kTopLeft_GrSurfaceOrigin,
        1,                        // sample count (no MSAA)
        kBGRA_8888_SkColorType,
        nullptr,                  // color space
        nullptr,                  // surface props
        &drawableHandle);

    if (!surface)
      return;

    // Clear the flag BEFORE DrawToCanvas so that any setDirty() fired during
    // the draw (e.g. from a CSS transition) re-arms it for the next frame.
    mRedrawRequested = false;

    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(clearColor());
    canvas->save();
    canvas->scale(static_cast<SkScalar>(scale), static_cast<SkScalar>(scale));
    mOwnRoot->devicePixelRatio = static_cast<float>(scale);
    mOwnRoot->DrawToCanvas(*canvas);
    canvas->restore();

    GrFlushInfo flushInfo{};
    mGrContext->flush(surface.get(), SkSurfaces::BackendSurfaceAccess::kPresent, flushInfo);
    mGrContext->submit();

    id<CAMetalDrawable>  drawable      = (__bridge id<CAMetalDrawable>) drawableHandle;
    id<MTLCommandBuffer> commandBuffer = [(id<MTLCommandQueue>) mMetalQueue commandBuffer];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
  }
}
