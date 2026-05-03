#pragma once

/**
 * glint.hpp
 * Compatibility umbrella for the standalone glint component library.
 *
 * This header aliases the standalone-safe umbrella so older include sites can
 * keep using glint.hpp without pulling any host bridge code.
 */

#include "glint_render_backend.h"
#include "glint_standalone.hpp"
