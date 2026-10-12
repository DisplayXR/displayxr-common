// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Contract test for cursor_depth.h (dxr::CursorDepth, XR_DXR_cursor_depth v5-v7).
 *
 * The pure parts: pointer -> canvas UV, the footprint (same rule as the
 * runtime / web SDK), footprint rays against a known Kooima pair (built the
 * way the runtime's tests_aux_cursor_depth.cpp builds them, incl. a camera-rig
 * transform), the nearest-hit reduction, the struct chain order, and the
 * OS-cursor hide/show transitions. No OpenXR loader, no window system.
 */

#include "cursor_depth.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                       \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool
near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

namespace {

constexpr float W = 0.30f; // canvas width, m
constexpr float H = 0.20f; // canvas height, m

struct Xform
{
    XrQuaternionf q{0, 0, 0, 1};
    XrVector3f t{0, 0, 0};
    float s = 1.0f;
};

XrVector3f
rotateRef(XrQuaternionf q, XrVector3f v)
{
    // Rotation-matrix reference, independent of the unit under test.
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    return {
        (1 - 2 * (y * y + z * z)) * v.x + 2 * (x * y - z * w) * v.y + 2 * (x * z + y * w) * v.z,
        2 * (x * y + z * w) * v.x + (1 - 2 * (x * x + z * z)) * v.y + 2 * (y * z - x * w) * v.z,
        2 * (x * z - y * w) * v.x + 2 * (y * z + x * w) * v.y + (1 - 2 * (x * x + y * y)) * v.z,
    };
}

XrVector3f
apply(const Xform &x, XrVector3f p)
{
    XrVector3f r = rotateRef(x.q, {p.x * x.s, p.y * x.s, p.z * x.s});
    return {r.x + x.t.x, r.y + x.t.y, r.z + x.t.z};
}

// Kooima view from a display-space eye onto the W x H canvas centred at the
// origin in the z = 0 plane, carried into the locate space by x.
XrView
kooima(XrVector3f eye, const Xform &x = Xform{})
{
    XrView v{XR_TYPE_VIEW};
    const float d = eye.z;
    v.fov.angleLeft = std::atan((-W / 2 - eye.x) / d);
    v.fov.angleRight = std::atan((W / 2 - eye.x) / d);
    v.fov.angleUp = std::atan((H / 2 - eye.y) / d);
    v.fov.angleDown = std::atan((-H / 2 - eye.y) / d);
    v.pose.orientation = x.q;
    v.pose.position = apply(x, eye);
    return v;
}

// Canvas point (display space) under canvas UV (origin top-left, v down).
XrVector3f
canvasPoint(XrVector2f uv)
{
    return {-W / 2 + uv.x * W, H / 2 - uv.y * H, 0.0f};
}

// Where a ray crosses the display plane, in DISPLAY space (undo the xform first).
XrVector3f
hitDisplayPlane(const dxr::CursorRay &r, const Xform &x)
{
    XrQuaternionf qi{-x.q.x, -x.q.y, -x.q.z, x.q.w};
    XrVector3f o = {r.origin.x - x.t.x, r.origin.y - x.t.y, r.origin.z - x.t.z};
    o = rotateRef(qi, o);
    o = {o.x / x.s, o.y / x.s, o.z / x.s};
    const XrVector3f d = rotateRef(qi, r.dir);
    const float k = -o.z / d.z;
    return {o.x + d.x * k, o.y + d.y * k, 0.0f};
}

struct FakePlatform : dxr::CursorPlatform
{
    XrVector2f uv{0.5f, 0.5f};
    bool known = true;
    int hides = 0, shows = 0;
    bool hidden = false;
    bool pointerUV(XrVector2f &out) override
    {
        out = uv;
        return known;
    }
    void setOsCursorHidden(bool h) override
    {
        hidden = h;
        (h ? hides : shows)++;
    }
};

struct Base
{
    XrStructureType type;
    const void *next;
};

} // namespace

