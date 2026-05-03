#pragma once

/**
 * glint_scrollbar.hpp
 * CSS-compatible scrollbar component for glint.
 *
 * glint_scrollbar is a plain glint_element subclass automatically added by
 * glint_element::_ensureScrollbars() when overflow-x or overflow-y is set
 * to "scroll" or "auto" on a parent component.
 *
 * It should NOT be created manually. Set overflow on the scrollable component:
 *
 *   _c.style.overflow   = "auto";      // or "scroll"
 *   _c.style.overflowY  = "scroll";    // per-axis control
 *
 * Scrollbar appearance is inherted from the parent's style:
 *   _c.style.scrollbarWidth       = 12.f;
 *   _c.style.scrollbarThumbColor  = "#888";
 *   _c.style.scrollbarTrackColor  = "#222";
 *   _c.style.scrollbarButtonColor = "#444";
 *
 * DOM:
 *   component->element.scrollTop = 200.f;      // scroll programmatically
 *   float total = component->element.scrollHeight;
 *
 * Arrow buttons are hidden by default. Show them via:
 *   scrollbar->buttonsVisible = true;   setDirty(false);
 *
 * Internal structure (automatically created, visible in inspector):
 *   glint_scrollbar                       (absolute, fills one edge)
 *     glint_scrollbar::ArrowComp [minus]  (up/left button, square)
 *     glint_scrollbar::TrackComp          (track background)
 *       glint_scrollbar::ThumbComp        (draggable thumb, absolute inside track)
 *     glint_scrollbar::ArrowComp [plus]   (down/right button, square)
 *
 * After this file is included, glint_element::_ensureScrollbars() is defined.
 */

#include "../glint_element.hpp"

#include <algorithm>

// ── glint_scrollbar ───────────────────────────────────────────────────────────

class glint_scrollbar : public glint_element
{
public:
  enum class Axis { Vertical, Horizontal };

  Axis             mAxis;
  glint_element* mScrollParent  = nullptr;   // the component that owns this scrollbar
  float            mLineHeight    = 40.f;      // pixels scrolled per arrow-button click
  bool             buttonsVisible = false;     // show/hide up-down (left-right) arrow buttons

  // Owns of internal child components (pointers into mChildren).
  glint_element* mBtnMinus = nullptr;   // up  / left  arrow
  glint_element* mTrack    = nullptr;   // track background
  glint_element* mThumb    = nullptr;   // draggable thumb (child of mTrack)
  glint_element* mBtnPlus  = nullptr;   // down / right arrow

  glint_scrollbar(Axis axis, glint_element* scrollParent)
    : mAxis(axis), mScrollParent(scrollParent)
  {
    style.position = "absolute";

    const glint_color thumbColor = mScrollParent
      ? mScrollParent->style.scrollbarThumbColor.value  : glint_color(255, 110, 110, 110);
    const glint_color trackColor = mScrollParent
      ? mScrollParent->style.scrollbarTrackColor.value  : glint_color(255,  40,  40,  40);
    const glint_color btnColor   = mScrollParent
      ? mScrollParent->style.scrollbarButtonColor.value : glint_color(255,  65,  65,  65);

    auto* btnM = new ArrowComp(this, true);
    btnM->style.backgroundColor = btnColor;
    btnM->style.display         = "none";
    addChild(btnM);
    mBtnMinus = btnM;

    auto* track = new TrackComp(this);
    track->style.backgroundColor = trackColor;
    addChild(track);
    mTrack = track;

    auto* thumb = new ThumbComp(this);
    thumb->style.backgroundColor = thumbColor;
    thumb->style.borderRadius    = 3.f;
    track->addChild(thumb);
    mThumb = thumb;

    auto* btnP = new ArrowComp(this, false);
    btnP->style.backgroundColor = btnColor;
    btnP->style.display         = "none";
    addChild(btnP);
    mBtnPlus = btnP;
  }

  const char* typeName() const override
  {
    return (mAxis == Axis::Vertical) ? "scrollbar-v" : "scrollbar-h";
  }

