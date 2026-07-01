// Copyright 2025-2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Type-neutral core of the DisplayXR multiview math — see dxr_view_math.h
 *
 * Moved VERBATIM (mechanical type rename only) from display3d_view.c +
 * camera3d_view.c, which are now thin casts over this file. The previously
 * duplicated ~60 lines of static helpers (quat rotate / view matrix) are
 * shared between the two rigs here — one TU, one copy.
 */

#include "dxr_view_math.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal helpers
// ============================================================================

// Quaternion-rotate a vector: v' = q * v * q^-1
// Uses the efficient cross-product form (no matrix conversion).
static dxr_vec3
quat_rotate(dxr_quat q, dxr_vec3 v)
{
	// t = 2 * cross(q.xyz, v)
	float tx = 2.0f * (q.y * v.z - q.z * v.y);
	float ty = 2.0f * (q.z * v.x - q.x * v.z);
	float tz = 2.0f * (q.x * v.y - q.y * v.x);

	// v' = v + w*t + cross(q.xyz, t)
	dxr_vec3 out;
	out.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
	out.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
	out.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
	return out;
}

// Set a column-major 4x4 matrix to identity.
static void
mat4_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build rotation matrix (column-major) from quaternion.
static void
mat4_from_quat(float *m, dxr_quat q)
{
	float qx = q.x, qy = q.y, qz = q.z, qw = q.w;

	mat4_identity(m);
	m[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
	m[1] = 2.0f * (qx * qy + qz * qw);
	m[2] = 2.0f * (qx * qz - qy * qw);
	m[4] = 2.0f * (qx * qy - qz * qw);
	m[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
	m[6] = 2.0f * (qy * qz + qx * qw);
	m[8] = 2.0f * (qx * qz + qy * qw);
	m[9] = 2.0f * (qy * qz - qx * qw);
	m[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
}

// Build view matrix from display/camera pose and world-space eye position.
// viewMatrix = transpose(R) * translate(-eye_world)
// where R is the rotation matrix from the pose orientation.
static void
build_view_matrix(float *out, dxr_quat orientation, dxr_vec3 eye_world)
{
	// Rotation matrix from quaternion
	float rot[16];
	mat4_from_quat(rot, orientation);

	// Transpose rotation (inverse of orthonormal matrix)
	float inv_rot[16];
	mat4_identity(inv_rot);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			inv_rot[j * 4 + i] = rot[i * 4 + j];

	// Translation: -eye_world
	float inv_trans[16];
	mat4_identity(inv_trans);
	inv_trans[12] = -eye_world.x;
	inv_trans[13] = -eye_world.y;
	inv_trans[14] = -eye_world.z;

	// viewMatrix = inv_rot * inv_trans (column-major multiplication)
	float tmp[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += inv_rot[k * 4 + row] * inv_trans[col * 4 + k];
			}
			tmp[col * 4 + row] = sum;
		}
	}
	memcpy(out, tmp, sizeof(tmp));
}

// Build asymmetric frustum projection (column-major) from tangent half-angles.
static void
build_projection_from_tangents(
    float tan_left, float tan_right, float tan_down, float tan_up, float near_z, float far_z, float *out)
{
	float left = -tan_left * near_z;
	float right = tan_right * near_z;
	float bottom = -tan_down * near_z;
	float top = tan_up * near_z;

	float w = right - left;
	float h = top - bottom;

	memset(out, 0, 16 * sizeof(float));
	out[0] = 2.0f * near_z / w;
	out[5] = 2.0f * near_z / h;
	out[8] = (right + left) / w;
	out[9] = (top + bottom) / h;
	out[10] = -(far_z + near_z) / (far_z - near_z);
	out[11] = -1.0f;
	out[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
}

// Canonical off-axis (Kooima) frustum — homogeneous form. This is the single
// source of truth that both the display-centric and camera-centric pipelines
// build their projection through.
//
// All asymmetry comes from `shear` = eye_offset * invd (dimensionless), so the
// convergence distance D = 1/invd lives in the numerator, never the
// denominator. That makes invd == 0 (convergence at infinity) a regular case:
// shear == 0 => denom == 1 => a symmetric frustum with half-tangents (ro, uo),
// i.e. parallel views. The older affine form (dxr_display3d_compute_projection)
// divides by an absolute eye depth ez and therefore degenerates as the
// convergence distance grows; this form does not. The two are the *same*
// projection for any finite convergence — see dxr_display3d_compute_projection,
// which now maps its (eye, screen) inputs onto this helper.
//
//   ro, uo : half-FOV tangents of the symmetric frustum measured at the
//            convergence plane (horizontal, vertical).
//   shear  : eye_offset * invd. shear.xy bias the lateral asymmetry, shear.z
//            biases the common depth (denom = 1 + shear.z).
//   out_fov: optional (may be NULL) signed FOV angles for the built frustum.
static void
dxr_offaxis_projection(
    float ro, float uo, dxr_vec3 shear, float near_z, float far_z, float *out_matrix, dxr_fov *out_fov)
{
	float denom = 1.0f + shear.z;
	float tan_right = (ro - shear.x) / denom;
	float tan_left = (ro + shear.x) / denom;
	float tan_up = (uo - shear.y) / denom;
	float tan_down = (uo + shear.y) / denom;

	build_projection_from_tangents(tan_left, tan_right, tan_down, tan_up, near_z, far_z, out_matrix);

	if (out_fov) {
		out_fov->angle_left = -atanf(tan_left);
		out_fov->angle_right = atanf(tan_right);
		out_fov->angle_up = atanf(tan_up);
		out_fov->angle_down = -atanf(tan_down);
	}
}

// ── The single Vulkan-render Y-mirror ───────────────────────────────────────
// This is the ONE place the +Y-up world frame is mirrored into the Vulkan
// render frame (Y-down NDC). The render path passes vulkan_flip_y=1 here; the
// pick path passes 0 to stay in the clean world frame. No other code (app,
// pick, geometry) negates Y — so the render and pick frames can only ever
// differ by this one flag, never by hand-rolled sign juggling.
static dxr_vec3
flip_vec_y(dxr_vec3 v, int flip)
{
	if (flip)
		v.y = -v.y;
	return v;
}

static dxr_pose
flip_pose_y(dxr_pose p, int flip)
{
	if (flip)
		p.position.y = -p.position.y;
	return p;
}

// ============================================================================
// Display-centric rig
// ============================================================================

dxr_display3d_tunables
dxr_display3d_default_tunables(void)
{
	dxr_display3d_tunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.perspective_factor = 1.0f;
	t.virtual_display_height = 0.0f;
	return t;
}

void
dxr_display3d_resolve_window_rect(const dxr_window_placement *p,
                                  const dxr_vec3 *raw_eyes,
                                  uint32_t count,
                                  dxr_screen *out_screen,
                                  dxr_vec3 *out_eyes)
{
	float px = (p->display_width_px != 0.0f) ? p->display_width_m / p->display_width_px : 0.0f;
	float py = (p->display_height_px != 0.0f) ? p->display_height_m / p->display_height_px : 0.0f;

	// Kooima screen = the rect's physical size in meters.
	out_screen->width_m = p->rect_width_px * px;
	out_screen->height_m = p->rect_height_px * py;

	// Rect-center -> display-center offset (meters). Display/screen pixels are
	// Y-down with the origin top-left; display/eye space is Y-up, so the Y
	// component is negated. Eyes are shifted so they read relative to the rect
	// center instead of the display center.
	float off_x = (p->rect_center_x_px - p->display_width_px * 0.5f) * px;
	float off_y = -((p->rect_center_y_px - p->display_height_px * 0.5f) * py);

	for (uint32_t i = 0; i < count; i++) {
		out_eyes[i].x = raw_eyes[i].x - off_x;
		out_eyes[i].y = raw_eyes[i].y - off_y;
		out_eyes[i].z = raw_eyes[i].z;
	}
}

void
dxr_display3d_apply_eye_factors(const dxr_vec3 *raw_left,
                                const dxr_vec3 *raw_right,
                                const dxr_vec3 *nominal_viewer,
                                float ipd_factor,
                                float parallax_factor,
                                dxr_vec3 *out_left,
                                dxr_vec3 *out_right)
{
	// Default nominal viewer if NULL (only z is used — x/y lerp toward origin)
	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Step 1: IPD factor — scale inter-eye vector, keep center fixed
	float cx = (raw_left->x + raw_right->x) * 0.5f;
	float cy = (raw_left->y + raw_right->y) * 0.5f;
	float cz = (raw_left->z + raw_right->z) * 0.5f;

	float lvx = (raw_left->x - cx) * ipd_factor;
	float lvy = (raw_left->y - cy) * ipd_factor;
	float lvz = (raw_left->z - cz) * ipd_factor;

	float rvx = (raw_right->x - cx) * ipd_factor;
	float rvy = (raw_right->y - cy) * ipd_factor;
	float rvz = (raw_right->z - cz) * ipd_factor;

	// Step 2: Parallax factor — lerp center toward (0, 0, nom_z).
	// We use origin for x/y so that reducing parallax drives the viewpoint
	// to the display-center axis rather than to an arbitrary nominal offset.
	float cx2 = parallax_factor * cx;
	float cy2 = parallax_factor * cy;
	float cz2 = nom_z + parallax_factor * (cz - nom_z);

	out_left->x = cx2 + lvx;
	out_left->y = cy2 + lvy;
	out_left->z = cz2 + lvz;

	out_right->x = cx2 + rvx;
	out_right->y = cy2 + rvy;
	out_right->z = cz2 + rvz;
}

void
dxr_display3d_apply_eye_factors_n(const dxr_vec3 *raw_eyes,
                                  uint32_t count,
                                  const dxr_vec3 *nominal_viewer,
                                  float ipd_factor,
                                  float parallax_factor,
                                  dxr_vec3 *out_eyes)
{
	if (count == 0) {
		return;
	}

	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Compute centroid of all eyes
	float cx = 0.0f, cy = 0.0f, cz = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		cx += raw_eyes[i].x;
		cy += raw_eyes[i].y;
		cz += raw_eyes[i].z;
	}
	float inv_n = 1.0f / (float)count;
	cx *= inv_n;
	cy *= inv_n;
	cz *= inv_n;

	// Parallax factor: lerp center toward (0, 0, nom_z)
	float cx2 = parallax_factor * cx;
	float cy2 = parallax_factor * cy;
	float cz2 = nom_z + parallax_factor * (cz - nom_z);

	// IPD factor: scale each eye's offset from center
	for (uint32_t i = 0; i < count; i++) {
		out_eyes[i].x = cx2 + (raw_eyes[i].x - cx) * ipd_factor;
		out_eyes[i].y = cy2 + (raw_eyes[i].y - cy) * ipd_factor;
		out_eyes[i].z = cz2 + (raw_eyes[i].z - cz) * ipd_factor;
	}
}

dxr_fov
dxr_display3d_compute_fov(dxr_vec3 eye_pos, float screen_width_m, float screen_height_m)
{
	float ez = eye_pos.z;
	if (ez <= 0.001f)
		ez = 0.65f; // Fallback: ~arm's length

	float half_w = screen_width_m / 2.0f;
	float half_h = screen_height_m / 2.0f;
	float ex = eye_pos.x;
	float ey = eye_pos.y;

	dxr_fov fov;
	fov.angle_left = atanf((-half_w - ex) / ez);
	fov.angle_right = atanf((half_w - ex) / ez);
	fov.angle_up = atanf((half_h - ey) / ez);
	fov.angle_down = atanf((-half_h - ey) / ez);

	return fov;
}

void
dxr_display3d_compute_projection(
    dxr_vec3 eye_pos, float screen_width_m, float screen_height_m, float near_z, float far_z, float *out_matrix)
{
	float ez = eye_pos.z;
	if (ez <= 0.001f)
		ez = 0.65f;

	// Display-centric mapping onto the canonical off-axis frustum: the
	// convergence plane is the screen (z=0), so the convergence distance is the
	// eye-to-screen distance ez (invd = 1/ez) and the base half-tangents are the
	// screen half-extents seen from the eye. shear.z = 0 because the reference is
	// taken at the eye's own depth. Mathematically identical to the former inline
	// similar-triangle form — now expressed through the shared helper so there is
	// one matrix builder, not two. See dxr_offaxis_projection.
	float invd = 1.0f / ez;
	float ro = (screen_width_m * 0.5f) * invd;
	float uo = (screen_height_m * 0.5f) * invd;
	dxr_vec3 shear = {eye_pos.x * invd, eye_pos.y * invd, 0.0f};

	dxr_offaxis_projection(ro, uo, shear, near_z, far_z, out_matrix, NULL);
}

void
dxr_display3d_compute_view(const dxr_vec3 *processed_eye,
                           const dxr_screen *screen,
                           const dxr_display3d_tunables *tunables,
                           const dxr_pose *display_pose,
                           float near_offset,
                           float far_offset,
                           dxr_display3d_view *out)
{
	dxr_display3d_tunables t = tunables ? *tunables : dxr_display3d_default_tunables();

	dxr_quat disp_ori = {0, 0, 0, 1};
	dxr_vec3 disp_pos = {0, 0, 0};
	if (display_pose) {
		disp_ori = display_pose->orientation;
		disp_pos = display_pose->position;
	}

	float m2v = t.virtual_display_height / screen->height_m;

	// Apply perspective * m2v to eye XYZ
	float es = t.perspective_factor * m2v;
	dxr_vec3 eye_scaled;
	eye_scaled.x = processed_eye->x * es;
	eye_scaled.y = processed_eye->y * es;
	eye_scaled.z = processed_eye->z * es;
	out->eye_display = eye_scaled;

	// Apply m2v to screen dimensions
	float kScreenW = screen->width_m * m2v;
	float kScreenH = screen->height_m * m2v;

	// Transform display-space eye -> world-space via display_pose
	dxr_vec3 eye_world = quat_rotate(disp_ori, eye_scaled);
	eye_world.x += disp_pos.x;
	eye_world.y += disp_pos.y;
	eye_world.z += disp_pos.z;
	out->eye_world = eye_world;

	// ZDP-relative clip planes: near/far placed at fixed *absolute* offsets in
	// front of / behind this eye's perpendicular distance to the display/
	// convergence plane (ZDP). eye_scaled.z is that per-eye distance (==
	// out->eye_display.z) in the same kooima-scaled units as the offsets, so
	// the planes are per-eye and the offsets are expressed in virtual-display-
	// height (vH) units by the caller. near = ez - near_offset, far = ez +
	// far_offset. far_offset = 0 => far sits at the ZDP (foreground-only, e.g.
	// transparent mode). Offsets are absolute (vH multiples), NOT fractions of
	// ez — large scenes no longer scale the band with ez.
	float ez = eye_scaled.z;
	float near_z = ez - near_offset;
	float far_z = ez + far_offset;
	if (near_z < 1.0e-4f)
		near_z = 1.0e-4f;
	// Guarantee a valid frustum (far strictly past near) even when ez is tiny
	// and far_offset is 0 (transparent mode at near-degenerate eye distance).
	if (far_z < near_z + 1.0e-4f)
		far_z = near_z + 1.0e-4f;
	// Expose the resolved view-space planes so the caller can drive the
	// renderer's explicit geometric near/far culls (e.g. a splat rasterizer
	// that does not clip against the projection matrix planes).
	out->near_z = near_z;
	out->far_z = far_z;

	// Build view matrix + projection + FOV
	build_view_matrix(out->view_matrix, disp_ori, eye_world);
	out->orientation = disp_ori;
	dxr_display3d_compute_projection(eye_scaled, kScreenW, kScreenH, near_z, far_z, out->projection_matrix);
	out->fov = dxr_display3d_compute_fov(eye_scaled, kScreenW, kScreenH);
}

void
dxr_display3d_compute_views(const dxr_vec3 *raw_eyes,
                            uint32_t count,
                            const dxr_vec3 *nominal_viewer,
                            const dxr_screen *screen,
                            const dxr_display3d_tunables *tunables,
                            const dxr_pose *display_pose,
                            float near_offset,
                            float far_offset,
                            int vulkan_flip_y,
                            dxr_display3d_view *out_views)
{
	dxr_display3d_tunables t = tunables ? *tunables : dxr_display3d_default_tunables();

	if (count == 0)
		return;

	// Mirror inputs into the render frame (vulkan_flip_y) — single source of
	// truth for the render-vs-pick frame relationship. Callers always supply
	// clean +Y-up world-space inputs.
	dxr_vec3 stack_eyes[8];
	dxr_vec3 *eyes = (count <= 8) ? stack_eyes : (dxr_vec3 *)malloc(count * sizeof(dxr_vec3));
	for (uint32_t i = 0; i < count; i++)
		eyes[i] = flip_vec_y(raw_eyes[i], vulkan_flip_y);
	dxr_vec3 nom_local;
	const dxr_vec3 *nom = nominal_viewer;
	if (nominal_viewer) {
		nom_local = flip_vec_y(*nominal_viewer, vulkan_flip_y);
		nom = &nom_local;
	}
	dxr_pose pose_local;
	const dxr_pose *pose = display_pose;
	if (display_pose) {
		pose_local = flip_pose_y(*display_pose, vulkan_flip_y);
		pose = &pose_local;
	}

	// Apply IPD and parallax factors (N-view path)
	dxr_vec3 stack_processed[8];
	dxr_vec3 *processed = (count <= 8) ? stack_processed : (dxr_vec3 *)malloc(count * sizeof(dxr_vec3));
	dxr_display3d_apply_eye_factors_n(eyes, count, nom, t.ipd_factor, t.parallax_factor, processed);

	// Compute each view via single-eye primitive
	for (uint32_t i = 0; i < count; i++) {
		dxr_display3d_compute_view(&processed[i], screen, &t, pose, near_offset, far_offset, &out_views[i]);
	}

	if (count > 8) {
		free(processed);
		free(eyes);
	}
}

void
dxr_display3d_compute_center_view(const dxr_vec3 *raw_eyes,
                                  uint32_t count,
                                  const dxr_vec3 *nominal_viewer,
                                  const dxr_screen *screen,
                                  const dxr_display3d_tunables *tunables,
                                  const dxr_pose *display_pose,
                                  float near_offset,
                                  float far_offset,
                                  int vulkan_flip_y,
                                  dxr_display3d_view *out_view)
{
	dxr_display3d_tunables t = tunables ? *tunables : dxr_display3d_default_tunables();
	if (count == 0)
		return;

	// Cyclopean eye = centroid of the raw eyes. (Averaging then mirroring Y is
	// identical to mirroring each then averaging — the flip is linear.)
	dxr_vec3 c = {0, 0, 0};
	for (uint32_t i = 0; i < count; i++) {
		c.x += raw_eyes[i].x;
		c.y += raw_eyes[i].y;
		c.z += raw_eyes[i].z;
	}
	float inv = 1.0f / (float)count;
	c.x *= inv;
	c.y *= inv;
	c.z *= inv;
	c = flip_vec_y(c, vulkan_flip_y);

	dxr_vec3 nom_local;
	const dxr_vec3 *nom = nominal_viewer;
	if (nominal_viewer) {
		nom_local = flip_vec_y(*nominal_viewer, vulkan_flip_y);
		nom = &nom_local;
	}
	dxr_pose pose_local;
	const dxr_pose *pose = display_pose;
	if (display_pose) {
		pose_local = flip_pose_y(*display_pose, vulkan_flip_y);
		pose = &pose_local;
	}

	// Run the SAME eye-factor pipeline as compute_views on the single centroid.
	// For a single eye the IPD term is a no-op, leaving the parallax-lerped
	// center — exactly the average of the per-eye processed positions.
	dxr_vec3 processed;
	dxr_display3d_apply_eye_factors_n(&c, 1, nom, t.ipd_factor, t.parallax_factor, &processed);
	dxr_display3d_compute_view(&processed, screen, &t, pose, near_offset, far_offset, out_view);
}

void
dxr_display3d_pose_slerp(const dxr_pose *from, const dxr_pose *to, float t, dxr_pose *out)
{
	// Linear lerp position
	out->position.x = from->position.x + (to->position.x - from->position.x) * t;
	out->position.y = from->position.y + (to->position.y - from->position.y) * t;
	out->position.z = from->position.z + (to->position.z - from->position.z) * t;

	// Shortest-arc slerp orientation
	dxr_quat q0 = from->orientation;
	dxr_quat q1 = to->orientation;
	float dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
	if (dot < 0.0f) {
		q1.x = -q1.x;
		q1.y = -q1.y;
		q1.z = -q1.z;
		q1.w = -q1.w;
		dot = -dot;
	}
	float k0, k1;
	if (dot > 0.9995f) {
		// Nearly identical — fall back to linear to avoid numerical issues
		k0 = 1.0f - t;
		k1 = t;
	} else {
		float theta = acosf(dot);
		float sin_theta = sinf(theta);
		k0 = sinf((1.0f - t) * theta) / sin_theta;
		k1 = sinf(t * theta) / sin_theta;
	}
	dxr_quat r;
	r.x = k0 * q0.x + k1 * q1.x;
	r.y = k0 * q0.y + k1 * q1.y;
	r.z = k0 * q0.z + k1 * q1.z;
	r.w = k0 * q0.w + k1 * q1.w;
	float norm = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
	if (norm > 1e-8f) {
		float inv = 1.0f / norm;
		r.x *= inv;
		r.y *= inv;
		r.z *= inv;
		r.w *= inv;
	} else {
		r.w = 1.0f;
		r.x = r.y = r.z = 0.0f;
	}
	out->orientation = r;
}

static dxr_vec3
vec3_normalize(dxr_vec3 v)
{
	float n = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (n < 1e-8f) {
		dxr_vec3 z = {0, 0, 0};
		return z;
	}
	float inv = 1.0f / n;
	dxr_vec3 r = {v.x * inv, v.y * inv, v.z * inv};
	return r;
}

static dxr_vec3
vec3_cross(dxr_vec3 a, dxr_vec3 b)
{
	dxr_vec3 r;
	r.x = a.y * b.z - a.z * b.y;
	r.y = a.z * b.x - a.x * b.z;
	r.z = a.x * b.y - a.y * b.x;
	return r;
}

void
dxr_display3d_align_pose_to_ray(dxr_vec3 hit_world, dxr_vec3 ray_dir_world, dxr_vec3 up_hint, dxr_pose *out)
{
	// Display convention: viewer sits at display-local +Z; eyes look toward
	// local -Z (into the display).  For the viewer to remain approximately in
	// place after focus, the new display's local +Z (in world) must point from
	// the hit back toward the old viewer — i.e. opposite the click ray.
	dxr_vec3 z_axis = {-ray_dir_world.x, -ray_dir_world.y, -ray_dir_world.z};
	z_axis = vec3_normalize(z_axis);
	if (z_axis.x == 0 && z_axis.y == 0 && z_axis.z == 0) {
		z_axis.x = 0;
		z_axis.y = 0;
		z_axis.z = 1;
	}

	// X = normalize(cross(up_hint, Z)); fall back if parallel
	dxr_vec3 x_axis = vec3_cross(up_hint, z_axis);
	float x_len2 = x_axis.x * x_axis.x + x_axis.y * x_axis.y + x_axis.z * x_axis.z;
	if (x_len2 < 1e-8f) {
		dxr_vec3 alt = {1, 0, 0};
		x_axis = vec3_cross(alt, z_axis);
		x_len2 = x_axis.x * x_axis.x + x_axis.y * x_axis.y + x_axis.z * x_axis.z;
		if (x_len2 < 1e-8f) {
			dxr_vec3 alt2 = {0, 0, 1};
			x_axis = vec3_cross(alt2, z_axis);
		}
	}
	x_axis = vec3_normalize(x_axis);

	// Y = cross(Z, X) — right-handed, already unit length
	dxr_vec3 y_axis = vec3_cross(z_axis, x_axis);

	// Rotation matrix columns = [X, Y, Z]; extract quaternion (trace method).
	float m00 = x_axis.x, m10 = x_axis.y, m20 = x_axis.z;
	float m01 = y_axis.x, m11 = y_axis.y, m21 = y_axis.z;
	float m02 = z_axis.x, m12 = z_axis.y, m22 = z_axis.z;

	float tr = m00 + m11 + m22;
	dxr_quat q;
	if (tr > 0.0f) {
		float s = sqrtf(tr + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m21 - m12) / s;
		q.y = (m02 - m20) / s;
		q.z = (m10 - m01) / s;
	} else if (m00 > m11 && m00 > m22) {
		float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		q.w = (m21 - m12) / s;
		q.x = 0.25f * s;
		q.y = (m01 + m10) / s;
		q.z = (m02 + m20) / s;
	} else if (m11 > m22) {
		float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		q.w = (m02 - m20) / s;
		q.x = (m01 + m10) / s;
		q.y = 0.25f * s;
		q.z = (m12 + m21) / s;
	} else {
		float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		q.w = (m10 - m01) / s;
		q.x = (m02 + m20) / s;
		q.y = (m12 + m21) / s;
		q.z = 0.25f * s;
	}

	out->position = hit_world;
	out->orientation = q;
}

void
dxr_display3d_unproject_ndc_to_ray(float ndc_x,
                                   float ndc_y,
                                   const float *V,
                                   const float *P,
                                   dxr_vec3 *out_origin_world,
                                   dxr_vec3 *out_dir_world)
{
	// View-space direction using Kooima-friendly shortcut.
	float vx = (ndc_x + P[8]) / P[0];
	float vy = (ndc_y + P[9]) / P[5];
	float vz = -1.0f;

	// World direction = R^T * view_dir (R = upper-left 3x3 of V, col-major).
	float wx = V[0] * vx + V[1] * vy + V[2] * vz;
	float wy = V[4] * vx + V[5] * vy + V[6] * vz;
	float wz = V[8] * vx + V[9] * vy + V[10] * vz;
	float len = sqrtf(wx * wx + wy * wy + wz * wz);
	if (len < 1e-8f) {
		out_dir_world->x = 0;
		out_dir_world->y = 0;
		out_dir_world->z = -1;
	} else {
		float inv = 1.0f / len;
		out_dir_world->x = wx * inv;
		out_dir_world->y = wy * inv;
		out_dir_world->z = wz * inv;
	}

	// World eye = -R^T * V_translation (V[12], V[13], V[14]).
	float tx = V[12], ty = V[13], tz = V[14];
	out_origin_world->x = -(V[0] * tx + V[1] * ty + V[2] * tz);
	out_origin_world->y = -(V[4] * tx + V[5] * ty + V[6] * tz);
	out_origin_world->z = -(V[8] * tx + V[9] * ty + V[10] * tz);
}

// Column-major 4x4 * vec4.
static void
mat4_mul_vec4_cm(const float *m, const float *v, float *out)
{
	for (int r = 0; r < 4; r++)
		out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] + m[3 * 4 + r] * v[3];
}

