#include "Compression.h"

#include "lzcompress/LZCompress.h"

#include <algorithm>
#include <cctype>

namespace attrib
{

	namespace
	{
		constexpr uint32_t kHeaderSize = 0x10;

		bool MagicIs(ByteSpan d, const char* m)
		{
			return d.size() >= 4 && std::memcmp(d.data(), m, 4) == 0;
		}

		void WriteHeader(uint8_t* out, const char* magic, uint8_t version, uint32_t usize, uint32_t csize)
		{
			std::memcpy(out, magic, 4);
			out[4] = version;
			out[5] = uint8_t(kHeaderSize);
			store16(out + 6, 0, Endian::Little);
			store32(out + 8, usize, Endian::Little);
			store32(out + 12, csize, Endian::Little);
		}
	} // namespace

	const char* LzKindName(LzKind kind)
	{
		switch (kind)
		{
			case LzKind::Jdlz: return "JDLZ";
			case LzKind::Huff: return "HUFF";
			case LzKind::Raww: return "RAWW";
			case LzKind::Rfpk: return "RFPK";
		}
		return "?";
	}

	std::optional<LzKind> ParseLzKind(std::string_view name)
	{
		for (LzKind k : {LzKind::Jdlz, LzKind::Huff, LzKind::Raww, LzKind::Rfpk})
		{
			const char* n = LzKindName(k);
			if (name.size() == 4 && std::equal(name.begin(), name.end(), n, [](char a, char b)
			{
				return std::toupper(static_cast<unsigned char>(a)) == b;
			}))
				return k;
		}
		return std::nullopt;
	}

	std::optional<LzHeader> ParseLzHeader(ByteSpan d)
	{
		if (d.size() < kHeaderSize)
			return std::nullopt;
		LzHeader h{};
		if (MagicIs(d, "JDLZ")) h.kind = LzKind::Jdlz;
		else if (MagicIs(d, "HUFF")) h.kind = LzKind::Huff;
		else if (MagicIs(d, "RAWW")) h.kind = LzKind::Raww;
		else if (MagicIs(d, "RFPK")) h.kind = LzKind::Rfpk;
		else return std::nullopt;
		h.version = d[4];
		h.headerSize = d[5];
		h.flags = load16(d.data() + 6, Endian::Little);
		h.uncompressedSize = load32(d.data() + 8, Endian::Little);
		h.compressedSize = load32(d.data() + 12, Endian::Little);
		return h;
	}

	Bytes Decompress(ByteSpan block)
	{
		auto h = ParseLzHeader(block);
		if (!h)
			throw FormatError("not an LZHeader block");
		// CompressedSize counts the whole block in JDLZ (its codec writes the
		// header) but only the payload in the game's HUFF and RAWW blocks; VLTEd's
		// RAWW counts the header too. Only the bytes present matter.
		const bool sizeOk = h->kind != LzKind::Jdlz || h->compressedSize >= h->headerSize;
		if (h->headerSize < kHeaderSize || h->compressedSize > block.size() || !sizeOk)
			throw FormatError(std::string(LzKindName(h->kind)) + " block header is inconsistent");
		// The codecs trust their input; never let them write past a buffer we sized
		// from an untrusted header.
		if (h->uncompressedSize > (256u << 20))
			throw FormatError("compressed block claims an uncompressed size over 256 MB");

		Bytes out(h->uncompressedSize + 64);
		// The decoders read a few bytes ahead of where they are (HUFF's bit reader
		// most of all). In a vault the block is followed by other data; a block in
		// its own buffer isn't, so decode from a zero-padded copy.
		Bytes in;
		if (h->kind != LzKind::Raww)
		{
			in.assign(block.begin(), block.end());
			in.resize(in.size() + 64, 0);
		}
		int got = 0;
		switch (h->kind)
		{
			case LzKind::Raww:
				if (block.size() - h->headerSize < h->uncompressedSize)
					throw FormatError("RAWW block is shorter than its uncompressed size");
				std::memcpy(out.data(), block.data() + h->headerSize, h->uncompressedSize);
				got = int(h->uncompressedSize);
				break;
			case LzKind::Jdlz:
				// JDLZ is the codec's own header: the decoder takes the whole block.
				got = JLZDecompress(in.data(), out.data());
				break;
			case LzKind::Huff:
				// HUFF is LZHeader + a raw Huffman stream: skip the header.
				got = HUFF_decompress(in.data() + h->headerSize, out.data());
				break;
			case LzKind::Rfpk:
			{
				// LZHeader + a RefPack stream, which starts with its own size; the
				// decoder writes that many bytes and checks nothing.
				const uint8_t* s = in.data() + h->headerSize;
				const uint32_t type = (uint32_t(s[0]) << 8) | s[1];
				const bool wide = type & 0x8000;
				const uint8_t* n = s + 2 + ((type & 0x100) ? (wide ? 4 : 3) : 0);
				const uint32_t streamSize = wide ? load32(n, Endian::Big) : (uint32_t(n[0]) << 16) | (uint32_t(n[1]) << 8) | n[2];
				if ((type & 0x7EFF) != 0x10FB || streamSize != h->uncompressedSize)
					throw FormatError("RFPK block's stream doesn't match its header");
				got = int(REFPACK_Decompress(out.data(), s));
				break;
			}
		}
		if (got != int(h->uncompressedSize))
			throw FormatError(std::string(LzKindName(h->kind)) + " decode produced " + std::to_string(got) +
							  " bytes, header says " + std::to_string(h->uncompressedSize));
		out.resize(h->uncompressedSize);
		return out;
	}

