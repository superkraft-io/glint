#pragma once

/**
 * glint_bg_shader.hpp
 * Base component for procedural SkSL background shaders.
 *
 * The SkSL shader is drawn in DrawContentToCanvas(), filling the element's
 * paint rect. Coord (0,0) = element top-left; (w,h) = element bottom-right.
 *
 * Subclass template:
 *   class MyShader : public glint_bg_shader {
 *   public:
 *     MyShader() { mAnimated = true; }
 *   protected:
 *     const char* sksl() const override { return R"(
 *       uniform float2 resolution;
 *       uniform float  time;
 *       half4 main(float2 coord) { ... }
 *     )"; }
 *     void setUniforms(SkRuntimeShaderBuilder& b, float w, float h, float t) override {
 *       b.uniform("resolution") = SkV2{w, h};
 *       b.uniform("time")       = t;
 *     }
 *   };
 *
 * Tip: Set style.backgroundColor = glint_color(0,0,0,0) if the shader provides
 * all the colour. Otherwise the normal background is drawn behind it.
 */


#include <chrono>
#include "../glint_element.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/effects/SkRuntimeEffect.h"

class glint_bg_shader : public glint_element
{
public:
  const char* typeName() const override { return "bg-shader"; }

  /** Set true to drive continuous animation via mTime. Each frame calls setDirty(). */
  bool mAnimated = false;

protected:
  sk_sp<SkRuntimeEffect>                          mEffect;
  float                                           mTime     = 0.f;
  bool                                            mCompiled = false;
  std::chrono::steady_clock::time_point           mStartTime;

  // ── Subclass interface ────────────────────────────────────────────────────
  /** SkSL source code. Called once on first compile. */
  virtual const char* sksl() const = 0;

  /** Set uniforms on the builder each frame before the shader is drawn. */
  virtual void setUniforms(SkRuntimeShaderBuilder& b, float w, float h, float t) = 0;

  // ── Internal ──────────────────────────────────────────────────────────────
  void _compile()
  {
    if (mCompiled) return;
    mCompiled  = true;
    mStartTime = std::chrono::steady_clock::now();
    auto result = SkRuntimeEffect::MakeForShader(SkString(sksl()));
    if (result.effect) mEffect = std::move(result.effect);
  }

public:
  glint_bg_shader()
  {
    _compile();
  }

  // Draws the SkSL shader over the element's paint rect.
  // Called by the base DrawToCanvas after DrawBackgroundToCanvas.
  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (!mEffect || !canvas) return;

    if (mAnimated)
    {
      using namespace std::chrono;
      mTime = duration<float>(steady_clock::now() - mStartTime).count();
    }

    const float w = mPaintRECT.W();
    const float h = mPaintRECT.H();
    if (w <= 0.f || h <= 0.f) return;

    SkRuntimeShaderBuilder builder(mEffect);
    setUniforms(builder, w, h, mTime);

    auto shader = builder.makeShader();
    if (!shader) return;

    SkPaint paint;
    paint.setShader(std::move(shader));

    // Translate into element-local space so SkSL coord (0,0) = top-left.
    canvas->save();
    canvas->translate(mPaintRECT.L, mPaintRECT.T);
    canvas->drawRect(SkRect::MakeWH(w, h), paint);
    canvas->restore();

    if (mAnimated) setDirty(false);
  }
};

