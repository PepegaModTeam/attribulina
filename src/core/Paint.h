#pragma once

// Pro Street car paint (`PresetRidePaint`, used by car.stockPaint and
// presetride.Paints) as a colour. The front end's paint chooser has 90 hue
// steps; a paint stores its step as a hue swatch part (car_part/
// generic_paint_swatch0..5, 16 steps each) plus a kit number:
//   step       = swatch * 16 + kit % 16
//   hue        = 1 - step / 90          (0..1, wrapping; step 0 is red)
//   saturation = mSaturation, value = mVariance (both 0..1)
// The finish (gloss, metallic, matte, ...) is the mGroup part. The mapping is
// the game's FECustomizePaint chooser, as read by color-preview/dllmain.cpp.
//
// PresetRidePaint (40 bytes): +0 ePaintSlot mSlotID, +4 RefSpec mGroup,
// +16 RefSpec mSwatch, +28 u32 mKitNumber, +32 float mSaturation,
// +36 float mVariance.

//
// Current-gen Undercover (PC, 360) stores the hue as an index instead:
// PresetRidePaint (32 bytes): +0 ePaintSlot mSlotID, +4 RefSpec mGroup,
// +16 u8 mSwatch, +20 float mSaturation, +24 float mVariance,
// +28 ePartMaterialFlags mMaterialFlags. mSwatch indexes the `swatch` array of
// car_paint_swatch/swatch (CarPaintSwatch, 20 bytes: +0 u32 rgb, 0xAARRGGBB as
// a value, then the two materials it blends and their blend); retail has 70,
// fully saturated hues from red round to red. The game (CarPaint::
// SetupPaintColor, CreateCustomPaintLightMaterial; the 360 debug build's
// 0x82D7E3C8, 0x82D7DE90) blends the swatch's materials, mixes white in by
// 1 - mSaturation and black by 1 - mVariance (both clamped to 0..1): HSV with
// the swatch's hue, S = mSaturation, V = mVariance. An index past the array is
// swatch 0 ("WARNING: invalid swatch index"). The rgb is what the front end
// shows; the car's colour comes from the materials.

#include "Bytes.h"

#include <string>
#include <vector>

namespace attrib
{

	class Database;
	class KeyNames;
	struct DataBlock;
	struct StructDef;

	constexpr int kPaintHueSteps = 90;
	constexpr int kPaintStepsPerSwatch = 16;

	struct PaintColour
	{
		float h = 0, s = 0, v = 0; // HSV, 0..1
		int step = 0;              // hue step in the chooser
		bool hueKnown = false;     // false when mSwatch isn't one of generic_paint_swatch0..5
	};

	// The swatch number (0..5) of a generic_paint_swatchN key, or -1.
	int PaintSwatchIndex(uint32_t collectionKey);
	uint32_t PaintSwatchKey(int index);

	PaintColour ReadPaint(const DataBlock& b, uint32_t offset, Endian e);
	// Writes a colour back: the hue snaps to the nearest of the 90 steps and sets
	// mSwatch (keeping its class, car_part when unset) and mKitNumber.
	void WritePaint(DataBlock& b, uint32_t offset, float h, float s, float v, Endian e);

	// Undercover's swatches by index, read from car_paint_swatch/swatch.
	struct PaintSwatches
	{
		std::vector<uint32_t> rgb; // 0xAARRGGBB
		std::vector<float> hue;    // 0..1, or -1 for a grey swatch
		bool empty() const
		{
			return rgb.empty();
		}
	};

	// Undercover's 32-byte PresetRidePaint (mSwatch an index).
	bool IsSwatchPaint(const StructDef& st);
	// The table, empty when the database has no car_paint_swatch/swatch.
	PaintSwatches ReadPaintSwatches(const Database& db);
	// step is the swatch index; hueKnown is false when it is past the table (the
	// game uses swatch 0, whose hue h then is) or the swatch is grey.
	PaintColour ReadSwatchPaint(const DataBlock& b, uint32_t offset, const PaintSwatches& sw, Endian e);
	// Sets mSaturation and mVariance, and mSwatch to the swatch nearest a hue that
	// differs from the current swatch's.
	void WriteSwatchPaint(DataBlock& b, uint32_t offset, float h, float s, float v, const PaintSwatches& sw, Endian e);

	// "gloss", "metallic", ... from the finish part's name
	// (generic_body_paint_group_gloss), or the name itself when it doesn't follow
	// that pattern; empty when unset.
	std::string PaintFinish(const KeyNames& names, uint32_t groupKey);

} // namespace attrib
