# Full Feature Inventory

This page lists the Glint capabilities that are currently present in the codebase.

It is intended as a practical inventory of what Glint actually ships today, not a roadmap.

## Platform and host integration

Relevant source:

- `glint_window.hpp`
- `glint_view.hpp`
- `platform/glint_window_base.hpp`
- `platform/glint_view_base.hpp`
- `platform/win32/glint_window_win32.hpp`
- `platform/win32/glint_view_win32.hpp`
- `platform/mac/glint_window_mac.hpp` / `.mm`
- `platform/mac/glint_view_mac.hpp` / `.mm`

Current host features:

- Standalone top-level app windows via `glint_window`
- Embedded child-window hosting via `glint_view` / `glint::createView(...)`
- Parent-handle-in embedded API that creates and owns the native child window handle
- Public `document()` access from the host/view layer
- Runtime backend selection through `glint_backend` (`Auto`, `CPU`, `Metal`, `OpenGL`, `D3D11`, `D3D12`, `Vulkan`)
- View lifecycle controls such as resize, redraw request, and native handle access
- Subclass entry points on `glint_window_base` (`buildUI`, `onCreated`, `onThreadStarted`, `onThreadEnded`, `onDestroyed`, `afterRun`, `clearColor`, `defaultWidth/Height`)

Current platform status:

- Win32 is the actively used and tested platform
- macOS host headers and Objective-C++ implementations exist (`platform/mac/`) and integrate with the Metal backend, but should be treated as in progress rather than fully validated
- Linux is not yet implemented; the public host dispatch headers explicitly mark the Linux include path as a TODO

## Rendering backends

Relevant source:

- `CMakeLists.txt`
- `glint_render_backend.h` (generated)
- `platform/win32/glint_renderer_backend_win32.hpp`
- `platform/win32/glint_window_win32.hpp`
- `platform/win32/glint_view_win32.hpp`
- `platform/win32/glint_win32_surface_shared.hpp`
- `platform/win32/glint_win32_host_shared.hpp`
- `platform/mac/glint_view_mac.mm`

Current rendering features:

- CPU raster backend on Win32
- GPU-capable OpenGL backend on Win32 (Skia Ganesh)
- GPU-capable D3D12 backend on Win32 (Skia Graphite)
- GPU-capable Dawn / WebGPU backend (Skia Graphite)
- Metal backend on macOS
- Runtime fallback from GPU to CPU when GPU initialization or surface creation fails
- Shared renderer backend path used by both the top-level window host and the embedded child-view host
- Runtime backend activation logging through `GLINT_ENABLE_RUNTIME_LOG`
- Lightweight frame-time telemetry through `GLINT_ENABLE_TELEMETRY`

The render backend is selected once during Skia setup via `init_skia.mjs --backend <name>`. The script generates `glint_render_backend.h`, which activates the correct compile-time paths automatically. No CMake flags are needed.

Available backends:

| Backend | Flag | Notes |
| --- | --- | --- |
| `cpu` | _(default)_ | Software rasterizer |
| `opengl` | `--backend opengl` | GPU, Skia Ganesh, Win32 |
| `d3d12` | `--backend d3d12` | GPU, Skia Graphite, Windows only |
| `dawn` | `--backend dawn` | GPU, Skia Graphite, WebGPU |
| `metal` | `--backend metal` | GPU, macOS/iOS |

The `glint_backend` enum also reserves `D3D11` and `Vulkan` values, but these are not currently shipped as implemented runtime backends.

## Scene graph and document model

Relevant source:

- `glint_document.hpp`
- `glint_element.hpp`
- `element/glint_html_element.hpp`
- `element/glint_element_tree.hpp`
- `render/glint_tree_node.hpp`

Core document features:

