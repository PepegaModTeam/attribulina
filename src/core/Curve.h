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

#include <string>
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

	// Natural cubic spline second derivatives (y2[0] = y2[n-1] = 0). X that
	// repeats or goes back (a mod's [0, 0.5, 1, 1]) has no spline through it:
	// each rising run of X gets its own, and a value that still isn't finite is 0,
	// so a NaN never reaches the file.
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

	// ------------------------------------------------ curves in a matrix ---
	//
	// Every Attrib::Types::Matrix field of the supported games holds a cubic
	// Bezier curve: four rows of (x, y, 0, 0) control points, P0 and P3 its ends
	// and P1 and P2 its handles. Attribulator edited them in a graph; the stored
	// values are pixel fractions (0.4485981 is 48/107, 0.9827586 is 57/58). The
	// games read them in one of two ways.

	enum class MatrixCurveRead : uint8_t
	{
		// GetValueFromSpline(t, m): the cubic Bernstein polynomial of the y column
		// at t in 0..1; the x column is ignored. As a graph, the Bezier whose
		// handles sit at t = 1/3 and 2/3.
		YOverT,
		// The Bezier through all four points, (x(t), y(t)), sampled and read as y
		// over x: SplineMap (64 points at t = i/63, linear between them, the end
		// values past them), FillGraphFromSpline.
		Bezier,
	};

	struct MatrixCurveInfo
	{
		MatrixCurveRead read = MatrixCurveRead::Bezier;
		// What reads it in the game, for the editor's tooltip; nullptr for a field
		// no code was found for (drawn as the Bezier, as Attribulator drew it).
		const char* reader = nullptr;
	};
	// How the game reads class `cls`'s Matrix field `field`.
	MatrixCurveInfo MatrixCurveOf(const std::string& cls, const std::string& field);

	// The x and y columns of a 64-byte matrix.
	struct CurvePoints
	{
		float x[4] = {}, y[4] = {};
	};
	CurvePoints ReadCurvePoints(const uint8_t* m, Endian e);
	// Sets row i's x and y, leaving its z and w as they are.
	void WriteCurvePoint(uint8_t* m, int i, float x, float y, Endian e);

	// GetValueFromSpline: (1-t)^3 y0 + 3t(1-t)^2 y1 + 3t^2(1-t) y2 + t^3 y3.
	float CurveYAt(const CurvePoints& c, float t);
	// The Bezier's point at t.
	void CurvePointAt(const CurvePoints& c, float t, float& x, float& y);
	// SplineMap::GetMapValue after SplineMap::Update: y at x on the Bezier sampled
	// at 64 points, the first segment holding x from the start, the end values
	// before the first point and past the last. 0 when no segment holds x, as the
	// game returns.
	float SplineMapValue(const CurvePoints& c, float x);
	// What the game makes of the curve at x (for YOverT x is t; clamped to 0..1).
	float MatrixCurveValue(const CurvePoints& c, MatrixCurveRead r, float x);

} // namespace attrib
