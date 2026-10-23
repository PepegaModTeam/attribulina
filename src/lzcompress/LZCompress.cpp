// LZCompress.cpp - LZHeader helpers and the per-codec header wrappers.
//
// Decompiled from LZCompress.cpp in Need for Speed: Undercover (Xbox 360, SpeedDebugOpt-enUS):
//   LZGetMaxCompressedSize 0x82A523B8   LZByteSwapHeader 0x82A523C8   LZValidHeader 0x82A52400
//   LZCompress             0x82A64A60   LZDecompress     0x82A64AD8
//   HUFFCompress           0x82A51D40   HUFFDecompress   0x82A51CA0
//   RAWCompress            0x82A51E30   RAWDecompress    0x82A51DB0
//   REFPACKCompress        0x82A6C908   REFPACKDecompress 0x82A64570
// and ProStreet (ProStreet08Milestone): LZCompress 0x82615418, LZDecompress 0x826154C0 (no RFPK).

#include "LZCompress.h"

#include <cstring>

// Worst case output size the game budgets for LZCompress().
unsigned int LZGetMaxCompressedSize(unsigned int uncompressed_size)
{
	return (uncompressed_size + 12) * 2;
}

// Swaps the multi-byte header fields between the on-disc (little endian) layout and a
// big-endian host. Version / HeaderSize are single bytes and stay put.
void LZByteSwapHeader(LZHeader* header)
{
	uint32_t id = header->ID;
	header->ID = (id >> 24) | ((id >> 8) & 0xFF00) | ((id << 8) & 0xFF0000) | (id << 24);
	header->Flags = static_cast<uint16_t>((header->Flags >> 8) | (header->Flags << 8));
	uint32_t u = header->UncompressedSize;
	header->UncompressedSize = (u >> 24) | ((u >> 8) & 0xFF00) | ((u << 8) & 0xFF0000) | (u << 24);
	uint32_t c = header->CompressedSize;
	header->CompressedSize = (c >> 24) | ((c >> 8) & 0xFF00) | ((c << 8) & 0xFF0000) | (c << 24);
}

// JDLZ must be version 2, everything else version 1. (ProStreet does not know RFPK.)
int LZValidHeader(LZHeader* header)
{
	switch (header->ID)
	{
		case LZ_ID_COMP:
		case LZ_ID_HUFF:
		case LZ_ID_RFPK:
		case LZ_ID_RAWW:
			return header->Version == 1;
		case LZ_ID_JDLZ:
			return header->Version == 2;
		default:
			return 0;
	}
}

// ---------------------------------------------------------------------------------------------
// RAWW: stored. CompressedSize = UncompressedSize = size, payload follows the header.
// ---------------------------------------------------------------------------------------------

int RAWCompress(unsigned char* pSrc, int size, unsigned char* pDst)
{
	LZHeader* header = reinterpret_cast<LZHeader*>(pDst);
	header->ID = LZ_ID_RAWW;
	header->Version = 1;
	header->HeaderSize = 0x10;
	header->Flags = 0;
	header->UncompressedSize = size;
	header->CompressedSize = size;
	std::memcpy(pDst + 0x10, pSrc, size);
	return static_cast<int>(header->CompressedSize) + 0x10;
}

int RAWDecompress(unsigned char* pSrc, unsigned char* pDst)
{
	LZHeader* header = reinterpret_cast<LZHeader*>(pSrc);
	if (header->ID == LZ_ID_RAWW && header->Version == 1)
	{
		std::memcpy(pDst, pSrc + 0x10, header->UncompressedSize);
		return static_cast<int>(header->UncompressedSize);
	}
	// bPrintf("ERROR:  RAWDecompress() Bad version\n");
	return 0;
}

// ---------------------------------------------------------------------------------------------
// HUFF / RFPK: LZHeader + bare stream. CompressedSize is the stream size (header excluded).
// ---------------------------------------------------------------------------------------------

