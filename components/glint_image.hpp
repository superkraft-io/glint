  // Keeps req.responseData alive alongside bitmap: the Skia img wrapper wraps
#pragma once

/**
 * glint_image.hpp
 * A component that renders a bitmap img (glint_bitmap) with CSS-inspired fit modes.
 *
 * Inspired by the JS glint_image class: loads an img from a URL, renders it
 * inside a styled panel with configurable fit, alignment, and aspect control.
 *
 * Usage (via builder):
 *
 *   _c.add.img([](glint_image_style& _c) {
 *     _c.style.left   = 8.f;  _c.style.top    = 8.f;
 *     _c.style.width  = 64.f; _c.style.height = 64.f;
 *     _c.src                       = "img/my_logo.png";  // auto-loaded by builder
 *     _c.style.objectFit           = "contain";     // default: contain | cover | fill | none
 *     _c.style.objectPosition      = "left center"; // default: center center
 *     _c.style.backgroundColor     = "#00000000";   // transparent (default)
 *   });
 *
 * Direct construction:
 *
 *   glint_image_style s;
 *   s.bitmap = pGraphics->LoadBitmap(LOGO_FN, 1);
 *   s.fit    = glint_image_style::Fit::Contain;
 *   auto* imgCtrl = new glint_image(glint_rect(8, 8, 72, 72), s);
 *   pGraphics->AttachControl(imgCtrl);
 *
 * Fit modes (mirrors CSS object-fit):
 *
 *   Contain  - Uniform scale so the full img fits inside the bounds, centred.
 *              Empty space is shown (letterboxing / pillarboxing).
 *   Cover    - Uniform scale so the img fills the entire bounds.
 *              Excess is cropped symmetrically.
 *   Fill     - Stretch to exactly fill the bounds (no aspect preservation).
 *   None     - Draw at the bitmap's natural pixel size, aligned by alignH/alignV.
 */

#include "../glint_element.hpp"
#include "../render/glint_resource_cache.hpp"
#include "../render/glint_resource_request.hpp"
#include "../render/glint_svg_cache.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

// ─── glint_image ─────────────────────────────────────────────────────────────

class glint_image : public glint_element
{
  // Fit modes (mirrors CSS object-fit) — internal implementation detail.
  enum class Fit { Contain, Cover, Fill, None };

  static Fit parseFit(const std::string& s)
  {
    if (s == "contain") return Fit::Contain;
    if (s == "cover")   return Fit::Cover;
    if (s == "none")    return Fit::None;
    return Fit::Fill; // default — matches Chrome <img> object-fit:fill
  }

  static void parsePosition(const std::string& s, EAlign& h, EVAlign& v)
  {
    std::istringstream ss(s);
    std::string tok;
    int i = 0;
    while (ss >> tok && i < 2)
    {
      std::string low = tok;
      for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if      (low == "left")                    h = EAlign::Near;
      else if (low == "right")                   h = EAlign::Far;
      else if (low == "top")                     v = EVAlign::Top;
      else if (low == "bottom")                  v = EVAlign::Bottom;
      else if (low == "middle")                  v = EVAlign::Middle;
      else if (low == "center" && i == 0)        h = EAlign::Center;
      else if (low == "center" && i == 1)        v = EVAlign::Middle;
      ++i;
    }
  }

public:
  // ── Asset fields — set in the add.img() callback ───────────────────────
  int         tag       = kNoTag;
  std::string src;                // resource path — auto-loaded by the builder
  glint_bitmap     bitmap;             // populated from src by the builder, or set directly
  int         numFrames = 1;      // sprite-sheet frame count (passed to LoadBitmap)

private:
  // Keeps req.responseData alive alongside bitmap: the Skia img wrapper wraps
  // the raw byte pointer with SkData::MakeWithoutCopy, so the SkImage_Lazy
  // inside glint_bitmap holds a non-owning pointer to these bytes.  If the SkData
  // were freed before the GPU-side lazy decode, we'd get a use-after-free
  // (crash inside SkPngCodec / memcpy).  This member extends the lifetime.
  sk_sp<SkData> _bitmapData;

public:

