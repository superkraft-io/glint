#pragma once
// Render methods for glint_element (drawContent, DrawBackground, Skia path).
// Included inside the class body of glint_element.hpp -- do not compile directly.

  /**
   * Override drawContent to paint on top of the panel background.
  * Delegates to _renderText() (Skia path) so the graphics and standalone-canvas
   * renderers produce identical output. Subclasses that override this without
   * calling the base suppress text rendering.
   */
  virtual void drawContent(glint_canvas& g)
  {
    if (auto* canvas = static_cast<SkCanvas*>(g.GetDrawContext()))
      _renderText(canvas);
  }

  /**
   * Draws the panel background using the given style.
   * Exposed as protected so subclasses with multi-state styles can call it
   * directly with a different glint_style (e.g. hover / pressed states).
   */
  void DrawBackground(glint_canvas& g, const glint_style& s) const
  {
    const float rW = mRect.W(), rH = mRect.H();
    const float gr = s.borderRadius.resolve(std::min(rW, rH));

    {
      SkCanvas* canvas = static_cast<SkCanvas*>(g.GetDrawContext());
      if (canvas)
      {
        _drawShadowSkia(canvas, s, mRect);
        _drawBackgroundSkia(canvas, s, mRect);
        return;
      }
    }

    if (s.shadowEnabled)
    {
      glint_rect sr(
        mRect.L + s.shadowOffsetX,
        mRect.T + s.shadowOffsetY,
        mRect.R + s.shadowOffsetX,
        mRect.B + s.shadowOffsetY
      );
      if (gr > 0.f)
        g.FillRoundRect(s.shadowColor, sr, gr);
      else
        g.FillRect(s.shadowColor, sr);
    }

    const glint_color bg     = ApplyOpacity(s.backgroundColor, s.opacity);
    const glint_color border = ApplyOpacity(s.borderColor,     s.opacity);

    if (gr > 0.f)
    {
      g.FillRoundRect(bg, mRect, gr);
      if (s.borderWidth > 0.f && s.borderStyle != "none")
      {
        if (s.borderStyle == "dashed" || s.borderStyle == "dotted")
          g.DrawDottedRect(border, mRect, nullptr, s.borderWidth, s.borderWidth * 3.f);
        else
          g.DrawRoundRect(border, mRect, gr, nullptr, s.borderWidth);
      }
    }
    else
    {
      g.FillRect(bg, mRect);
      if (s.borderWidth > 0.f && s.borderStyle != "none")
      {
        if (s.borderStyle == "dashed" || s.borderStyle == "dotted")
          g.DrawDottedRect(border, mRect, nullptr, s.borderWidth, s.borderWidth * 3.f);
        else
          g.DrawRect(border, mRect, nullptr, s.borderWidth);
      }
    }
  }

  // ── Helpers ─────────────────────────────────────────────────────────────────

  /** Multiplies the alpha channel of a colour by [0,1] opacity. */
  static glint_color ApplyOpacity(const glint_color& c, float opacity)
  {
    const float o = std::max(0.f, std::min(1.f, opacity));
    return glint_color(static_cast<int>(c.A * o), c.R, c.G, c.B);
  }

  static float _shadowSigma(float blur)
  {
    return blur > 0.f ? std::max(0.001f, blur * 0.5f) : 0.f;
  }

  void _drawShadowSkia(SkCanvas* canvas, const glint_style& s, const glint_rect& rect) const
  {
    if (!canvas || !s.shadowEnabled) return;

    if (s.shadowInset)
    {
      _drawInsetShadowSkia(canvas, s, rect);
      return;
    }

    const glint_rect shadowRect(rect.L + s.shadowOffsetX - s.shadowSpread,
                           rect.T + s.shadowOffsetY - s.shadowSpread,
                           rect.R + s.shadowOffsetX + s.shadowSpread,
                           rect.B + s.shadowOffsetY + s.shadowSpread);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(skColor(ApplyOpacity(s.shadowColor, s.opacity)));
    if (const float sigma = _shadowSigma(s.shadowBlur); sigma > 0.f)
      paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));
    canvas->drawRRect(_makeSkRRect(shadowRect, s), paint);
  }

  void _drawInsetShadowSkia(SkCanvas* canvas, const glint_style& s, const glint_rect& rect) const
  {
    const float baseInset = std::max(0.5f, s.shadowSpread);
    const glint_rect holeRect(rect.L + baseInset - s.shadowOffsetX,
                         rect.T + baseInset - s.shadowOffsetY,
                         rect.R - baseInset - s.shadowOffsetX,
                         rect.B - baseInset - s.shadowOffsetY);
    if (holeRect.W() <= 0.f || holeRect.H() <= 0.f) return;

    SkPath shadowRing;
    shadowRing.setFillType(SkPathFillType::kEvenOdd);
    shadowRing.addRRect(_makeSkRRect(rect, s));
    shadowRing.addRRect(_makeSkRRect(holeRect, s));

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(skColor(ApplyOpacity(s.shadowColor, s.opacity)));
    if (const float sigma = _shadowSigma(s.shadowBlur); sigma > 0.f)
      paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));

    canvas->save();
    canvas->clipRRect(_makeSkRRect(rect, s), SkClipOp::kIntersect, true);
    canvas->drawPath(shadowRing, paint);
    canvas->restore();
  }