- `glint_document` scene-graph root
- Canvas root element that covers the full host surface
- Parent/child element ownership and tree mutation support
- DOM-like node identity with unique numeric IDs (atomic counter)
- String `id`, `className`, and `innerText` fields on elements
- `classList`-style add/remove/toggle/contains behavior that re-applies CSS and triggers redraw
- Tag registry via `RegisterTag` / `GetNodeWithTag`
- Public `add` convenience API on the document canvas
- Two-phase render pipeline: top-down `Layout()` followed by `Draw()` traversal
- Document-level event bus (`glint_bus`) that publishes `glint_tree_changed_event` and `glint_node_style_changed_event`
- Host-injected redraw callback and popup-menu callback
- DevTools-style matched-rule inspection through `matchedCssRulesFor(...)` with a stylesheet/tree-revision cache

## Declarative builder API

Relevant source:

- `glint_standalone.hpp`
- `glint_core.hpp`
- `components/glint_builder.hpp`

Builder features:

- Declarative callback-driven UI construction
- Built-in `add.*` helpers: `add.div`, `add.button`, `add.checkbox`, `add.input`, `add.select`, `add.image`, `add.list`, `add.tree`, `add.scrollbar`, `add.dial`, `add.colorpicker`, `add.colorpickerWindow`, `add.gradientEditor`, `add.component`, `add.attach`, `add.fromClass<T>`, `add.custom<T>`
- Inline custom-draw components without creating a subclass
- Builder-time class list, inline style, and layout-bounds configuration
- Standalone-safe umbrella headers (`glint_core.hpp`, `glint_standalone.hpp`) that expose the builder without the host bridge layer

## CSS, styling, and layout

Relevant source:

- `glint_style.hpp`
- `default_style.hpp`
- `glint_document.hpp`
- `element/glint_element_layout.hpp`
- `element/glint_element_style.hpp`
- `glint_css_parser/` (tokenizer, rule, selector, cascade, apply)
- `glint_css_parser/glint_css_dom_adapter.hpp`
- `glint_animator.hpp`

Styling and layout features:

- CSS-style properties exposed directly through `glint_style`
- External stylesheet loading through `glint_document::loadStylesheet(...)`
- Inline style parsing support
- CSS cascade and stylesheet application
- CSS selector matching with specificity handling
- Selector support for type, class, ID, attribute, and pseudo-class matching
- DevTools-style matched-rule inspection through `matchedCssRulesFor(...)`
- Polling-based stylesheet hot reload
- Flex, block, inline, and table layout algorithms
- Intrinsic measurement and self-sizing helpers
- Positioning support for static/relative/absolute/fixed/sticky layout states in the layout engine
- CSS-compatible transitions and animation timing functions
- Animated style interpolation for colors, opacity, dimensions, transforms, filters, spacing, borders, masks, and related properties

### HTML and CSS support matrix

These tables are intentionally conservative.

- `Full` means Glint exposes the feature as a supported first-class capability today.
- `Partial` means the syntax or concept exists, but the runtime behavior is narrower than a browser or limited to specific components.
- Features not listed here should be treated as unsupported until they are verified in code.

Glint does not currently ship a generic HTML parser. The HTML rows below refer to the DOM-like C++ surface and the built-in components that intentionally mirror common HTML controls.

#### HTML / DOM support matrix

| Attribute or feature | Full | Partial | Notes |
| --- | --- | --- | --- |
| DOM-like node tree (`id`, `className`, `innerText`, children) | Yes |  | Core fields are exposed directly on `glint_element` and `glint_html_element`. |
| `classList` (`add`, `remove`, `toggle`, `contains`) | Yes |  | Mutations re-apply CSS and trigger redraw. |
| Inline style object (`element.style`) | Yes |  | Backed directly by `glint_style`. |
| EventTarget-style listeners (`addEventListener`, `removeEventListener`, `once`) | Yes |  | Bubble-phase DOM-style dispatch is implemented. |
| Capture-phase listeners |  | Yes | The `capture` option exists, but public capture-phase dispatch is not implemented yet. |
| Scroll DOM properties (`scrollTop`, `scrollLeft`, `scrollWidth`, `scrollHeight`) | Yes |  | Wired into scrollable elements via the element-side scroll bindings. |
| Selector-visible attributes |  | Yes | Live CSS attribute matching currently exposes `id` and `class` only. |
| HTML-style built-ins (`button`, `input`, `select`, `img`, `ul`, `li`) |  | Yes | These are implemented as C++ components with HTML-style tag names, not via markup parsing. |
| `input`-style attributes (`value`, `type`, `placeholder`, `min`, `max`, `readonly`, `disabled`) |  | Yes | Implemented on `glint_input`, not as generic DOM attributes across all elements. |
| `select`-style attributes (`options`, `selectedIndex`, `placeholder`) |  | Yes | Implemented on `glint_select` with native popup-menu backing. |
| `img`-style `src` loading |  | Yes | Implemented by `glint_image`, including file and request-handler loading paths. |

