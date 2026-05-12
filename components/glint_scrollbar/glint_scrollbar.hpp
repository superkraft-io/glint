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
 *     glint_scrollbar_arrow [minus]       (up/left button, square)
 *     glint_scrollbar_track               (track background)
 *       glint_scrollbar_thumb             (draggable thumb, absolute inside track)
 *     glint_scrollbar_arrow [plus]        (down/right button, square)
 *
 * After this file is included, glint_element::_ensureScrollbars() is defined.
 */

#include "../../glint_element.hpp"

#include "glint_scrollbar_arrow.hpp"
#include "glint_scrollbar_thumb.hpp"
#include "glint_scrollbar_track.hpp"

#include <algorithm>

class glint_scrollbar : public glint_element
{
public:
  enum class Axis { Vertical, Horizontal };

  Axis                   mAxis;
  glint_element*         mScrollParent  = nullptr;
  float                  mLineHeight    = 40.f;
  bool                   buttonsVisible = false;

  glint_scrollbar_arrow* mBtnMinus = nullptr;
  glint_scrollbar_track* mTrack    = nullptr;
  glint_scrollbar_thumb* mThumb    = nullptr;
  glint_scrollbar_arrow* mBtnPlus  = nullptr;

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

    auto* btnM = new glint_scrollbar_arrow(true);
    btnM->style.backgroundColor = btnColor;
    btnM->style.display         = "none";
    btnM->onPress = [this]() {
      const float delta = mLineHeight;
      setScrollPos(getScrollPos() - delta);
    };
    btnM->getBaseColor = [this]() {
      return mScrollParent
        ? mScrollParent->style.scrollbarButtonColor.value
        : glint_color(255, 65, 65, 65);
    };
    addChild(btnM);
    mBtnMinus = btnM;

    auto* track = new glint_scrollbar_track();
    track->style.backgroundColor = trackColor;
    track->onPress = [this](float x, float y) { onTrackClick(x, y); };
    addChild(track);
    mTrack = track;

    auto* thumb = new glint_scrollbar_thumb();
    thumb->style.backgroundColor = thumbColor;
    thumb->style.borderRadius    = mScrollParent ? mScrollParent->style.scrollbarThumbBorderRadius : 3.f;
    thumb->onPress = [this](float x, float y) { startThumbDrag(x, y); };
    thumb->onDrag = [this](float x, float y, float, float) { updateThumbDrag(x, y); };
    thumb->onRelease = [this]() { endThumbDrag(); };
    track->addChild(thumb);
    mThumb = thumb;

