#pragma once

// Type-aware editors for attribute values. Every editor works on bytes at an
// offset inside a DataBlock (an attribute's data or a nested block), so the
// same code edits a top-level value, a struct member or an element of an
// array a struct points to. Compound values (structs, arrays) are expanded
// into rows by the inspector (App::drawValueRows); the editors here draw one row.

#include "core/Database.h"
#include "core/Gameplay.h"
#include "core/Paint.h"
#include "core/Values.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace app
{

	// Every collection, labelled "class/collection" and sorted, for pickers and search.
	struct CollectionIndex
	{
		struct Entry
		{
			std::string label;       // "class/collection"
			std::string lowerLabel;  // for case-insensitive filtering
			attrib::Collection* collection;
		};
		std::vector<Entry> entries;
		std::vector<attrib::ClassDef*> classes; // sorted by name
		std::unordered_map<const attrib::ClassDef*, std::vector<attrib::Collection*>> byClass; // sorted by name
		uint64_t version = ~0ull;

		void rebuild(const attrib::Database& db, uint64_t modelVersion);
	};

	// How a 4-byte integer holds a colour, as a value (so either byte order reads
	// the same). The emitters' Color1..4 are 0xRRGGBBAA (GetFloatColor in Pro
	// Street's 360 milestone and Carbon's PS3 build takes red from the top byte);
	// gameplay's CrewColor and Undercover's CarPaintSwatch rgb are 0xAARRGGBB,
	// the front end's order.
	enum class PackedColour : uint8_t
	{
		None,
		RGBA, // 0xRRGGBBAA
		ARGB, // 0xAARRGGBB
	};

	// Per-name choices the user made about how to show a value.
	struct DisplayPrefs
	{
		// Field or member name -> show as a colour (true) or as numbers (false).
		std::unordered_map<std::string, bool> colour;
		// Field or member name -> the order a 4-byte integer colour is read in,
		// when the user picked one.
		std::unordered_map<std::string, PackedColour> packing;
		// Field name -> show a Matrix as a curve's graph (true) or as rows (false).
		std::unordered_map<std::string, bool> graph;
	};

	struct EditEnv
	{
		attrib::Database& db;
		attrib::Endian endian = attrib::Endian::Little;
		const CollectionIndex* index = nullptr;
		// Called by a reference's "Go" button; newTab for a middle-click.
		std::function<void(attrib::Collection*, bool newTab)> navigate;
		// Called with every name the user types (keys, string keys) so it can be
		// remembered; without it names only go into the database's dictionary.
		std::function<void(const std::string&)> rememberName;
		DisplayPrefs* prefs = nullptr;
		// Set for gameplay collections in the gameplay view: GCollectionKey and
		// path Text fields become references to elements.
		const attrib::GameplayModel* gameplay = nullptr;
		// Set for message handlers: opens their Lua in the State Graph Builder.
		std::function<void()> openScript = nullptr;
		// Current-gen Undercover's car_paint_swatch/swatch colours, which its
		// PresetRidePaint picker snaps to; without them (none, or empty) its
		// paints show as members only.
		const attrib::PaintSwatches* swatches = nullptr;
		// Called with each keystroke of a name being typed (a StringKey's text),
		// with the text before it; set, it remembers the name instead once the
		// box is left (App::commitTypedName), unless it holds the text it started with.
		std::function<void(const std::string& before, const std::string& now)> typingName = nullptr;
		// The class of the collection being edited (0 if none): how the game reads
		// a Matrix curve goes by class and field (MatrixCurveOf).
		uint32_t classKey = 0;
	};

	using attrib::ExpandsIntoMembers;
	using attrib::ReadInteger;
	using attrib::TypeLabel;
	using attrib::TypeOfAttribute;
	using attrib::TypeOfMember;
	using attrib::TypeOfNamed;
	using attrib::ValueType;
	using attrib::WriteInteger;

	// Shown as a colour picker: the user's choice for this name, else the hint,
	// else a colour-sounding name on a vector type, a 4-byte integer
	// (IntegerColour) or four bytes (BytesColour).
	bool ShowsAsColour(const EditEnv& env, const ValueType& vt);
	// The colour a 4-byte integer holds, None when it shows as a number: the
	// user's choice for this name, else by name (Color1, ColourStart, FogColour,
	// LensFlareCol: RGBA; CrewColor, rgb: ARGB).
	PackedColour IntegerColour(const EditEnv& env, const ValueType& vt);
	// Four u8 elements shown as one colour, R, G, B, A (Undercover's frontend
	// damageColor0, flashColor): a colour-sounding name, or the user's choice.
	bool BytesColour(const EditEnv& env, const ValueType& element, uint32_t count);
	// A Matrix field's value shown as a curve's graph (Curve.h, MatrixCurveOf):
	// the user's choice for this name, else every Matrix that isn't a struct's
	// member (all of the supported games' Matrix fields hold curves).
	bool ShowsAsGraph(const EditEnv& env, const ValueType& vt);
	// The picker for four bytes R, G, B, A at `p`, with the menu that switches
	// the name between colour and numbers.
	bool EditBytesColour(EditEnv& env, uint8_t* p, const ValueType& element);

	// Draws the one-row editor for the value at `offset` in `b`. Returns true
	// when it changed. `editable` false draws it disabled (inherited values).
	bool EditValue(EditEnv& env, attrib::DataBlock& b, uint32_t offset, const ValueType& vt, bool editable = true);

	// One-line text for a value, for previews and summaries.
	std::string FormatValue(const attrib::Database& db, const attrib::DataBlock& b, uint32_t offset, const ValueType& vt,
							attrib::Endian e);

	std::string ToLower(std::string s);

} // namespace app
