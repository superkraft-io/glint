#pragma once

/**
 * glint_input.hpp
 * Single-line text-input component (HTML <input> equivalent).
 *
 * Extends glint_text_editor_base with:
 *   - Input types: "text", "number", "password", "email"
 *   - Placeholder text
 *   - Horizontal text scrolling when content overflows
 *   - onSubmit callback (Enter key)
 *   - Number type: key filtering, min/max clamping on blur
 *   - Click-to-position cursor, drag-to-select
 *   - Select-all on focus (matches browser default behaviour)
 *   - Full Skia draw path: selection highlight, caret blink
 *   - inspector DrawContentToCanvas path
 *
 * Usage via builder:
 *   add.input([](glint_input& _c) {
 *     _c.style.width  = "100%";
 *     _c.style.height = 36.f;
 *     _c.placeholder  = "Enter value…";
 *     _c.type         = "number";
 *     _c.min          = 0.f;
 *     _c.max          = 100.f;
 *     _c.onChange  = [](const std::string& v) { DBGMSG("changed: %s\n", v.c_str()); };
 *     _c.onSubmit  = [](const std::string& v) { DBGMSG("submit:  %s\n", v.c_str()); };
 *   });
 */

#include "glint_text_editor_base.hpp"
#include "../default_style.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <functional>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkFont.h"

// ─── glint_input ──────────────────────────────────────────────────────────────

class glint_input : public glint_text_editor_base
{
public:
  // ── Configuration (set in builder callback) ────────────────────────────────

  /** Input type: "text" | "number" | "password" | "email" */
  std::string type = "text";

  /** Placeholder text shown when the field is empty and unfocused. */
  std::string placeholder;

  /** Minimum value (number type only). */
  float min = std::numeric_limits<float>::lowest();

  /** Maximum value (number type only). */
  float max = std::numeric_limits<float>::max();

  /**
   * Called when the user presses Enter.
   * The current text value is passed as the argument.
   * The field does NOT automatically blur — call blur() in the handler if desired.
   */
  std::function<void(const std::string&)> onSubmit;

  /**
   * Optional key interceptor — called first in OnKeyDown, before built-in handling.
   * Return true to consume the key (suppresses default behaviour).
   */
  std::function<bool(const glint_key_press&)> onKeyDown;

  // ── Tag (for glint_document::GetNodeWithTag) ──────────────────────────────────
  int tag = glint_no_tag;

  // ── Construction ────────────────────────────────────────────────────────────

