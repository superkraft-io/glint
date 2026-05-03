# Glint Bundling

Glint's bundler converts a directory of web assets (CSS, images, fonts, etc.) into C++ headers that are compiled directly into the binary. At runtime, `glint_document::onRequest` intercepts resource requests and serves them from memory instead of disk.

## Bundle modes

| Mode | Symbol | How data is stored | Typical use |
|---|---|---|---|
| 💽 No bundle | _(none)_ | Files read from disk at runtime | Fast iteration — no bundler step needed |
| 📦 Shallow | `GLINT_BUNDLE_SHALLOW` | Headers contain metadata (offsets/sizes) only; raw bytes live in `.bin` files next to the headers and are loaded on first access | Debug builds that need the bundle code path exercised |
| 🧱 Deep | `GLINT_BUNDLE_DEEP` | All bytes are embedded as `static` arrays in the headers | Release builds — fully self-contained binary |

## Running the bundler

```sh
node third_party/glint/scripts/bundler/glint_bundler.mjs \
  --input  <web-assets-dir> \
  --output <generated-headers-dir> \
  --mode   deep|shallow \
  [--namespace <cpp-namespace>]   # default: glint_bundle
```

- `--input` — the root of your web assets tree (e.g. `glint_user_code/web`)
- `--output` — where the generated headers will be written (add this dir to your CMake include paths)
- `--mode` — `shallow` for debug, `deep` for release
- `--namespace` — C++ namespace for all generated types (default `glint_bundle`)

The bundler cleans `--output` completely on every run, then writes:

```
<output>/
  glint_bundle_library.hpp        ← top-level include
  glint_bundle_include.hpp
  glint_bundle_groups.hpp
  glint_bundle_group_root.hpp
  glint_bundle_entry.hpp
  glint_bundle_entries_files.hpp
  glint_bundle_entries_folders.hpp
  deep/
    groups/
      <N>.hpp                     ← one per group, bytes embedded as static arrays
  shallow/
    groups/
      <N>.hpp                     ← metadata only
      data/
        <N>.bin                   ← raw bytes, loaded from disk on first access
```

The bundler also checks for locked files before running and exits with an error if any source asset is in use.

## CMake integration

Add the compile definition and include path for the chosen mode:

```cmake
# Shallow (debug)
target_compile_definitions(my_app PRIVATE GLINT_BUNDLE_SHALLOW)
target_include_directories(my_app PRIVATE <output-dir>)

# Deep (release)
target_compile_definitions(my_app PRIVATE GLINT_BUNDLE_DEEP)
target_include_directories(my_app PRIVATE <output-dir>)
```

No definition = no bundling; `onRequest` falls through to disk.

## Runtime integration

Include the generated top-level header and hook `glint_document::onRequest`:

```cpp
#include "glint_bundle_library.hpp"

glint_bundle::glint_bundle_library bundle;

doc.onRequest = [&](glint_resource_request& req) {
    bundle.dispatch(req);   // serves from bundle; no-ops if path not found
};
```

`dispatch()` calls `req.fromBuffer(data, size)` internally when it finds a match. If the path is not in the bundle the request falls through to glint's normal disk fallback.

For shallow builds, `dispatch()` lazily loads the `.bin` file for the relevant group on first access; subsequent requests for files in the same group reuse the already-loaded data.

## Binary output path

The bundler is a **pre-build code generation step**. All three bundle modes produce the same binary path — only the compiled-in data (or lack thereof) differs. Separate CMake `binaryDir` values per bundle mode are not needed.

## Build workflow summary

| Mode | Steps |
|---|---|
| 💽 No bundle | CMake build only |
| 📦 Shallow | Run bundler (`--mode shallow`) → CMake build |
| 🧱 Deep | Run bundler (`--mode deep`) → CMake build |