  bool isVertical() const { return mAxis == Axis::Vertical; }

  void Layout(glint_canvas* g) override;

  // ── Scroll accessors ───────────────────────────────────────────────────────

  float getScrollPos()   const
  {
    if (!mScrollParent) return 0.f;
    return isVertical() ? mScrollParent->mScrollTop : mScrollParent->mScrollLeft;
  }

  float getViewportSize() const
  {
    if (!mScrollParent) return 1.f;
    const float sbW = mScrollParent->style.scrollbarWidth;
    const glint_rect pr  = mScrollParent->GetPaintRECT();
    if (isVertical())
      return pr.H() - (mScrollParent->mScrollbarH && mScrollParent->mScrollbarH->style.display != "none" ? sbW : 0.f);
    else
      return pr.W() - (mScrollParent->mScrollbarV && mScrollParent->mScrollbarV->style.display != "none" ? sbW : 0.f);
  }

  float getContentSize() const
  {
    if (!mScrollParent) return 1.f;
    return isVertical() ? mScrollParent->mScrollHeight : mScrollParent->mScrollWidth;
  }

  float getScrollMax() const
  {
    return std::max(0.f, getContentSize() - getViewportSize());
  }

  void setScrollPos(float v)
  {
    if (!mScrollParent) return;
    const float clamped = std::max(0.f, std::min(v, getScrollMax()));
    if (isVertical())
      mScrollParent->mScrollTop  = clamped;
    else
      mScrollParent->mScrollLeft = clamped;
    // Sync element property (read path — no recursion since we set the field directly).
    mScrollParent->element.scrollWidth  = mScrollParent->mScrollWidth;
    mScrollParent->element.scrollHeight = mScrollParent->mScrollHeight;
    // Full setDirty: scrollbar thumb position is computed during Layout().
    mScrollParent->setDirty(false);
  }

  // ── Thumb drag ─────────────────────────────────────────────────────────────

  void startThumbDrag(float mouseX, float mouseY)
  {
    mDragging        = true;
    mDragStartScroll = getScrollPos();
    mDragStartMouse  = isVertical() ? mouseY : mouseX;
  }

  void updateThumbDrag(float mouseX, float mouseY)
  {
    if (!mDragging || !mTrack) return;
    const float trackSize  = isVertical() ? mTrack->GetPaintRECT().H() : mTrack->GetPaintRECT().W();
    const float viewSize   = getViewportSize();
    const float contentSz  = getContentSize();
    const float scrollMax  = getScrollMax();
    if (trackSize <= 0.f || contentSz <= 0.f) return;

    const float ratio       = std::min(1.f, viewSize / contentSz);
    const float thumbSz     = std::max(20.f, trackSize * ratio);
    const float trackUsable = trackSize - thumbSz;
    if (trackUsable <= 0.f) return;

    const float delta = (isVertical() ? mouseY : mouseX) - mDragStartMouse;
    setScrollPos(mDragStartScroll + delta * scrollMax / trackUsable);
  }

  void endThumbDrag() { mDragging = false; }

  // ── Track click (page scroll) ──────────────────────────────────────────────

  void onTrackClick(float mouseX, float mouseY)
  {
    if (!mThumb) return;
    const glint_rect& tr = mThumb->GetPaintRECT();
    const float thumbCenter = isVertical()
      ? (tr.T + tr.B) * 0.5f
      : (tr.L + tr.R) * 0.5f;
    const float clickPos = isVertical() ? mouseY : mouseX;
    const float pageSize = getViewportSize();
    setScrollPos(getScrollPos() + (clickPos < thumbCenter ? -pageSize : pageSize));
  }

private:
  bool  mDragging        = false;
  float mDragStartScroll = 0.f;
  float mDragStartMouse  = 0.f;

  // ── Inner component types ──────────────────────────────────────────────────

  /** Draggable thumb — routes mousedown/drag/up to parent scrollbar. */
  struct ThumbComp : public glint_element
  {
    glint_scrollbar* mScrollbar;
    explicit ThumbComp(glint_scrollbar* sb) : mScrollbar(sb)
    { style.position = "absolute"; }

