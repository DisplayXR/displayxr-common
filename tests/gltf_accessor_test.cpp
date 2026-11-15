// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Contract test for gltf_accessor.h (the validated glTF accessor reader).
 *
 * Drives the template readers with small structs shaped like tinygltf's
 * (same member names; displayxr-common does not depend on tinygltf), built
 * from in-memory buffers — no asset files. Covers every decode path (each
 * componentType, normalized and not, strided, no bufferView, sparse) and every
 * rejection (bad indices, ranges past the view or buffer, wrong type, bad
 * sparse data, out-of-range indices / joints, sampler count mismatches).
 */

#include "gltf_accessor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                          \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

// --- tinygltf-shaped mocks ---------------------------------------------------

struct MBuffer
{
	std::vector<unsigned char> data;
};

struct MBufferView
{
	int buffer = -1;
	size_t byteOffset = 0;
	size_t byteLength = 0;
	size_t byteStride = 0;
};

struct MAccessor
{
	int bufferView = -1;
	size_t byteOffset = 0;
	bool normalized = false;
	int componentType = -1;
	size_t count = 0;
	int type = -1;
	struct Sparse
	{
		int count = 0;
		bool isSparse = false;
		struct
		{
			size_t byteOffset = 0;
			int bufferView = -1;
			int componentType = -1;
		} indices;
		struct
		{
			int bufferView = -1;
			size_t byteOffset = 0;
		} values;
	} sparse;
};

struct MModel
{
	std::vector<MAccessor> accessors;
	std::vector<MBufferView> bufferViews;
	std::vector<MBuffer> buffers;
};

struct MSampler
{
	int input = -1;
	int output = -1;
	std::string interpolation = "LINEAR";
};

namespace g = dxr::gltf;

template <class T>
static void
Append(std::vector<unsigned char> &b, const T &v)
{
	const size_t at = b.size();
	b.resize(at + sizeof(T));
	std::memcpy(b.data() + at, &v, sizeof(T));
}

// One buffer holding `bytes`, one view over all of it, one accessor on it.
static MModel
OneAccessor(const std::vector<unsigned char> &bytes, int componentType, int type, size_t count, bool normalized = false,
            size_t stride = 0)
{
	MModel m;
	m.buffers.push_back(MBuffer{bytes});
	MBufferView bv;
	bv.buffer = 0;
	bv.byteLength = bytes.size();
	bv.byteStride = stride;
	m.bufferViews.push_back(bv);
	MAccessor a;
	a.bufferView = 0;
	a.componentType = componentType;
	a.type = type;
	a.count = count;
	a.normalized = normalized;
	m.accessors.push_back(a);
	return m;
}

static bool
Near(float a, float b)
{
	return std::fabs(a - b) < 1e-6f;
}

// --- decode paths --------------------------------------------------------------

static void
TestDecodeFloat()
{
	std::vector<unsigned char> b;
	const float v[6] = {1.0f, -2.5f, 3.25f, 0.0f, 7.0f, -8.0f};
	for (float f : v)
		Append(b, f);
	MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 2);
	std::vector<float> out;
	std::string err;
	int comps = 0;
	CHECK(g::ReadFloats(m, 0, 3, out, &err, &comps), "FLOAT VEC3 reads");
	CHECK(comps == 3 && out.size() == 6, "FLOAT VEC3 size");
	bool same = out.size() == 6;
	for (size_t i = 0; same && i < 6; i++)
		same = out[i] == v[i];
	CHECK(same, "FLOAT values pass through");
}

