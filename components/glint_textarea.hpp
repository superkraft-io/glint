#pragma once

/**
 * glint_textarea.hpp
 * Multi-line text area component for glint.
 *
 * Extends glint_text_editor_base with:
 *   - Multi-line rendering (text split by \n)
 *   - Enter inserts \n (Shift+Enter also inserts \n)
 *   - Up/Down arrow navigates between lines
 *   - Vertical scroll when content overflows
 *   - Placeholder text shown when empty and unfocused
 *   - Resize handle (visual only)
 *
 * Usage:
 *   add.fromClass<glint_textarea>([](glint_textarea& ta) {
 *     ta.style.width   = "100%";
 *     ta.style.height  = 100.f;
 *     ta.placeholder   = "Enter text here\xe2\x80\xa6";
 *     ta.onChange      = [](const std::string& v) { ... };
 *   });
 */

#include "glint_text_editor_base.hpp"
#include "glint_scrollbar/glint_scrollbar.hpp"
#include "../default_style.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <functional>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"

class glint_textarea;   // forward declaration for resize handle

// ── Resize handle element ─────────────────────────────────────────────────
// A small square that draws diagonal grip lines (like a browser resize corner).
struct glint_textarea_resize_handle : public glint_element
{
  glint_textarea_resize_handle()
  {
    style.cursor = "se-resize";
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
  }

  const char* typeName() const override { return "textarea-resize-handle"; }

  glint_textarea* mOwner     = nullptr;
  float mDragStartX          = 0.f;
  float mDragStartY          = 0.f;
  float mStartW              = 0.f;
  float mStartH              = 0.f;

  void OnMouseDown(float x, float y, const glint_mouse_mod&) override;
  void OnMouseDrag(float x, float y, float, float, const glint_mouse_mod&) override;

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    const glint_rect r = GetPaintRECT();
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.f);
    p.setStrokeCap(SkPaint::kRound_Cap);

    const float sz     = r.W();          // square handle
    const float margin = sz * 0.20f;     // inset from edges
    const float spacing = sz * 0.28f;   // gap between lines

    // Draw 3 parallel diagonal lines from bottom-left-ish to top-right-ish
    // (NW-SE direction, matching the browser resize corner convention).
    for (int i = 0; i < 3; ++i)
    {
      const float offset = (i + 1) * spacing;

      // Each line goes from (r.R - margin, r.T + margin + i*spacing)
      // to (r.R - margin - offset + spacing, r.B - margin)
      // Simplified: short diagonal segment, staggered
      const float x1 = r.L + margin + i * spacing;
      const float y1 = r.B - margin;
      const float x2 = r.R - margin;
      const float y2 = r.T + margin + i * spacing;

      // Dark shadow line
      p.setColor(SkColorSetARGB(120, 0, 0, 0));
      p.setStrokeWidth(1.5f);
      canvas->drawLine(x1 + 0.5f, y1 + 0.5f, x2 + 0.5f, y2 + 0.5f, p);

      // Light highlight line
      p.setColor(SkColorSetARGB(60, 255, 255, 255));
      p.setStrokeWidth(1.0f);
      canvas->drawLine(x1, y1, x2, y2, p);
    }
  }
};

class glint_textarea : public glint_text_editor_base
{
public:
  // ── Public fields ──────────────────────────────────────────────────────────
  std::string placeholder;
  std::string inputmode;
  std::string enterkeyhint;
  int         maxlength = -1;
  int         minlength = -1;
  float       lineHeight = 1.5f;   // multiplier applied to fontSize