    void OnMouseDown(float x, float y, const glint_mouse_mod&) override
    { mScrollbar->startThumbDrag(x, y); }

    void OnMouseDrag(float x, float y, float, float, const glint_mouse_mod&) override
    { mScrollbar->updateThumbDrag(x, y); }

    void OnMouseUp(float, float, const glint_mouse_mod&) override
    { mScrollbar->endThumbDrag(); }

    const char* typeName() const override { return "scrollbar-thumb"; }
  };

  /** Track background — page-scrolls on click. */
  struct TrackComp : public glint_element
  {
    glint_scrollbar* mScrollbar;
    explicit TrackComp(glint_scrollbar* sb) : mScrollbar(sb) {}

    void OnMouseDown(float x, float y, const glint_mouse_mod&) override
    { mScrollbar->onTrackClick(x, y); }

    const char* typeName() const override { return "scrollbar-track"; }
  };

  /** Arrow button — scrolls by mLineHeight per click with hover colour feedback. */
  struct ArrowComp : public glint_element
  {
    glint_scrollbar* mScrollbar;
    bool             mIsMinus;
    bool             mHovered = false;

    ArrowComp(glint_scrollbar* sb, bool isMinus)
      : mScrollbar(sb), mIsMinus(isMinus)
    {
      style.position = "absolute";
      element.addEventListener("mouseenter", [this](glint_event&) { mHovered = true;  setDirty(false); });
      element.addEventListener("mouseleave", [this](glint_event&) { mHovered = false; setDirty(false); });
    }

    void OnMouseDown(float, float, const glint_mouse_mod&) override
    {
      const float delta = mScrollbar->mLineHeight;
      mScrollbar->setScrollPos(mScrollbar->getScrollPos() + (mIsMinus ? -delta : delta));
    }

    void Draw(glint_canvas& g) override
    {
      // Draw with a slightly lighter colour on hover.
      if (mHovered)
      {
        const glint_color base = mScrollbar->mScrollParent
          ? mScrollbar->mScrollParent->style.scrollbarButtonColor.value
          : glint_color(255, 65, 65, 65);
        glint_style hov = style;
        hov.backgroundColor = glint_color(
          base.A,
          std::min(255, base.R + 25),
          std::min(255, base.G + 25),
          std::min(255, base.B + 25));
        DrawBackground(g, hov);
      }
      else
      {
        glint_element::Draw(g);
      }
    }

    const char* typeName() const override
    { return mIsMinus ? "scrollbar-arrow-minus" : "scrollbar-arrow-plus"; }
  };
};

// New API name — both refer to the same class.
// (glint_scrollbar is constructed internally by the scroll system; no createElement registration)

// ── glint_scrollbar::Layout ───────────────────────────────────────────────────

