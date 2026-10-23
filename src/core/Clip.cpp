#include "Clip.h"

#include "Edit.h"
#include "Hash.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace attrib
{

	namespace
	{

		constexpr char kHeader[] = "Attribulina collections 1";
		constexpr uint32_t kVersion = 2; // 2: u64 export ids (the early Most Wanted demos)
		constexpr int kMaxDepth = 64; // nested blocks; retail goes two deep

		const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		std::string Base64(const Bytes& in)
		{
			std::string out;
			out.reserve((in.size() + 2) / 3 * 4 + in.size() / 57 + 1);
			for (size_t i = 0; i < in.size(); i += 3)
			{
				const uint32_t n = (uint32_t(in[i]) << 16) | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0) |
					(i + 2 < in.size() ? uint32_t(in[i + 2]) : 0);
				out += kBase64[(n >> 18) & 63];
				out += kBase64[(n >> 12) & 63];
				out += i + 1 < in.size() ? kBase64[(n >> 6) & 63] : '=';
				out += i + 2 < in.size() ? kBase64[n & 63] : '=';
				if ((i / 3 + 1) % 19 == 0 && i + 3 < in.size()) out += '\n'; // 76 characters a line
			}
			return out;
		}

		std::optional<Bytes> FromBase64(std::string_view in)
		{
			int value[256];
			std::fill(std::begin(value), std::end(value), -1);
			for (int i = 0; i < 64; ++i) value[uint8_t(kBase64[i])] = i;
			Bytes out;
			uint32_t acc = 0;
			int bits = 0;
			for (char ch : in)
			{
				if (std::isspace(static_cast<unsigned char>(ch))) continue;
				if (ch == '=') break;
				const int v = value[uint8_t(ch)];
				if (v < 0) return std::nullopt;
				acc = (acc << 6) | uint32_t(v);
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					out.push_back(uint8_t(acc >> bits));
				}
			}
			return out;
		}

		// --------------------------------------------------------------- writing ---

		class ClipWriter
		{
		public:
			ByteWriter w{ Endian::Little };

			void str(const std::string& s)
			{
				w.u32(uint32_t(s.size()));
				w.raw(s.data(), s.size());
			}

			void block(const DataBlock& b)
			{
				w.u32(b.align);
				w.u32(uint32_t(b.bytes.size()));
				w.raw(b.bytes);
				w.u32(uint32_t(b.ptrs.size()));
				for (const PtrSlot& p : b.ptrs)
				{
					w.u32(p.offset);
					if (p.isString)
					{
						w.u8(1);
						str(p.text);
					}
					else if (!p.data)
					{
						w.u8(0);
					}
					else if (auto it = seen_.find(p.data.get()); it != seen_.end())
					{
						w.u8(3);
						w.u32(it->second);
					}
					else
					{
						// Numbered before its contents, as the reader numbers it.
						const uint32_t index = uint32_t(seen_.size());
						seen_[p.data.get()] = index;
						w.u8(2);
						block(*p.data);
					}
				}
			}

		private:
			std::unordered_map<const DataBlock*, uint32_t> seen_;
		};

		// --------------------------------------------------------------- reading ---

		class ClipReader
		{
		public:
			explicit ClipReader(const Bytes& b) : r_(b, Endian::Little)
			{
			}

			uint8_t u8()
			{
				return r_.u8(take(1));
			}
			uint16_t u16()
			{
				return r_.u16(take(2));
			}
			uint32_t u32()
			{
				return r_.u32(take(4));
			}
			uint64_t u64()
			{
				return r_.u64(take(8));
			}
			std::string str()
			{
				const uint32_t n = u32();
				const ByteSpan s = r_.slice(take(n), n);
				return std::string(s.begin(), s.end());
			}
			// A count of things at least `each` bytes long, checked against what is left.
			uint32_t count(size_t each)
			{
				const uint32_t n = u32();
				if (each && n > (r_.size() - pos_) / each) throw FormatError("count past the end");
				return n;
			}

			DataBlock block(int depth = 0)
			{
				if (depth > kMaxDepth) throw FormatError("blocks nest too deep");
				DataBlock b;
				b.align = u32();
				const uint32_t n = u32();
				const ByteSpan s = r_.slice(take(n), n);
				b.bytes.assign(s.begin(), s.end());
				const uint32_t ptrs = count(5);
				for (uint32_t i = 0; i < ptrs; ++i)
				{
					PtrSlot p;
					p.offset = u32();
					if (size_t(p.offset) + 4 > b.bytes.size()) throw FormatError("pointer outside its block");
					switch (u8())
					{
						case 0: break;
						case 1:
							p.isString = true;
							p.text = str();
							break;
						case 2:
						{
							auto nb = std::make_shared<DataBlock>();
							blocks_.push_back(nb);
							*nb = block(depth + 1);
							p.data = nb;
							break;
						}
						case 3:
						{
							const uint32_t at = u32();
							if (at >= blocks_.size()) throw FormatError("bad block reference");
							p.data = blocks_[at];
							break;
						}
						default: throw FormatError("bad pointer kind");
					}
					b.ptrs.push_back(std::move(p));
				}
				return b;
			}

			bool atEnd() const
			{
				return pos_ == r_.size();
			}

		private:
			size_t take(size_t n)
			{
				r_.require(pos_, n);
				const size_t at = pos_;
				pos_ += n;
				return at;
			}

			ByteReader r_;
			size_t pos_ = 0;
			std::vector<std::shared_ptr<DataBlock>> blocks_;
		};

		std::string KeyText(uint32_t key)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "0x%08X", key);
			return b;
		}

		Vault* VaultNamed(Database& db, const std::string& name, LoadedPack* prefer)
		{
			if (prefer)
				if (Vault* v = FindVault(*prefer, name)) return v;
			for (const auto& p : db.packs())
				if (Vault* v = FindVault(*p, name)) return v;
			return nullptr;
		}

	} // namespace

	Clip CopyCollections(const Database& db, const std::vector<const Collection*>& roots, bool withChildren)
	{
		Clip clip;
		if (roots.empty()) return clip;
		const Collection& first = *roots.front();
		clip.classKey = first.classKey;
		clip.className = db.names().format(first.classKey);
		clip.layoutSize = first.cls ? first.cls->layoutSize : 0;
		clip.endian = first.vault ? first.vault->raw.endian : Endian::Little;

		std::unordered_map<const Collection*, int> index;
		std::function<void(const Collection*)> add = [&](const Collection* c)
		{
			if (c->classKey != clip.classKey || index.count(c)) return;
			ClipCollection cc;
			cc.key = c->key;
			cc.exportId = c->exportId;
			if (const std::string* n = db.names().find(c->key)) cc.name = *n;
			const auto pit = c->parent ? index.find(c->parent) : index.end();
			cc.parent = pit != index.end() ? pit->second : -1;
			cc.parentKey = c->parentKey;
			if (c->vault) cc.vault = c->vault->name;
			cc.layout = c->layout;
			for (const Attribute& a : c->attributes)
			{
				ClipValue v;
				v.field = a.key;
				v.typeHash = a.def ? a.def->typeHash : a.type ? a.type->hash : 0;
				v.typeSize = a.type ? a.type->size : a.elemSize;
				v.inLayout = a.inLayout;
				v.isArray = a.isArray;
				v.capacity = a.capacity;
				v.encodedTypePad = a.encodedTypePad;
				v.nodeFlags = a.nodeFlags;
				v.entryFlags = a.entryFlags;
				v.count = a.count;
				v.elemSize = a.elemSize;
				v.data = a.data;
				cc.values.push_back(std::move(v));
			}
			index[c] = int(clip.collections.size());
			clip.collections.push_back(std::move(cc));
			if (withChildren)
				for (const Collection* k : c->children) add(k);
		};
		// Shallow roots first, so a root under another one lands below it.
		std::vector<const Collection*> order = roots;
		auto depth = [](const Collection* c)
		{
			int d = 0;
			for (const Collection* p = c->parent; p && d < 4096; p = p->parent) ++d;
			return d;
		};
		std::stable_sort(order.begin(), order.end(), [&](const Collection* a, const Collection* b)
		{
			return depth(a) < depth(b);
		});
		for (const Collection* c : order) add(c);
		return clip;
	}

	std::string EncodeClip(const Clip& clip)
	{
		ClipWriter cw;
		ByteWriter& w = cw.w;
		w.raw("ACLP", 4);
		w.u32(kVersion);
		w.u64(clip.source);
		w.u8(clip.cut ? 1 : 0);
		w.u8(clip.endian == Endian::Big ? 1 : 0);
		cw.str(clip.origin);
		w.u32(clip.classKey);
		cw.str(clip.className);
		w.u32(clip.layoutSize);
		w.u32(uint32_t(clip.collections.size()));
		for (const ClipCollection& c : clip.collections)
		{
			w.u32(c.key);
			w.u64(c.exportId);
			cw.str(c.name);
			w.u32(uint32_t(c.parent));
			w.u32(c.parentKey);
			cw.str(c.vault);
			cw.block(c.layout);
			w.u32(uint32_t(c.values.size()));
			for (const ClipValue& v : c.values)
			{
				w.u32(v.field);
				w.u32(v.typeHash);
				w.u32(v.typeSize);
				w.u8(uint8_t((v.inLayout ? 1 : 0) | (v.isArray ? 2 : 0)));
				w.u16(v.capacity);
				w.u16(v.encodedTypePad);
				w.u8(v.nodeFlags);
				w.u8(v.entryFlags);
				w.u32(v.count);
				w.u32(v.elemSize);
				cw.block(v.data);
			}
		}

		// What's in it, readable, for anyone who pastes it into a text editor.
		std::string text = std::string(kHeader) + "\n";
		if (!clip.collections.empty())
		{
			const ClipCollection& c = clip.collections.front();
			text += "# " + clip.className + "/" + (c.name.empty() ? KeyText(c.key) : c.name);
			if (clip.collections.size() > 1) text += " and " + std::to_string(clip.collections.size() - 1) + " more";
			if (!clip.origin.empty()) text += ", from " + clip.origin;
			text += "\n";
		}
		return text + Base64(w.bytes()) + "\n";
	}

	std::optional<Clip> DecodeClip(std::string_view text)
	{
		// The header line, comments, then base64.
		const size_t nl = text.find('\n');
		std::string_view head = text.substr(0, nl);
		if (!head.empty() && head.back() == '\r') head.remove_suffix(1);
		if (head != kHeader || nl == std::string_view::npos) return std::nullopt;
		std::string payload;
		for (size_t at = nl + 1; at < text.size();)
		{
			size_t end = text.find('\n', at);
			if (end == std::string_view::npos) end = text.size();
			const std::string_view line = text.substr(at, end - at);
			if (!line.empty() && line.front() != '#') payload.append(line);
			at = end + 1;
		}
		const std::optional<Bytes> bytes = FromBase64(payload);
		if (!bytes || bytes->size() < 8 || std::memcmp(bytes->data(), "ACLP", 4) != 0) return std::nullopt;
		try
		{
			ClipReader r(*bytes);
			r.u32(); // magic
			if (r.u32() != kVersion) return std::nullopt;
			Clip clip;
			clip.source = r.u64();
			clip.cut = r.u8() != 0;
			clip.endian = r.u8() ? Endian::Big : Endian::Little;
			clip.origin = r.str();
			clip.classKey = r.u32();
			clip.className = r.str();
			clip.layoutSize = r.u32();
			const uint32_t n = r.count(32);
			for (uint32_t i = 0; i < n; ++i)
			{
				ClipCollection c;
				c.key = r.u32();
				c.exportId = r.u64();
				c.name = r.str();
				c.parent = int32_t(r.u32());
				if (c.parent < -1 || c.parent >= int(i)) return std::nullopt; // parents come first
				c.parentKey = r.u32();
				c.vault = r.str();
				c.layout = r.block();
				const uint32_t values = r.count(40);
				for (uint32_t j = 0; j < values; ++j)
				{
					ClipValue v;
					v.field = r.u32();
					v.typeHash = r.u32();
					v.typeSize = r.u32();
					const uint8_t flags = r.u8();
					v.inLayout = flags & 1;
					v.isArray = flags & 2;
					v.capacity = r.u16();
					v.encodedTypePad = r.u16();
					v.nodeFlags = r.u8();
					v.entryFlags = r.u8();
					v.count = r.u32();
					v.elemSize = r.u32();
					v.data = r.block();
					if (uint64_t(v.count) * v.elemSize != v.data.bytes.size()) return std::nullopt;
					c.values.push_back(std::move(v));
				}
				clip.collections.push_back(std::move(c));
			}
			if (!r.atEnd()) return std::nullopt;
			return clip;
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}

	std::string CopyName(const Database& db, uint32_t classKey, const std::string& name)
	{
		std::string base = name;
		int n = 1;
		if (const size_t at = name.rfind("_copy"); at != std::string::npos)
		{
			const std::string tail = name.substr(at + 5);
			if (tail.size() < 6 && std::all_of(tail.begin(), tail.end(), [](char ch)
			{
				return std::isdigit(static_cast<unsigned char>(ch));
			}))
			{
				base = name.substr(0, at);
				n = tail.empty() ? 1 : std::max(1, std::atoi(tail.c_str()));
			}
		}
		for (int i = n; i < n + 100000; ++i)
		{
			const std::string candidate = base + "_copy" + (i == 1 ? std::string() : std::to_string(i));
			if (!db.findCollection(classKey, StringHash32(candidate))) return candidate;
		}
		throw std::runtime_error("no free name for a copy of " + name);
	}

	PasteResult PasteClip(Database& db, const Clip& clip, ClassDef& cls, Collection* parent, Vault& vault)
	{
		if (clip.classKey != cls.key)
			throw std::runtime_error("the copied collections are of class " + clip.className + ", not " + db.names().format(cls.key));
		if (parent && parent->cls != &cls)
			throw std::runtime_error("a collection's parent must be of its own class");
		if (clip.endian != vault.raw.endian)
			throw std::runtime_error(std::string("the copied values are ") + (clip.endian == Endian::Big ? "big" : "little") +
									 "-endian and vault " + vault.name + " isn't");
		const KeyNames& names = db.names();
		CollectionEditor ed(db);
		PasteResult r;
		std::vector<Collection*> made(clip.collections.size(), nullptr);
		for (size_t i = 0; i < clip.collections.size(); ++i)
		{
			const ClipCollection& cc = clip.collections[i];
			Collection* p = cc.parent >= 0 ? made[size_t(cc.parent)] : parent;

			// The vault: the roots' for anything that was in the roots' vault.
			Vault* v = &vault;
			if (cc.parent >= 0)
			{
				size_t root = i;
				while (clip.collections[root].parent >= 0) root = size_t(clip.collections[root].parent);
				if (cc.vault != clip.collections[root].vault)
					if (Vault* same = VaultNamed(db, cc.vault, vault.pack); same && same->raw.endian == clip.endian) v = same;
			}

			// The name: kept while free, else a copy name. A collection known
			// only by its key keeps the key (and the original's export id).
			Collection* n = nullptr;
			const bool taken = db.findCollection(cls.key, cc.key) != nullptr;
			if (!taken && cc.name.empty())
			{
				n = &ed.create(*v, cls, cc.key, p);
				n->exportId = cc.exportId;
			}
			else
			{
				const std::string name = taken ? CopyName(db, cls.key, cc.name.empty() ? KeyText(cc.key) : cc.name) : cc.name;
				if (!names.find(StringHash32(name))) r.newNames.push_back(name);
				n = &ed.create(*v, cls, name, p);
			}
			made[i] = n;
			const std::string label = names.format(n->key);

			if (clip.layoutSize == cls.layoutSize)
			{
				n->layout = cc.layout;
				DetachCopy(n->layout);
			}
			for (const ClipValue& cv : cc.values)
			{
				const FieldDef* f = cls.field(cv.field);
				const std::string field = names.format(cv.field);
				if (!f)
				{
					r.warnings.push_back(label + ": " + clip.className + " has no field " + field + " here; its value was left out");
					continue;
				}
				const uint32_t size = f->type ? f->type->size : f->size;
				if (f->typeHash != cv.typeHash || size != cv.typeSize || f->inLayout() != cv.inLayout || f->isArray() != cv.isArray)
				{
					r.warnings.push_back(label + ": " + field + " has another type here; its value was left out");
					continue;
				}
				if (cv.inLayout && cv.isArray && (cv.count > cv.capacity || f->offset + LayoutExtent(*f, cv.capacity) > cls.layoutSize))
				{
					r.warnings.push_back(label + ": " + field + " doesn't fit the layout here; its value was left out");
					continue;
				}
				Attribute a;
				a.key = f->key;
				a.def = f;
				a.type = f->type;
				a.inLayout = cv.inLayout;
				a.isArray = cv.isArray;
				a.capacity = cv.capacity;
				a.encodedTypePad = cv.encodedTypePad;
				a.nodeFlags = cv.nodeFlags;
				a.entryFlags = cv.entryFlags;
				a.count = cv.count;
				a.elemSize = cv.elemSize;
				a.data = cv.data;
				DetachCopy(a);
				if (Attribute* own = n->find(f->key)) *own = std::move(a);
				else n->attributes.push_back(std::move(a));
			}
			r.created.push_back(n);
		}
		return r;
	}

} // namespace attrib