  glint_input()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
  }

  const char* typeName() const override { return "input"; }

  bool wantsPeriodicRedraw() const override { return mFocused; }

  std::chrono::steady_clock::time_point nextPeriodicRedrawTime() const override
  {
    return nextCaretToggleTime();
  }

  /**
   * Map a screen-space X coordinate to a byte index in getValue().
   * Uses the same text-base calculation as click targeting so it correctly
   * accounts for horizontal scroll and textAlign.
   * Returns 0 when clientX is to the left of the text, text.size() at the right.
   */
  int charIndexAtMouseX(float clientX) const
  {
    const std::string disp = _display();
    if (disp.empty()) return 0;
    const glint_rect content = getContent();
    const float fs      = _fontSize();
    const float baseX   = _textBaseX(disp, content, fs);
    return charIndexAtX(disp, clientX - baseX, fs);
  }

  // ── Focus ─────────────────────────────────────────────────────────────────

  void onFocusGained() override
  {
    glint_text_editor_base::onFocusGained();
    selectAll();   // match browser: focus selects all existing text
  }

  void onFocusLost() override
  {
    glint_text_editor_base::onFocusLost();
    // Clamp number value on blur.
    if (type == "number" && !mText.empty())
      _clampNumber();
    mScrollOffsetX = 0.f;   // reset scroll when focus leaves
    // Reset click-count state so the next focus session starts fresh.
    mClickCount     = 0;
    mLastClickTime  = {};
    setDirty(false);
  }

  // ── Keyboard overrides ────────────────────────────────────────────────────

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (disabled) return false;

    // External interceptor — checked before any built-in handling.
    if (onKeyDown && onKeyDown(key)) return true;

    // Enter → fire onSubmit, then blur.
    if (!key.ctrl && key.vk == glint_vk::RETURN)
    {
      if (onSubmit) onSubmit(mText);
      blur();
      return true;
    }

    // Up/Down → increment/decrement for number type.
    if (!key.ctrl && type == "number")
    {
      if (key.vk == glint_vk::UP)   { _adjustNumber( 1.f); return true; }
      if (key.vk == glint_vk::DOWN) { _adjustNumber(-1.f); return true; }
    }

    return glint_text_editor_base::OnKeyDown(key);
  }

  // ── Mouse — click to position cursor, drag to select ─────────────────────

  void OnMouseDown(float x, float y, const glint_mouse_mod& mod) override
  {
    // Consume the "just gained focus" flag regardless of click type.
    const bool justFocused = mJustGainedFocus;
    mJustGainedFocus = false;

    // Right-click: show context menu and bail out.
    if (mod.R)
    {
      _showContextMenu(x, y);
      return;
    }

    // -- Cursor position under click ------------------------------------------
    const float fontSize_pre = _fontSize();
    const std::string disp_pre = _display();
    const glint_rect content_pre  = getContent();
    const float relX_pre     = x - _textBaseX(disp_pre, content_pre, fontSize_pre);
    int pos_pre = charIndexAtX(disp_pre, relX_pre, fontSize_pre);
    if (type == "password") pos_pre = _displayToTextIdx(pos_pre);

    // -- Shift+Click: extend selection from anchor ----------------------------
    // Skip when the input just got focused this click (treat as regular click,
    // matching Chrome's behaviour: Shift+Click on an unfocused field does not
    // extend the previous selection).
    if (mod.S && !justFocused)
    {
      if (mSelStart == -1) mSelStart = mCursorPos;   // anchor at current cursor
      mSelEnd    = pos_pre;
      mCursorPos = pos_pre;
      if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
      mDragStartPos = pos_pre;
      mDragMode = DragMode::CHAR;
      resetBlink();
      setDirty(false);
      return;
    }

    // ── Click-count increment (time + distance check, matching browser rules) ─
    const auto now = std::chrono::steady_clock::now();
    const long elapsedMs = static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastClickTime).count());
#ifdef _WIN32
    const long dblMs = static_cast<long>(::GetDoubleClickTime());
#else
    const long dblMs = 500L;
