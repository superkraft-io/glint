#pragma once

/**
 * glint_svg_draw_cache.hpp
 * Draws SVGs through two caches instead of walking the SVG DOM every frame.
 *
 * Measured on the demo (200 icons at 24 px, D3D12): plain boxes 0.7 ms a
 * frame, 2-path icons 4.6 ms, a 41-path illustration with gradients 43 ms.
 *
 *  - Picture: the DOM rendered once into an SkPicture per container size
 *    (the drawing commands, resolution independent) and replayed. Saves the
 *    DOM walk at any size, also while it animates.
 *  - Bitmap: when an element draws its SVG at the same device size and
 *    sub-pixel position for kStableFrames frames, with only scale and
 *    translation on the canvas, the picture is rasterized once at exactly
 *    those device pixels (the fractional offset baked in) and drawn as an
 *    img, pixel aligned. Elements drawing the same SVG at the same size share
 *    it. Larger than kMaxRasterPixels, rotated, or changing size every frame
 *    (an animation, a live resize): the picture is drawn instead.
 *
 * GLINT_SVG_CACHE=0 renders the DOM every time instead (no caching).
 *
 * Callers keep a glint_svg_draw_state per drawn SVG: it holds what it drew
 * (so glint_trim_svg_draw_cache() keeps it) and counts stable frames.
 * Cache entries nothing holds are dropped at idle, except the most recently
 * used within a small budget.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

/** Per element (per drawn SVG) state; see the header comment. */
struct glint_svg_draw_state
{
  sk_sp<SkPicture> picture;     // what was drawn last
  sk_sp<SkImage>   image;       // its rasterized form, when cached
  // The device placement the stable-frame count is for.
  const SkPicture* placedPicture = nullptr;
  float            placedScaleX = 0.f, placedScaleY = 0.f;
  int              placedFracX = 0, placedFracY = 0;
  int              stableFrames = 0;
};

namespace glint_svg_draw_cache
{
constexpr int kStableFrames    = 2;
constexpr int kMaxRasterPixels = 512 * 512;
constexpr int kFracSteps       = 16;   // sub-pixel positions told apart

using clock = std::chrono::steady_clock;

struct picture_entry { sk_sp<SkPicture> picture; clock::time_point used; };
struct image_entry   { sk_sp<SkPicture> picture; sk_sp<SkImage> image; clock::time_point used; };

// (dom, container w, container h)
using picture_key = std::tuple<const SkSVGDOM*, float, float>;
// (picture, scale x, scale y, frac x, frac y)
using image_key   = std::tuple<const SkPicture*, float, float, int, int>;

inline std::mutex& mutex() { static std::mutex m; return m; }
inline std::map<picture_key, picture_entry>& pictures() { static std::map<picture_key, picture_entry> m; return m; }
inline std::map<image_key, image_entry>& images() { static std::map<image_key, image_entry> m; return m; }

/** The DOM's drawing at `container` size, recorded once. `region` (SVG
 *  units) bounds what it draws. */
inline sk_sp<SkPicture> picture(SkSVGDOM& dom, SkSize container, const SkRect& region)
{
  const picture_key key{ &dom, container.width(), container.height() };
  std::lock_guard<std::mutex> lock(mutex());
  auto& map = pictures();
  auto it = map.find(key);
  if (it != map.end()) { it->second.used = clock::now(); return it->second.picture; }
  SkPictureRecorder recorder;
  SkCanvas* rec = recorder.beginRecording(region);
  dom.setContainerSize(container);
  dom.render(rec);
  sk_sp<SkPicture> pic = recorder.finishRecordingAsPicture();
  if (pic) map[key] = { pic, clock::now() };
  return pic;
}

/** `pic` rasterized under `device` (scale + translate) at `dims` device
 *  pixels from `origin` (the integer device position it is drawn at). */
inline sk_sp<SkImage> image(const sk_sp<SkPicture>& pic, const SkMatrix& device, SkIPoint origin, SkISize dims,
                            int fracX, int fracY)
{
  const image_key key{ pic.get(), device.getScaleX(), device.getScaleY(), fracX, fracY };
  {
    std::lock_guard<std::mutex> lock(mutex());
    auto it = images().find(key);
    if (it != images().end() && it->second.image && it->second.image->dimensions() == dims)
    {
      it->second.used = clock::now();
      return it->second.image;
    }
  }
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(dims))) return nullptr;
  bitmap.eraseColor(SK_ColorTRANSPARENT);
  {
    SkCanvas canvas(bitmap);
    canvas.translate(static_cast<float>(-origin.x()), static_cast<float>(-origin.y()));
    canvas.concat(device);
    canvas.drawPicture(pic);
  }
  bitmap.setImmutable();
  sk_sp<SkImage> img = bitmap.asImage();
  if (!img) return nullptr;
  std::lock_guard<std::mutex> lock(mutex());
  images()[key] = { pic, img, clock::now() };
  return img;
}
} // namespace glint_svg_draw_cache

