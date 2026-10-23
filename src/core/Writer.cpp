#include "Writer.h"

#include "Hash.h"
#include "TypeLayouts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace attrib
{

	namespace
	{

		constexpr uint32_t kTypeDatabase = ExportType::DatabaseLoadData;
		constexpr uint32_t kTypeClass = ExportType::ClassLoadData;
		constexpr uint32_t kTypeCollection = ExportType::CollectionLoadData;

		// A pointer to emit in PtrN.
		struct Fixup
		{
			int group;                        // 1: located in the bin, 0: in the vlt
			uint32_t loc;
			uint32_t target = 0;              // resolved bin offset
			const DataBlock* block = nullptr; // target block, if not yet placed
			PtrSeq seq;
			bool null = false;                // Most Wanted's layout: a null pointer, listed (type 1)
		};

		// What Most Wanted's layout fills a vlt pointer with before it is fixed up.
		constexpr uint32_t kUnfixed = 0xEFFECADDu;

		// A block to place in the bin's data area.
		struct OutBlock
		{
			const DataBlock* identity = nullptr; // nested blocks are placed once
			Bytes bytes;
			std::vector<std::pair<uint32_t, const PtrSlot*>> ptrs; // offset in `bytes`
			uint32_t align = 4;
			uint32_t* placedAt = nullptr;        // receives the bin offset
			// Most Wanted's layout: the null pointers to list, and the nested
			// pointers' fields (for the null pointers in what they lead to).
			std::vector<std::pair<uint32_t, PtrSeq>> nulls;
			std::unordered_map<uint32_t, const NestedField*> nested;
		};

		// Every pointer of an element: its strings, nested blocks and the rest.
		std::vector<uint32_t> PointerOffsets(const PointerLayout& pl)
		{
			std::vector<uint32_t> out = pl.strings;
			for (const NestedField& n : pl.nested) out.push_back(n.offset);
			out.insert(out.end(), pl.unfollowed.begin(), pl.unfollowed.end());
			return out;
		}

		uint32_t Align(uint32_t v, uint32_t a)
		{
			return uint32_t(AlignUp(v, a ? a : 1));
		}

		uint32_t AlignOfField(const FieldDef* f)
		{
			return f ? 1u << f->alignment : 4u;
		}

		// Type list for a collection: the stored list (with its extras) plus anything
		// missing, in the derived order: layout field types by offset, then entry types.
		std::vector<uint32_t> TypeList(const Collection& c)
		{
			std::vector<uint32_t> derived;
			auto add = [](std::vector<uint32_t>& v, uint32_t h)
			{
				if (std::find(v.begin(), v.end(), h) == v.end()) v.push_back(h);
			};
			if (c.cls)
			{
				std::vector<const FieldDef*> lf;
				for (const auto& f : c.cls->fields)
					if (f.inLayout()) lf.push_back(&f);
				std::stable_sort(lf.begin(), lf.end(), [](const FieldDef* a, const FieldDef* b)
				{
					return a->offset < b->offset;
				});
				for (const FieldDef* f : lf) add(derived, f->typeHash);
			}
			for (const Attribute& a : c.attributes)
				if (!a.inLayout && a.type) add(derived, a.type->hash);
			std::vector<uint32_t> out = c.typeHashes;
			for (uint32_t h : derived) add(out, h);
			return out;
		}

		class VaultWriter
		{
		public:
			VaultWriter(const Database& db, const Vault& v) : db_(db), v_(v), e_(v.raw.endian), bin_(e_), vlt_(e_)
			{
			}

			VaultBlobs write()
			{
				plan();
				writeStrings();
				writeData();
				writeVlt();
				return { std::move(bin_.bytes()), std::move(vlt_.bytes()) };
			}

		private:
			struct ExportPlan
			{
				const ExportRef* ref;
				uint64_t id = 0;
				uint32_t type = 0;
				std::vector<uint32_t> types;   // collections
				// Bin positions of the top-level blocks, filled when placed.
				uint32_t typenamesPos = 0, defsPos = 0, staticPos = 0, layoutPos = 0;
				std::vector<uint32_t> entryPos; // per non-layout attribute (UINT32_MAX: inline)
				std::vector<OutBlock> top;
			};

			// ---- planning: the top-level blocks of every export -------------------

			void plan()
			{
				for (const ExportRef& r : v_.exportOrder)
				{
					ExportPlan p;
					p.ref = &r;
					switch (r.kind)
					{
						case ExportRef::Kind::Database: planDatabase(p); break;
						case ExportRef::Kind::Class: planClass(p, *r.cls); break;
						case ExportRef::Kind::Collection: planCollection(p, *r.collection); break;
						case ExportRef::Kind::Data:
							p.id = v_.dataExports.at(r.data).id;
							p.type = v_.dataExports.at(r.data).type;
							break;
					}
					plans_.push_back(std::move(p));
				}
				// Fix up placedAt pointers now that plans_ won't reallocate.
				for (auto& p : plans_)
				{
					size_t i = 0;
					if (p.ref->kind == ExportRef::Kind::Database)
					{
						p.top[i++].placedAt = &p.typenamesPos;
					}
					else if (p.ref->kind == ExportRef::Kind::Class)
					{
						const ClassDef& c = *p.ref->cls;
						if (!c.fields.empty()) p.top[i++].placedAt = &p.defsPos;
						if (c.staticSize) p.top[i++].placedAt = &p.staticPos;
					}
					else if (p.ref->kind == ExportRef::Kind::Collection)
					{
						const Collection& c = *p.ref->collection;
						if (c.cls && c.cls->layoutSize) p.top[i++].placedAt = &p.layoutPos;
						size_t k = 0;
						for (const Attribute& a : c.attributes)
						{
							if (a.inLayout) continue;
							if (!(a.nodeFlags & NodeFlag::ByValue)) p.top[i++].placedAt = &p.entryPos[k];
							++k;
						}
					}
				}
			}

			// A key as the vault stores it: StringHash64 in the early Most Wanted demos.
			uint64_t fileKey(uint32_t key) const
			{
				return k64_ ? db_.keyTo64(key) : key;
			}
			uint64_t fileType(const TypeInfo* t) const
			{
				return !t ? 0 : k64_ ? (t->hash64 ? t->hash64 : StringHash64(t->name)) : t->hash;
			}

			void planDatabase(ExportPlan& p)
			{
				p.id = v_.databaseExportId ? v_.databaseExportId : k64_ ? StringHash64("Attrib::Database") : StringHash32("Attrib::Database");
				p.type = kTypeDatabase;
				OutBlock b;
				b.align = 1;
				for (const auto& t : db_.types())
				{
					b.bytes.insert(b.bytes.end(), t.name.begin(), t.name.end());
					b.bytes.push_back(0);
				}
				p.top.push_back(std::move(b));
			}

			void planClass(ExportPlan& p, const ClassDef& c)
			{
				p.id = fileKey(c.key);
				p.type = kTypeClass;
				if (!c.fields.empty())
				{
					// Sorted by the key as stored; 24 bytes with u64 key and type in the demos.
					std::vector<std::pair<uint64_t, const FieldDef*>> defs;
					for (const auto& f : c.fields) defs.emplace_back(fileKey(f.key), &f);
					std::stable_sort(defs.begin(), defs.end(), [](auto& a, auto& b)
					{
						return a.first < b.first;
					});
					const uint32_t size = k64_ ? 24 : 16, at = k64_ ? 16 : 8;
					OutBlock b;
					b.align = 8;
					b.bytes.resize(defs.size() * size);
					for (size_t i = 0; i < defs.size(); ++i)
					{
						uint8_t* d = b.bytes.data() + i * size;
						const FieldDef& f = *defs[i].second;
						if (k64_)
						{
							store64(d, defs[i].first, e_);
							store64(d + 8, fileType(f.type), e_);
						}
						else
						{
							store32(d, f.key, e_);
							store32(d + 4, f.typeHash, e_);
						}
						store16(d + at, f.offset, e_);
						store16(d + at + 2, f.size, e_);
						store16(d + at + 4, f.maxCount, e_);
						d[at + 6] = f.flags;
						d[at + 7] = f.alignment;
					}
					p.top.push_back(std::move(b));
				}
				if (c.staticSize)
				{
					OutBlock b;
					uint32_t al = 0;
					for (const auto& f : c.fields)
						if (f.isStatic()) al = std::max<uint32_t>(al, 1u << f.alignment);
					b.align = al ? al : 4;
					b.bytes = c.staticData.bytes;
					b.bytes.resize(c.staticSize, 0);
					for (const auto& s : c.staticData.ptrs) b.ptrs.emplace_back(s.offset, &s);
					for (const auto& f : c.fields)
						if (f.isStatic() && !f.isArray()) addNulls(b, c.staticData, f.offset, f.offset, f.type, f.size, 1);
					p.top.push_back(std::move(b));
				}
			}

			void planCollection(ExportPlan& p, const Collection& c)
			{
				p.id = c.exportId;
				p.type = kTypeCollection;
				p.types = TypeList(c);
				const ClassDef* cls = c.cls;
				if (cls && cls->layoutSize) p.top.push_back(layoutBlock(c));
				for (const Attribute& a : c.attributes)
				{
					if (a.inLayout) continue;
					if (a.nodeFlags & NodeFlag::ByValue)
					{
						p.entryPos.push_back(UINT32_MAX);
						continue;
					}
					p.entryPos.push_back(0);
					OutBlock b;
					if (a.isArray)
					{
						const uint16_t alloc = uint16_t(std::max<uint32_t>(a.capacity, a.count));
						const uint32_t hdr = ArrayHeaderSize(a.encodedTypePad);
						b.align = std::max(2u, AlignOfField(a.def));
						b.bytes.assign(hdr + size_t(alloc) * a.elemSize, 0);
						store16(b.bytes.data(), alloc, e_);
						store16(b.bytes.data() + 2, uint16_t(a.count), e_);
						store16(b.bytes.data() + 4, uint16_t(a.elemSize), e_);
						store16(b.bytes.data() + 6, a.encodedTypePad, e_);
						std::copy(a.data.bytes.begin(), a.data.bytes.end(), b.bytes.begin() + hdr);
						for (const auto& s : a.data.ptrs) b.ptrs.emplace_back(hdr + s.offset, &s);
						addNulls(b, a.data, hdr, 0, a.type, a.elemSize, a.count);
					}
					else
					{
						b.align = AlignOfField(a.def);
						b.bytes = a.data.bytes;
						for (const auto& s : a.data.ptrs) b.ptrs.emplace_back(s.offset, &s);
						addNulls(b, a.data, 0, 0, a.type, a.elemSize, a.count);
					}
					p.top.push_back(std::move(b));
				}
			}

			// The layout block: the raw layout with every layout attribute laid over
			// it, cut at the end of the last field.
			OutBlock layoutBlock(const Collection& c)
			{
				const ClassDef& cls = *c.cls;
				OutBlock b;
				b.bytes = c.layout.bytes;
				b.bytes.resize(LayoutBytes(cls), 0);
				uint32_t end = 0, al = 1;
				std::vector<std::pair<uint32_t, uint32_t>> covered; // [begin, end) owned by attributes
				for (const auto& f : cls.fields)
				{
					if (!f.inLayout()) continue;
					al = std::max<uint32_t>(al, 1u << f.alignment);
					const Attribute* a = c.find(f.key);
					if (f.isArray())
					{
						const uint16_t capacity = a ? a->capacity : load16(b.bytes.data() + f.offset, e_);
						end = std::max(end, f.offset + LayoutExtent(f, capacity));
						if (!a) continue;
						uint8_t* h = b.bytes.data() + f.offset;
						store16(h, a->capacity, e_);
						store16(h + 2, uint16_t(a->count), e_);
						store16(h + 4, a->capacity ? uint16_t(f.size) : load16(h + 4, e_), e_);
						store16(h + 6, a->encodedTypePad, e_);
						const uint32_t first = f.offset + LayoutArrayHeader(f);
						std::copy(a->data.bytes.begin(), a->data.bytes.end(), b.bytes.begin() + first);
						covered.emplace_back(first, first + uint32_t(a->data.bytes.size()));
						for (const auto& s : a->data.ptrs) b.ptrs.emplace_back(first + s.offset, &s);
						addNulls(b, a->data, first, 0, f.type, f.size, a->count);
					}
					else
					{
						const uint32_t at = LayoutOffset(f);
						end = std::max(end, at + f.size);
						if (!a) continue;
						std::copy(a->data.bytes.begin(), a->data.bytes.end(), b.bytes.begin() + at);
						covered.emplace_back(at, at + f.size);
						for (const auto& s : a->data.ptrs) b.ptrs.emplace_back(at + s.offset, &s);
						addNulls(b, a->data, at, 0, f.type, f.size, 1);
					}
				}
				// Pointers in unused array capacity stay as loaded.
				for (const auto& s : c.layout.ptrs)
				{
					bool owned = false;
					for (auto& [lo, hi] : covered) owned |= s.offset >= lo && s.offset < hi;
					if (!owned && s.offset + 4 <= end) b.ptrs.emplace_back(s.offset, &s);
				}
				b.bytes.resize(end);
				b.align = al;
				std::sort(b.ptrs.begin(), b.ptrs.end(), [](auto& x, auto& y)
				{
					return x.first < y.first;
				});
				return b;
			}

			// Most Wanted's layout lists every pointer, null ones too: those of
			// `count` elements of `type` at `at` in `b`, which come from `src` at
			// `srcAt` (whose hints say where each stood in PtrN).
			void addNulls(OutBlock& b, const DataBlock& src, uint32_t at, uint32_t srcAt, const TypeInfo* type, uint32_t elemSize,
						  uint32_t count)
			{
				if (type) addNulls(b, src, at, srcAt, type->pointers(), elemSize, count);
			}
			void addNulls(OutBlock& b, const DataBlock& src, uint32_t at, uint32_t srcAt, const PointerLayout* pl, uint32_t elemSize,
						  uint32_t count)
			{
				if (!v_.raw.mostWantedLayout || !pl) return;
				const std::vector<uint32_t> offsets = PointerOffsets(*pl);
				for (uint32_t k = 0; k < count; ++k)
				{
					for (const NestedField& n : pl->nested) b.nested[at + k * elemSize + n.offset] = &n;
					for (uint32_t off : offsets)
					{
						const uint32_t rel = srcAt + k * elemSize + off;
						if (src.ptrAt(rel)) continue;
						PtrSeq seq = kNewPtr;
						for (const auto& n : src.nulls)
							if (n.offset == rel) seq = n.seq;
						b.nulls.emplace_back(at + k * elemSize + off, seq);
					}
				}
			}

			// The pointers of a nested block's elements.
			const PointerLayout* elementPointers(const NestedField& n) const
			{
				if (const PointerLayout* pl = PointerLayoutFor(n.elemType, n.elemSize)) return pl;
				const TypeInfo* t = db_.typeByHash(StringHash32(n.elemType));
				return t && t->size == n.elemSize ? t->pointers() : nullptr;
			}

			// ---- bin -------------------------------------------------------------

			void collectStrings(const DataBlock& b, std::unordered_set<const DataBlock*>& seen, std::vector<std::string>& out)
			{
				for (const auto& s : b.ptrs)
				{
					if (s.isString) out.push_back(s.text);
					else if (s.data && seen.insert(s.data.get()).second) collectStrings(*s.data, seen, out);
				}
			}

			void writeStrings()
			{
				// Every string the model references, in walk order.
				std::vector<std::string> used;
				std::unordered_set<const DataBlock*> seen;
				for (auto& p : plans_)
				{
					if (p.ref->kind == ExportRef::Kind::Collection)
						for (const Attribute& a : p.ref->collection->attributes)
							if (!a.inLayout && (a.nodeFlags & NodeFlag::ByValue))
								for (const auto& s : a.data.ptrs)
									if (s.isString) used.push_back(s.text);
					for (const OutBlock& b : p.top)
						for (auto& [off, s] : b.ptrs)
						{
							if (s->isString) used.push_back(s->text);
							else if (s->data && seen.insert(s->data.get()).second) collectStrings(*s->data, seen, used);
						}
				}
				std::unordered_set<std::string> usedSet(used.begin(), used.end());

				bin_.u32(FourCC("StrE"));
				bin_.u32(0);
				auto emit = [&](const std::string& s)
				{
					if (strings_.count(s)) return;
					strings_[s] = uint32_t(bin_.size());
					bin_.cstr(s);
				};
				for (const std::string& s : v_.stringOrder)
					if (usedSet.count(s)) emit(s);
				for (const std::string& s : used) emit(s);
				bin_.align(16);
				bin_.put32(4, uint32_t(bin_.size()));
				if (v_.raw.endC)
				{
					bin_.u32(FourCC("EndC"));
					bin_.u32(16);
					bin_.fill(8);
				}
			}

			void place(OutBlock& b)
			{
				const uint32_t pos = Align(uint32_t(bin_.size()), b.align);
				bin_.fill(pos - bin_.size());
				bin_.raw(b.bytes);
				if (b.placedAt) *b.placedAt = pos;
				if (b.identity) blockPos_[b.identity] = pos;
				for (auto& [off, s] : b.ptrs)
				{
					Fixup f{ 1, pos + off, 0, nullptr, s->seq };
					if (s->isString) f.target = strings_.at(s->text);
					else if (s->data) f.block = s->data.get();
					else continue;
					fixups_.push_back(f);
				}
				for (auto& [off, seq] : b.nulls) fixups_.push_back(Fixup{ 1, pos + off, 0, nullptr, seq, true });
			}

			void writeData()
			{
				// Most Wanted's own vaults put what a block points at before the block
				// (a blob's compiled Lua, then the Blob); the others after, breadth first.
				const bool childrenFirst = v_.raw.mostWantedOwn();
				for (auto& p : plans_)
				{
					std::deque<OutBlock> nestedStore;
					// The blocks `b` points at that aren't placed yet, in pointer order.
					auto children = [&](OutBlock& b)
					{
						std::vector<OutBlock*> out;
						std::vector<std::pair<uint32_t, const PtrSlot*>> ptrs = b.ptrs;
						std::stable_sort(ptrs.begin(), ptrs.end(), [](auto& x, auto& y)
						{
							return x.first < y.first;
						});
						for (auto& [off, s] : ptrs)
						{
							if (!s->data || blockPos_.count(s->data.get())) continue;
							OutBlock n;
							n.identity = s->data.get();
							n.bytes = s->data->bytes;
							n.align = s->data->align;
							for (const auto& ns : s->data->ptrs) n.ptrs.emplace_back(ns.offset, &ns);
							if (auto it = b.nested.find(off); it != b.nested.end() && it->second->elemSize)
								addNulls(n, *s->data, 0, 0, elementPointers(*it->second), it->second->elemSize,
										 uint32_t(n.bytes.size() / it->second->elemSize));
							nestedStore.push_back(std::move(n));
							out.push_back(&nestedStore.back());
						}
						return out;
					};
					if (childrenFirst)
					{
						std::function<void(OutBlock&)> tree = [&](OutBlock& b)
						{
							if (b.identity && blockPos_.count(b.identity)) return;
							for (OutBlock* n : children(b)) tree(*n);
							place(b);
						};
						for (OutBlock& b : p.top) tree(b);
						continue;
					}
					// The export's blocks, then what they point at.
					std::deque<OutBlock*> queue;
					for (OutBlock& b : p.top) queue.push_back(&b);
					while (!queue.empty())
					{
						OutBlock* b = queue.front();
						queue.pop_front();
						if (b->identity && blockPos_.count(b->identity)) continue;
						place(*b);
						for (OutBlock* n : children(*b)) queue.push_back(n);
					}
				}
				for (Fixup& f : fixups_)
					if (f.block) f.target = blockPos_.at(f.block);
			}

			// ---- vlt -------------------------------------------------------------

			size_t beginChunk(const char(&id)[5])
			{
				const size_t start = vlt_.size();
				vlt_.u32(FourCC(id));
				vlt_.u32(0);
				return start;
			}
			void endChunk(size_t start)
			{
				vlt_.align(16);
				vlt_.put32(start + 4, uint32_t(vlt_.size() - start));
			}

			void exportBytes(ExportPlan& p)
			{
				const uint32_t base = uint32_t(vlt_.size());
				const bool mw = v_.raw.mostWantedLayout;
				auto vfix = [&](uint32_t rel, uint32_t target, PtrSeq seq)
				{
					fixups_.push_back(Fixup{ 0, base + rel, target, nullptr, seq });
				};
				// Most Wanted's layout lists a null pointer too.
				auto vnull = [&](uint32_t rel, PtrSeq seq)
				{
					if (mw) fixups_.push_back(Fixup{ 0, base + rel, 0, nullptr, seq, true });
				};
				// A pointer slot, filled in by its fixup.
				auto slot = [&]()
				{
					vlt_.u32(mw ? kUnfixed : 0);
				};
				// The early Most Wanted demos' keys are u64, and the fields after
				// them move by `x`.
				const uint32_t x = k64_ ? 4 : 0;
				auto key = [&](uint32_t k)
				{
					if (k64_) vlt_.u64(db_.keyTo64(k));
					else vlt_.u32(k);
				};
				switch (p.ref->kind)
				{
					case ExportRef::Kind::Data:
						vlt_.raw(v_.dataExports.at(p.ref->data).bytes);
						break;
					case ExportRef::Kind::Database:
					{
						vlt_.u32(db_.numClassesDeclared());
						if (k64_) vlt_.u32(db_.databaseWord4());
						vlt_.u32(db_.defaultDataSize());
						vlt_.u32(uint32_t(db_.types().size()));
						slot();
						for (const auto& t : db_.types()) vlt_.u32(t.size);
						vfix(12 + x, p.typenamesPos, v_.typenamesSeq);
						break;
					}
					case ExportRef::Kind::Class:
					{
						const ClassDef& c = *p.ref->cls;
						// As read: the layout fields that aren't NotSearchable up to
						// Undercover, a number nothing here derives in World (61 in
						// caraudio, which lays out 32). Nothing edits a class's fields.
						const uint16_t layoutCount = c.layoutCount;
						key(c.key);
						vlt_.u32(c.collectionReserve);
						vlt_.u32(uint32_t(c.fields.size()));
						slot();
						// Most Wanted's own vaults have no static data in a class.
						const bool noStatic = v_.raw.mostWantedOwn();
						if (mw)
						{
							// { ..., layoutSize, u32 keyShift, u32 count, staticSize, static* }
							vlt_.u32(c.layoutSize);
							vlt_.u32(c.layoutKeyShift);
							vlt_.u32(layoutCount);
							if (!noStatic)
							{
								vlt_.u32(c.staticSize);
								slot();
							}
						}
						else
						{
							vlt_.u32(c.staticSize);
							vlt_.u32(0);
							vlt_.u32(c.layoutSize);
							vlt_.u16(c.layoutKeyShift);
							vlt_.u16(layoutCount);
						}
						const uint32_t staticAt = mw ? 32 : 20;
						if (!c.fields.empty()) vfix(12 + x, p.defsPos, c.defsSeq);
						else vnull(12 + x, c.defsSeq);
						if (c.staticSize) vfix(staticAt, p.staticPos, c.staticSeq);
						else if (!noStatic) vnull(staticAt, c.staticSeq);
						break;
					}
					case ExportRef::Kind::Collection:
					{
						const Collection& c = *p.ref->collection;
						uint32_t entries = 0;
						for (const Attribute& a : c.attributes) entries += !a.inLayout;
						const uint32_t numTypes = uint32_t(p.types.size());
						// Most Wanted's own: a u32 count and no padding.
						const bool mwOwn = v_.raw.mostWantedOwn();
						const uint32_t typesLen = mwOwn ? numTypes : (numTypes + 1) & ~1u;
						key(c.key);
						key(c.classKey);
						key(c.parentKey);
						vlt_.u32(entries);
						// The demos' tool wrote 0 (the game grows a table whose probes run long).
						vlt_.u32(k64_ && !SameEntryKeys(c) ? 0 : ChooseTableKeyShift(c));
						vlt_.u32(entries);
						if (mwOwn)
						{
							vlt_.u32(numTypes);
						}
						else
						{
							vlt_.u16(uint16_t(numTypes));
							vlt_.u16(uint16_t(typesLen));
						}
						if (c.layoutSlotStored && c.cls && c.cls->layoutSize) vlt_.u32(c.layoutSlotStored);
						else slot();
						if (k64_) vlt_.u32(0);
						for (uint32_t i = 0; i < typesLen; ++i)
						{
							const uint32_t h = i < numTypes ? p.types[i] : 0;
							if (k64_) vlt_.u64(fileType(db_.typeByHash(h)));
							else vlt_.u32(h);
						}
						const uint32_t layoutAt = k64_ ? 0x28 : 0x1C;
						if (c.cls && c.cls->layoutSize) vfix(layoutAt, p.layoutPos, c.layoutSeq);
						else vnull(layoutAt, c.layoutSeq);
						size_t k = 0;
						for (const Attribute& a : c.attributes)
						{
							if (a.inLayout) continue;
							const uint32_t rel = uint32_t(vlt_.size()) - base;
							const uint32_t typeIndex = uint32_t(std::find(p.types.begin(), p.types.end(), a.type ? a.type->hash : 0) - p.types.begin());
							key(a.key);
							const uint32_t dataAt = rel + (k64_ ? 8 : 4);
							if (a.nodeFlags & NodeFlag::ByValue)
							{
								uint8_t inline4[4] = { 0, 0, 0, 0 };
								std::memcpy(inline4, a.data.bytes.data(), std::min<size_t>(4, a.data.bytes.size()));
								vlt_.raw(inline4, 4);
								for (const auto& s : a.data.ptrs)
									if (s.offset == 0 && s.isString) vfix(dataAt, strings_.at(s.text), s.seq);
							}
							else
							{
								slot();
								vfix(dataAt, p.entryPos[k], a.dataSeq);
							}
							vlt_.u16(uint16_t(typeIndex));
							uint8_t nodeFlags = a.nodeFlags;
							if (mwOwn && (nodeFlags & NodeFlag::ByValue))
								nodeFlags = uint8_t((nodeFlags & ~NodeFlag::ByValue) | NodeFlag::MostWantedByValue);
							vlt_.u8(nodeFlags);
							vlt_.u8(a.entryFlags);
							++k;
						}
						break;
					}
				}
			}

			void writeVlt()
			{
				size_t c = 0;
				if (v_.raw.hasVers)
				{
					c = beginChunk("Vers");
					vlt_.u64(v_.raw.version);
					endChunk(c);
				}

				auto strN = [&]()
				{
					c = beginChunk("StrN");
					if (v_.raw.strN.empty()) vlt_.fill(8);
					else vlt_.raw(v_.raw.strN);
					endChunk(c);
				};
				if (v_.raw.strNFirst) strN();
				c = beginChunk("DepN");
				const auto& deps = v_.raw.dependencies;
				vlt_.u32(uint32_t(deps.size()));
				if (k64_) vlt_.u32(0);
				for (const auto& d : deps)
				{
					if (k64_) vlt_.u64(d.id);
					else vlt_.u32(uint32_t(d.id));
				}
				uint32_t off = 0;
				for (const auto& d : deps)
				{
					vlt_.u32(off);
					off += uint32_t(d.name.size()) + 1;
				}
				for (const auto& d : deps) vlt_.cstr(d.name);
				endChunk(c);
				if (!v_.raw.strNFirst) strN();

				c = beginChunk("DatN");
				std::vector<std::pair<uint32_t, uint32_t>> spans; // offset, size
				for (auto& p : plans_)
				{
					vlt_.align(8);
					const uint32_t start = uint32_t(vlt_.size());
					exportBytes(p);
					spans.emplace_back(start, uint32_t(vlt_.size()) - start);
				}
				endChunk(c);

				c = beginChunk("ExpN");
				vlt_.u32(uint32_t(plans_.size()));
				if (k64_) vlt_.u32(0);
				for (size_t i = 0; i < plans_.size(); ++i)
				{
					if (k64_)
					{
						const uint32_t t = plans_[i].type;
						vlt_.u64(plans_[i].id);
						vlt_.u64(t == kTypeDatabase ? ExportType::DatabaseLoadData64
								 : t == kTypeClass ? ExportType::ClassLoadData64
								 : ExportType::CollectionLoadData64);
					}
					else
					{
						vlt_.u32(uint32_t(plans_[i].id));
						vlt_.u32(plans_[i].type);
					}
					if (v_.raw.mostWantedLayout) vlt_.u32(0); // the name offset
					vlt_.u32(spans[i].second);
					vlt_.u32(spans[i].first);
					if (k64_) vlt_.u32(0);
				}
				endChunk(c);

				c = beginChunk("PtrN");
				for (int group : {1, 0})
				{
					std::vector<const Fixup*> g;
					for (const Fixup& f : fixups_)
						if (f.group == group) g.push_back(&f);
					// Known pointers in their original order, new ones after by location.
					std::stable_sort(g.begin(), g.end(), [](const Fixup* a, const Fixup* b)
					{
						const bool an = a->seq < 0, bn = b->seq < 0;
						if (an != bn) return bn;
						if (!an) return a->seq < b->seq;
						return a->loc < b->loc;
					});
					// The demos give a group with no pointers no selection entry.
					if (k64_ && g.empty()) continue;
					auto value = [&](uint32_t v)
					{
						if (k64_) vlt_.u64(v);
						else vlt_.u32(v);
					};
					vlt_.u32(0);
					vlt_.u16(2);
					vlt_.u16(uint16_t(group));
					value(0);
					for (const Fixup* f : g)
					{
						vlt_.u32(f->loc);
						vlt_.u16(f->null ? 1 : 3);
						vlt_.u16(f->null ? 0 : 1);
						value(f->null ? 0 : f->target);
					}
				}
				vlt_.fill(k64_ ? 16 : 12); // terminator
				endChunk(c);

				// World's event vaults: the signature, while it still signs what is
				// written (the bin, and the vlt up to it, as read). An edited vault
				// goes without, as the community Attribulator writes them: the game
				// counts a vault without one as matching (RawVault.h).
				if (!v_.raw.sign.empty() && signatureHolds())
				{
					c = beginChunk("Sign");
					vlt_.raw(v_.raw.sign);
					endChunk(c);
				}
				if (v_.raw.endC)
				{
					c = beginChunk("EndC");
					vlt_.fill(8);
					endChunk(c);
				}
			}

			bool signatureHolds() const
			{
				const RawVault& r = v_.raw;
				auto sign = std::find_if(r.vltChunks.begin(), r.vltChunks.end(), [](const RawChunk& k)
				{
					return k.id == FourCC("Sign");
				});
				if (sign == r.vltChunks.end() || bin_.bytes() != r.bin) return false;
				const Bytes& vlt = vlt_.bytes();
				return vlt.size() == sign->offset && std::equal(vlt.begin(), vlt.end(), r.vlt.begin());
			}

			const Database& db_;
			const Vault& v_;
			Endian e_;
			const bool k64_ = v_.raw.keys64;
			ByteWriter bin_, vlt_;
			std::vector<ExportPlan> plans_;
			std::unordered_map<std::string, uint32_t> strings_;
			std::unordered_map<const DataBlock*, uint32_t> blockPos_;
			std::vector<Fixup> fixups_;
		};

		uint32_t Rotl(uint32_t v, uint32_t s)
		{
			s &= 31;
			return s ? (v << s) | (v >> (32 - s)) : v;
		}

	} // namespace

	bool SameEntryKeys(const Collection& c)
	{
		std::vector<uint32_t> keys;
		for (const Attribute& a : c.attributes)
			if (!a.inLayout) keys.push_back(a.key);
		return keys == c.loadedEntryKeys || keys.empty();
	}

	uint32_t ChooseTableKeyShift(const Collection& c)
	{
		std::vector<uint32_t> keys;
		for (const Attribute& a : c.attributes)
			if (!a.inLayout) keys.push_back(a.key);
		if (keys == c.loadedEntryKeys || keys.empty())
			return c.tableKeyShift;
		const uint32_t n = uint32_t(keys.size());
		auto worstProbe = [&](uint32_t shift)
		{
			std::vector<char> used(n, 0);
			uint32_t worst = 0;
			for (uint32_t k : keys)
			{
				uint32_t i = Rotl(k, shift) % n, probe = 0;
				while (used[i])
				{
					i = (i + 1) % n;
					++probe;
				}
				used[i] = 1;
				worst = std::max(worst, probe);
			}
			return worst;
		};
		uint32_t best = c.tableKeyShift & 31, bestWorst = worstProbe(best);
		for (uint32_t s = 0; s < 32; ++s)
		{
			const uint32_t w = worstProbe(s);
			if (w < bestWorst)
			{
				best = s;
				bestWorst = w;
			}
		}
		return best;
	}

	VaultBlobs WriteVault(const Database& db, const Vault& vault)
	{
		return VaultWriter(db, vault).write();
	}

	Bytes WritePack(const Database& db, const LoadedPack& pack)
	{
		if (pack.format == PackFormat::VaultFile)
		{
			// World's event vault: its own header, one vault (Pack.h).
			if (pack.vaults.size() != 1) throw std::runtime_error(pack.path + ": an event vault file holds one vault");
			const Vault& v = *pack.vaults.front();
			if (v.name.empty() || v.name.size() >= 0x2C) throw std::runtime_error("the vault name " + v.name + " doesn't fit an event vault file");
			VaultBlobs b = WriteVault(db, v);
			ByteWriter out(Endian::Little);
			out.raw(v.name.data(), v.name.size());
			out.fill(0x2C - v.name.size());
			out.fill(0x14);
			out.align(0x40);
			const uint32_t binOff = uint32_t(out.size());
			out.raw(b.bin);
			out.align(0x40);
			const uint32_t vltOff = uint32_t(out.size());
			out.raw(b.vlt);
			out.align(0x40);
			out.put32(0x2C, binOff);
			out.put32(0x30, uint32_t(b.bin.size()));
			out.put32(0x34, vltOff);
			out.put32(0x38, uint32_t(b.vlt.size()));
			out.put32(0x3C, uint32_t(out.size()));
			return std::move(out.bytes());
		}
		std::vector<const Vault*> vaults;
		for (const auto& v : pack.vaults) vaults.push_back(v.get());
		std::stable_sort(vaults.begin(), vaults.end(), [](const Vault* a, const Vault* b)
		{
			return a->name < b->name;
		});

		ByteWriter out(Endian::Little);
		const uint32_t n = uint32_t(vaults.size());
		out.raw("VPAK", 4);
		out.u32(n);
		out.u32(0);
		out.u32(0);
		out.fill(size_t(n) * 0x14);
		out.align(pack.align);
		const uint32_t strOff = uint32_t(out.size());
		std::vector<uint32_t> nameOff;
		for (const Vault* v : vaults)
		{
			nameOff.push_back(uint32_t(out.size()) - strOff);
			out.cstr(v->name);
		}
		out.put32(8, strOff);
		out.put32(12, uint32_t(out.size()) - strOff);
		for (uint32_t i = 0; i < n; ++i)
		{
			VaultBlobs b = WriteVault(db, *vaults[i]);
			out.align(pack.align);
			const uint32_t binOff = uint32_t(out.size());
			out.raw(b.bin);
			out.align(pack.align);
			const uint32_t vltOff = uint32_t(out.size());
			out.raw(b.vlt);
			const size_t e = 0x10 + size_t(i) * 0x14;
			out.put32(e, nameOff[i]);
			out.put32(e + 4, uint32_t(b.bin.size()));
			out.put32(e + 8, uint32_t(b.vlt.size()));
			out.put32(e + 12, binOff);
			out.put32(e + 16, vltOff);
		}
		out.align(pack.align);
		return std::move(out.bytes());
	}

	Bytes WritePackFile(const Database& db, const LoadedPack& pack)
	{
		Bytes data = WritePack(db, pack);
		if (pack.compression)
			return Compress(*pack.compression, data);
		return data;
	}

	namespace
	{
		std::string Lower(std::string s)
		{
			for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// Writes next to the target first so a failure can't leave a half-written file.
		void ReplaceFile(const std::string& path, ByteSpan data)
		{
			const std::string tmp = path + ".tmp";
			WriteFile(tmp, data);
			std::remove(path.c_str());
			if (std::rename(tmp.c_str(), path.c_str()) != 0)
				throw std::runtime_error("could not replace " + path);
		}
	} // namespace

	std::optional<std::string> PackTwin(const std::string& path)
	{
		namespace fs = std::filesystem;
		const fs::path p = fs::path(std::u8string(path.begin(), path.end()));
		const std::u8string u8ext = p.extension().u8string();
		const std::string ext(u8ext.begin(), u8ext.end());
		const bool lzc = Lower(ext) == ".lzc";
		if (!lzc && Lower(ext) != ".bin") return std::nullopt;
		// Same case as the original's extension.
		const bool upper = !ext.empty() && std::isupper(static_cast<unsigned char>(ext.back()));
		std::string twinExt = lzc ? ".bin" : ".lzc";
		if (upper)
			for (char& c : twinExt) c = char(std::toupper(static_cast<unsigned char>(c)));
		fs::path twin = p;
		twin.replace_extension(fs::path(twinExt));
		// An existing file under another case is the one to write.
		std::error_code ec;
		const fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
		const std::u8string want8 = twin.filename().u8string();
		const std::string want = Lower(std::string(want8.begin(), want8.end()));
		bool exists = false;
		for (const auto& e : fs::directory_iterator(dir, ec))
		{
			const std::u8string n8 = e.path().filename().u8string();
			if (e.is_regular_file(ec) && Lower(std::string(n8.begin(), n8.end())) == want)
			{
				twin = e.path();
				exists = true;
				break;
			}
		}
		if (!lzc && !exists) return std::nullopt;
		const std::u8string out = twin.u8string();
		return std::string(out.begin(), out.end());
	}

	void SavePack(const Database& db, LoadedPack& pack, const std::string& path)
	{
		const Bytes data = WritePack(db, pack);
		ReplaceFile(path, pack.compression ? Compress(*pack.compression, data) : data);
		if (const std::optional<std::string> twin = PackTwin(path))
		{
			if (Lower(*twin).ends_with(".bin"))
			{
				ReplaceFile(*twin, data);
			}
			else
			{
				// Compressed the way the twin on disk is (JDLZ, as in retail, if unreadable).
				LzKind kind = LzKind::Jdlz;
				try
				{
					if (auto h = ParseLzHeader(ReadFile(*twin))) kind = h->kind;
				}
				catch (const std::exception&)
				{
				}
				ReplaceFile(*twin, Compress(kind, data));
			}
		}
		pack.path = path;
		pack.dirty = false;
	}

} // namespace attrib