inline void glint_scrollbar::Layout(glint_canvas* /*g*/)
{
  if (!mScrollParent) return;

  const float sbW = mScrollParent->style.scrollbarWidth;
  const glint_rect sb  = GetPaintRECT();
  const glint_color thumbColor = mScrollParent->style.scrollbarThumbColor.value;
  const glint_color trackColor = mScrollParent->style.scrollbarTrackColor.value;
  const glint_color btnColor   = mScrollParent->style.scrollbarButtonColor.value;

  if (mThumb)    mThumb->style.backgroundColor    = thumbColor;
  if (mTrack)    mTrack->style.backgroundColor    = trackColor;
  if (mBtnMinus) mBtnMinus->style.backgroundColor = btnColor;
  if (mBtnPlus)  mBtnPlus->style.backgroundColor  = btnColor;

  // Sync button display with the buttonsVisible flag every layout pass so a
  // runtime change (scrollbar->buttonsVisible = true; setDirty(false)) takes
  // effect without requiring a full tree rebuild.
  const std::string btnDisplay = buttonsVisible ? "" : "none";
  if (mBtnMinus) mBtnMinus->style.display = btnDisplay;
  if (mBtnPlus)  mBtnPlus->style.display  = btnDisplay;

  if (isVertical())
  {
    // Clamp each button to at most half the scrollbar height so they never
    // overlap or exceed bounds when the component is very small.
    const float avail  = sb.H();
    const float btnSz  = buttonsVisible ? std::min(sbW, avail * 0.5f) : 0.f;
    const float trackT = sb.T + btnSz;
    const float trackB = std::max(trackT, sb.B - btnSz);
    if (mBtnMinus) mBtnMinus->mRect = mBtnMinus->mPaintRECT = glint_rect(sb.L, sb.T,    sb.R, trackT);
    if (mBtnPlus)  mBtnPlus->mRect  = mBtnPlus->mPaintRECT  = glint_rect(sb.L, trackB,  sb.R, sb.B);
    if (mTrack)    mTrack->mRect    = mTrack->mPaintRECT     = glint_rect(sb.L, trackT,  sb.R, trackB);
  }
  else
  {
    const float avail  = sb.W();
    const float btnSz  = buttonsVisible ? std::min(sbW, avail * 0.5f) : 0.f;
    const float trackL = sb.L + btnSz;
    const float trackR = std::max(trackL, sb.R - btnSz);
    if (mBtnMinus) mBtnMinus->mRect = mBtnMinus->mPaintRECT = glint_rect(sb.L,   sb.T, trackL,  sb.B);
    if (mBtnPlus)  mBtnPlus->mRect  = mBtnPlus->mPaintRECT  = glint_rect(trackR, sb.T, sb.R,    sb.B);
    if (mTrack)    mTrack->mRect    = mTrack->mPaintRECT     = glint_rect(trackL, sb.T, trackR,  sb.B);
  }

  // ── Position thumb inside track ─────────────────────────────────────────
  if (mTrack && mThumb)
  {
    const glint_rect tr        = mTrack->GetPaintRECT();
    const float trackSize = isVertical() ? tr.H() : tr.W();
    const float viewSize  = getViewportSize();
    const float contentSz = getContentSize();
    const float scrollMax = getScrollMax();

    if (trackSize <= 0.f || contentSz <= 0.f)
    {
      mThumb->mRect = mThumb->mPaintRECT = glint_rect{};
      return;
    }

    const float ratio     = std::min(1.f, viewSize / contentSz);
    const float thumbSz   = std::max(20.f, trackSize * ratio);
    const float thumbOff  = (scrollMax > 0.f)
                              ? getScrollPos() / scrollMax * (trackSize - thumbSz)
                              : 0.f;

    if (isVertical())
      mThumb->mRect = mThumb->mPaintRECT = glint_rect(tr.L, tr.T + thumbOff, tr.R, tr.T + thumbOff + thumbSz);
    else
      mThumb->mRect = mThumb->mPaintRECT = glint_rect(tr.L + thumbOff, tr.T, tr.L + thumbOff + thumbSz, tr.B);
  }
}

// ── glint_element::_ensureScrollbars ───────────────────────────────────────
// Defined here (after glint_scrollbar is complete) because it constructs
// glint_scrollbar instances.  Declared in glint_element.hpp.

inline void glint_element::_ensureScrollbars(bool needsX, bool needsY)
{
  if (needsY && !mScrollbarV)
  {
    auto* sb = new glint_scrollbar(glint_scrollbar::Axis::Vertical, this);
    addChild(sb);
    mScrollbarV = sb;
  }

  if (needsX && !mScrollbarH)
  {
    auto* sb = new glint_scrollbar(glint_scrollbar::Axis::Horizontal, this);
    addChild(sb);
    mScrollbarH = sb;
  }

  if (needsX && needsY && !mScrollCorner)
  {
    auto* corner = new glint_element();
    corner->style.position        = "absolute";
    corner->style.backgroundColor = style.scrollbarTrackColor;
    addChild(corner);
    mScrollCorner = corner;
    // Expose on element so devs can reach it.
    element.scrollCornerBox = mScrollCorner;
  }
}