  // ── Construction ──────────────────────────────────────────────────────────
  glint_textarea()
  {
    style.overflowY    = "auto";
    style.cursor       = "text";
    style.whiteSpace   = "pre-wrap";
    mAcceptsFocus      = true;

    // Create the vertical scrollbar immediately so the framework's hit-test
    // and draw paths pick it up.  _ensureScrollbars() is private, so we do
    // this manually here (same three lines it would execute).
    auto* sb = new glint_scrollbar(glint_scrollbar::Axis::Vertical, this);
    addChild(sb);
    mScrollbarV = sb;

    // Resize handle — absolute div pinned to the bottom-right corner.
    // Registered as mScrollCorner so the framework draws it in screen space
    // AFTER canvas->restore() (bypassing the overflow clip + scroll translate).
    auto* rh = new glint_textarea_resize_handle();
    rh->mOwner       = this;
    rh->style.width  = style.scrollbarWidth;
    rh->style.height = style.scrollbarWidth;
    addChild(rh);
    mResizeHandle = rh;
    mScrollCorner = rh;   // drawn in screen space, not clipped
  }

  const char* typeName() const override { return "textarea"; }

  std::string getAttribute(const std::string& name, bool& found) const override
  {
    if (name == "inputmode") { found = true; return inputmode; }
    if (name == "enterkeyhint") { found = true; return enterkeyhint; }
    if (name == "maxlength") { found = true; return maxlength >= 0 ? std::to_string(maxlength) : std::string(); }
    if (name == "minlength") { found = true; return minlength >= 0 ? std::to_string(minlength) : std::string(); }
    return glint_text_editor_base::getAttribute(name, found);
  }

  void onFocusGained() override
  {
    if (disabled)
      return;
    glint_text_editor_base::onFocusGained();
  }

protected:
  int maxTextLength() const override { return maxlength; }
  int minTextLength() const override { return minlength; }

public:

  // ── Layout: report real content height so the scrollbar is shown/hidden ──
  void Layout(glint_canvas* g) override
  {
    mAcceptsFocus = !disabled;
    if (disabled)
    {
      if (!mDisabledOpacityApplied)
      {
        mEnabledOpacity = style.opacity;
        mDisabledOpacityApplied = true;
      }
      style.opacity = mEnabledOpacity * 0.5f;
      if (mRoot && mRoot->getFocusedNode() == this)
        mRoot->SetFocus(nullptr);
    }
    else if (mDisabledOpacityApplied)
    {
      style.opacity = mEnabledOpacity;
      mDisabledOpacityApplied = false;
    }

    // The base Layout() measures no children → sets mScrollHeight=0 → clamps
    // mScrollTop to 0 inside _clampScroll().  Save the scroll position first
    // and restore it after we've set the real content height.
    const float savedScrollTop = mScrollTop;

    glint_text_editor_base::Layout(g);

    // Set real content height from line count.
    // The framework's viewport is GetPaintRECT().H() (full paint rect), so
    // mScrollHeight must be in the same coordinate space: add padding so the
    // thumb ratio and wheel maxY calculation are both correct.
    const float fs    = _fontSize();
    const float lh    = fs * lineHeight;
    const int   nLines = static_cast<int>(_splitLines().size());
    const float pT    = static_cast<float>(computedStyle.paddingTop);
    const float pB    = static_cast<float>(computedStyle.paddingBottom);
    mScrollHeight      = nLines * lh + pT + pB;
    element.scrollHeight = mScrollHeight;

    const glint_rect pr      = GetPaintRECT();
    const glint_rect content  = getContent();
    const float      sbW      = computedStyle.scrollbarWidth;
    // Viewport = full paint rect height (matches getViewportSize() and wheel handler).
    const float      viewH    = pr.H();
    const bool       needsY   = mScrollHeight > viewH;

    // Restore and clamp scroll position with the correct content height.
    mScrollTop = std::max(0.f, std::min(savedScrollTop,
                   std::max(0.f, mScrollHeight - viewH)));

    if (mScrollbarV)
    {
      const float handleSz = computedStyle.scrollbarWidth;
      if (needsY)
      {
        // Scrollbar stops above the resize handle.
        mScrollbarV->mRect = mScrollbarV->mPaintRECT =
          glint_rect(pr.R - sbW, pr.T, pr.R, pr.B - handleSz);
        mScrollbarV->style.display = "";
      }
      else
      {
        mScrollbarV->style.display = "none";
      }
      mScrollbarV->Layout(g);
    }

    // Position the resize handle at the border-box bottom-right.
    if (mResizeHandle)
    {
      const float handleSz = computedStyle.scrollbarWidth;
      mResizeHandle->mRect = mResizeHandle->mPaintRECT =
        glint_rect(pr.R - handleSz, pr.B - handleSz, pr.R, pr.B);
      mResizeHandle->style.width   = handleSz;
      mResizeHandle->style.height  = handleSz;
      // _positionScrollbars (called by the base Layout) sets mScrollCorner->style.display="none"
      // when there is no horizontal scrollbar. Restore it before calling Layout so
      // computedStyle.display is recomputed as visible.
      mResizeHandle->style.display = "";
      mResizeHandle->Layout(g);
    }
  }

