#pragma once

/**
 * glint_progress.hpp
 * Progress bar and meter components for glint.
 *
 * glint_progress  — models <progress>: determinate or indeterminate bar.
 * glint_meter     — models <meter>: bounded gauge with low/high/optimum zones.
 *
 * Both are built as styled child elements (track + fill).  Set public fields
 * then call setDirty() to refresh, or use the setValue() / setProgress() helpers.
 *
 * Usage (progress):
 *   add.fromClass<glint_progress>([](glint_progress& p) {
 *     p.value = 0.65f;            // raw value; ratio = value/max
 *     p.max   = 1.0f;             // default 1.0 — matches HTML spec
 *     // Negative value (or leave default) = indeterminate animation
 *     p.style.width  = "100%";    // overrides UA default of 300px
 *     p.style.height = 8.f;       // overrides UA default of 14px
 *     // Visual appearance driven by CSS class selectors:
 *     //   p.trackClassName = "glint_progress_track";  (default)
 *     //   p.fillClassName  = "glint_progress_fill";   (default)
 *     // Override fillClassName to use a custom CSS rule, e.g.:
 *     //   p.fillClassName = "glint_progress_fill--green";
 *   });
 *
 * Usage (meter):
 *   add.fromClass<glint_meter>([](glint_meter& m) {
 *     m.value   = 0.6f;   m.low  = 0.25f;  m.high = 0.75f;
 *     m.optimum = 0.85f;  // above 'high' → green zone is upper range
 *     m.style.width = "100%";  m.style.height = 10.f;
 *   });
 */

#include "../glint_element.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

// ── glint_progress ────────────────────────────────────────────────────────────

class glint_progress : public glint_element
{
public:
  // value: raw value in [0, max]. Negative = indeterminate (matches Chrome's
  // "value attribute absent" behaviour). Default is -1 (indeterminate).
  float       value          = -1.0f;
  // max: the upper bound for value (default 1.0, matching the HTML spec).
  // Fill ratio = clamp(value / max, 0, 1).
  float       max            = 1.0f;

  const char* trackClassName = "glint_progress_track";
  const char* fillClassName  = "glint_progress_fill";

  glint_progress()
  {
    mTrack = new glint_element();
    mTrack->className = trackClassName;
    addChild(mTrack);

    mFill = new glint_element();
    mFill->className = fillClassName;
    addChild(mFill);
  }

  // Set the value (and optionally the max).
  void setValue(float v, float m = -1.f)
  {
    value = v;
    if (m > 0.f) max = m;
    _sync();
    setDirty(false);
  }

  const char* typeName() const override { return "progress"; }

  void Layout(glint_canvas* g) override
  {
    _sync();
    glint_element::Layout(g);
  }

  void syncBeforeLayout() override { _sync(); }

  // Drive the indeterminate animation: called every frame while indeterminate.
  void DrawContentToCanvas(SkCanvas* /*canvas*/) override
  {
    if (value >= 0.f) return;   // determinate — nothing extra to do

    // Advance time and request another frame so the animation keeps running.
    const auto now = std::chrono::steady_clock::now();
    if (mAnimStart.time_since_epoch().count() == 0)
      mAnimStart = now;

    _updateIndeterminate(now);
    setDirty(false);   // request next frame
  }

private:
  glint_element* mTrack = nullptr;
  glint_element* mFill  = nullptr;

  // Indeterminate animation state
  std::chrono::steady_clock::time_point mAnimStart{};

  // Smooth cubic ease-in-out: accelerates from 0, decelerates to 1.
  static float _easeInOut(float t)
  {
    t = std::max(0.f, std::min(1.f, t));
    return t < 0.5f ? 4.f * t * t * t
                    : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
  }

