#pragma once

/**
 * glint_perf.hpp
 * Per-thread reflow counters and phase timers.
 *
 * Phase timers (cascade / tick / layout / paint) are always on: each costs one
 * steady_clock read per phase per frame (cascade: per _applyCssToElement call,
 * which is far more expensive than the clock read itself).
 *
 * Event counters (merges, layout calls, text measurements, ...) are compiled in
 * only when GLINT_PERF_COUNTERS is defined (the bench target defines it), so
 * release apps pay nothing for them.
 *
 *   glint_perf_counters& p = glint_perf();
 *   p.reset();
 *   doc.updateStyleAndLayout();
 *   std::printf("%llu merges\n", p.mergedStyle);
 *
 * Counters are thread_local: a document's style/layout/paint all run on its
 * owning window thread, so read them from that thread.
 */

#include <chrono>
#include <cstdint>

struct glint_perf_counters
{
  // ── Phase times (ms), accumulated until reset() ────────────────────────────
  double cascadeMs = 0.0;   // _applyCssToElement (selector matching + apply)
  double tickMs    = 0.0;   // tickTransitionsAll (style merge + transitions)
  double layoutMs  = 0.0;   // root Layout()
  double paintMs   = 0.0;   // DrawToCanvas / Draw traversal
  uint64_t layoutPasses = 0; // root Layout() runs

  // ── Event counters (GLINT_PERF_COUNTERS only) ──────────────────────────────
  uint64_t applyCss            = 0;  // _applyCssToElement calls
  uint64_t rulesTested         = 0;  // selector-list match attempts in the cascade
  uint64_t mergedStyle         = 0;  // _mergedStyle() calls
  uint64_t refreshLayoutStyle  = 0;  // _refreshLayoutStyle() calls
  uint64_t layoutCalls         = 0;  // glint_element::Layout() (base dispatcher) calls
  uint64_t childPrefW          = 0;  // childPrefW() calls
  uint64_t childPrefH          = 0;  // childPrefH() calls
  uint64_t measureIntrinsicW   = 0;
  uint64_t measureIntrinsicH   = 0;
  uint64_t measureText         = 0;  // SkFont::measureText calls on layout paths
  uint64_t measureTextBytes    = 0;  // total UTF-8 bytes measured (exposes O(n^2) wrapping)
  uint64_t lengthParses        = 0;  // glint_length / side calc parses
  uint64_t styleSkips          = 0;  // merges skipped by incremental style
  uint64_t layoutSkips         = 0;  // child Layout() calls skipped by incremental layout
  uint64_t paintCulled         = 0;  // children not drawn: entirely outside the clip
  uint64_t cascadeVerifyFailures = 0; // indexed cascade != reference (verify mode; always counted)
  uint64_t styleVerifyFailures = 0;  // skipped merges that differed (verify mode; always counted)

  void reset() { *this = glint_perf_counters{}; }
};

inline glint_perf_counters& glint_perf()
{
  static thread_local glint_perf_counters sCounters;
  return sCounters;
}

#ifdef GLINT_PERF_COUNTERS
  #define GLINT_PERF_INC(field)       (++glint_perf().field)
  #define GLINT_PERF_ADD(field, n)    (glint_perf().field += static_cast<uint64_t>(n))
  #define GLINT_PERF_TEXT(bytes)      (++glint_perf().measureText, \
                                       glint_perf().measureTextBytes += static_cast<uint64_t>(bytes))
#else
  #define GLINT_PERF_INC(field)       ((void)0)
  #define GLINT_PERF_ADD(field, n)    ((void)0)
  #define GLINT_PERF_TEXT(bytes)      ((void)0)
#endif

/** RAII timer that adds its lifetime (ms) to a double accumulator. */
struct glint_perf_timer
{
  explicit glint_perf_timer(double& acc)
    : mAcc(acc), mStart(std::chrono::steady_clock::now()) {}
  ~glint_perf_timer()
  {
    mAcc += std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - mStart).count();
  }
  glint_perf_timer(const glint_perf_timer&) = delete;
  glint_perf_timer& operator=(const glint_perf_timer&) = delete;

private:
  double& mAcc;
  std::chrono::steady_clock::time_point mStart;
};

/** Per-frame phase breakdown exposed by glint_document::lastFrameStats(). */
struct glint_frame_stats
{
  double cascadeMs = 0.0;   // cascade work since the previous frame (events + this frame)
  double tickMs    = 0.0;
  double layoutMs  = 0.0;
  double paintMs   = 0.0;
  bool   layoutRan = false;
};
