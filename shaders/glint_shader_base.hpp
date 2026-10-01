#pragma once

/**
 * glint_shader_base.hpp
 *
 * Abstract base class for all glint SkSL shaders.
 *
 * Unlike the old component-based approach, shaders are NOT scene-graph nodes.
 * They attach to a glint_element via:
 *
 *   comp->style.backdropFilter = "shader(glass, wave)";   // animated backdrop shader
 *   comp->style.filter         = "shader(vign, vignette)";        // procedural overlay
 *
 *   comp->shaders["glass"]->params["strength"] = 20.f;
 *   comp->shaders["glass"]->params["speed"]    = 0.5f;
 *   comp->shaders["vign"]->params["tintColor"] = glint_color(180, 0, 0, 0);
 *
 * The component's draw flow automatically:
 *   1. Parses the filter/backdropFilter string at draw time.
 *   2. Auto-creates missing shader instances from glint_shader_registry.
 *   3. Calls beginBackdropLayer / drawDirect / endBackdropLayer as appropriate.
 *   4. Keeps redraws running via setDirty() while any shader has animated = true.
 *
 * Subclass interface:
 *   - sksl()        — return SkSL source as a string literal
 *   - setUniforms() — map params + time to SkSL uniforms
 *   - sampleRadius()  (backdrop only) — maximum pixel displacement read from src
 *   - onMouseDown() (interactive shaders like ripple)
 *
 * isBackdrop:
 *   true  → shader samples pixels BEHIND the element ("src" = backdrop)
 *   false → shader draws procedurally (no src needed; used for aurora, vignette)
 */


#include "../render/glint_filter.hpp"
#include "../glint_style.hpp"
#include "../glint_graphics.hpp"      // glint_rect, glint_color
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"

#include <chrono>
#include <map>
#include <string>
#include <variant>

using namespace glint_graphics;

// ── Parameter value type ──────────────────────────────────────────────────────
// Covers the three most common SkSL uniform types.
// float  → uniform float / half
// SkV2   → uniform float2 / half2
// glint_color → uniform float4 / half4  (converted to normalised RGBA in setUniforms)
using sk_shader_param = std::variant<float, SkV2, glint_color>;

// ─────────────────────────────────────────────────────────────────────────────

class glint_shader_base
{
public:
  // ── Public data ────────────────────────────────────────────────────────────

  /** Named parameters.  Set before or between draw calls.
   *
  *    comp->shaders["glass"]->params["strength"] = 20.f;
  *    comp->shaders["glass"]->params["tint"]     = glint_color(128, 0, 200, 255);
  *    comp->shaders["glass"]->params["dir"]      = SkV2{1.f, 0.f};
   */
  std::map<std::string, sk_shader_param> params;

  /** Set true in subclass constructor to drive continuous animation.
   *  The host component calls setDirty() every frame while this is true. */
  bool animated = false;

  /** true  = backdrop shader — "src" uniform receives pixels BEHIND the element.
   *  false = procedural shader — no src; draws purely generative content. */
  bool isBackdrop = true;

  virtual ~glint_shader_base() = default;

  // ── Subclass interface ─────────────────────────────────────────────────────

  /** SkSL source code.  Return a string literal; called once on compile(). */
  virtual const char* sksl() const = 0;

  /** Set SkRuntimeShaderBuilder uniforms from params and time each frame.
   *  w, h  = element paint-rect pixel size.
   *  t     = elapsed seconds from the first draw (0 when !animated). */
  virtual void setUniforms(SkRuntimeShaderBuilder& b, float w, float h, float t) = 0;

  /** Maximum pixel offset from coord that sksl() may read from "src".
   *  Non-zero expands the Skia capture region to prevent black edge samples.
   *  Only meaningful for backdrop shaders with displacement. */
  virtual float sampleRadius() const { return 0.f; }