  glint_image() = default;
  explicit glint_image(const glint_rect& bounds) { mRect = mPaintRECT = bounds; }

  void SetBitmap(const glint_bitmap& bmp) { bitmap = bmp; _loaded = true; setDirty(false); }

  // Assign a new src at runtime — resets the loaded state so the next draw
  // re-fires onRequest (mirroring img.src = "..." in a browser).
  void SetSrc(const char* newSrc, int frames = 1)
  {
    src         = newSrc ? newSrc : "";
    numFrames   = frames;
    _svgImg     = glint_svg{ nullptr };
    bitmap      = glint_bitmap{};
    _bitmapData = nullptr;  // release retained SkData
    _loaded     = false;    // trigger lazy reload on next drawContent
    setDirty(false);
  }

  // Load (or reload) the asset at src, firing onRequest first.
  // Must be called after the element is attached to a document (mRoot set)
  // so that _getOnRequest() is valid.
  void _loadSrc(glint_canvas* g)
  {
    if (src.empty()) return;

    _svgImg = glint_svg{ nullptr };
    bitmap  = glint_bitmap{};

    const bool isSVG = src.size() >= 4 &&
                       src.compare(src.size() - 4, 4, ".svg") == 0;

    const auto resourceType = isSVG ? glint_resource_request::Type::SVG
                                    : glint_resource_request::Type::Image;
    GlintCachedResource cachedRes;
    if (glint_resource_cache_lookup(src, resourceType, &cachedRes))
    {
      if (cachedRes.data)
      {
        if (isSVG)
          _svgImg = glint_load_svg_cached(src, cachedRes.data);
        else
        {
          _bitmapData = cachedRes.data;
          bitmap = g ? g->LoadBitmap(src.c_str(),
                                     cachedRes.data->data(),
                                     static_cast<int>(cachedRes.data->size()),
                                     numFrames)
                     : glint_graphics::LoadBitmapFromData(cachedRes.data, numFrames);
        }
      }
      _loaded = true;
      setDirty(false);
      return;
    }

    if (auto* cb = _getOnRequest())
    {
      if (*cb)
      {
        // onRequest is registered — fire it as the sole authority.
        // Use req.handled to detect whether the handler responded.
        // If handled && responseData → decode and display.
        // If handled && !responseData → explicit error, silent 404.
        // If !handled → handler deliberately passed through; fall to disk.
        glint_resource_request req;
        req.url    = src;
        req.parseUrl();
        req.type   = resourceType;
        req.source = this;
        (*cb)(req);
        // If the handler returned without calling any respond helper, auto-stamp a
        // 500 error so the network log always records a meaningful status.
        if (!req.handled) req.error(500, "Handler did not respond to request");
        glint_network_log_push(_getNetworkLog(), src, req.type, req,
                              mId, mTag, typeName(), id.c_str());
        glint_resource_cache_store(src, req.type, req);
        // handler is always the sole authority — no disk fallback.
        if (req.responseData)
        {
          if (isSVG)
            _svgImg = glint_load_svg_cached(src, req.responseData);
          else
          {
            _bitmapData = req.responseData;
            bitmap = g ? g->LoadBitmap(src.c_str(),
                                       req.responseData->data(),
                                       static_cast<int>(req.responseData->size()),
                                       numFrames)
                       : glint_graphics::LoadBitmapFromData(req.responseData, numFrames);
          }
        }
        // no responseData → silent 404
        _loaded = true;
        setDirty(false);
        return;
      }
    }

    // No handler registered — fall back to direct disk / embedded resource load.
    auto data = SkData::MakeFromFileName(src.c_str());
    glint_resource_cache_store_disk(src, resourceType, data, src);
    if (isSVG)
    {
      _svgImg = data ? glint_load_svg_cached(src, data) : glint_svg{};
      glint_network_log_push_disk(_getNetworkLog(), src,
                                  glint_resource_request::Type::SVG,
                                  _svgImg.IsValid(),
                                  mId, mTag, typeName(), id.c_str());
    }
    else
    {
      if (data)
      {
        _bitmapData = data;
        bitmap = g ? g->LoadBitmap(src.c_str(),
                                   data->data(),
                                   static_cast<int>(data->size()),
                                   numFrames)
                   : glint_graphics::LoadBitmapFromData(data, numFrames);
      }
      else
      {
        bitmap = {};
      }
      glint_network_log_push_disk(_getNetworkLog(), src,
                                  glint_resource_request::Type::Image,
                                  bitmap.IsValid(),
                                  mId, mTag, typeName(), id.c_str());
    }
    _loaded = true;
    setDirty(false);
  }

