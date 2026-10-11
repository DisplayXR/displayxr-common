// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief `dxr::CursorDepth` — the app-side helper for the depth-aware cursor
 *        (`XR_DXR_cursor_depth`, ADR-046 + Amendment 2).
 *
 * The runtime DRAWS the depth cursor (spec v5, Amendment 2 §B); the app only
 * owns the depth under it. What is left for an app is bookkeeping that every
 * app would otherwise re-type: read the pointer as a canvas UV, fire the
 * cursor FOOTPRINT rays through the previous frame's views, keep the nearest
 * hit, chain four structs on the right `next` pointers, and hide the OS
 * cursor exactly while the runtime says it draws. That is this class.
 *
 *     dxr::CursorDepth cursor;                         // inert until enable()
 *     cursor.enable(cursorDepthSpecVersion);           // 0 = ext not enabled: stays inert, zero cost
 *     cursor.setWindowCocoa((__bridge void*)view);     // or setWindowWin32(hwnd), or
 *                                                      // setPlatform(dxr::cursorPlatformX11(dpy, win))
 *     cursor.setRayCast([&](const dxr::CursorRay& r) -> std::optional<float> {
 *         float t; return myScene.raycast(r.origin, r.dir, t) ? std::optional<float>(t) : std::nullopt;
 *     });                                              // or useDepthLayer(true), or setHitTest(...)
 *     // per frame, around xrLocateViews:
 *     cursor.chain(locateInfo, viewState);
 *     xrLocateViews(session, &locateInfo, &viewState, cap, &n, views);
 *     cursor.afterLocate(views, activeViewCount);      // OS cursor hidden exactly while willDraw
 *
 * Manual path (any platform, incl. Wayland / Android / displayxr::linux_window):
 * feed `setPointerUV(u, v)` (or `clearPointerUV()` when the pointer leaves
 * the canvas) and react to `onDepthCursorShownChanged(cb)` with your own
 * cursor-hide call, e.g. `win.set_cursor(shown ? DxrCursor::Hidden : DxrCursor::Default)`.
 *
 * Design rules:
 *  - ZERO COST WHEN NOT ENABLED (ADR-046 §0): before `enable(v>0)`, `chain()`
 *    returns without touching either struct chain and `afterLocate()` returns
 *    at once. Enabled, a frame allocates nothing (fixed-size storage only).
 *  - No graphics-API code: the runtime draws (or, in DrawMode::App, the app
 *    draws from `placement()`).
 *  - Never no cursor: the OS cursor is hidden only while the runtime reports
 *    willDraw (DrawMode::Runtime) or the placement is active (DrawMode::App).
 *    On a pre-v5 runtime, Runtime mode never hides it (nothing would draw).
 *
 * The pure parts (`cursorFootprintUV`, `cursorFootprintRays`, `cursorViewRay`,
 * `cursorCanvasUV`, the class itself) are header-only and platform-neutral,
 * so this header is part of `displayxr::rules` and works on Android. The
 * window adapters live in `cursor_depth_win32.cpp` (displayxr::common,
 * Windows), `cursor_depth_macos.mm` (displayxr::common, macOS) and the
 * header-only `cursor_depth_x11.h` (include it where Xlib is available).
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <openxr/openxr.h>
#include <openxr/XR_DXR_cursor_depth.h>

#if !defined(XR_DXR_cursor_depth_SPEC_VERSION) || (XR_DXR_cursor_depth_SPEC_VERSION < 5)
#error "cursor_depth.h needs XR_DXR_cursor_depth.h SPEC_VERSION >= 5 (displayxr-runtime v2.36+ headers)"
#endif

