#include "Curve.h"

#include "Database.h"

#include <algorithm>
#include <cmath>

namespace attrib
{

	namespace
	{

		std::vector<float> ReadArray(const DataBlock& b, uint32_t base, uint32_t field, Endian e)
		{
			const uint32_t len = load32(b.bytes.data() + base + field + 4, e);
			std::vector<float> out;
			const PtrSlot* p = b.ptrAt(base + field);
			if (!p || !p->data)
				return out;
			const uint32_t n = std::min<uint32_t>(len, uint32_t(p->data->bytes.size() / 4));
			out.resize(n);
			for (uint32_t i = 0; i < n; ++i)
				out[i] = loadf32(p->data->bytes.data() + i * 4, e);
			return out;
		}

		void WriteArray(DataBlock& b, uint32_t base, uint32_t field, const std::vector<float>& v, Endian e)
		{
			store32(b.bytes.data() + base + field + 4, uint32_t(v.size()), e);
			PtrSlot* p = b.ptrAt(base + field);
			if (!p)
			{
				b.ptrs.push_back(PtrSlot{});
				p = &b.ptrs.back();
				p->offset = base + field;
			}
			p->isString = false;
			p->text.clear();
			p->data = std::make_shared<DataBlock>();
			p->data->bytes.resize(v.size() * 4);
			for (size_t i = 0; i < v.size(); ++i)
				storef32(p->data->bytes.data() + i * 4, v[i], e);
		}
	} // namespace

	namespace
	{
		std::vector<float> Spline(const std::vector<float>& x, const std::vector<float>& y)
		{
			const size_t n = std::min(x.size(), y.size());
			std::vector<float> y2(n, 0.0f);
			if (n < 3)
				return y2;
			// Numerical Recipes `spline` with natural end conditions, in float like the game data.
			std::vector<float> u(n, 0.0f);
			for (size_t i = 1; i + 1 < n; ++i)
			{
				const float sig = (x[i] - x[i - 1]) / (x[i + 1] - x[i - 1]);
				const float p = sig * y2[i - 1] + 2.0f;
				y2[i] = (sig - 1.0f) / p;
				u[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]) - (y[i] - y[i - 1]) / (x[i] - x[i - 1]);
				u[i] = (6.0f * u[i] / (x[i + 1] - x[i - 1]) - sig * u[i - 1]) / p;
			}
			y2[n - 1] = 0.0f;
			for (size_t k = n - 1; k-- > 0;)
				y2[k] = y2[k] * y2[k + 1] + u[k];
			return y2;
		}