int
dxr_display3d_selftest(void)
{
	int fails = 0;

	dxr_screen screen = {0.30f, 0.20f};
	dxr_display3d_tunables t = dxr_display3d_default_tunables();
	t.virtual_display_height = 0.20f;
	dxr_pose pose;
	pose.orientation = (dxr_quat){0, 0, 0, 1};
	pose.position = (dxr_vec3){0, 0, 0};
	dxr_vec3 eye = {0, 0, 0.6f};

	// Picking (clean) frame view. Offsets are absolute (vH units); the round-
	// trip/orientation checks below only need a valid frustum.
	dxr_display3d_view v;
	dxr_display3d_compute_center_view(&eye, 1, NULL, &screen, &t, &pose, t.virtual_display_height,
	                                  1000.0f * t.virtual_display_height,
	                                  /*vulkan_flip_y=*/0, &v);

	// (a) Round-trip: project a world point, then unproject its NDC; the ray
	//     must pass through the original point.
	dxr_vec3 pts[3] = {{0.0f, 0.0f, -0.30f}, {0.05f, 0.04f, -0.50f}, {-0.06f, 0.03f, -0.80f}};
	for (int i = 0; i < 3; i++) {
		float w[4] = {pts[i].x, pts[i].y, pts[i].z, 1.0f};
		float vc[4], cl[4];
		mat4_mul_vec4_cm(v.view_matrix, w, vc);
		mat4_mul_vec4_cm(v.projection_matrix, vc, cl);
		if (fabsf(cl[3]) < 1e-6f) {
			fails++;
			continue;
		}
		float ndcx = cl[0] / cl[3], ndcy = cl[1] / cl[3];
		dxr_vec3 ro, rd;
		dxr_display3d_unproject_ndc_to_ray(ndcx, ndcy, v.view_matrix, v.projection_matrix, &ro, &rd);
		float dx = pts[i].x - ro.x, dy = pts[i].y - ro.y, dz = pts[i].z - ro.z;
		float tt = dx * rd.x + dy * rd.y + dz * rd.z;
		float ex = dx - tt * rd.x, ey = dy - tt * rd.y, ez = dz - tt * rd.z;
		float perp = sqrtf(ex * ex + ey * ey + ez * ez);
		if (perp > 1e-3f) {
			fails++;
			fprintf(stderr, "[dxr_display3d_selftest] round-trip perp=%.5f for point (%.2f,%.2f,%.2f)\n",
			        perp, pts[i].x, pts[i].y, pts[i].z);
		}
	}

	// (b) Orientation: +NDC.y must map to higher world Y, +NDC.x to higher world X.
	dxr_vec3 roU, rdU, roD, rdD, roR, rdR, roL, rdL;
	dxr_display3d_unproject_ndc_to_ray(0.0f, 0.5f, v.view_matrix, v.projection_matrix, &roU, &rdU);
	dxr_display3d_unproject_ndc_to_ray(0.0f, -0.5f, v.view_matrix, v.projection_matrix, &roD, &rdD);
	if (!((roU.y + rdU.y) > (roD.y + rdD.y))) {
		fails++;
		fprintf(stderr, "[dxr_display3d_selftest] +NDC.y did not map to higher world Y (%.3f <= %.3f)\n",
		        roU.y + rdU.y, roD.y + rdD.y);
	}
	dxr_display3d_unproject_ndc_to_ray(0.5f, 0.0f, v.view_matrix, v.projection_matrix, &roR, &rdR);
	dxr_display3d_unproject_ndc_to_ray(-0.5f, 0.0f, v.view_matrix, v.projection_matrix, &roL, &rdL);
	if (!((roR.x + rdR.x) > (roL.x + rdL.x))) {
		fails++;
		fprintf(stderr, "[dxr_display3d_selftest] +NDC.x did not map to higher world X (%.3f <= %.3f)\n",
		        roR.x + rdR.x, roL.x + rdL.x);
	}

	// (c) Rig round-trip + equivalence. display→camera→display recovers the
	//     display rig EXACTLY (vH, persp, ipd, parallax) — the m2v→ipd
	//     normalization (display→camera emits m2v=1, ipd*=es) is drift-free. And
	//     a display rig and its camera twin render identically through the N-view
	//     path, which applies ipd/parallax (where the eye-scale now lives) — a
	//     stereo pair exercises the per-eye disparity, not just the centroid.
	//     (Frustum elems [0],[5],[8],[9] + the view matrix are near/far-independent.)
	dxr_rig_display_info info = {screen.height_m, screen.width_m / screen.height_m, 0.6f};
	float persps[3] = {0.5f, 1.0f, 2.0f};
	dxr_vec3 nomv = {0, 0, 0.6f};
	dxr_vec3 pairs[2][2] = {
	    {{-0.03f, 0.0f, 0.6f}, {0.03f, 0.0f, 0.6f}},   // centered stereo pair
	    {{-0.01f, 0.04f, 0.6f}, {0.05f, 0.04f, 0.6f}}, // off-centre-centroid stereo pair
	};
	for (int pi = 0; pi < 3; pi++) {
		dxr_display_rig dr = {{{0, 0, 0, 1}, {0, 0, 0}}, 0.20f, 1.0f, 1.0f, persps[pi]};
		dxr_camera_rig cr;
		dxr_view_rig_display_to_camera(&dr, &info, &cr);

		// Round-trip identity (params, incl. ipd/parallax — drift-free under A).
		dxr_display_rig dr2;
		(void)dxr_view_rig_camera_to_display(&cr, &info, &dr2);
		if (fabsf(dr2.virtual_display_height - dr.virtual_display_height) > 1e-3f ||
		    fabsf(dr2.perspective_factor - dr.perspective_factor) > 1e-3f ||
		    fabsf(dr2.ipd_factor - dr.ipd_factor) > 1e-3f ||
		    fabsf(dr2.parallax_factor - dr.parallax_factor) > 1e-3f) {
			fails++;
			fprintf(stderr,
			        "[dxr_display3d_selftest] rig round-trip drift at persp=%.2f (vH=%.4f persp=%.4f ipd=%.4f)\n",
			        persps[pi], dr2.virtual_display_height, dr2.perspective_factor, dr2.ipd_factor);
		}

		// Per-eye frustum + view-matrix match through the N-view (ipd-applied) path.
		dxr_display3d_tunables dt = {dr.ipd_factor, dr.parallax_factor, dr.perspective_factor,
		                             dr.virtual_display_height};
		dxr_camera3d_tunables ct = {cr.ipd_factor, cr.parallax_factor, cr.inv_convergence_distance,
		                            cr.half_tan_vfov, cr.m2v};
		for (int s = 0; s < 2; s++) {
			dxr_display3d_view dv[2];
			dxr_camera3d_view cv[2];
			dxr_display3d_compute_views(pairs[s], 2, &nomv, &screen, &dt, &dr.pose, 0.05f, 100.0f, 0, dv);
			dxr_camera3d_compute_views(pairs[s], 2, &nomv, &screen, &ct, &cr.pose, 0.1f, 100.0f, cv);
			for (int e = 0; e < 2; e++) {
				int idx[4] = {0, 5, 8, 9};
				for (int k = 0; k < 4; k++) {
					float d = fabsf(dv[e].projection_matrix[idx[k]] - cv[e].projection_matrix[idx[k]]);
					if (d > 1e-3f) {
						fails++;
						fprintf(stderr,
						        "[dxr_display3d_selftest] frustum mismatch persp=%.2f pair=%d eye=%d elem=%d diff=%.2e\n",
						        persps[pi], s, e, idx[k], d);
					}
				}
				for (int k = 0; k < 16; k++) {
					float d = fabsf(dv[e].view_matrix[k] - cv[e].view_matrix[k]);
					if (d > 1e-3f) {
						fails++;
						fprintf(stderr,
						        "[dxr_display3d_selftest] view-matrix mismatch persp=%.2f pair=%d eye=%d k=%d diff=%.2e\n",
						        persps[pi], s, e, k, d);
						break;
					}
				}
			}
		}
	}

	// (d) INDEPENDENT ground truth: start from a CAMERA rig, convert it to a
	//     display rig, and check the converted display rig reproduces the
	//     camera rig's OWN output (fov + eye_world) at the reference eye
	//     (0,0,N) — the untracked/centred viewer. Unlike (c), this does not
	//     round-trip through a display rig first, so it catches a converter
	//     that is self-consistent (round-trips) yet wrong against the camera
	//     it claims to mirror. The vH value is asserted separately because at
	//     the reference eye vH cancels out of both fov and eye_world (only its
	//     stored VALUE matters — e.g. for a disturbance-free qwerty P-toggle).
	{
		struct {
			float invd, ht, m2v, H, aspect, N, want_vH;
		} cases[] = {
		    // qwerty default: 0.5 dp / 36° vFOV → display default vH = 1.30 m.
		    {0.5f, 0.3249f, 1.0f, 0.194f, 1.7783f, 0.6f, 1.2996f},
		    // a wide + a narrow convergence, different panel/distance.
		    {1.0f, 0.4f, 1.0f, 0.30f, 1.6f, 0.8f, 0.8f},
		    {0.25f, 0.2f, 2.0f, 0.20f, 1.5f, 0.5f, 1.6f},
		};
		for (int ci = 0; ci < 3; ci++) {
			float H = cases[ci].H, N = cases[ci].N, aspect = cases[ci].aspect;
			dxr_screen scr = {H * aspect, H};
			dxr_rig_display_info info = {H, aspect, N};
			dxr_pose pcam = {{0, 0, 0, 1}, {0, 0, 0}};
			dxr_camera_rig cam = {pcam, 1.0f, 1.0f, cases[ci].invd, cases[ci].ht, cases[ci].m2v};

			// Camera ground truth at the reference (centred) eye.
			dxr_vec3 eye = {0, 0, N};
			dxr_camera3d_tunables ct = {cam.ipd_factor, cam.parallax_factor,
			                            cam.inv_convergence_distance, cam.half_tan_vfov, cam.m2v};
			dxr_camera3d_view cv;
			dxr_camera3d_compute_view(&eye, N, &scr, &ct, &cam.pose, 0.1f, 100.0f, &cv);

			// Convert → display, then compute the same eye through the display rig.
			dxr_display_rig dr;
			dxr_view_rig_camera_to_display(&cam, &info, &dr);
			dxr_display3d_tunables dt = {dr.ipd_factor, dr.parallax_factor, dr.perspective_factor,
			                             dr.virtual_display_height};
			dxr_display3d_view dv;
			dxr_display3d_compute_view(&eye, &scr, &dt, &dr.pose, 0.05f, 100.0f, &dv);

			if (fabsf(dr.virtual_display_height - cases[ci].want_vH) > 1.0e-3f) {
				fails++;
				fprintf(stderr,
				        "[dxr_display3d_selftest] camera→display vH=%.4f, want %.4f (case %d)\n",
				        dr.virtual_display_height, cases[ci].want_vH, ci);
			}

			float dfov[4] = {dv.fov.angle_left - cv.fov.angle_left, dv.fov.angle_right - cv.fov.angle_right,
			                 dv.fov.angle_up - cv.fov.angle_up, dv.fov.angle_down - cv.fov.angle_down};
			for (int k = 0; k < 4; k++) {
				if (fabsf(dfov[k]) > 1.0e-3f) {
					fails++;
					fprintf(stderr,
					        "[dxr_display3d_selftest] camera→display FOV[%d] diff=%.2e (case %d)\n", k,
					        dfov[k], ci);
				}
			}
			float dex = dv.eye_world.x - cv.eye_world.x;
			float dey = dv.eye_world.y - cv.eye_world.y;
			float dez = dv.eye_world.z - cv.eye_world.z;
			if (fabsf(dex) > 1.0e-3f || fabsf(dey) > 1.0e-3f || fabsf(dez) > 1.0e-3f) {
				fails++;
				fprintf(stderr,
				        "[dxr_display3d_selftest] camera→display eye_world diff=(%.2e,%.2e,%.2e) (case %d)\n",
				        dex, dey, dez, ci);
			}

			// dxr_rig_max_ipd_factor: M = 1/f is the camera ipd whose display
			// equivalent is exactly 1. cam.ipd_factor==1 here, so dr.ipd_factor == f
			// and M must equal 1/dr.ipd_factor; converting a rig at ipd=M must then
			// yield a display ipd of 1.
			float M = dxr_rig_max_ipd_factor(&cam, &info);
			if (fabsf(M - 1.0f / dr.ipd_factor) > 1.0e-3f * M) {
				fails++;
				fprintf(stderr,
				        "[dxr_display3d_selftest] max_ipd_factor=%.4f, want %.4f (case %d)\n", M,
				        1.0f / dr.ipd_factor, ci);
			}
			dxr_camera_rig camM = cam;
			camM.ipd_factor = M;
			dxr_display_rig drM;
			dxr_view_rig_camera_to_display(&camM, &info, &drM);
			if (fabsf(drM.ipd_factor - 1.0f) > 1.0e-3f) {
				fails++;
				fprintf(stderr,
				        "[dxr_display3d_selftest] display ipd at M=%.4f is %.4f, want 1 (case %d)\n", M,
				        drM.ipd_factor, ci);
			}
		}
	}

	// (e) OFF-EYE disparity: a reconverged (non-display-compatible) camera rig
	//     must still convert EXACTLY through the N-view path (which applies the
	//     ipd/parallax factors) — the ipd-rescale makes per-eye disparity match,
	//     not just the cyclopean framing part (d) checks. Without it, far/near
	//     convergence drifts the off-eye frustum + eye_world (the "stereo jump on
	//     toggle"). f>1 (near) wants ipd>1, which is fine here (no clamp).
	{
		float H = 0.194f, N = 0.6f, aspect = 1.7783f, ipd = 0.063f;
		dxr_screen scr = {H * aspect, H};
		dxr_rig_display_info info = {H, aspect, N};
		dxr_vec3 eyes[2] = {{-ipd * 0.5f, 0, N}, {ipd * 0.5f, 0, N}};
		dxr_vec3 nomv = {0, 0, N};
		float invds[3] = {0.5f, 1.0f / 0.6f, 2.5f}; // far, compatible, near
		for (int ci = 0; ci < 3; ci++) {
			dxr_camera_rig cam = {{{0, 0, 0, 1}, {0, 0, 0}}, 1.0f, 1.0f, invds[ci], 0.3249f, 1.0f};
			dxr_camera3d_tunables ct = {cam.ipd_factor, cam.parallax_factor, cam.inv_convergence_distance,
			                            cam.half_tan_vfov, cam.m2v};
			dxr_camera3d_view cv[2];
			dxr_camera3d_compute_views(eyes, 2, &nomv, &scr, &ct, &cam.pose, 0.1f, 100.0f, cv);

			dxr_display_rig dr;
			dxr_view_rig_camera_to_display(&cam, &info, &dr);
			dxr_display3d_tunables dt = {dr.ipd_factor, dr.parallax_factor, dr.perspective_factor,
			                             dr.virtual_display_height};
			dxr_display3d_view dv[2];
			dxr_display3d_compute_views(eyes, 2, &nomv, &scr, &dt, &dr.pose, 0.05f, 100.0f, 0, dv);

			for (int e = 0; e < 2; e++) {
				float d = fabsf(dv[e].fov.angle_left - cv[e].fov.angle_left);
				d = fmaxf(d, fabsf(dv[e].fov.angle_right - cv[e].fov.angle_right));
				d = fmaxf(d, fabsf(dv[e].eye_world.x - cv[e].eye_world.x));
				d = fmaxf(d, fabsf(dv[e].eye_world.z - cv[e].eye_world.z));
				if (d > 1.0e-3f) {
					fails++;
					fprintf(stderr,
					        "[dxr_display3d_selftest] reconverged off-eye mismatch invd=%.2f eye=%d diff=%.2e\n",
					        invds[ci], e, d);
				}
			}
		}
	}

	// (f) RIG TRANSITION (convert-then-lerp) + comfort — see dxr_rig_transition.
	{
		float H = 0.194f, N = 0.6f, aspect = 1.7783f, ipd = 0.063f;
		dxr_screen scr = {H * aspect, H};
		dxr_rig_display_info info = {H, aspect, N};
		dxr_vec3 eyes[2] = {{-ipd * 0.5f, 0, N}, {ipd * 0.5f, 0, N}};
		dxr_vec3 nomv = {0, 0, N};

		// (f.1) Disturbance-free at t=0: a cross-type transition's first frame is
		//       the exact converted `from`, so it renders identically to `from`.
		dxr_rig from = {.type = DXR_RIG_DISPLAY, .u.display = {{{0, 0, 0, 1}, {0, 0, 0}}, 1.30f, 1.0f, 1.0f, 1.0f}};
		dxr_rig to = {.type = DXR_RIG_CAMERA, .u.camera = {{{0, 0, 0, 1}, {0.1f, 0.05f, 0.2f}}, 1.0f, 1.0f, 0.5f,
		                                                   0.40f, 1.0f}};
		dxr_rig out;
		dxr_rig_transition(&from, &to, &info, 0.0f, 0.0f, &out); // t=0 → converted `from`
		dxr_display3d_tunables dft = {from.u.display.ipd_factor, from.u.display.parallax_factor,
		                              from.u.display.perspective_factor, from.u.display.virtual_display_height};
		dxr_display3d_view dfv[2];
		dxr_display3d_compute_views(eyes, 2, &nomv, &scr, &dft, &from.u.display.pose, 0.05f, 100.0f, 0, dfv);
		dxr_camera3d_tunables oct = {out.u.camera.ipd_factor, out.u.camera.parallax_factor,
		                             out.u.camera.inv_convergence_distance, out.u.camera.half_tan_vfov,
		                             out.u.camera.m2v};
		dxr_camera3d_view ocv[2];
		dxr_camera3d_compute_views(eyes, 2, &nomv, &scr, &oct, &out.u.camera.pose, 0.1f, 100.0f, ocv);
		for (int e = 0; e < 2; e++) {
			float d = fabsf(dfv[e].fov.angle_left - ocv[e].fov.angle_left);
			d = fmaxf(d, fabsf(dfv[e].fov.angle_right - ocv[e].fov.angle_right));
			d = fmaxf(d, fabsf(dfv[e].eye_world.x - ocv[e].eye_world.x));
			d = fmaxf(d, fabsf(dfv[e].eye_world.z - ocv[e].eye_world.z));
			if (d > 1.0e-3f) {
				fails++;
				fprintf(stderr, "[dxr_display3d_selftest] (f) transition t=0 not disturbance-free eye=%d diff=%.2e\n",
				        e, d);
			}
		}

		// (f.2) Endpoint t=1 lands exactly on `to` (which is comfortable here).
		dxr_rig_transition(&from, &to, &info, 1.0f, 0.0f, &out);
		const dxr_camera_rig *tc = &to.u.camera, *oc = &out.u.camera;
		if (out.type != DXR_RIG_CAMERA || fabsf(oc->inv_convergence_distance - tc->inv_convergence_distance) > 1e-5f ||
		    fabsf(oc->half_tan_vfov - tc->half_tan_vfov) > 1e-5f || fabsf(oc->ipd_factor - tc->ipd_factor) > 1e-5f ||
		    fabsf(oc->pose.position.x - tc->pose.position.x) > 1e-5f) {
			fails++;
			fprintf(stderr, "[dxr_display3d_selftest] (f) transition t=1 != target\n");
		}

		// (f.3) virtual_display_height lerps log/multiplicatively: t=0.5 = geometric mean.
		dxr_rig da = {.type = DXR_RIG_DISPLAY, .u.display = {{{0, 0, 0, 1}, {0, 0, 0}}, 1.0f, 1.0f, 1.0f, 1.0f}};
		dxr_rig db = {.type = DXR_RIG_DISPLAY, .u.display = {{{0, 0, 0, 1}, {0, 0, 0}}, 4.0f, 1.0f, 1.0f, 1.0f}};
		dxr_rig dm;
		dxr_rig_transition(&da, &db, &info, 0.5f, 0.0f, &dm);
		float want = sqrtf(1.0f * 4.0f); // 2.0
		if (fabsf(dm.u.display.virtual_display_height - want) > 1e-4f) {
			fails++;
			fprintf(stderr, "[dxr_display3d_selftest] (f) vHeight not geometric mean: %.4f want %.4f\n",
			        dm.u.display.virtual_display_height, want);
		}

		// (f.4) Comfort. comfort_factor at the 1/(m2v*N) clamp == ipd.
		dxr_camera_rig cc = {{{0, 0, 0, 1}, {0, 0, 0}}, 0.8f, 1.0f, 1.0f / (1.0f * N), 0.3249f, 1.0f};
		if (fabsf(dxr_rig_comfort_factor(&cc, &info) - 0.8f) > 1e-4f) {
			fails++;
			fprintf(stderr, "[dxr_display3d_selftest] (f) comfort_factor at limit != ipd\n");
		}
		// A camera `to` converging too near is clamped; the whole transition stays <= 1.
		dxr_rig cfrom = {.type = DXR_RIG_CAMERA, .u.camera = {{{0, 0, 0, 1}, {0, 0, 0}}, 1.0f, 1.0f, 0.5f, 0.3249f, 1.0f}};
		dxr_rig nearto = {.type = DXR_RIG_CAMERA, .u.camera = {{{0, 0, 0, 1}, {0, 0, 0}}, 1.0f, 1.0f, 5.0f, 0.3249f, 1.0f}};
		for (int s = 0; s <= 10; s++) {
			dxr_rig ev;
			dxr_rig_transition(&cfrom, &nearto, &info, s * 0.1f, 0.0f, &ev);
			if (dxr_rig_comfort_factor(&ev.u.camera, &info) > 1.0f + 1e-4f) {
				fails++;
				fprintf(stderr, "[dxr_display3d_selftest] (f) transition exceeded comfort at t=%.1f\n", s * 0.1f);
			}
		}
		// A depth-aware far plane allows a strictly nearer comfortable convergence.
		float inf_max = dxr_rig_max_convergence(&cfrom.u.camera, &info, 0.0f);
		float dep_max = dxr_rig_max_convergence(&cfrom.u.camera, &info, 2.0f);
		if (!(dep_max > inf_max)) {
			fails++;
			fprintf(stderr, "[dxr_display3d_selftest] (f) depth budget did not allow nearer convergence\n");
		}
	}

	return fails;
}

