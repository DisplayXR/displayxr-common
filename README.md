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
| Viewer launch contract: `--transparent/--rect/--src/--vh/...` flags + the `displayxr-view:` URL, parsed and policy-checked (loopback-only http, no local files from a protocol launch) | `launch_args.h` | both (Win32 command-line entry) |
| Asset download to `%LOCALAPPDATA%\DisplayXR\<app>\cache\<sha1>.<ext>` (WinHTTP, size cap, no downgrade redirects, extension from path/Content-Type/magic) | `url_fetch.{h,cpp}` | Windows (pure helpers: both) |
| `displayxr-view:` protocol: per-user self-registration, sibling-viewer forward by `type=`, single-instance `WM_COPYDATA` hand-off | `view_protocol.h` | Windows |
| Rear-depth-budget clip policy: near/far/clipFar from `XrRearDepthBudgetDXR` (+ pre-extension fallback) | `clip_policy.h` | both |
| Content-bounds ROI for the rear-depth budget: project a world-space AABB to a canvas-normalised rect, rebase a zoned app's rect into window space, chain it as `XrContentBoundsDXR` | `content_bounds.h` | both |
| Content-MASK ROI for the rear-depth budget: any-coverage downsample of the app's own silhouette/alpha coverage onto the extension's cell grid (whole-window or zone-placed), union, cell count, chain it as `XrContentMaskDXR` | `content_mask.h` | both |

**Divergence policy:** behavior differences between consumers are parameterized at the call site (e.g. `InputState::hudToggleRequiresShift`, `EndFrame(..., projectionLayerFlags)`) — never `#ifdef APP` in the lib. Request-flag fields that only one app consumes (file picker, clip playback, transparency toggle) are fine: unconsumed flags are inert.

**stb ownership:** the lib owns exactly one `STB_IMAGE_IMPLEMENTATION` and one `STB_IMAGE_WRITE_IMPLEMENTATION` TU per platform (Windows: `d3d11_renderer.cpp` / `atlas_capture.cpp`; Apple: `stb_image_impl_macos.cpp` / `atlas_capture_macos.mm`). Consumers must not define them.

### The undock launch contract (`launch_args.h`, `url_fetch.h`, `view_protocol.h`)

A viewer that can float a 3D asset over the desktop (the model viewer, the splat viewer) is
started three ways - by a shell tile, by a web page through the OS protocol handler, or by a
native app "undocking" a part - and all three arrive as argv. These headers make every viewer
speak the same grammar and apply the same policy:

```
model_viewer.exe --transparent --rect=200,200,800,800 --vh=0.2 --src=https://host/x.glb
model_viewer.exe "displayxr-view://open?src=https%3A%2F%2Fhost%2Fx.glb&type=model&rect=200,200,800,800&vh=0.2&v=1"
```

- `dxr::ParseLaunchArgsFromCommandLine()` -> `LaunchArgs` (`ok()`, `errors`, `warnings`). The
  policy is keyed on `fromProtocol`: from a web page, `src` must be `https:` or loopback `http:`;
  `file:`/UNC/bare paths are refused (a native caller passes `--allow-local`, which a page cannot).
  A protocol launch is **transparent by default** (`transparent=0` opts out); on the CLI
  `--transparent` stays opt-in. `env=` carries the sender's lighting, `pose=YAW,PITCH[,ZOOM]` its
  opening orbit (SDK `setPose` convention) and `margin=` its fit fraction, so the undocked view
  opens looking like the tile it came from.
- `dxr::ForceInProcessRuntimeForUndock(args)` — call before `xrCreateInstance`. A protocol handler
  inherits the browser's environment, and the DisplayXR browser sets `XRT_FORCE_MODE=ipc` for
  itself; inherited, that makes the viewer an IPC client that is not the panel owner (flips to 2D
  whenever the browser holds the panel, no drag phase-snap). A protocol or `--transparent` launch
  pins `XRT_FORCE_MODE=native` and clears `DXR_IPC_FD` / `DISPLAYXR_WORKSPACE_SESSION`; a plain
  shell-tile launch is left alone. Pair it with `dxr::ReexecWithCleanRuntimeEnvIfNeeded(args)`
  (call first; exit if it returns true): an in-place override is not enough because the runtime
  DLL's dynamic CRT snapshots the environment at process start and its `getenv` wins, so a launch
  that inherited IPC routing re-launches itself once with a scrubbed block (`DXR_UNDOCK_REEXEC=1`
  guards the loop).