static void
TestDecodeIntegers()
{
	std::string err;
	std::vector<float> out;

	{ // UNSIGNED_BYTE normalized + raw
		std::vector<unsigned char> b = {0, 255, 51, 128};
		MModel m = OneAccessor(b, g::kComponentUnsignedByte, g::kTypeVec4, 1, true);
		CHECK(g::ReadFloats(m, 0, 4, out, &err), "UBYTE normalized reads");
		CHECK(out.size() == 4 && Near(out[0], 0.0f) && Near(out[1], 1.0f) && Near(out[2], 0.2f) &&
		          Near(out[3], 128.0f / 255.0f),
		      "UBYTE normalized = c/255");
		m.accessors[0].normalized = false;
		CHECK(g::ReadFloats(m, 0, 4, out, &err) && out[1] == 255.0f && out[3] == 128.0f, "UBYTE raw = c");
	}
	{ // BYTE normalized: max(c/127, -1)
		std::vector<unsigned char> b;
		const int8_t v[4] = {127, -127, -128, 0};
		for (int8_t x : v)
			Append(b, x);
		MModel m = OneAccessor(b, g::kComponentByte, g::kTypeVec4, 1, true);
		CHECK(g::ReadFloats(m, 0, 4, out, &err), "BYTE normalized reads");
		CHECK(out.size() == 4 && Near(out[0], 1.0f) && Near(out[1], -1.0f) && out[2] == -1.0f && out[3] == 0.0f,
		      "BYTE normalized = max(c/127,-1)");
		m.accessors[0].normalized = false;
		CHECK(g::ReadFloats(m, 0, 4, out, &err) && out[2] == -128.0f, "BYTE raw = c");
	}
	{ // SHORT normalized: max(c/32767, -1)
		std::vector<unsigned char> b;
		const int16_t v[2] = {32767, -32768};
		for (int16_t x : v)
			Append(b, x);
		MModel m = OneAccessor(b, g::kComponentShort, g::kTypeVec2, 1, true);
		CHECK(g::ReadFloats(m, 0, 2, out, &err), "SHORT normalized reads");
		CHECK(out.size() == 2 && Near(out[0], 1.0f) && out[1] == -1.0f, "SHORT normalized = max(c/32767,-1)");
		m.accessors[0].normalized = false;
		CHECK(g::ReadFloats(m, 0, 2, out, &err) && out[1] == -32768.0f, "SHORT raw = c");
	}
	{ // UNSIGNED_SHORT normalized: c/65535
		std::vector<unsigned char> b;
		const uint16_t v[2] = {65535, 13107};
		for (uint16_t x : v)
			Append(b, x);
		MModel m = OneAccessor(b, g::kComponentUnsignedShort, g::kTypeVec2, 1, true);
		CHECK(g::ReadFloats(m, 0, 2, out, &err), "USHORT normalized reads");
		CHECK(out.size() == 2 && Near(out[0], 1.0f) && Near(out[1], 0.2f), "USHORT normalized = c/65535");
	}
	{ // UNSIGNED_INT (never normalized)
		std::vector<unsigned char> b;
		Append(b, uint32_t(70000));
		MModel m = OneAccessor(b, g::kComponentUnsignedInt, g::kTypeScalar, 1);
		CHECK(g::ReadFloats(m, 0, 1, out, &err) && out.size() == 1 && out[0] == 70000.0f, "UINT raw = c");
		m.accessors[0].normalized = true;
		CHECK(!g::ReadFloats(m, 0, 1, out, &err) && out.empty(), "UINT normalized rejected");
	}
	{ // FLOAT normalized is invalid glTF
		std::vector<unsigned char> b;
		Append(b, 1.0f);
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeScalar, 1, true);
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "FLOAT normalized rejected");
	}
}