namespace dxr {

// ---------------------------------------------------------------------------
// Pure geometry
// ---------------------------------------------------------------------------

//! A ray in the locate space: origin = a view's eye, dir = unit direction.
struct CursorRay
{
    XrVector3f origin;
    XrVector3f dir;
};

/*!
 * The footprint's ring centre relative to the hotspot, in units of the cursor
 * height (x right, y DOWN). The default arrow hangs down-right of its tip;
 * this is the SAME rule as the runtime / web SDK (`cursorArrowFootprint`:
 * centre = tip + (0.3 h / aspect, 0.5 h)).
 */
inline constexpr XrVector2f kCursorArrowBodyOffset = {0.3f, 0.5f};
//! A sprite whose hotspot is its centre (crosshair, ring): the ring is around the hotspot.
inline constexpr XrVector2f kCursorCenteredBodyOffset = {0.0f, 0.0f};

/*!
 * Body offset for a custom sprite: from its hotspot (normalised, v down) to
 * its centre, in units of its height. `spriteAspect` = width / height.
 */
inline XrVector2f
cursorBodyOffsetForSprite(XrVector2f hotSpot, float spriteAspect)
{
    return {(0.5f - hotSpot.x) * spriteAspect, 0.5f - hotSpot.y};
}

//! Max points `cursorFootprintUV` produces: the hotspot + an 8-point ring (+ the ring centre).
inline constexpr uint32_t kCursorFootprintMaxPoints = 10;

/*!
 * The cursor FOOTPRINT, canvas-normalised (origin top-left, v down): the
 * hotspot first, then 8 points on a circle of radius 0.75 x heightFrac
 * (canvas heights) around the sprite's body (hotspot + bodyOffset x height),
 * plus the body centre itself when it is not the hotspot. Same rule as the
 * runtime's submitted-depth search and the web SDK.
 *
 * @param canvasAspect canvas width / height (> 0).
 * @return the number of points written (<= kCursorFootprintMaxPoints).
 */
inline uint32_t
cursorFootprintUV(XrVector2f uv,
                  float heightFrac,
                  float canvasAspect,
                  XrVector2f out[kCursorFootprintMaxPoints],
                  XrVector2f bodyOffset = kCursorArrowBodyOffset)
{
    if (!(canvasAspect > 0.0f) || !std::isfinite(canvasAspect)) canvasAspect = 1.0f;
    uint32_t n = 0;
    out[n++] = uv;
    const float cu = uv.x + bodyOffset.x * heightFrac / canvasAspect;
    const float cv = uv.y + bodyOffset.y * heightFrac;
    const float r = 0.75f * heightFrac;
    for (int k = 0; k < 8; k++) {
        const float a = (float)k * 0.785398163f; // pi/4
        out[n++] = {cu + r * std::cos(a) / canvasAspect, cv + r * std::sin(a)};
    }
    if (bodyOffset.x != 0.0f || bodyOffset.y != 0.0f) out[n++] = {cu, cv};
    return n;
}

namespace detail {
inline XrVector3f
rotate(const XrQuaternionf &q, XrVector3f v)
{
    // v' = v + 2w (q x v) + 2 q x (q x v)
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
inline float dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
//! Zeroed struct with its type set (no -Wmissing-field-initializers noise).
template <typename T>
inline T
makeXr(XrStructureType type)
{
    T v{};
    v.type = type;
    return v;
}
} // namespace detail

/*!
 * Ray from a located view through canvas point (u, v). Every DisplayXR view
 * is a Kooima frustum framing the canvas, so (u, v) maps linearly onto the
 * fov tangents — exact for display rigs, camera rigs and any m2v.
 */
inline CursorRay
cursorViewRay(const XrView &view, float u, float v)
{
    const float tl = std::tan(view.fov.angleLeft), tr = std::tan(view.fov.angleRight);
    const float tu = std::tan(view.fov.angleUp), td = std::tan(view.fov.angleDown);
    XrVector3f d = detail::rotate(view.pose.orientation, {tl + u * (tr - tl), tu - v * (tu - td), -1.0f});
    const float len = std::sqrt(detail::dot(d, d));
    if (len > 0.0f) d = {d.x / len, d.y / len, d.z / len};
    return {view.pose.position, d};
}

/*!
 * The canvas aspect (width / height) a Kooima view frames: the tangent
 * extents of its fov ARE the canvas extents over the eye distance. 0 when the
 * fov is degenerate.
 */
inline float
cursorCanvasAspect(const XrView &view)
{
    const float w = std::tan(view.fov.angleRight) - std::tan(view.fov.angleLeft);
    const float h = std::tan(view.fov.angleUp) - std::tan(view.fov.angleDown);
    return (h > 0.0f && w > 0.0f) ? w / h : 0.0f;
}

//! Max rays: footprint points x the two outer views.
inline constexpr uint32_t kCursorFootprintMaxRays = 2 * kCursorFootprintMaxPoints;

//! Fixed-size ray set (no allocation).
struct CursorFootprintRays
{
    CursorRay rays[kCursorFootprintMaxRays];
    uint32_t count = 0;
    const CursorRay *begin() const { return rays; }
    const CursorRay *end() const { return rays + count; }
};

/*!
 * Rays from the OUTER views (views[0] and views[viewCount-1]) through every
 * footprint point (cursorFootprintUV). "Ray -> nearest hit" over these, kept
 * nearest the viewer, is the hint's nearestPoint.
 *
 * @param viewCount the ACTIVE view count (1 in 2D -> no rays: nothing to hint).
 * @param canvasAspect width / height; <= 0 derives it from views[0]'s fov.
 */
inline CursorFootprintRays
cursorFootprintRays(const XrView *views,
                    uint32_t viewCount,
                    XrVector2f uv,
                    float heightFrac,
                    float canvasAspect = 0.0f,
                    XrVector2f bodyOffset = kCursorArrowBodyOffset)
{
    CursorFootprintRays out;
    if (views == nullptr || viewCount < 2) return out;
    if (!(canvasAspect > 0.0f)) canvasAspect = cursorCanvasAspect(views[0]);
    XrVector2f pts[kCursorFootprintMaxPoints];
    const uint32_t np = cursorFootprintUV(uv, heightFrac, canvasAspect, pts, bodyOffset);
    const XrView *outer[2] = {&views[0], &views[viewCount - 1]};
    for (uint32_t i = 0; i < np; i++)
        for (const XrView *v : outer) out.rays[out.count++] = cursorViewRay(*v, pts[i].x, pts[i].y);
    return out;
}

/*!
 * Pointer position in a window's client area -> canvas UV (origin top-left,
 * v down). `yUp` = the coordinates grow upward (an unflipped NSView).
 * Returns false when the area is empty; the UV may lie outside [0,1].
 */
inline bool
cursorCanvasUV(double x, double y, double width, double height, bool yUp, XrVector2f &uv)
{
    if (!(width > 0.0) || !(height > 0.0)) return false;
    uv.x = (float)(x / width);
    uv.y = (float)(yUp ? 1.0 - y / height : y / height);
    return true;
}

inline bool
cursorUVInCanvas(XrVector2f uv)
{
    return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
}

// ---------------------------------------------------------------------------
// Window adapters
// ---------------------------------------------------------------------------

/*!
 * A window the helper reads the pointer from and hides the OS cursor over.
 * Ready-made: Win32 (setWindowWin32), Cocoa (setWindowCocoa), X11
 * (cursor_depth_x11.h). Implement it yourself for anything else, or skip it
 * and use the manual path.
 */
class CursorPlatform
{
public:
    virtual ~CursorPlatform() = default;
    //! Pointer as canvas UV (may be outside [0,1]); false = unknown.
    virtual bool pointerUV(XrVector2f &uv) = 0;
    //! Hide (true) / show (false) the OS cursor over the canvas. Called on transitions only.
    virtual void setOsCursorHidden(bool hidden) = 0;
};

//! Win32: HWND. Defined in cursor_depth_win32.cpp (displayxr::common on Windows).
std::unique_ptr<CursorPlatform> cursorPlatformWin32(void *hwnd);
//! Cocoa: NSView* (the canvas view). Defined in cursor_depth_macos.mm (displayxr::common on macOS).
std::unique_ptr<CursorPlatform> cursorPlatformCocoa(void *nsView);

// ---------------------------------------------------------------------------
// The helper
// ---------------------------------------------------------------------------

class CursorDepth
{
public:
    //! Who draws the depth cursor.
    enum class DrawMode
    {
        Runtime, //!< spec v5: chain XrCursorDepthDrawInfoDXR; hide the OS cursor while willDraw
        App,     //!< v1-v4: the app draws from placement(); OS cursor hidden while it is active
    };