#### CSS selector support matrix

| Attribute or feature | Full | Partial | Notes |
| --- | --- | --- | --- |
| Universal, type, ID, and class selectors | Yes |  | Core selector matching is wired through the DOM adapter and selector engine. |
| Attribute selector syntax (`[attr]`, `[attr=value]`, prefix, suffix, substring, case-insensitive flag) |  | Yes | The parser and matcher support the operators, but the live DOM adapter currently exposes only `id` and `class` as attributes. |
| Descendant and child combinators (` ` and `>`) | Yes |  | These are matched in the live selector engine today. |
| Adjacent sibling combinator (`+`) |  | Yes | Syntax is parsed, but the generic matcher still marks this as DOM-specific placeholder behavior. |
| General sibling combinator (`~`) |  | Yes | Syntax is parsed, but the generic matcher currently returns false for this path. |
| Structural pseudo-classes (`:root`, `:empty`, `:first-child`, `:last-child`, `:only-child`, `:first-of-type`, `:last-of-type`, `:only-of-type`, `:nth-*`) | Yes |  | These are handled directly by the selector matcher and DOM adapter. |
| Logical selector functions (`:not()`, `:is()`, `:where()`) | Yes |  | Nested selector lists are matched by the selector engine. |
| `:has()` |  | Yes | Accepted by the parser and matcher, but should be treated as narrower than full browser descendant-aware `:has()` semantics. |
| State pseudo-classes |  | Yes | Live matching is wired for `:hover`, `:active`, `:focus`, and `:focus-within`; browser-style states such as `:checked`, `:disabled`, and `:enabled` are not surfaced generically. |
| Pseudo-elements (`::before`, `::after`, `::first-line`, `::first-letter`, `::placeholder`, `::selection`) |  | Yes | Names are recognized, but runtime support is selective rather than a full generated-content browser model. |

#### CSS property support matrix

