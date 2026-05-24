#pragma once

/**
 * glint_core.hpp
 * Standalone-safe umbrella for the glint core library.
 *
 * This header intentionally excludes host adapters. It provides the scene
 * graph, document, standalone window dependencies, and built-in components
 * without pulling in any host bridge.
 *
 * Adapter-specific entry points remain separate:
 *   - glint.hpp         standalone compatibility umbrella
 *   - adapters/sk/...     raw-Skia bridge helpers
 *   - glint_window.hpp     native standalone window host
 */

#include "events/glint_event.hpp"
#include "element/glint_html_element.hpp"
#include "glint_types.hpp"
#include "render/glint_resource_request.hpp"
#include "utils/glint_path.hpp"
#include "glint_style.hpp"
#include "render/glint_tree_node.hpp"
#include "utils/glint_debug.hpp"
#include "render/glint_filter.hpp"
#include "glint_element.hpp"
#include "glint_document.hpp"
#include "components/glint_button.hpp"
#include "components/glint_image.hpp"
#include "components/input/glint_input.hpp"
#include "components/glint_scrollbar/glint_scrollbar.hpp"