    auto* btnP = new glint_scrollbar_arrow(false);
    btnP->style.backgroundColor = btnColor;
    btnP->style.display         = "none";
    btnP->onPress = [this]() {
      const float delta = mLineHeight;
      setScrollPos(getScrollPos() + delta);
    };
    btnP->getBaseColor = [this]() {
      return mScrollParent
        ? mScrollParent->style.scrollbarButtonColor.value
        : glint_color(255, 65, 65, 65);
    };
    addChild(btnP);
    mBtnPlus = btnP;
  }

  const char* typeName() const override
  {
    return (mAxis == Axis::Vertical) ? "scrollbar-v" : "scrollbar-h";
  }

  bool isVertical() const { return mAxis == Axis::Vertical; }

  void Layout(glint_canvas* g) override;

  float getScrollPos() const
  {
    if (!mScrollParent) return 0.f;
    return isVertical() ? mScrollParent->mScrollTop : mScrollParent->mScrollLeft;
  }

  float getViewportSize() const
  {
    if (!mScrollParent) return 1.f;
    const float sbW = mScrollParent->style.scrollbarWidth;
    const glint_rect pr = mScrollParent->GetPaintRECT();
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
    mScrollParent->element.scrollWidth  = mScrollParent->mScrollWidth;
    mScrollParent->element.scrollHeight = mScrollParent->mScrollHeight;
    mScrollParent->setDirty(false);
  }

  void startThumbDrag(float mouseX, float mouseY)
  {
    if (!mThumb) return;
    const glint_rect thumbRect = mThumb->GetPaintRECT();
    mDragging = true;
    mDragGrabOffset = isVertical() ? (mouseY - thumbRect.T) : (mouseX - thumbRect.L);
  }

  void updateThumbDrag(float mouseX, float mouseY)
  {
    if (!mDragging || !mTrack) return;
    const glint_rect trackRect = mTrack->GetPaintRECT();
    const float trackSize  = isVertical() ? trackRect.H() : trackRect.W();
    const float viewSize   = getViewportSize();
    const float contentSz  = getContentSize();
    const float scrollMax  = getScrollMax();
    if (trackSize <= 0.f || contentSz <= 0.f) return;

    const float ratio       = std::min(1.f, viewSize / contentSz);
    const float thumbSz     = std::max(20.f, trackSize * ratio);
    const float trackUsable = trackSize - thumbSz;
    if (trackUsable <= 0.f) return;

    const float mousePos = isVertical() ? mouseY : mouseX;
    const float trackStart = isVertical() ? trackRect.T : trackRect.L;
    const float thumbOff = std::max(0.f, std::min(mousePos - trackStart - mDragGrabOffset, trackUsable));
    setScrollPos((thumbOff / trackUsable) * scrollMax);
  }

  void endThumbDrag()
  {
    mDragging = false;
  }

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
  float mDragGrabOffset  = 0.f;
};

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

  const std::string btnDisplay = buttonsVisible ? "" : "none";
  if (mBtnMinus) mBtnMinus->style.display = btnDisplay;
  if (mBtnPlus)  mBtnPlus->style.display  = btnDisplay;

  if (isVertical())
  {
    const float avail  = sb.H();
    const float btnSz  = buttonsVisible ? std::min(sbW, avail * 0.5f) : 0.f;
    const float trackT = sb.T + btnSz;
    const float trackB = std::max(trackT, sb.B - btnSz);
    if (mBtnMinus) mBtnMinus->mRect = mBtnMinus->mPaintRECT = glint_rect(sb.L, sb.T,    sb.R, trackT);
    if (mBtnPlus)  mBtnPlus->mRect  = mBtnPlus->mPaintRECT  = glint_rect(sb.L, trackB,  sb.R, sb.B);
    if (mTrack)    mTrack->mRect    = mTrack->mPaintRECT    = glint_rect(sb.L, trackT,  sb.R, trackB);
  }
  else
  {
    const float avail  = sb.W();
    const float btnSz  = buttonsVisible ? std::min(sbW, avail * 0.5f) : 0.f;
    const float trackL = sb.L + btnSz;
    const float trackR = std::max(trackL, sb.R - btnSz);
    if (mBtnMinus) mBtnMinus->mRect = mBtnMinus->mPaintRECT = glint_rect(sb.L,   sb.T, trackL,  sb.B);
    if (mBtnPlus)  mBtnPlus->mRect  = mBtnPlus->mPaintRECT  = glint_rect(trackR, sb.T, sb.R,    sb.B);
    if (mTrack)    mTrack->mRect    = mTrack->mPaintRECT    = glint_rect(trackL, sb.T, trackR,  sb.B);
  }

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
    {
      glint_rect thumbRect(tr.L, tr.T + thumbOff, tr.R, tr.T + thumbOff + thumbSz);
      const float twProp = mScrollParent ? mScrollParent->style.scrollbarThumbWidth : -1.f;
      if (twProp > 0.f)
      {
        const float tw    = std::min(twProp, thumbRect.W());
        const float inset = (thumbRect.W() - tw) * 0.5f;
        thumbRect.L += inset;
        thumbRect.R -= inset;
      }
      mThumb->mRect = mThumb->mPaintRECT = thumbRect;
    }
    else
    {
      glint_rect thumbRect(tr.L + thumbOff, tr.T, tr.L + thumbOff + thumbSz, tr.B);
      const float thProp = mScrollParent ? mScrollParent->style.scrollbarThumbHeight : -1.f;
      if (thProp > 0.f)
      {
        const float th    = std::min(thProp, thumbRect.H());
        const float inset = (thumbRect.H() - th) * 0.5f;
        thumbRect.T += inset;
        thumbRect.B -= inset;
      }
      mThumb->mRect = mThumb->mPaintRECT = thumbRect;
    }
    mThumb->style.borderRadius = mScrollParent ? mScrollParent->style.scrollbarThumbBorderRadius : 3.f;
  }
}

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
    element.scrollCornerBox = mScrollCorner;
  }
}