// ============================================================================
// Camera-centric rig
// ============================================================================

dxr_camera3d_tunables
dxr_camera3d_default_tunables(void)
{
	dxr_camera3d_tunables t;
	t.ipd_factor = 1.0f;
	t.parallax_factor = 1.0f;
	t.inv_convergence_distance = 1.0f;
	t.half_tan_vfov = 0.32491969623f; // tan(18 deg) → 36° vFOV
	t.m2v = 1.0f;
	return t;
}

void
dxr_camera3d_compute_view(const dxr_vec3 *processed_eye,
                          float nominal_z,
                          const dxr_screen *screen,
                          const dxr_camera3d_tunables *tunables,
                          const dxr_pose *camera_pose,
                          float near_z,
                          float far_z,
                          dxr_camera3d_view *out)
{
	dxr_camera3d_tunables t = tunables ? *tunables : dxr_camera3d_default_tunables();

	dxr_quat cam_ori = {0, 0, 0, 1};
	dxr_vec3 cam_pos = {0, 0, 0};
	if (camera_pose) {
		cam_ori = camera_pose->orientation;
		cam_pos = camera_pose->position;
	}

	float aspect = screen->width_m / screen->height_m;
	float ro = t.half_tan_vfov * aspect;
	float uo = t.half_tan_vfov;
	float invd = t.inv_convergence_distance;
	float m2v = t.m2v > 0.0f ? t.m2v : 1.0f; // meters→world; <=0 (e.g. unset) means identity

	// eye_local = physical displacement from the nominal screen plane (meters),
	// scaled to world units by m2v. This single world-space eye drives BOTH the
	// modelview (eye_world) and the projection shear, so head motion scales into
	// world units. invd is in 1/world-units, so m2v cancels in the shear for a
	// converted frustum (no visual effect there) — its real effect is the
	// modelview translation. m2v == 1 reproduces the pre-m2v behavior exactly.
	dxr_vec3 eye_local;
	eye_local.x = m2v * processed_eye->x;
	eye_local.y = m2v * processed_eye->y;
	eye_local.z = m2v * (processed_eye->z - nominal_z);

	// Transform to world space
	dxr_vec3 eye_world = quat_rotate(cam_ori, eye_local);
	eye_world.x += cam_pos.x;
	eye_world.y += cam_pos.y;
	eye_world.z += cam_pos.z;
	out->eye_world = eye_world;

	// Build view matrix
	build_view_matrix(out->view_matrix, cam_ori, eye_world);
	out->orientation = cam_ori;

	// Camera-centric mapping onto the canonical off-axis frustum: the base
	// half-tangents are the camera's vFOV directly, and the world-space eye
	// displacement scaled by invd is the shear. invd == 0 (convergence at
	// infinity) yields symmetric, parallel views with no special-casing. See
	// dxr_offaxis_projection.
	dxr_vec3 shear = {eye_local.x * invd, eye_local.y * invd, eye_local.z * invd};
	dxr_offaxis_projection(ro, uo, shear, near_z, far_z, out->projection_matrix, &out->fov);
}

