#include "Paint.h"

#include "Database.h"
#include "Hash.h"
#include "Schema.h"

#include <algorithm>
#include <cmath>

namespace attrib
{

	namespace
	{
		constexpr uint32_t kOffSwatchClass = 16, kOffSwatchKey = 20, kOffKit = 28, kOffSat = 32, kOffVar = 36;
	} // namespace

	uint32_t PaintSwatchKey(int index)
	{
		return StringHash32("generic_paint_swatch" + std::to_string(index));
	}

	int PaintSwatchIndex(uint32_t collectionKey)
	{
		for (int i = 0; i < 6; ++i)
			if (PaintSwatchKey(i) == collectionKey) return i;
		return -1;
	}

	PaintColour ReadPaint(const DataBlock& b, uint32_t offset, Endian e)
	{
		PaintColour c;
		if (offset + 40 > b.bytes.size()) return c;
		const uint8_t* p = b.bytes.data() + offset;
		const int swatch = PaintSwatchIndex(load32(p + kOffSwatchKey, e));
		const uint32_t kit = load32(p + kOffKit, e);
		c.s = loadf32(p + kOffSat, e);
		c.v = loadf32(p + kOffVar, e);
		c.hueKnown = swatch >= 0;
		c.step = std::max(swatch, 0) * kPaintStepsPerSwatch + int(kit % kPaintStepsPerSwatch);
		float h = 1.0f - float(c.step) / float(kPaintHueSteps);
		h -= std::floor(h); // step 0 is hue 1.0, the same red as 0.0
		c.h = h;
		return c;
	}

	void WritePaint(DataBlock& b, uint32_t offset, float h, float s, float v, Endian e)
	{
		if (offset + 40 > b.bytes.size()) return;
		uint8_t* p = b.bytes.data() + offset;
		h -= std::floor(h);
		int step = int(std::lround((1.0f - h) * float(kPaintHueSteps))) % kPaintHueSteps;
		if (step < 0) step += kPaintHueSteps;
		if (load32(p + kOffSwatchClass, e) == 0) store32(p + kOffSwatchClass, StringHash32("car_part"), e);
		store32(p + kOffSwatchKey, PaintSwatchKey(step / kPaintStepsPerSwatch), e);
		store32(p + kOffKit, uint32_t(step % kPaintStepsPerSwatch), e);
		storef32(p + kOffSat, s, e);
		storef32(p + kOffVar, v, e);
	}

	namespace
	{
		constexpr uint32_t kOffSwatchIndex = 16, kOffSwatchSat = 20, kOffSwatchVar = 24;

		// The hue of a 0xAARRGGBB colour, 0..1, or -1 when it is grey.
		float HueOf(uint32_t argb)
		{
			const float r = float((argb >> 16) & 0xFF) / 255.0f, g = float((argb >> 8) & 0xFF) / 255.0f, b = float(argb & 0xFF) / 255.0f;
			const float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), d = mx - mn;
			if (d <= 0) return -1;
			float h = mx == r ? (g - b) / d : mx == g ? 2 + (b - r) / d : 4 + (r - g) / d;
			h /= 6;
			return h - std::floor(h);
		}

		float HueDistance(float a, float b)
		{
			const float d = std::fabs(a - b);
			return std::min(d, 1 - d);
		}

		// The swatch whose hue is nearest h (grey ones never), or -1.
		int NearestSwatch(const PaintSwatches& sw, float h)
		{
			int best = -1;
			for (size_t i = 0; i < sw.hue.size() && i < 256; ++i)
				if (sw.hue[i] >= 0 && (best < 0 || HueDistance(h, sw.hue[i]) < HueDistance(h, sw.hue[size_t(best)]))) best = int(i);
			return best;
		}