	Bytes Compress(LzKind kind, ByteSpan data)
	{
		const uint32_t usize = uint32_t(data.size());
		Bytes out;
		switch (kind)
		{
			case LzKind::Raww:
				out.resize(kHeaderSize + usize);
				WriteHeader(out.data(), "RAWW", 1, usize, uint32_t(out.size()));
				std::memcpy(out.data() + kHeaderSize, data.data(), usize);
				return out;
			case LzKind::Jdlz:
			{
				// The encoder hashes 3 bytes at every position, so it reads up to
				// 2 bytes past the end; give it zeros there, not whatever follows.
				Bytes in(data.begin(), data.end());
				in.resize(in.size() + 16, 0);
				out.resize(size_t(usize) * 2 + 1024);
				int n = JLZCompress(in.data(), int(usize), out.data());
				if (n <= 0 || size_t(n) > out.size())
					throw FormatError("JDLZ encode failed");
				out.resize(size_t(n));
				return out;
			}
			case LzKind::Huff:
			{
				out.resize(kHeaderSize + size_t(usize) * 2 + 1024);
				int n = HUFF_encode(out.data() + kHeaderSize, data.data(), int(usize));
				if (n <= 0 || size_t(n) + kHeaderSize > out.size())
					throw FormatError("HUFF encode failed");
				out.resize(kHeaderSize + size_t(n));
				// The game's HUFF blocks count only the stream (as all 235 retail
				// ones do); JDLZ's codec counts the whole block.
				WriteHeader(out.data(), "HUFF", 1, usize, uint32_t(n));
				return out;
			}
			case LzKind::Rfpk:
			{
				// Literals cost a byte per 112 at worst. The encoder hashes 3
				// bytes ahead, so it gets zeros past the end.
				Bytes in(data.begin(), data.end());
				in.resize(in.size() + 16, 0);
				out.resize(kHeaderSize + size_t(usize) + usize / 64 + 1024);
				int n = REFPACK_Compress(out.data() + kHeaderSize, in.data(), int(usize));
				if (n <= 0 || size_t(n) + kHeaderSize > out.size())
					throw FormatError("RFPK encode failed");
				out.resize(kHeaderSize + size_t(n));
				WriteHeader(out.data(), "RFPK", 1, usize, uint32_t(n));
				return out;
			}
		}
		throw FormatError("unknown compression kind");
	}

	Bytes CompressBest(ByteSpan data)
	{
		Bytes huff = Compress(LzKind::Huff, data);
		Bytes jdlz = Compress(LzKind::Jdlz, data);
		if (huff.size() > jdlz.size()) return jdlz;
		const uint32_t usize = uint32_t(data.size());
		if (huff.size() > size_t(usize) + kHeaderSize)
		{
			Bytes out(kHeaderSize + usize);
			WriteHeader(out.data(), "RAWW", 1, usize, usize);
			std::memcpy(out.data() + kHeaderSize, data.data(), usize);
			return out;
		}
		return huff;
	}

} // namespace attrib
