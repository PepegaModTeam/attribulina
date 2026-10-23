// LZCompress.h
//
// Decompiled from Need for Speed: Undercover (Xbox 360, SpeedDebugOpt-enUS.xex)
// and Need for Speed: ProStreet (Xbox 360, ProStreet08Milestone.exe).
// Original source file: Speed\Indep\Src\Misc\LZCompress.cpp
//
// All of the game's general-purpose compressors live behind a 16 byte LZHeader:
//
//   +0x00  u32  ID                 FourCC, stored as the ASCII bytes "JDLZ", "HUFF", ...
//   +0x04  u8   Version            2 for JDLZ, 1 for everything else
//   +0x05  u8   HeaderSize         always 0x10
//   +0x06  u16  Flags              always 0
//   +0x08  u32  UncompressedSize   little endian
//   +0x0C  u32  CompressedSize     little endian. JDLZ / COMP: whole buffer including the header.
//                                  HUFF / RFPK / RAWW: just the stream after the header.
//
// The data on disc is little endian (it is the PC layout). The Xbox 360 build calls LZByteSwapHeader()
// on the header before handing it to LZDecompress(), after which the ID compares against the
// little-endian reading of the FourCC ('JDLZ' -> 0x5A4C444A). This code reads the header through
// the struct, so it expects a little-endian host (x86, ARM). On a big-endian host call
// LZByteSwapHeader() first, exactly like the game does.
//
// Codecs and the functions that implement them:
//
//   ID     Version  compress                     decompress
//   JDLZ   2        JLZCompress                  JLZDecompress          (header is part of the stream)
//   HUFF   1        HUFFCompress -> HUFF_encode  HUFFDecompress -> HUFF_decode / HUFF_decompress
//   RFPK   1        REFPACKCompress              REFPACKDecompress      (Undercover only)
//                     -> REFPACK_Compress          -> REFPACK_Decompress
//   COMP   1        (no compressor in the game)  OldLZDecompress        (header is part of the stream)
//   RAWW   1        RAWCompress                  RAWDecompress
//
// HUFF_encode / HUFF_decode / REFPACK_Compress / REFPACK_Decompress work on the bare stream that
// follows the 16 byte LZHeader. JLZCompress / JLZDecompress / OldLZDecompress take the LZHeader
// as part of their buffers.

#ifndef LZCOMPRESS_H
#define LZCOMPRESS_H

#include <cstdint>

class LZHeader
{
public:
	uint32_t ID;
	uint8_t Version;
	uint8_t HeaderSize;
	uint16_t Flags;
	uint32_t UncompressedSize;
	uint32_t CompressedSize;
};
static_assert(sizeof(LZHeader) == 0x10, "LZHeader must be 16 bytes");

// FourCC values as the game compares them: the four ASCII bytes read as a little-endian u32.
enum LZHeaderID : uint32_t
{
	LZ_ID_JDLZ = 0x5A4C444A, // "JDLZ"
	LZ_ID_HUFF = 0x46465548, // "HUFF"
	LZ_ID_RFPK = 0x4B504652, // "RFPK"
	LZ_ID_COMP = 0x504D4F43, // "COMP"
	LZ_ID_RAWW = 0x57574152, // "RAWW"
};

// Header helpers (LZCompress.cpp)
unsigned int LZGetMaxCompressedSize(unsigned int uncompressed_size);
void LZByteSwapHeader(LZHeader* header);
int LZValidHeader(LZHeader* header);

// Top level entry points (LZCompress.cpp). pDst for LZCompress must hold LZGetMaxCompressedSize(size).
// ProStreet picks the smaller of HUFF and JDLZ. Falls back to RAWW.
unsigned int LZDecompress(unsigned char* pSrc, unsigned char* pDst);
unsigned int LZCompress(unsigned char* pSrc, unsigned int size, unsigned char* pDst);

// JDLZ (JLZ.cpp)
int JLZCompress(unsigned char* pSrc, int SrcSize, unsigned char* pDst);
int JLZDecompress(unsigned char* pSrc, unsigned char* pDst);

// HUFF (Huff.cpp). These work on the stream *after* the LZHeader.
int HUFF_encode(void* compresseddata, const void* source, int sourcesize);
int HUFF_decode(void* dest, const void* compresseddata);
int HUFF_decompress(const unsigned char* packbuf, unsigned char* unpackbuf);
int HUFFCompress(unsigned char* pSrc, int size, unsigned char* pDst);
int HUFFDecompress(unsigned char* pSrc, unsigned char* pDst);

// REFPACK (RefPack.cpp). These work on the stream *after* the LZHeader.
int REFPACK_Compress(void* compresseddata, const void* source, int sourcesize);
unsigned int REFPACK_Decompress(void* dest, const void* compresseddata);
unsigned int GetDecompressedSize(const void* compresseddata);
int REFPACKCompress(unsigned char* pSrc, int size, unsigned char* pDst);
int REFPACKDecompress(unsigned char* pSrc, unsigned char* pDst);

// COMP (OldLZ.cpp)
unsigned int OldLZDecompress(unsigned char* pSrc, unsigned char* pDst);

// RAWW (LZCompress.cpp)
int RAWCompress(unsigned char* pSrc, int size, unsigned char* pDst);
int RAWDecompress(unsigned char* pSrc, unsigned char* pDst);

#endif