void
dxr_camera3d_compute_views(const dxr_vec3 *raw_eyes,
                           uint32_t count,
                           const dxr_vec3 *nominal_viewer,
                           const dxr_screen *screen,
                           const dxr_camera3d_tunables *tunables,
                           const dxr_pose *camera_pose,
                           float near_z,
                           float far_z,
                           dxr_camera3d_view *out_views)
{
	dxr_camera3d_tunables t = tunables ? *tunables : dxr_camera3d_default_tunables();

	float nom_z = 0.5f;
	if (nominal_viewer) {
		nom_z = nominal_viewer->z;
	}

	// Apply IPD and parallax factors (N-view path)
	dxr_vec3 stack_processed[8];
	dxr_vec3 *processed = (count <= 8) ? stack_processed : (dxr_vec3 *)malloc(count * sizeof(dxr_vec3));
	dxr_display3d_apply_eye_factors_n(raw_eyes, count, nominal_viewer, t.ipd_factor, t.parallax_factor, processed);

	// Compute each view via single-eye primitive
	for (uint32_t i = 0; i < count; i++) {
		dxr_camera3d_compute_view(&processed[i], nom_z, screen, &t, camera_pose, near_z, far_z, &out_views[i]);
	}

	if (count > 8)
		free(processed);
}