  const char* typeName() const override { return "img"; }
  const char* tagName()  const override { return "img"; }

protected:
  void drawContent(glint_canvas& g) override
  {
    if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext()))
    {
      DrawContentToCanvas(canvas);
      return;
    }

    // ── Lazy load: deferred until first draw so onRequest is always set ───
    // (mirrors browser behaviour: resource fetches fire after the document
    //  is fully set up, not during HTML parsing / DOM construction)
    if (!_loaded && !src.empty())
      _loadSrc(&g);

    // ── SVG path: src ends in ".svg" ─────────────────────────────────────
    if (_svgImg.IsValid())
    {
      const glint_rect content = getContent();
      const float svgW = _svgImg.W();
      const float svgH = _svgImg.H();
      Fit     fit    = parseFit(style.objectFit);
      EAlign  alignH = EAlign::Center;
      EVAlign alignV = EVAlign::Middle;
      parsePosition(style.objectPosition, alignH, alignV);
      glint_rect dest = content;
      switch (fit)
      {
        case Fit::Contain:
          if (svgW > 0.f && svgH > 0.f) {
            const float scale = std::min(content.W() / svgW, content.H() / svgH);
            dest = AlignRect(content, svgW * scale, svgH * scale, alignH, alignV);
          }
          break;
        case Fit::Cover:
          if (svgW > 0.f && svgH > 0.f) {
            const float scale = std::max(content.W() / svgW, content.H() / svgH);
            dest = AlignRect(content, svgW * scale, svgH * scale, alignH, alignV);
          }
          break;
        case Fit::Fill:
          dest = content;
          break;
        case Fit::None:
          if (svgW > 0.f && svgH > 0.f)
            dest = AlignRect(content, svgW, svgH, alignH, alignV);
          break;
      }
      const glint_color* fillPtr = style.fill.isSet ? &style.fill.value : nullptr;
      g.DrawSVG(_svgImg, dest, nullptr, nullptr, fillPtr);
      return;
    }

    // ── Raster bitmap path ────────────────────────────────────────────────
    if (!bitmap.GetAPIBitmap()) return;

    const glint_rect content = getContent();
    const float bmpW    = static_cast<float>(bitmap.FW());
    const float bmpH    = static_cast<float>(bitmap.FH());
    if (bmpW <= 0.f || bmpH <= 0.f) return;

    Fit     fit = parseFit(style.objectFit);
    EAlign  alignH = EAlign::Center;
    EVAlign alignV = EVAlign::Middle;
    parsePosition(style.objectPosition, alignH, alignV);

    switch (fit)
    {
      // ── Contain: uniform scale, full img visible, centred ───────────────
      case Fit::Contain:
      {
        const float scale = std::min(content.W() / bmpW, content.H() / bmpH);
        const float fw    = bmpW * scale;
        const float fh    = bmpH * scale;
        const glint_rect dest  = AlignRect(content, fw, fh, alignH, alignV);
        g.DrawFittedBitmap(bitmap, dest);
        break;
      }

      // ── Cover: uniform scale, fills bounds, crop symmetrically ────────────
      case Fit::Cover:
      {
        const float scale = std::max(content.W() / bmpW, content.H() / bmpH);
        const float fw    = bmpW * scale;
        const float fh    = bmpH * scale;

        // Source crop: which region of the original bitmap to draw.
        // We pick the centre-aligned crop box in bitmap-pixel space.
        const float cropX = (fw - content.W()) / 2.f / scale;
        const float cropY = (fh - content.H()) / 2.f / scale;
        const float cropW = content.W() / scale;
        const float cropH = content.H() / scale;

        // The legacy host path clipped drawing to the control's mRect, so the over-sized
        // dest rect is automatically cropped to the content bounds.
        const float dl = content.L - cropX * scale;
        const float dt = content.T - cropY * scale;
        g.DrawFittedBitmap(bitmap, glint_rect(dl, dt, dl + fw, dt + fh));
        break;
      }

      // ── Fill: stretch to exactly fill content bounds ───────────────────────
      case Fit::Fill:
      {
        g.DrawFittedBitmap(bitmap, content);
        break;
      }

      // ── None: natural pixel size, aligned ────────────────────────────────
      case Fit::None:
      {
        const glint_rect dest = AlignRect(content, bmpW, bmpH, alignH, alignV);
        // DrawBitmap with srcX=0, srcY=0 draws the first frame at natural size.
        g.DrawBitmap(bitmap, dest, 0, 0);
        break;
      }
    }
  }

  // Raw-Skia counterpart to drawContent(glint_canvas&).
  // Called by _drawToCanvasImpl when this element is inside a CSS stacking context
  // (parent with transform or opacity < 1).  Replicates the objectFit logic using
  // canvas drawing primitives so the asset remains visible inside Skia saveLayer composites.
  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    // Lazy-load via the cached glint_canvas pointer (same deferral contract as drawContent).
    if (!_loaded && !src.empty())
      _loadSrc(mpG);

    if (_svgImg.IsValid())
    {
      const glint_rect content = getContent();
      const float svgW = _svgImg.W();
      const float svgH = _svgImg.H();
      Fit     fit    = parseFit(style.objectFit);
      EAlign  alignH = EAlign::Center;
      EVAlign alignV = EVAlign::Middle;
      parsePosition(style.objectPosition, alignH, alignV);

      glint_rect dest = content;
      switch (fit)
      {
        case Fit::Contain:
          if (svgW > 0.f && svgH > 0.f)
          {
            const float scale = std::min(content.W() / svgW, content.H() / svgH);
            dest = AlignRect(content, svgW * scale, svgH * scale, alignH, alignV);
          }
          break;
        case Fit::Cover:
          if (svgW > 0.f && svgH > 0.f)
          {
            const float scale = std::max(content.W() / svgW, content.H() / svgH);
            dest = AlignRect(content, svgW * scale, svgH * scale, alignH, alignV);
          }
          break;
        case Fit::Fill:
          dest = content;
          break;
        case Fit::None:
          if (svgW > 0.f && svgH > 0.f)
            dest = AlignRect(content, svgW, svgH, alignH, alignV);
          break;
      }

      canvas->save();
      if (fit == Fit::Cover)
        canvas->clipRect(SkRect::MakeLTRB(content.L, content.T, content.R, content.B));

      glint_canvas svgGraphics(canvas, mpG ? mpG->GetWindow() : nullptr);
      const glint_color* fillPtr = style.fill.isSet ? &style.fill.value : nullptr;
      svgGraphics.DrawSVG(_svgImg, dest, nullptr, nullptr, fillPtr);

      canvas->restore();
      return;
    }

    auto* apiBitmap = bitmap.GetAPIBitmap();
    if (!apiBitmap) return;

    sk_sp<SkImage> img = apiBitmap->img();
    if (!img) return;

    const glint_rect content = getContent();
    const float bmpW    = static_cast<float>(bitmap.FW());
    const float bmpH    = static_cast<float>(bitmap.FH());
    if (bmpW <= 0.f || bmpH <= 0.f) return;

    const float physW   = static_cast<float>(img->width());
    const float physH   = static_cast<float>(img->height());

    Fit     fit    = parseFit(style.objectFit);
    EAlign  alignH = EAlign::Center;
    EVAlign alignV = EVAlign::Middle;
    parsePosition(style.objectPosition, alignH, alignV);

    SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kLinear);

    auto drawImg = [&](SkRect srcPx, SkRect dstLogical) {
      canvas->drawImageRect(img, srcPx, dstLogical, sampling, nullptr,
                            SkCanvas::kFast_SrcRectConstraint);
    };

    switch (fit)
    {
      case Fit::Contain:
      {
        const float scale = std::min(content.W() / bmpW, content.H() / bmpH);
        const glint_rect dest  = AlignRect(content, bmpW * scale, bmpH * scale, alignH, alignV);
        drawImg(SkRect::MakeWH(physW, physH),
                SkRect::MakeLTRB(dest.L, dest.T, dest.R, dest.B));
        break;
      }
      case Fit::Cover:
      {
        const float scale = std::max(content.W() / bmpW, content.H() / bmpH);
        const float fw    = bmpW * scale;
        const float fh    = bmpH * scale;
        const float cropX = (fw - content.W()) * 0.5f / scale;
        const float cropY = (fh - content.H()) * 0.5f / scale;
        const float cropW = content.W() / scale;
        const float cropH = content.H() / scale;
        const float pw    = physW / bmpW;  // physical-to-logical ratio
        const float ph    = physH / bmpH;
        drawImg(SkRect::MakeLTRB(cropX * pw, cropY * ph,
                                  (cropX + cropW) * pw, (cropY + cropH) * ph),
                SkRect::MakeLTRB(content.L, content.T, content.R, content.B));
        break;
      }
      case Fit::Fill:
        drawImg(SkRect::MakeWH(physW, physH),
                SkRect::MakeLTRB(content.L, content.T, content.R, content.B));
        break;
      case Fit::None:
      {
        const glint_rect dest = AlignRect(content, bmpW, bmpH, alignH, alignV);
        drawImg(SkRect::MakeWH(physW, physH),
                SkRect::MakeLTRB(dest.L, dest.T, dest.R, dest.B));
        break;
      }
    }
  }