#endif
    const float dist = std::abs(x - mLastClickX);
    if (elapsedMs <= dblMs && dist <= 4.f)
    {
      ++mClickCount;
      if (mClickCount >= 4) mClickCount = 1;   // Chrome resets on 4th click
    }
    else
    {
      mClickCount = 1;
    }
    mLastClickTime = now;
    mLastClickX    = x;

    // ── Cursor position under click ────────────────────────────────────────
    const float fontSize     = _fontSize();
    const std::string disp   = _display();
    const glint_rect content      = getContent();
    const float relX         = x - _textBaseX(disp, content, fontSize);
    int pos = charIndexAtX(disp, relX, fontSize);
    if (type == "password") pos = _displayToTextIdx(pos);

    // ── Apply action based on click count ─────────────────────────────────
    if (mClickCount == 1)
    {
      // Single click: place cursor, clear selection.
      mCursorPos    = pos;
      mSelStart = mSelEnd = -1;
      mDragStartPos = pos;
      mDragMode     = DragMode::CHAR;
    }
    else if (mClickCount == 2)
    {
      // Double click: select word / whitespace / punct under cursor.
      // Password type has no meaningful word boundaries — select all.
      if (type == "password")
      {
        selectAll();
        mDragMode = DragMode::ALL;
      }
      else
      {
        auto [ws, we]    = wordBoundary(mText, pos);
        mSelStart        = ws;
        mSelEnd          = we;
        mCursorPos       = we;
        mWordAnchorStart = ws;
        mWordAnchorEnd   = we;
        mDragMode        = DragMode::WORD;
      }
    }
    else
    {
      // Triple click (or reset count=1 after 3): select all.
      selectAll();
      mDragMode = DragMode::ALL;
    }

    resetBlink();
    setDirty(false);
  }

  void OnMouseDrag(float x, float /*y*/, float /*dX*/, float /*dY*/,
                   const glint_mouse_mod& /*mod*/) override
  {
    if (mDragMode == DragMode::ALL) return;   // triple-click: nothing to extend

    const float fontSize   = _fontSize();
    const std::string disp = _display();
    const glint_rect content    = getContent();
    const float relX       = x - _textBaseX(disp, content, fontSize);
    int pos = charIndexAtX(disp, relX, fontSize);
    if (type == "password") pos = _displayToTextIdx(pos);

    if (mDragMode == DragMode::CHAR)
    {
      // Character-by-character selection from the initial click position.
      if (pos != mDragStartPos)
      {
        mSelStart  = mDragStartPos;
        mSelEnd    = pos;
        mCursorPos = pos;
      }
      else
      {
        mCursorPos = pos;
        mSelStart  = mSelEnd = -1;
      }
    }
    else   // WORD
    {
      // Word-by-word selection: the initially double-clicked word is the anchor.
      // Dragging left extends selStart to the word-start of the drag position;
      // dragging right extends selEnd to the word-end of the drag position.
      if (pos <= mWordAnchorStart)
      {
        auto [ws, we] = wordBoundary(mText, pos);
        mSelStart     = mWordAnchorEnd;   // anchor right edge stays fixed
        mSelEnd       = ws;
        mCursorPos    = ws;
      }
      else if (pos >= mWordAnchorEnd)
      {
        auto [ws, we] = wordBoundary(mText, pos);
        mSelStart     = mWordAnchorStart; // anchor left edge stays fixed
        mSelEnd       = we;
        mCursorPos    = we;
      }
      else
      {
        // Back inside the anchor word: restore anchor selection.
        mSelStart  = mWordAnchorStart;
        mSelEnd    = mWordAnchorEnd;
        mCursorPos = mWordAnchorEnd;
      }
    }

    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  // ── Drawing ────────────────────────────────────────────────────────────────

  void drawContent(glint_canvas& g) override
  {
    SkCanvas* canvas = static_cast<SkCanvas*>(g.GetDrawContext());
    if (!canvas) return;

    _drawToSkia(canvas);
  }

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (!canvas) return;
    _drawToSkia(canvas);
  }

protected:
  // ── Scroll ────────────────────────────────────────────────────────────────

  float mScrollOffsetX = 0.f;   // horizontal scroll offset in pixels
  int   mDragStartPos  = 0;     // byte pos where a char-drag started

  // ── Click count & drag mode ────────────────────────────────────────────────

  enum class DragMode { CHAR, WORD, ALL };

  int        mClickCount     = 0;
  float      mLastClickX     = -9999.f;   // screen X of previous mousedown
  std::chrono::steady_clock::time_point mLastClickTime {};  // epoch = far past

  DragMode   mDragMode         = DragMode::CHAR;
  int        mWordAnchorStart  = 0;   // left boundary of the double-clicked word
  int        mWordAnchorEnd    = 0;   // right boundary

  // ── Subclass hooks ────────────────────────────────────────────────────────

  void onCursorMoved() override
  {
    ensureCursorVisible();
  }

  /**
   * Number type character filter.
   * Accepts digits, one '.', and a leading '-'.
   * All other printable characters are rejected.
   */
  bool filterChar(const std::string& s) override
  {
    if (type != "number") return true;
    if (s.size() != 1)    return true;   // multi-byte UTF-8: not a number char

    const char c = s[0];
    const bool isDigit = (c >= '0' && c <= '9');
    const bool isDot   = (c == '.' && mText.find('.') == std::string::npos);
    const bool isMinus = (c == '-' && mCursorPos == 0
                          && mText.find('-') == std::string::npos);
    return (isDigit || isDot || isMinus);
  }

