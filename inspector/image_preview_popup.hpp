#pragma once

/**
 * inspector/image_preview_popup.hpp
 * InspImagePreviewPopup — hover-triggered img thumbnail overlay for the
 * glint inspector's Computed panel.
 *
 * Rendered as a position:absolute element on the inspector's root canvas
 * (a sibling of the right-sidebar column, not inside the scroll container).
 * Uses cached runtime resource bytes only; the inspector never fetches or
 * reads files on its own.
 *
 * Covered img-bearing properties:
 *   • background-img  — url(path.png / path.svg)
 *   • mask              — url(path.png / path.svg) [first url() token only]
 *
 * State machine (all transitions happen on the inspector Win32 thread):
 *   Idle ──(scheduleShow)──▶ ShowPending ──(150 ms)──▶ Visible
 *                                │                        │
 *                            (cancel)                 (scheduleHide)
 *                                │                        │
 *                                ▼                        ▼
 *                              Idle          HidePending ──(100 ms)──▶ Idle
 *
 * Timer IDs used (must not collide with glint_window_win32::SKUI_ANIM_TIMER=1
 * or the inspector stats timer WM_INSP_TIMER_ID=2):
 *   3 = WM_INSP_PREVIEW_SHOW_TIMER
 *   4 = WM_INSP_PREVIEW_HIDE_TIMER
 *
 * The owning glint_inspector_window manages the Win32 timers and calls
 * commitShow() / commitHide() when they fire.
 */

#include "../glint_element.hpp"
#include "../render/glint_resource_cache.hpp"
#include "../render/glint_svg_cache.hpp"
#include "../utils/glint_path.hpp"

#include <cmath>
#include <functional>
#include <string>

#  include "include/core/SkData.h"
#  include "include/core/SkImage.h"

// ── Helper: extract the first img/SVG path from a CSS url() token ──────────
//
// Accepts:
//   background-img  → url("/abs/path.png") or url(rel.png)
//   mask              → url("path.svg") no-repeat ...  (stop at first token)
//
// Returns the bare filesystem path (no quotes), or "" if not extractable.
static inline std::string _inspExtractUrlPath(const std::string& val)
{
    const char* kUrl = "url(";
    const size_t pos = val.find(kUrl);
    if (pos == std::string::npos) return {};

    size_t inner = pos + 4;  // skip "url("

    // Optional quote
    char quote = 0;
    if (inner < val.size() && (val[inner] == '"' || val[inner] == '\''))
        quote = val[inner++];

    size_t end;
    if (quote)
    {
        end = val.find(quote, inner);
        if (end == std::string::npos) return {};
    }
    else
    {
        end = val.find(')', inner);
        if (end == std::string::npos) return {};
    }

    std::string path = val.substr(inner, end - inner);
    while (!path.empty() && std::isspace((unsigned char)path.front())) path.erase(path.begin());
    while (!path.empty() && std::isspace((unsigned char)path.back()))  path.pop_back();

    if (!path.empty() && path[0] == '#') return {};
    if (path.rfind("data:", 0) == 0) return {};

    return path;
}

// ── Helper: classify extension ────────────────────────────────────────────────
static inline bool _inspPathIsSvg(const std::string& path)
{
    if (path.size() < 4) return false;
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".svg";
}

// ── Thumbnail element ─────────────────────────────────────────────────────────
// A child of InspImagePreviewPopup that paints the actual bitmap inside a
// dark-background thumbnail area.  Has no children; completely custom draw.
class _InspThumbElement : public glint_element
{
public:
    sk_sp<SkImage> mImg;   // null when not loaded or SVG
    glint_svg           mSvg;   // populated for SVG previews
    bool           mIsSvg  = false;
    bool           mLoaded = false; // true after at least one load attempt

    _InspThumbElement()
    {
        style.width      = "100%";
        style.flexGrow   = 1.f;
        style.overflow   = "hidden";
    }

    const char* typeName() const override { return "_insp_thumb"; }

