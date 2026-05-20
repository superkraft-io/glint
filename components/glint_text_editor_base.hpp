#pragma once

/**
 * glint_text_editor_base.hpp
 * Shared text-editing kernel for glint.
 *
 * Derives from glint_element. Implements:
 *   - UTF-8 aware text buffer with cursor (byte-index based)
 *   - Selection range (selStart / selEnd, -1 = no selection)
 *   - Keyboard dispatch: arrows, home/end, backspace/delete, ctrl+A/C/V/X/Z
 *   - Cross-platform clipboard (copy / paste / cut)
 *   - Caret blink via std::chrono
 *   - Skia text measurement helpers (charXOffset, charIndexAtX)
 *   - Focus / blur lifecycle (wired to glint_document via mAcceptsFocus = true)
 *
 * Subclasses:
 *   glint_input    — single-line, type filtering, horizontal scroll, onSubmit
 *   sk_ui_textarea — multi-line, word-wrap, vertical scroll  (TBD)
 *
 * Usage:
 *   class MyInput : public glint_text_editor_base {
 *     void drawContent(glint_canvas& g) override { ... }
 *   };
 */

#include "../glint_element.hpp"
#if defined(__APPLE__)
#include "../platform/glint_platform.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkCanvas.h"

// ─── VK constants (mirrored so this header stays host-independent) ───────────
// Match the Windows VK_* values used by the standalone message loop.
namespace glint_vk {
  static constexpr int LEFT    = 0x25;
  static constexpr int RIGHT   = 0x27;
  static constexpr int UP      = 0x26;
  static constexpr int DOWN    = 0x28;
  static constexpr int HOME    = 0x24;
  static constexpr int END     = 0x23;
  static constexpr int BACK    = 0x08;   // Backspace
  static constexpr int DEL     = 0x2E;   // Delete
  static constexpr int RETURN  = 0x0D;
  static constexpr int ESCAPE  = 0x1B;
  static constexpr int TAB     = 0x09;
  static constexpr int KEY_A   = 0x41;
  static constexpr int KEY_C   = 0x43;
  static constexpr int KEY_V   = 0x56;
  static constexpr int KEY_X   = 0x58;
  static constexpr int KEY_Y   = 0x59;
  static constexpr int KEY_Z   = 0x5A;
}

// ─── glint_text_editor_base ────────────────────────────────────────────────────

class glint_text_editor_base : public glint_element
{
public:
  // ── Public data ────────────────────────────────────────────────────────────

  /** Callback fired on every content change.  Receives the current text. */
  std::function<void(const std::string&)> onChange;

  /** Called when this node gains keyboard focus. */
  std::function<void()> onFocus;

  /** Called when this node loses keyboard focus. */
  std::function<void()> onBlur;

  /** When true, keyboard input is ignored but selection/copy still work. */
  bool readonly = false;

  /** When true, the field is entirely non-interactive. */
  bool disabled = false;

  // ── Value API ──────────────────────────────────────────────────────────────

  const std::string& getValue() const { return mText; }

  /** Returns the current text-cursor byte index (always on a codepoint boundary). */
  int getCursorPos() const { return mCursorPos; }

  bool hasSelection() const
  {
    return mSelStart != -1 && mSelStart != mSelEnd;
  }

  bool canCopySelection() const
  {
    return hasSelection();
  }

  bool canCutSelection() const
  {
    return !readonly && hasSelection() && canDeleteCodepoints(selectedCodepointLength());
  }

  bool canPasteFromClipboard() const
  {
    return !readonly && !getClipboard().empty();
  }

  bool canSelectAllText() const
  {
    return !mText.empty() && (!hasSelection() || std::min(mSelStart, mSelEnd) != 0
                              || std::max(mSelStart, mSelEnd) != static_cast<int>(mText.size()));
  }

  void copySelection()
  {
    copy();
  }

  void cutSelection()
  {
    if (!readonly) cut();
  }