public:
  static int fontWeightToInt(float weight)
  {
    if (weight <= 0.f) return 400;
    if (weight < 1.f) return 1;
    if (weight > 1000.f) return 1000;
    return static_cast<int>(weight + 0.5f);
  }

  /**
   * Create a platform-backed SkFont at the given size.
   * Resolves (family, weight, style) against the three-axis font registry,
    * implementing the CSS font-matching algorithm (§9 of CSS Fonts Level 4).
    * When the family is not registered via @font-face, falls back to the installed
    * system font with the same family name before using the default UI typeface.
   */
  static SkFont skFont(float size,
                        const char* family = nullptr,
                        int         weight = 400,
                        const char* style  = nullptr)
  {
    static const sk_sp<SkTypeface>* sFallback = []() -> const sk_sp<SkTypeface>* {
      auto* p = new sk_sp<SkTypeface>();
      auto mgr = glint_font_registry::systemFontMgr();
      if (mgr) *p = mgr->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
      return p;
    }();

    // Per-(family,weight,style) typeface cache. The font-matching algorithm in
    // glint_font_registry::getTypefaceByAxes() walks every registered face and
    // sorts them; doing that for every text element every frame is the second
    // hottest item on text-heavy pages. Caching the resolved sk_sp<SkTypeface>
    // collapses it to a single hash lookup.
    struct _TfKey {
      std::string family;
      int         weight;
      std::string style;
      bool operator==(const _TfKey& o) const noexcept {
        return weight == o.weight && family == o.family && style == o.style;
      }
    };
    struct _TfKeyHash {
      std::size_t operator()(const _TfKey& k) const noexcept {
        return std::hash<std::string>{}(k.family)
             ^ (std::hash<int>{}(k.weight) << 1)
             ^ (std::hash<std::string>{}(k.style) << 2);
      }
    };
    static std::unordered_map<_TfKey, sk_sp<SkTypeface>, _TfKeyHash> sTfCache;

    sk_sp<SkTypeface> tf;
    const bool hasFamily = family && family[0];
    if (hasFamily)
    {
      _TfKey key{ std::string(family), weight, std::string(style ? style : "") };
      auto it = sTfCache.find(key);
      if (it != sTfCache.end()) {
        tf = it->second;
      } else {
        bool shouldCache = false;

        tf = glint_font_registry::getTypefaceByAxes(family, weight, style);
        // Axis lookup failed (unregistered family) — try legacy weight-only lookup.
        if (tf)
        {
          shouldCache = true;
        }
        else
        {
          tf = glint_font_registry::getTypefaceWeighted(family, weight);
          if (tf)
            shouldCache = true;
        }

        // Still unresolved: ask the platform font manager for an installed system face.
        if (!tf)
        {
          tf = glint_font_registry::getSystemTypefaceByAxes(family, weight, style);
          if (tf)
          {
            SkString matchedFamily;
            tf->getFamilyName(&matchedFamily);
            shouldCache = matchedFamily.equals(family);
          }
        }

        // Only cache concrete matches. Startup can request text before deferred
        // @font-face registration completes; caching a substituted system face
        // like Helvetica would pin the fallback even after the real family loads.
        if (shouldCache)
          sTfCache.emplace(std::move(key), tf);
      }
    }
    if (!tf) tf = *sFallback;

    SkFont f(tf, size);
    f.setSubpixel(true);
    f.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    return f;
  }

  static SkFont skFont(float size,
                        const char* family,
                        float       weight,
                        const char* style)
  {
    return skFont(size, family, fontWeightToInt(weight), style);
  }

  /** Pre-populate the shaders map from the registry for any shader(id,name)
   *  token in filterStr.  Called immediately when style.filter /
   *  style.backdropFilter is assigned (before any draw).  Only creates
   *  entries that are not already present — never overwrites user-set
   *  instances.  No compile() call here; that happens at draw time when
   *  pixel dimensions are available. */
  void _prePopulateShaders(const std::string& filterStr)
  {
    if (filterStr.empty() || filterStr == "none") return;
    size_t i = 0;
    const size_t n = filterStr.size();
    auto skipWS = [&]{ while (i < n && std::isspace((unsigned char)filterStr[i])) ++i; };
    while (i < n)
    {
      skipWS();
      if (i >= n) break;
      size_t nameStart = i;
      while (i < n && filterStr[i] != '(' && !std::isspace((unsigned char)filterStr[i])) ++i;
      std::string fname = filterStr.substr(nameStart, i - nameStart);
      skipWS();
      if (i >= n || filterStr[i] != '(') { if (i < n) ++i; continue; }
      size_t tokStart = nameStart;
      int depth = 0;
      while (i < n)
      {
        if      (filterStr[i] == '(') ++depth;
        else if (filterStr[i] == ')') { --depth; if (depth == 0) { ++i; break; } }
        ++i;
      }
      if (fname == "shader")
      {
        std::string fullToken = filterStr.substr(tokStart, i - tokStart);
        size_t pL = fullToken.find('(');
        std::string inner = (pL != std::string::npos)
          ? fullToken.substr(pL + 1, fullToken.size() - pL - 2) : "";
        auto trim_ = [](std::string s) {
          size_t a = s.find_first_not_of(" \t\r\n");
          size_t b = s.find_last_not_of(" \t\r\n");
          return (a == std::string::npos) ? std::string{} : s.substr(a, b - a + 1);
        };
        auto comma  = inner.find(',');
        std::string id   = (comma != std::string::npos) ? trim_(inner.substr(0, comma))    : trim_(inner);
        std::string type = (comma != std::string::npos) ? trim_(inner.substr(comma + 1)) : std::string{};
        if (shaders.find(id) == shaders.end() || !shaders[id])
        {
          auto ptr = glint_shader_registry::create(type);
          if (ptr) shaders[id] = std::move(ptr);
        }
      }
    }
  }

  /** Parse a CSS filter/backdropFilter string, splitting out shader(id,name)
   *  tokens from built-in CSS functions.  Auto-creates missing shader instances
   *  from the registry on first encounter and compiles their SkSL effect.
   *  Returns a struct with the remaining CSS string and an ordered list of
   *  shader IDs for draw-time dispatch. */
  struct _ShaderParseResult {
    std::string css;
    std::vector<std::string> shaderIds;
  };

  _ShaderParseResult _parseFilter(const std::string& filterStr)
  {
    _ShaderParseResult r;
    size_t i = 0;
    const size_t n = filterStr.size();
    auto skipWS = [&]{ while (i < n && std::isspace((unsigned char)filterStr[i])) ++i; };
    while (i < n) {
      skipWS();
      if (i >= n) break;
      size_t nameStart = i;
      while (i < n && filterStr[i] != '(' && !std::isspace((unsigned char)filterStr[i])) ++i;
      std::string fname = filterStr.substr(nameStart, i - nameStart);
      skipWS();
      if (i >= n || filterStr[i] != '(') { if (i < n) ++i; continue; }
      size_t tokStart = nameStart;
      int depth = 0;
      while (i < n) {
        if (filterStr[i] == '(') ++depth;
        else if (filterStr[i] == ')') { --depth; if (depth == 0) { ++i; break; } }
        ++i;
      }
      std::string fullToken = filterStr.substr(tokStart, i - tokStart);
      size_t pL = fullToken.find('(');
      std::string inner = (pL != std::string::npos)
        ? fullToken.substr(pL + 1, fullToken.size() - pL - 2) : "";
      auto trim_ = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        return (a == std::string::npos) ? std::string{} : s.substr(a, b - a + 1);
      };
      if (fname == "shader") {
        auto comma = inner.find(',');
        std::string id   = (comma != std::string::npos) ? trim_(inner.substr(0, comma))    : trim_(inner);
        std::string type = (comma != std::string::npos) ? trim_(inner.substr(comma + 1)) : std::string{};
        if (shaders.find(id) == shaders.end()) {
          auto ptr = glint_shader_registry::create(type);
          if (ptr) { ptr->compile(); shaders[id] = std::move(ptr); }
        } else {
          shaders[id]->compile();
        }
        r.shaderIds.push_back(id);
      } else {
        if (!r.css.empty()) r.css += ' ';
        r.css += fullToken;
      }
    }
    return r;
  }

  // ── Standalone SkCanvas draw path ────────────────────────────────────────────────
  // Used by the inspector window (no glint_canvas available).
  // DrawToCanvas / DrawBackgroundToCanvas / DrawContentToCanvas mirror the
  // glint_canvas-based Draw() / DrawBackground() / drawContent() path but call
  // raw Skia so the inspector can render into its own CPU SkSurface.

  /** Convert an glint_rect to a SkRect. */
  static SkRect skRect(const glint_rect& r)
  {
    return SkRect::MakeLTRB(r.L, r.T, r.R, r.B);
  }

  /** Convert an glint_color to a SkColor (0xAARRGGBB). */
  static SkColor skColor(const glint_color& c)
  {
    return SkColorSetARGB(c.A, c.R, c.G, c.B);
  }

  /** Build an SkRRect from an glint_rect honouring per-corner radii from a glint_style. */
  SkRRect _makeSkRRect(const glint_rect& r, const glint_style& s) const
  {
    auto radii = s.resolveCornerRadii(r.W(), r.H());
    SkVector corners[4] = {
      { radii[0], radii[0] },  // top-left
      { radii[1], radii[1] },  // top-right
      { radii[2], radii[2] },  // bottom-right
      { radii[3], radii[3] },  // bottom-left
    };
    SkRRect rr;
    rr.setRectRadii(SkRect::MakeLTRB(r.L, r.T, r.R, r.B), corners);
    return rr;
  }

  /** Resolve a CSS mask box keyword against this element. */
  glint_rect _maskBoxRect(const std::string& box) const
  {
    if (box == "padding-box") return GetPaddingBox();
    if (box == "content-box") return getContent();
    return GetPaintRECT();  // border-box / fallback
  }

  /** Build the rounded clip geometry for CSS mask boxes.
   *  For inner boxes, shrink the border-box radii by the inset on each axis,
   *  matching the same inner-curve construction browsers use for box clipping. */
  SkRRect _makeMaskBoxRRect(const glint_rect& r, const std::string& box) const
  {
    const auto outer = computedStyle.resolveCornerRadii(GetPaintRECT().W(), GetPaintRECT().H());

    float insetT = 0.f, insetR = 0.f, insetB = 0.f, insetL = 0.f;
    if (box == "padding-box" || box == "content-box")
    {
      insetT += computedStyle.resolvedBorderWidth(0);
      insetR += computedStyle.resolvedBorderWidth(1);
      insetB += computedStyle.resolvedBorderWidth(2);
      insetL += computedStyle.resolvedBorderWidth(3);
    }
    if (box == "content-box")
    {
      insetT += static_cast<float>(computedStyle.paddingTop);
      insetR += static_cast<float>(computedStyle.paddingRight);
      insetB += static_cast<float>(computedStyle.paddingBottom);
      insetL += static_cast<float>(computedStyle.paddingLeft);
    }

    SkVector corners[4] = {
      { std::max(0.f, outer[0] - insetL), std::max(0.f, outer[0] - insetT) },
      { std::max(0.f, outer[1] - insetR), std::max(0.f, outer[1] - insetT) },
      { std::max(0.f, outer[2] - insetR), std::max(0.f, outer[2] - insetB) },
      { std::max(0.f, outer[3] - insetL), std::max(0.f, outer[3] - insetB) },
    };

    SkRRect rr;
    rr.setRectRadii(skRect(r), corners);
    return rr;
  }

  /** Clip the current canvas to one CSS mask-clip box.
   *  Chrome parity here treats the mask geometry box as box-aligned; the
   *  element's border-radius has already shaped the source pixels before the
   *  filter stage and should not become an extra post-filter clip. */
  void _clipToMaskBox(SkCanvas* canvas, const std::string& box) const
  {
    if (!canvas || box == "no-clip") return;

    const glint_rect clipRect = _maskBoxRect(box);
    if (clipRect.W() <= 0.f || clipRect.H() <= 0.f)
    {
      canvas->clipRect(SkRect::MakeEmpty(), SkClipOp::kIntersect, false);
      return;
    }

    canvas->clipRect(skRect(clipRect), SkClipOp::kIntersect, false);
  }

  /**
   * Core Skia background renderer supporting per-corner radius and per-side border.
   * Shadow is NOT rendered here; callers handle shadow separately.
   */
  void _drawBackgroundSkia(SkCanvas* canvas, const glint_style& s, const glint_rect& rect) const
  {
    const float rW = rect.W(), rH = rect.H();
    const SkBlendMode _bgBlend = glint_css_blend_mode(s.backgroundBlendMode);

    // CSS Compositing §14.2: background layers must blend with each other within an
    // isolated compositing group — NOT with the canvas content behind the element.
    // Opening a saveLayer ensures the group result is composited normally (SrcOver)
    // onto the parent canvas after all background layers are drawn.
    const bool _bgNeedsIsolation = (_bgBlend != SkBlendMode::kSrcOver);
    if (_bgNeedsIsolation)
      canvas->saveLayer(skRect(rect), nullptr);

    // background-color — always the bottommost background layer (always SrcOver).
    // This must be drawn regardless of whether a gradient or img is also present,
    // so that transparent gradient/img stops correctly reveal the color.
    {
      const glint_color _bgColorLayer = ApplyOpacity(s.backgroundColor, s.opacity);
      if (_bgColorLayer.A > 0)
      {
        SkPaint _bcp; _bcp.setAntiAlias(true); _bcp.setColor(skColor(_bgColorLayer));
        canvas->drawRRect(_makeSkRRect(rect, s), _bcp);
      }
    }

    if (!s.backgroundGradient.empty() && s.backgroundGradient.size() >= 2)
    {
      // Build colour / position arrays.
      std::vector<sk_gradient_stop> sorted = s.backgroundGradient;
      std::stable_sort(sorted.begin(), sorted.end());

      std::vector<SkColor>  colors;
      std::vector<SkScalar> positions;
      colors.reserve(sorted.size()); positions.reserve(sorted.size());
      for (const auto& st : sorted)
      {
        glint_color c = ApplyOpacity(st.color, s.opacity);
        colors.push_back(SkColorSetARGB(c.A, c.R, c.G, c.B));
        positions.push_back(static_cast<SkScalar>(st.position));
      }

      sk_sp<SkShader> shader;
      const std::string& gtype = s.backgroundGradientType;

      if (gtype == "radial")
      {
        // Radial gradient: center + radius.
        const float cx = rect.L + rW * s.backgroundGradientCX;
        const float cy = rect.T + rH * s.backgroundGradientCY;
        const float r  = std::min(rW, rH) * 0.5f * s.backgroundGradientRadius;
        shader = SkGradientShader::MakeRadial(
            {cx, cy}, std::max(r, 1.f),
            colors.data(), positions.data(),
            static_cast<int>(colors.size()), SkTileMode::kClamp);
      }
      else if (gtype == "conic")
      {
        // Conic (sweep) gradient: center + start angle.
        const float cx   = rect.L + rW * s.backgroundGradientCX;
        const float cy   = rect.T + rH * s.backgroundGradientCY;
        const float startDeg = s.backgroundGradientAngle;
        SkMatrix localMatrix;
        localMatrix.setRotate(startDeg - 90.f, cx, cy);
        shader = SkGradientShader::MakeSweep(
            cx, cy,
            colors.data(), positions.data(),
            static_cast<int>(colors.size()),
            SkTileMode::kClamp,
            0.f, 360.f,
            0, &localMatrix);
      }
      else
      {
        // Linear gradient: derive start/end points from angle or direction keyword.
        // CSS angle convention: 0deg = to top, 90deg = to right, 180deg = to bottom.
        const float cx = rect.L + rW * 0.5f;
        const float cy = rect.T + rH * 0.5f;
        float dx, dy;

        // CSS spec for diagonal side-or-corner keywords (e.g. "to bottom right"):
        // The gradient direction is NOT simply atan2 of the corner — it is
        // perpendicular to the line connecting the two ADJACENT corners.
        //   "to bottom right": adjacent corners are top-right & bottom-left
        //     → their connecting line direction is (-W, H)
        //     → gradient direction (perpendicular, rotated 90° CW) = (H, W)
        // Endpoint distance from center: t = W*H / (H²+W²)
        // This differs from a fixed 135° angle whenever W ≠ H.
        const std::string& dir = s.backgroundGradientDirection;
        if (dir == "to bottom right" || dir == "to right bottom")
        {
          const float t = (rW * rH) / (rH * rH + rW * rW);
          dx =  rH * t;  dy =  rW * t;
        }
        else if (dir == "to bottom left" || dir == "to left bottom")
        {
          const float t = (rW * rH) / (rH * rH + rW * rW);
          dx = -rH * t;  dy =  rW * t;
        }
        else if (dir == "to top right" || dir == "to right top")
        {
          const float t = (rW * rH) / (rH * rH + rW * rW);
          dx =  rH * t;  dy = -rW * t;
        }
        else if (dir == "to top left" || dir == "to left top")
        {
          const float t = (rW * rH) / (rH * rH + rW * rW);
          dx = -rH * t;  dy = -rW * t;
        }
        else
        {
          // Numeric angle — CSS Images §3.1.3 spec-correct gradient line length:
          //   half = (|W·sin θ| + |H·cos θ|) / 2
          // The endpoint is offset from centre by this single scalar in direction (sin θ, −cos θ).
          // The previous elliptic formula (dx=sin·W/2, dy=−cos·H/2) was wrong for non-square
          // elements at non-axis-aligned angles, causing stop positions to diverge from Chrome.
          const float rad  = s.backgroundGradientAngle * 3.14159265358979323846f / 180.f;
          const float sinA = std::sin(rad);
          const float cosA = std::cos(rad);
          const float half = (std::abs(sinA) * rW + std::abs(cosA) * rH) * 0.5f;
          dx =  sinA * half;
          dy = -cosA * half;
        }

        SkPoint pts[2] = { {cx - dx, cy - dy}, {cx + dx, cy + dy} };
        shader = SkGradientShader::MakeLinear(
            pts, colors.data(), positions.data(),
            static_cast<int>(colors.size()), SkTileMode::kClamp);
      }

      // background-color is already drawn above as the base layer.
      // Composite the gradient on top with the blend mode (inside the isolation group when active).
      SkPaint fp; fp.setAntiAlias(true); fp.setShader(shader);
      fp.setBlendMode(_bgBlend);
      canvas->drawRRect(_makeSkRRect(rect, s), fp);
    }
    // (else: no gradient — background-color was already drawn above as the only layer.)

    // ── Background img (background-img: url("path") or `background` shorthand) ──────
    if (!s.backgroundImage.empty())
    {
      sk_sp<SkImage> bgImg;
      const std::string& bgSrc = s.backgroundImage;
      const bool isSVG = bgSrc.size() > 4 &&
                         (bgSrc.substr(bgSrc.size() - 4) == ".svg" ||
                          bgSrc.substr(bgSrc.size() - 4) == ".SVG");

      if (isSVG)
      {
        auto dom = glint_load_svg_dom(bgSrc, _getOnRequest(), this, _getNetworkLog());
        if (dom)
        {
          // Draw SVG directly onto the main canvas using the same destination-fit path.
          // This avoids the offscreen raster surface + SkImage + shader pipeline which
          // can silently produce blank output when a CPU-raster snapshot is used as a
          // shader on a GPU-backed canvas that doesn't support that code path.
          //
          // We replicate background-size / background-position / background-repeat
          // via canvas save/translate/scale before calling dom->render().
          const SkSize cs = dom->containerSize();
          const float svgW = cs.width()  > 0.f ? cs.width()  : rW;
          const float svgH = cs.height() > 0.f ? cs.height() : rH;

          // ── background-size → compute scaled SVG dimensions ────────────
          const std::string& bsz = s.backgroundSize;
          float tileW, tileH;
          if (bsz == "cover") {
            const float scl = std::max(rW / svgW, rH / svgH);
            tileW = svgW * scl;  tileH = svgH * scl;
          } else if (bsz == "contain") {
            const float scl = std::min(rW / svgW, rH / svgH);
            tileW = svgW * scl;  tileH = svgH * scl;
          } else if (bsz == "auto" || bsz.empty()) {
            tileW = svgW;  tileH = svgH;
          } else {
            // "W H" | "W%" | "100% 50%" etc.
            std::istringstream ss(bsz);
            std::string sw, sh;
            tileW = rW;  tileH = rH;
            if (ss >> sw) {
              try { tileW = sw.back()=='%' ? std::stof(sw)*rW/100.f : std::stof(sw); } catch(...) {}
              if (ss >> sh) {
                try { tileH = sh.back()=='%' ? std::stof(sh)*rH/100.f : std::stof(sh); } catch(...) {}
              }
            }
          }

          // ── background-position → offset within element ────────────────
          float fx = 0.5f, fy = 0.5f;
          glint_mask_resolve_position(s.backgroundPosition, fx, fy);
          const float offX = fx * (rW - tileW);
          const float offY = fy * (rH - tileH);

          // ── Draw (respects border-radius clip) ────────────────────────
          canvas->save();
          canvas->clipRRect(_makeSkRRect(rect, s), SkClipOp::kIntersect, true);
          // Move to element top-left, apply position offset, then scale SVG coords to tile size.
          canvas->translate(rect.L + offX, rect.T + offY);
          canvas->scale(tileW / svgW, tileH / svgH);
          // background-blend-mode: wrap SVG render in a blend saveLayer when non-normal.
          if (_bgBlend != SkBlendMode::kSrcOver)
          {
            SkPaint _svgBP; _svgBP.setBlendMode(_bgBlend);
            canvas->saveLayer(nullptr, &_svgBP);
            dom->render(canvas);
            canvas->restore();
          }
          else
          {
            dom->render(canvas);
          }
          canvas->restore();
        }
      }
      else
      {
        (void)isSVG;
        bgImg = glint_load_image(bgSrc, _getOnRequest(), this, _getNetworkLog());
      }

      if (bgImg)
      {
        // Reuse the previously built SkShader when nothing relevant changed.
        // Stable shader instances let Skia's GPU program cache hit on
        // subsequent frames; this is the same fix that resolved the masks
        // page first-switch stall and is critical for img-heavy pages
        // (the demo's Images and BlendModes pages each have 16+ cards
        // sharing one source img).
        const bool _bgShCacheHit =
              mBgImgCacheValid
          && mBgImgCacheImg.get()  == bgImg.get()
          && mBgImgCachePath       == bgSrc
          && mBgImgCacheSize       == s.backgroundSize
          && mBgImgCachePosition   == s.backgroundPosition
          && mBgImgCacheRepeat     == s.backgroundRepeat
          && mBgImgCacheRectL      == rect.L
          && mBgImgCacheRectT      == rect.T
          && mBgImgCacheRectR      == rect.R
          && mBgImgCacheRectB      == rect.B;

        sk_sp<SkShader> shader;
        if (_bgShCacheHit)
        {
          shader = mBgImgCacheShader;
        }
        else
        {
          // Synthesise a glint_mask_layer so we can reuse glint_mask_image_shader.
          glint_mask_layer bgLayer;
          bgLayer.type     = glint_mask_layer::URL_IMAGE;
          bgLayer.size     = s.backgroundSize;
          bgLayer.position = s.backgroundPosition;
          bgLayer.repeat   = s.backgroundRepeat;

          shader = glint_mask_image_shader(bgImg, rect, bgLayer);

          mBgImgCacheImg       = bgImg;
          mBgImgCacheShader    = shader;
          mBgImgCachePath      = bgSrc;
          mBgImgCacheSize      = s.backgroundSize;
          mBgImgCachePosition  = s.backgroundPosition;
          mBgImgCacheRepeat    = s.backgroundRepeat;
          mBgImgCacheRectL     = rect.L;
          mBgImgCacheRectT     = rect.T;
          mBgImgCacheRectR     = rect.R;
          mBgImgCacheRectB     = rect.B;
          mBgImgCacheValid     = true;
        }

        if (shader)
        {
          SkPaint imgPaint;
          imgPaint.setAntiAlias(true);
          imgPaint.setShader(shader);
          imgPaint.setAlphaf(s.opacity);
          imgPaint.setBlendMode(_bgBlend);  // background-blend-mode (CSS Compositing §14.2)
          // Clip to the element's rounded shape (border-radius).
          canvas->save();
          canvas->clipRRect(_makeSkRRect(rect, s), SkClipOp::kIntersect, true);
          canvas->drawRect(SkRect::MakeLTRB(rect.L, rect.T, rect.R, rect.B), imgPaint);
          canvas->restore();
        }
      }
    }

    // Close the isolation group opened for background-blend-mode (if any).
    if (_bgNeedsIsolation)
      canvas->restore();
  }

  /**
   * Draw the border of an element on top of its content.
   * Extracted from _drawBackgroundSkia so it can be called AFTER children paint,
   * matching the CSS stacking model (border is above background and content).
   */
  void _drawBorderSkia(SkCanvas* canvas, const glint_style& s, const glint_rect& rect) const
  {
    if (!canvas) return;
    const float rW = rect.W(), rH = rect.H();
    auto radii = s.resolveCornerRadii(rW, rH);
    const bool anyRadius = radii[0]>0.f || radii[1]>0.f || radii[2]>0.f || radii[3]>0.f;

    // Helper: apply all SVG stroke properties to a prepared SkPaint (kStroke_Style must be set).
    // fallbackWidth/Color/Style are the resolved border-* values for that side / unified border.
    auto applyStrokeProps = [&](SkPaint& bp, float fallbackWidth,
                                const glint_color& fallbackColor, const std::string& fallbackStyle)
    {
      const float sw = s.strokeWidth > 0.f ? s.strokeWidth : fallbackWidth;
      bp.setStrokeWidth(sw);
      glint_color col = s.strokeColor.isSet ? ApplyOpacity(s.strokeColor.value, s.opacity) : fallbackColor;
      if (s.strokeOpacity < 1.f)
        col = glint_color(static_cast<int>(col.A * s.strokeOpacity), col.R, col.G, col.B);
      bp.setColor(skColor(col));
      // linecap
      if      (s.strokeLinecap == "round")  bp.setStrokeCap(SkPaint::kRound_Cap);
      else if (s.strokeLinecap == "square") bp.setStrokeCap(SkPaint::kSquare_Cap);
      else                                   bp.setStrokeCap(SkPaint::kButt_Cap);
      // linejoin
      if      (s.strokeLinejoin == "round") bp.setStrokeJoin(SkPaint::kRound_Join);
      else if (s.strokeLinejoin == "bevel") bp.setStrokeJoin(SkPaint::kBevel_Join);
      else                                   bp.setStrokeJoin(SkPaint::kMiter_Join);
      bp.setStrokeMiter(s.strokeMiterlimit);
      // dasharray / dashoffset
      if (!s.strokeDasharray.empty() && s.strokeDasharray != "none")
      {
        // Cache the SkPathEffect across frames keyed on (dasharray, offset).
        // SkDashPathEffect::Make is a non-trivial alloc and was a per-frame
        // cost on the Stroke demo (every dashed border rebuilt every paint).
        if (!mDashCacheValid
            || mDashCacheStr    != s.strokeDasharray
            || mDashCacheOffset != s.strokeDashoffset)
        {
          std::vector<SkScalar> ivs;
          std::istringstream iss(s.strokeDasharray);
          std::string tok;
          while (iss >> tok) { try { ivs.push_back(std::stof(tok)); } catch (...) {} }
          // SVG spec: odd count → duplicate list
          if (!ivs.empty() && ivs.size() % 2 != 0) { auto cp = ivs; ivs.insert(ivs.end(), cp.begin(), cp.end()); }
          mDashCacheEffect = ivs.empty() ? sk_sp<SkPathEffect>{}
                                         : SkDashPathEffect::Make(ivs.data(), static_cast<int>(ivs.size()), s.strokeDashoffset);
          mDashCacheStr    = s.strokeDasharray;
          mDashCacheOffset = s.strokeDashoffset;
          mDashCacheValid  = true;
        }
        if (mDashCacheEffect) bp.setPathEffect(mDashCacheEffect);
      }
      else if (fallbackStyle == "dashed" || fallbackStyle == "dotted")
      {
        // Build a synthetic dasharray key so the same cache slot can serve
        // border-style: dashed/dotted. Width-dependent: re-key on `sw`.
        const float unit = (fallbackStyle == "dotted") ? sw : sw * 3.f;
        char _buf[64];
        std::snprintf(_buf, sizeof(_buf), "__bs_%s_%g", fallbackStyle.c_str(), unit);
        if (!mDashCacheValid
            || mDashCacheStr    != _buf
            || mDashCacheOffset != s.strokeDashoffset)
        {
          const SkScalar ivs[] = { unit, unit };
          mDashCacheEffect = SkDashPathEffect::Make(ivs, 2, s.strokeDashoffset);
          mDashCacheStr    = _buf;
          mDashCacheOffset = s.strokeDashoffset;
          mDashCacheValid  = true;
        }
        if (mDashCacheEffect) bp.setPathEffect(mDashCacheEffect);
      }
    };

    if (s.hasSidedBorder())
    {
      if (anyRadius)
      {
        // Rounded + per-side: fall back to top-side stroke (CSS limitation).
        const float w         = s.resolvedBorderWidth(0);
        const glint_color c        = ApplyOpacity(s.resolvedBorderColor(0), s.opacity);
        const std::string& st = s.resolvedBorderStyle(0);
        if (w > 0.f && st != "none" && c.A > 0)
        {
          SkPaint bp; bp.setAntiAlias(true);
          bp.setStyle(SkPaint::kStroke_Style);
          applyStrokeProps(bp, w, c, st);
          const float eff_h = (s.strokeWidth > 0.f ? s.strokeWidth : w) / 2.f;
          const glint_rect _br(rect.L+eff_h, rect.T+eff_h, rect.R-eff_h, rect.B-eff_h);
          canvas->drawRRect(_makeSkRRect(_br, s), bp);
        }
      }
      else
      {
        // No radius: draw each solid side as the CSS border-region polygon so
        // adjacent side widths produce the expected slanted joins.
        const float L = rect.L, T = rect.T, R = rect.R, B = rect.B;
        const float wT = s.resolvedBorderWidth(0);
        const float wR = s.resolvedBorderWidth(1);
        const float wB = s.resolvedBorderWidth(2);
        const float wL = s.resolvedBorderWidth(3);
        auto drawSide = [&](int idx) {
          const float        w  = idx==0?wT : idx==1?wR : idx==2?wB : wL;
          const glint_color       c  = ApplyOpacity(s.resolvedBorderColor(idx), s.opacity);
          const std::string& st = s.resolvedBorderStyle(idx);
          if (w <= 0.f || st == "none" || c.A == 0) return;
          if (st == "dashed" || st == "dotted")
          {
            // Stroke the centreline of the border rect with a dash effect.
            float x1, y1, x2, y2;
            float lx1, ly1, lx2, ly2;
            switch (idx)
            {
              case 0: x1 = L;      y1 = T;      x2 = R;      y2 = T + wT; break;
              case 1: x1 = R - wR; y1 = T + wT; x2 = R;      y2 = B - wB; break;
              case 2: x1 = L;      y1 = B - wB; x2 = R;      y2 = B;      break;
              default:
              case 3: x1 = L;      y1 = T + wT; x2 = L + wL; y2 = B - wB; break;
            }
            const float cx = (x1 + x2) * 0.5f, cy = (y1 + y2) * 0.5f;
            const bool  horiz = (std::abs(x2 - x1) > std::abs(y2 - y1));
            if (horiz) { lx1 = x1; ly1 = cy; lx2 = x2; ly2 = cy; }
            else       { lx1 = cx; ly1 = y1; lx2 = cx; ly2 = y2; }
            SkPaint bp; bp.setAntiAlias(true);
            bp.setStyle(SkPaint::kStroke_Style);
            applyStrokeProps(bp, w, c, st);
            canvas->drawLine(lx1, ly1, lx2, ly2, bp);
          }
          else
          {
            float xs[4];
            float ys[4];
            switch (idx)
            {
              case 0:
                xs[0] = L;      ys[0] = T;
                xs[1] = R;      ys[1] = T;
                xs[2] = R - wR; ys[2] = T + wT;
                xs[3] = L + wL; ys[3] = T + wT;
                break;
              case 1:
                xs[0] = R;      ys[0] = T;
                xs[1] = R;      ys[1] = B;
                xs[2] = R - wR; ys[2] = B - wB;
                xs[3] = R - wR; ys[3] = T + wT;
                break;
              case 2:
                xs[0] = L + wL; ys[0] = B - wB;
                xs[1] = R - wR; ys[1] = B - wB;
                xs[2] = R;      ys[2] = B;
                xs[3] = L;      ys[3] = B;
                break;
              default:
                xs[0] = L;      ys[0] = T;
                xs[1] = L + wL; ys[1] = T + wT;
                xs[2] = L + wL; ys[2] = B - wB;
                xs[3] = L;      ys[3] = B;
                break;
            }
            SkPaint bp; bp.setAntiAlias(true);
            bp.setColor(skColor(c));
            SkPath sidePath;
            sidePath.moveTo(xs[0], ys[0]);
            sidePath.lineTo(xs[1], ys[1]);
            sidePath.lineTo(xs[2], ys[2]);
            sidePath.lineTo(xs[3], ys[3]);
            sidePath.close();
            canvas->drawPath(sidePath, bp);
          }
        };
        drawSide(0);
        drawSide(2);
        drawSide(3);
        drawSide(1);
      }
    }
    else
    {
      const glint_color brd = ApplyOpacity(s.borderColor, s.opacity);
      if (s.borderWidth > 0.f && s.borderStyle != "none" && brd.A > 0)
      {
        SkPaint bp; bp.setAntiAlias(true);
        bp.setStyle(SkPaint::kStroke_Style);
        applyStrokeProps(bp, s.borderWidth, brd, s.borderStyle);
        const float _bh = s.borderWidth / 2.f;
        const glint_rect _br(rect.L+_bh, rect.T+_bh, rect.R-_bh, rect.B-_bh);
        canvas->drawRRect(_makeSkRRect(_br, s), bp);
      }
    }
  }

  /**
   * Draw styled background fill using raw Skia (no border — see _drawBorderSkia).
   * Respects per-corner radius, gradients, opacity, and the active shadow path.
   */
  void DrawBackgroundToCanvas(SkCanvas* canvas) const
  {
    _drawShadowSkia(canvas, computedStyle, mRect);
    _drawBackgroundSkia(canvas, computedStyle, mRect);
  }

  glint_rect _stackingVisualBounds()
  {
    glint_rect bounds = GetRECT();

    auto unionRect = [](const glint_rect& a, const glint_rect& b) {
      return glint_rect(std::min(a.L, b.L),
                   std::min(a.T, b.T),
                   std::max(a.R, b.R),
                   std::max(a.B, b.B));
    };

    auto visit = [&](auto&& self, glint_element* node) -> void {
      if (!node) return;

      node->EnsureFilterPad();
      bounds = unionRect(bounds, node->GetRECT());

      const bool clipsDescendants = (node->computedStyle.overflowX != "visible" ||
                                     node->computedStyle.overflowY != "visible");
      if (clipsDescendants) return;

      for (const auto& child : node->mChildren)
      {
        auto* c = child.get();
        if (!c || c == node->mScrollbarV || c == node->mScrollbarH || c == node->mScrollCorner)
          continue;
        self(self, c);
      }
    };

    visit(visit, this);
    return bounds;
  }

  /**
   * Canonical Skia text renderer. Called by both drawContent() and DrawContentToCanvas().
   * Handles font validation, selection highlights, nowrap, greedy word-wrap, and
   * correct baseline positioning via SkFontMetrics.
   */
  void _renderText(SkCanvas* canvas)
  {
    if (!canvas || innerText.empty()) return;

    const std::string& displayText = innerText;
    const sk_color     displayCol  = computedStyle.color;

    // Selection highlights rendered under text:
    if (mSelStart >= 0 && mSelStart != mSelEnd)
      _txtDrawSelectionHighlights(canvas);

    const float sz = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 12.f;
    const glint_rect r  = getContent();
    SkFont  font = skFont(sz,
                          computedStyle.fontFamily.c_str(),
                          computedStyle.fontWeight,
                          computedStyle.fontStyle.c_str());
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(skColor(ApplyOpacity(displayCol.value, computedStyle.opacity)));

    // Ink-box metrics: measure the actual glyph bounds of the first (or only) line.
    // bounds.top() is the distance from the baseline to the topmost rendered pixel
    // (negative = above baseline). Using this eliminates the phantom gap that appears
    // between fAscent (typographic max) and the real glyph tops of capital letters.
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const auto renderLines = _buildRenderLines(font);
    if (renderLines.empty()) return;

    SkRect _inkB;
    font.measureText(renderLines[0].text.c_str(), renderLines[0].text.size(), SkTextEncoding::kUTF8, &_inkB);

    // Generic block text in Chrome starts at the first line box near the top of
    // the content box. glint intentionally does not expose a nonstandard
    // block-text vertical-alignment property here.
    for (const auto& line : renderLines) {
      if (!line.text.empty()) {
        const float x = line.x;
        const float y = line.drawBaselineY;
        canvas->drawString(line.text.c_str(), x, y, font, paint);
        {
          const std::string& td = computedStyle.textDecoration;
          const bool doStrike    = td == "line-through" || td == "underline line-through" || td == "line-through underline";
          const bool doUnderline = td == "underline"    || td == "underline line-through" || td == "line-through underline";
          if (doStrike || doUnderline)
          {
            const float lineAdv = line.width;
            SkPaint lp; lp.setAntiAlias(true); lp.setColor(paint.getColor()); lp.setStyle(SkPaint::kStroke_Style);
            if (doStrike) {
              lp.setStrokeWidth(std::max(0.5f, sz / 14.f));
              const float ly = y + (metrics.fStrikeoutPosition != 0.f ? metrics.fStrikeoutPosition : -metrics.fXHeight * 0.5f);
              canvas->drawLine(x, ly, x + lineAdv, ly, lp);
            }
            if (doUnderline) {
              const float thickness = std::max(1.f, std::round(metrics.fUnderlineThickness > 0.f ? metrics.fUnderlineThickness : std::max(1.f, sz / 14.f)));
              lp.setStrokeWidth(thickness);
              const float rawLy = y + (metrics.fUnderlinePosition != 0.f ? metrics.fUnderlinePosition : sz * 0.08f);
              const float ly = std::floor(rawLy) + 0.5f;
              canvas->drawLine(x, ly, x + lineAdv, ly, lp);
            }
          }
        }
      }
    }
  }

  /** Content drawing hook for the SkCanvas path. Override in subclasses.
   *  Base implementation delegates to _renderText(). */
  virtual void DrawContentToCanvas(SkCanvas* canvas)
  {
    _renderText(canvas);
  }

  /**
   * Full render traversal using raw Skia.
   * Draws background, then content, then recurses into children.
   * Override in subclasses that use non-standard draw order (e.g. glint_button).
   */
  void _drawToCanvasImpl(SkCanvas* canvas, bool tickSelf, bool _skipStacking = false,
                         bool _skipSelfFilter = false)
  {
    if (tickSelf) tickTransitions();

    if (computedStyle.display == "none") return;

    EnsureFilterPad();
    const glint_rect _stackBounds = _stackingVisualBounds();
    const glint_rect _expandedRECT = mRect;
    if (mFilterPad > 0.f) mRect = mPaintRECT;

    const glint_rect _cpr = GetPaintRECT();
    const SkM44 _cmat = _skipStacking ? SkM44{} : computedStyle.ResolveTransformMatrix(_cpr.W(), _cpr.H(), _cpr.MW(), _cpr.MH());
    const bool hasTransform = !_skipStacking && !(_cmat == SkM44{});
    const float _selfOpacity = _skipStacking ? 1.0f : static_cast<float>(computedStyle.opacity);
    const bool _needsOpacityLayer = !_skipStacking && (_selfOpacity < 0.9999f);
    const SkBlendMode _selfBlendMode  = _skipStacking ? SkBlendMode::kSrcOver : glint_css_blend_mode(computedStyle.mixBlendMode);
    const bool _needsBlendLayer       = !_skipStacking && (_selfBlendMode != SkBlendMode::kSrcOver);
    const bool _needsIsolationLayer   = !_skipStacking && (computedStyle.isolation == "isolate");
    const bool _hasSelfFilterStyle = !computedStyle.filter.empty() && computedStyle.filter != "none";
    const bool _hasSelfBackdropFilterStyle = !computedStyle.backdropFilter.empty() && computedStyle.backdropFilter != "none";
    const bool _hasSelfMaskStyle = !computedStyle.mask.empty() && computedStyle.mask != "none";
    const bool _hasRenderableChildren = std::any_of(mChildren.begin(), mChildren.end(), [&](const auto& child) {
      const auto* c = child.get();
      return c && c != mScrollbarV && c != mScrollbarH && c != mScrollCorner;
    });
    _ShaderParseResult _bdParsedDTC, _fParsedDTC;
    if (_hasSelfBackdropFilterStyle)
      _bdParsedDTC = _parseFilter(computedStyle.backdropFilter);
    if (_hasSelfFilterStyle)
      _fParsedDTC  = _parseFilter(computedStyle.filter);

    const bool _hasBackdropShadersDTC = !_bdParsedDTC.shaderIds.empty();
    const bool _hasFilterShadersDTC = !_fParsedDTC.shaderIds.empty();
    const bool _hasBackdropFilterDTC = !_bdParsedDTC.css.empty();
    const bool _hasFilterDTC = !_skipSelfFilter && !_fParsedDTC.css.empty();
    const float _filterExpansionDTC = _hasFilterDTC
      ? glint_filter::ComputeExpansion(_fParsedDTC.css)
      : 0.f;

    // ── CSS transform + opacity stacking context ──────────────────────────────────
    // Chrome creates a compositing layer whenever transform != identity OR opacity < 1.
    // The entire subtree (element + all descendants) is drawn into an offscreen surface
    // at natural coordinates, then that surface is composited back with the transform
    // and/or opacity applied as a unit.
    //
    // Naive approach (concat CTM then draw children at absolute mRect coords) breaks
    // because absolute child coordinates do not scale relative to the parent's local
    // origin — they scale from the screen origin, so children appear to fly away.
    //
    // Correct approach:
    //   1. Open a saveLayer bounded by the element's UNTRANSFORMED paint rect.
    //   2. Inside the layer, reset the canvas translation so all drawing happens
    //      in the element's local space (origin = element's top-left corner).
    //   3. Draw self + children using local-space coords (mRect − local origin).
    //      → We re-use _drawToCanvasImpl with a translated canvas so the existing
    //        absolute-rect draw calls become local-space automatically.
    //   4. restore() composites the layer onto the parent with the transform matrix
    //      and opacity paint already recorded in the saveLayer.
    //
    // When NEITHER transform nor opacity nor blend mode nor isolation is active we fall through to the fast path.
    if (hasTransform || _needsOpacityLayer || _needsBlendLayer || _needsIsolationLayer)
    {
      if (!hasTransform
          && _needsOpacityLayer
          && !_needsBlendLayer
          && !_needsIsolationLayer
          && (_hasBackdropFilterDTC || _hasBackdropShadersDTC))
      {
        render_timing_scope _timingScope(render_timing_bucket::backdrop);
        SkPaint _compPaint;
        _compPaint.setAlphaf(_selfOpacity);
        const SkRect _stackRect = skRect(_stackBounds);
        SkCanvas::SaveLayerRec _backdropOpacityRec(&_stackRect, &_compPaint, nullptr,
            SkCanvas::kInitWithPrevious_SaveLayerFlag);
        canvas->saveLayer(_backdropOpacityRec);
        _drawToCanvasImpl(canvas, false, true, _skipSelfFilter);
        canvas->restore();
        if (mFilterPad > 0.f) mRect = _expandedRECT;
        return;
      }

      if (hasTransform
          && !_hasRenderableChildren
          && !_hasSelfBackdropFilterStyle
          && !_hasSelfMaskStyle
          && !_hasBackdropShadersDTC
          && !_hasFilterShadersDTC
          && (!_hasFilterDTC || _filterExpansionDTC <= 0.001f))
      {
        render_timing_scope _timingScope(render_timing_bucket::transform_direct);
        auto _directFilter = _hasFilterDTC ? glint_filter::Build(_fParsedDTC.css) : nullptr;

        canvas->save();
        canvas->concat(_cmat);

        if (_needsBlendLayer && !_needsIsolationLayer)
        {
          canvas->clipRRect(_makeSkRRect(_cpr, computedStyle), SkClipOp::kIntersect, true);
          const SkRect _blendRect = skRect(_cpr);
          SkCanvas::SaveLayerRec _blendBackdropRec(&_blendRect, nullptr,
              SkCanvas::kInitWithPrevious_SaveLayerFlag);
          canvas->saveLayer(_blendBackdropRec);

          SkPaint _blendPaint;
          _blendPaint.setAlphaf(_selfOpacity);
          _blendPaint.setBlendMode(_selfBlendMode);
          if (_directFilter)
            _blendPaint.setImageFilter(std::move(_directFilter));
          canvas->saveLayer(nullptr, &_blendPaint);
          _drawToCanvasImpl(canvas, false, true, true);
          canvas->restore();
          canvas->restore();
          canvas->restore();
          if (mFilterPad > 0.f) mRect = _expandedRECT;
          return;
        }

        SkPaint _compPaint;
        _compPaint.setAlphaf(_selfOpacity);
        _compPaint.setBlendMode(_selfBlendMode);
        if (_directFilter)
          _compPaint.setImageFilter(std::move(_directFilter));
        canvas->saveLayer(nullptr, &_compPaint);
        _drawToCanvasImpl(canvas, false, true, true);
        canvas->restore();
        canvas->restore();
        if (mFilterPad > 0.f) mRect = _expandedRECT;
        return;
      }

      // ── Pure transform (no opacity layer needed) ────────────────────────────
      // Match Chrome's paint-then-composite model: rasterise the subtree into an
      // offscreen surface at natural (untransformed) coordinates, then drawImage
      // back with the transform and bilinear sampling.  drawImage through an
      // SkM44 perspective CTM performs perspective-correct texture coordinate
      // interpolation (W-div per pixel), giving smooth resampling in the interior.
      // The offscreen approach also isolates the subtree so individual draw-call
      // AA flags do not affect the composite boundary.
      if (hasTransform && !_needsOpacityLayer && !_needsBlendLayer && !_needsIsolationLayer)
      {
        if (!_hasSelfBackdropFilterStyle
            && !_hasSelfMaskStyle
            && !_hasBackdropShadersDTC
            && !_hasFilterShadersDTC
            && (!_hasFilterDTC || _filterExpansionDTC <= 0.001f))
        {
          render_timing_scope _timingScope(render_timing_bucket::transform_direct);
          canvas->save();
          canvas->concat(_cmat);
          if (_hasFilterDTC)
          {
            auto _filter = glint_filter::Build(_fParsedDTC.css);
            if (_filter)
            {
              SkPaint _directPaint;
              _directPaint.setAntiAlias(true);
              _directPaint.setImageFilter(std::move(_filter));
              canvas->saveLayer(nullptr, &_directPaint);
              _drawToCanvasImpl(canvas, false, true, true);
              canvas->restore();
            }
            else
            {
              _drawToCanvasImpl(canvas, false, true, _skipSelfFilter);
            }
          }
          else
          {
            _drawToCanvasImpl(canvas, false, true, _skipSelfFilter);
          }
          canvas->restore();
          if (mFilterPad > 0.f) mRect = _expandedRECT;
          return;
        }

        render_timing_scope _timingScope(render_timing_bucket::transform_offscreen);
        const int _ow = std::max(1, static_cast<int>(std::ceil(_stackBounds.W())));
        const int _oh = std::max(1, static_cast<int>(std::ceil(_stackBounds.H())));
        // Prefer a GPU-backed surface so the subtree (including any blur filters)
        // is rendered entirely on the GPU.  Falls back to CPU raster if the canvas
        // does not expose a recording context (e.g. CPU-only build).
        auto _offscreen = canvas->makeSurface(SkImageInfo::MakeN32Premul(_ow, _oh));
        if (!_offscreen)
          _offscreen = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(_ow, _oh));
        if (_offscreen)
        {
          SkCanvas* _oc = _offscreen->getCanvas();
          _oc->translate(-_stackBounds.L, -_stackBounds.T);
          _drawToCanvasImpl(_oc, false, true, _skipSelfFilter);
          sk_sp<SkImage> _img = _offscreen->makeImageSnapshot();
          // Pass a paint with setAntiAlias(true) so Skia's CPU raster
          // scan-converter uses AntiFillPath for the projected quad boundary.
          SkPaint _imgPaint;
          _imgPaint.setAntiAlias(true);
          canvas->save();
          canvas->concat(_cmat);
            canvas->drawImage(_img.get(), _stackBounds.L, _stackBounds.T,
              SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
              &_imgPaint);
          canvas->restore();
        }
        else
        {
          // Fallback: direct draw
          canvas->save();
          canvas->concat(_cmat);
          _drawToCanvasImpl(canvas, false, true, _skipSelfFilter);
          canvas->restore();
        }
        return;
      }

      // ── Transform + opacity: same offscreen-raster path as pure transform ────
      // Chrome model: rasterise the subtree at its natural (pre-transform) bounds,
      // then composite the resulting texture with the transform matrix and opacity
      // as post-steps.  This means filter:blur and other effects see the correct
      // pre-transform pixels regardless of the current transform value (even
      // scale(0)), and saveLayer is never opened on a CTM with scale≈0 (which
      // produces a degenerate zero-area layer in the Skia CPU raster path).
      if (hasTransform)
      {
        if (!_needsBlendLayer
            && !_needsIsolationLayer
            && !_hasSelfBackdropFilterStyle
            && !_hasSelfMaskStyle
            && !_hasBackdropShadersDTC
            && !_hasFilterShadersDTC
            && (!_hasFilterDTC || _filterExpansionDTC <= 0.001f))
        {
          render_timing_scope _timingScope(render_timing_bucket::transform_direct);
          canvas->save();
          canvas->concat(_cmat);
          SkPaint _directPaint;
          _directPaint.setAntiAlias(true);
          _directPaint.setAlphaf(_selfOpacity);
          _directPaint.setBlendMode(_selfBlendMode);
          if (_hasFilterDTC)
          {
            auto _filter = glint_filter::Build(_fParsedDTC.css);
            if (_filter)
              _directPaint.setImageFilter(std::move(_filter));
          }
          canvas->saveLayer(nullptr, &_directPaint);
          _drawToCanvasImpl(canvas, false, true, true);
          canvas->restore();
          canvas->restore();
          if (mFilterPad > 0.f) mRect = _expandedRECT;
          return;
        }

        render_timing_scope _timingScope(render_timing_bucket::transform_offscreen);
        const int _ow2 = std::max(1, static_cast<int>(std::ceil(_stackBounds.W())));
        const int _oh2 = std::max(1, static_cast<int>(std::ceil(_stackBounds.H())));
        auto _offscreen2 = canvas->makeSurface(SkImageInfo::MakeN32Premul(_ow2, _oh2));
        if (!_offscreen2)
          _offscreen2 = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(_ow2, _oh2));
        if (_offscreen2)
        {
          SkCanvas* _oc2 = _offscreen2->getCanvas();
          _oc2->translate(-_stackBounds.L, -_stackBounds.T);
          _drawToCanvasImpl(_oc2, false, true, _skipSelfFilter);
          sk_sp<SkImage> _img2 = _offscreen2->makeImageSnapshot();
          SkPaint _imgPaint2;
          _imgPaint2.setAntiAlias(true);
          _imgPaint2.setAlphaf(_selfOpacity);
          _imgPaint2.setBlendMode(_selfBlendMode);
          canvas->save();
          canvas->concat(_cmat);
            canvas->drawImage(_img2.get(), _stackBounds.L, _stackBounds.T,
              SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
              &_imgPaint2);
          canvas->restore();
        }
        else
        {
          // Fallback: direct draw with saveLayer for opacity
          canvas->save();
          canvas->concat(_cmat);
          SkPaint _fbPaint;
          _fbPaint.setAlphaf(_selfOpacity);
          _fbPaint.setBlendMode(_selfBlendMode);
          canvas->saveLayer(nullptr, &_fbPaint);
          _drawToCanvasImpl(canvas, false, true, _skipSelfFilter);
          canvas->restore();
          canvas->restore();
        }
        return;
      }

      // ── Opacity / blend-mode / isolation only (no transform) ─────────────────
      // Pure mix-blend-mode needs the current backdrop pixels from the element's
      // actual clipped viewport region. Use an outer layer initialised from the
      // current canvas, then flatten the subtree through an inner blend layer.
      if (_needsBlendLayer && !_needsIsolationLayer)
      {
        canvas->save();
        canvas->clipRRect(_makeSkRRect(_cpr, computedStyle), SkClipOp::kIntersect, true);
        const SkRect _blendRect = skRect(_cpr);
        SkCanvas::SaveLayerRec _blendBackdropRec(&_blendRect, nullptr,
            SkCanvas::kInitWithPrevious_SaveLayerFlag);
        canvas->saveLayer(_blendBackdropRec);

        SkPaint _blendPaint;
        _blendPaint.setAlphaf(_selfOpacity);
        _blendPaint.setBlendMode(_selfBlendMode);
        if (_hasFilterDTC)
        {
          auto _filter = glint_filter::Build(_fParsedDTC.css);
          if (_filter)
            _blendPaint.setImageFilter(std::move(_filter));
        }
        canvas->saveLayer(nullptr, &_blendPaint);
        _drawToCanvasImpl(canvas, false, true, true);
        canvas->restore();
        canvas->restore();
        canvas->restore();
        return;
      }

      SkPaint _compPaint;
      _compPaint.setAlphaf(_selfOpacity);
      _compPaint.setBlendMode(_selfBlendMode);
      canvas->saveLayer(nullptr, &_compPaint);
      _drawToCanvasImpl(canvas, false, true, _skipSelfFilter);
      canvas->restore();
      if (mFilterPad > 0.f) mRect = _expandedRECT;
      return;
    }

    const bool _hasMask = !computedStyle.mask.empty() && computedStyle.mask != "none";

    // ── CSS mask: open an offscreen layer for the entire element paint ───────────
    // The mask is applied after the element content is drawn. This preserves the
    // previously stable standalone composition order while the mask+filter
    // semantics are investigated further.
    if (_hasMask)
      canvas->saveLayer(nullptr, nullptr);

    float _simpleBlurSigma = 0.f;
    const bool _isSimpleBlurOnly = _hasFilterDTC && glint_filter::ParseSingleBlur(_fParsedDTC.css, &_simpleBlurSigma);
    const bool _canDrawDirectBlurredBackground = _isSimpleBlurOnly
      && !_hasBackdropFilterDTC
      && !_hasMask
      && !_hasBackdropShadersDTC
      && !_hasFilterShadersDTC
      && !_hasRenderableChildren
      && innerText.empty()
      && computedStyle.backgroundGradient.empty()
      && computedStyle.backgroundImage.empty();

    if (_canDrawDirectBlurredBackground)
    {
      const glint_color _bgColor = ApplyOpacity(computedStyle.backgroundColor, computedStyle.opacity);
      if (_bgColor.A > 0)
      {
        SkPaint _blurPaint;
        _blurPaint.setAntiAlias(true);
        _blurPaint.setColor(skColor(_bgColor));
        if (_simpleBlurSigma > 0.f)
          _blurPaint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle,
                                                          std::max(0.001f, _simpleBlurSigma)));
        canvas->drawRRect(_makeSkRRect(_cpr, computedStyle), _blurPaint);
      }

      if (mFilterPad > 0.f) mRect = _expandedRECT;
      return;
    }

    const bool _canCompositeFilterInPlace = !_skipSelfFilter
      && _hasFilterDTC
      && !_hasBackdropFilterDTC
      && !_hasMask
      && !_hasBackdropShadersDTC
      && !_hasFilterShadersDTC
      && _filterExpansionDTC <= 0.001f;

    if (_canCompositeFilterInPlace)
    {
      render_timing_scope _timingScope(render_timing_bucket::filter_in_place);
      auto _filter = glint_filter::Build(_fParsedDTC.css);
      if (_filter)
      {
        SkPaint _filterPaint;
        _filterPaint.setImageFilter(std::move(_filter));
        _filterPaint.setAntiAlias(true);
        const SkRect _filterRect = skRect(_cpr);
        canvas->saveLayer(&_filterRect, &_filterPaint);
        _drawToCanvasImpl(canvas, false, false, true);
        canvas->restore();

        if (mFilterPad > 0.f) mRect = _expandedRECT;
        return;
      }
    }

    const bool _canCompositeFilterOffscreen = !_skipSelfFilter
      && _hasFilterDTC
      && !_hasBackdropFilterDTC
      && !_hasMask
      && !_hasBackdropShadersDTC
      && !_hasFilterShadersDTC
      && _filterExpansionDTC > 0.001f;

    if (_canCompositeFilterOffscreen)
    {
      render_timing_scope _timingScope(render_timing_bucket::filter_offscreen);
      auto _filter = glint_filter::Build(_fParsedDTC.css);
      const float _fp = _filterExpansionDTC;
      const int _ow = std::max(1, static_cast<int>(std::ceil(_cpr.W() + 2.f * _fp)));
      const int _oh = std::max(1, static_cast<int>(std::ceil(_cpr.H() + 2.f * _fp)));
      auto _offscreen = canvas->makeSurface(SkImageInfo::MakeN32Premul(_ow, _oh));
      if (!_offscreen)
        _offscreen = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(_ow, _oh));
      if (_offscreen && _filter)
      {
        SkCanvas* _oc = _offscreen->getCanvas();
        _oc->clear(SK_ColorTRANSPARENT);
        _oc->translate(-_cpr.L + _fp, -_cpr.T + _fp);
        _drawToCanvasImpl(_oc, false, false, true);

        sk_sp<SkImage> _img = _offscreen->makeImageSnapshot();
        SkPaint _filterPaint;
        _filterPaint.setImageFilter(std::move(_filter));
        _filterPaint.setAntiAlias(true);
        canvas->drawImage(_img.get(), _cpr.L - _fp, _cpr.T - _fp,
                          SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                          &_filterPaint);

        if (mFilterPad > 0.f) mRect = _expandedRECT;
        return;
      }
    }

    std::unique_ptr<render_timing_scope> _backdropTimingScope;

    // CSS paint model: backdrop-filter samples the canvas pixels BEHIND this element,
    // so its layer must be opened first (outermost), before the self-filter layer.
    // Opening the self-filter saveLayer first would give backdrop-filter an empty
    // layer to sample from, producing no visible effect.
    for (auto& _shId : _bdParsedDTC.shaderIds) {
      auto _shIt = shaders.find(_shId);
      if (_shIt != shaders.end() && _shIt->second->isBackdrop) {
        _shIt->second->mDpr = _getRootDpr();
        _shIt->second->beginBackdropLayer(canvas, mPaintRECT, computedStyle);
      }
    }
    if (_hasBackdropFilterDTC)
    {
      _backdropTimingScope = std::make_unique<render_timing_scope>(render_timing_bucket::backdrop);
      glint_filter::BeginBackdropLayer(canvas, mPaintRECT, computedStyle, _bdParsedDTC.css);
    }

    // Self-filter layer opens INSIDE the backdrop layer (correct LIFO nesting).
    if (_hasFilterDTC) glint_filter::BeginLayer(canvas, mPaintRECT, _fParsedDTC.css);

    // Draw non-backdrop (bg) shaders as the background layer, before content.
    for (auto& _shId : _fParsedDTC.shaderIds) {
      auto _shIt = shaders.find(_shId);
      if (_shIt != shaders.end() && !_shIt->second->isBackdrop)
        _shIt->second->drawDirect(canvas, mPaintRECT);
    }

    {
      render_timing_scope _timingScope(render_timing_bucket::self_paint);
      DrawBackgroundToCanvas(canvas);

      // Border draws after background, before content/children (CSS paint order).
      // Children that overflow via transform may paint on top — this is correct per spec.
      _drawBorderSkia(canvas, computedStyle, mRect);
    }

    // overflow clip + scroll translation (mirrors Draw() / Skia path).
    const bool _clipContent   = (computedStyle.overflowX != "visible" || computedStyle.overflowY != "visible");
    const bool _hasScrollBars = (mScrollbarV || mScrollbarH || mScrollCorner);

    if (_clipContent)
    {
      canvas->save();
      const glint_rect clip = _hasScrollBars ? _getContentClipRect() : GetPaintRECT();
      {
        auto _cr = computedStyle.resolveCornerRadii(clip.W(), clip.H());
        if (_cr[0]>0.f || _cr[1]>0.f || _cr[2]>0.f || _cr[3]>0.f)
        {
          SkVector _ck[4] = {{_cr[0],_cr[0]},{_cr[1],_cr[1]},{_cr[2],_cr[2]},{_cr[3],_cr[3]}};
          SkRRect rr; rr.setRectRadii(SkRect::MakeLTRB(clip.L, clip.T, clip.R, clip.B), _ck);
          canvas->clipRRect(rr, true);
        }
        else
        {
          canvas->clipRect(SkRect::MakeLTRB(clip.L, clip.T, clip.R, clip.B));
        }
      }

      if (mScrollLeft != 0.f || mScrollTop != 0.f)
        canvas->translate(-mScrollLeft, -mScrollTop);
    }

    // Fast path: most parents have all children at zIndex 0 (e.g. labels in
    // text content). Skip the sort vector entirely and walk mChildren directly.
    // computedStyle.zIndex is kept fresh by tickTransitionsAll() which runs
    // before draw.
    if (!_hasSpecialDirectChildPaintOrder())
    {
      {
        render_timing_scope _timingScope(render_timing_bucket::content);
        DrawContentToCanvas(canvas);
      }

      {
        render_timing_scope _timingScope(render_timing_bucket::children);
        for (auto& child : mChildren)
        {
          auto* c = child.get();
          if (c == mScrollbarV || c == mScrollbarH || c == mScrollCorner) continue;
          const auto _childStart = std::chrono::steady_clock::now();
          c->DrawToCanvas(canvas);
          _recordChildSubtreeTiming(
            c,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _childStart).count());
        }
      }
    }
    else
    {
      std::vector<glint_element*> _negativeChildren;
      std::vector<glint_element*> _normalChildren;
      std::vector<glint_element*> _zeroChildren;
      std::vector<glint_element*> _positiveChildren;
      std::vector<glint_element*> _overlayChildren;
      _collectDirectChildPaintOrder(
        _negativeChildren, _normalChildren, _zeroChildren, _positiveChildren, _overlayChildren);

      {
        render_timing_scope _timingScope(render_timing_bucket::children);
        for (auto* child : _negativeChildren)
        {
          const auto _childStart = std::chrono::steady_clock::now();
          child->DrawToCanvas(canvas);
          _recordChildSubtreeTiming(
            child,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _childStart).count());
        }
      }

      {
        render_timing_scope _timingScope(render_timing_bucket::content);
        DrawContentToCanvas(canvas);
      }

      {
        render_timing_scope _timingScope(render_timing_bucket::children);
        for (auto* child : _normalChildren)
        {
          const auto _childStart = std::chrono::steady_clock::now();
          child->DrawToCanvas(canvas);
          _recordChildSubtreeTiming(
            child,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _childStart).count());
        }

        for (auto* child : _zeroChildren)
        {
          const auto _childStart = std::chrono::steady_clock::now();
          child->DrawToCanvas(canvas);
          _recordChildSubtreeTiming(
            child,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _childStart).count());
        }

        for (auto* child : _positiveChildren)
        {
          const auto _childStart = std::chrono::steady_clock::now();
          child->DrawToCanvas(canvas);
          _recordChildSubtreeTiming(
            child,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _childStart).count());
        }

        for (auto* child : _overlayChildren)
        {
          const auto _childStart = std::chrono::steady_clock::now();
          child->DrawToCanvas(canvas);
          _recordChildSubtreeTiming(
            child,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _childStart).count());
        }
      }
    }

    if (_clipContent) canvas->restore();

    // Close in LIFO order: self-filter (innermost) before backdrop (outermost).
    if (_hasFilterDTC) glint_filter::EndLayer(canvas);

    if (_hasBackdropFilterDTC)
    {
      glint_filter::EndBackdropLayer(canvas);
      _backdropTimingScope.reset();
    }
    // Close backdrop shader layers in REVERSE order.
    for (auto _shIt2 = _bdParsedDTC.shaderIds.rbegin(); _shIt2 != _bdParsedDTC.shaderIds.rend(); ++_shIt2) {
      auto _shSit = shaders.find(*_shIt2);
      if (_shSit != shaders.end() && _shSit->second->isBackdrop)
        _shSit->second->endBackdropLayer(canvas);
    }

    // CSS parity: border-radius shapes the source paint, filter composites that
    // rounded source, and mask applies afterward to the filtered result.
    if (_hasMask)
    {
      render_timing_scope _timingScope(render_timing_bucket::mask);

      // Resolve the canonical origin box once (used for both shader bounds and
      // cache keying). For multi-layer masks, layers can override `origin` per
      // layer — those layers are not common in the demo and still benefit from
      // the parsed-layer cache; only the gradient shader cache is keyed on
      // this box. Layers that override origin will fall through to a per-frame
      // shader rebuild via the box compare in the loop below.
      glint_rect _maskCacheBox;
      {
        const glint_mask_layer _probe;  // default origin = "border-box"
        _maskCacheBox = _maskBoxRect(_probe.origin);
      }

      // Reuse parsed layers + gradient shaders when none of the mask-* style
      // strings nor the origin box have changed since the last paint.
      const bool _maskCacheHit =
            mMaskCacheValid
        && mMaskCacheMask      == computedStyle.mask
        && mMaskCacheMode      == computedStyle.maskMode
        && mMaskCachePosition  == computedStyle.maskPosition
        && mMaskCacheSize      == computedStyle.maskSize
        && mMaskCacheRepeat    == computedStyle.maskRepeat
        && mMaskCacheOrigin    == computedStyle.maskOrigin
        && mMaskCacheClip      == computedStyle.maskClip
        && mMaskCacheComposite == computedStyle.maskComposite
        && mMaskCacheBoxL      == _maskCacheBox.L
        && mMaskCacheBoxT      == _maskCacheBox.T
        && mMaskCacheBoxR      == _maskCacheBox.R
        && mMaskCacheBoxB      == _maskCacheBox.B;

      if (!_maskCacheHit)
      {
        mMaskCacheLayers   = glint_parse_mask_layers(computedStyle);
        mMaskCacheShaders.assign(mMaskCacheLayers.size(), sk_sp<SkShader>{});
        for (size_t _i = 0; _i < mMaskCacheLayers.size(); ++_i)
        {
          const auto& _ml = mMaskCacheLayers[_i];
          if (_ml.type == glint_mask_layer::GRADIENT)
          {
            const glint_rect _ob = _maskBoxRect(_ml.origin);
            mMaskCacheShaders[_i] = glint_mask_gradient_shader(_ml, _ob);
          }
        }
        mMaskCacheMask      = computedStyle.mask;
        mMaskCacheMode      = computedStyle.maskMode;
        mMaskCachePosition  = computedStyle.maskPosition;
        mMaskCacheSize      = computedStyle.maskSize;
        mMaskCacheRepeat    = computedStyle.maskRepeat;
        mMaskCacheOrigin    = computedStyle.maskOrigin;
        mMaskCacheClip      = computedStyle.maskClip;
        mMaskCacheComposite = computedStyle.maskComposite;
        mMaskCacheBoxL      = _maskCacheBox.L;
        mMaskCacheBoxT      = _maskCacheBox.T;
        mMaskCacheBoxR      = _maskCacheBox.R;
        mMaskCacheBoxB      = _maskCacheBox.B;
        mMaskCacheValid     = true;
      }

      const auto& _maskLayers = mMaskCacheLayers;

      SkPaint _dstInPaint;
      _dstInPaint.setBlendMode(SkBlendMode::kDstIn);
      canvas->saveLayer(nullptr, &_dstInPaint);

      bool _firstMaskLayer = true;
      for (size_t _li = 0; _li < _maskLayers.size(); ++_li)
      {
        const auto& _ml = _maskLayers[_li];
        const glint_rect _maskOriginBox = _maskBoxRect(_ml.origin);
        sk_sp<SkShader> _mShader;
        sk_sp<SkImage>  _mImg;

        if (_ml.type == glint_mask_layer::GRADIENT)
        {
          _mShader = mMaskCacheShaders[_li];
          // Origin box mismatch (rare — happens when a layer overrides `origin`
          // and the cache was keyed on default). Rebuild the shader for this
          // layer only without invalidating the parsed layer vector.
          if (!_mShader)
            _mShader = glint_mask_gradient_shader(_ml, _maskOriginBox);
        }
        else if (_ml.type == glint_mask_layer::URL_ELEMENT_ID)
        {
          glint_element* _mSrc = findMaskSourceElement(_ml.urlTarget);
          if (_mSrc && _mSrc != this)
          {
            const int _mW = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.W())));
            const int _mH = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.H())));
            auto _mSurf = canvas->makeSurface(SkImageInfo::MakeN32Premul(_mW, _mH));
            if (!_mSurf)
              _mSurf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(_mW, _mH));
            if (_mSurf)
            {
              SkCanvas* _mc = _mSurf->getCanvas();
              _mc->clear(SK_ColorTRANSPARENT);
              _mc->translate(-_maskOriginBox.L, -_maskOriginBox.T);
              _mSrc->DrawToCanvas(_mc);
              _mImg = _mSurf->makeImageSnapshot();
            }
          }
        }
        else if (_ml.type == glint_mask_layer::URL_SVG_FILE ||
                 _ml.type == glint_mask_layer::URL_SVG_FILE_ID)
        {
          auto _dom = glint_load_svg_dom(_ml.urlTarget, _getOnRequest(), this, _getNetworkLog());
          if (_dom)
          {
            const int _mW = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.W())));
            const int _mH = std::max(1, static_cast<int>(std::ceil(_maskOriginBox.H())));
            const char* _fId = (_ml.type == glint_mask_layer::URL_SVG_FILE_ID)
                                ? _ml.urlFragId.c_str() : nullptr;
            _mImg = glint_rasterize_svg(_dom, _mW, _mH, _fId);
          }
        }
        else if (_ml.type == glint_mask_layer::URL_IMAGE)
        {
          _mImg = glint_load_image(_ml.urlTarget, _getOnRequest(), this, _getNetworkLog());
        }

        SkPaint _mp;
        _mp.setBlendMode(glint_mask_accum_blend_mode(_ml.composite, _firstMaskLayer));
        if (_ml.mode == "luminance")
          _mp.setColorFilter(glint_mask_luma_color_filter());

        canvas->save();
        _clipToMaskBox(canvas, _ml.clip);

        if (_mImg)
        {
          const SkRect _srcR = SkRect::MakeWH(static_cast<float>(_mImg->width()),
                                              static_cast<float>(_mImg->height()));
          const SkRect _dstR = glint_mask_image_dst_rect(_mImg, _maskOriginBox, _ml);
          canvas->drawImageRect(_mImg, _srcR, _dstR,
                                SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear),
                                &_mp,
                                SkCanvas::kStrict_SrcRectConstraint);
        }
        else if (_mShader)
        {
          _mp.setShader(_mShader);
          canvas->drawPaint(_mp);
        }

        canvas->restore();
        _firstMaskLayer = false;
      }
      canvas->restore();
      canvas->restore();
    }

    if (mFilterPad > 0.f) mRect = _expandedRECT;
    // Keep redraws going while any shader is animated.
    for (auto& [_sid, _s] : shaders)
      if (_s->animated) { setDirty(false); break; }

    // Draw scrollbar children in screen space.
    if (mScrollbarV) mScrollbarV->DrawToCanvas(canvas);
    if (mScrollbarH) mScrollbarH->DrawToCanvas(canvas);
    if (mScrollCorner) mScrollCorner->DrawToCanvas(canvas);

    // Focus ring — drawn inline at this element's z-order position (mirrors _drawImpl).
    if (mIsFocused && mAcceptsFocus && mTabStop && _isFocusViaKeyboard())
    {
      const glint_rect _fr = GetPaintRECT();
      SkPaint _fp;
      _fp.setStyle(SkPaint::kStroke_Style);
      _fp.setColor(SkColorSetARGB(210, 74, 158, 255));
      _fp.setStrokeWidth(2.f);
      _fp.setAntiAlias(true);
      canvas->drawRect(SkRect::MakeLTRB(_fr.L, _fr.T, _fr.R, _fr.B), _fp);
    }

    if (glint_debug::colorizedBorders && static_cast<void*>(mRoot) != glint_debug::inspectorDoc)
    {
      glint_color bc = glint_debug::borderColorFor(this);
      SkPaint _dbgP;
      _dbgP.setStyle(SkPaint::kStroke_Style);
      _dbgP.setColor(skColor(bc));
      _dbgP.setStrokeWidth(1.0f);
      _dbgP.setAntiAlias(true);
      const glint_rect& _dbgR = mFilterPad > 0.f ? mPaintRECT : mRect;
      canvas->drawRect(SkRect::MakeLTRB(_dbgR.L, _dbgR.T, _dbgR.R, _dbgR.B), _dbgP);
    }

    // transform + opacity: handled at top of _drawToCanvasImpl via early-return layer path
  }

  virtual void DrawToCanvas(SkCanvas* canvas)
  {
    _drawToCanvasImpl(canvas, true);
  }