| Attribute or feature | Full | Partial | Notes |
| --- | --- | --- | --- |
| `color` | Yes |  | Mapped directly into `glint_style`. |
| `opacity` | Yes |  | Used by the renderer and animator. |
| `background-color` | Yes |  | Direct style mapping. |
| `background-image`, `background-size`, `background-position`, `background-repeat` | Yes |  | Backed by the style mapper and renderer. |
| `background` shorthand |  | Yes | Stored as a shorthand proxy rather than expanded into the full browser background model. |
| `border-color`, `border-width`, `border-style` | Yes |  | Includes per-edge variants. |
| `border-radius` | Yes |  | Includes per-corner radius fields. |
| `box-shadow` | Yes |  | Parsed and rendered through the style/effect pipeline. |
| `stroke`, `stroke-width`, `stroke-linecap`, `stroke-linejoin`, `stroke-dasharray` | Yes |  | Available for SVG-style drawing and related render paths. |
| `font-family`, `font-size`, `font-weight`, `font-style`, `line-height` | Yes |  | Wired into layout and text rendering. |
| `text-align`, `vertical-align`, `text-decoration` | Yes |  | Mapped through the CSS apply layer and text rendering path. |
| `pointer-events`, `user-select`, `white-space` | Yes |  | Explicitly mapped into `glint_style` and consumed by input/layout code. |
| `margin` and `padding` | Yes |  | Includes individual edge properties and shorthand mapping. |
| `width`, `height`, `min-width`, `max-width`, `min-height`, `max-height` | Yes |  | Used directly by layout and intrinsic sizing helpers. |
| `position`, `top`, `right`, `bottom`, `left`, `z-index` |  | Yes | Positioning exists in the layout engine, but the behavior should be treated as narrower than a full browser layout engine. |
| `display` |  | Yes | Supported values are the shipped Glint layout modes: `block`, `inline`, `flex`, `table`, `table-row`, and `table-cell`. |
| `flex-direction`, `justify-content`, `align-items`, `gap`, `flex-grow` |  | Yes | Flex layout is implemented, but the surface is a subset of full browser Flexbox. |
| `flex` shorthand |  | Yes | Currently maps only the grow component rather than the full `grow shrink basis` model. |
| `overflow`, `overflow-x`, `overflow-y` | Yes |  | Supports visible, hidden, scroll, and auto. |
| `scrollbar-width`, `scrollbar-color` | Yes |  | Explicitly mapped into style state and used by the scrollable rendering path. |
| `object-fit`, `object-position` |  | Yes | Implemented for image/media-style components, not as a generic property every element consumes. |
| `transform` |  | Yes | The documented public surface is currently the translate subset (`translate`, `translateX`, `translateY`). |
| `filter` |  | Yes | Supports the shipped filter function set: blur, brightness, contrast, saturate, grayscale, sepia, invert, opacity, hue-rotate, and drop-shadow. |
| `backdrop-filter` |  | Yes | Uses the same shipped filter-function family, applied to the backdrop path. |
| `mix-blend-mode`, `background-blend-mode`, `isolation` |  | Yes | Integrated into the renderer, but should be treated as narrower than full browser compositing semantics. |
| `mask`, `mask-mode`, `mask-position`, `mask-size`, `mask-repeat`, `mask-origin`, `mask-clip`, `mask-composite` |  | Yes | The mask pipeline is real, including gradients and URL-backed masks, but the overall CSS mask model is still a subset. |
| `transition` |  | Yes | CSS-style transitions are implemented, but the public surface is centered on the shorthand property. |
| `animation` and `@keyframes` |  | Yes | The animator supports `animation` shorthand parsing and document-level `@keyframes` registries, but not the full browser animation/event surface. |

## Events and input

Relevant source:

- `events/glint_event.hpp`
- `events/glint_keyboard_event.hpp`
- `glint_document.hpp`
- `element/glint_html_element.hpp`
- `element/glint_element_events.hpp`

Input and event features:

- DOM-style event objects with `type`, `target`, `currentTarget`, `bubbles`, `cancelable`, `defaultPrevented`
- `addEventListener` / `removeEventListener` with `{ once, capture }` options and a numeric listener-id return value
- Mouse events (`mousedown`, `mouseup`, `click`, `mousemove`, `mouseover`, `mouseout`, `mouseenter`, `mouseleave`) with `clientX/Y`, `movementX/Y`, `button`, `buttons`, and `shiftKey`/`ctrlKey`/`altKey`/`metaKey`
- Wheel events with `deltaX`, `deltaY`, and `deltaZ`
- Keyboard events (`keydown`, `keyup`) with VK code, UTF-8 text, modifier flags, and auto-repeat tracking
- Focus routing (`focus`, `blur`)
- Bubble-phase dispatch with `preventDefault()`, `stopPropagation()`, and `stopImmediatePropagation()`
- Hover tracking, active/pressed state tracking, and double-click detection
- Global key interceptor support at the document level

Current limitation:

- The capture phase is reserved in the event model and the `capture` listener option is accepted, but capture-phase dispatch is not yet implemented as a working public feature

## Popup menus and host callbacks

Relevant source:

- `glint_document.hpp`
- `glint_element.hpp`
- `glint_view_base.hpp`

Popup and host bridge features:

- Popup-menu requests from scene-graph elements through the host callback
- `IPopupMenu` integration points for host-owned menu presentation
- Popup selection round-trip back into the Glint element/document model

## Graphics, drawing, and text

Relevant source:

- `glint_graphics.hpp`
- `element/glint_element_render.hpp`
- `utils/glint_debug.hpp`

Graphics features:

