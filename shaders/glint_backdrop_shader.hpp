#pragma once

/**
 * glint_backdrop_shader.hpp
 * Base component for SkSL shaders that sample and transform the pixels
 * already drawn *behind* the element (the "backdrop").
 *
 * Uses Skia's SaveLayerRec::fBackdrop + SkImageFilters::RuntimeShader.
 * The SkSL shader receives backdrop pixels via a child shader named "src":
 *
 *   uniform shader src;
 *   half4 main(float2 coord) {
 *     // coord is in screen-pixel space
 *     return src.eval(coord + float2(offset, 0.0));  // shift right
 *   }
 *
 * Set sampleRadius() to the maximum pixel offset your shader may read from
 * "src" so Skia can pre-expand the capture region far enough (otherwise
 * samples at the element edges will return black).
 *
 * The element's style.backgroundColor is drawn on top of the processed
 * backdrop (use a semi-transparent glint_color for glass tint).
 *
 * Subclass template:
 *   class MyShader : public glint_backdrop_shader {
 *   public:
 *     MyShader() { mAnimated = true; }
 *   protected:
 *     float  sampleRadius()  const override { return 20.f; }
 *     const char* sksl()     const override { return R"(...)"; }
 *     void setUniforms(SkRuntimeShaderBuilder& b, float w, float h, float t) override { ... }
 *   };
 *
 * Limitations:
 *   - style.filter on this element is not forwarded — use adjacent elements.
 *   - style.transform is ignored in this DrawToCanvas override.
 *   - Children are drawn inside the backdrop layer (appear crisp over backdrop).
 */


#include <chrono>
#include "../glint_element.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkImageFilters.h"

class glint_backdrop_shader : public glint_element
{
public:
  const char* typeName() const override { return "backdrop-shader"; }

  /** Set true to drive continuous animation via mTime. Each frame calls setDirty(). */
  bool mAnimated = false;

protected:
  sk_sp<SkRuntimeEffect>                mEffect;
  float                                 mTime     = 0.f;
  bool                                  mCompiled = false;
  std::chrono::steady_clock::time_point mStartTime;

  // ── Subclass interface ────────────────────────────────────────────────────

  /** Maximum pixel distance from the current coord at which "src" is sampled.
   *  Skia uses this to pre-expand the backdrop capture region so samples at
   *  the element boundary don't return black. Override with your shader's
   *  maximum displacement in pixels. */
  virtual float sampleRadius() const { return 0.f; }

  /** SkSL source. Must declare "uniform shader src;" as the backdrop input. */
  virtual const char* sksl() const = 0;

  /** Set uniforms on the builder each frame. */
  virtual void setUniforms(SkRuntimeShaderBuilder& b, float w, float h, float t) = 0;

  // ── Internal ─────────────────────────────────────────────────────────────
  void _compile()
  {
    if (mCompiled) return;
    mCompiled  = true;
    mStartTime = std::chrono::steady_clock::now();
    auto result = SkRuntimeEffect::MakeForShader(SkString(sksl()));
    if (result.effect) mEffect = std::move(result.effect);
  }

public:
  glint_backdrop_shader()
  {
    _compile();
  }

  // Full DrawToCanvas override — inserts the backdrop-shader saveLayer around
  // all normal drawing (background tint, content, children).
  void DrawToCanvas(SkCanvas* canvas) override
  {
    if (!canvas) return;
    tickTransitions();
    if (computedStyle.display == "none") return;

    // Graceful fallback if compilation failed.
    if (!mEffect) { glint_element::DrawToCanvas(canvas); return; }

    if (mAnimated)
    {
      using namespace std::chrono;
      mTime = duration<float>(steady_clock::now() - mStartTime).count();
    }

    const float dpr = (mRoot && mRoot->devicePixelRatio > 0.f) ? mRoot->devicePixelRatio : 1.f;
    // Use the full CTM (DPI scale + scroll/translate) to map the logical rect to
    // physical device pixels.  rect*dpr only works at the top of the scroll tree.
    const SkMatrix ctm = canvas->getTotalMatrix();
    const SkRect logRect = SkRect::MakeLTRB(mPaintRECT.L, mPaintRECT.T,
                                            mPaintRECT.R, mPaintRECT.B);  // logical
    SkRect physRect;
    ctm.mapRect(&physRect, logRect);  // physical, accounts for scroll + DPI
    const float w = physRect.width();   // physical width
    const float h = physRect.height();  // physical height
    if (w <= 0.f || h <= 0.f) return;

    // Build the image filter from the runtime shader.
    SkRuntimeShaderBuilder builder(mEffect);
    setUniforms(builder, w, h, mTime);

    const float sr = sampleRadius() * dpr;  // physical sample radius
    auto filter = (sr > 0.f)
      ? SkImageFilters::RuntimeShader(builder, sr, "src", nullptr)
      : SkImageFilters::RuntimeShader(builder, "src", nullptr);

    if (!filter) { glint_element::DrawToCanvas(canvas); return; }

    // Step 1: clip in logical space (CTM = scale(mDpr) is active here).
    canvas->save();                     // save A: clip guard
    canvas->clipRect(logRect);

    // Step 2: reset to identity so the saveLayer is in physical pixel space.
    const SkMatrix savedCTM = ctm;  // captured before any canvas manipulation
    canvas->save();                     // save B: CTM guard
    canvas->setMatrix(SkMatrix::I());

    // Open the backdrop layer in physical pixel space.
    SkCanvas::SaveLayerRec rec(&physRect, nullptr, filter.get(), 0);
    canvas->saveLayer(rec);             // save C: the layer

    // Step 3: restore the logical CTM inside the layer.
    canvas->setMatrix(savedCTM);

    // Draw element background (glass tint, border-radius etc.) on top.
    DrawBackgroundToCanvas(canvas);

    // Custom content hook (text overlays, icons, etc.).
    DrawContentToCanvas(canvas);

    // Children render crisp on top of the distorted/filtered backdrop.
    for (auto& child : mChildren)
      child->DrawToCanvas(canvas);

    canvas->restore(); // close save C: saveLayer (filter applied at physical res)
    canvas->restore(); // close save B: restore CTM
    canvas->restore(); // close save A: remove clip guard

    if (mAnimated) setDirty(false);
  }
};