  /** Called when the host element receives OnMouseDown.
   *  Coordinates are in the element's local (content) pixel space. */
  virtual void onMouseDown(float /*localX*/, float /*localY*/) {}

  // ── Helpers for use inside setUniforms() ──────────────────────────────────

  float  getFloat(const std::string& key, float  def = 0.f)        const;
  SkV2   getV2   (const std::string& key, SkV2   def = {0.f,0.f})  const;
  glint_color getColor(const std::string& key, glint_color def = {})          const;

  // ── Draw API (called by glint_element draw flow) ────────────────────────

  /** Compile the SkSL effect once.  Called automatically by the draw flow
   *  the first time a shader token is encountered. */
  void compile();

  /** Open a backdrop-filter save-layer around the element paint rect.
   *  The layer reads the pixels behind the element via the SkSL "src" sampler.
   *  Must be balanced by endBackdropLayer(). */
  void beginBackdropLayer(SkCanvas* canvas, const glint_rect& rect, const glint_style& style);

  /** Close the backdrop-filter save-layer opened by beginBackdropLayer(). */
  void endBackdropLayer(SkCanvas* canvas);

  /** Draw a procedural shader directly into the canvas (non-backdrop shaders).
   *  coord (0,0) maps to rect.L, rect.T after the internal translate. */
  void drawDirect(SkCanvas* canvas, const glint_rect& rect);

  /** Device pixel ratio stamped from glint_document::devicePixelRatio by
   *  glint_element just before each beginBackdropLayer call.  Backdrop shaders
   *  use this to scale pixel-magnitude params (e.g. strength, amplitude) to
   *  physical pixels, since SkImageFilters::RuntimeShader operates in device
   *  pixel space and bypasses the canvas CTM. */
  float                                 mDpr       = 1.f;

protected:
  sk_sp<SkRuntimeEffect>                mEffect;
  float                                 mTime      = 0.f;
  bool                                  mCompiled  = false;
  std::chrono::steady_clock::time_point mStartTime;
  /** Set by beginBackdropLayer / drawDirect before each setUniforms call.
   *  Use this in setUniforms() to convert element-local coords to the
   *  screen-space coord system used by SkSL backdrop shaders. */
  glint_rect                            mCurrentRect;

  float _currentTime();
};

// ─── Inline implementations ───────────────────────────────────────────────────

inline float glint_shader_base::getFloat(const std::string& k, float def) const
{
  auto it = params.find(k);
  if (it == params.end()) return def;
  if (auto* v = std::get_if<float>(&it->second)) return *v;
  return def;
}

inline SkV2 glint_shader_base::getV2(const std::string& k, SkV2 def) const
{
  auto it = params.find(k);
  if (it == params.end()) return def;
  if (auto* v = std::get_if<SkV2>(&it->second)) return *v;
  return def;
}

inline glint_color glint_shader_base::getColor(const std::string& k, glint_color def) const
{
  auto it = params.find(k);
  if (it == params.end()) return def;
  if (auto* v = std::get_if<glint_color>(&it->second)) return *v;
  return def;
}

inline void glint_shader_base::compile()
{
  if (mCompiled) return;
  mCompiled  = true;
  mStartTime = std::chrono::steady_clock::now();
  auto result = SkRuntimeEffect::MakeForShader(SkString(sksl()));
  if (result.effect) mEffect = std::move(result.effect);
}

inline float glint_shader_base::_currentTime()
{
  if (!animated) return 0.f;
  auto now = std::chrono::steady_clock::now();
  mTime = std::chrono::duration<float>(now - mStartTime).count();
  return mTime;
}