    //! "ray -> nearest hit": the ray parameter t of the first hit (point = origin + t * dir).
    using RayCastFn = std::function<std::optional<float>(const CursorRay &)>;
    //! Full control: return the nearest content point under the footprint, in the locate space.
    using HitTestFn = std::function<std::optional<XrVector3f>(const XrView *views, uint32_t viewCount,
                                                              XrVector2f uv, const CursorFootprintRays &rays)>;

    CursorDepth() = default;
    ~CursorDepth() { setDepthCursorShown(false); }
    CursorDepth(const CursorDepth &) = delete;
    CursorDepth &operator=(const CursorDepth &) = delete;

    // ---- setup ------------------------------------------------------------

    /*!
     * @param specVersion the XR_DXR_cursor_depth extensionVersion the runtime
     *        reported, IF you enabled the extension at xrCreateInstance; 0 =
     *        not enabled (the helper stays inert). Below 5 the helper falls
     *        back to DrawMode::App semantics for the OS cursor (see DrawMode).
     */
    void enable(uint32_t specVersion)
    {
        m_spec = specVersion;
        if (m_spec == 0) setDepthCursorShown(false);
    }
    bool enabled() const { return m_spec > 0; }
    uint32_t specVersion() const { return m_spec; }

    void setPlatform(std::unique_ptr<CursorPlatform> p)
    {
        setDepthCursorShown(false);
        m_platform = std::move(p);
    }
    void setWindowWin32(void *hwnd) { setPlatform(cursorPlatformWin32(hwnd)); }
    void setWindowCocoa(void *nsView) { setPlatform(cursorPlatformCocoa(nsView)); }