static void
testCanvasUV()
{
    XrVector2f uv{};
    CHECK(dxr::cursorCanvasUV(100, 50, 400, 200, false, uv) && near(uv.x, 0.25f) && near(uv.y, 0.25f),
          "y-down client coords map straight");
    CHECK(dxr::cursorCanvasUV(100, 50, 400, 200, true, uv) && near(uv.x, 0.25f) && near(uv.y, 0.75f),
          "y-up (unflipped NSView) coords flip v");
    CHECK(!dxr::cursorCanvasUV(1, 1, 0, 200, false, uv), "empty client area -> unknown");
    CHECK(dxr::cursorCanvasUV(-10, 50, 400, 200, false, uv) && !dxr::cursorUVInCanvas(uv),
          "left of the window is off the canvas");
    CHECK(dxr::cursorUVInCanvas({0.0f, 1.0f}), "canvas edges are on the canvas");
}

static void
testFootprintUV()
{
    XrVector2f pts[dxr::kCursorFootprintMaxPoints];
    const float h = 0.04f, aspect = 1.5f;
    const uint32_t n = dxr::cursorFootprintUV({0.5f, 0.4f}, h, aspect, pts);
    CHECK(n == 10, "arrow footprint = tip + 8 ring points + body centre");
    CHECK(near(pts[0].x, 0.5f) && near(pts[0].y, 0.4f), "hotspot (the arrow tip) first");
    // web SDK cursorArrowFootprint: ring around (u + 0.3 h / aspect, v + 0.5 h), radius 0.75 h.
    const float cu = 0.5f + 0.3f * h / aspect, cv = 0.4f + 0.5f * h;
    CHECK(near(pts[1].x, cu + 0.75f * h / aspect) && near(pts[1].y, cv), "ring k=0 is right of the body");
    CHECK(near(pts[3].x, cu, 1e-5f) && near(pts[3].y, cv + 0.75f * h), "ring k=2 is below the body (v down)");
    CHECK(near(pts[9].x, cu) && near(pts[9].y, cv), "body centre last");

    const uint32_t nc = dxr::cursorFootprintUV({0.5f, 0.4f}, h, aspect, pts, dxr::kCursorCenteredBodyOffset);
    CHECK(nc == 9, "centred sprite: hotspot + ring, no duplicate centre");
    CHECK(near(pts[5].x, 0.5f - 0.75f * h / aspect) && near(pts[5].y, 0.4f), "centred ring is around the hotspot");

    const XrVector2f off = dxr::cursorBodyOffsetForSprite({0.5f, 0.5f}, 2.0f);
    CHECK(near(off.x, 0.0f) && near(off.y, 0.0f), "centre hotspot -> centred footprint");
    const XrVector2f tl = dxr::cursorBodyOffsetForSprite({0.0f, 0.0f}, 12.0f / 19.0f);
    CHECK(near(tl.x, 0.5f * 12.0f / 19.0f) && near(tl.y, 0.5f), "top-left hotspot -> body down-right");
}

static void
testRaysKooima()
{
    const XrVector2f uv{0.62f, 0.31f};
    const float h = 0.04f;
    Xform rigs[2];
    // A camera rig: rotated 30 deg about Y and 10 deg about X, moved, scaled x3.
    const float ay = 0.2617994f, ax = 0.0872665f;
    const XrQuaternionf qy{0, std::sin(ay), 0, std::cos(ay)}, qx{std::sin(ax), 0, 0, std::cos(ax)};
    rigs[1].q = {qy.w * qx.x + qy.x * qx.w + qy.y * qx.z - qy.z * qx.y,
                 qy.w * qx.y - qy.x * qx.z + qy.y * qx.w + qy.z * qx.x,
                 qy.w * qx.z + qy.x * qx.y - qy.y * qx.x + qy.z * qx.w,
                 qy.w * qx.w - qy.x * qx.x - qy.y * qx.y - qy.z * qx.z};
    rigs[1].t = {1.0f, -2.0f, 0.5f};
    rigs[1].s = 3.0f;

    for (const Xform &x : rigs) {
        XrView views[3] = {kooima({-0.032f, 0.01f, 0.6f}, x), kooima({0.0f, 0.01f, 0.6f}, x),
                           kooima({0.032f, 0.01f, 0.6f}, x)};
        CHECK(near(dxr::cursorCanvasAspect(views[0]), W / H), "aspect from the fov = canvas W/H");

        const dxr::CursorFootprintRays rays = dxr::cursorFootprintRays(views, 3, uv, h);
        CHECK(rays.count == 20, "10 footprint points x 2 outer views");
        XrVector2f pts[dxr::kCursorFootprintMaxPoints];
        dxr::cursorFootprintUV(uv, h, W / H, pts);
        for (uint32_t i = 0; i < rays.count; i++) {
            const dxr::CursorRay &r = rays.rays[i];
            const XrView &outer = (i % 2 == 0) ? views[0] : views[2];
            CHECK(near(r.origin.x, outer.pose.position.x) && near(r.origin.z, outer.pose.position.z),
                  "ray starts at an OUTER view's eye");
            CHECK(near(r.dir.x * r.dir.x + r.dir.y * r.dir.y + r.dir.z * r.dir.z, 1.0f), "unit direction");
            const XrVector3f p = hitDisplayPlane(r, x);
            const XrVector3f e = canvasPoint(pts[i / 2]);
            CHECK(near(p.x, e.x) && near(p.y, e.y), "each ray crosses the canvas at its footprint point");
        }
        CHECK(dxr::cursorFootprintRays(views, 1, uv, h).count == 0, "one active view (2D): no rays");
    }
}