// ============================================================================
// Rig equivalence / conversion
// ============================================================================
//
// Notation: H = physical_height_m, N = nominal_distance_m,
//           tan(physFOV_v/2) = H/(2N), m2v_d = vHeight/H.
// The world-space convergence distance is D_world = persp * m2v_d * N, and the
// camera scene-scale is m2v_c = persp * m2v_d (forced by the modelview match —
// the display rig scales the eye XYZ by perspective, so the camera rig must
// carry the same factor for off-centre/ moving viewers to coincide).

void
dxr_view_rig_display_to_camera(const dxr_display_rig *in, const dxr_rig_display_info *info, dxr_camera_rig *out)
{
	float H = info->physical_height_m;
	float N = info->nominal_distance_m;
	float persp = in->perspective_factor;
	float m2v_d = in->virtual_display_height / H;
	float tan_half_phys = H / (2.0f * N);
	float es = persp * m2v_d; // display eye scale: eye_scaled = es * eye

	out->half_tan_vfov = tan_half_phys / persp; // tan(vFOV/2) = tan(physFOV/2)/persp

	// m2v is mathematically redundant with ipd/parallax (it only ever multiplies
	// the modelview eye, exactly like they do). Emit the CANONICAL m2v = 1 and put
	// the display eye-scale es into ipd/parallax instead. This is what keeps
	// display<->camera round-trips DRIFT-FREE: camera→display rescales ipd by
	// f = m2v*invd*N, and this reverse multiplies by es = 1/(invd*N) (= 1/f when
	// m2v==1), so a round trip recovers the original ipd exactly — the camera rig
	// never has its m2v silently rewritten. (Synthesizing m2v = es here instead
	// would render identically but drift m2v 1→es on every camera→display→camera,
	// corrupting m2v's meaning as a meters→world unit scale.)
	out->m2v = 1.0f;
	out->ipd_factor = in->ipd_factor * es;
	out->parallax_factor = in->parallax_factor * es;

	// Convergence distance (where the off-axis frustums cross) = es * N in world
	// units; with m2v == 1 the camera's 1/invd must equal it.
	float d_world = es * N;
	out->inv_convergence_distance = 1.0f / d_world;

	// Camera sits d_world in front (+Z) of the convergence plane (= display).
	out->pose.orientation = in->pose.orientation;
	dxr_vec3 fwd = quat_rotate(in->pose.orientation, (dxr_vec3){0.0f, 0.0f, d_world});
	out->pose.position.x = in->pose.position.x + fwd.x;
	out->pose.position.y = in->pose.position.y + fwd.y;
	out->pose.position.z = in->pose.position.z + fwd.z;
}