  bool wantsPeriodicRedraw() const override { return mFocused; }
  std::chrono::steady_clock::time_point nextPeriodicRedrawTime() const override
  {
    return nextCaretToggleTime();
  }

  // ── Keyboard ──────────────────────────────────────────────────────────────
  bool OnKeyDown(const glint_key_press& key) override
  {
    if (disabled) return false;

    // Enter inserts a newline (both plain and Shift+Enter)
    if (!key.ctrl && key.vk == glint_vk::RETURN)
    {
      if (!readonly)
      {
        insertText("\n");
        _ensureCursorVisible();
      }
      return true;
    }

    // Up/Down arrow: navigate between lines
    if (!key.ctrl)
    {
      if (key.vk == glint_vk::UP)
      {
        _moveCursorVertical(-1, key.shift);
        _ensureCursorVisible();
        return true;
      }
      if (key.vk == glint_vk::DOWN)
      {
        _moveCursorVertical(+1, key.shift);
        _ensureCursorVisible();
        return true;
      }
    }

    // Ctrl+Home / Ctrl+End
    if (key.ctrl)
    {
      if (key.vk == glint_vk::HOME) { moveToStart(key.shift); _ensureCursorVisible(); return true; }
      if (key.vk == glint_vk::END)  { moveToEnd  (key.shift); _ensureCursorVisible(); return true; }
    }

    bool consumed = glint_text_editor_base::OnKeyDown(key);
    if (consumed)
      _ensureCursorVisible();
    return consumed;
  }

  // ── Mouse ─────────────────────────────────────────────────────────────────
  void OnMouseDown(float x, float y, const glint_mouse_mod& mod) override
  {
    if (mod.R) return;
    const int bytePos = _hitTestPos(x, y);

    if (!mod.S)
    {
      mCursorPos    = bytePos;
      mSelStart     = mSelEnd = -1;
      mDragStartPos = bytePos;
    }
    else
    {
      if (mSelStart == -1) mSelStart = mCursorPos;
      mSelEnd       = bytePos;
      mCursorPos    = bytePos;
      mDragStartPos = bytePos;
    }

    resetBlink();
    setDirty(false);
  }

  void OnMouseDrag(float x, float y, float /*dX*/, float /*dY*/,
                   const glint_mouse_mod& /*mod*/) override
  {
    const int bytePos = _hitTestPos(x, y);
    if (bytePos == mDragStartPos)
    {
      mSelStart = mSelEnd = -1;
      mCursorPos = bytePos;
    }
    else
    {
      mSelStart  = mDragStartPos;
      mSelEnd    = bytePos;
      mCursorPos = bytePos;
    }
    resetBlink();
    _ensureCursorVisible();
    setDirty(false);
  }

