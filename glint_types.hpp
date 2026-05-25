#pragma once

/**
 * glint_types.hpp
 * Platform-independent POD types for the glint component library.
 *
 * This header has zero dependencies on any host SDK or OS SDK.
 * It is included by glint_element.hpp so every component has access to
 * these types without pulling in any host-specific control headers.
 */

// ── glint_mouse_mod ──────────────────────────────────────────────────────────
// Platform-neutral mouse modifier state captured at the time of a mouse event.
// Using our own type keeps glint_element and glint_document decoupled from any
// host UI SDK.
struct glint_mouse_mod
{
  bool L   = false;   // left mouse button held
  bool R   = false;   // right mouse button held
  bool Mid = false;   // middle mouse button held
  bool S   = false;   // Shift
  bool C   = false;   // Ctrl
  bool A   = false;   // Alt / Option
  bool M   = false;   // Meta / Cmd (macOS)
};

// ── glint_key_press ──────────────────────────────────────────────────────────
// Platform-independent key press payload used by the scene graph.
struct glint_key_press
{
  int  vk       = 0;      // Windows VK_* virtual key code
  char utf8[33] = {};     // UTF-8 text (printable keys, up to 32 bytes + NUL; covers ZWJ emoji sequences)
  bool shift    = false;
  bool ctrl     = false;
  bool alt      = false;
};

// ── glint_point ──────────────────────────────────────────────────────────────
// Lightweight float point used for layout-space geometry queries.
struct glint_point
{
  float x = 0.f;
  float y = 0.f;

  glint_point() = default;
  glint_point(float px, float py) : x(px), y(py) {}
};

// ── Sentinel values ───────────────────────────────────────────────────────────
// Standalone-owned sentinels used throughout the scene graph.

static constexpr int glint_no_tag     = -1;   // "no tag" sentinel (= kNoTag)
static constexpr int glint_no_val_idx = -1;   // "no value index" sentinel (= kNoValIdx)

// Backward-compatible names retained for older call sites.
static constexpr int kNoTag    = glint_no_tag;
static constexpr int kNoValIdx = glint_no_val_idx;