    //! Manual pointer (overrides the platform until clearPointerUV()). Outside [0,1] = off the canvas.
    void setPointerUV(float u, float v)
    {
        m_manualUV = true;
        m_uv = {u, v};
    }
    void clearPointerUV() { m_manualUV = false; }

    //! Called on every change of "a depth cursor replaces the OS cursor" (after the platform hide/show).
    void onDepthCursorShownChanged(std::function<void(bool shown)> cb) { m_onShown = std::move(cb); }

    void setDrawMode(DrawMode m) { m_drawMode = m; }
    DrawMode drawMode() const { return m_drawMode; }

    /*!
     * The runtime-drawn sprite (spec v5): an XrSwapchain the app filled and
     * released, its hotspot (normalised, v down), its height on the panel in
     * metres (0 = the OS cursor's size), and its aspect (w/h, for the
     * footprint). XR_NULL_HANDLE (the default) = the runtime's default look
     * (the OS cursor / classic arrow), footprint = the arrow's.
     */
    void setSprite(XrSwapchain image, XrVector2f hotSpot, float sizeMeters, float spriteAspect = 1.0f)
    {
        m_drawInfo.image = image;
        m_drawInfo.hotSpot = hotSpot;
        m_drawInfo.sizeMeters = sizeMeters;
        m_bodyOffset = image == XR_NULL_HANDLE ? kCursorArrowBodyOffset
                                               : cursorBodyOffsetForSprite(hotSpot, spriteAspect);
    }
    void setVisible(bool visible) { m_drawInfo.visible = visible ? XR_TRUE : XR_FALSE; }
    //! Override the footprint shape (e.g. kCursorCenteredBodyOffset for an app-drawn crosshair).
    void setFootprintBodyOffset(XrVector2f o) { m_bodyOffset = o; }

    //! Cursor height as a fraction of the canvas height: the footprint size (and the
    //! hint's cursorHeight). Default 0.03, the runtime's.
    void setCursorHeight(float frac) { m_height = frac; }
    float cursorHeight() const { return m_height; }

    //! Spec v3 options; chained only once one of these was called.
    void setAnchor(XrCursorDepthAnchorModeDXR mode)
    {
        m_options.anchorMode = mode;
        m_chainOptions = true;
    }
    void setMargin(float marginBaselines)
    {
        m_options.margin = marginBaselines;
        m_chainOptions = true;
    }

    /*!
     * Spec v2: the runtime finds the nearest content in the depth the app
     * submits (XR_KHR_composition_layer_depth, projection layer in the locate
     * space). No hit test runs. Ignored below v2.
     */
    void useDepthLayer(bool on) { m_depthLayer = on; }

    void setRayCast(RayCastFn fn) { m_rayCast = std::move(fn); }
    void setHitTest(HitTestFn fn) { m_hitTest = std::move(fn); }

    //! Also chain XrCursorDepthPlacementDXR (diagnostics in Runtime mode; always chained in App mode).
    void setWantPlacement(bool on) { m_wantPlacement = on; }

    // ---- per frame --------------------------------------------------------

