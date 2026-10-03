# Getting Started Guide

This guide shows the supported way to start a new native Win32 Glint project.

If you stay on the stock host APIs:

- `glint_window` for a standalone top-level app window
- `glint_view` / `glint::createView(...)` for an embedded child view

then getting started is straightforward, including GPU-capable rendering.

The main caveat is that `GLINT_RENDER_GPU=ON` compiles the GPU path in, but runtime can still fall back to CPU if GPU initialization or surface creation fails.

macOS host code lives under `platform/mac/` and integrates with the Metal backend, but should be treated as in progress; this walkthrough covers the actively used Win32 path.

## 1. Install prerequisites

Use this setup on Windows:

```text
Visual Studio 2022 with MSVC
CMake 3.25+
Node.js
Git
```

To build Skia from source (section 4, option B) you also need:

```text
Visual Studio component "C++ Clang Compiler for Windows" (clang-cl)
Python 3
```

Add the Clang component in the Visual Studio Installer under Modify → Individual components, or install LLVM separately. Your app itself still builds with MSVC; only Skia needs clang-cl (see section 4).

## 2. Create a new project layout

Start with a simple layout like this:

```text
my_glint_app/
  CMakeLists.txt
  src/
    main.cpp
    app_window.hpp
    app_window.cpp
  third_party/
    glint/
```

## 3. Vendor Glint into the project

Copy or add the Glint tree under:

```text
my_glint_app/third_party/glint
```

## 4. Prepare the Skia bundle

By default, Glint expects:

```text
my_glint_app/third_party/skia
```

### Option A — Prebuilt libraries (recommended, fast)

For the quickest setup, download prebuilt Skia libraries. No Python, no lengthy compile.

```powershell
# Direct3D 12 (GPU, Graphite backend, Windows only)
node .\third_party\glint\scripts\init_skia.mjs --prebuilt --backend d3d12
```

### Option B — Build from source

If you need a custom configuration or a backend not covered by the prebuilt packages, build Skia from source. This takes significantly longer but gives you full control.

```powershell
# CPU (default — software rasterizer)
node .\third_party\glint\scripts\init_skia.mjs --source --config Both

# OpenGL (GPU, Ganesh backend)
node .\third_party\glint\scripts\init_skia.mjs --source --config Both --backend opengl

# Direct3D 12 (GPU, Graphite backend, Windows only)
node .\third_party\glint\scripts\init_skia.mjs --source --config Both --backend d3d12
```

On Windows, the source build uses **clang-cl**, found automatically from your Visual Studio installation or a standalone LLVM install (or set `CLANG_WIN` to an LLVM folder that contains `bin\clang-cl.exe`). Skia's CPU renderer relies on Clang's vector extensions; built with MSVC it falls back to scalar code, and text, anti-aliased shapes and shadows draw **10–40× slower**. This matters even for GPU apps, because Glint falls back to CPU rendering when the GPU can't be used (Remote Desktop, virtual machines, broken drivers). The script stops with installation instructions when clang-cl is missing; there is no MSVC fallback. Debug Skia libraries are built optimized too (with the debug runtime and Skia's debug checks), so Debug builds of your app draw at nearly Release speed. CMake refuses Windows Skia libraries that weren't built with clang-cl, and `--prebuilt` refuses a package that wasn't.

Useful options:

```powershell
# Reuse an existing Skia checkout (deps already synced) instead of cloning one
node .\third_party\glint\scripts\init_skia.mjs --source --backend d3d12 --skia-src D:\skia --skip-sync
```

The script records the compiler in `third_party/skia/win/<arch>/<config>/glint_skia_build.json`. When you configure, CMake prints which compiler built the Skia libraries, and warns if it was MSVC.

In both cases, the script generates `third_party/glint/glint_render_backend.h`, which is included automatically by `glint.hpp` and activates the correct compile-time paths. No CMake flags are needed.

That should produce:

```text
third_party/skia/
  src/skia/
  win/x64/Release/
  win/x64/Debug/
  win/bin/
```

If you already have a compatible Skia bundle somewhere else, you can point CMake at it with `GLINT_DEPS_DIR`.

## 5. Write the root CMakeLists.txt

Use this minimal standalone setup:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_glint_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/glint" "${CMAKE_CURRENT_BINARY_DIR}/glint")

add_executable(my_glint_app WIN32
  src/main.cpp
  src/app_window.cpp
  src/app_window.hpp
)

target_link_libraries(my_glint_app PRIVATE glint::host_win32)
```

## 6. Add a minimal top-level Glint window

Create `src/app_window.hpp`:

```cpp
#pragma once

#include "glint/glint_window.hpp"

class AppWindow final : public glint_window
{
public:
  static void open();
  static bool isOpen();

protected:
  const wchar_t* windowClassName() const override { return L"my_glint_app"; }
  const wchar_t* windowTitle() const override { return L"My Glint App"; }
  void buildUI() override;
  void onThreadEnded() override;

private:
  AppWindow() = default;
  static AppWindow* sInstance;
};
```

Create `src/app_window.cpp`:

```cpp
#include "app_window.hpp"

#include "glint/glint_standalone.hpp"

AppWindow* AppWindow::sInstance = nullptr;

