<table>
	<tr>
		<td>
			<img src="assets/glint_logo.png" alt="glint logo" width="75" width="75">
		</td>
		<td valign="middle">
            <p style="font-size: 28px; margin-bottom: 0px;">Glint</p>
            <p>A web renderer and UI system for native C++ applications.</p>
		</td>
	</tr>
</table>

## What is Glint?


It brings web rendering, a DOM-like document tree, and CSS-style layout into a native C++ app while keeping the backend native: the C++ side owns resources, events, and integration points, while Glint handles layout, rendering, and host-side presentation.



## Features

- GPU-capable rendering on the current native host path
- DOM-like document tree exposed to the C++ backend
- CSS-style layout and styling, including flex layout, transitions, and `@keyframes` animations
- CSS `filter`, `backdrop-filter`, and `mask` support, plus an SkSL shader registry for custom procedural and backdrop shaders
- Built-in components: button, checkbox, input, select, image, list, tree, scrollbar, color picker, dial, gradient editor, and text editor base
- DOM-style event model with mouse, wheel, and keyboard events
- Resource request handler with image / SVG / stylesheet caching and a per-document network log
- Optional inspector window with style editor, attributes panel, network tab, and element picker
- Standalone top-level window hosting
- Embedded child-view hosting

For a full capability inventory, see [features.md](features.md).

## Start here

Read the full setup walkthrough in [guide.md](guide.md).

That guide covers:

- Starting a new Glint project
- Preparing the Skia bundle with the render backend of your choice
- Wiring CMake
- Validating runtime GPU activation
- Embedding Glint into an existing native parent window

## Rendering summary

- CPU raster is the default render path.
- The render backend is selected once during Skia setup by passing `--backend <name>` to `init_skia.mjs`. This generates `glint_render_backend.h`, which is included automatically and activates the correct compile-time paths.
- Available backends: `cpu` (default), `opengl`, `d3d12`, `dawn`, `metal`.
- The stock host integrations use `glint_backend::Auto`, which resolves to the compiled-in backend automatically. No app-side configuration is needed.
- The stock host integrations fall back to CPU automatically if GPU initialization or surface creation fails.
- The public `document()` API stays the same regardless of whether the active runtime path is CPU or GPU.

## Runtime verification

- Set `GLINT_ENABLE_RUNTIME_LOG=1` before launching the app
- Optionally set `GLINT_ENABLE_TELEMETRY=1` to add lightweight frame-time telemetry
- Inspect `%TEMP%/glint_runtime.log`

Typical successful runtime signals include:

```text
GLINT WINDOW: active backend = OpenGL (GPU)
GLINT VIEW: active backend = OpenGL (GPU)
```

For the full GPU checklist, example log lines, and troubleshooting flow, use [guide.md](guide.md).

## Notes

- The public API is intended to stay portable as host backends expand.
- Use `glint_window` for a standalone top-level app window.
- Use `glint_view` / `glint::createView(...)` for embedding into an existing native parent window.
- The supported embedded path is parent-handle-in, Glint-owned child-view-out.
- Use the stock host target exported by Glint for your integration.
- Main headers: `glint/glint_standalone.hpp`, `glint/glint_window.hpp`, and `glint/glint_view.hpp`.
- If you bypass the stock host APIs and build directly on lower-level host plumbing such as `glint_window_base`, GPU activation becomes your responsibility.
- Debug builds are not valid for performance evaluation; use `Release` when comparing CPU and GPU behavior.