  void pasteFromClipboard()
  {
    if (!readonly) paste();
  }

  void selectAllText()
  {
    selectAll();
  }

  void setValue(const std::string& s)
  {
    setValueInternal(s, true);
  }

  void setValueWithoutRedraw(const std::string& s)
  {
    setValueInternal(s, false);
  }

private:
  void setValueInternal(const std::string& s, bool requestRedraw)
  {
    const std::string clamped = clampTextToMaxLength(s);
    if (mText == clamped
        && mCursorPos == static_cast<int>(mText.size())
        && mSelStart == -1
        && mSelEnd == -1)
      return;

    mText      = clamped;
    mCursorPos = static_cast<int>(mText.size());
    mSelStart = mSelEnd = -1;
    onTextChanged();
    if (requestRedraw) setDirty(false);
  }

  public:

  std::string clampTextToMaxLength(const std::string& s) const
  {
    const int maxLength = maxTextLength();
    if (maxLength < 0) return s;
    return truncateToCodepoints(s, maxLength);
  }

  /** Maximum number of Unicode codepoints allowed, or -1 when unlimited. */
  virtual int maxTextLength() const { return -1; }

  /** Minimum number of Unicode codepoints required, or -1 when unlimited. */
  virtual int minTextLength() const { return -1; }

  bool satisfiesMinTextLength() const
  {
    const int minLength = minTextLength();
    if (minLength < 0) return true;
    if (mText.empty()) return true;
    return codepointCount(mText) >= minLength;
  }

  // ── Construction ────────────────────────────────────────────────────────────

  glint_text_editor_base()
  {
    mAcceptsFocus = true;
  }

  // Text editors handle Ctrl+A internally (glint_text_editor_base::OnKeyDown).
  // Returning true here prevents glint_document's global select-all handler from
  // firing when this component has keyboard focus — matching Chrome's rule
  // that Ctrl+A selects the input's own text, not the whole page.
  bool consumesCtrlA() const override { return true; }

  // ── Focus events (called by glint_document::SetFocus) ─────────────────────────

  void onFocusGained() override
  {
    if (disabled)
    {
      mFocused = false;
      mSelStart = mSelEnd = -1;
      setDirty(false);
      return;
    }

    mFocused        = true;
    mJustGainedFocus = true;
    mBlinkStart = std::chrono::steady_clock::now();
    if (onFocus) onFocus();
    setDirty(false);
  }

  void onFocusLost() override
  {
    mFocused  = false;
    mSelStart = mSelEnd = -1;   // clear selection when focus leaves
    if (onBlur) onBlur();
    setDirty(false);
  }

  // ── Keyboard (routed from glint_document via OnKeyDown virtual) ───────────────

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (disabled) return false;

    // ── Ctrl shortcuts ───────────────────────────────────────────────────────
    if (key.ctrl)
    {
      switch (key.vk)
      {
        case glint_vk::KEY_A: selectAll();                                        return true;
        case glint_vk::KEY_C: copy();                                             return true;
        case glint_vk::KEY_V: if (!readonly) { paste(); }                         return true;
        case glint_vk::KEY_X: if (!readonly) { cut();   }                         return true;
        case glint_vk::KEY_Z: if (!readonly) { if (key.shift) redo(); else undo(); } return true;
        case glint_vk::KEY_Y: if (!readonly) { redo(); }                          return true;
        case glint_vk::LEFT:  moveWordLeft (key.shift);                           return true;
        case glint_vk::RIGHT: moveWordRight(key.shift);                           return true;
        case glint_vk::UP:                                                        return true;
        case glint_vk::DOWN:                                                      return true;
        case glint_vk::HOME:  moveToStart  (key.shift);                           return true;
        case glint_vk::END:   moveToEnd    (key.shift);                           return true;
        case glint_vk::BACK:  if (!readonly) { deleteWordBackward(); }            return true;
        case glint_vk::DEL:   if (!readonly) { deleteWordForward();  }            return true;
      }
      // Other ctrl combos — don't consume, let them fall through.
      return false;
    }