inline void glint_shader_base::beginBackdropLayer(SkCanvas* canvas, const glint_rect& rect, const glint_style& style)
{
  if (!canvas || !mEffect) return;
  // All uniforms and mCurrentRect in PHYSICAL pixel space.
  // SkImageFilters::RuntimeShader bypasses the canvas CTM and evaluates in the
  // canvas's LOCAL (pre-CTM) coordinate space.  When the draw loop has applied
  // canvas->scale(mDpr), the filter runs at LOGICAL (1×) resolution and the
  // output is upscaled → visible pixelation on non-100% DPI displays.  The fix:
  // clip first (while the logical CTM is active), then reset the canvas matrix
  // to identity so the saveLayer is opened in DEVICE (physical) pixel space.
  // The filter then evaluates at full physical resolution.  The CTM is restored
  // after saveLayer so content drawn into the layer still uses logical coords.
  const float dpr = mDpr > 0.f ? mDpr : 1.f;
  // Use the full CTM (DPI scale + any scroll/translate transforms) to map the
  // logical rect to physical device pixels.  Simple rect*dpr only works at
  // the top of the scroll tree; once a scroll container has translated the
  // canvas, rect*dpr gives the wrong physical origin.
  const SkMatrix ctm = canvas->getTotalMatrix();
  SkRect physBounds;
  ctm.mapRect(&physBounds, SkRect::MakeLTRB(rect.L, rect.T, rect.R, rect.B));
  mCurrentRect = glint_rect{ physBounds.left(), physBounds.top(),
                              physBounds.right(), physBounds.bottom() };
  const float w = physBounds.width();   // physical width
  const float h = physBounds.height();  // physical height
  SkRuntimeShaderBuilder builder(mEffect);
  setUniforms(builder, w, h, _currentTime());
  const float sr = sampleRadius() * dpr;  // physical sample radius
  sk_sp<SkImageFilter> filter =
    (sr > 0.f)
    ? SkImageFilters::RuntimeShader(builder, sr, "src", nullptr)
    : SkImageFilters::RuntimeShader(builder, "src", nullptr);
  if (!filter) return;

  // ── Step 1: apply the element-shape clip while the logical CTM is still active.
  // ClipBackdropShape uses logical rect / style values; the active scale(mDpr)
  // maps them to physical device pixels correctly.
  canvas->save();                                  // save A: clip guard
  glint_filter::ClipBackdropShape(canvas, rect, style);

  // ── Step 2: reset to identity (device/physical pixel space).
  // The saveLayer must be opened in physical space so that the fBackdrop
  // RuntimeShader filter evaluates once per PHYSICAL pixel, not per LOGICAL pixel.
  const SkMatrix savedCTM = ctm;  // captured before any canvas manipulation
  canvas->save();                                  // save B: CTM guard
  canvas->setMatrix(SkMatrix::I());

  SkCanvas::SaveLayerRec rec(&physBounds, nullptr, filter.get(), 0);
  canvas->saveLayer(rec);                          // save C: the layer (physical res)

  // ── Step 3: restore the logical CTM inside the layer.
  // DrawBackgroundToCanvas, children, etc. still use logical CSS coordinates;
  // the restored scale(mDpr) maps them to the physical layer surface correctly.
  canvas->setMatrix(savedCTM);
}

inline void glint_shader_base::endBackdropLayer(SkCanvas* canvas)
{
  if (!canvas) return;
  canvas->restore();  // close save C: the saveLayer (filter applied at physical res)
  canvas->restore();  // close save B: restore CTM to scale(mDpr)
  canvas->restore();  // close save A: remove clip guard
}

inline void glint_shader_base::drawDirect(SkCanvas* canvas, const glint_rect& rect)
{
  if (!canvas || !mEffect) return;
  mCurrentRect = rect;
  const float w = rect.W(), h = rect.H();
  SkRuntimeShaderBuilder builder(mEffect);
  setUniforms(builder, w, h, _currentTime());
  auto shader = builder.makeShader();
  if (!shader) return;
  SkPaint paint;
  paint.setShader(std::move(shader));
  canvas->save();
  canvas->translate(rect.L, rect.T);
  canvas->drawRect(SkRect::MakeWH(w, h), paint);
  canvas->restore();
}