    void DrawToCanvas(SkCanvas* canvas) override
    {
        const glint_rect& r = mRect;

        // Checkered background (standard transparency indicator).
        // Two alternating grey shades in 8×8 px tiles.
        {
            constexpr float kTile = 8.f;
            const SkColor kDark  = SkColorSetARGB(255, 64,  64,  64);
            const SkColor kLight = SkColorSetARGB(255, 96,  96,  96);
            SkPaint cp;
            cp.setAntiAlias(false);

            // Clip to the element rect so we never paint outside it.
            canvas->save();
            canvas->clipRect(skRect(r));

            const int cols = static_cast<int>(std::ceil(r.W() / kTile)) + 1;
            const int rows = static_cast<int>(std::ceil(r.H() / kTile)) + 1;
            // Tile origin aligned to pixel grid
            const float ox = std::floor(r.L / kTile) * kTile;
            const float oy = std::floor(r.T / kTile) * kTile;
            for (int row = 0; row < rows; ++row)
            {
                for (int col = 0; col < cols; ++col)
                {
                    cp.setColor(((row + col) & 1) ? kLight : kDark);
                    const float tx = ox + col * kTile;
                    const float ty = oy + row * kTile;
                    canvas->drawRect(SkRect::MakeXYWH(tx, ty, kTile, kTile), cp);
                }
            }
            canvas->restore();
        }

        if (mImg)
        {
            const float iw = static_cast<float>(mImg->width());
            const float ih = static_cast<float>(mImg->height());
            const float rw = r.W(), rh = r.H();
            const float scale = std::min(rw / std::max(iw, 1.f), rh / std::max(ih, 1.f));
            const float dw = iw * scale;
            const float dh = ih * scale;
            const float dx = r.L + (rw - dw) * 0.5f;
            const float dy = r.T + (rh - dh) * 0.5f;
            const SkRect dst = SkRect::MakeXYWH(dx, dy, dw, dh);

            SkPaint imgPaint;
            imgPaint.setAntiAlias(true);
            canvas->drawImageRect(mImg, dst,
                                  SkSamplingOptions(SkFilterMode::kLinear),
                                  &imgPaint);
        }
        else if (mSvg.IsValid())
        {
            const float iw = std::max(1.f, mSvg.W());
            const float ih = std::max(1.f, mSvg.H());
            const float rw = r.W(), rh = r.H();
            const float scale = std::min(rw / iw, rh / ih);
            const float dw = iw * scale;
            const float dh = ih * scale;
            const float dx = r.L + (rw - dw) * 0.5f;
            const float dy = r.T + (rh - dh) * 0.5f;

            glint_canvas svgGraphics(canvas, mpG ? mpG->GetWindow() : nullptr);
            svgGraphics.DrawSVG(mSvg, glint_rect(dx, dy, dx + dw, dy + dh));
        }
        else
        {
            // Placeholder: centred label
            const char* label = mLoaded ? (mIsSvg ? "SVG" : "?") : "\xe2\x80\x94";
            SkPaint tp;
            tp.setColor(SkColorSetARGB(180, 130, 130, 130));
            tp.setAntiAlias(true);
            SkFont fnt = skFont(mIsSvg ? 14.f : 11.f);
            SkRect bounds;
            fnt.measureText(label, std::strlen(label), SkTextEncoding::kUTF8, &bounds);
            canvas->drawString(label,
                               r.L + (r.W() - bounds.width()) * 0.5f,
                               r.T + (r.H() - bounds.height()) * 0.5f - bounds.top(),
                               fnt, tp);
        }

        // Subtle inner border
        SkPaint brd;
        brd.setColor(SkColorSetARGB(80, 100, 100, 100));
        brd.setStyle(SkPaint::kStroke_Style);
        brd.setStrokeWidth(1.f);
        brd.setAntiAlias(false);
        canvas->drawRect(skRect(r), brd);
    }
};