  // ── HitTest: route resize-handle clicks before the content-clip guard ────
  // The framework's HitTest returns `this` when a click is in the scrollbar X
  // zone (right strip) but below the scrollbar rect.  That routes to
  // OnMouseDown → cursor placement → drag-selection + ensureCursorVisible →
  // scrollbar thumb movement.  We intercept that zone here.
  glint_element* HitTest(float lx, float ly) override
  {
    if (mResizeHandle)
    {
      const glint_rect& rhr = mResizeHandle->GetPaintRECT();
      if (rhr.Contains(lx, ly)) return mResizeHandle;
    }
    return glint_text_editor_base::HitTest(lx, ly);
  }

  // ── Drawing ───────────────────────────────────────────────────────────────
  void drawContent(glint_canvas& /*g*/) override {}   // use Skia path only

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (!canvas) return;

    const float fs = _fontSize();
    const float lh = fs * lineHeight;
    const glint_rect content = getContent();
    // Clip right edge: leave room for scrollbar (and resize handle which sits
    // at the same x as the scrollbar).
    const float sbW      = computedStyle.scrollbarWidth;
    const bool  hasSbarV = mScrollbarV && mScrollbarV->style.display != "none";
    const float contentR = hasSbarV ? content.R - sbW : content.R;

    // The framework has already applied canvas->translate(0, -mScrollTop).
    // Our clip must compensate: clip at [content.T + mScrollTop] in canvas
    // coords so it corresponds to screen rect [content.T, content.B].
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(content.L,
                                       content.T + mScrollTop,
                                       contentR,
                                       content.B + mScrollTop));

    SkFont font = skFont(fs);
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const float ascent = -metrics.fAscent;   // positive value

    const auto lines = _splitLines();
    const bool empty = mText.empty();

    // ── Placeholder ──────────────────────────────────────────────────────────
    if (empty && !placeholder.empty() && !mFocused)
    {
      SkPaint ph;
      ph.setColor(SkColorSetARGB(160, 140, 140, 140));
      ph.setAntiAlias(true);
      // Draw at canvas y = content.T + ascent; canvas translate makes it appear
      // at screen y = content.T + ascent - mScrollTop (scrolled with content).
      canvas->drawString(placeholder.c_str(),
                         content.L,
                         content.T + ascent,
                         font, ph);
      canvas->restore();
      return;
    }

    // Cursor line/col
    int cursorLine = 0, cursorCol = 0;
    _cursorToLineCol(cursorLine, cursorCol);

    const glint_color& col = computedStyle.color.value;
    SkPaint tp;
    tp.setColor(SkColorSetARGB(col.A, col.R, col.G, col.B));
    tp.setAntiAlias(true);

    const int lo = mSelStart != -1 ? std::min(mSelStart, mSelEnd) : -1;
    const int hi = mSelStart != -1 ? std::max(mSelStart, mSelEnd) : -1;
    int absOffset = 0;

    for (int li = 0; li < static_cast<int>(lines.size()); ++li)
    {
      // Line position in canvas coords (framework translate handles scroll).
      const float lineTop = content.T + li * lh;
      const float lineBot = lineTop + lh;
      const float textY   = lineTop + ascent + (lh - fs) * 0.5f;

      // Cull: compare screen positions against visible screen area.
      const float screenTop = lineTop - mScrollTop;
      const float screenBot = lineBot - mScrollTop;
      if (screenBot < content.T || screenTop > content.B)
      {
        absOffset += static_cast<int>(lines[li].size()) + 1;
        continue;
      }

      const std::string& line = lines[li];
      const int lineStart = absOffset;
      const int lineEnd   = absOffset + static_cast<int>(line.size());

      // ── Selection highlight ───────────────────────────────────────────────
      if (lo != -1 && hi != -1 && lo != hi)
      {
        const int selL = std::max(lo, lineStart);
        const int selR = std::min(hi, lineEnd);
        if (selL < selR)
        {
          const float x0 = content.L + charXOffset(line, selL - lineStart, fs);
          const float x1 = content.L + charXOffset(line, selR - lineStart, fs);
          SkPaint selP;
          selP.setColor(SkColorSetARGB(180, 93, 177, 255));
          canvas->drawRect(
            SkRect::MakeLTRB(x0, lineTop + (lh - fs) * 0.5f,
                             x1, lineTop + (lh - fs) * 0.5f + fs * 1.15f),
            selP);
        }
        // End-of-line selection (newline char)
        else if (lo <= lineEnd && hi > lineEnd && li < static_cast<int>(lines.size()) - 1)
        {
          const float x0 = content.L + charXOffset(line, static_cast<int>(line.size()), fs);
          const float x1 = x0 + fs * 0.4f;
          SkPaint selP;
          selP.setColor(SkColorSetARGB(180, 93, 177, 255));
          canvas->drawRect(
            SkRect::MakeLTRB(x0, lineTop + (lh - fs) * 0.5f,
                             x1, lineTop + (lh - fs) * 0.5f + fs * 1.15f),
            selP);
        }
      }

      // ── Text ──────────────────────────────────────────────────────────────
      if (!line.empty())
        canvas->drawString(line.c_str(), content.L, textY, font, tp);

      // ── Caret ─────────────────────────────────────────────────────────────
      if (mFocused && !readonly && !disabled && caretVisible() && (mSelStart == -1 || mSelStart == mSelEnd))
      {
        if (li == cursorLine)
        {
          const float cx = content.L + charXOffset(line, cursorCol, fs);
          SkPaint cp;
          cp.setColor(SkColorSetARGB(255, 220, 220, 220));
          cp.setStrokeWidth(1.5f);
          cp.setAntiAlias(true);
          canvas->drawLine(cx, lineTop + (lh - fs) * 0.5f,
                           cx, lineTop + (lh - fs) * 0.5f + fs * 1.15f, cp);
        }
      }

      absOffset += static_cast<int>(line.size()) + 1; // +1 for \n
    }

    canvas->restore();
  }

