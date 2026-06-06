<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/DisplayXR/displayxr-runtime/main/doc/displayxr_white.png" width="100">
  <source media="(prefers-color-scheme: light)" srcset="https://raw.githubusercontent.com/DisplayXR/displayxr-runtime/main/doc/displayxr.png" width="100">
  <img alt="DisplayXR" src="https://raw.githubusercontent.com/DisplayXR/displayxr-runtime/main/doc/displayxr.png" width="100">
</picture>

# displayxr-common

Shared native helper code for the [DisplayXR](https://github.com/DisplayXR/displayxr-runtime) ecosystem — the **canonical, versioned home** for the off-axis Kooima projection math and the C++ app scaffolding that were previously copy-vendored across the runtime test apps, the demos, and the Unreal/Unity plug-ins ([#396](https://github.com/DisplayXR/displayxr-runtime/issues/396)).

It exposes two CMake targets:

- **`displayxr::math`** — pure-C off-axis Kooima projection (display-centric + camera-centric rigs, FOV + matrices). Implements the [Kooima algorithm](http://csc.lsu.edu/~kooima/articles/genperspective/) — perspective-correct multiview 3D for physical displays with eye tracking. Linked by **everything**, including the Unity/Unreal engine plug-ins.
- **`displayxr::common`** — C++17 app scaffolding (depends on `displayxr::math`): logging, Win32 input + window management, OpenXR session/swapchain/frame lifecycle, D2D/DirectWrite text + HUD rendering, the D3D11 reference renderer, window-space-layer UI helpers, the thin app-side atlas-capture helper, and the vendored stb image headers. Linked by **C++ apps only** (runtime test apps, standalone demos) — not by engines.

## What It Does

Given a viewer's eye positions and a physical display's dimensions, this library computes **asymmetric frustum projection matrices** that produce geometrically correct 3D. Each eye gets a different frustum that accounts for its off-axis position relative to the display surface.

```
         Eye (tracked)
          \
           \  ← asymmetric frustum
            \
    ┌────────┴────────┐
    │  Physical Display │   ← known size in meters
    └─────────────────────┘
```

This is fundamentally different from symmetric projection (which assumes the viewer is centered) and is required for autostereoscopic/lenticular 3D displays.

## Two Pipelines

### Display-Centric (`display3d_view.*`)

The physical display is the reference frame. The library maps the display into your app's virtual world and computes view + projection matrices from each eye's position relative to the screen surface.

**Use when:** Your app has a concept of a "virtual display" — the screen is a window into a 3D world, and the viewer looks through it.

```c
Display3DTunables tunables = {
    .ipd_factor = 1.0f,              // full stereo separation
    .parallax_factor = 1.0f,         // full eye tracking
    .perspective_factor = 1.0f,      // 1:1 perspective
    .virtual_display_height = 0.5f   // display is 0.5 app-units tall
};

Display3DScreen screen = { .width_m = 0.344f, .height_m = 0.194f };

Display3DView views[2];
display3d_compute_views(
    eye_positions, 2,   // N eye positions from tracker
    &nominal_viewer,    // default viewer position
    &screen,            // physical display dimensions
    &tunables,
    &display_pose,      // display position/orientation in world
    near_offset,        // ZDP-anchored clip: near = ez - near_offset (in vH units)
    far_offset,         //                    far  = ez + far_offset (0 => far at ZDP)
    0,                  // vulkan_flip_y: 0 = clean +Y-up world frame; 1 = mirror for Vulkan render
    views               // output: view + projection matrices, resolved near_z/far_z, per eye
);
```

### Camera-Centric (`camera3d_view.*`)

The app defines a virtual camera (position, orientation, vFOV) and eye tracking produces per-eye asymmetric views around that camera. The convergence distance controls where left/right views overlap.

**Use when:** Your app has a traditional camera (FPS game, 3D viewer) and you want to add eye-tracked stereo without rethinking your rendering pipeline.

```c
Camera3DTunables tunables = {
    .ipd_factor = 1.0f,
    .parallax_factor = 1.0f,
    .inv_convergence_distance = 2.0f,  // converge at 0.5m
    .half_tan_vfov = 0.577f            // 60 degree vFOV
};

Camera3DView views[2];
camera3d_compute_views(
    eye_positions, 2,
    &nominal_viewer,
    &screen,
    &tunables,
    &camera_pose,       // camera position/orientation in world
    near, far,
    views
);
```

## Eye Factor Processing

Both pipelines share a common first stage that processes raw eye positions:

1. **IPD Factor** (`ipd_factor`, 0-1) — scales the inter-eye distance around the midpoint. 0 = mono (both eyes at center), 1 = full physical IPD.

2. **Parallax Factor** (`parallax_factor`, 0-1) — lerps the eye center toward the nominal (default) viewer position. 0 = no head tracking (fixed viewpoint), 1 = full tracking.

These factors are applied before the view/projection computation, allowing smooth transitions between mono/stereo and tracked/untracked modes.

## Files

| File | Description |
|------|-------------|
| `include/display3d_view.h` | Display-centric API — structs + declarations (incl. `display3d_compute_center_view`, `display3d_selftest`) |
| `include/display3d_view.c` | Display-centric implementation |
| `include/camera3d_view.h` | Camera-centric API — structs + declarations |
| `include/camera3d_view.c` | Camera-centric implementation |
| `include/projection_depth.h` | `convert_projection_gl_to_zero_to_one()` — remap the GL `[-1,1]` clip-depth to `[0,1]` for D3D/Vulkan/Metal consumers (GL needs none) |

> `leia_math.h` (a Windows-only, DirectXMath/C++ Leia SDK reference example) is intentionally **not** part of `displayxr::math` — it isn't portable pure-C math and stays app-local.

## `displayxr::common` — C++ app scaffolding

The `common/` directory is the lib's second target (epic #396 W4, re-scoped [#393](https://github.com/DisplayXR/displayxr-runtime/issues/393)): the application plumbing that was copy-vendored (and drifting) across the runtime's `test_apps/common/` and both standalone demos. **Single target, platform-gated sources** — Windows consumers get the full stack; Apple consumers get the cross-platform subset plus the macOS HUD/capture helpers.

| Component | Files | Platform |
|-----------|-------|----------|
| Logging (`%LOCALAPPDATA%\DisplayXR\<app>` file log) | `logging.{h,cpp}` | Windows |
| Win32 input (WASD/mouse/mode keys → request flags) | `input_handler.{h,cpp}` | Windows |
| Win32 window + monitor management | `window_manager.{h,cpp}` | Windows |
| OpenXR session/swapchain/frame lifecycle (`XrSessionManager`) | `xr_session_common.{h,cpp}` | Windows |
| D2D/DirectWrite text + buttons | `text_overlay.{h,cpp}` | Windows |
| CPU-readable HUD (side D3D11 device — usable from GL/VK/D3D12 apps too) | `hud_renderer.{h,cpp}` | Windows |
| CoreText/CoreGraphics HUD | `hud_renderer_macos.{h,mm}` | macOS |
| D3D11 reference renderer (cube/grid/textures, mip chains) | `d3d11_renderer.{h,cpp}`, `mip_chain.h` | Windows |
| Window-space-layer UI helpers (pure OpenXR) | `xr_window_space_hud.{h,cpp}` | both |
| App-side atlas-capture helper (filename numbering, flash overlay, `RequestRuntimeAtlasCapture`) | `atlas_capture.{h,cpp}`, `atlas_capture_macos.mm` | both |
| View parameter struct | `view_params.h` | both |
| stb image (read+write headers + the **only** implementation TUs) | `stb_image*.h`, `stb_image_impl_macos.cpp` | both |
| dGPU hint (`NvOptimusEnablement`, force-included into consumer EXEs) | `optimus_dgpu_hint.c` | Windows |
| Workspace-manifest CMake helper (`displayxr_install_manifest()`) | `displayxr_manifest.cmake` | both |

**Divergence policy:** behavior differences between consumers are parameterized at the call site (e.g. `InputState::hudToggleRequiresShift`, `EndFrame(..., projectionLayerFlags)`) — never `#ifdef APP` in the lib. Request-flag fields that only one app consumes (file picker, clip playback, transparency toggle) are fine: unconsumed flags are inert.

**stb ownership:** the lib owns exactly one `STB_IMAGE_IMPLEMENTATION` and one `STB_IMAGE_WRITE_IMPLEMENTATION` TU per platform (Windows: `d3d11_renderer.cpp` / `atlas_capture.cpp`; Apple: `stb_image_impl_macos.cpp` / `atlas_capture_macos.mm`). Consumers must not define them.

### DisplayXR extension headers

`xr_session_common` / `xr_window_space_hud` need the DisplayXR OpenXR extension headers (`XR_EXT_display_info.h`, `XR_EXT_win32_window_binding.h`, `XR_EXT_workspace_file_dialog.h`, `XR_EXT_atlas_capture.h`, `XR_EXT_mcp_tools.h`, …), which are **not** in the Khronos SDK. Set `DISPLAYXR_EXTENSIONS_INCLUDE_DIR` (the directory **containing** `openxr/XR_EXT_*.h`) before bringing in this project:

- runtime test apps: `src/external/openxr_includes` (the source of truth)
- demos: their vendored `openxr_includes/` (keep it refreshed from [displayxr-extensions](https://github.com/DisplayXR/displayxr-extensions))

When unset (this repo's own CI), the build fetches `displayxr-extensions` at a pinned commit. When the consumer's dir also carries the full Khronos set (every current consumer's does), it wins the include order, so lib TUs and app TUs compile against the same `openxr.h`.

## Integration

Consume via CMake `FetchContent`, pinned to a tag — the same pattern the DisplayXR runtime uses for its other deps. Don't vendor a copy (that's exactly the drift this repo exists to kill).

```cmake
include(FetchContent)
# For displayxr::common only — the dir containing openxr/XR_EXT_*.h:
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/openxr_includes" CACHE PATH "")
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v0.3.0
)
FetchContent_MakeAvailable(displayxr_common)

target_link_libraries(your_engine_plugin PRIVATE displayxr::math)   # engines
target_link_libraries(your_app           PRIVATE displayxr::common) # C++ apps (math comes transitively)
```

For local co-development against a checkout, point CMake at it:
`-DFETCHCONTENT_SOURCE_DIR_DISPLAYXR_COMMON=../displayxr-common`.

**Dependency:** `displayxr::math` is typed in OpenXR types (`XrVector3f`/`XrPosef`/`XrFovf`), so it links `OpenXR::headers` (headers only — the build above fetches OpenXR-SDK with the loader disabled). Every real consumer already builds against OpenXR.

**Matrix convention:** output matrices are **column-major** with **OpenGL `[-1,1]` clip-depth**. DirectX callers transpose into row-major `XMMATRIX`; D3D/Vulkan/Metal callers also remap depth to `[0,1]` via `convert_projection_gl_to_zero_to_one()` (`include/projection_depth.h`) — applied to the per-view `projection_matrix`. GL needs no remap.

## N-View Multiview

Both APIs accept an array of N eye positions and produce N views. This works for stereo (2 views), quad (4 views), or any N-view light field display:

```c
XrVector3f eyes[4] = { ... };  // 4 eye positions from tracker
Display3DView views[4];
display3d_compute_views(eyes, 4, &nominal, &screen, &tunables, &pose, near_offset, far_offset, 0, views);
// views[0..3] each have their own view_matrix + projection_matrix
```

## Documentation

- [Math Reference](docs-math-reference.md) — full pipeline derivation with diagrams
- [DisplayXR Runtime](https://github.com/DisplayXR/displayxr-runtime) — the OpenXR runtime that uses this library
- [Kooima's Original Paper](http://csc.lsu.edu/~kooima/articles/genperspective/) — the foundational algorithm

## Roadmap

Epic [#396](https://github.com/DisplayXR/displayxr-runtime/issues/396) status:

- **W2** (`v0.1.0`) ✅ — math-only first cut.
- **W3** (`v0.2.0`) ✅ — all 5 consumers (runtime test apps, both demos, Unreal, Unity) pin `displayxr::math` by tag; Layer 1 window/canvas resolve added.
- **W4** (`v0.3.0`) ✅ — `displayxr::common` C++ scaffolding target; the 3 C++ consumers migrate off their vendored `common/` copies.
- **Deferred** — a **FOV-only, type-neutral** layer so the runtime's `m_camera3d_view`/`m_display3d_view`/`m_multiview` ports (xrt-typed, no matrices) can share this exact source instead of being hand-synced; this would also drop the OpenXR-header dependency from the core.

## License

[ISC License](LICENSE) — same as the DisplayXR runtime.

## Source of truth

This repository is the **canonical home** for the shared math and scaffolding. The `display3d_view.*` / `camera3d_view.*` were reconciled from `displayxr-runtime`'s `test_apps/common/` (the most-advanced superset, with ZDP-anchored clip, `near_z/far_z` outputs, `vulkan_flip_y`, `center_view`, and `selftest`); `common/` was reconciled as the superset of the runtime test apps + both demos' forks (file-picker state machine and window-space-layer generics from the model-viewer fork, ADR-021/#441/#425/#457 features from the runtime). Both now lead: consumers pin a tag; changes land here first.