// ── InspImagePreviewPopup ─────────────────────────────────────────────────────
class InspImagePreviewPopup : public glint_element
{
public:
    // Popup outer dimensions
    static constexpr float kW      = 240.f;
    static constexpr float kThumbH = 132.f;
    static constexpr float kLabelH = 34.f;
    static constexpr float kH      = kThumbH + kLabelH;  // 166.f

    // State machine
    enum class State { Idle, ShowPending, Visible, HidePending };
    State mState = State::Idle;

    // Pending anchor: set by prepareShow(), used in commitShow()
    std::string mPendingPath;
    float mAnchorLeft   = 0.f;
    float mAnchorTop    = 0.f;
    float mAnchorBottom = 0.f;

    // Root-canvas dimensions — set by the inspector after buildUI so
    // commitShow() can clamp within bounds.  Updated on WM_SIZE.
    float mRootW = 820.f;
    float mRootH = 650.f;

    InspImagePreviewPopup()
    {
        style.position        = "absolute";
        style.display         = "none";
        style.width           = kW;
        style.height          = kH;
        style.zIndex          = 50;
        style.backgroundColor = glint_color(255, 33, 33, 33);
        style.borderColor     = glint_color(255, 72, 72, 72);
        style.borderWidth     = 1.f;
        style.borderRadius    = "6px";
        style.overflow        = "hidden";
        style.flexDirection   = "column";

        // Thumbnail area (fills remainder after labels)
        mThumb = new _InspThumbElement();
        addChild(mThumb);

        // Dimensions label (e.g. "512 × 256")
        auto* dimRow = new glint_element();
        dimRow->style.width           = "100%";
        dimRow->style.height          = 17.f;
        dimRow->style.paddingLeft     = 6.f;
        dimRow->style.paddingRight    = 6.f;
        dimRow->style.alignItems      = "center";
        dimRow->style.backgroundColor = glint_color(255, 25, 25, 25);
        addChild(dimRow);

        mDimLabel = new glint_element();
        mDimLabel->innerText      = "";
        mDimLabel->style.flexGrow = 1.f;
        mDimLabel->style.fontSize = 10.f;
        mDimLabel->style.color    = glint_color(255, 140, 140, 140);
        mDimLabel->style.overflow = "hidden";
        mDimLabel->style.textAlign  = EAlign::Near;
        mDimLabel->style.height     = "100%";
        dimRow->addChild(mDimLabel);

        // Path/filename label
        auto* pathRow = new glint_element();
        pathRow->style.width           = "100%";
        pathRow->style.height          = 17.f;
        pathRow->style.paddingLeft     = 6.f;
        pathRow->style.paddingRight    = 6.f;
        pathRow->style.alignItems      = "center";
        pathRow->style.backgroundColor = glint_color(255, 28, 28, 28);
        addChild(pathRow);

        mPathLabel = new glint_element();
        mPathLabel->innerText      = "";
        mPathLabel->style.flexGrow = 1.f;
        mPathLabel->style.fontSize = 10.f;
        mPathLabel->style.color    = glint_color(255, 100, 140, 200);
        mPathLabel->style.overflow = "hidden";
        mPathLabel->style.textAlign  = EAlign::Near;
        mPathLabel->style.height     = "100%";
        pathRow->addChild(mPathLabel);
    }

    const char* typeName() const override { return "insp_image_preview_popup"; }

    // ── Called by inspector event callbacks ───────────────────────────────────

    // Record the pending show target; inspector starts the 150 ms timer.
    void prepareShow(const std::string& path,
                     float anchorLeft, float anchorTop, float anchorBottom)
    {
        mPendingPath  = path;
        mAnchorLeft   = anchorLeft;
        mAnchorTop    = anchorTop;
        mAnchorBottom = anchorBottom;
        mState        = State::ShowPending;
    }

    // Fires when the 150 ms show timer completes.
    void commitShow()
    {
        _loadImage(mPendingPath);
        _updateLabels(mPendingPath);
        _reposition();
        style.display = "flex";
        mState = State::Visible;
        setDirty(false);
    }

    // Begin hide sequence; inspector starts the 100 ms hide timer.
    void prepareHide()
    {
        mState = State::HidePending;
    }