    // ── Navigation ───────────────────────────────────────────────────────────
    switch (key.vk)
    {
      case glint_vk::LEFT:   moveCursorLeft (key.shift); return true;
      case glint_vk::RIGHT:  moveCursorRight(key.shift); return true;
      case glint_vk::UP:                                 return true;
      case glint_vk::DOWN:                               return true;
      case glint_vk::HOME:   moveToStart    (key.shift); return true;
      case glint_vk::END:    moveToEnd      (key.shift); return true;
      case glint_vk::BACK:   if (!readonly) { deleteBackward(); } return true;
      case glint_vk::DEL:    if (!readonly) { deleteForward();  } return true;
      case glint_vk::ESCAPE: blur();                     return true;
    }

    // ── Printable character ────────────────────────────────────────────────
    // utf8[0] >= 0x20 covers all printable ASCII.
    // Values with high bit set (>= 0x80) are multi-byte UTF-8 continuations
    // (including multi-codepoint emoji ZWJ sequences up to 32 bytes).
    if (!readonly && (static_cast<unsigned char>(key.utf8[0]) >= 0x20))
    {
      // Build the full UTF-8 string: take bytes up to the NUL.
      std::string s;
      for (int i = 0; i < 32 && key.utf8[i] != '\0'; ++i)
        s += key.utf8[i];
      if (!s.empty() && filterChar(s)) insertText(s);
      return true;
    }

    return false;
  }

