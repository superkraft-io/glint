#pragma once

/**
 * glint_animation_clock.hpp
 * The clock time-based drawing reads: transitions and @keyframes
 * (glint_element::_animationNow()), animated shaders, indeterminate
 * progress. The real clock unless a host installs another one, e.g.
 * headless mode's virtual clock (one 60 Hz step per frame drawn) so
 * screenshots of animated content are the same every run.
 */

#include <chrono>

/** Installed clock; nullptr = std::chrono::steady_clock. Set it on the
 *  document thread before the first frame. */
inline std::chrono::steady_clock::time_point (*gGlintAnimationClock)() = nullptr;

inline std::chrono::steady_clock::time_point glint_animation_now()
{
  return gGlintAnimationClock ? gGlintAnimationClock() : std::chrono::steady_clock::now();
}