static void
TestStrideAndOffsets()
{
	// Interleaved: [vec2 float][4 pad bytes] x 3, stride 12; view starts 4
	// bytes into the buffer and the accessor 0 bytes into the view.
	std::vector<unsigned char> b(4, 0xEE);
	for (int i = 0; i < 3; i++) {
		Append(b, float(i));
		Append(b, float(10 * i));
		Append(b, uint32_t(0xDEADBEEF));
	}
	MModel m;
	m.buffers.push_back(MBuffer{b});
	MBufferView bv;
	bv.buffer = 0;
	bv.byteOffset = 4;
	bv.byteLength = 36;
	bv.byteStride = 12;
	m.bufferViews.push_back(bv);
	MAccessor a;
	a.bufferView = 0;
	a.componentType = g::kComponentFloat;
	a.type = g::kTypeVec2;
	a.count = 3;
	m.accessors.push_back(a);
	std::vector<float> out;
	std::string err;
	CHECK(g::ReadFloats(m, 0, 2, out, &err), "strided read");
	CHECK(out.size() == 6 && out[2] == 1.0f && out[3] == 10.0f && out[4] == 2.0f && out[5] == 20.0f,
	      "strided values skip padding");

	// The last element may end exactly at the view end with a stride larger
	// than the element: extent = (count-1)*stride + elemSize = 32 <= 36.
	m.bufferViews[0].byteLength = 32;
	CHECK(g::ReadFloats(m, 0, 2, out, &err), "extent uses elemSize for the last element");

	// Stride smaller than an element is rejected.
	m.bufferViews[0].byteStride = 4;
	CHECK(!g::ReadFloats(m, 0, 2, out, &err), "stride < element size rejected");
}

static void
TestNoBufferView()
{
	MModel m;
	MAccessor a;
	a.componentType = g::kComponentFloat;
	a.type = g::kTypeVec3;
	a.count = 4;
	m.accessors.push_back(a);
	std::vector<float> out;
	std::string err;
	CHECK(g::ReadFloats(m, 0, 3, out, &err), "no bufferView reads as zeros: " );
	bool zeros = out.size() == 12;
	for (float f : out)
		zeros = zeros && f == 0.0f;
	CHECK(zeros, "no bufferView -> 12 zeros");

	m.accessors[0].count = g::kMaxZeroFillElements + 1;
	CHECK(!g::ReadFloats(m, 0, 3, out, &err) && out.empty(), "no bufferView count over the zero-fill cap rejected");
}

// Sparse accessor: base of `count` zeros or values, with overrides.
static MModel
SparseModel(bool withBase)
{
	MModel m;
	std::vector<unsigned char> b;
	// base: 4 x SCALAR float 1,2,3,4 at [0,16)
	for (int i = 1; i <= 4; i++)
		Append(b, float(i));
	// indices: uint16 {1, 3} at [16,20)
	Append(b, uint16_t(1));
	Append(b, uint16_t(3));
	// values: float {20, 40} at [20,28)
	Append(b, 20.0f);
	Append(b, 40.0f);
	m.buffers.push_back(MBuffer{b});
	MBufferView base, idx, val;
	base.buffer = idx.buffer = val.buffer = 0;
	base.byteOffset = 0;
	base.byteLength = 16;
	idx.byteOffset = 16;
	idx.byteLength = 4;
	val.byteOffset = 20;
	val.byteLength = 8;
	m.bufferViews = {base, idx, val};
	MAccessor a;
	a.bufferView = withBase ? 0 : -1;
	a.componentType = g::kComponentFloat;
	a.type = g::kTypeScalar;
	a.count = 4;
	a.sparse.isSparse = true;
	a.sparse.count = 2;
	a.sparse.indices.bufferView = 1;
	a.sparse.indices.componentType = g::kComponentUnsignedShort;
	a.sparse.values.bufferView = 2;
	m.accessors.push_back(a);
	return m;
}