int HUFFCompress(unsigned char* pSrc, int size, unsigned char* pDst)
{
	LZHeader* header = reinterpret_cast<LZHeader*>(pDst);
	header->ID = LZ_ID_HUFF;
	header->Version = 1;
	header->HeaderSize = 0x10;
	header->Flags = 0;
	header->UncompressedSize = size;
	int compressed_size = HUFF_encode(pDst + 0x10, pSrc, size);
	header->CompressedSize = compressed_size;
	return compressed_size + 0x10;
}

int HUFFDecompress(unsigned char* pSrc, unsigned char* pDst)
{
	LZHeader* header = reinterpret_cast<LZHeader*>(pSrc);
	if (header->ID == LZ_ID_HUFF && header->Version == 1)
	{
		int error = HUFF_decompress(pSrc + 0x10, pDst);
		(void)error; // bAssert(error == header->UncompressedSize);  (Undercover only)
		return static_cast<int>(header->UncompressedSize);
	}
	// bPrintf("ERROR:  HuffDecompress() Bad version\n");
	return 0;
}

int REFPACKCompress(unsigned char* pSrc, int size, unsigned char* pDst)
{
	LZHeader* header = reinterpret_cast<LZHeader*>(pDst);
	header->ID = LZ_ID_RFPK;
	header->Version = 1;
	header->HeaderSize = 0x10;
	header->Flags = 0;
	header->UncompressedSize = size;
	int compressed_size = REFPACK_Compress(pDst + 0x10, pSrc, size);
	header->CompressedSize = compressed_size;
	return compressed_size + 0x10;
}

int REFPACKDecompress(unsigned char* pSrc, unsigned char* pDst)
{
	LZHeader* header = reinterpret_cast<LZHeader*>(pSrc);
	if (header->ID == LZ_ID_RFPK && header->Version == 1)
	{
		unsigned int error = REFPACK_Decompress(pDst, pSrc + 0x10);
		(void)error; // bAssert(error == header->UncompressedSize);
		return static_cast<int>(header->UncompressedSize);
	}
	// bPrintf("ERROR:  RefPackDecompress() Bad version\n");
	return 0;
}

// ---------------------------------------------------------------------------------------------
// Top level
// ---------------------------------------------------------------------------------------------


// ProStreet: tries HUFF and JDLZ and keeps the smaller (re-running HUFF if it won, since JDLZ
// overwrote the buffer). Note the RAWW fallback is only checked when HUFF wins.
unsigned int LZCompress(unsigned char* pSrc, unsigned int size, unsigned char* pDst)
{
	unsigned int huff_size = static_cast<unsigned int>(HUFFCompress(pSrc, static_cast<int>(size), pDst));
	unsigned int jlz_size = static_cast<unsigned int>(JLZCompress(pSrc, static_cast<int>(size), pDst));
	if (huff_size > jlz_size)
	{
		return jlz_size;
	}
	if (huff_size > size + 0x10)
	{
		return static_cast<unsigned int>(RAWCompress(pSrc, static_cast<int>(size), pDst));
	}
	return static_cast<unsigned int>(HUFFCompress(pSrc, static_cast<int>(size), pDst));
}

// Dispatches on the header ID. Returns the uncompressed size, or 0 for an unknown ID.
// (The game expects LZByteSwapHeader() to have been applied on big-endian hosts.)
unsigned int LZDecompress(unsigned char* pSrc, unsigned char* pDst)
{
	if (pSrc == nullptr || pDst == nullptr)
	{
		return 0;
	}

	switch (reinterpret_cast<LZHeader*>(pSrc)->ID)
	{
		case LZ_ID_COMP:
			return OldLZDecompress(pSrc, pDst);
		case LZ_ID_HUFF:
			return static_cast<unsigned int>(HUFFDecompress(pSrc, pDst));
		case LZ_ID_RFPK:
			return static_cast<unsigned int>(REFPACKDecompress(pSrc, pDst));
		case LZ_ID_RAWW:
			return static_cast<unsigned int>(RAWDecompress(pSrc, pDst));
		case LZ_ID_JDLZ:
			return static_cast<unsigned int>(JLZDecompress(pSrc, pDst));
		default:
			// bPrintf("ERROR:  LZDecompress() given data that was not LZCompressed!\n");
			return 0;
	}
}