protected:
  // ── Text buffer ────────────────────────────────────────────────────────────

  std::string mText;
  int  mCursorPos = 0;   // byte index into mText (always on a codepoint boundary)
  int  mSelStart  = -1;  // -1 = no selection; otherwise lo ≤ hi byte positions
  int  mSelEnd    = -1;  // can be < mSelStart (cursor-to-anchor order)

  bool mFocused         = false;
  bool mJustGainedFocus = false;  // cleared by first click/key after focus gain
  std::chrono::steady_clock::time_point mBlinkStart;

  // ── Undo / Redo (multi-level, up to 64 entries) ────────────────────────────
  struct UndoEntry { std::string text; int cursor = 0; };
  std::vector<UndoEntry> mUndoStack;   // history (most-recent last)
  std::vector<UndoEntry> mRedoStack;   // redo history (most-recent last)
  static constexpr int kMaxUndoLevels = 64;

  // ── Hooks for subclasses ───────────────────────────────────────────────────

  /** Called after any text change (insert / delete / paste / undo). */
  virtual void onTextChanged() {}

  /** Called after any cursor or selection move.  glint_input uses this to
   *  call ensureCursorVisible() so the caret scrolls into view. */
  virtual void onCursorMoved() {}

  /**
   * Character filter — called before inserting a printable character.
   * Return false to reject the character.  Default: accept all.
   * glint_input overrides this for the "number" type.
   */
  virtual bool filterChar(const std::string& /*utf8char*/) { return true; }

  // ── Blur helper ────────────────────────────────────────────────────────────

  // Remove focus from this node. Delegates to glint_element::Blur() which
  // is defined in glint_element.hpp after glint_document is complete.
  void blur()
  {
    Blur();   // glint_element::Blur() declared in glint_element.hpp
  }

  // ── Caret blink ────────────────────────────────────────────────────────────

  bool caretVisible() const
  {
    if (!mFocused) return false;
    auto elapsed = std::chrono::steady_clock::now() - mBlinkStart;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return (ms / 530) % 2 == 0;   // ~900 ms period, visible for first half
  }

  void resetBlink()
  {
    mBlinkStart = std::chrono::steady_clock::now();
  }

  std::chrono::steady_clock::time_point nextCaretToggleTime() const
  {
    using namespace std::chrono;
    if (!mFocused) return steady_clock::time_point::max();

    constexpr auto kBlinkHalfPeriod = milliseconds(530);
    if (mBlinkStart == steady_clock::time_point{})
      return steady_clock::now();

    const auto now = steady_clock::now();
    if (now <= mBlinkStart)
      return mBlinkStart + kBlinkHalfPeriod;

    const auto elapsed = duration_cast<milliseconds>(now - mBlinkStart);
    const auto phaseCount = elapsed / kBlinkHalfPeriod;
    return mBlinkStart + (phaseCount + 1) * kBlinkHalfPeriod;
  }

  // ── UTF-8 helpers ────────────────────────────────────────────────────────

  /** Advance one Unicode codepoint from `pos`. */
  static int nextCodepoint(const std::string& s, int pos)
  {
    if (pos >= static_cast<int>(s.size())) return pos;
    const auto c = static_cast<unsigned char>(s[pos]);
    if      (c < 0x80) return pos + 1;
    else if (c < 0xE0) return pos + 2;
    else if (c < 0xF0) return pos + 3;
    else               return pos + 4;
  }

  /** Retreat one Unicode codepoint from `pos`. */
  static int prevCodepoint(const std::string& s, int pos)
  {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return p;
  }

  static int codepointCount(const std::string& s, int start = 0, int end = -1)
  {
    const int clampedStart = std::max(0, std::min(start, static_cast<int>(s.size())));
    const int clampedEnd = end < 0
      ? static_cast<int>(s.size())
      : std::max(clampedStart, std::min(end, static_cast<int>(s.size())));

    int count = 0;
    for (int pos = clampedStart; pos < clampedEnd; pos = nextCodepoint(s, pos))
      ++count;
    return count;
  }

  static std::string truncateToCodepoints(const std::string& s, int maxCodepoints)
  {
    if (maxCodepoints < 0) return s;
    int end = 0;
    for (int count = 0; count < maxCodepoints && end < static_cast<int>(s.size()); ++count)
      end = nextCodepoint(s, end);
    return s.substr(0, static_cast<std::size_t>(end));
  }

  // ── Word boundary helpers ───────────────────────────────────────────────────

  /** True for ASCII alphanumeric, underscore, or any non-ASCII byte (UTF-8). */
  static bool isWordChar(unsigned char c)
  {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
  }

  /** True for horizontal whitespace (space, tab). */
  static bool isSpaceChar(unsigned char c)
  {
    return c == ' ' || c == '\t';
  }

  /**
   * Compute the [start, end) byte range of the lexical unit at byte position
   * `bytePos` in `text`, matching Chrome double-click selection rules:
   *   - Alphanumeric / '_' / non-ASCII  → expand through the contiguous word run
   *   - Whitespace                      → expand through the whitespace run
   *   - Punctuation                     → exactly that one codepoint
   * Returns {0,0} for empty text.
   */
  static std::pair<int,int> wordBoundary(const std::string& text, int bytePos)
  {
    if (text.empty()) return { 0, 0 };
    bytePos = std::max(0, std::min(bytePos, static_cast<int>(text.size())));

    // If positioned at end, step back to the last codepoint.
    int checkPos = bytePos;
    if (checkPos >= static_cast<int>(text.size()))
      checkPos = prevCodepoint(text, static_cast<int>(text.size()));

    const unsigned char c = static_cast<unsigned char>(text[checkPos]);

    enum class Cls { Word, Space, Punct };
    const Cls cls = isWordChar(c)  ? Cls::Word
                  : isSpaceChar(c) ? Cls::Space
                                   : Cls::Punct;

    // Punctuation: select only the single codepoint.
    if (cls == Cls::Punct)
      return { checkPos, nextCodepoint(text, checkPos) };

    // Expand left.
    int start = checkPos;
    while (start > 0)
    {
      const int prev = prevCodepoint(text, start);
      const unsigned char pc = static_cast<unsigned char>(text[prev]);
      const bool match = (cls == Cls::Word) ? isWordChar(pc) : isSpaceChar(pc);
      if (!match) break;
      start = prev;
    }

    // Expand right.
    int end = nextCodepoint(text, checkPos);
    while (end < static_cast<int>(text.size()))
    {
      const unsigned char ec = static_cast<unsigned char>(text[end]);
      const bool match = (cls == Cls::Word) ? isWordChar(ec) : isSpaceChar(ec);
      if (!match) break;
      end = nextCodepoint(text, end);
    }

    return { start, end };
  }

  // ── Text operations ────────────────────────────────────────────────────────

  void pushUndo()
  {
    if (!mRedoStack.empty()) mRedoStack.clear();   // new mutation clears redo
    mUndoStack.push_back({ mText, mCursorPos });
    if (static_cast<int>(mUndoStack.size()) > kMaxUndoLevels)
      mUndoStack.erase(mUndoStack.begin());
  }

  void insertText(const std::string& s)
  {
    std::string limited = s;
    const int maxLength = maxTextLength();
    if (maxLength >= 0)
    {
      const int selectionLength = (mSelStart != -1 && mSelStart != mSelEnd)
        ? codepointCount(mText, std::min(mSelStart, mSelEnd), std::max(mSelStart, mSelEnd))
        : 0;
      const int currentLength = codepointCount(mText);
      const int available = maxLength - (currentLength - selectionLength);
      if (available <= 0) return;
      limited = truncateToCodepoints(s, available);
      if (limited.empty()) return;
    }

    pushUndo();
  if (hasSelection()) deleteSelection(false);   // replace selection if any
    mText.insert(static_cast<size_t>(mCursorPos), limited);
    mCursorPos += static_cast<int>(limited.size());
    mSelStart = mSelEnd = -1;
    onTextChanged();
    if (onChange) onChange(mText);
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  void deleteBackward()
  {
    if (mSelStart != -1)
    {
      const int selectionLength = selectedCodepointLength();
      if (!canDeleteCodepoints(selectionLength)) return;
      pushUndo();
      deleteSelection();
      return;
    }
    if (mCursorPos <= 0) return;
    if (!canDeleteCodepoints(1)) return;
    pushUndo();
    const int prev = prevCodepoint(mText, mCursorPos);
    mText.erase(static_cast<size_t>(prev),
                static_cast<size_t>(mCursorPos - prev));
    mCursorPos = prev;
    onTextChanged();
    if (onChange) onChange(mText);
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  void deleteForward()
  {
    if (mSelStart != -1)
    {
      const int selectionLength = selectedCodepointLength();
      if (!canDeleteCodepoints(selectionLength)) return;
      pushUndo();
      deleteSelection();
      return;
    }
    if (mCursorPos >= static_cast<int>(mText.size())) return;
    if (!canDeleteCodepoints(1)) return;
    pushUndo();
    const int next = nextCodepoint(mText, mCursorPos);
    mText.erase(static_cast<size_t>(mCursorPos),
                static_cast<size_t>(next - mCursorPos));
    onTextChanged();
    if (onChange) onChange(mText);
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  bool deleteSelection(bool enforceMinLength = true)
  {
    if (mSelStart == -1) return false;
    const int lo = std::min(mSelStart, mSelEnd);
    const int hi = std::max(mSelStart, mSelEnd);
    const int selectionLength = codepointCount(mText, lo, hi);
    if (enforceMinLength && !canDeleteCodepoints(selectionLength)) return false;
    mText.erase(static_cast<size_t>(lo), static_cast<size_t>(hi - lo));
    mCursorPos = lo;
    mSelStart = mSelEnd = -1;
    onTextChanged();
    if (onChange) onChange(mText);
    setDirty(false);
    return true;
  }

  // ── Cursor movement ────────────────────────────────────────────────────────

  void moveCursorLeft(bool shift)
  {
    resetBlink();
    if (!shift && mSelStart != -1)
    {
      // Collapse to left end of selection.
      mCursorPos = std::min(mSelStart, mSelEnd);
      mSelStart = mSelEnd = -1;
    }
    else
    {
      if (shift && mSelStart == -1) mSelStart = mCursorPos;
      mCursorPos = prevCodepoint(mText, mCursorPos);
      if (shift)
      {
        mSelEnd = mCursorPos;
        if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
      }
      else
      {
        mSelStart = mSelEnd = -1;
      }
    }
    onCursorMoved();
    setDirty(false);
  }

  void moveCursorRight(bool shift)
  {
    resetBlink();
    if (!shift && mSelStart != -1)
    {
      mCursorPos = std::max(mSelStart, mSelEnd);
      mSelStart = mSelEnd = -1;
    }
    else
    {
      if (shift && mSelStart == -1) mSelStart = mCursorPos;
      mCursorPos = std::min(nextCodepoint(mText, mCursorPos),
                            static_cast<int>(mText.size()));
      if (shift)
      {
        mSelEnd = mCursorPos;
        if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
      }
      else
      {
        mSelStart = mSelEnd = -1;
      }
    }
    onCursorMoved();
    setDirty(false);
  }

  void moveToStart(bool shift)
  {
    resetBlink();
    if (shift && mSelStart == -1) mSelStart = mCursorPos;
    mCursorPos = 0;
    if (shift)
    {
      mSelEnd = 0;
      if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
    }
    else
    {
      mSelStart = mSelEnd = -1;
    }
    onCursorMoved();
    setDirty(false);
  }

  void moveToEnd(bool shift)
  {
    resetBlink();
    const int end = static_cast<int>(mText.size());
    if (shift && mSelStart == -1) mSelStart = mCursorPos;
    mCursorPos = end;
    if (shift)
    {
      mSelEnd = end;
      if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
    }
    else
    {
      mSelStart = mSelEnd = -1;
    }
    onCursorMoved();
    setDirty(false);
  }

  void selectAll()
  {
    mSelStart  = 0;
    mSelEnd    = mCursorPos = static_cast<int>(mText.size());
    if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  // ── Word-level cursor movement (Ctrl+Left / Ctrl+Right) ───────────────────

  void moveWordLeft(bool shift)
  {
    resetBlink();
    if (!shift && mSelStart != -1)
    {
      mCursorPos = std::min(mSelStart, mSelEnd);
      mSelStart = mSelEnd = -1;
    }
    else
    {
      if (shift && mSelStart == -1) mSelStart = mCursorPos;
      mCursorPos = _wordLeft(mCursorPos);
      if (shift)
      {
        mSelEnd = mCursorPos;
        if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
      }
      else { mSelStart = mSelEnd = -1; }
    }
    onCursorMoved();
    setDirty(false);
  }

  void moveWordRight(bool shift)
  {
    resetBlink();
    if (!shift && mSelStart != -1)
    {
      mCursorPos = std::max(mSelStart, mSelEnd);
      mSelStart = mSelEnd = -1;
    }
    else
    {
      if (shift && mSelStart == -1) mSelStart = mCursorPos;
      mCursorPos = _wordRight(mCursorPos);
      if (shift)
      {
        mSelEnd = mCursorPos;
        if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
      }
      else { mSelStart = mSelEnd = -1; }
    }
    onCursorMoved();
    setDirty(false);
  }

  // ── Word-level deletion (Ctrl+Backspace / Ctrl+Delete) ────────────────────

  void deleteWordBackward()
  {
    if (mSelStart != -1)
    {
      const int selectionLength = selectedCodepointLength();
      if (!canDeleteCodepoints(selectionLength)) return;
      pushUndo();
      deleteSelection();
      return;
    }
    const int target = _wordLeft(mCursorPos);
    if (target == mCursorPos) return;
    if (!canDeleteCodepoints(codepointCount(mText, target, mCursorPos))) return;
    pushUndo();
    mText.erase(static_cast<size_t>(target),
                static_cast<size_t>(mCursorPos - target));
    mCursorPos = target;
    onTextChanged();
    if (onChange) onChange(mText);
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  void deleteWordForward()
  {
    if (mSelStart != -1)
    {
      const int selectionLength = selectedCodepointLength();
      if (!canDeleteCodepoints(selectionLength)) return;
      pushUndo();
      deleteSelection();
      return;
    }
    const int target = _wordRight(mCursorPos);
    if (target == mCursorPos) return;
    if (!canDeleteCodepoints(codepointCount(mText, mCursorPos, target))) return;
    pushUndo();
    mText.erase(static_cast<size_t>(mCursorPos),
                static_cast<size_t>(target - mCursorPos));
    onTextChanged();
    if (onChange) onChange(mText);
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  // ── Clipboard ──────────────────────────────────────────────────────────────

  void copy()
  {
    if (mSelStart == -1) return;
    const int lo = std::min(mSelStart, mSelEnd);
    const int hi = std::max(mSelStart, mSelEnd);
    setClipboard(mText.substr(static_cast<size_t>(lo),
                              static_cast<size_t>(hi - lo)));
  }

  void cut()
  {
    if (mSelStart == -1) return;
    if (!canDeleteCodepoints(selectedCodepointLength())) return;
    copy();
    pushUndo();
    deleteSelection();
  }

  int selectedCodepointLength() const
  {
    if (mSelStart == -1 || mSelStart == mSelEnd) return 0;
    return codepointCount(mText, std::min(mSelStart, mSelEnd), std::max(mSelStart, mSelEnd));
  }

  bool canDeleteCodepoints(int removedCodepoints) const
  {
    const int minLength = minTextLength();
    if (minLength < 0 || removedCodepoints <= 0) return true;
    return codepointCount(mText) - removedCodepoints >= minLength;
  }

  void paste()
  {
    const std::string s = getClipboard();
    if (!s.empty()) insertText(s);
  }

  void undo()
  {
    if (mUndoStack.empty()) return;
    mRedoStack.push_back({ mText, mCursorPos });
    const auto& e = mUndoStack.back();
    mText      = e.text;
    mCursorPos = e.cursor;
    mUndoStack.pop_back();
    mSelStart = mSelEnd = -1;
    onTextChanged();
    if (onChange) onChange(mText);
    onCursorMoved();
    setDirty(false);
  }

  void redo()
  {
    if (mRedoStack.empty()) return;
    mUndoStack.push_back({ mText, mCursorPos });
    const auto& e = mRedoStack.back();
    mText      = e.text;
    mCursorPos = e.cursor;
    mRedoStack.pop_back();
    mSelStart = mSelEnd = -1;
    onTextChanged();
    if (onChange) onChange(mText);
    onCursorMoved();
    setDirty(false);
  }

  // ── System clipboard ──────────────────────────────────────────────────────

  static void setClipboard(const std::string& text)
  {
#ifdef _WIN32
    if (!::OpenClipboard(nullptr)) return;
    ::EmptyClipboard();
    // Convert UTF-8 → UTF-16 and store as CF_UNICODETEXT for full Unicode support.
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen > 0)
    {
      HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(WCHAR));
      if (h)
      {
        WCHAR* p = static_cast<WCHAR*>(::GlobalLock(h));
        if (p) { ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p, wlen); ::GlobalUnlock(h); }
        ::SetClipboardData(CF_UNICODETEXT, h);
      }
    }
    ::CloseClipboard();
#elif defined(__APPLE__)
    glint_platform::setClipboardText(text);
#endif
  }

  static std::string getClipboard()
  {
#ifdef _WIN32
    if (!::OpenClipboard(nullptr)) return {};
    // Prefer CF_UNICODETEXT for full Unicode; convert back to UTF-8.
    HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
    std::string result;
    if (h)
    {
      const WCHAR* p = static_cast<const WCHAR*>(::GlobalLock(h));
      if (p)
      {
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1)   // len includes NUL
        {
          result.resize(static_cast<size_t>(len - 1));
          ::WideCharToMultiByte(CP_UTF8, 0, p, -1, &result[0], len, nullptr, nullptr);
        }
        ::GlobalUnlock(h);
      }
    }
    ::CloseClipboard();
    return result;
#elif defined(__APPLE__)
    return glint_platform::getClipboardText();
#else
    return {};
#endif
  }

  // ── Word-boundary helpers (Ctrl+Arrow / double-click) ──────────────────────

  /** Returns the byte position of the start of the previous word (Ctrl+Left). */
  int _wordLeft(int pos) const
  {
    if (pos <= 0) return 0;
    pos = prevCodepoint(mText, pos);
    // Skip whitespace left.
    while (pos > 0 && isSpaceChar(static_cast<unsigned char>(mText[pos])))
      pos = prevCodepoint(mText, pos);
    // Skip the word-char run left.
    if (pos > 0 && isWordChar(static_cast<unsigned char>(mText[pos])))
    {
      while (pos > 0)
      {
        const int prev = prevCodepoint(mText, pos);
        if (!isWordChar(static_cast<unsigned char>(mText[prev]))) break;
        pos = prev;
      }
    }
    return pos;
  }

  /** Returns the byte position of the end of the current/next word (Ctrl+Right). */
  int _wordRight(int pos) const
  {
    const int n = static_cast<int>(mText.size());
    if (pos >= n) return n;
    // Skip whitespace right.
    while (pos < n && isSpaceChar(static_cast<unsigned char>(mText[pos])))
      pos = nextCodepoint(mText, pos);
    if (pos >= n) return n;
    // Skip word-char run, or a single punctuation codepoint.
    if (isWordChar(static_cast<unsigned char>(mText[pos])))
    {
      while (pos < n && isWordChar(static_cast<unsigned char>(mText[pos])))
        pos = nextCodepoint(mText, pos);
    }
    else
    {
      pos = nextCodepoint(mText, pos);   // single punct codepoint
    }
    return pos;
  }

  // ── Skia text measurement helpers ──────────────────────────────────────────
  // Used by subclasses for cursor placement and click-to-position.

  /** Pixel width of `text` at `fontSize` using the shared skFont. */
  float measureFragment(const std::string& text, float fontSize) const
  {
    if (text.empty()) return 0.f;
    SkFont f = skFont(fontSize);
    return f.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8);
  }

  /**
   * X pixel offset of byte position `pos` from the left edge of `text`.
   * Clamped to [0, textWidth].
   */
  float charXOffset(const std::string& text, int pos, float fontSize) const
  {
    if (pos <= 0)                               return 0.f;
    if (pos >= static_cast<int>(text.size()))   return measureFragment(text, fontSize);
    return measureFragment(text.substr(0, static_cast<size_t>(pos)), fontSize);
  }

  /**
   * Find the byte index in `text` closest to pixel x (relative to text start).
   * Uses a "snap to midpoint" rule matching browser selection behaviour.
   */
  int charIndexAtX(const std::string& text, float x, float fontSize) const
  {
    if (text.empty() || x <= 0.f) return 0;
    SkFont f = skFont(fontSize);
    int   pos   = 0;
    float accum = 0.f;
    while (pos < static_cast<int>(text.size()))
    {
      const int next = nextCodepoint(text, pos);
      const std::string ch = text.substr(static_cast<size_t>(pos),
                                         static_cast<size_t>(next - pos));
      const float w = f.measureText(ch.c_str(), ch.size(), SkTextEncoding::kUTF8);
      if (accum + w * 0.5f >= x) return pos;
      accum += w;
      pos = next;
    }
    return static_cast<int>(text.size());
  }
};