void AppWindow::open()
{
  if (sInstance && sInstance->isRunning())
    return;

  if (!sInstance)
    sInstance = new AppWindow();

  sInstance->startThread();
}

bool AppWindow::isOpen()
{
  return sInstance && sInstance->isRunning();
}

void AppWindow::buildUI()
{
  mOwnRoot->mCanvas.style.backgroundColor = "#101010";

  mOwnRoot->add.div([](glint_component_style& root) {
    root.style.width = "100%";
    root.style.height = "100%";
    root.style.display = "flex";
    root.style.alignItems = "center";
    root.style.justifyContent = "center";
    root.style.color = "white";
    root.innerText = "Hello from Glint";
  });
}

void AppWindow::onThreadEnded()
{
  sInstance = nullptr;
}
```

Create `src/main.cpp`:

```cpp
#include "app_window.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  AppWindow::open();

  while (AppWindow::isOpen())
    ::Sleep(16);

  return 0;
}
```

## 7. Configure the build

From the project root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

If your Skia bundle is not in `third_party/skia`, use:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -D GLINT_DEPS_DIR=C:\path\to\third_party\skia
```

No GPU flags are needed — the render backend was baked in by `init_skia.mjs` via `glint_render_backend.h`.

## 8. Build the project

Use Release for real runtime behavior:

```powershell
cmake --build build --config Release
```

## 9. Run it with runtime verification enabled

Before launching the app:

```powershell
$env:GLINT_ENABLE_RUNTIME_LOG='1'
$env:GLINT_ENABLE_TELEMETRY='1'
.\build\Release\my_glint_app.exe
```

## 10. Check that GPU actually activated

Look at:

```text
%TEMP%/glint_runtime.log
```

For a top-level app window, good signs look like:

```text
GLINT WINDOW: requested backend = OpenGL
GLINT WINDOW: GrDirectContext created
GLINT WINDOW: GPU surface created (OpenGL)
GLINT WINDOW: active backend = OpenGL (GPU)
```

For the Direct3D backend, the equivalent success path is:

```text
GLINT WINDOW: requested backend = D3D12
GLINT WINDOW: GrDirectContext created
GLINT WINDOW: GPU surface created (D3D12)
GLINT WINDOW: active backend = D3D12 (GPU)
```

If telemetry is enabled, you should also see timing lines.

### Ship compiled shaders (Direct3D)

The first time a page draws an effect it hasn't drawn before (a blur, a blend mode, a gradient, a custom shader), Skia generates a GPU shader for it and Windows compiles it with `D3DCompile`, which takes 3–10 ms per shader. A page with new effects can pause for 100–200 ms the first time it opens.

Glint caches compiled shaders so this happens at most once:

- **On disk:** every compiled shader is saved in `%LOCALAPPDATA%\Glint\ShaderCache\D3D`. Later launches on the same PC skip compiling.
- **Inside your app:** capture the shaders your app uses once, and embed them, so even a user's first launch doesn't compile:

  1. Run your app with `GLINT_D3D_SHADER_CAPTURE=<path>\glint_d3d_shaders.bin` and open every page (the demo's `demo/scripts/capture_d3d_shaders.ps1` does this automatically).
  2. Embed the pack in your executable:

     ```cmake
     glint_embed_d3d_shaders(my_app "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glint_d3d_shaders.bin")
     ```

  3. Capture again after updating Skia or changing what your pages draw. A shader missing from the pack still works: it compiles once and goes to the disk cache.

Nothing in Skia is modified: Glint intercepts `D3DCompile` in its own process and looks shaders up by their exact source text, so a stale pack can only make things slower, never wrong. `GLINT_D3D_SHADER_CACHE=0` turns the cache off.

> **Debugging tip:** when a debugger starts your app, Windows switches to a debug memory allocator that makes shader compilation (and other allocation-heavy work) up to 10× slower. Set `_NO_DEBUG_HEAP=1` in your launch configuration's environment.

## 11. Embed Glint into an existing Win32 app when needed

If you want to embed Glint instead of creating a standalone window, switch to `glint_view`.

Minimal embedded wrapper:

```cpp
#include "glint/glint_view.hpp"

class EmbeddedGlintView
{
public:
  bool open(HWND parent, int width, int height)
  {
    glint::glint_view_options options{};
    options.parent = parent;
    options.width = width;
    options.height = height;
    options.onDocumentCreated = [](glint_document& document) {
      document.loadStylesheet("/styles/main.css");
    };

    mView = glint::createView(options);
    return static_cast<bool>(mView);
  }

  void resize(int width, int height)
  {
    if (mView)
      mView->resize(width, height);
  }

private:
  std::unique_ptr<glint::glint_view> mView;
};
```

On the supported embedded Win32 path, Glint creates and owns the child `HWND`, sets up the device context, and manages redraw plus CPU/GPU fallback internally.

## 12. Keep the main caveat in mind

This is easy if you stay on:

```text
glint_window
glint_view / createView(...)
```

It is not equally easy if you bypass those and build directly on lower-level host plumbing such as `glint_window_base`. In that case, you are responsible for WGL, device-context ownership, surface lifecycle, and fallback behavior yourself.