static void
testChainDisabled()
{
    dxr::CursorDepth c;
    auto *fp = new FakePlatform;
    c.setPlatform(std::unique_ptr<dxr::CursorPlatform>(fp));
    Base rig{XR_TYPE_VIEW_LOCATE_INFO, nullptr};
    int vsTail = 0;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, &rig};
    XrViewState vs{XR_TYPE_VIEW_STATE, &vsTail};
    c.chain(li, vs);
    CHECK(li.next == &rig && vs.next == &vsTail, "not enabled: neither chain touched");
    XrView v[2] = {kooima({-0.03f, 0, 0.6f}), kooima({0.03f, 0, 0.6f})};
    c.afterLocate(v, 2);
    CHECK(fp->hides == 0 && fp->shows == 0 && !c.depthCursorShown(), "not enabled: OS cursor untouched");
    c.enable(0);
    c.chain(li, vs);
    CHECK(li.next == &rig && vs.next == &vsTail, "enable(0) = still inert");
}

static void
testChainOrder()
{
    Base rig{XR_TYPE_VIEW_LOCATE_INFO, nullptr};
    int vsTail = 0;

    { // v5, runtime draw, default options.
        dxr::CursorDepth c;
        c.enable(5);
        c.setPointerUV(0.5f, 0.5f);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, &rig};
        XrViewState vs{XR_TYPE_VIEW_STATE, &vsTail};
        c.chain(li, vs);
        const Base *hint = static_cast<const Base *>(li.next);
        CHECK(hint && hint->type == XR_TYPE_CURSOR_DEPTH_HINT_DXR, "locateInfo -> hint");
        const Base *draw = hint ? static_cast<const Base *>(hint->next) : nullptr;
        CHECK(draw && draw->type == XR_TYPE_CURSOR_DEPTH_DRAW_INFO_DXR, "hint -> drawInfo");
        CHECK(draw && draw->next == &rig, "drawInfo -> the app's previous chain");
        const Base *ds = static_cast<const Base *>(vs.next);
        CHECK(ds && ds->type == XR_TYPE_CURSOR_DEPTH_DRAW_STATE_DXR && ds->next == &vsTail,
              "viewState -> drawState -> previous (no placement unless asked)");
        CHECK(c.hint().cursorUV.x == 0.5f && c.hint().hasContent == XR_FALSE, "no hit test, no content");
    }
    { // v5, every optional struct.
        dxr::CursorDepth c;
        c.enable(5);
        c.setPointerUV(0.2f, 0.7f);
        c.setAnchor(XR_CURSOR_DEPTH_ANCHOR_MODE_SCREEN_DXR);
        c.useDepthLayer(true);
        c.setWantPlacement(true);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, &rig};
        XrViewState vs{XR_TYPE_VIEW_STATE, &vsTail};
        c.chain(li, vs);
        const XrStructureType want[] = {XR_TYPE_CURSOR_DEPTH_HINT_DXR, XR_TYPE_CURSOR_DEPTH_DRAW_INFO_DXR,
                                        XR_TYPE_CURSOR_DEPTH_OPTIONS_DXR, XR_TYPE_CURSOR_DEPTH_SOURCE_DXR};
        const Base *b = static_cast<const Base *>(li.next);
        for (XrStructureType t : want) {
            CHECK(b && b->type == t, "hint -> drawInfo -> options -> source");
            b = b ? static_cast<const Base *>(b->next) : nullptr;
        }
        CHECK(b == &rig, "... -> the app's previous chain");
        const Base *v = static_cast<const Base *>(vs.next);
        CHECK(v && v->type == XR_TYPE_CURSOR_DEPTH_DRAW_STATE_DXR, "viewState -> drawState");
        v = v ? static_cast<const Base *>(v->next) : nullptr;
        CHECK(v && v->type == XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR && v->next == &vsTail, "-> placement -> previous");
    }
    { // Pointer off the canvas: no hint, but the draw state is still read (willDraw false).
        dxr::CursorDepth c;
        c.enable(5);
        c.setPointerUV(1.2f, 0.5f);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, &rig};
        XrViewState vs{XR_TYPE_VIEW_STATE, &vsTail};
        c.chain(li, vs);
        CHECK(li.next == &rig && !c.hintChained(), "off the canvas: no hint");
        CHECK(vs.next != &vsTail, "off the canvas: draw state still chained");
    }
    { // v4 runtime: no draw request, placement chained; v1: no options / source.
        dxr::CursorDepth c;
        c.enable(1);
        c.setPointerUV(0.5f, 0.5f);
        c.setAnchor(XR_CURSOR_DEPTH_ANCHOR_MODE_WORLD_DXR);
        c.useDepthLayer(true);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, &rig};
        XrViewState vs{XR_TYPE_VIEW_STATE, &vsTail};
        c.chain(li, vs);
        const Base *hint = static_cast<const Base *>(li.next);
        CHECK(hint && hint->next == &rig, "v1: hint only (no v2 source, v3 options, v5 draw)");
        const Base *v = static_cast<const Base *>(vs.next);
        CHECK(v && v->type == XR_TYPE_CURSOR_DEPTH_PLACEMENT_DXR && v->next == &vsTail, "v1: placement only");
    }
}

