// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Robust load-time framing of a mesh: area-weighted surface percentiles
 *         instead of the bounding box for the pivot, the screen plane and the
 *         fit extent.
 *
 * The axis-aligned bounding box does three jobs when framing a just-loaded
 * mesh, and one thin outlier (a tail, an antenna, a strap) skews all three:
 *
 *   1. SCALE — the fit (auto_fit.h) uses the full extent, so the outlier
 *      shrinks the whole subject.
 *   2. PIVOT — the orbit / turntable turns about the box centre.
 *   3. SCREEN PLANE — the box centre lands on the zero-disparity plane, so
 *      whatever lies in front of it pops out of the glass.
 *
 * A thin outlier carries little surface AREA. So: sample the surface uniformly
 * by triangle area (not by vertex, which lets dense detail such as eyes or
 * text dominate), and derive the three numbers separately:
 *
 *   | job                  | rule                                              |
 *   |----------------------|---------------------------------------------------|
 *   | pivot x, z           | area-weighted median                              |
 *   | pivot y              | bounding-box centre (unchanged)                   |
 *   | zero-disparity depth | area-weighted 75th percentile of z (+z toward the |
 *   |                      | viewer), so ~25% of the surface is in front of    |
 *   |                      | the glass                                         |
 *   | fit extent           | per-axis area-weighted 1st..99th percentile, but  |
 *   |                      | never so small that the FULL box, measured about  |
 *   |                      | the pivot, exceeds kRobustFrameCapFill (90%) of   |
 *   |                      | the viewport — see RobustFitExtent()              |
 *
 * THE PIVOT AND THE SCREEN PLANE ARE SEPARATE. The subject turns about
 * `pivot`; the screen plane is applied as a slide along the view axis, in
 * display space, by RobustScreenPlaneShift() = pivot.z - zdpDepth model units
 * (times the fit and zoom scale), so `zdpDepth` sits on the glass at rest and
 * stays there under zoom. Do NOT fold them into one centre
 * (pivot.x, pivot.y, zdpDepth): that gives the same rest pose but turns the
 * subject about the screen plane instead of its median, which is a different
 * picture as soon as it turns. In a model transform:
 *
 *     S = T(0, 0, scale * shift) * Scale(scale) * R^-1 * T(-pivot)
 *
 * This is the shared rule of the native model viewer
 * (displayxr-demo-modelviewer#192) and the web SDK's addModel
 * (displayxr-web#327). The two must frame identically, because an undocked
 * product keeps its size and placement as it leaves the page, so the sampling
 * is fully specified below — a port must reproduce it exactly:
 *
 *   - Triangles are visited in the order added. Each one is placed through its
 *     transform (column-major 4x4, float), its area is
 *     0.5 * |(b - a) x (c - a)| (float, accumulated in double); zero-area and
 *     non-finite triangles are skipped.
 *   - RNG: 64-bit LCG  s = s * 6364136223846793005 + 1442695040888963407
 *     (mod 2^64), seeded with kRobustFrameSeed; each draw is
 *     (s >> 11) * 2^-53, a uniform double in [0, 1).
 *   - Per sample (kRobustFrameSamples of them): r = draw() * totalArea picks
 *     the first triangle whose cumulative area is >= r (lower_bound); then
 *     u = (float)draw(), v = (float)draw(), folded (u + v > 1 -> 1-u, 1-v);
 *     point = a + u (b - a) + v (c - a).
 *   - Percentile q of n sorted samples is element
 *     min(n - 1, (size_t)(q * (float)(n - 1) + 0.5f)).
 *
 * Bind pose. An animated model is sampled at its bind placement; the caller
 * carries the resulting offset (robust centre - box centre) on whatever
 * per-frame re-centring it does, so the screen plane stays put while it moves.
 *
 * Header-only and platform-neutral (displayxr::rules): compiles on Windows,
 * macOS, Linux and Android with no dependency. Cost: one pass over the
 * triangles plus kRobustFrameSamples samples and three sorts — a few ms.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxr {

//! Number of area-weighted surface samples.
inline constexpr int kRobustFrameSamples = 65536;

//! Fixed RNG seed: a model always frames the same.
inline constexpr uint64_t kRobustFrameSeed = 0x9E3779B97F4A7C15ull;

//! The full box, measured from the robust centre, may fill at most this much
//! of the viewport (see RobustFitExtent).
inline constexpr float kRobustFrameCapFill = 0.9f;

//! Result of RobustFrameSampler::compute(). All values are in the space the
//! triangles were transformed into (world space, normally).
struct RobustFrame
{
	bool valid = false;

	//! Area-weighted per-axis percentiles of the surface samples.
	float median[3] = {0, 0, 0};
	float p01[3] = {0, 0, 0};
	float p75[3] = {0, 0, 0};
	float p99[3] = {0, 0, 0};

	//! AABB of the sampled triangles (after their transforms).
	float boxMin[3] = {0, 0, 0};
	float boxMax[3] = {0, 0, 0};

	//! Turn pivot: { median x, box centre y, median z }. The subject turns
	//! about this point; it is NOT what lands on the glass (zdpDepth is).
	float pivot[3] = {0, 0, 0};

	//! Zero-disparity (screen-plane) depth: the 75th-percentile z, +z toward
	//! the viewer, so ~25% of the surface sits in front of the glass. Applied
	//! as a slide (RobustScreenPlaneShift), not by moving the pivot.
	float zdpDepth = 0.0f;

	//! Per-axis 1st..99th percentile extent (FULL sizes, never below 1e-6).
	float trimmedExtent[3] = {0, 0, 0};
};

//! Accumulates triangles (from one or more meshes, each with its own
//! transform), then computes the robust frame.
class RobustFrameSampler
{
public:
	//! Add an indexed triangle list. `positions` points at the first vertex's
	//! x; consecutive vertices are `strideBytes` apart (x, y, z floats at the
	//! start of each). `indices` holds `indexCount` entries (a trailing
	//! partial triangle is ignored; out-of-range indices skip the triangle).
	//! `transform` is an optional column-major 4x4 (nullptr = identity).
	void addTriangles(const float *positions,
	                  size_t strideBytes,
	                  size_t vertexCount,
	                  const uint32_t *indices,
	                  size_t indexCount,
	                  const float *transform = nullptr)
	{
		if (positions == nullptr || indices == nullptr || strideBytes < 3 * sizeof(float)) {
			return;
		}
		const unsigned char *base = reinterpret_cast<const unsigned char *>(positions);
		auto vert = [&](uint32_t i, float out[3]) {
			const float *p = reinterpret_cast<const float *>(base + (size_t)i * strideBytes);
			if (transform == nullptr) {
				out[0] = p[0];
				out[1] = p[1];
				out[2] = p[2];
				return;
			}
			const float *m = transform;
			for (int r = 0; r < 3; ++r) {
				out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
			}
		};
		for (size_t ii = 0; ii + 2 < indexCount; ii += 3) {
			const uint32_t i0 = indices[ii], i1 = indices[ii + 1], i2 = indices[ii + 2];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
				continue;
			}
			Tri t;
			vert(i0, t.a);
			vert(i1, t.b);
			vert(i2, t.c);
			addTriangle(t);
		}
	}

	//! Add one triangle given by its three (already transformed) corners.
	void addTriangle(const float a[3], const float b[3], const float c[3])
	{
		Tri t;
		for (int i = 0; i < 3; ++i) {
			t.a[i] = a[i];
			t.b[i] = b[i];
			t.c[i] = c[i];
		}
		addTriangle(t);
	}

	size_t triangleCount() const { return tris_.size(); }
	double totalArea() const { return total_; }

	//! Compute the frame. Returns false (out.valid = false) when no triangle
	//! with positive area was added.
	bool compute(RobustFrame &out,
	             int samples = kRobustFrameSamples,
	             uint64_t seed = kRobustFrameSeed) const
	{
		out = RobustFrame{};
		if (tris_.empty() || !(total_ > 0.0) || samples <= 0) {
			return false;
		}
		std::vector<float> axis[3];
		for (auto &a : axis) {
			a.reserve((size_t)samples);
		}
		uint64_t s = seed;
		auto rnd = [&]() {
			s = s * 6364136223846793005ull + 1442695040888963407ull;
			return (double)(s >> 11) * (1.0 / 9007199254740992.0);
		};
		for (int k = 0; k < samples; ++k) {
			const double r = rnd() * total_;
			const size_t ti = (size_t)(std::lower_bound(cum_.begin(), cum_.end(), r) - cum_.begin());
			const Tri &t = tris_[std::min(ti, tris_.size() - 1)];
			float u = (float)rnd(), v = (float)rnd();
			if (u + v > 1.0f) {
				u = 1.0f - u;
				v = 1.0f - v;
			}
			for (int i = 0; i < 3; ++i) {
				axis[i].push_back(t.a[i] + u * (t.b[i] - t.a[i]) + v * (t.c[i] - t.a[i]));
			}
		}
		for (int i = 0; i < 3; ++i) {
			std::vector<float> &a = axis[i];
			std::sort(a.begin(), a.end());
			auto pct = [&](float q) {
				return a[std::min(a.size() - 1, (size_t)(q * (float)(a.size() - 1) + 0.5f))];
			};
			out.p01[i] = pct(0.01f);
			out.median[i] = pct(0.50f);
			out.p75[i] = pct(0.75f);
			out.p99[i] = pct(0.99f);
			out.boxMin[i] = boxMin_[i];
			out.boxMax[i] = boxMax_[i];
			out.trimmedExtent[i] = std::max(out.p99[i] - out.p01[i], 1e-6f);
		}
		out.pivot[0] = out.median[0];
		out.pivot[1] = 0.5f * (boxMin_[1] + boxMax_[1]);
		out.pivot[2] = out.median[2];
		out.zdpDepth = out.p75[2];
		out.valid = true;
		return true;
	}

private:
	struct Tri
	{
		float a[3], b[3], c[3];
	};

	void addTriangle(const Tri &t)
	{
		const float e1[3] = {t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2]};
		const float e2[3] = {t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2]};
		const float cx = e1[1] * e2[2] - e1[2] * e2[1];
		const float cy = e1[2] * e2[0] - e1[0] * e2[2];
		const float cz = e1[0] * e2[1] - e1[1] * e2[0];
		const double area = 0.5 * (double)std::sqrt(cx * cx + cy * cy + cz * cz);
		if (!(area > 0.0) || !std::isfinite(area)) {
			return;
		}
		for (int i = 0; i < 3; ++i) {
			const float lo = std::min(t.a[i], std::min(t.b[i], t.c[i]));
			const float hi = std::max(t.a[i], std::max(t.b[i], t.c[i]));
			if (tris_.empty()) {
				boxMin_[i] = lo;
				boxMax_[i] = hi;
			} else {
				boxMin_[i] = std::min(boxMin_[i], lo);
				boxMax_[i] = std::max(boxMax_[i], hi);
			}
		}
		total_ += area;
		tris_.push_back(t);
		cum_.push_back(total_);
	}

	std::vector<Tri> tris_;
	std::vector<double> cum_;
	double total_ = 0.0;
	float boxMin_[3] = {0, 0, 0};
	float boxMax_[3] = {0, 0, 0};
};