		bool AllFinite(const std::vector<float>& v)
		{
			return std::all_of(v.begin(), v.end(), [](float f)
			{
				return std::isfinite(f);
			});
		}
	} // namespace

	std::vector<float> NaturalY2(const std::vector<float>& x, const std::vector<float>& y)
	{
		std::vector<float> y2 = Spline(x, y);
		if (AllFinite(y2)) return y2;
		// X repeats or goes back somewhere (an interval of no width divides by
		// zero): a spline over each rising run, its ends natural.
		const size_t n = y2.size();
		std::fill(y2.begin(), y2.end(), 0.0f);
		size_t b = 0;
		for (size_t i = 1; i <= n; ++i)
		{
			if (i < n && x[i] > x[i - 1]) continue;
			if (i - b >= 3)
			{
				const std::vector<float> run = Spline(std::vector<float>(x.begin() + b, x.begin() + i), std::vector<float>(y.begin() + b, y.begin() + i));
				std::copy(run.begin(), run.end(), y2.begin() + b);
			}
			b = i;
		}
		for (float& f : y2)
			if (!std::isfinite(f)) f = 0.0f;
		return y2;
	}

	float SplineEval(const CurveValue& c, float xn)
	{
		const size_t n = std::min({ c.x.size(), c.y.size(), c.y2.size() });
		if (n == 0) return 0.0f;
		if (n == 1) return c.y[0];
		size_t lo = 0, hi = n - 1;
		while (hi - lo > 1)
		{
			const size_t k = (hi + lo) / 2;
			if (c.x[k] > xn) hi = k; else lo = k;
		}
		const float h = c.x[hi] - c.x[lo];
		if (h == 0.0f) return c.y[lo];
		const float a = (c.x[hi] - xn) / h, b = (xn - c.x[lo]) / h;
		return a * c.y[lo] + b * c.y[hi] + ((a * a * a - a) * c.y2[lo] + (b * b * b - b) * c.y2[hi]) * (h * h) / 6.0f;
	}

	float CurveEvalReal(const CurveValue& c, float x)
	{
		const float dx = c.xMax - c.xMin;
		const float xn = dx != 0.0f ? (x - c.xMin) / dx : 0.0f;
		return c.yMin + SplineEval(c, xn) * (c.yMax - c.yMin);
	}

	CurveValue ReadCurve(const DataBlock& b, uint32_t base, Endian e, uint32_t size)
	{
		const CurveLayout l = CurveAt(size);
		CurveValue c;
		const uint8_t* p = b.bytes.data() + base;
		c.xMin = loadf32(p, e);
		c.xMax = loadf32(p + 4, e);
		c.yMin = loadf32(p + 8, e);
		c.yMax = loadf32(p + 12, e);
		if (l.graphScale) c.graphScale = loadf32(p + 16, e);
		c.x = ReadArray(b, base, l.arrays, e);
		c.y = ReadArray(b, base, l.arrays + 8, e);
		c.y2 = ReadArray(b, base, l.arrays + 16, e);
		c.allocatedMemory = p[l.allocated];
		return c;
	}

	void WriteCurve(DataBlock& b, uint32_t base, const CurveValue& c, Endian e, uint32_t size)
	{
		const CurveLayout l = CurveAt(size);
		uint8_t* p = b.bytes.data() + base;
		storef32(p, c.xMin, e);
		storef32(p + 4, c.xMax, e);
		storef32(p + 8, c.yMin, e);
		storef32(p + 12, c.yMax, e);
		if (l.graphScale) storef32(p + 16, c.graphScale, e);
		WriteArray(b, base, l.arrays, c.x, e);
		WriteArray(b, base, l.arrays + 8, c.y, e);
		WriteArray(b, base, l.arrays + 16, c.y2, e);
		p[l.allocated] = c.allocatedMemory;
	}

	CurveValue ReadCurve(const Attribute& a, uint32_t index, Endian e)
	{
		return ReadCurve(a.data, index * a.elemSize, e, a.elemSize);
	}

	void WriteCurve(Attribute& a, uint32_t index, const CurveValue& c, Endian e)
	{
		WriteCurve(a.data, index * a.elemSize, c, e, a.elemSize);
	}

	// ------------------------------------------------ curves in a matrix ---

	MatrixCurveInfo MatrixCurveOf(const std::string& cls, const std::string& field)
	{
		using R = MatrixCurveRead;
		struct Rule
		{
			const char* cls;
			const char* field; // nullptr: every Matrix field of the class
			R read;
			const char* reader;
		};
		static const Rule kRules[] = {
			// Most Wanted's bloom curves, baked into a lookup texture: on PC 256
			// texels at t = i/255 (0x6C3B20, GetValueFromSpline at 0x73DAB0; black
			// bloom in red, colour bloom in green, the detail map curve in blue,
			// which no shader reads), on the GameCube 64 entries
			// (IVisualTreatmentPlat::UpdateIndirectTexture).
			{ "visuallook", "BlackBloomCurve", R::YOverT, "The game evaluates the Y column at evenly spaced t with GetValueFromSpline, for the bloom lookup texture." },
			{ "visuallook", "ColourBloomCurve", R::YOverT, "The game evaluates the Y column at evenly spaced t with GetValueFromSpline, for the bloom lookup texture." },
			{ "visuallook", "DetailMapCurve", R::YOverT, "Most Wanted PC evaluates the Y column at evenly spaced t into the lookup texture's blue channel, which no visual treatment shader reads." },
			// Carbon's colour curves (IVisualTreatmentPlat's SplineMaps, built from
			// visuallook/colorcurves); the later games' are inferred from the same
			// fields and SplineMap in their headers.
			{ "visuallook", nullptr, R::Bezier, "The game samples the Bezier in X and Y at 64 points and looks Y up by X, in SplineMap." },
			// VisualLookEffect::UpdateActive: elapsed time over length is t, the
			// value times magnitude.
			{ "visuallookeffect", "graph", R::YOverT, "The game evaluates the Y column at t = elapsed time / length and multiplies it by magnitude." },
			// SFXCTL_Physics::UpdateParams (Most Wanted GameCube, Carbon Wii).
			{ "engineaudio", "PhysicsRPM_Map", R::YOverT, "The game evaluates the Y column at evenly spaced t with GetValueFromSpline." },
			// SFXCTL_Shifting (Most Wanted GameCube): FillGraphFromSpline samples the
			// Bezier into a graph of (x, y) points.
			{ "shiftpattern", nullptr, R::Bezier, "The game samples the Bezier in X and Y into a graph of points, in FillGraphFromSpline." },
		};
		for (const Rule& r : kRules)
			if (cls == r.cls && (!r.field || field == r.field)) return { r.read, r.reader };
		return {};
	}

	CurvePoints ReadCurvePoints(const uint8_t* m, Endian e)
	{
		CurvePoints c;
		for (int i = 0; i < 4; ++i)
		{
			c.x[i] = loadf32(m + i * 16, e);
			c.y[i] = loadf32(m + i * 16 + 4, e);
		}
		return c;
	}

	void WriteCurvePoint(uint8_t* m, int i, float x, float y, Endian e)
	{
		storef32(m + i * 16, x, e);
		storef32(m + i * 16 + 4, y, e);
	}

	namespace
	{
		float Bernstein(const float v[4], float t)
		{
			const float u = 1.0f - t;
			return u * u * u * v[0] + 3.0f * t * u * u * v[1] + 3.0f * t * t * u * v[2] + t * t * t * v[3];
		}
	} // namespace

	float CurveYAt(const CurvePoints& c, float t)
	{
		return Bernstein(c.y, t);
	}

	void CurvePointAt(const CurvePoints& c, float t, float& x, float& y)
	{
		x = Bernstein(c.x, t);
		y = Bernstein(c.y, t);
	}

	float SplineMapValue(const CurvePoints& c, float x)
	{
		float mx[64], my[64];
		for (int i = 0; i < 64; ++i) CurvePointAt(c, float(i) * (1.0f / 63.0f), mx[i], my[i]);
		if (x < mx[0]) return my[0];
		if (x > mx[63]) return my[63];
		for (int i = 0; i < 63; ++i)
			if (mx[i] <= x && x <= mx[i + 1])
				return mx[i + 1] == mx[i] ? my[i] : my[i] + (x - mx[i]) * (my[i + 1] - my[i]) / (mx[i + 1] - mx[i]);
		return 0.0f;
	}

	float MatrixCurveValue(const CurvePoints& c, MatrixCurveRead r, float x)
	{
		return r == MatrixCurveRead::YOverT ? CurveYAt(c, std::clamp(x, 0.0f, 1.0f)) : SplineMapValue(c, x);
	}

} // namespace attrib
