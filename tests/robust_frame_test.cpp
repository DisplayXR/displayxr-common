// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// robust_frame.h contract test: area weighting, outlier rejection, the p75
// screen plane, transforms, determinism and the RobustFitExtent cap. Pure;
// runs everywhere.

#include "robust_frame.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
			++g_fail;                                                                  \
		}                                                                                  \
	} while (0)

static bool
near(float a, float b, float tol)
{
	return std::fabs(a - b) <= tol;
}

// Unit cube centred at the origin, 12 triangles.
static const float kCube[8][3] = {
    {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
    {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},
};
static const uint32_t kCubeIdx[36] = {
    0, 2, 1, 0, 3, 2, // back  (z = -0.5)
    4, 5, 6, 4, 6, 7, // front (z = +0.5)
    0, 1, 5, 0, 5, 4, // bottom
    3, 7, 6, 3, 6, 2, // top
    0, 4, 7, 0, 7, 3, // left
    1, 2, 6, 1, 6, 5, // right
};

static void
add_cube(dxr::RobustFrameSampler &s, const float *transform = nullptr)
{
	s.addTriangles(&kCube[0][0], sizeof(kCube[0]), 8, kCubeIdx, 36, transform);
}

static void
test_cube()
{
	dxr::RobustFrameSampler s;
	add_cube(s);
	CHECK(s.triangleCount() == 12);
	CHECK(near((float)s.totalArea(), 6.0f, 1e-5f));
	dxr::RobustFrame f;
	CHECK(s.compute(f));
	CHECK(f.valid);
	for (int i = 0; i < 3; ++i) {
		CHECK(near(f.median[i], 0.0f, 0.02f));
		CHECK(near(f.boxMin[i], -0.5f, 1e-6f));
		CHECK(near(f.boxMax[i], 0.5f, 1e-6f));
	}
	// z mass: 1/6 at -0.5 (back), 4/6 uniform on [-0.5, 0.5] (sides), 1/6 at
	// +0.5 (front) -> p75 solves 1/6 + 4/6 (z + 0.5) = 0.75 -> z = 0.375.
	CHECK(near(f.zdpDepth, 0.375f, 0.02f));
	CHECK(f.zdpDepth == f.p75[2]);
	CHECK(f.pivot[0] == f.median[0] && f.pivot[2] == f.median[2]);
	CHECK(f.pivot[1] == 0.0f);
	// Pivot and screen plane are separate: the slide puts p75 on the glass.
	CHECK(near(dxr::RobustScreenPlaneShift(f), f.median[2] - 0.375f, 0.03f));
	CHECK(dxr::RobustScreenPlaneShift(dxr::RobustFrame{}) == 0.0f);
	// The front and back faces put 1/6 of the samples exactly on +-0.5, so the
	// trimmed z extent is the full side.
	CHECK(near(f.trimmedExtent[2], 1.0f, 1e-6f));
}

static void
test_thin_outlier()
{
	// The cube plus a long, thin tail reaching back to z = -5: it moves the box
	// centre by 2.25 in z but carries ~0.2% of the area.
	dxr::RobustFrameSampler s;
	add_cube(s);
	const float a[3] = {-0.005f, 0.0f, -0.5f}, b[3] = {0.005f, 0.0f, -0.5f}, c[3] = {0.0f, 0.0f, -5.0f};
	s.addTriangle(a, b, c);
	dxr::RobustFrame f;
	CHECK(s.compute(f));
	CHECK(near(f.boxMin[2], -5.0f, 1e-6f));
	const float boxCentreZ = 0.5f * (f.boxMin[2] + f.boxMax[2]);
	CHECK(near(boxCentreZ, -2.25f, 1e-6f));
	CHECK(near(f.median[2], 0.0f, 0.03f));
	CHECK(near(f.zdpDepth, 0.375f, 0.03f));
	CHECK(f.trimmedExtent[2] < 1.05f); // the tail is trimmed away

	// Fit extent with the default 0.8 fill: the full box from the robust
	// centre (|-5 - zdp| on the far side) bounds it from below at 0.8 / 0.9.
	// The guard is measured about the PIVOT (median z), not the screen plane.
	float ext[3];
	dxr::RobustFitExtent(f.trimmedExtent, f.boxMin, f.boxMax, f.pivot, 0.8f, ext);
	const float symFullZ = 2.0f * (f.pivot[2] + 5.0f);
	CHECK(near(ext[2], symFullZ * 0.8f / 0.9f, 1e-4f));
	CHECK(ext[2] > f.trimmedExtent[2]);
	// The frame form pins the guard to the pivot and reports it.
	float ext2[3], piv[3];
	dxr::RobustFitExtent(f, f.boxMin, f.boxMax, 0.8f, ext2, piv);
	CHECK(std::memcmp(ext, ext2, sizeof(ext)) == 0);
	CHECK(piv[0] == f.pivot[0] && piv[1] == f.pivot[1] && piv[2] == f.pivot[2]);
	// x/y: the box is symmetric about the (near-zero) centre, so the cap
	// (1.0 * 0.8 / 0.9 < 1) never binds and the trimmed extent wins.
	CHECK(near(ext[0], f.trimmedExtent[0], 1e-6f) || ext[0] >= f.trimmedExtent[0]);
}

static void
test_area_not_vertex_weighting()
{
	// One big triangle at z = 0 and a dense fan of 200 tiny ones at z = 1: by
	// vertex count the fan dominates, by area the big triangle does.
	dxr::RobustFrameSampler s;
	const float a[3] = {-1, -1, 0}, b[3] = {1, -1, 0}, c[3] = {0, 1, 0};
	s.addTriangle(a, b, c);
	for (int k = 0; k < 200; ++k) {
		const float x = 0.001f * (float)k;
		const float p[3] = {x, 0, 1}, q[3] = {x + 0.0005f, 0, 1}, r[3] = {x, 0.0005f, 1};
		s.addTriangle(p, q, r);
	}
	dxr::RobustFrame f;
	CHECK(s.compute(f));
	CHECK(f.median[2] == 0.0f);
	CHECK(f.zdpDepth == 0.0f);
}

static void
test_transform_and_stride()
{
	// Column-major translation by (1, 2, 3) and a vertex stride wider than
	// the position (interleaved vertex layout).
	const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
	struct V
	{
		float pos[3];
		float uv[2];
	} verts[8];
	for (int i = 0; i < 8; ++i) {
		std::memcpy(verts[i].pos, kCube[i], sizeof(verts[i].pos));
		verts[i].uv[0] = verts[i].uv[1] = 99.0f;
	}
	dxr::RobustFrameSampler s;
	s.addTriangles(verts[0].pos, sizeof(V), 8, kCubeIdx, 36, m);
	dxr::RobustFrame f;
	CHECK(s.compute(f));
	CHECK(near(f.pivot[0], 1.0f, 0.02f));
	CHECK(near(f.pivot[1], 2.0f, 1e-6f));
	CHECK(near(f.pivot[2], 3.0f, 0.02f));
	CHECK(near(f.zdpDepth, 3.375f, 0.02f));
	CHECK(near(f.boxMin[0], 0.5f, 1e-6f) && near(f.boxMax[2], 3.5f, 1e-6f));
}

static void
test_determinism_and_degenerate()
{
	dxr::RobustFrameSampler s;
	add_cube(s);
	dxr::RobustFrame f1, f2;
	CHECK(s.compute(f1));
	CHECK(s.compute(f2));
	CHECK(std::memcmp(&f1, &f2, sizeof(f1)) == 0);

	// Out-of-range indices, zero-area and non-finite triangles are skipped;
	// nothing usable -> invalid.
	dxr::RobustFrameSampler e;
	const uint32_t bad[3] = {0, 1, 9};
	e.addTriangles(&kCube[0][0], sizeof(kCube[0]), 8, bad, 3);
	const float p[3] = {0, 0, 0}, q[3] = {1, 1, 1}, r[3] = {2, 2, 2};
	e.addTriangle(p, q, r); // collinear
	const float inf = INFINITY;
	const float n0[3] = {inf, 0, 0}, n1[3] = {0, 1, 0}, n2[3] = {0, 0, 1};
	e.addTriangle(n0, n1, n2);
	CHECK(e.triangleCount() == 0);
	dxr::RobustFrame fe;
	CHECK(!e.compute(fe));
	CHECK(!fe.valid);
}

int
main()
{
	test_cube();
	test_thin_outlier();
	test_area_not_vertex_weighting();
	test_transform_and_stride();
	test_determinism_and_degenerate();
	if (g_fail != 0) {
		std::printf("robust_frame_test: %d failure(s)\n", g_fail);
		return 1;
	}
	std::printf("robust_frame_test: OK\n");
	return 0;
}