static void
testShowHide()
{
    XrView v[2] = {kooima({-0.032f, 0, 0.6f}), kooima({0.032f, 0, 0.6f})};
    { // Runtime mode: hidden exactly while willDraw.
        dxr::CursorDepth c;
        auto *fp = new FakePlatform;
        c.setPlatform(std::unique_ptr<dxr::CursorPlatform>(fp));
        int cbShown = -1, cbCalls = 0;
        c.onDepthCursorShownChanged([&](bool s) {
            cbShown = s;
            cbCalls++;
        });
        c.enable(5);
        for (int frame = 0; frame < 6; frame++) {
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            XrViewState vs{XR_TYPE_VIEW_STATE};
            c.chain(li, vs);
            const bool runtimeWillDraw = frame >= 2 && frame < 4;
            auto *ds = (XrCursorDepthDrawStateDXR *)vs.next; // what the runtime fills
            ds->willDraw = runtimeWillDraw ? XR_TRUE : XR_FALSE;
            c.afterLocate(v, 2);
            CHECK(fp->hidden == runtimeWillDraw && c.depthCursorShown() == runtimeWillDraw,
                  "OS cursor hidden exactly while willDraw");
        }
        CHECK(fp->hides == 1 && fp->shows == 1, "transitions only");
        CHECK(cbCalls == 2 && cbShown == 0, "callback on each transition");
        fp->uv = {-0.1f, 0.5f}; // leaves the canvas while drawn
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        c.chain(li, vs);
        ((XrCursorDepthDrawStateDXR *)vs.next)->willDraw = XR_TRUE; // stale TRUE must not hide it
        c.afterLocate(v, 2);
        CHECK(!fp->hidden, "no hint chained -> OS cursor shown");
    }
    { // Runtime mode on a v4 runtime: nothing would draw -> never hide.
        dxr::CursorDepth c;
        auto *fp = new FakePlatform;
        c.setPlatform(std::unique_ptr<dxr::CursorPlatform>(fp));
        c.enable(4);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        c.chain(li, vs);
        ((XrCursorDepthPlacementDXR *)vs.next)->isActive = XR_TRUE;
        c.afterLocate(v, 2);
        CHECK(!fp->hidden && !c.appShouldDraw(), "pre-v5 runtime mode never hides the OS cursor");
    }
    { // App mode: hidden while the placement is active; appShouldDraw follows.
        dxr::CursorDepth c;
        auto *fp = new FakePlatform;
        c.setPlatform(std::unique_ptr<dxr::CursorPlatform>(fp));
        c.setDrawMode(dxr::CursorDepth::DrawMode::App);
        c.enable(5);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        c.chain(li, vs);
        CHECK(static_cast<const Base *>(li.next)->next == nullptr, "app mode: no draw request");
        ((XrCursorDepthPlacementDXR *)vs.next)->isActive = XR_TRUE;
        c.afterLocate(v, 2);
        CHECK(fp->hidden && c.appShouldDraw(), "app mode: hidden + draw while active");
    }
    { // Destruction while hidden restores the OS cursor.
        FakePlatform probe;
        bool restored = false;
        {
            dxr::CursorDepth c;
            c.enable(5);
            c.setPointerUV(0.5f, 0.5f);
            c.onDepthCursorShownChanged([&](bool s) { restored = !s; });
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            XrViewState vs{XR_TYPE_VIEW_STATE};
            c.chain(li, vs);
            ((XrCursorDepthDrawStateDXR *)vs.next)->willDraw = XR_TRUE;
            c.afterLocate(v, 2);
        }
        CHECK(restored, "destructor shows the OS cursor again");
    }
}

