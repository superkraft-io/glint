#pragma once

/**
 * glint_view_linux.hpp
 * Linux embedded view host for glint_document.
 *
 * glint_view_linux manages a glint_document and a CPU Skia render surface.
 * In this initial implementation the view renders into a CPU SkBitmap but
 * does not create its own native X11 child window.  Embedding into an
 * external native parent can be added in a follow-up once the required
 * event-routing interface is defined.
 *
 * For the demo application, only glint_window_linux (not glint_view_linux)
 * is required; this class exists to satisfy the glint_view.hpp interface
 * and allow the project to link cleanly.
 */

#include "../glint_view_base.hpp"

class glint_view_linux final : public glint_view_base
{
public:
  static std::unique_ptr<glint_view_linux> create(const glint_view_options& options = {})
  {
    auto view = std::unique_ptr<glint_view_linux>(new glint_view_linux(options));
    if (!view->open())
      return nullptr;
    return view;
  }

  ~glint_view_linux() override = default;

  void* nativeHandle() const override { return nullptr; }

  void resize(int width, int height) override
  {
    if (width <= 0 || height <= 0) return;
    mW   = width;
    mH   = height;
    mWpx = width;
    mHpx = height;
    recreateCpuSurface();
    updateDocumentBounds();
  }

  void requestRedraw() override
  {
    // No native window to invalidate; callers that own the render loop
    // may poll hasDocument() + document().DrawToCanvas(*canvas).
  }

private:
  explicit glint_view_linux(const glint_view_options& options)
    : glint_view_base(options)
  {}

  bool open()
  {
    recreateCpuSurface();
    createDocument([this]{ requestRedraw(); });
    if (mOptions.onDocumentCreated && mDocument)
      mOptions.onDocumentCreated(*mDocument);
    return static_cast<bool>(mDocument);
  }
};