private:
  // ── Helpers ────────────────────────────────────────────────────────────────

  const glint_style& _activeStyle() const
  {
    return computedStyle;
  }

  float _fontSize() const
  {
    const float fs = _activeStyle().fontSize.toFloat();
    return (fs > 0.f) ? fs : 13.f;
  }

  // -- Context menu (right-click) ------------------------------------------
  // Uses Win32 TrackPopupMenu directly (synchronous, TPM_RETURNCMD) so the
  // result is handled inline.
  // x/y are scroll-adjusted content-space; we reverse the ancestor scroll
  // chain to get window-client coords for ClientToScreen.

  void _showContextMenu(float x, float y)
  {
#ifdef _WIN32
    const bool hasSelection = (mSelStart != -1 && mSelStart != mSelEnd);

    // Reverse scroll-adjusted content-space coords back to window-client space.
    float wx = x, wy = y;
    for (glint_element* p = mParent; p; p = p->mParent)
    {
      wx -= p->mScrollLeft;
      wy -= p->mScrollTop;
    }

    HWND hwnd = mpG ? static_cast<HWND>(mpG->GetWindow())
                     : (mRoot ? mRoot->hwnd : nullptr);
    if (!hwnd) return;

    // Load localized strings from user32.dll (edit-control context menu strings).
    // These IDs are stable across all Windows versions and honour the OS language.
    auto sysStr = [](UINT id, const wchar_t* fallback) -> std::wstring {
      wchar_t buf[256] = {};
      if (int n = ::LoadStringW(::GetModuleHandleW(L"user32.dll"), id, buf, 256); n > 0)
        return std::wstring(buf, static_cast<size_t>(n));
      return fallback;
    };
    const std::wstring sCut       = sysStr(31961, L"Cu&t");
    const std::wstring sCopy      = sysStr(31962, L"&Copy");
    const std::wstring sPaste     = sysStr(31963, L"&Paste");
    const std::wstring sSelectAll = sysStr(31965, L"Select &All");

    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenuW(hMenu, MF_STRING | ((!readonly && hasSelection) ? 0u : MF_GRAYED), 1, sCut.c_str());
    ::AppendMenuW(hMenu, MF_STRING | (hasSelection ? 0u : MF_GRAYED),               2, sCopy.c_str());
    ::AppendMenuW(hMenu, MF_STRING | (!readonly ? 0u : MF_GRAYED),                  3, sPaste.c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hMenu, MF_STRING | MF_ENABLED, 5, sSelectAll.c_str());

    POINT pt = { static_cast<LONG>(wx), static_cast<LONG>(wy) };
    ::ClientToScreen(hwnd, &pt);

    const int result = static_cast<int>(::TrackPopupMenu(
      hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
      pt.x, pt.y, 0, hwnd, nullptr));
    ::DestroyMenu(hMenu);

    switch (result)
    {
      case 1: cut();                      break;
      case 2: copy();                     break;
      case 3: if (!readonly) { paste(); } break;
      case 5: selectAll();                break;
    }
    setDirty(false);
#elif defined(__APPLE__)
    {
      const bool hasSelection = (mSelStart != -1 && mSelStart != mSelEnd);
      using P = std::pair<int, std::string>;
      const std::vector<P> items = {
        {1, "Cut"},
        {2, "Copy"},
        {3, "Paste"},
        {0, "-"},
        {5, "Select All"},
      };
      std::vector<int> disabled;
      if (readonly || !hasSelection) disabled.push_back(1);  // Cut
      if (!hasSelection)             disabled.push_back(2);  // Copy
      if (readonly)                  disabled.push_back(3);  // Paste
      const int result = glint_platform::showContextMenu(0, 0, items, disabled);
      switch (result) {
        case 1: cut();                      break;
        case 2: copy();                     break;
        case 3: if (!readonly) { paste(); } break;
        case 5:
          // The NSMenu tracking run loop can cause focus to be cleared while
          // the menu is open.  Re-focus unconditionally before selecting so
          // the selection state survives until the next render frame.
          // SetFocus is a no-op if this element is already focused.
          if (mRoot) mRoot->SetFocus(this);
          selectAll();
          break;
      }
      setDirty(false);
    }
#endif
  }

  // -- Number field increment/decrement (Up/Down arrow) ----------------------

  void _adjustNumber(float delta)
  {
    float v = 0.f;
    try { v = mText.empty() ? 0.f : std::stof(mText); } catch (...) {}
    v = std::max(min, std::min(max, v + delta));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    pushUndo();
    mText      = buf;
    mCursorPos = static_cast<int>(mText.size());
    mSelStart = mSelEnd = -1;
    onTextChanged();
    if (onChange) onChange(mText);
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  /** Returns the display string (password-masked or raw). */
  std::string _display() const
  {
    if (type != "password") return mText;
    // Count codepoints for masking.
    int n = 0, pos = 0;
    while (pos < static_cast<int>(mText.size()))
    { pos = nextCodepoint(mText, pos); ++n; }
    return std::string(static_cast<size_t>(n), '*');
  }

  /** Map a byte index in the display (masked) string back to mText byte index.
   *  For non-password types this is identity.
   *  For password, display char i  → mText codepoint i → byte boundary. */
  int _displayToTextIdx(int displayByteIdx) const
  {
    if (type != "password") return displayByteIdx;
    // display string is all ASCII '*', so displayByteIdx == codepoint count.
    int cp = displayByteIdx;
    int pos = 0;
    while (cp > 0 && pos < static_cast<int>(mText.size()))
    { pos = nextCodepoint(mText, pos); --cp; }
    return pos;
  }

  /** Adjust mScrollOffsetX so the cursor byte position is visible. */
  void ensureCursorVisible()
  {
    const float fs     = _fontSize();
    const std::string disp = _display();
    const glint_rect content = getContent();
    const float viewW  = content.W();
    if (viewW <= 0.f) return;

    // Map cursor byte pos to display byte pos for password type.
    int dispCursorPos = mCursorPos;
    if (type == "password")
    {
      // Count codepoints up to mCursorPos.
      int cp = 0, p = 0;
      while (p < mCursorPos && p < static_cast<int>(mText.size()))
      { p = nextCodepoint(mText, p); ++cp; }
      dispCursorPos = cp;   // each CP → one '*' byte
    }

    const float cx = charXOffset(disp, dispCursorPos, fs);
    const float margin = 4.f;

    if (cx - mScrollOffsetX > viewW - margin)
      mScrollOffsetX = cx - viewW + margin;
    if (cx - mScrollOffsetX < margin)
      mScrollOffsetX = cx - margin;

    mScrollOffsetX = std::max(0.f, mScrollOffsetX);
  }

  /** Clamp numeric value to [min, max] and update mText. */
  void _clampNumber()
  {
    if (mText.empty() || mText == "-" || mText == ".") return;
    try
    {
      const float v = std::stof(mText);
      const float clamped = std::max(min, std::min(max, v));
      if (clamped != v)
      {
        // Format: strip trailing zeros.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", clamped);
        mText = buf;
        mCursorPos = static_cast<int>(mText.size());
        if (onChange) onChange(mText);
      }
    }
    catch (...) {}   // non-numeric string: leave as-is
  }

  void _drawToSkia(SkCanvas* canvas)
  {
    const float fs      = _fontSize();
    const glint_rect content = getContent();
    const std::string disp = _display();
    const glint_style& active = _activeStyle();

    // Map cursor/selection positions to display positions for password type.
    int dispCursor = mCursorPos;
    int dispSelSt  = mSelStart;
    int dispSelEnd = mSelEnd;
    if (type == "password")
    {
      dispCursor = _cpCount(mText, mCursorPos);
      dispSelSt  = (mSelStart >= 0) ? _cpCount(mText, mSelStart) : -1;
      dispSelEnd = (mSelEnd   >= 0) ? _cpCount(mText, mSelEnd)   : -1;
    }

    // ── Clip to content area ────────────────────────────────────────────────
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(content.L, content.T, content.R, content.B));

    const float textY = content.T + content.H() * 0.5f + fs * 0.35f;

    SkFont font = skFont(fs);

    // ── Compute text base X according to style.textAlign ────────────────────
    // Shared with OnMouseDown/OnMouseDrag via _textBaseX().
    const bool isCentered = (active.textAlign == EAlign::Center);
    const bool isRight    = (active.textAlign == EAlign::Far);
    const float baseX = _textBaseX(disp, content, fs);

    // ── Selection highlight ─────────────────────────────────────────────────
    if (dispSelSt != -1 && dispSelSt != dispSelEnd)
    {
      const int lo = std::min(dispSelSt, dispSelEnd);
      const int hi = std::max(dispSelSt, dispSelEnd);
      const float x0 = baseX + charXOffset(disp, lo, fs);
      const float x1 = baseX + charXOffset(disp, hi, fs);
      SkPaint selP;
      selP.setColor(SkColorSetARGB(180, 93, 177, 255));
      canvas->drawRect(
        SkRect::MakeLTRB(x0, textY - fs * 0.85f, x1, textY + fs * 0.3f), selP);
    }

    // ── Placeholder ─────────────────────────────────────────────────────────
    if (disp.empty() && !placeholder.empty() && !mFocused)
    {
      SkPaint ph;
      ph.setColor(SkColorSetARGB(160, 140, 140, 140));
      ph.setAntiAlias(true);
      const float phW = font.measureText(placeholder.c_str(), placeholder.size(), SkTextEncoding::kUTF8);
      const float phX = isCentered ? content.L + (content.W() - phW) * 0.5f
                      : isRight    ? content.R - phW
                      : content.L - mScrollOffsetX;
      canvas->drawString(placeholder.c_str(), phX, textY, font, ph);
    }
    else if (!disp.empty())
    {
      // ── Text ───────────────────────────────────────────────────────────────
      const glint_color& col = active.color.value;
      SkPaint tp;
      tp.setColor(SkColorSetARGB(col.A, col.R, col.G, col.B));
      tp.setAntiAlias(true);
      canvas->drawString(disp.c_str(), baseX, textY, font, tp);
    }

    // ── Caret ───────────────────────────────────────────────────────────────
    if (mFocused && caretVisible() && (dispSelSt == -1 || dispSelSt == dispSelEnd))
    {
      const float cx       = baseX + charXOffset(disp, dispCursor, fs);
      const float caretTop = textY - fs * 0.9f;   // near cap-height
      const float caretBot = textY + fs * 0.3f;    // includes descenders
      SkPaint cp;
      cp.setColor(SkColorSetARGB(255, 220, 220, 220));
      cp.setStrokeWidth(1.5f);
      cp.setAntiAlias(true);
      canvas->drawLine(cx, caretTop, cx, caretBot, cp);
    }

    canvas->restore();
  }

  /** Returns the X pixel origin of the text (left edge of glyph 0).
   *  Matches the baseX used by _drawToSkia() so click/drag hit-testing
   *  lands in the same coordinate space as the rendered text for all
   *  textAlign values (Near/Center/Far).
   *
   *  Chrome parity: text-align only takes effect when text FITS inside the
   *  content box.  Once text overflows (textW > viewW) the input switches to
   *  left-aligned scroll behaviour regardless of text-align, exactly as
   *  Chrome does.  mScrollOffsetX (kept up-to-date by ensureCursorVisible)
   *  drives the scroll in both centered and right-aligned overflow cases. */
  float _textBaseX(const std::string& disp, const glint_rect& content, float fs) const
  {
    const glint_style& active = _activeStyle();
    const bool isCentered = (active.textAlign == EAlign::Center);
    const bool isRight    = (active.textAlign == EAlign::Far);
    if (isCentered || isRight)
    {
      SkFont font = skFont(fs);
      const float textW = disp.empty() ? 0.f
        : font.measureText(disp.c_str(), disp.size(), SkTextEncoding::kUTF8);
      const float viewW = content.W();
      // When text fits, apply alignment.  When text overflows, fall through
      // to the left-aligned scroll path so ensureCursorVisible works.
      if (textW <= viewW)
        return isCentered ? content.L + (viewW - textW) * 0.5f
                          : content.R - textW;
    }
    return content.L - mScrollOffsetX;   // left-aligned or overflow scroll
  }

  /** Count Unicode codepoints in `s` up to byte position `byteEnd`. */
  static int _cpCount(const std::string& s, int byteEnd)
  {
    int cp = 0, pos = 0;
    while (pos < byteEnd && pos < static_cast<int>(s.size()))
    { pos = nextCodepoint(s, pos); ++cp; }
    return cp;
  }
};

// New API name — both refer to the same class.
namespace { struct _glint_input_reg { _glint_input_reg() { glint_element::registerElement("input", []{ return new glint_input(); }); } } _glint_input_reg_; }