private:
  friend struct glint_adder;   // builder needs access to _loaded
  glint_svg _svgImg  { nullptr };  // set when src ends in ".svg"
  bool _loaded  = false;      // false → _loadSrc() will be called on next draw

  // Returns a rect of size (fw × fh) placed inside `parent` according to style alignment.
  glint_rect AlignRect(const glint_rect& parent, float fw, float fh,
                   EAlign alignH, EVAlign alignV) const
  {
    float ox = 0.f;
    switch (alignH)
    {
      case EAlign::Near:   ox = 0.f;                           break;
      case EAlign::Center: ox = (parent.W() - fw) * 0.5f;     break;
      case EAlign::Far:    ox = parent.W() - fw;               break;
    }

    float oy = 0.f;
    switch (alignV)
    {
      case EVAlign::Top:    oy = 0.f;                          break;
      case EVAlign::Middle: oy = (parent.H() - fh) * 0.5f;    break;
      case EVAlign::Bottom: oy = parent.H() - fh;              break;
    }

    return glint_rect(parent.L + ox, parent.T + oy,
                 parent.L + ox + fw, parent.T + oy + fh);
  }
};

// Backward-compat alias — code that uses glint_image_style continues to compile.
using glint_image_style = glint_image;

namespace { struct _glint_image_reg { _glint_image_reg() { glint_element::registerElement("img", []{ return new glint_image(); }); } } _glint_image_reg_; }