/**
 * Draws `dom` as `canvas->concat(local); dom->setContainerSize(container);
 * dom->render(canvas)` would, inside a layer with `layerPaint` when given
 * (fill tint, blend mode). `region` (SVG units) bounds what the SVG draws.
 */
inline void glint_draw_svg(SkCanvas* canvas, SkSVGDOM& dom, SkSize container, const SkRect& region,
                           const SkMatrix& local, const SkPaint* layerPaint, glint_svg_draw_state& state)
{
  namespace c = glint_svg_draw_cache;
  // GLINT_SVG_CACHE=0: render the DOM every time, without either cache.
  static const bool cacheOff = [] { const char* v = std::getenv("GLINT_SVG_CACHE"); return v && v[0] == '0'; }();
  if (cacheOff)
  {
    canvas->save();
    canvas->concat(local);
    if (layerPaint) canvas->saveLayer(nullptr, layerPaint);
    dom.setContainerSize(container);
    dom.render(canvas);
    if (layerPaint) canvas->restore();
    canvas->restore();
    return;
  }
  sk_sp<SkPicture> pic = c::picture(dom, container, region);
  if (!pic) return;
  state.picture = pic;

  // Bitmap when placed the same as the last kStableFrames frames.
  const SkMatrix device = SkMatrix::Concat(canvas->getTotalMatrix(), local);
  if (device.isScaleTranslate() && device.getScaleX() > 0.f && device.getScaleY() > 0.f)
  {
    const SkRect devRect = device.mapRect(region);
    const SkIPoint origin{ static_cast<int>(std::floor(devRect.left())), static_cast<int>(std::floor(devRect.top())) };
    const SkISize dims{ static_cast<int>(std::ceil(devRect.right())) - origin.x(),
                        static_cast<int>(std::ceil(devRect.bottom())) - origin.y() };
    const int fracX = static_cast<int>(std::lround((devRect.left() - origin.x()) * c::kFracSteps));
    const int fracY = static_cast<int>(std::lround((devRect.top() - origin.y()) * c::kFracSteps));
    if (!dims.isEmpty() && static_cast<int64_t>(dims.width()) * dims.height() <= c::kMaxRasterPixels)
    {
      const bool same = state.placedPicture == pic.get() && state.placedScaleX == device.getScaleX()
                     && state.placedScaleY == device.getScaleY() && state.placedFracX == fracX
                     && state.placedFracY == fracY;
      if (!same)
      {
        state.placedPicture = pic.get();
        state.placedScaleX = device.getScaleX(); state.placedScaleY = device.getScaleY();
        state.placedFracX = fracX; state.placedFracY = fracY;
        state.stableFrames = 0;
        state.image = nullptr;
      }
      if (++state.stableFrames >= c::kStableFrames)
      {
        if (!state.image || state.image->dimensions() != dims)
          state.image = c::image(pic, device, origin, dims, fracX, fracY);
        if (state.image)
        {
          canvas->save();
          canvas->resetMatrix();
          canvas->drawImage(state.image, static_cast<float>(origin.x()), static_cast<float>(origin.y()),
                            SkSamplingOptions(SkFilterMode::kNearest), layerPaint);
          canvas->restore();
          return;
        }
      }
    }
    else
    {
      state.image = nullptr;
      state.stableFrames = 0;
    }
  }
  else
  {
    state.image = nullptr;
    state.stableFrames = 0;
  }

  canvas->save();
  canvas->concat(local);
  if (layerPaint) canvas->saveLayer(nullptr, layerPaint);
  canvas->drawPicture(pic);
  if (layerPaint) canvas->restore();
  canvas->restore();
}

/** Drops cache entries no element holds (see the header comment). */
inline void glint_trim_svg_draw_cache()
{
  namespace c = glint_svg_draw_cache;
  constexpr size_t kUnusedBudgetBytes = 4 * 1024 * 1024;
  const auto now = c::clock::now();
  std::lock_guard<std::mutex> lock(c::mutex());

  // Bitmaps: keep the most recently used unheld ones within the budget.
  std::vector<std::pair<c::clock::time_point, c::image_key>> unheld;
  for (auto& [key, e] : c::images())
    if (e.image && e.image->unique() && now - e.used >= std::chrono::seconds(5)) unheld.push_back({ e.used, key });
  std::sort(unheld.begin(), unheld.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  size_t kept = 0;
  for (const auto& [used, key] : unheld)
  {
    const auto& e = c::images()[key];
    const size_t bytes = e.image->imageInfo().computeMinByteSize();
    if (kept + bytes <= kUnusedBudgetBytes) { kept += bytes; continue; }
    c::images().erase(key);
  }
  // Pictures: unheld (by elements or bitmaps) and unused for a while.
  for (auto it = c::pictures().begin(); it != c::pictures().end();)
    if (it->second.picture->unique() && now - it->second.used >= std::chrono::seconds(5)) it = c::pictures().erase(it);
    else ++it;
}