float
dxr_view_rig_camera_to_display(const dxr_camera_rig *in, const dxr_rig_display_info *info, dxr_display_rig *out)
{
	float H = info->physical_height_m;
	float N = info->nominal_distance_m;
	float tan_half_phys = H / (2.0f * N);
	float persp = tan_half_phys / in->half_tan_vfov; // tan(physFOV/2)/tan(vFOV/2)

	out->perspective_factor = persp;

	// IPD / parallax rescale — this is what makes the conversion EXACT for any
	// convergence, not just the display-compatible one. The display rig couples
	// convergence distance and eye-separation through one scale es = persp*vH/H
	// (eye_scaled = es*eye); the camera rig has them independent (invd sets
	// convergence, m2v scales the eye). Matching vH to the convergence distance
	// (below) forces es != m2v for an off-nominal convergence, which would rescale
	// the per-eye disparity — the "stereo jump after reconverging then toggling"
	// we saw. Both factors only act on the modelview eye, so absorb that disparity
	// rescale into ipd/parallax: with f = m2v*invd*N, the display eye-separation
	// es*(ipd*f) == m2v*ipd (the camera's), so every eye's frustum + eye_world
	// match exactly. f == 1 for a compatible rig (no change). The reverse
	// (display->camera) mirrors this: it emits m2v = 1 and multiplies ipd by
	// es = 1/(invd*N), so a display<->camera round trip recovers the original
	// ipd EXACTLY — no drift, the camera's m2v is never silently rewritten.
	// NOTE: f > 1 (convergence NEARER than nominal) wants ipd_factor > 1; callers
	// that clamp ipd to [0,1] will fall short of exact there — far convergence
	// (f <= 1, incl. the 0.5 dp default) is always exact.
	float f = in->m2v * in->inv_convergence_distance * N;
	out->ipd_factor = in->ipd_factor * f;
	out->parallax_factor = in->parallax_factor * f;

	// Virtual display height = the world-space height of the virtual screen at
	// the convergence plane: vH = 2 * tan(vFOV/2) * D_world = 2 * half_tan_vfov /
	// invd. This is the camera frustum's own geometry in world units (m2v scales
	// the eye, NOT the frustum base, so it does not appear here). The earlier
	// (m2v/persp)*H form equals this ONLY when the convergence sits at the
	// nominal distance (a display-compatible rig, invd = 1/(m2v*N)); for any
	// other convergence it drifted — e.g. the 0.5 dp / 36° camera default came
	// out 0.39 m instead of the intended 1.30 m, popping the qwerty P-toggle.
	float invd = in->inv_convergence_distance;
	float safe_invd = (invd > 1.0e-6f) ? invd : 1.0e-6f; // invd→0 = convergence at infinity
	out->virtual_display_height = 2.0f * in->half_tan_vfov / safe_invd;

	// Pose: place the display plane so the display rig's eye_world coincides with
	// the camera rig's at the reference eye (0,0,N). The display rig scales the
	// eye by es = persp * vH / H, so its reference eye_scaled = (0,0,es*N) while
	// the camera's eye_local = (0,0,0); the offset that cancels the difference is
	// R*(eye_local - eye_scaled) = R*(0,0,-es*N). (This reduces to -D_world only
	// for a display-compatible rig, where es*N == 1/invd; the old code hard-coded
	// -1/invd and so misplaced the plane — and hence eye_world — by es*N - 1/invd
	// for every off-nominal convergence.)
	float es = persp * out->virtual_display_height / H;
	out->pose.orientation = in->pose.orientation;
	dxr_vec3 back = quat_rotate(in->pose.orientation, (dxr_vec3){0.0f, 0.0f, -es * N});
	out->pose.position.x = in->pose.position.x + back.x;
	out->pose.position.y = in->pose.position.y + back.y;
	out->pose.position.z = in->pose.position.z + back.z;

	// Convergence delta from the display-compatible value. With the ipd/parallax
	// rescale above the conversion is now exact for any convergence (modulo a
	// caller ipd clamp for f>1), so this is informational — how far the camera's
	// convergence sits from the nominal distance — not a "lossy" flag.
	float compatible_invd = 1.0f / (in->m2v * N);
	return in->inv_convergence_distance - compatible_invd;
}