    // Fires when the 100 ms hide timer completes.
    void commitHide()
    {
        style.display  = "none";
        mState         = State::Idle;
        mPendingPath.clear();
        setDirty(false);
    }

    // Instant dismiss (selection change, tab switch, window close).
    void dismiss()
    {
        mPendingPath.clear();
        if (mThumb)
        {
            mThumb->mImg.reset();
            mThumb->mSvg = glint_svg{ nullptr };
        }
        style.display = "none";
        mState        = State::Idle;
        setDirty(false);
    }

private:
    _InspThumbElement* mThumb     = nullptr;
    glint_element*      mDimLabel  = nullptr;
    glint_element*      mPathLabel = nullptr;

    void _loadImage(const std::string& path)
    {
        if (!mThumb) return;
        mThumb->mImg.reset();
        mThumb->mSvg = glint_svg{ nullptr };
        mThumb->mLoaded = false;

        const bool isSvg = _inspPathIsSvg(path);
        mThumb->mIsSvg = isSvg;

        if (!path.empty())
        {
            GlintCachedResource cachedRes;
            const auto resourceType = isSvg ? glint_resource_request::Type::SVG
                                            : glint_resource_request::Type::Image;
            if (glint_resource_cache_lookup(path, resourceType, &cachedRes))
            {
                mThumb->mLoaded = true;
                if (isSvg)
                {
                    if (cachedRes.data)
                        mThumb->mSvg = glint_load_svg_cached(path, cachedRes.data);
                }
                else if (cachedRes.data)
                {
                    std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
                    auto& cache = glint_img_cache();
                    auto it = cache.find(path);
                    if (it != cache.end())
                    {
                        mThumb->mImg = it->second;
                    }
                    else
                    {
                        mThumb->mImg = SkImages::DeferredFromEncodedData(cachedRes.data);
                        cache[path] = mThumb->mImg;
                    }
                }
            }
        }
        mThumb->setDirty(false);
    }

    void _updateLabels(const std::string& path)
    {
        // Dimensions
        std::string dimText;
        if (mThumb && mThumb->mImg)
        {
            dimText = std::to_string(mThumb->mImg->width())
                    + " \xc3\x97 "    // × (UTF-8 MULTIPLICATION SIGN)
                    + std::to_string(mThumb->mImg->height())
                    + " px";
        }
        else if (mThumb && mThumb->mSvg.IsValid())
        {
            dimText = std::to_string(static_cast<int>(std::round(mThumb->mSvg.W())))
                    + " \xc3\x97 "
                    + std::to_string(static_cast<int>(std::round(mThumb->mSvg.H())))
                    + " px";
        }
        else if (_inspPathIsSvg(path))
        {
            dimText = "SVG vector";
        }
        else if (!path.empty())
        {
            dimText = (mThumb && mThumb->mLoaded) ? "Unavailable" : "Not loaded";
        }
        if (mDimLabel) mDimLabel->innerText = dimText;

        // Filename (last path segment, truncated)
        std::string filename = path;
        const size_t slash = filename.find_last_of("/\\");
        if (slash != std::string::npos) filename = filename.substr(slash + 1);
        if (filename.size() > 30) filename = filename.substr(0, 27) + "...";
        if (mPathLabel) mPathLabel->innerText = filename;
    }

    void _reposition()
    {
        // Prefer BELOW the anchor row; flip ABOVE if there's not enough room.
        float top = mAnchorBottom + 4.f;
        if (top + kH > mRootH - 8.f)
            top = mAnchorTop - kH - 4.f;
        if (top < 40.f) top = 40.f; // clamp below the inspector header

        // Horizontally: 8 px to the left of the anchor (i.e. left of the panel column).
        float left = mAnchorLeft - kW - 8.f;
        if (left < 4.f) left = 4.f;
        if (left + kW > mRootW - 4.f) left = mRootW - kW - 4.f;

        style.top  = top;
        style.left = left;
    }
};
