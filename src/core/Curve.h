#pragma once

// Attrib `Curve` (0x2C):
//   +0x00 float xMin, xMax      real-unit domain
//   +0x08 float yMin, yMax      real-unit range
//   +0x10 VariableArray X       { float* mArray; u32 mLength }, values normalised to 0..1
//   +0x18 VariableArray Y       normalised to 0..1
//   +0x20 VariableArray Y2      natural cubic spline second derivatives of the
//                               NORMALISED arrays; the game never recomputes them
//   +0x28 bool allocatedMemory  0 for attribute-owned data
// Undercover's (0x30, its Wii version's too) has a float graphScale at +0x10
// and the rest 4 bytes later (CurveAt).
// The game evaluates it with Numerical Recipes `splint`. Any edit to X or Y
// must regenerate Y2 (tools/attrib/curve.py has the background).

#include "Bytes.h"

#include <vector>

namespace attrib
{

	struct Attribute;
	struct DataBlock;

	struct CurveLayout
	{
		uint32_t arrays;    // offset of X; Y and Y2 follow, 8 bytes each
		uint32_t allocated; // offset of allocatedMemory
		bool graphScale;    // a float graphScale at 0x10
	};
	inline CurveLayout CurveAt(uint32_t size)
	{
		return size == 48 ? CurveLayout{ 0x14, 0x2C, true } : CurveLayout{ 0x10, 0x28, false };
	}

	struct CurveValue
	{
		float xMin = 0, xMax = 1, yMin = 0, yMax = 1;
		float graphScale = 0; // Undercover's only
		std::vector<float> x, y, y2; // normalised
		uint8_t allocatedMemory = 0;

		float realX(size_t i) const
		{
			return xMin + x[i] * (xMax - xMin);
		}
		float realY(size_t i) const
		{
			return yMin + y[i] * (yMax - yMin);
		}
	};

	// Natural cubic spline second derivatives (y2[0] = y2[n-1] = 0).
	std::vector<float> NaturalY2(const std::vector<float>& x, const std::vector<float>& y);
	// splint on normalised arrays; xn is normalised.
	float SplineEval(const CurveValue& c, float xn);
	// Evaluates at a real-unit x, returning a real-unit y.
	float CurveEvalReal(const CurveValue& c, float x);

	// The Curve of `size` bytes (44, or Undercover's 48) at byte `offset` of `b`
	// (an attribute's data or a nested block).
	CurveValue ReadCurve(const DataBlock& b, uint32_t offset, Endian e, uint32_t size);
	// Writes the curve back, resizing the X/Y/Y2 blocks. Y2 is taken as given;
	// call NaturalY2 first after changing X or Y.
	void WriteCurve(DataBlock& b, uint32_t offset, const CurveValue& c, Endian e, uint32_t size);

	// Element `index` of a Curve attribute.
	CurveValue ReadCurve(const Attribute& a, uint32_t index, Endian e);
	void WriteCurve(Attribute& a, uint32_t index, const CurveValue& c, Endian e);

} // namespace attrib