    /*!
     * Before xrLocateViews: reads the pointer, runs the hit test against the
     * PREVIOUS locate's views (a frame late; the runtime's filter absorbs it),
     * and prepends the cursor structs to both chains:
     *   locateInfo -> hint [-> drawInfo] [-> options] [-> source] -> (previous locateInfo.next)
     *   viewState  -> drawState [-> placement] -> (previous viewState.next)
     * The structs live in this object: keep it alive across the xrLocateViews.
     * Not enabled: touches nothing.
     */
    void chain(XrViewLocateInfo &locateInfo, XrViewState &viewState)
    {
        m_hintChained = false;
        if (m_spec == 0) return;

        XrVector2f uv{};
        bool have = false;
        if (m_manualUV) {
            uv = m_uv;
            have = true;
        } else if (m_platform) {
            have = m_platform->pointerUV(uv);
        }
        m_inCanvas = have && cursorUVInCanvas(uv);

        if (m_inCanvas) {
            m_hint = detail::makeXr<XrCursorDepthHintDXR>(XR_TYPE_CURSOR_DEPTH_HINT_DXR);
            m_hint.cursorUV = uv;
            m_hint.cursorHeight = m_height;
            m_hint.hasContent = XR_FALSE;
            m_hint.nearestPoint = {0.0f, 0.0f, 0.0f};
            const bool layer = m_depthLayer && m_spec >= 2;
            if (!layer) {
                std::optional<XrVector3f> hit = hitTest(uv);
                if (hit) {
                    m_hint.hasContent = XR_TRUE;
                    m_hint.nearestPoint = *hit;
                }
            }
            const void *tail = locateInfo.next;
            if (layer) {
                m_source = detail::makeXr<XrCursorDepthSourceDXR>(XR_TYPE_CURSOR_DEPTH_SOURCE_DXR);
                m_source.next = tail;
                m_source.source = XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR;
                tail = &m_source;
            }
            if (m_chainOptions && m_spec >= 3) {
                m_options.type = XR_TYPE_CURSOR_DEPTH_OPTIONS_DXR;
                m_options.next = tail;
                tail = &m_options;
            }
            if (runtimeDraws()) {
                m_drawInfo.type = XR_TYPE_CURSOR_DEPTH_DRAW_INFO_DXR;
                m_drawInfo.next = tail;
                tail = &m_drawInfo;
            }
            m_hint.next = tail;
            locateInfo.next = &m_hint;
            m_hintChained = true;
        }

        m_placement = detail::makeXr<XrCursorDepthPlacementDXR>(XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR);
        m_drawState = detail::makeXr<XrCursorDepthDrawStateDXR>(XR_TYPE_CURSOR_DEPTH_DRAW_STATE_DXR);
        void *vtail = viewState.next;
        if (m_wantPlacement || !runtimeDraws()) {
            m_placement.next = vtail;
            vtail = &m_placement;
        }
        if (runtimeDraws()) {
            m_drawState.next = vtail;
            vtail = &m_drawState;
        }
        viewState.next = vtail;
    }

    /*!
     * After xrLocateViews: remembers the views for next frame's hit test and
     * hides / shows the OS cursor on transitions.
     * @param activeViewCount the views that are live this frame (1 in 2D).
     */
    void afterLocate(const XrView *views, uint32_t activeViewCount)
    {
        if (m_spec == 0) return;
        // Keep the outer pair only: that is all the footprint rays use.
        m_prevCount = 0;
        if (views != nullptr && activeViewCount >= 2) {
            m_prevViews[0] = views[0];
            m_prevViews[1] = views[activeViewCount - 1];
            m_prevCount = 2;
        }
        bool shown;
        if (runtimeDraws())
            shown = m_hintChained && m_drawState.willDraw == XR_TRUE;
        else
            shown = m_drawMode == DrawMode::App && m_hintChained && m_placement.isActive == XR_TRUE;
        setDepthCursorShown(shown);
    }

    // ---- results ----------------------------------------------------------

