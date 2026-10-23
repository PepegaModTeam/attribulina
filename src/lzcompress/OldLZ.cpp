// OldLZ.cpp - "COMP" decompressor (the game has no compressor for it any more).
//
// Decompiled from LZCompress.cpp:
//   Undercover  OldLZDecompress  0x82A51EA0
//   ProStreet   OldLZDecompress  0x826151B0   (identical apart from the bPrintf)
//
// Stream layout (the 16 byte LZHeader is part of the stream, CompressedSize includes it):
//
//   LZHeader  "COMP", 1, 0x10, Flags, UncompressedSize, CompressedSize
//   If Flags == 1 the payload is stored and simply copied out.
//   Otherwise the payload is a sequence of groups:
//     u16 (little endian)  control word, 16 flags LSB first: 0 = literal byte, 1 = match
//     tokens               literal: 1 byte
//                          match:   b0 = OOOO LLLL, b1 = OOOOOOOO
//                                   offset = ((b0 & 0xF0) << 4) | b1     (bytes back from the output cursor)
//                                   length = (b0 & 0x0F) + 3             3..18
//   Decoding stops when the input cursor reaches pSrc + CompressedSize.

#include "LZCompress.h"

#include <cstring>

// Returns the number of bytes written to pDst, or 0 if the header is not "COMP".
unsigned int OldLZDecompress(unsigned char* pSrc, unsigned char* pDst)
{
	if (pSrc == nullptr || pDst == nullptr)
	{
		return 0;
	}

	LZHeader* header = reinterpret_cast<LZHeader*>(pSrc);
	unsigned int compressed_size = header->CompressedSize;
	if (header->ID != LZ_ID_COMP)
	{
		// bPrintf("ERROR:  LZDecompress() given data that was not LZCompressed!\n");
		return 0;
	}

	unsigned char* pIn = pSrc + 0x10;
	unsigned char* pEnd = pSrc + compressed_size;
	unsigned char* pOut = pDst;
	unsigned int control = 1;

	if (header->Flags == 1)
	{
		unsigned int size = compressed_size - 0x10;
		std::memcpy(pDst, pIn, size);
		return size;
	}

	while (pIn != pEnd)
	{
		if (control == 1)
		{
			control = 0x10000 | pIn[0] | (pIn[1] << 8);
			pIn += 2;
		}

		// Far from the end of the input decode a whole group of 16 tokens without re-checking
		// pIn; near the end go one token at a time. (The game compares pIn <= pEnd - 32.)
		int count = (pEnd - pIn >= 32) ? 16 : 1;
		do
		{
			if (control & 1)
			{
				unsigned int extra = pIn[0] & 0x0F;
				unsigned int offset = ((pIn[0] & 0xF0) << 4) | pIn[1];
				pIn += 2;

				const unsigned char* pCopy = pOut - offset;
				*pOut++ = *pCopy++;
				*pOut++ = *pCopy++;
				*pOut++ = *pCopy++;
				while (extra != 0)
				{
					*pOut++ = *pCopy++;
					extra--;
				}
			}
			else
			{
				*pOut++ = *pIn++;
			}
			control >>= 1;
		} while (--count != 0);
	}

	return static_cast<unsigned int>(pOut - pDst);
}
