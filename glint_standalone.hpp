#pragma once

/**
 * glint_standalone.hpp
 * Umbrella for standalone/native glint code.
 *
 * This header includes the standalone-safe core plus the declarative builder
 * API and excludes any host bridge layer.
 *
 * Use this from native windows, standalone tools, and app-side UI headers that
 * need `add.*` / builder facilities without pulling in plugin bridge code.
 */

#include "glint_core.hpp"
#include "components/glint_builder.hpp"
#include "inspector/glint_inspector_support.hpp"