  void _updateIndeterminate(std::chrono::steady_clock::time_point now)
  {
    if (!mFill) return;

    constexpr float kCycleMs     = 2500.f;  // 20 × 125 ms (Chrome cycle)
    constexpr float kBarFrac     = 0.2f;    // 1/5 of track width
    constexpr float kMovableFrac = 1.f - kBarFrac; // 0.8

    const float elapsedMs = std::chrono::duration<float, std::milli>(now - mAnimStart).count();
    const float progress  = std::fmod(elapsedMs, kCycleMs) / kCycleMs; // [0, 1)

    // Map to a 0→1→0 triangle, then ease each half for smooth tweening.
    const float t        = (progress < 0.5f) ? progress * 2.f : (1.f - progress) * 2.f;
    const float leftFrac = _easeInOut(t) * kMovableFrac;

    char wBuf[32], lBuf[32];
    std::snprintf(wBuf, sizeof(wBuf), "%.4g%%", kBarFrac  * 100.f);
    std::snprintf(lBuf, sizeof(lBuf), "%.4g%%", leftFrac  * 100.f);

    mFill->style.width = wBuf;
    mFill->style.left  = lBuf;
    mFill->className   = "glint_progress_fill--indeterminate";
  }

  void _sync()
  {
    if (mTrack)
      mTrack->className = trackClassName;

    if (mFill)
    {
      mFill->className = fillClassName;

      if (value >= 0.f)
      {
        // Determinate: reset animation clock and show fixed fill width.
        mAnimStart = {};
        const float pct = std::max(0.f, std::min(1.f, value / std::max(max, 1e-6f)));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g%%", pct * 100.f);
        mFill->style.width = buf;
        mFill->style.left  = 0.f;
      }
      else
      {
        // Indeterminate: kick off first frame if not already animating.
        if (mAnimStart.time_since_epoch().count() == 0)
        {
          mFill->style.left  = "0%";
          mFill->style.width = "20%";
          setDirty(false);   // ensure first frame fires
        }
      }
    }
  }
};

// ── glint_meter ───────────────────────────────────────────────────────────────
// Colours the fill green/yellow/red based on value vs low/high/optimum zones.

class glint_meter : public glint_element
{
public:
  float value   = 0.5f;   // 0.0 – 1.0
  float low     = 0.25f;  // threshold for "low" zone start
  float high    = 0.75f;  // threshold for "high" zone start
  float optimum = 0.85f;  // hints which end of the range is "good"
  float radius  = 999.f;

  const char* trackColor    = "#2d2d2d";

  glint_meter()
  {
    style.position = "relative";
    style.overflow = "hidden";

    mTrack = new glint_element();
    mTrack->style.position        = "absolute";
    mTrack->style.left            = 0.f;
    mTrack->style.top             = 0.f;
    mTrack->style.width           = "100%";
    mTrack->style.height          = "100%";
    addChild(mTrack);

    mFill = new glint_element();
    mFill->style.position  = "absolute";
    mFill->style.left      = 0.f;
    mFill->style.top       = 0.f;
    mFill->style.height    = "100%";
    addChild(mFill);
  }

  void setValue(float v)
  {
    value = std::max(0.f, std::min(1.f, v));
    _sync();
    setDirty(false);
  }

  const char* typeName() const override { return "meter"; }

  void Layout(glint_canvas* g) override
  {
    _sync();
    glint_element::Layout(g);
  }

  void syncBeforeLayout() override { _sync(); }

private:
  glint_element* mTrack = nullptr;
  glint_element* mFill  = nullptr;

  // Resolve fill color: green (optimal), yellow (suboptimal), red (bad).
  const char* _fillColor() const
  {
    const bool optimumIsHigh = (optimum >= high);
    if (optimumIsHigh)
    {
      if (value >= high)  return "#3a7a3a";   // optimal (green)
      if (value >= low)   return "#b5a400";   // suboptimal (yellow)
      return "#8b3a3a";                       // bad (red)
    }
    else
    {
      if (value <= low)   return "#3a7a3a";   // optimal
      if (value <= high)  return "#b5a400";   // suboptimal
      return "#8b3a3a";                       // bad
    }
  }

  void _sync()
  {
    if (mTrack)
    {
      mTrack->style.borderRadius    = radius;
      mTrack->style.backgroundColor = trackColor;
    }
    if (mFill)
    {
      const float pct = std::max(0.f, std::min(1.f, value));
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.4g%%", pct * 100.f);
      mFill->style.width           = buf;
      mFill->style.borderRadius    = radius;
      mFill->style.backgroundColor = _fillColor();
    }
  }
};