		uint8_t UnitByte(float f)
		{
			return uint8_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f));
		}
	} // namespace

	bool IsSwatchPaint(const StructDef& st)
	{
		if (st.name != "PresetRidePaint" || st.size != 32) return false;
		const MemberDef* m = st.member("mSwatch");
		return m && m->offset == kOffSwatchIndex && m->elemSize == 1 && m->count == 1;
	}

	PaintSwatches ReadPaintSwatches(const Database& db)
	{
		PaintSwatches sw;
		const uint32_t key = StringHash32("swatch");
		const Collection* c = db.findCollection(StringHash32("car_paint_swatch"), key);
		const Attribute* a = c ? c->resolve(key) : nullptr;
		if (!a || a->elemSize < 4 || !c->vault) return sw;
		for (uint32_t i = 0; i < a->count; ++i)
		{
			const uint32_t rgb = load32(a->elem(i), c->vault->raw.endian);
			sw.rgb.push_back(rgb);
			sw.hue.push_back(HueOf(rgb));
		}
		return sw;
	}

	PaintColour ReadSwatchPaint(const DataBlock& b, uint32_t offset, const PaintSwatches& sw, Endian e)
	{
		PaintColour c;
		if (offset + 32 > b.bytes.size()) return c;
		const uint8_t* p = b.bytes.data() + offset;
		c.step = p[kOffSwatchIndex];
		c.s = loadf32(p + kOffSwatchSat, e);
		c.v = loadf32(p + kOffSwatchVar, e);
		const size_t shown = size_t(c.step) < sw.hue.size() ? size_t(c.step) : 0;
		c.hueKnown = size_t(c.step) < sw.hue.size() && sw.hue[shown] >= 0;
		c.h = shown < sw.hue.size() ? std::max(sw.hue[shown], 0.0f) : 0.0f;
		return c;
	}

	void WriteSwatchPaint(DataBlock& b, uint32_t offset, float h, float s, float v, const PaintSwatches& sw, Endian e)
	{
		if (offset + 32 > b.bytes.size()) return;
		uint8_t* p = b.bytes.data() + offset;
		h -= std::floor(h);
		const PaintColour was = ReadSwatchPaint(b, offset, sw, e);
		// Only a new hue picks a swatch, so changing S or V keeps the one stored.
		if (HueDistance(h, was.h) > 1e-4f)
			if (const int best = NearestSwatch(sw, h); best >= 0) p[kOffSwatchIndex] = uint8_t(best);
		storef32(p + kOffSwatchSat, s, e);
		storef32(p + kOffSwatchVar, v, e);
	}

	bool IsVinylColour(const StructDef& st)
	{
		if (st.name != "VinylsAttrib::VinylColor" || st.size != 3) return false;
		const MemberDef* sw = st.member("swatch");
		const MemberDef* sat = st.member("saturation");
		const MemberDef* br = st.member("brightness");
		return sw && sat && br && sw->offset == 0 && sat->offset == 1 && br->offset == 2;
	}

	PaintColour ReadVinylColour(const DataBlock& b, uint32_t offset, const PaintSwatches& sw)
	{
		PaintColour c;
		if (offset + 3 > b.bytes.size()) return c;
		const uint8_t* p = b.bytes.data() + offset;
		c.step = p[0];
		c.s = float(p[1]) / 255.0f;
		c.v = float(p[2]) / 255.0f;
		const bool inTable = size_t(c.step) < sw.hue.size();
		c.hueKnown = inTable && sw.hue[size_t(c.step)] >= 0;
		c.h = c.hueKnown ? sw.hue[size_t(c.step)] : 0.0f;
		return c;
	}

	void WriteVinylColour(DataBlock& b, uint32_t offset, float h, float s, float v, const PaintSwatches& sw)
	{
		if (offset + 3 > b.bytes.size()) return;
		uint8_t* p = b.bytes.data() + offset;
		h -= std::floor(h);
		const PaintColour was = ReadVinylColour(b, offset, sw);
		if (HueDistance(h, was.h) > 1e-4f)
			if (const int best = NearestSwatch(sw, h); best >= 0) p[0] = uint8_t(best);
		p[1] = UnitByte(s);
		p[2] = UnitByte(v);
	}

	std::string PaintFinish(const KeyNames& names, uint32_t groupKey)
	{
		if (!groupKey) return {};
		const std::string n = names.format(groupKey);
		const size_t at = n.rfind("_group_");
		return at == std::string::npos ? n : n.substr(at + 7);
	}

} // namespace attrib
