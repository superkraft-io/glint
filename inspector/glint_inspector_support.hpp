#pragma once

/**
 * glint_inspector_support.hpp
 * Glint-owned inspector integration seam.
 *
 * Keeps the inspector as a glint_document capability while letting the host
 * target decide whether the real implementation is available.
 */

#ifndef GLINT_INSPECTOR_DISABLED
#include "window.hpp"
#endif