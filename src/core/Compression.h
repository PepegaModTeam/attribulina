#pragma once

// LZHeader-wrapped compressed blocks: gameplay.lzc and the compiled Lua chunks
// inside gameplay vaults. The codecs are the game's own (BBXCompression/).
//
//   LZHeader (0x10, little-endian):
//     +0x0 char ID[4]            "JDLZ" | "HUFF" | "RAWW" | "RFPK"
//     +0x4 u8   Version          JDLZ 2, HUFF 1, RAWW 1, RFPK 1
//     +0x5 u8   HeaderSize       0x10
//     +0x6 u16  Flags            0 in all retail data
//     +0x8 u32  UncompressedSize
//     +0xC u32  CompressedSize   JDLZ: the whole block; the game's HUFF, RAWW and
//                                RFPK blocks: the payload after this header
//
// JDLZ's codec writes this header itself; HUFF is the header followed by a raw
// EA Huffman stream (0x30FB); RAWW is the header followed by stored bytes;
// RFPK is the header followed by an EA RefPack stream (0x10FB). Only
// Undercover's code reads RFPK, and only its 360 alpha's gameplay.lzc uses it.

#include "Bytes.h"

#include <optional>
#include <string_view>

namespace attrib
{

	enum class LzKind : uint8_t
	{
		Jdlz, Huff, Raww, Rfpk
	};

	struct LzHeader
	{
		LzKind kind;
		uint8_t version;
		uint8_t headerSize;
		uint16_t flags;
		uint32_t uncompressedSize;
		uint32_t compressedSize;
	};

	// Parses an LZHeader at the start of data; nullopt if the magic is not one we know.
	std::optional<LzHeader> ParseLzHeader(ByteSpan data);

	// Decompresses one LZHeader block. Throws FormatError on a bad block.
	Bytes Decompress(ByteSpan block);

	// Produces a complete LZHeader block.
	Bytes Compress(LzKind kind, ByteSpan data);

	// The game's LZCompress (Pro Street milestone, 0x82615418): encodes with HUFF
	// and JDLZ and keeps JDLZ only when it is strictly smaller. When HUFF wins but
	// is bigger than the data plus a header, the data is stored as RAWW instead,
	// with CompressedSize = the data size (not counting the header), as the game
	// writes it. Use it for blocks the editor creates, such as compiled Lua.
	Bytes CompressBest(ByteSpan data);

	const char* LzKindName(LzKind kind);
	// "jdlz", "huff", "raww" or "rfpk", in any case; nullopt for anything else.
	std::optional<LzKind> ParseLzKind(std::string_view name);

} // namespace attrib