//! The extent to hand the fit (auto_fit.h, or an app's wrapper of it) for a
//! subject turning about `centre` (pass the frame's `pivot`):
//!
//!     out[i] = max(trimmed[i], symFull[i] * fill / capFill)
//!     symFull[i] = 2 * max(boxMax[i] - centre[i], centre[i] - boxMin[i])
//!
//! i.e. the 1st..99th-percentile extent, but never so small that the FULL box
//! — measured symmetrically about the pivot, since the pivot is no longer the
//! box centre — would exceed `capFill` of the viewport once the fit scales
//! the binding axis to `fill`. Pass the box the app frames with (it may be
//! wider than frame.boxMin/Max, e.g. an animated model's swept bounds) and the
//! fill the fit uses (kAutoFitDefaultFill = 0.8 unless the user changed it).
inline void
RobustFitExtent(const float trimmed[3],
                const float boxMin[3],
                const float boxMax[3],
                const float centre[3],
                float fill,
                float out[3],
                float capFill = kRobustFrameCapFill)
{
	const float scale = (capFill > 0.0f) ? fill / capFill : 1.0f;
	for (int i = 0; i < 3; ++i) {
		const float symFull = 2.0f * std::max(std::fabs(boxMax[i] - centre[i]), std::fabs(centre[i] - boxMin[i]));
		out[i] = std::max(trimmed[i], symFull * scale);
	}
}

//! The screen-plane slide, in MODEL units: how far the pivot sits in front of
//! the glass once `zdpDepth` is put on it (negative = the subject slides back).
//! Multiply by the model's display scale (fit x zoom) and translate along the
//! view axis in display space, AFTER the rotation about the pivot. Zero for an
//! invalid frame, so a box fallback puts the pivot on the glass as before.
inline float
RobustScreenPlaneShift(const RobustFrame &frame)
{
	return frame.valid ? frame.pivot[2] - frame.zdpDepth : 0.0f;
}

} // namespace dxr