- `dxr::FetchUrlToCache()` downloads on a worker thread into a SHA-1-named cache file and reports
  progress for the viewer's toast; a cache hit never touches the network.
- `dxr::EnsureViewProtocolRegistered()` writes `HKCU\Software\Classes\displayxr-view` on launch
  (the installers run elevated, so an HKCU write there lands in the wrong hive);
  `FindSiblingViewer()` + `LaunchViewerWithUrl()` forward a URL whose `type=` belongs to another
  viewer; `AcquireSingleInstanceOrForward()` hands a second launch to the running instance.

The security negatives in `tests/launch_args_test.cpp` are the contract.

### DisplayXR extension headers

`xr_session_common` / `xr_window_space_hud` need the DisplayXR OpenXR extension headers (`XR_DXR_display_info.h`, `XR_DXR_win32_window_binding.h`, `XR_DXR_workspace_file_dialog.h`, `XR_DXR_atlas_capture.h`, `XR_DXR_mcp_tools.h`, …), which are **not** in the Khronos SDK. Set `DISPLAYXR_EXTENSIONS_INCLUDE_DIR` (the directory **containing** `openxr/XR_EXT_*.h`) before bringing in this project:

- runtime test apps: `src/external/openxr_includes` (the source of truth)
- demos: their vendored `openxr_includes/` (keep it refreshed from [displayxr-extensions](https://github.com/DisplayXR/displayxr-extensions))

When unset (this repo's own CI), the build fetches `displayxr-extensions` at a pinned commit. When the consumer's dir also carries the full Khronos set (every current consumer's does), it wins the include order, so lib TUs and app TUs compile against the same `openxr.h`.

> **`XR_DXR_depth_budget.h` (below) needs a newer pin.** It ships from
> `displayxr-runtime` PR [#1366](https://github.com/DisplayXR/displayxr-runtime/pull/1366) and auto-syncs to
> `displayxr-extensions` only once that PR merges to `main`. Until this repo's pinned
> `GIT_TAG` (the `displayxr_extensions_headers` `FetchContent_Declare` above) is bumped past that
> sync in a follow-up commit, this repo's own standalone CI build (and any consumer relying on the
> pinned fallback rather than `DISPLAYXR_EXTENSIONS_INCLUDE_DIR`) will fail to find the header.

### Rear-depth-budget clip policy (`clip_policy.h`)

`dxr::ResolveClipPlanes()` is the one place that turns the runtime's advisory `XR_DXR_depth_budget`
rear-depth budget (see runtime PR [#1366](https://github.com/DisplayXR/displayxr-runtime/pull/1366)) —
or its absence, on an older runtime / an app that hasn't opted in — into an eye's near/far
clip planes and the shader/rasterizer far-cull value — replacing the hand-rolled "clip the far plane
at the ZDP when transparent and standalone" block every transparent demo used to carry. Pure,
stateless, and does no smoothing of its own (the runtime already time-ramps the budget so the clip
plane glides, not pops):

```cpp
XrRearDepthBudgetDXR budgetStorage;
dxr::ChainRearDepthBudget(viewState, budgetStorage);   // before xrLocateViews, once per XrViewState
xrLocateViews(session, &locateInfo, &viewState, viewCount, &viewCountOutput, views);
const XrRearDepthBudgetDXR* budget =
    (budgetStorage.type == XR_TYPE_REAR_DEPTH_BUDGET_DXR) ? &budgetStorage : nullptr;

dxr::ClipPlanes clip = dxr::ResolveClipPlanes(ez, vHeight, budget, transparent, standalone);
// clip.near_z / clip.far_z feed the projection matrix; clip.clipFar (0 = no cull) feeds
// the shader/rasterizer far-cull the transparent demos already carry.
```

`budget == nullptr` — no `XR_DXR_depth_budget` support, the app didn't enable it, or the chained
struct came back untouched — reproduces today's rule bit-for-bit: `farOffsetVH = (transparent &&
standalone) ? 0 : 1000`. `dxr::RearDepthBudgetStateName()` gives HUD/log code a human-readable name
for `budget->state`.

### Content-bounds ROI (`content_bounds.h`, `XR_DXR_depth_budget` v2)

The rear-depth-budget analysis defaults to looking at the whole canvas, which can needlessly
close the budget over busy pixels the app's own content never sits behind (a window's menu bar,
a taskbar). `XrContentBoundsDXR` (SPEC_VERSION 2, chained on `XrFrameEndInfo::next` in `xrEndFrame`)
lets the app narrow the runtime's analysis to where its content actually projects.
`dxr::ProjectAabbToCanvasBounds()` does the geometry — project a world-space content AABB through
each eye's column-major view-projection matrix, union over eyes, and express the result as a
canvas-normalised rect (origin top-left, v down, same convention as `clip_policy.h`'s canvas
coordinates) — and `dxr::ChainContentBounds()` attaches it to the frame:

```cpp
const float* viewProj[2] = { leftViewProjColMajor, rightViewProjColMajor }; // column-major 4x4 each
XrRect2Df bounds{};
dxr::ProjectAabbToCanvasBounds(contentAabbMin, contentAabbMax, viewProj, 2, &bounds);

XrContentBoundsDXR contentBounds;
dxr::ChainContentBounds(frameEndInfo, contentBounds, bounds); // before xrEndFrame
xrEndFrame(session, &frameEndInfo);
```

`ProjectAabbToCanvasBounds` fails closed: any projected corner with `w <= 0` (behind the eye)
returns false and writes the whole canvas ({0,0,1,1}) rather than a partial or garbage rect; an
oversized AABB that exceeds the frustum still succeeds, clamped to `[0,1]`. If the pinned
extensions header predates the v2 struct bump, `content_bounds.h` defines an ABI-identical local
`XrContentBoundsDXR` (guarded by `DXR_CONTENT_BOUNDS_LOCAL_DEF`) so this API is usable against an
older pin; it compiles out once the pin advances.

**Zoned apps must rebase before chaining.** `XrContentBoundsDXR::bounds` wants window-client-
normalised space (the frame of the display processor's background preview). `ProjectAabbToCanvasBounds`
normalises to whatever view-proj it was handed — for a window-filling app that already IS the
window, but for an `XR_DXR_display_zones` app (e.g. the avatar layout: a 3D zone in the bottom
band, a Local2D speech bubble stacked on top) a zone's own view-proj yields a rect normalised to
that ZONE. Chaining a zone-normalised rect unchanged makes the runtime's analysis region scale
onto the whole window and reach into the 2D band above it. `dxr::RebaseZoneBoundsToWindow()`
maps a zone-normalised rect into window-normalised space given the zone's own rect in window
client pixels (`zoneRectPx` — what the app chained in `XrDisplayZoneDXR`, or read back in
`XrViewDisplayRawDXR::canvasRectPx`); `dxr::ProjectAabbToWindowBounds()` does the projection and
the rebase in one call:

```cpp
// zoneRectPx: the 3D zone's rect in window client pixels (from XrDisplayZoneDXR /
// XrViewDisplayRawDXR::canvasRectPx); windowW/H: the app window's client size.
XrRect2Df bounds{};
dxr::ProjectAabbToWindowBounds(contentAabbMin, contentAabbMax, viewProj, 2,
                               zoneRectPx, windowW, windowH, &bounds);

XrContentBoundsDXR contentBounds;
dxr::ChainContentBounds(frameEndInfo, contentBounds, bounds); // before xrEndFrame
```

Both rebase helpers clamp the input to `[0,1]` in ZONE space first (animation bounds routinely
project outside their own frustum) before mapping through `zoneRectPx`, and clamp the mapped
result to `[0,1]` again on the way out; a zero/negative-area `zoneRectPx` means "the zone is the
whole window" and `ProjectAabbToWindowBounds` skips the rebase, passing the projected rect
through unchanged. A non-zoned app (single full-window view) can keep calling
`ProjectAabbToCanvasBounds` + `ChainContentBounds()` directly, as above.

**Wiring it through `EndFrame`/`EndFrameWithWindowSpaceLayers`.** Both helpers build their own
`XrFrameEndInfo` internally and call `xrEndFrame` themselves, so an app using them has no
`XrFrameEndInfo&` of its own to hand to `dxr::ChainContentBounds()` directly — which is exactly
why two demos ended up copy-vendoring the helper bodies locally just to add `endInfo.next = ...`.
Both now take an optional trailing `const void* frameEndNext = nullptr` that they write straight
to `XrFrameEndInfo::next` — a *different* chain from the existing `projectionNext` parameter,
which lands on `XrCompositionLayerProjection::next` instead. `XrContentBoundsDXR` is the first
user: chain it with `dxr::ChainContentBounds()` against a scratch `XrFrameEndInfo` (its `next` is
never read by `EndFrame`/`EndFrameWithWindowSpaceLayers` — only the `contentBounds` struct they
point at matters) and pass that struct's address as `frameEndNext`:

```cpp
XrFrameEndInfo scratch{}; // ChainContentBounds needs a fei.next to read/link; discarded here
XrContentBoundsDXR contentBounds;
dxr::ChainContentBounds(scratch, contentBounds, bounds);
EndFrame(xr, displayTime, views, viewCount, /*projectionLayerFlags=*/0,
         /*projectionNext=*/nullptr, /*frameEndNext=*/&contentBounds);
```

It is appended as the true *last* parameter of both functions (after `extraLayerCount` on
`EndFrameWithWindowSpaceLayers`, not next to `projectionNext`) so every pre-existing call site —
including ones passing later positional arguments — keeps compiling unchanged.

### Content mask (v3) (`content_mask.h`, `XR_DXR_depth_budget` v3)

A rect is still mostly background. A zone-clamped box around a character is roughly two-thirds
pixels the model never covers, and any horizontal structure sitting in that surplus closes the
clip for content that never overlapped it. `XrContentMaskDXR` (SPEC_VERSION 3, chained on
`XrFrameEndInfo::next` in `xrEndFrame`, beside or instead of `XrContentBoundsDXR`) replaces the
box with the **silhouette** — the union over all views of where the app's content actually lands,
as a small occupancy grid.

The app produces nothing new for this. A transparent app already derives exactly that artefact
every frame, from its own rendered alpha, to build its click-through window region.
`dxr::ContentMaskFromCoverage()` is an any-coverage (max-filter) downsample of that existing
coverage buffer onto the extension's cell grid; `dxr::ChainContentMask()` attaches it:

```cpp
// cov: 1 byte per pixel, nonzero = covered, row-major, top-left origin.
std::vector<uint8_t> cells;                      // must outlive xrEndFrame
dxr::ContentMaskFromCoverage(cov, covW, covH, /*srcStride=*/covW,
                             windowW, windowH, /*srcRectPx=*/nullptr,  // whole window
                             64, 64, cells);
if (dxr::ContentMaskCoverageCells(cells) != 0) {  // 0 => the runtime reads it as absent
    XrContentMaskDXR mask;
    dxr::ChainContentMask(frameEndInfo, mask, cells, 64, 64);
}
xrEndFrame(session, &frameEndInfo);
```

**Grid convention and sizing.** Row-major, top-left origin, window-client-normalised: cell
`(x, y)` covers `[x/width, (x+1)/width) x [y/height, (y+1)/height)` of the app window's *client*
rect — the same frame as `XrContentBoundsDXR::bounds` after `RebaseZoneBoundsToWindow()`. The
extension allows 1..512 cells per side; the recommendation is **256x256 maximum**, and in
practice **the app's own coverage buffer downsampled by 4-8x** (a 480x270 click-through raster
→ a 120x68 or 60x34 grid). Finer buys nothing: the runtime dilates the mask by its own disparity
band before measuring, which erases sub-cell detail, and every extra cell is bytes copied inside
`xrEndFrame`.

**Any-coverage, on purpose.** A cell is marked if *any* overlapping source pixel is covered — a
single covered pixel marks its cell. Erring outward is correct here: the runtime measures the
background only *inside* the mask, so a cell wrongly cleared hides a real conflict while a cell
wrongly set at worst measures a little extra background. The app does **not** dilate, does **not**
clamp to its 3D zones and does **not** smooth — the runtime does all three, and pre-dilating here
would compound with its band.

**Zoned apps** pass `srcRectPx` (the zone's rect in window client pixels) instead of `nullptr`:
the coverage is placed into the window grid at that rect and every cell outside stays 0, which is
exactly the "leave the rest of the window unmasked" contract. `dxr::ContentMaskUnion()` ORs two
same-dimension grids together for an app with more than one 3D zone, and
`dxr::ContentMaskCoverageCells()` counts occupied cells — check it before chaining, since the
runtime treats an all-zero mask as absent and falls back to the content bounds.

`ContentMaskFromCoverage` returns false only on **degenerate input** (null source, a zero
dimension, `srcStride < srcW`, a non-positive `srcRectPx` extent, or a grid outside 1..512),
leaving `out` all-zero. A valid but fully-uncovered source is *not* a failure — it returns true
with an all-zero grid. `ChainContentMask` refuses a grid it cannot describe (dims outside 1..512,
or a `cells` buffer shorter than `width * height`) without touching `XrFrameEndInfo`, so a frame
is never left half-chained. As with `content_bounds.h`, if the pinned extensions header predates
the v3 bump this header defines an ABI-identical local `XrContentMaskDXR` and
`XR_TYPE_CONTENT_MASK_DXR` (guarded by `DXR_CONTENT_MASK_LOCAL_DEF`); both compile out once the
pin advances, with no call-site changes.

**Feeding it from the click-through region (no second readback).** On Windows/Vulkan,
`dxr::ClickThroughRegion` (`vk_clickthrough_region.h`) already reads back the union-over-views
alpha coverage every frame. `ClickThroughRegion::coverage()` exposes that buffer read-only —
one byte per texel, nonzero = covered, tightly packed at `coverageWidth() x coverageHeight()` —
so the mask costs a downsample rather than a second alpha readback. It is the **raw,
un-dilated** coverage (the region needs the dilated one; the depth budget must not be
pre-dilated), it lags one `update()` call like everything on that pipelined readback, and it is
`nullptr` until the first region has been applied. Nothing about how the click-through region
itself is computed changed.

```cpp
if (const uint8_t* cov = punch.coverage()) {
    dxr::ContentMaskFromCoverage(cov, punch.coverageWidth(), punch.coverageHeight(),
                                 punch.coverageWidth(), winW, winH, nullptr, 64, 64, cells);
}
```

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
- **W7** (`v0.4.0`) ✅ — **type-neutral core** (`displayxr::math_core`, own `dxr_*` POD types, zero OpenXR dep, `include/dxr_view_math.{h,c}`) now holds ALL the math; `displayxr::math` is a byte-compatible cast-wrapper over it (existing pins unaffected). New **`displayxr::math_xrt`** — the xrt-typed FOV-only wrapper the DisplayXR runtime links in place of its hand-synced `m_camera3d_view`/`m_display3d_view`/`m_multiview` ports (gated on `DISPLAYXR_XRT_INCLUDE_DIR`, the dir containing `xrt/xrt_defines.h`; set `DISPLAYXR_BUILD_MATH_OPENXR=OFF` to skip all OpenXR provisioning). Runtime render-ready output ≡ app-from-raw output **by construction** — the `XR_DXR_view_rig` equivalence guarantee.

## License

[ISC License](LICENSE) — same as the DisplayXR runtime.

## Source of truth

This repository is the **canonical home** for the shared math and scaffolding. The `display3d_view.*` / `camera3d_view.*` were reconciled from `displayxr-runtime`'s `test_apps/common/` (the most-advanced superset, with ZDP-anchored clip, `near_z/far_z` outputs, `vulkan_flip_y`, `center_view`, and `selftest`); `common/` was reconciled as the superset of the runtime test apps + both demos' forks (file-picker state machine and window-space-layer generics from the model-viewer fork, ADR-021/#441/#425/#457 features from the runtime). Both now lead: consumers pin a tag; changes land here first.