- Skia-backed rendering primitives (`glint_canvas` wrapper plus direct `SkCanvas` access)
- Geometry types: `glint_rect`, `glint_color`, `glint_halign` / `glint_valign`
- Text drawing with font, alignment, and metrics support via `glint_text`
- Platform-native font managers: DirectWrite on Windows, CoreText / FontConfig on macOS
- Image, SVG, text blob, paint, path, and blend-mode support through the Glint graphics layer
- Native popup-menu wrappers (`glint_popup_menu`) used by `glint_select`
- Rounded corners, borders, shadows, and custom element drawing hooks
- Background fill, padded content area, and border-radius clipping in the standard render path

## Filters, masks, and shaders

Relevant source:

- `render/glint_filter.hpp`
- `render/glint_mask.hpp`
- `glint_mask.hpp`
- `element/glint_element_render.hpp`
- `shaders/glint_shader_base.hpp`
- `shaders/glint_shader_registry.hpp`
- `shaders/glint_bg_shader.hpp`
- `shaders/glint_backdrop_shader.hpp`
- `shaders/glint_shaders.hpp`

Visual effects features:

- CSS-compatible `filter` support
- Built-in filter functions including blur, brightness, contrast, saturate, grayscale, sepia, invert, opacity, hue-rotate, and drop-shadow
- Percentage and unit parsing on filter arguments (e.g. `grayscale(100%)`)
- CSS-compatible `mask` support with multi-layer comma lists
- Gradient masks (`linear-gradient`, `radial-gradient`, `conic-gradient`)
- URL-backed masks for element IDs, SVG files, SVG fragment IDs, and image files
- Per-layer mask properties: mode, position, size, repeat, origin, clip, and composite
- SkSL shader registry for named shader creation, parameter binding, and lifetime management
- `glint_shader_base` with typed parameter map (`float`, `SkV2`, `glint_color`), `animated` flag, and optional input hooks (e.g. `onMouseDown` for interactive shaders)
- Procedural background shader base class (`glint_bg_shader`)
- Backdrop shader base class (`glint_backdrop_shader`) with `sampleRadius()` for displacement bounds
- Shader dispatch through `shader(id, name)` tokens in `filter` and `backdrop-filter` parsing, including auto-creation when an element first references a registered shader name

## Images, SVG, and asset handling

Relevant source:

- `components/glint_image.hpp`
- `render/glint_svg_cache.hpp`
- `render/glint_resource_request.hpp`
- `render/glint_resource_cache.hpp`

Asset features:

- Image component support
- SVG DOM loading and caching
- SVG loading from files and in-memory data
- Intrinsic SVG size recovery when needed
- Resource request parsing with scheme, host, pathname, query, and hash fields
- Resource interception through `glint_document::onRequest`
- Response helpers for serving assets from disk, raw buffers, or `SkData`
- Disk fallback when no request handler is registered
- Resource cache support for images, SVG, and stylesheet loads

## Network-style resource logging

Relevant source:

- `utils/glint_network_log.hpp`
- `inspector/window.hpp`

Network/resource tooling features:

- Per-document request log for resource loads, capped at 500 entries with oldest-dropped overflow
- Thread-safe `push` / `snapshot` / `clear` API
- Tracking of URL, pathname, type, status code and message, handled flag, byte size, source element identity (numeric id, tag, type, DOM id), and timestamp
- Inspector integration modeled after a DevTools-style Network tab

## Inspector and debugging tools

Relevant source:

- `inspector/glint_inspector_support.hpp`
- `inspector/window.hpp`
- `inspector/style_editor.hpp`
- `inspector/image_preview_popup.hpp`
- `inspector/glint_attributes_list.hpp`
- `utils/glint_debug.hpp`

Tooling features:

- Optional Glint inspector window on Win32 (no-op stubs on non-Win32)
- Per-document open / close / `isOpen` API; each `glint_document` gets an independent inspector instance
- Inspector runs its own message loop on a background thread, with bus-driven invalidation via `glint_tree_changed_event` and `glint_node_style_changed_event`
- Style editor with matched-rule listing and `glint_style_set_by_name` / `glint_style_is_valid_by_name` hooks
- Attribute list panel for DOM-style element properties
- Image preview popup for `glint_image` hover thumbnails
- DevTools-style matched CSS rule inspection
- Network-log inspection support
- Element-picker style inspect mode in the inspector
- Runtime debug border colorization (`glint_debug::colorizedBorders`)

