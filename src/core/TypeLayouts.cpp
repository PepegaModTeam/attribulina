#include "TypeLayouts.h"

#include "Schema.h"

#include <algorithm>
#include <unordered_map>

namespace attrib
{

	namespace
	{
		using L = PointerLayout;
		using N = NestedField;
		constexpr auto U32 = CountSource::U32;
		constexpr auto U8 = CountSource::U8;
		constexpr auto One = CountSource::One;
		constexpr auto PairSum = CountSource::PairSum;

		// Each layout with the size of the type it was verified on (PC).
		struct Sized
		{
			uint32_t size;
			PointerLayout layout;
		};

		const std::unordered_map<std::string_view, Sized>& Table()
		{
			static const std::unordered_map<std::string_view, Sized> table = {
				{"EA::Reflection::Text", {4, L{{0}, {}}}},
				{"Attrib::StringKey", {8, L{{4}, {}}}},
				{"ControllerDataRecord", {20, L{{4}, {}}}},
				{"CCarKitSlotEntry", {16, L{{12}, {}}}},
				{"TargetTimeOverrides", {28, L{{12}, {}}}},
				{"SlotStats", {24, L{{0}, {}}}},    // FE_ATTRIB frontend/slot_stats
				{"FEPartCamera", {16, L{{0}, {}}}}, // FE_ATTRIB frontend/paint_camera
				// Curve { xMin, xMax, yMin, yMax; VariableArray X, Y, Y2; bool }
				{"Curve", {44, L{{}, {N{16, "f32", 4, 4, U32, 20}, N{24, "f32", 4, 4, U32, 28}, N{32, "f32", 4, 4, U32, 36}}}}},
				{"VariableArray", {8, L{{}, {N{0, "f32", 4, 4, U32, 4}}}}},
				{"DynamicSizeArray", {8, L{{}, {N{0, "Attrib::RefSpec", 12, 4, U32, 4}}}}},
				{"CCarSlotEntry", {12, L{{8}, {N{0, "Attrib::RefSpec", 12, 4, U32, 4}}}}},
				// Three MiniRefSpec arrays (counts in bytes 28..30) that may share one
				// block, an FEPartDetail, and a name. (PS2's has 4-byte bools: 68 bytes.)
				{"FEPartData", {56, L{{52},
									  {N{32, "MiniRefSpec", 4, 4, U8, 28}, N{36, "MiniRefSpec", 4, 4, U8, 29},
									   N{40, "MiniRefSpec", 4, 4, U8, 30}, N{48, "FEPartDetail", 8, 4, One, 0}}}}},
									   // Attrib::Blob { u32 mSize; void* mData }: compiled Lua lives in these.
									   {"Attrib::Blob", {8, L{{}, {N{4, "u8", 1, 1, U32, 0, true}}}}},
									   // A static index table: mCount keys, mCount ISets { index; count } and
									   // the values they index (current-gen Undercover; kUnlistedTypes).
									   {kIndexTableType, {16, L{{}, {N{4, "u32", 4, 4, U32, 0}, N{8, "ISet", 8, 4, U32, 0}, N{12, "u32", 4, 4, PairSum, 8}}}}},
			};
			return table;
		}
	} // namespace

	const PointerLayout* PointerLayoutFor(std::string_view typeName, uint32_t size)
	{
		const auto& t = Table();
		auto it = t.find(typeName);
		return it == t.end() || it->second.size != size ? nullptr : &it->second.layout;
	}

	uint32_t NestedElementCount(const NestedField& f, const uint8_t* elem, Endian e)
	{
		switch (f.count)
		{
			case CountSource::U32: return load32(elem + f.countOffset, e);
			case CountSource::U16: return load16(elem + f.countOffset, e);
			case CountSource::U8: return elem[f.countOffset];
			case CountSource::One: return 1;
			case CountSource::PairSum: return 0;
		}
		return 0;
	}

} // namespace attrib

namespace attrib
{

	namespace
	{
		void Derive(const StructDef& st, const TypeSchema& schema, uint32_t base, PointerLayout& out, int depth)
		{
			if (depth > 16) return;
			for (const MemberDef& m : st.members)
			{
				for (uint32_t k = 0; k < m.count; ++k)
				{
					const uint32_t off = base + m.offset + k * m.elemSize;
					if (m.prim == MemberPrim::CStr)
					{
						out.strings.push_back(off);
					}
					else if (m.prim == MemberPrim::Ptr && (m.ptrTarget.empty() || m.ptrTarget == "void"))
					{
						out.unfollowed.push_back(off);
					}
					else if (m.prim == MemberPrim::Ptr)
					{
						// count= names the sibling holding the element count; without
						// it the pointer leads to exactly one element.
						const MemberDef* cnt = m.ptrCount.empty() ? nullptr : st.member(m.ptrCount);
						if (!m.ptrCount.empty() && !cnt)
						{
							out.unfollowed.push_back(off);
							continue;
						}
						NestedField n;
						n.offset = off;
						n.elemType = m.ptrTarget;
						n.elemSize = 0;
						n.align = 4;
						n.countOffset = 0;
						n.count = CountSource::One;
						for (MemberPrim p : {MemberPrim::Bool, MemberPrim::Int8, MemberPrim::UInt8, MemberPrim::Int16, MemberPrim::UInt16,
							 MemberPrim::Int32, MemberPrim::UInt32, MemberPrim::Int64, MemberPrim::UInt64,
							 MemberPrim::Float, MemberPrim::Double, MemberPrim::Char, MemberPrim::Bool32})
							if (m.ptrTarget == PrimName(p)) n.elemSize = PrimSize(p);
						if (n.elemSize == 1) n.align = 1;
						if (const StructDef* t = schema.findStruct(m.ptrTarget))
						{
							n.elemSize = t->size;
							n.align = std::max<uint32_t>(4, t->align);
						}
						else if (const EnumDef* e = schema.findEnum(m.ptrTarget))
						{
							n.elemSize = e->size;
						}
						if (!n.elemSize)
						{
							out.unfollowed.push_back(off);
							continue;
						}
						if (cnt)
						{
							n.countOffset = base + cnt->offset; // the count lives in the same struct instance
							n.count = cnt->elemSize == 1 ? CountSource::U8 : cnt->elemSize == 2 ? CountSource::U16 : CountSource::U32;
						}
						out.nested.push_back(std::move(n));
					}
					else if (m.st)
					{
						Derive(*m.st, schema, off, out, depth + 1);
					}
				}
			}
		}
	} // namespace

	PointerLayout DerivePointerLayout(const StructDef& st, const TypeSchema& schema)
	{
		PointerLayout out;
		Derive(st, schema, 0, out, 0);
		return out;
	}

} // namespace attrib