// ============================================================================
// Rig transition (convert-then-lerp) + stereo comfort
// ============================================================================

static float
rig_lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

// Multiplicative / log-space lerp: the geometric path a -> b (perceptually
// uniform for zoom-like quantities; t=0.5 is the geometric mean). Falls back to
// linear if either endpoint is non-positive (log undefined).
static float
rig_loglerpf(float a, float b, float t)
{
	if (a > 0.0f && b > 0.0f) {
		return a * powf(b / a, t);
	}
	return rig_lerpf(a, b, t);
}

// Lerp a half-tangent FOV in ANGLE space: interpolate the angle, then re-tan.
static float
rig_htan_lerp(float a, float b, float t)
{
	return tanf(rig_lerpf(atanf(a), atanf(b), t));
}

float
dxr_rig_comfort_factor(const dxr_camera_rig *cam, const dxr_rig_display_info *info)
{
	float N = info->nominal_distance_m;
	float m2v = (cam->m2v > 0.0f) ? cam->m2v : 1.0f;
	float f = m2v * cam->inv_convergence_distance * N; // = ipd_eff / ipd
	return cam->ipd_factor * f;
}

float
dxr_rig_max_convergence(const dxr_camera_rig *cam, const dxr_rig_display_info *info, float far_z)
{
	float N = info->nominal_distance_m;
	float m2v = (cam->m2v > 0.0f) ? cam->m2v : 1.0f;
	// Comfort is the PRODUCT comfort_factor = ipd*m2v*invd*N <= 1, enforced via the
	// convergence knob: invd <= 1/(ipd*m2v*N). For ipd <= 1 that ceiling sits at or
	// above the nominal-distance floor 1/(m2v*N), so take max(1, ipd): the clamp is
	// then 1/(m2v*N) (zero-parallax no nearer than nominal — the qwerty conv_max),
	// which also keeps the rig in the converter's always-exact f<=1 regime. For
	// ipd > 1 (e.g. a display->camera conversion with large eye-scale) the binding
	// term is 1/(ipd*m2v*N); that is < 1/(m2v*N) so it stays f<=1 too. Either way
	// comfort_factor <= 1. We clamp invd, NEVER ipd — a rig with ipd>1 but tiny
	// invd is already comfortable and is left untouched.
	float ipd = (cam->ipd_factor > 1.0f) ? cam->ipd_factor : 1.0f;
	float invd_inf = 1.0f / (ipd * m2v * N);
	if (far_z <= 0.0f) {
		return invd_inf;
	}
	// Depth-aware: with no content past far_z (world distance from the eye), the
	// binding disparity is the parallax at far_z, not at infinity, so convergence
	// may move nearer by 1/far_z: invd_max = 1/(ipd*m2v*N) + 1/far_z. Collapses to
	// the infinity case as far_z -> inf. (Here convergence may go nearer than
	// nominal, i.e. f>1 — the depth-aware caller accepts that trade.)
	return invd_inf + 1.0f / far_z;
}