static void
TestSparse()
{
	std::vector<float> out;
	std::string err;
	{
		MModel m = SparseModel(true);
		CHECK(g::ReadFloats(m, 0, 1, out, &err), "sparse over base reads");
		CHECK(out.size() == 4 && out[0] == 1.0f && out[1] == 20.0f && out[2] == 3.0f && out[3] == 40.0f,
		      "sparse overrides base");
	}
	{
		MModel m = SparseModel(false);
		CHECK(g::ReadFloats(m, 0, 1, out, &err), "sparse over zeros reads");
		CHECK(out.size() == 4 && out[0] == 0.0f && out[1] == 20.0f && out[2] == 0.0f && out[3] == 40.0f,
		      "sparse overrides zeros");
	}
	// Rejections.
	{
		MModel m = SparseModel(true);
		m.buffers[0].data[18] = 4; // second sparse index = 4 >= count
		CHECK(!g::ReadFloats(m, 0, 1, out, &err) && out.empty(), "sparse index >= count rejected");
	}
	{
		MModel m = SparseModel(true);
		m.accessors[0].sparse.count = 5; // > count
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "sparse count > accessor count rejected");
		m.accessors[0].sparse.count = 0;
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "sparse count 0 rejected");
	}
	{
		MModel m = SparseModel(true);
		m.accessors[0].sparse.indices.bufferView = 9;
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "sparse indices bufferView out of range rejected");
	}
	{
		MModel m = SparseModel(true);
		m.accessors[0].sparse.values.bufferView = -1;
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "sparse values with no bufferView rejected");
	}
	{
		MModel m = SparseModel(true);
		m.accessors[0].sparse.values.byteOffset = 4; // 4 + 2*4 > 8
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "sparse values extent overrun rejected");
	}
	{
		MModel m = SparseModel(true);
		m.accessors[0].sparse.indices.byteOffset = 2; // 2 + 2*2 > 4
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "sparse indices extent overrun rejected");
	}
	{
		MModel m = SparseModel(true);
		m.accessors[0].sparse.indices.componentType = g::kComponentFloat;
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "sparse indices of FLOAT rejected");
	}
}

// --- rejections -------------------------------------------------------------------

static void
TestRejections()
{
	std::vector<unsigned char> b;
	for (int i = 0; i < 6; i++)
		Append(b, float(i));
	std::vector<float> out;
	std::string err;

	{
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 2);
		CHECK(!g::ReadFloats(m, 1, 3, out, &err), "accessor index out of range rejected");
		CHECK(!g::ReadFloats(m, -1, 3, out, &err), "negative accessor index rejected");
	}
	{
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 2);
		m.accessors[0].bufferView = 3;
		CHECK(!g::ReadFloats(m, 0, 3, out, &err), "bufferView out of range rejected");
		CHECK(err.find("bufferView 3") != std::string::npos, "reason names the bufferView");
	}
	{
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 2);
		m.bufferViews[0].buffer = 1;
		CHECK(!g::ReadFloats(m, 0, 3, out, &err), "bufferView.buffer out of range rejected");
	}
	{
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 2);
		m.bufferViews[0].byteOffset = 4; // 4 + 24 > 24
		CHECK(!g::ReadFloats(m, 0, 3, out, &err), "bufferView past buffer end rejected");
		m.bufferViews[0].byteOffset = 0;
		m.bufferViews[0].byteLength = static_cast<size_t>(-1); // overflow guard
		CHECK(!g::ReadFloats(m, 0, 3, out, &err), "huge bufferView length rejected");
	}
	{
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 3); // 36 bytes > 24
		CHECK(!g::ReadFloats(m, 0, 3, out, &err) && out.empty(), "accessor count overrun rejected");
		m.accessors[0].count = 2;
		m.accessors[0].byteOffset = 4; // 4 + 24 > 24
		CHECK(!g::ReadFloats(m, 0, 3, out, &err), "accessor byteOffset overrun rejected");
		m.accessors[0].byteOffset = 0;
		m.accessors[0].count = static_cast<size_t>(-1); // (count-1)*stride overflow guard
		CHECK(!g::ReadFloats(m, 0, 3, out, &err), "huge accessor count rejected");
	}
	{
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 2);
		CHECK(!g::ReadFloats(m, 0, 2, out, &err), "VEC3 read as VEC2 rejected");
		CHECK(!g::ReadFloats(m, 0, 4, out, &err), "VEC3 read as VEC4 rejected");
		m.accessors[0].type = 77;
		CHECK(!g::ReadFloats(m, 0, 0, out, &err), "unknown type rejected");
	}
	{
		MModel m = OneAccessor(b, 5124 /* INT: not allowed in glTF accessors */, g::kTypeScalar, 1);
		CHECK(!g::ReadFloats(m, 0, 1, out, &err), "componentType INT rejected");
	}
	{
		std::vector<unsigned char> mb(16, 1);
		MModel m = OneAccessor(mb, g::kComponentUnsignedByte, g::kTypeMat3, 1);
		CHECK(!g::ReadFloats(m, 0, 9, out, &err), "column-padded MAT3 of UBYTE rejected");
	}
	{
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec3, 2);
		CHECK(g::ReadFloats(m, 0, 0, out, &err) && out.size() == 6, "expectedComponents 0 accepts any type");
	}
}