## Built-in components

Relevant source:

- `components/glint_builder.hpp` (`glint_div`, builder helpers)
- `components/glint_button.hpp`
- `components/glint_checkbox.hpp`
- `components/glint_colorpicker.hpp`
- `components/glint_colorpicker_window.hpp`
- `components/glint_dial.hpp`
- `components/glint_gradient_editor.hpp`
- `components/glint_image.hpp`
- `components/glint_input.hpp`
- `components/glint_scrollbar/glint_scrollbar.hpp`
- `components/glint_select.hpp`
- `components/glint_text_editor_base.hpp`
- `components/glint_tree.hpp`
- `components/glint_list/`

Shipped component surface:

| Component | Notes |
| --- | --- |
| `glint_div` | Generic flex/block container with optional inline custom-draw lambda. |
| `glint_button` | Clickable button with animated normal / hover / pressed state transitions. |
| `glint_checkbox` | Labeled checkbox with optional radio-style mode (`keepChecked`). |
| `glint_input` | Single-line text entry; supports `text`, `number`, `password`, `email` types, placeholder, `min` / `max`, `readonly`, `disabled`, and horizontal scroll. |
| `glint_select` | Dropdown menu backed by a native OS popup menu, with hover/pressed animation. |
| `glint_image` | Bitmap component with CSS `object-fit` (`contain`, `cover`, `fill`, `none`) and `object-position` support, file and request-handler loading paths. |
| `glint_list` / `glint_list_item` | Scrollable item list with header/footer slots, `userData` (`std::any`), selection styling, and selection callbacks. |
| `glint_tree` | Collapsible tree view with DFS traversal, indentation, and expand/collapse callbacks. |
| `glint_scrollbar` | Vertical or horizontal scrollbar, auto-created for overflowing containers. |
| `glint_colorpicker` | Collapsed/expanded color picker with SV canvas, hue strip, alpha strip, and hex input. |
| `glint_colorpicker_window` | Standalone Win32 popup window wrapper around the color picker. |
| `glint_dial` | Circular angle dial with CSS-compatible angle semantics and drag input. |
| `glint_gradient_editor` | Gradient ramp with draggable color stops, double-click delete, and color-picker popup. |
| `glint_text_editor_base` | Reusable text-editor base with UTF-8 buffer, cursor, selection, clipboard support (Ctrl+A/C/V/X/Z), and caret blink. |

All built-in components inherit the standard `glint_element` / `glint_html_element` surface: full `glint_style` access, event listeners, `id` / `className` / `classList` / `innerText`, transition animations, scroll support, and filter / mask / shader integration.

## Practical limits today

Relevant source:

- `glint_window.hpp`
- `glint_view.hpp`
- `platform/win32/glint_renderer_backend_win32.hpp`
- `adapters/sk/`

Current limits to keep in mind:

- Win32 is the actively used host; macOS host code exists but should be treated as in progress, and Linux is not yet implemented
- The runtime rendering backends shipped today are CPU, OpenGL, D3D12, and Dawn on Win32 plus Metal on macOS; `D3D11` and `Vulkan` are reserved enum values rather than working backends
- Capture-phase event dispatch is reserved but not yet implemented
- The CSS DOM adapter currently exposes only `id` and `class` to attribute selectors; sibling combinators (`+`, `~`) and `:has()` are parsed but matched in a narrower form than the browser; `:checked`, `:disabled`, and `:enabled` are not surfaced as generic state pseudo-classes
- The `transform` public surface is the `translate` subset; `flex` shorthand currently maps only the grow component
- The `adapters/sk/` raw-Skia bridge is a stub with several `TODO` markers and is not yet wired as a usable adapter
- If you stay on `glint_window` or `glint_view`, the stock host code handles the normal GPU/CPU fallback path for you
- If you bypass those APIs and build directly on lower-level host plumbing, GPU activation and fallback behavior become your responsibility