static void
testNearestHit()
{
    // Right eye's rays hit a plane 8 cm in front of the canvas, the left eye's
    // a plane 2 cm in front: the nearest (to the viewer) wins.
    XrView v[2] = {kooima({-0.032f, 0, 0.6f}), kooima({0.032f, 0, 0.6f})};
    dxr::CursorDepth c;
    c.enable(5);
    c.setPointerUV(0.5f, 0.5f);
    int casts = 0;
    c.setRayCast([&](const dxr::CursorRay &r) -> std::optional<float> {
        casts++;
        const float z = r.origin.x > 0.0f ? 0.08f : 0.02f;
        return (z - r.origin.z) / r.dir.z;
    });
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    XrViewState vs{XR_TYPE_VIEW_STATE};
    c.chain(li, vs);
    CHECK(casts == 0 && c.hint().hasContent == XR_FALSE, "first frame: no previous views, no hit test");
    c.afterLocate(v, 2);
    c.chain(li, vs);
    CHECK(casts == 20, "every footprint ray cast once");
    CHECK(c.hint().hasContent == XR_TRUE && near(c.hint().nearestPoint.z, 0.08f), "nearest-to-viewer hit kept");

    c.setRayCast([](const dxr::CursorRay &) -> std::optional<float> { return std::nullopt; });
    c.chain(li, vs);
    CHECK(c.hint().hasContent == XR_FALSE, "all misses -> hasContent false");

    c.useDepthLayer(true);
    casts = 0;
    c.setRayCast([&](const dxr::CursorRay &) -> std::optional<float> {
        casts++;
        return 1.0f;
    });
    c.chain(li, vs);
    CHECK(casts == 0 && c.hint().hasContent == XR_FALSE, "depth-layer source: no app hit test");

    c.useDepthLayer(false);
    c.afterLocate(v, 1); // 2D
    c.chain(li, vs);
    CHECK(casts == 0 && c.hint().hasContent == XR_FALSE, "2D: no rays");
}

static const XrCursorDepthSourceDXR *
findSource(const XrViewLocateInfo &li)
{
    for (const Base *b = static_cast<const Base *>(li.next); b != nullptr; b = static_cast<const Base *>(b->next))
        if (b->type == XR_TYPE_CURSOR_DEPTH_SOURCE_DXR) return reinterpret_cast<const XrCursorDepthSourceDXR *>(b);
    return nullptr;
}

