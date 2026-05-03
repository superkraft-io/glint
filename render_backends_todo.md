# Render Backends Todo

## Goals

- [x] Make render backend selection a first-class part of Glint's embedded-view architecture.
- [x] Keep `glint_document` backend-agnostic.
- [x] Keep backend selection and GPU/CPU lifecycle inside the platform view host.
- [ ] Allow the same public `glint_view` API to run on CPU, OpenGL, Direct3D, or Vulkan.

## Public API Shape

- [x] Add a public backend enum.
- [x] Evaluate a shape like:

```cpp
enum class glint_backend {
  Auto,
  CPU,
  OpenGL,
  D3D11,
  D3D12,
  Vulkan
};
```

- [x] Add backend selection to `glint_view` creation options.
- [x] Support a `preferredBackend` or equivalent field in the public options struct.
- [x] Support automatic fallback when the preferred backend is unavailable or fails to initialize.
- [x] Expose the active backend at runtime through `glint_view`.
- [x] Expose whether the active backend is GPU or CPU.

## Internal Abstraction

- [x] Introduce a Win32 renderer-backend interface that can be shared by Win32 Glint hosts.
- [x] Reuse the same renderer-backend interface in `glint_window_win32`.
- [x] Evaluate a shape like:

```cpp
class glint_renderer_backend_win32 {
public:
  virtual ~glint_renderer_backend_win32() = default;
  virtual bool initialize(HWND hwnd) = 0;
  virtual void shutdown() = 0;
  virtual bool resize(int width, int height) = 0;
  virtual SkCanvas* beginFrame() = 0;
  virtual void endFrame() = 0;
  virtual void present() = 0;
  virtual glint_backend backend() const = 0;
  virtual bool isGpu() const = 0;
};
```

- [x] Keep window creation, input routing, popup menus, and document ownership in `glint_view_win32`.
- [x] Keep rendering implementation details inside backend-specific classes.
- [x] Keep backend-specific native requirements isolated to the relevant backend implementation.

## Win32 Backend Implementations

### CPU

- [x] Extract the current CPU bitmap path into a dedicated CPU backend.
- [x] Move `SkBitmap` + `SkCanvas` CPU surface creation into the CPU backend.
- [x] Move `StretchDIBits` presentation into the CPU backend.
- [x] Ensure the CPU backend remains the universal fallback path.

### OpenGL

- [x] Extract the current WGL/OpenGL path into a dedicated OpenGL backend.
- [ ] Keep `CS_OWNDC` or equivalent OpenGL-specific requirements isolated to the OpenGL backend path.
- [x] Move WGL context creation into the OpenGL backend.
- [x] Move `GrDirectContext` creation for the GL interface into the OpenGL backend.
- [x] Move GPU `SkSurface` creation and resize handling into the OpenGL backend.
- [x] Move `SwapBuffers` presentation into the OpenGL backend.

### Direct3D

- [x] Decide whether the first DirectX backend should be D3D11 or D3D12.
- [x] Research the exact Skia backend support and build requirements for the chosen DirectX path.
- [x] Add a dedicated DirectX backend implementation instead of mixing DirectX logic into the OpenGL backend.
- [x] Ensure swapchain creation, resize, and present are encapsulated inside the DirectX backend.
- [ ] Rebuild the vendored Skia bundle with `skia_use_direct3d = true` so the new backend can be enabled in consuming builds.

### Vulkan

- [ ] Research the exact Skia Vulkan backend requirements for Win32.
- [ ] Add a dedicated Vulkan backend implementation.
- [ ] Ensure device, surface, swapchain, resize, and present logic are encapsulated inside the Vulkan backend.

## Backend Selection Policy

- [ ] Define which backend order `Auto` should prefer on Win32.
- [ ] Decide whether `Auto` should prefer:
- [ ] Direct3D first
- [ ] Vulkan second
- [ ] OpenGL third
- [ ] CPU fallback last
- [ ] Make the fallback order explicit and testable.
- [x] Log the reason when the preferred backend cannot be initialized and Glint falls back.

## Extraction Plan

- [x] Start by proving the abstraction with two backends only:
- [x] CPU
- [x] OpenGL
- [x] Do not add DirectX or Vulkan until the backend interface is validated by CPU and OpenGL.
- [x] Extract rendering logic out of the current Win32 host in small steps.
- [x] Keep behavior unchanged while moving code into backend classes.

## Validation and Tooling

- [x] Add runtime logging for backend selection.
- [x] Log the requested backend.
- [x] Log the active backend.
- [x] Log fallback reasons.
- [x] Log whether the active backend is GPU or CPU.
- [ ] Add lightweight per-frame timing hooks that work across all backend types.
- [ ] Expose enough telemetry to separate draw cost from present cost.

## Build System

- [ ] Add compile-time backend capability flags.
- [ ] Evaluate flags such as:
- [ ] `GLINT_ENABLE_OPENGL`
- [ ] `GLINT_ENABLE_D3D11`
- [x] `GLINT_ENABLE_D3D12`
- [ ] `GLINT_ENABLE_VULKAN`
- [ ] Keep `GLINT_RENDER_GPU` as a higher-level GPU capability switch only if it still adds value after backend-specific flags exist.
- [ ] Make sure unsupported backends are unavailable at runtime when they are not compiled in.

## Documentation After Implementation

- [ ] Document the backend enum and selection policy after the implementation is complete.
- [ ] Document the default backend order for `Auto`.
- [ ] Document which backends are currently implemented versus planned.
- [ ] Document how to inspect the active backend at runtime.