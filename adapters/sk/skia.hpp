#pragma once

/**
 * adapters/sk/skia.hpp
 * Standalone Skia adapter for glint.
 *
 * glint_skia_adapter owns a native OS window and a Skia GPU surface.  It
 * drives glint_document directly from raw Skia (SkCanvas).  This is the adapter used by the glint inspector window and
 * any future standalone glint host.
 *
 * ── Current state ────────────────────────────────────────────────────────────
 * The interface is defined and ready to use.  The Win32 + Skia GPU surface
 * implementation is a work in progress:
 *
 *   open()  — creates a native window, sets up GrDirectContext + SkSurface.
 *   close() — destroys the surface and window.
 *   draw()  — flushes one frame: calls glint_document::Draw() into the SkSurface,
 *              then presents via SwapBuffers / flush.
 *
 * Mouse events are translated from OS messages to glint_mouse_mod and forwarded
 * to glint_document::OnMouseDown/Up/Drag/Over/Out.
 *
 * ── Design notes ─────────────────────────────────────────────────────────────
 * The adapter holds glint_document by value and exposes mRoot publicly so callers
 * can build the scene graph with the same add.* / glint_ctx API used in the
 * standalone app.
 *
 * glint_document::Draw() currently takes glint_canvas& because the drawing helpers
 * (FillRoundRect, DrawText, etc.) are exposed through that seam.  A future sk_ui_canvas
 * abstraction will allow plugging in a raw SkCanvas here.  Until then, the
 * standalone adapter is the right architectural hook but does not yet render
 * glint components — it can host components that draw via SkCanvas directly.
 *
 * ── Usage (planned API) ──────────────────────────────────────────────────────
 *
 *   #include "glint/adapters/sk/skia.hpp"
 *
 *   glint_skia_adapter inspector;
 *   inspector.open("glint Inspector", 460, 660);
 *
 *   // Build the inspector UI — same glint_ctx API.
 *   glint_ctx ctx(&inspector.mRoot.mCanvas);
 *   ctx.add.div([](auto& _c) { _c.innerText = "Hello from inspector"; });
 *
 *   // Per-frame: inspector.draw();
 *   // Cleanup:   inspector.close();
 */

#include "../../glint_document.hpp"

// ── glint_skia_adapter ────────────────────────────────────────────────────────

class glint_skia_adapter
{
public:
  // The scene-graph root.  Build the tree by creating an glint_ctx over
  // mRoot.mCanvas and calling add.* methods (see glint_builder.hpp).
  // mRoot is initialized in open() once the window bounds are known.

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  /**
   * Create a native window of the given size and title, initialise a Skia GPU
   * surface, and start accepting draw/mouse calls.
   *
   * @param title   Window title bar text.
   * @param width   Client area width in pixels.
   * @param height  Client area height in pixels.
   * @param parent  (optional) native parent window handle (HWND / NSView / …).
   *
   * Returns true on success, false if surface creation fails.
   */
  bool open(const char* title, int width, int height, void* parent = nullptr)
  {
    (void)title; (void)width; (void)height; (void)parent;
    // TODO: platform implementation
    //   Win32:  CreateWindowEx → wglCreateContext / D3D11 → GrDirectContext
    //           → SkSurface::MakeFromBackendRenderTarget
    //   macOS:  NSWindow + NSOpenGLContext → GrDirectContext → SkSurface
    return false;
  }

  /** Flush one frame into the native window. */
  void draw()
  {
    // TODO: canvas->clear(SK_ColorTRANSPARENT);
    //       mRoot.Draw(*skCanvas);   // needs sk_ui_canvas abstraction
    //       surface->flush();
    //       SwapBuffers(hdc) / glFlush();
  }

  /** Destroy the Skia surface and native window. */
  void close()
  {
    // TODO: release surface, context, window
  }

  bool isOpen() const { return mOpen; }

private:
  bool mOpen = false;

  // TODO: native window handle (HWND / NSWindow*)
  // TODO: GrDirectContext* mGrContext = nullptr;
  // TODO: sk_sp<SkSurface> mSurface;
  // TODO: glint_document mRoot;  (initialised in open() once bounds are known)
};
