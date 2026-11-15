// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Validated glTF 2.0 accessor reader (header-only, any platform).
 *
 * The demos' glTF loaders (displayxr-demo-avatar #125, displayxr-demo-modelviewer
 * #184) read accessors through hand-rolled helpers that reinterpret every
 * attribute as float32 and trust every index and length in the file. This
 * header is the one checked reader they can share. Every index and byte range
 * a read depends on is validated BEFORE any byte is touched, and a read that
 * cannot be done exactly as the file describes fails with a reason instead of
 * returning partial or reinterpreted data:
 *
 *   - accessor index, bufferView index and bufferView.buffer index in range;
 *   - bufferView byteOffset + byteLength within the buffer;
 *   - accessor extent  byteOffset + (count-1)*stride + elementSize  within the
 *     bufferView (overflow-safe), and a byteStride no smaller than one element;
 *   - accessor.type gives the component count the caller expects;
 *   - componentType is one glTF allows (BYTE, UNSIGNED_BYTE, SHORT,
 *     UNSIGNED_SHORT, UNSIGNED_INT, FLOAT) and `normalized` is only set on the
 *     8/16-bit integer types (glTF 2.0 §3.6.2);
 *   - sparse: count in [1, accessor.count], both bufferViews valid, both
 *     ranges within their views, index type UNSIGNED_BYTE/SHORT/INT, and every
 *     sparse index < accessor.count;
 *   - no bufferView: the base data is zeros (glTF §3.6.2.3), with sparse
 *     overrides applied on top; the element count is capped (kMaxZeroFill...)
 *     because no buffer bounds it.
 *
 * Decoding (glTF §3.11, KHR_mesh_quantization): FLOAT passes through;
 * integers become floats, scaled when `normalized` (UBYTE c/255, USHORT
 * c/65535, BYTE max(c/127,-1), SHORT max(c/32767,-1)). Components are read
 * with memcpy (no alignment assumption) as little-endian, which is what glTF
 * stores and what every DisplayXR target is.
 *
 * Helpers for the rest of a loader: ReadIndices (range-checked against the
 * vertex count), ReadJoints (range-checked against the skin's joint count) and
 * CheckAnimationSampler (output count vs input count for the interpolation
 * mode).
 *
 * ── Coupling ────────────────────────────────────────────────────────────────
 * displayxr-common does not depend on tinygltf. Every reader is a template
 * over the model type and touches only these members, named exactly as in
 * tinygltf (checked against v2.9.3, the version both demos pin):
 *
 *   Model:      accessors, bufferViews, buffers            (indexable, .size())
 *   Buffer:     data                       (contiguous bytes: .data(), .size())
 *   BufferView: buffer, byteOffset, byteLength, byteStride
 *   Accessor:   bufferView, byteOffset, normalized, componentType, count, type,
 *               sparse.isSparse, sparse.count,
 *               sparse.indices.{bufferView, byteOffset, componentType},
 *               sparse.values.{bufferView, byteOffset}
 *   Sampler:    input, output, interpolation     (CheckAnimationSampler only)
 *
 * so a demo passes its tinygltf::Model straight in (no adapter), and the unit
 * test drives it with small structs of the same shape. The componentType and
 * type codes below are tinygltf's (TINYGLTF_COMPONENT_TYPE_* = the glTF GL
 * enums, TINYGLTF_TYPE_*); if tiny_gltf.h is included first they are
 * static_assert-ed equal.
 *
 * Reachable through displayxr::rules (and displayxr::common). Needs only the
 * C++17 standard library.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace dxr {
namespace gltf {

// --- glTF codes (tinygltf's encoding) ---------------------------------------

constexpr int kComponentByte = 5120;
constexpr int kComponentUnsignedByte = 5121;
constexpr int kComponentShort = 5122;
constexpr int kComponentUnsignedShort = 5123;
constexpr int kComponentUnsignedInt = 5125;
constexpr int kComponentFloat = 5126;

constexpr int kTypeVec2 = 2;
constexpr int kTypeVec3 = 3;
constexpr int kTypeVec4 = 4;
constexpr int kTypeMat2 = 32 + 2;
constexpr int kTypeMat3 = 32 + 3;
constexpr int kTypeMat4 = 32 + 4;
constexpr int kTypeScalar = 64 + 1;

#ifdef TINYGLTF_COMPONENT_TYPE_FLOAT
static_assert(kComponentByte == TINYGLTF_COMPONENT_TYPE_BYTE, "tinygltf code drift");
static_assert(kComponentUnsignedByte == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, "tinygltf code drift");
static_assert(kComponentShort == TINYGLTF_COMPONENT_TYPE_SHORT, "tinygltf code drift");
static_assert(kComponentUnsignedShort == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, "tinygltf code drift");
static_assert(kComponentUnsignedInt == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, "tinygltf code drift");
static_assert(kComponentFloat == TINYGLTF_COMPONENT_TYPE_FLOAT, "tinygltf code drift");
#endif
#ifdef TINYGLTF_TYPE_SCALAR
static_assert(kTypeVec2 == TINYGLTF_TYPE_VEC2 && kTypeVec3 == TINYGLTF_TYPE_VEC3 && kTypeVec4 == TINYGLTF_TYPE_VEC4,
              "tinygltf code drift");
static_assert(kTypeMat2 == TINYGLTF_TYPE_MAT2 && kTypeMat3 == TINYGLTF_TYPE_MAT3 && kTypeMat4 == TINYGLTF_TYPE_MAT4,
              "tinygltf code drift");
static_assert(kTypeScalar == TINYGLTF_TYPE_SCALAR, "tinygltf code drift");
#endif

//! Upper bound on the element count of an accessor with NO bufferView (all
//! zeros + sparse overrides): nothing in the file bounds it, so the reader
//! refuses to allocate more than this many elements for one.
constexpr size_t kMaxZeroFillElements = size_t(1) << 24;

//! Components per element for an accessor.type code; 0 if unknown.
inline int
ComponentCount(int type)
{
	switch (type) {
	case kTypeScalar: return 1;
	case kTypeVec2: return 2;
	case kTypeVec3: return 3;
	case kTypeVec4: return 4;
	case kTypeMat2: return 4;
	case kTypeMat3: return 9;
	case kTypeMat4: return 16;
	default: return 0;
	}
}

//! Bytes per component for a componentType glTF allows in an accessor; 0 if
//! not one of BYTE / UNSIGNED_BYTE / SHORT / UNSIGNED_SHORT / UNSIGNED_INT / FLOAT.
inline size_t
ComponentSize(int componentType)
{
	switch (componentType) {
	case kComponentByte:
	case kComponentUnsignedByte: return 1;
	case kComponentShort:
	case kComponentUnsignedShort: return 2;
	case kComponentUnsignedInt:
	case kComponentFloat: return 4;
	default: return 0;
	}
}

//! A validated accessor: every pointer and range below was checked against
//! the model's buffers by ResolveAccessor, so VisitComponents cannot read out
//! of bounds.
struct AccessorLayout
{
	size_t count = 0;         //!< elements
	int components = 0;       //!< components per element
	int componentType = 0;    //!< kComponent*
	size_t componentSize = 0; //!< bytes per component
	bool normalized = false;
	const unsigned char *data = nullptr; //!< first element; nullptr = no bufferView (zeros)
	size_t stride = 0;                   //!< bytes between elements (data != nullptr)

	size_t sparseCount = 0; //!< 0 = not sparse
	const unsigned char *sparseIndices = nullptr;
	int sparseIndexType = 0; //!< kComponentUnsigned{Byte,Short,Int}
	size_t sparseIndexSize = 0;
	const unsigned char *sparseValues = nullptr; //!< tightly packed elements
};

namespace detail {

inline bool
Fail(std::string *err, const std::string &msg)
{
	if (err != nullptr) {
		*err = msg;
	}
	return false;
}

inline std::string
Num(size_t v)
{
	return std::to_string(static_cast<unsigned long long>(v));
}

//! offset + (count-1)*stride + elemSize <= length, without overflow.
//! count == 0 needs only offset <= length.
inline bool
ExtentFits(size_t offset, size_t count, size_t stride, size_t elemSize, size_t length)
{
	if (count == 0) {
		return offset <= length;
	}
	if (elemSize > length) {
		return false;
	}
	const size_t room = length - elemSize; // bytes available for offset + (count-1)*stride
	if (count > 1 && stride != 0 && (count - 1) > room / stride) {
		return false;
	}
	const size_t span = (count - 1) * stride; // <= room
	return offset <= room - span;
}

//! Validate bufferViews[index] and its buffer; yield the view's bytes.
template <class Model>
bool
ResolveView(const Model &m, long long index, const char *what, const unsigned char *&bytes, size_t &length,
            size_t &byteStride, std::string *err)
{
	if (index < 0 || static_cast<unsigned long long>(index) >= m.bufferViews.size()) {
		return Fail(err, std::string(what) + ": bufferView " + std::to_string(index) + " out of range (" +
		                     Num(m.bufferViews.size()) + " bufferViews)");
	}
	const auto &bv = m.bufferViews[static_cast<size_t>(index)];
	const long long buffer = static_cast<long long>(bv.buffer);
	if (buffer < 0 || static_cast<unsigned long long>(buffer) >= m.buffers.size()) {
		return Fail(err, std::string(what) + ": bufferView " + std::to_string(index) + " references buffer " +
		                     std::to_string(buffer) + " out of range (" + Num(m.buffers.size()) + " buffers)");
	}
	const auto &data = m.buffers[static_cast<size_t>(buffer)].data;
	const size_t off = static_cast<size_t>(bv.byteOffset);
	const size_t len = static_cast<size_t>(bv.byteLength);
	if (off > data.size() || len > data.size() - off) {
		return Fail(err, std::string(what) + ": bufferView " + std::to_string(index) + " [" + Num(off) + ", +" +
		                     Num(len) + ") exceeds buffer " + std::to_string(buffer) + " size " +
		                     Num(data.size()));
	}
	bytes = data.data() + off;
	length = len;
	byteStride = static_cast<size_t>(bv.byteStride);
	return true;
}

inline uint32_t
DecodeUint(const unsigned char *p, int componentType)
{
	switch (componentType) {
	case kComponentUnsignedByte: return p[0];
	case kComponentUnsignedShort: {
		uint16_t v;
		std::memcpy(&v, p, sizeof v);
		return v;
	}
	case kComponentUnsignedInt: {
		uint32_t v;
		std::memcpy(&v, p, sizeof v);
		return v;
	}
	default: return 0;
	}
}

//! One component as float with glTF normalized scaling; p == nullptr -> 0.
inline float
DecodeFloat(const unsigned char *p, int componentType, bool normalized)
{
	if (p == nullptr) {
		return 0.0f;
	}
	switch (componentType) {
	case kComponentFloat: {
		float v;
		std::memcpy(&v, p, sizeof v);
		return v;
	}
	case kComponentByte: {
		int8_t v;
		std::memcpy(&v, p, sizeof v);
		if (!normalized) {
			return static_cast<float>(v);
		}
		const float f = static_cast<float>(v) / 127.0f;
		return f < -1.0f ? -1.0f : f;
	}
	case kComponentUnsignedByte: return normalized ? p[0] / 255.0f : static_cast<float>(p[0]);
	case kComponentShort: {
		int16_t v;
		std::memcpy(&v, p, sizeof v);
		if (!normalized) {
			return static_cast<float>(v);
		}
		const float f = static_cast<float>(v) / 32767.0f;
		return f < -1.0f ? -1.0f : f;
	}
	case kComponentUnsignedShort: {
		uint16_t v;
		std::memcpy(&v, p, sizeof v);
		return normalized ? v / 65535.0f : static_cast<float>(v);
	}
	case kComponentUnsignedInt: {
		uint32_t v;
		std::memcpy(&v, p, sizeof v);
		return static_cast<float>(v); // never normalized (rejected by ResolveAccessor)
	}
	default: return 0.0f;
	}
}

} // namespace detail

/*!
 * Validate accessors[accessorIndex] against the model and describe how to read
 * it. expectedComponents > 0 requires accessor.type to have exactly that many
 * components (1 SCALAR, 2 VEC2, 3 VEC3, 4 VEC4/MAT2, 9 MAT3, 16 MAT4); 0
 * accepts any type. Returns false with *err set on the first violation; on
 * success every byte VisitComponents will read is inside a buffer.
 */
template <class Model>
bool
ResolveAccessor(const Model &m, int accessorIndex, int expectedComponents, AccessorLayout &out,
                std::string *err = nullptr)
{
	using detail::Fail;
	using detail::Num;
	out = AccessorLayout{};

	if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= m.accessors.size()) {
		return Fail(err, "accessor " + std::to_string(accessorIndex) + " out of range (" +
		                     Num(m.accessors.size()) + " accessors)");
	}
	const auto &acc = m.accessors[static_cast<size_t>(accessorIndex)];
	const std::string what = "accessor " + std::to_string(accessorIndex);

	const int comps = ComponentCount(static_cast<int>(acc.type));
	if (comps == 0) {
		return Fail(err, what + ": unknown type " + std::to_string(static_cast<int>(acc.type)));
	}
	if (expectedComponents > 0 && comps != expectedComponents) {
		return Fail(err, what + ": has " + std::to_string(comps) + " components per element, expected " +
		                     std::to_string(expectedComponents));
	}
	const int ctype = static_cast<int>(acc.componentType);
	const size_t csize = ComponentSize(ctype);
	if (csize == 0) {
		return Fail(err, what + ": unsupported componentType " + std::to_string(ctype));
	}
	const bool normalized = static_cast<bool>(acc.normalized);
	if (normalized && (ctype == kComponentFloat || ctype == kComponentUnsignedInt)) {
		return Fail(err, what + ": normalized is not allowed for componentType " + std::to_string(ctype));
	}
	// glTF pads each matrix column to 4 bytes for 1- and 2-byte components
	// (MAT2/MAT3 of BYTE, MAT3 of SHORT). Not supported: refuse rather than
	// read the padding as data.
	const int type = static_cast<int>(acc.type);
	if ((type == kTypeMat2 && csize == 1) || (type == kTypeMat3 && csize < 4)) {
		return Fail(err, what + ": column-padded matrix layout (MAT2/MAT3 of 8/16-bit) is not supported");
	}

	const size_t count = static_cast<size_t>(acc.count);
	const size_t elemSize = static_cast<size_t>(comps) * csize;
	out.count = count;
	out.components = comps;
	out.componentType = ctype;
	out.componentSize = csize;
	out.normalized = normalized;

	// Base data.
	const long long bvIndex = static_cast<long long>(acc.bufferView);
	if (bvIndex < 0) {
		if (count > kMaxZeroFillElements) {
			return Fail(err, what + ": no bufferView and count " + Num(count) + " exceeds the zero-fill limit " +
			                     Num(kMaxZeroFillElements));
		}
	} else {
		const unsigned char *bytes = nullptr;
		size_t length = 0, byteStride = 0;
		if (!detail::ResolveView(m, bvIndex, what.c_str(), bytes, length, byteStride, err)) {
			return false;
		}
		const size_t stride = byteStride != 0 ? byteStride : elemSize;
		if (stride < elemSize) {
			return Fail(err, what + ": byteStride " + Num(stride) + " is smaller than the element size " +
			                     Num(elemSize));
		}
		const size_t off = static_cast<size_t>(acc.byteOffset);
		if (!detail::ExtentFits(off, count, stride, elemSize, length)) {
			return Fail(err, what + ": byteOffset " + Num(off) + " + " + Num(count) + " elements (stride " +
			                     Num(stride) + ", size " + Num(elemSize) + ") exceeds bufferView length " +
			                     Num(length));
		}
		out.data = bytes + off;
		out.stride = stride;
	}

	// Sparse overrides.
	if (static_cast<bool>(acc.sparse.isSparse)) {
		const long long scount = static_cast<long long>(acc.sparse.count);
		if (scount < 1 || static_cast<unsigned long long>(scount) > count) {
			return Fail(err, what + ": sparse count " + std::to_string(scount) + " not in [1, " + Num(count) + "]");
		}
		const size_t sc = static_cast<size_t>(scount);

		const int itype = static_cast<int>(acc.sparse.indices.componentType);
		if (itype != kComponentUnsignedByte && itype != kComponentUnsignedShort && itype != kComponentUnsignedInt) {
			return Fail(err, what + ": sparse indices componentType " + std::to_string(itype) +
			                     " is not UNSIGNED_BYTE/SHORT/INT");
		}
		const size_t isize = ComponentSize(itype);

		const unsigned char *ibytes = nullptr;
		size_t ilen = 0, istrideUnused = 0;
		const std::string iwhat = what + " sparse indices";
		if (!detail::ResolveView(m, static_cast<long long>(acc.sparse.indices.bufferView), iwhat.c_str(), ibytes,
		                         ilen, istrideUnused, err)) {
			return false;
		}
		const size_t ioff = static_cast<size_t>(acc.sparse.indices.byteOffset);
		if (!detail::ExtentFits(ioff, sc, isize, isize, ilen)) {
			return Fail(err, iwhat + ": byteOffset " + Num(ioff) + " + " + Num(sc) + " x " + Num(isize) +
			                     " bytes exceeds bufferView length " + Num(ilen));
		}

		const unsigned char *vbytes = nullptr;
		size_t vlen = 0, vstrideUnused = 0;
		const std::string vwhat = what + " sparse values";
		if (!detail::ResolveView(m, static_cast<long long>(acc.sparse.values.bufferView), vwhat.c_str(), vbytes,
		                         vlen, vstrideUnused, err)) {
			return false;
		}
		const size_t voff = static_cast<size_t>(acc.sparse.values.byteOffset);
		if (!detail::ExtentFits(voff, sc, elemSize, elemSize, vlen)) {
			return Fail(err, vwhat + ": byteOffset " + Num(voff) + " + " + Num(sc) + " x " + Num(elemSize) +
			                     " bytes exceeds bufferView length " + Num(vlen));
		}

		const unsigned char *idx = ibytes + ioff;
		for (size_t s = 0; s < sc; s++) {
			const uint32_t target = detail::DecodeUint(idx + s * isize, itype);
			if (target >= count) {
				return Fail(err, what + ": sparse index " + Num(target) + " (entry " + Num(s) +
				                     ") >= accessor count " + Num(count));
			}
		}
		out.sparseCount = sc;
		out.sparseIndices = idx;
		out.sparseIndexType = itype;
		out.sparseIndexSize = isize;
		out.sparseValues = vbytes + voff;
	}
	return true;
}

/*!
 * Call f(element, component, const unsigned char *p) for every component of
 * a resolved accessor: first the base data (p == nullptr when the accessor
 * has no bufferView, meaning zero), then each sparse override in file order.
 */
template <class F>
void
VisitComponents(const AccessorLayout &L, F &&f)
{
	for (size_t i = 0; i < L.count; i++) {
		const unsigned char *e = L.data != nullptr ? L.data + i * L.stride : nullptr;
		for (int c = 0; c < L.components; c++) {
			f(i, c, e != nullptr ? e + static_cast<size_t>(c) * L.componentSize : nullptr);
		}
	}
	const size_t elemSize = static_cast<size_t>(L.components) * L.componentSize;
	for (size_t s = 0; s < L.sparseCount; s++) {
		const size_t i = detail::DecodeUint(L.sparseIndices + s * L.sparseIndexSize, L.sparseIndexType);
		const unsigned char *e = L.sparseValues + s * elemSize;
		for (int c = 0; c < L.components; c++) {
			f(i, c, e + static_cast<size_t>(c) * L.componentSize);
		}
	}
}

/*!
 * Read an accessor as floats, count * components values, element-major.
 * Decodes every allowed componentType (normalized scaling applied) and sparse
 * overrides. expectedComponents as in ResolveAccessor (0 = any); the actual
 * count is written to *componentsOut when non-null. On failure `out` is empty.
 */
template <class Model>
bool
ReadFloats(const Model &m,
           int accessorIndex,
           int expectedComponents,
           std::vector<float> &out,
           std::string *err = nullptr,
           int *componentsOut = nullptr)
{
	out.clear();
	AccessorLayout L;
	if (!ResolveAccessor(m, accessorIndex, expectedComponents, L, err)) {
		return false;
	}
	out.assign(L.count * static_cast<size_t>(L.components), 0.0f);
	const size_t comps = static_cast<size_t>(L.components);
	VisitComponents(L, [&](size_t i, int c, const unsigned char *p) {
		out[i * comps + static_cast<size_t>(c)] = detail::DecodeFloat(p, L.componentType, L.normalized);
	});
	if (componentsOut != nullptr) {
		*componentsOut = L.components;
	}
	return true;
}

/*!
 * Read a primitive's index accessor (SCALAR of UNSIGNED_BYTE / SHORT / INT,
 * not normalized) and require every index < vertexCount. On failure `out` is
 * empty and *err names the first offending index.
 */
template <class Model>
bool
ReadIndices(const Model &m, int accessorIndex, size_t vertexCount, std::vector<uint32_t> &out,
            std::string *err = nullptr)
{
	out.clear();
	AccessorLayout L;
	if (!ResolveAccessor(m, accessorIndex, 1, L, err)) {
		return false;
	}
	const std::string what = "accessor " + std::to_string(accessorIndex);
	if ((L.componentType != kComponentUnsignedByte && L.componentType != kComponentUnsignedShort &&
	     L.componentType != kComponentUnsignedInt) ||
	    L.normalized) {
		return detail::Fail(err, what + ": indices must be non-normalized UNSIGNED_BYTE/SHORT/INT");
	}
	out.assign(L.count, 0u);
	VisitComponents(L, [&](size_t i, int, const unsigned char *p) {
		out[i] = p != nullptr ? detail::DecodeUint(p, L.componentType) : 0u;
	});
	for (size_t i = 0; i < out.size(); i++) {
		if (out[i] >= vertexCount) {
			const uint32_t bad = out[i];
			out.clear();
			return detail::Fail(err, what + ": index " + detail::Num(bad) + " at " + detail::Num(i) +
			                             " >= vertex count " + detail::Num(vertexCount));
		}
	}
	return true;
}

/*!
 * Read a JOINTS_n accessor (VEC4 of UNSIGNED_BYTE / UNSIGNED_SHORT, not
 * normalized) into 4 joint indices per vertex and require each < jointCount
 * (the skin's joints.size()). On failure `out` is empty.
 */
template <class Model>
bool
ReadJoints(const Model &m, int accessorIndex, size_t jointCount, std::vector<uint16_t> &out,
           std::string *err = nullptr)
{
	out.clear();
	AccessorLayout L;
	if (!ResolveAccessor(m, accessorIndex, 4, L, err)) {
		return false;
	}
	const std::string what = "accessor " + std::to_string(accessorIndex);
	if ((L.componentType != kComponentUnsignedByte && L.componentType != kComponentUnsignedShort) || L.normalized) {
		return detail::Fail(err, what + ": joints must be non-normalized UNSIGNED_BYTE/SHORT");
	}
	out.assign(L.count * 4, 0u);
	VisitComponents(L, [&](size_t i, int c, const unsigned char *p) {
		out[i * 4 + static_cast<size_t>(c)] =
		    p != nullptr ? static_cast<uint16_t>(detail::DecodeUint(p, L.componentType)) : uint16_t(0);
	});
	for (size_t k = 0; k < out.size(); k++) {
		if (out[k] >= jointCount) {
			const uint16_t bad = out[k];
			out.clear();
			return detail::Fail(err, what + ": joint " + detail::Num(bad) + " at vertex " + detail::Num(k / 4) +
			                             " >= joint count " + detail::Num(jointCount));
		}
	}
	return true;
}

/*!
 * Animation sampler count rule (glTF §3.11.3). With valuesPerKey output
 * elements per keyframe (1 for translation/rotation/scale, the morph-target
 * count for weights):
 *   LINEAR / STEP (or empty = LINEAR):  outputCount == inputCount * valuesPerKey
 *   CUBICSPLINE:                        outputCount == 3 * inputCount * valuesPerKey,
 *                                       inputCount >= 2
 * inputCount must be >= 1 and valuesPerKey >= 1; any other interpolation fails.
 */
inline bool
CheckAnimationSamplerCounts(size_t inputCount,
                            size_t outputCount,
                            const std::string &interpolation,
                            size_t valuesPerKey = 1,
                            std::string *err = nullptr)
{
	using detail::Fail;
	using detail::Num;
	if (inputCount < 1) {
		return Fail(err, "sampler: input has no keyframes");
	}
	if (valuesPerKey < 1) {
		return Fail(err, "sampler: valuesPerKey must be >= 1");
	}
	size_t perKey = valuesPerKey;
	if (interpolation == "CUBICSPLINE") {
		if (inputCount < 2) {
			return Fail(err, "sampler: CUBICSPLINE needs at least 2 keyframes");
		}
		perKey = valuesPerKey * 3; // in-tangent, value, out-tangent
	} else if (!(interpolation.empty() || interpolation == "LINEAR" || interpolation == "STEP")) {
		return Fail(err, "sampler: unknown interpolation \"" + interpolation + "\"");
	}
	if (perKey > std::numeric_limits<size_t>::max() / inputCount || outputCount != inputCount * perKey) {
		return Fail(err, "sampler: output count " + Num(outputCount) + " != " + Num(inputCount) + " keyframes x " +
		                     Num(perKey) + " (" + (interpolation.empty() ? std::string("LINEAR") : interpolation) +
		                     ")");
	}
	return true;
}

/*!
 * Validate an animation sampler against the model: input and output accessor
 * indices in range, input is SCALAR FLOAT (glTF requires keyframe times as
 * float), then CheckAnimationSamplerCounts on their counts.
 */
template <class Model, class Sampler>
bool
CheckAnimationSampler(const Model &m, const Sampler &s, size_t valuesPerKey = 1, std::string *err = nullptr)
{
	using detail::Fail;
	const long long in = static_cast<long long>(s.input);
	const long long outIdx = static_cast<long long>(s.output);
	if (in < 0 || static_cast<unsigned long long>(in) >= m.accessors.size()) {
		return Fail(err, "sampler: input accessor " + std::to_string(in) + " out of range");
	}
	if (outIdx < 0 || static_cast<unsigned long long>(outIdx) >= m.accessors.size()) {
		return Fail(err, "sampler: output accessor " + std::to_string(outIdx) + " out of range");
	}
	const auto &ia = m.accessors[static_cast<size_t>(in)];
	if (static_cast<int>(ia.type) != kTypeScalar || static_cast<int>(ia.componentType) != kComponentFloat) {
		return Fail(err, "sampler: input accessor " + std::to_string(in) + " is not SCALAR FLOAT");
	}
	const auto &oa = m.accessors[static_cast<size_t>(outIdx)];
	return CheckAnimationSamplerCounts(static_cast<size_t>(ia.count), static_cast<size_t>(oa.count),
	                                   std::string(s.interpolation), valuesPerKey, err);
}

} // namespace gltf
} // namespace dxr
