#pragma once

/**
 * glint_view_mac.hpp
 * macOS embedded view host for glint_document.
 *
 * glint_view_mac creates an NSView child inside an existing native parent
 * and owns the document, input routing, redraw invalidation, and CPU/Skia
 * render surface lifecycle for that embedded view.
 *
 * The header is pure C++ — no ObjC types appear here.  The ObjC GlintMacView
 * NSView subclass and all Cocoa calls live entirely in glint_view_mac.mm.
 */

#include "../glint_view_base.hpp"

#include <CoreVideo/CVDisplayLink.h>
#include <atomic>
#include <memory>

class glint_view_mac final : public glint_view_base
{
public:
  static std::unique_ptr<glint_view_mac> create(const glint_view_options& options = {})
  {
    auto view = std::unique_ptr<glint_view_mac>(new glint_view_mac(options));
    if (!view->open())
      return nullptr;

    return view;
  }

  ~glint_view_mac() override
  {
    close();
  }

  void* nativeHandle() const override
  {
    return mViewHandle;
  }

  bool isOpen() const
  {
    return mViewHandle != nullptr;
  }

  void resize(int width, int height) override;
  void requestRedraw() override;

  // -------------------------------------------------------------------------
  // Called from the ObjC GlintMacView — not part of the public embedding API.
  // -------------------------------------------------------------------------
  void _paint(void* cgContextRef, void* nsViewRef);
  void _handleMouseDown(float x, float y, const glint_mouse_mod& mod);
  void _handleMouseUp(float x, float y, const glint_mouse_mod& mod);
  void _handleMouseMove(float x, float y, const glint_mouse_mod& mod);
  void _handleMouseLeave();
  void _handleScrollWheel(float x, float y, float dx, float dy, const glint_mouse_mod& mod,
                          bool hasPreciseDeltas = false,
                          glint_input_phase phase = glint_input_phase::none,
                          glint_input_phase momentumPhase = glint_input_phase::none);
  void _handleGesture(float x, float y, glint_gesture_kind kind,
                      glint_input_phase phase, const glint_mouse_mod& mod,
                      float deltaX = 0.f, float deltaY = 0.f,
                      float magnification = 0.f, float rotation = 0.f,
                      bool isInertial = false, bool hasPreciseDeltas = false);
  void _handleKeyDown(const glint_key_press& kp);
  void _handleKeyUp(const glint_key_press& kp);
  void _handleAnimationTimer();
  void _viewDidMoveToWindow(void* nsWindowRef);

  bool _metalEnabled() const { return mActiveBackend == glint_backend::Metal; }
  void _cvDisplayLinkFired();  // called from CVDisplayLink callback thread

private:
  explicit glint_view_mac(const glint_view_options& options)
    : glint_view_base(options)
  {}

  bool open();
  void close();
  void initDocument();

  void setupMetal(void* glintMacViewHandle);
  void teardownMetal();
  void _paintMetal();

  void* mParentHandle    = nullptr;   // NSView* (unretained weak ref)
  void* mViewHandle      = nullptr;   // GlintMacView* (CFRetained)
  CVDisplayLinkRef mDisplayLink = nullptr;
  std::atomic<bool> mFramePending{false};
  bool  mRedrawRequested = false;

  void* mMetalDevice  = nullptr;  // id<MTLDevice>        (+1 retain, released in teardownMetal)
  void* mMetalQueue   = nullptr;  // id<MTLCommandQueue>  (+1 retain, released in teardownMetal)
  void* mMetalLayer   = nullptr;  // CAMetalLayer*        (unretained weak ref to view.layer)
};