// --- indices / joints / samplers -------------------------------------------------------

static void
TestIndices()
{
	std::vector<uint32_t> idx;
	std::string err;
	for (int ct : {g::kComponentUnsignedByte, g::kComponentUnsignedShort, g::kComponentUnsignedInt}) {
		std::vector<unsigned char> b;
		const uint32_t v[3] = {0, 2, 1};
		for (uint32_t x : v) {
			if (ct == g::kComponentUnsignedByte)
				Append(b, uint8_t(x));
			else if (ct == g::kComponentUnsignedShort)
				Append(b, uint16_t(x));
			else
				Append(b, x);
		}
		MModel m = OneAccessor(b, ct, g::kTypeScalar, 3);
		CHECK(g::ReadIndices(m, 0, 3, idx, &err) && idx.size() == 3 && idx[1] == 2, "indices decode");
		CHECK(!g::ReadIndices(m, 0, 2, idx, &err) && idx.empty(), "index >= vertex count rejected");
		CHECK(err.find("index 2") != std::string::npos, "reason names the index");
	}
	std::vector<unsigned char> fb;
	Append(fb, 1.0f);
	MModel fm = OneAccessor(fb, g::kComponentFloat, g::kTypeScalar, 1);
	CHECK(!g::ReadIndices(fm, 0, 10, idx, &err), "FLOAT indices rejected");
	std::vector<unsigned char> vb(6, 0);
	MModel vm = OneAccessor(vb, g::kComponentUnsignedShort, g::kTypeVec3, 1);
	CHECK(!g::ReadIndices(vm, 0, 10, idx, &err), "VEC3 indices rejected");
}

static void
TestJoints()
{
	std::vector<uint16_t> j;
	std::string err;
	{
		std::vector<unsigned char> b = {0, 1, 2, 3, 3, 2, 1, 0};
		MModel m = OneAccessor(b, g::kComponentUnsignedByte, g::kTypeVec4, 2);
		CHECK(g::ReadJoints(m, 0, 4, j, &err) && j.size() == 8 && j[3] == 3 && j[4] == 3, "UBYTE joints decode");
		CHECK(!g::ReadJoints(m, 0, 3, j, &err) && j.empty(), "joint >= joint count rejected");
	}
	{
		std::vector<unsigned char> b;
		for (uint16_t x : {uint16_t(0), uint16_t(300), uint16_t(1), uint16_t(2)})
			Append(b, x);
		MModel m = OneAccessor(b, g::kComponentUnsignedShort, g::kTypeVec4, 1);
		CHECK(g::ReadJoints(m, 0, 301, j, &err) && j.size() == 4 && j[1] == 300, "USHORT joints decode");
		CHECK(!g::ReadJoints(m, 0, 300, j, &err), "USHORT joint == joint count rejected");
		m.accessors[0].normalized = true;
		CHECK(!g::ReadJoints(m, 0, 301, j, &err), "normalized joints rejected");
	}
	{
		std::vector<unsigned char> b(16, 0);
		MModel m = OneAccessor(b, g::kComponentFloat, g::kTypeVec4, 1);
		CHECK(!g::ReadJoints(m, 0, 4, j, &err), "FLOAT joints rejected");
		m.accessors[0].type = g::kTypeVec3;
		CHECK(!g::ReadJoints(m, 0, 4, j, &err), "VEC3 joints rejected");
	}
}