    //! True while a depth cursor replaces the OS cursor (the OS cursor is hidden).
    bool depthCursorShown() const { return m_shown; }
    //! Runtime mode on a v5+ runtime: the runtime is asked to draw.
    bool runtimeDraws() const { return m_drawMode == DrawMode::Runtime && m_spec >= 5; }
    //! DrawMode::App: draw your sprite at placement().pose / height while this is true.
    bool appShouldDraw() const { return m_drawMode == DrawMode::App && m_shown; }
    bool pointerInCanvas() const { return m_inCanvas; }
    bool hintChained() const { return m_hintChained; }
    const XrCursorDepthHintDXR &hint() const { return m_hint; }
    const XrCursorDepthPlacementDXR &placement() const { return m_placement; }
    const XrCursorDepthDrawStateDXR &drawState() const { return m_drawState; }

private:
    std::optional<XrVector3f> hitTest(XrVector2f uv) const
    {
        if (m_prevCount < 2) return std::nullopt; // 2D or no previous locate: nothing to hint
        const CursorFootprintRays rays = cursorFootprintRays(m_prevViews, 2, uv, m_height, 0.0f, m_bodyOffset);
        if (m_hitTest) return m_hitTest(m_prevViews, 2, uv, rays);
        if (!m_rayCast) return std::nullopt;
        const XrVector3f fwd = detail::rotate(m_prevViews[0].pose.orientation, {0.0f, 0.0f, -1.0f});
        std::optional<XrVector3f> best;
        float bestDepth = 0.0f;
        for (const CursorRay &r : rays) {
            const std::optional<float> t = m_rayCast(r);
            if (!t || !std::isfinite(*t) || *t < 0.0f) continue;
            const XrVector3f p = {r.origin.x + r.dir.x * *t, r.origin.y + r.dir.y * *t,
                                  r.origin.z + r.dir.z * *t};
            const float depth = detail::dot(p, fwd); // smaller = nearer the viewer
            if (!best || depth < bestDepth) {
                best = p;
                bestDepth = depth;
            }
        }
        return best;
    }

    void setDepthCursorShown(bool shown)
    {
        if (shown == m_shown) return;
        m_shown = shown;
        if (m_platform) m_platform->setOsCursorHidden(shown);
        if (m_onShown) m_onShown(shown);
    }

    uint32_t m_spec = 0;
    DrawMode m_drawMode = DrawMode::Runtime;
    std::unique_ptr<CursorPlatform> m_platform;
    std::function<void(bool)> m_onShown;
    RayCastFn m_rayCast;
    HitTestFn m_hitTest;

    bool m_manualUV = false;
    XrVector2f m_uv{};
    bool m_inCanvas = false;
    float m_height = 0.03f;
    XrVector2f m_bodyOffset = kCursorArrowBodyOffset;
    bool m_depthLayer = false;
    bool m_chainOptions = false;
    bool m_wantPlacement = false;
    bool m_hintChained = false;
    bool m_shown = false;

    XrView m_prevViews[2] = {};
    uint32_t m_prevCount = 0;

    XrCursorDepthHintDXR m_hint = detail::makeXr<XrCursorDepthHintDXR>(XR_TYPE_CURSOR_DEPTH_HINT_DXR);
    XrCursorDepthSourceDXR m_source = detail::makeXr<XrCursorDepthSourceDXR>(XR_TYPE_CURSOR_DEPTH_SOURCE_DXR);
    //! anchorMode 0 = HYBRID, margin 0 = the runtime default.
    XrCursorDepthOptionsDXR m_options = detail::makeXr<XrCursorDepthOptionsDXR>(XR_TYPE_CURSOR_DEPTH_OPTIONS_DXR);
    XrCursorDepthDrawInfoDXR m_drawInfo = makeDrawInfo();
    XrCursorDepthPlacementDXR m_placement =
        detail::makeXr<XrCursorDepthPlacementDXR>(XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR);
    XrCursorDepthDrawStateDXR m_drawState =
        detail::makeXr<XrCursorDepthDrawStateDXR>(XR_TYPE_CURSOR_DEPTH_DRAW_STATE_DXR);

    static XrCursorDepthDrawInfoDXR makeDrawInfo()
    {
        XrCursorDepthDrawInfoDXR d = detail::makeXr<XrCursorDepthDrawInfoDXR>(XR_TYPE_CURSOR_DEPTH_DRAW_INFO_DXR);
        d.image = XR_NULL_HANDLE; // the runtime's default look
        d.visible = XR_TRUE;
        return d;
    }
};

} // namespace dxr