float
dxr_rig_max_ipd_factor(const dxr_camera_rig *cam, const dxr_rig_display_info *info)
{
	// Dual of dxr_rig_max_convergence: the largest ipd_factor whose DISPLAY-centric
	// equivalent (comfort_factor = ipd*f, f = m2v*invd*N) is exactly 1, at the rig's
	// CURRENT convergence — i.e. M = 1/f. cam->ipd_factor itself is NOT read. For far
	// convergence (f<1) M>1, so the camera rig legitimately exceeds the display rig's
	// [0,1]; for near convergence (f>1) M<1. f<=0 (convergence at/behind infinity, or
	// degenerate m2v/N) imposes no comfort ceiling, so return +infinity — callers pass
	// it straight into a clamp as an unbounded upper limit. NOTE the library enforces
	// comfort at the CONVERGENCE knob (dxr_rig_clamp_for_comfort), never by clamping
	// ipd; this helper is for callers that deliberately bound an ipd knob at fixed
	// convergence (e.g. a UI/keyboard IPD control).
	float N = info->nominal_distance_m;
	float m2v = (cam->m2v > 0.0f) ? cam->m2v : 1.0f;
	float f = m2v * cam->inv_convergence_distance * N;
	if (f <= 0.0f) {
		return HUGE_VALF; // +infinity: no comfort ceiling from this term
	}
	return 1.0f / f;
}

void
dxr_rig_clamp_for_comfort(dxr_camera_rig *cam, const dxr_rig_display_info *info, float far_z)
{
	float invd_max = dxr_rig_max_convergence(cam, info, far_z);
	if (cam->inv_convergence_distance > invd_max) {
		cam->inv_convergence_distance = invd_max;
	}
}

void
dxr_rig_toggle(const dxr_rig *in, const dxr_rig_display_info *info, dxr_rig *out)
{
	if (in->type == DXR_RIG_CAMERA) {
		out->type = DXR_RIG_DISPLAY;
		dxr_view_rig_camera_to_display(&in->u.camera, info, &out->u.display);
	} else {
		out->type = DXR_RIG_CAMERA;
		dxr_view_rig_display_to_camera(&in->u.display, info, &out->u.camera);
	}
}

void
dxr_rig_transition(const dxr_rig *from,
                   const dxr_rig *to,
                   const dxr_rig_display_info *info,
                   float t,
                   float comfort_far_z,
                   dxr_rig *out)
{
	out->type = to->type;

	// Land exactly on the target at t >= 1.
	if (t >= 1.0f) {
		*out = *to;
		if (out->type == DXR_RIG_CAMERA) {
			dxr_rig_clamp_for_comfort(&out->u.camera, info, comfort_far_z);
		}
		return;
	}

	float tt = (t < 0.0f) ? 0.0f : t; // t <= 0 emits the converted `from`.

	// Bring `from` into the target type's space — exact converters, so the
	// converted rig renders identically to `from` (disturbance-free at tt=0).
	if (out->type == DXR_RIG_DISPLAY) {
		dxr_display_rig a;
		if (from->type == DXR_RIG_DISPLAY) {
			a = from->u.display;
		} else {
			(void)dxr_view_rig_camera_to_display(&from->u.camera, info, &a);
		}
		const dxr_display_rig *b = &to->u.display;
		dxr_display_rig *o = &out->u.display;
		dxr_display3d_pose_slerp(&a.pose, &b->pose, tt, &o->pose);
		o->virtual_display_height = rig_loglerpf(a.virtual_display_height, b->virtual_display_height, tt);
		o->perspective_factor = rig_loglerpf(a.perspective_factor, b->perspective_factor, tt);
		o->ipd_factor = rig_lerpf(a.ipd_factor, b->ipd_factor, tt);
		o->parallax_factor = rig_lerpf(a.parallax_factor, b->parallax_factor, tt);
	} else {
		dxr_camera_rig a;
		if (from->type == DXR_RIG_CAMERA) {
			a = from->u.camera;
		} else {
			dxr_view_rig_display_to_camera(&from->u.display, info, &a);
		}
		const dxr_camera_rig *b = &to->u.camera;
		dxr_camera_rig *o = &out->u.camera;
		dxr_display3d_pose_slerp(&a.pose, &b->pose, tt, &o->pose);
		o->inv_convergence_distance = rig_lerpf(a.inv_convergence_distance, b->inv_convergence_distance, tt);
		o->half_tan_vfov = rig_htan_lerp(a.half_tan_vfov, b->half_tan_vfov, tt);
		o->m2v = rig_lerpf(a.m2v, b->m2v, tt);
		o->ipd_factor = rig_lerpf(a.ipd_factor, b->ipd_factor, tt);
		o->parallax_factor = rig_lerpf(a.parallax_factor, b->parallax_factor, tt);
		// Keep the whole path comfortable (clamp the evaluated convergence).
		dxr_rig_clamp_for_comfort(o, info, comfort_far_z);
	}
}