static void
testMeasuredDisparity()
{
    XrView v[2] = {kooima({-0.032f, 0, 0.6f}), kooima({0.032f, 0, 0.6f})};
    int casts = 0;
    auto rayCast = [&](const dxr::CursorRay &) -> std::optional<float> {
        casts++;
        return 1.0f;
    };
    { // v7: the source struct carries MEASURED_DISPARITY; no app hit test, no point.
        dxr::CursorDepth c;
        c.enable(7);
        c.setPointerUV(0.4f, 0.6f);
        c.setRayCast(rayCast);
        c.useMeasuredDisparity(true);
        CHECK(c.measuredDisparity(), "measured source selected");
        c.afterLocate(v, 2);
        casts = 0;
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        c.chain(li, vs);
        const XrCursorDepthSourceDXR *src = findSource(li);
        CHECK(src && src->source == XR_CURSOR_DEPTH_SOURCE_MEASURED_DISPARITY_DXR, "v7: source = MEASURED (2)");
        CHECK(casts == 0 && c.hint().hasContent == XR_FALSE, "v7 measured: no app hit test, no point");
        CHECK(c.hint().cursorUV.x == 0.4f && c.hint().cursorUV.y == 0.6f, "cursorUV still sent");

        // Pointer off the canvas: nothing chained on the locate (zero runtime cost).
        c.setPointerUV(-0.1f, 0.5f);
        XrViewLocateInfo li2{XR_TYPE_VIEW_LOCATE_INFO};
        c.chain(li2, vs);
        CHECK(li2.next == nullptr && findSource(li2) == nullptr, "off the canvas: no request");

        // Off again: the next frame carries no source and the hit test is back.
        c.setPointerUV(0.4f, 0.6f);
        c.useMeasuredDisparity(false);
        XrViewLocateInfo li3{XR_TYPE_VIEW_LOCATE_INFO};
        c.chain(li3, vs);
        CHECK(findSource(li3) == nullptr && casts == 20, "measured off: no source, ray cast resumes");
    }
    { // v6 runtime: measured is not understood -> not sent; the ray cast is the fallback.
        dxr::CursorDepth c;
        c.enable(6);
        c.setPointerUV(0.5f, 0.5f);
        c.setRayCast(rayCast);
        c.useMeasuredDisparity(true);
        c.afterLocate(v, 2);
        casts = 0;
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        c.chain(li, vs);
        CHECK(findSource(li) == nullptr && casts == 20, "v6: no measured request, ray cast used");
    }
    { // The two runtime sources replace each other; turning off the inactive one is a no-op.
        dxr::CursorDepth c;
        c.enable(7);
        c.setPointerUV(0.5f, 0.5f);
        c.useDepthLayer(true);
        c.useMeasuredDisparity(true);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        c.chain(li, vs);
        const XrCursorDepthSourceDXR *src = findSource(li);
        CHECK(src && src->source == XR_CURSOR_DEPTH_SOURCE_MEASURED_DISPARITY_DXR, "last call wins (measured)");
        c.useDepthLayer(false);
        XrViewLocateInfo li2{XR_TYPE_VIEW_LOCATE_INFO};
        c.chain(li2, vs);
        src = findSource(li2);
        CHECK(src && src->source == XR_CURSOR_DEPTH_SOURCE_MEASURED_DISPARITY_DXR,
              "useDepthLayer(false) leaves measured selected");
        c.useDepthLayer(true);
        XrViewLocateInfo li3{XR_TYPE_VIEW_LOCATE_INFO};
        c.chain(li3, vs);
        src = findSource(li3);
        CHECK(src && src->source == XR_CURSOR_DEPTH_SOURCE_SUBMITTED_DEPTH_DXR, "last call wins (depth layer)");
    }
}

int
main()
{
    testCanvasUV();
    testFootprintUV();
    testRaysKooima();
    testChainDisabled();
    testChainOrder();
    testShowHide();
    testNearestHit();
    testMeasuredDisparity();
    if (g_failures == 0) std::printf("cursor_depth_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