private:
  int             mDragStartPos  = 0;
  glint_element*  mResizeHandle  = nullptr;
  float           mEnabledOpacity = 1.f;
  bool            mDisabledOpacityApplied = false;

  float _fontSize() const
  {
    const float fs = computedStyle.fontSize.toFloat();
    return (fs > 0.f) ? fs : 13.f;
  }

  // Split mText by '\n' into individual lines.
  // Empty string → one empty line.
  std::vector<std::string> _splitLines() const
  {
    std::vector<std::string> result;
    std::string cur;
    for (char c : mText)
    {
      if (c == '\n') { result.push_back(cur); cur.clear(); }
      else           cur += c;
    }
    result.push_back(cur);   // last (or only) line
    return result;
  }

  // Convert mCursorPos (byte offset in mText) → (lineIdx, colByte).
  void _cursorToLineCol(int& outLine, int& outCol) const
  {
    outLine = 0;
    outCol  = 0;
    int remaining = mCursorPos;
    for (char c : mText)
    {
      if (remaining <= 0) break;
      if (c == '\n') { ++outLine; outCol = 0; }
      else           { ++outCol; }
      --remaining;
    }
  }

  // Move cursor up (-1) or down (+1) one line, preserving X pixel column.
  void _moveCursorVertical(int dir, bool extend)
  {
    const float fs = _fontSize();
    const auto lines = _splitLines();
    int curLine = 0, curCol = 0;
    _cursorToLineCol(curLine, curCol);

    const int targetLine = curLine + dir;
    if (targetLine < 0 || targetLine >= static_cast<int>(lines.size()))
    {
      // At top/bottom: move to start/end
      if (extend)
      {
        if (mSelStart == -1) mSelStart = mCursorPos;
        mCursorPos = (dir < 0) ? 0 : static_cast<int>(mText.size());
        mSelEnd = mCursorPos;
      }
      else
      {
        mCursorPos = (dir < 0) ? 0 : static_cast<int>(mText.size());
        mSelStart = mSelEnd = -1;
      }
      return;
    }

    // Desired X pixel position in current line
    const float desiredX = charXOffset(lines[curLine], curCol, fs);
    // Find closest byte in target line
    const int newCol = charIndexAtX(lines[targetLine], desiredX, fs);

    // Convert (targetLine, newCol) → absolute byte
    int bytePos = 0;
    for (int i = 0; i < targetLine; ++i)
      bytePos += static_cast<int>(lines[i].size()) + 1;
    bytePos += newCol;

    if (extend)
    {
      if (mSelStart == -1) mSelStart = mCursorPos;
      mCursorPos = bytePos;
      mSelEnd    = bytePos;
    }
    else
    {
      mCursorPos = bytePos;
      mSelStart  = mSelEnd = -1;
    }

    resetBlink();
    setDirty(false);
  }

  // Hit-test (x, y) in screen coords → absolute byte offset in mText.
  int _hitTestPos(float x, float y) const
  {
    const float fs = _fontSize();
    const float lh = fs * lineHeight;
    const glint_rect content = getContent();
    const float pT   = static_cast<float>(computedStyle.paddingTop);
    // mScrollTop is in paint-rect space (includes padding), so subtract pT
    // to get offset relative to the first line of text.
    const float relY = y - content.T + mScrollTop - pT;
    const int lineIdx = std::max(0, static_cast<int>(relY / lh));
    const auto lines = _splitLines();
    const int clampedLine = std::min(lineIdx, static_cast<int>(lines.size()) - 1);
    if (clampedLine < 0) return 0;
    const float relX = x - content.L;
    const int colByte = charIndexAtX(lines[clampedLine], relX, fs);
    int bytePos = 0;
    for (int i = 0; i < clampedLine; ++i)
      bytePos += static_cast<int>(lines[i].size()) + 1;
    bytePos += colByte;
    return std::min(bytePos, static_cast<int>(mText.size()));
  }

  // Scroll so the caret is visible.
  void _ensureCursorVisible()
  {
    const float fs = _fontSize();
    const float lh = fs * lineHeight;
    const glint_rect pr = GetPaintRECT();
    // Use same viewport as getViewportSize() / wheel handler.
    const float viewH = pr.H();
    if (viewH <= 0.f) return;

    const float pT = static_cast<float>(computedStyle.paddingTop);

    int curLine = 0, curCol = 0;
    _cursorToLineCol(curLine, curCol);

    // Caret position in mScrollHeight space (padded).
    const float caretTop = pT + curLine * lh;
    const float caretBot = caretTop + lh;

    if (caretBot - mScrollTop > viewH)
      mScrollTop = caretBot - viewH + 4.f;
    if (caretTop - mScrollTop < 0.f)
      mScrollTop = caretTop;

    mScrollTop = std::max(0.f, mScrollTop);
    _refreshRootHoverFromPointer();
    setDirty(false);
  }
};

// ── glint_textarea_resize_handle — mouse handlers (defined after glint_textarea) ──

inline void glint_textarea_resize_handle::OnMouseDown(float x, float y,
                                                        const glint_mouse_mod&)
{
  mDragStartX = x;
  mDragStartY = y;
  if (mOwner)
  {
    const glint_rect pr = mOwner->GetPaintRECT();
    mStartW = pr.W();
    mStartH = pr.H();
  }
}

inline void glint_textarea_resize_handle::OnMouseDrag(float x, float y,
                                                       float, float,
                                                       const glint_mouse_mod&)
{
  if (!mOwner) return;
  const float minW = 80.f;
  const float minH = 40.f;
  const float newW = std::max(minW, mStartW + (x - mDragStartX));
  const float newH = std::max(minH, mStartH + (y - mDragStartY));
  mOwner->style.width  = newW;
  mOwner->style.height = newH;
  mOwner->setDirty(false);
}