static void
TestSamplers()
{
	std::string err;
	CHECK(g::CheckAnimationSamplerCounts(5, 5, "LINEAR", 1, &err), "LINEAR 5 -> 5");
	CHECK(g::CheckAnimationSamplerCounts(5, 5, "STEP", 1, &err), "STEP 5 -> 5");
	CHECK(g::CheckAnimationSamplerCounts(5, 5, "", 1, &err), "empty interpolation = LINEAR");
	CHECK(g::CheckAnimationSamplerCounts(5, 15, "CUBICSPLINE", 1, &err), "CUBICSPLINE 5 -> 15");
	CHECK(g::CheckAnimationSamplerCounts(4, 8, "LINEAR", 2, &err), "weights: 2 targets x 4 keys");
	CHECK(g::CheckAnimationSamplerCounts(4, 24, "CUBICSPLINE", 2, &err), "weights cubic: 3 x 2 x 4");
	CHECK(!g::CheckAnimationSamplerCounts(5, 4, "LINEAR", 1, &err), "LINEAR output short rejected");
	CHECK(!g::CheckAnimationSamplerCounts(5, 5, "CUBICSPLINE", 1, &err), "CUBICSPLINE output not 3x rejected");
	CHECK(!g::CheckAnimationSamplerCounts(1, 3, "CUBICSPLINE", 1, &err), "CUBICSPLINE with 1 key rejected");
	CHECK(!g::CheckAnimationSamplerCounts(0, 0, "LINEAR", 1, &err), "no keyframes rejected");
	CHECK(!g::CheckAnimationSamplerCounts(2, 2, "BOUNCE", 1, &err), "unknown interpolation rejected");
	CHECK(!g::CheckAnimationSamplerCounts(static_cast<size_t>(-1) / 2, 0, "CUBICSPLINE", 1, &err),
	      "count overflow rejected");

	// Model-level: indices in range, input SCALAR FLOAT.
	MModel m;
	MAccessor in, out;
	in.componentType = g::kComponentFloat;
	in.type = g::kTypeScalar;
	in.count = 3;
	out.componentType = g::kComponentFloat;
	out.type = g::kTypeVec4;
	out.count = 3;
	m.accessors = {in, out};
	MSampler s;
	s.input = 0;
	s.output = 1;
	CHECK(g::CheckAnimationSampler(m, s, 1, &err), "sampler model check passes");
	s.output = 2;
	CHECK(!g::CheckAnimationSampler(m, s, 1, &err), "sampler output index out of range rejected");
	s.output = 1;
	s.input = -1;
	CHECK(!g::CheckAnimationSampler(m, s, 1, &err), "sampler input index out of range rejected");
	s.input = 1; // VEC4 input
	CHECK(!g::CheckAnimationSampler(m, s, 1, &err), "non-SCALAR sampler input rejected");
	s.input = 0;
	s.interpolation = "CUBICSPLINE";
	CHECK(!g::CheckAnimationSampler(m, s, 1, &err), "sampler count mismatch rejected");
}

int
main()
{
	TestDecodeFloat();
	TestDecodeIntegers();
	TestStrideAndOffsets();
	TestNoBufferView();
	TestSparse();
	TestRejections();
	TestIndices();
	TestJoints();
	TestSamplers();
	if (g_failures != 0) {
		std::fprintf(stderr, "gltf_accessor_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("gltf_accessor_test: OK\n");
	return 0;
}
