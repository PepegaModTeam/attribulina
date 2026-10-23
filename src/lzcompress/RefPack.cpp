// RefPack.cpp - EA RefPack ("RFPK" in an LZHeader, 0x10FB stream). Undercover only.
//
// Decompiled from LZCompress.cpp in Need for Speed: Undercover (Xbox 360, SpeedDebugOpt-enUS):
//   REFPACK_Compress     0x82A64618        REFPACKCompress     0x82A6C908
//   REFPACK_Decompress   0x82A52050        REFPACKDecompress   0x82A64570
//   GetHash 0x82A52360, MatchLength 0x82A52380, copyn 0x82A52030, GetDecompressedSize 0x82A52358
//
// Stream layout (what REFPACK_Compress writes and REFPACK_Decompress reads; it follows the
// 16 byte LZHeader):
//
//   u16 BE   type: 0x10FB, or 0x90FB when the sizes are 4 bytes (0x8000 flag). If 0x0100 is set
//            a compressed size field follows and is skipped by the decoder (never written here).
//   u24/u32  BE uncompressed size (u32 when type & 0x8000)
//   commands:
//     0PPL LLRR  OOOOOOOO                       copy R literals, then L+3 bytes from offset (P<<8 | O) + 1
//     10LL LLLL  RROO OOOO  OOOOOOOO            copy R literals, then L+4 bytes from offset (O) + 1
//     110P LLRR  OOOOOOOO  OOOOOOOO  LLLLLLLL   copy R literals, then L+5 bytes from offset (P<<16 | O) + 1
//     111R RRRR                                 copy (R+1)*4 literals (4..112)
//     1111 11RR                                 end of stream: copy R literals and stop
//
// The Xbox 360 REFPACK_Compress writes the type and size bytes by indexing into the in-memory
// short/int (code written for a little-endian PC), so on the 360 itself the header is broken:
// "FB 10" followed by the size's bits 8-15, 16-23, 24-31 (the low byte is lost). The compressed
// body is unaffected. This version writes the big-endian bytes the decoder expects, which is what
// the PC build produces and what example-data/refpacktest.lzc contains.

#include "LZCompress.h"

#include <cstdlib>
#include <cstring>

namespace
{

	const int kHashTableEntries = 0x10000; // GetHash() is 16 bits wide
	const int kWindowMask = 0x1FFFF;       // 128 KB window, link[] is indexed by position & mask
	const unsigned int kMaxMatch = 0x404;

} // namespace

// 3 byte hash used by the compressor.
unsigned int GetHash(const unsigned char* p)
{
	return ((p[0] << 8) | p[2]) ^ (p[1] << 4);
}

// Number of leading bytes that match, up to len.
unsigned int MatchLength(const unsigned char* a, const unsigned char* b, unsigned int len)
{
	unsigned int n = 0;
	while (n < len && *a++ == *b++)
	{
		n++;
	}
	return n;
}

static unsigned char* copyn(unsigned char* dst, const unsigned char* src, unsigned int n)
{
	do
	{
		*dst++ = *src++;
	} while (--n != 0);
	return dst;
}

// Reads the UncompressedSize out of an LZHeader.
unsigned int GetDecompressedSize(const void* compresseddata)
{
	return reinterpret_cast<const LZHeader*>(compresseddata)->UncompressedSize;
}

// Compresses sourcesize bytes from source into compresseddata (no LZHeader) and returns the
// number of bytes written.
int REFPACK_Compress(void* compresseddata, const void* source, int sourcesize)
{
	unsigned char* to = static_cast<unsigned char*>(compresseddata);
	const unsigned char* src = static_cast<const unsigned char*>(source);

	unsigned short type = 0x10FB;
	unsigned int size_bytes = 3;
	if (sourcesize >= 0x1000000)
	{
		type = 0x90FB;
		size_bytes = 4;
	}

	// bMalloc(0x100000, "refpack hashtbl") / bMalloc(0x200000, "refpack link"); only the first
	// 0x10000 / 0x20000 entries are ever touched.
	int* hashtbl = static_cast<int*>(std::malloc(0x100000));
	int* link = static_cast<int*>(std::malloc(0x200000));

	to[0] = static_cast<unsigned char>(type >> 8);
	to[1] = static_cast<unsigned char>(type);
	to += 2;
	for (unsigned int i = 0; i < size_bytes; i++)
	{
		to[i] = static_cast<unsigned char>(static_cast<unsigned int>(sourcesize) >> ((size_bytes - 1 - i) * 8));
	}
	to += size_bytes;

	std::memset(hashtbl, 0xFF, kHashTableEntries * sizeof(int)); // every head = -1

	const unsigned char* cptr = src; // current position
	const unsigned char* rptr = src; // start of the pending literal run
	unsigned int run = 0;            // pending literal count
	int len = sourcesize - 4;        // bytes left, minus the 4 that always end up as literals

	while (len >= 0)
	{
		unsigned int boffset = 0;
		unsigned int blen = 2;
		unsigned int bcost = 2;
		unsigned int mlen = len < static_cast<int>(kMaxMatch) ? static_cast<unsigned int>(len) : kMaxMatch;

		int pos = static_cast<int>(cptr - src);
		unsigned int hash = GetHash(cptr);
		int hoffset = hashtbl[hash];
		int minhoffset = pos - kWindowMask > 0 ? pos - kWindowMask : 0;

		for (int h = hoffset; h >= minhoffset; h = link[h & kWindowMask])
		{
			const unsigned char* tptr = src + h;
			if (cptr[blen] != tptr[blen])
			{
				continue;
			}

			unsigned int tlen = MatchLength(cptr, tptr, mlen);
			if (tlen <= blen)
			{
				continue;
			}

			unsigned int toffset = static_cast<unsigned int>(cptr - tptr) - 1;
			unsigned int tcost;
			if (toffset < 0x400 && tlen <= 0x0A)
			{
				tcost = 2;
			}
			else if (toffset < 0x4000 && tlen <= 0x43)
			{
				tcost = 3;
			}
			else
			{
				tcost = 4;
			}

			if (tlen - tcost + 4 > blen - bcost + 4)
			{
				blen = tlen;
				bcost = tcost;
				boffset = toffset;
				if (blen >= kMaxMatch)
				{
					break;
				}
			}
		}

		if (bcost >= blen || len < 4)
		{
			// No worthwhile match: queue a literal.
			link[pos & kWindowMask] = hoffset;
			hashtbl[hash] = pos;
			run++;
			cptr++;
			len--;
			continue;
		}

		// Flush literals in blocks of 4 until at most 3 are left; those ride along in the match
		// command.
		while (run > 3)
		{
			unsigned int tlen = run & ~3u;
			if (tlen > 0x70)
			{
				tlen = 0x70;
			}
			*to++ = static_cast<unsigned char>(0xE0 + (tlen >> 2) - 1);
			std::memcpy(to, rptr, tlen);
			rptr += tlen;
			to += tlen;
			run -= tlen;
		}

		if (bcost == 2)
		{
			to[0] = static_cast<unsigned char>(((boffset >> 8) << 5) + ((blen - 3) << 2) + run);
			to[1] = static_cast<unsigned char>(boffset);
			to += 2;
		}
		else if (bcost == 3)
		{
			to[0] = static_cast<unsigned char>(0x80 + (blen - 4));
			to[1] = static_cast<unsigned char>((run << 6) + (boffset >> 8));
			to[2] = static_cast<unsigned char>(boffset);
			to += 3;
		}
		else
		{
			to[0] = static_cast<unsigned char>(0xC0 + ((boffset >> 16) << 4) + (((blen - 5) >> 8) << 2) + run);
			to[1] = static_cast<unsigned char>(boffset >> 8);
			to[2] = static_cast<unsigned char>(boffset);
			to[3] = static_cast<unsigned char>(blen - 5);
			to += 4;
		}

		if (run != 0)
		{
			std::memcpy(to, rptr, run);
			to += run;
			run = 0;
		}

		// Hash every position the match covers.
		for (unsigned int i = 0; i < blen; i++)
		{
			unsigned int h = GetHash(cptr);
			link[pos & kWindowMask] = hashtbl[h];
			hashtbl[h] = pos;
			pos++;
			cptr++;
		}

		rptr = cptr;
		len -= static_cast<int>(blen);
	}

	// Whatever is left (at least the final 3 bytes) goes out as literals, then the EOF command
	// carries the last 0..3 of them.
	run += static_cast<unsigned int>(len + 4);
	while (run > 3)
	{
		unsigned int tlen = run & ~3u;
		if (tlen > 0x70)
		{
			tlen = 0x70;
		}
		*to++ = static_cast<unsigned char>(0xE0 + (tlen >> 2) - 1);
		std::memcpy(to, rptr, tlen);
		rptr += tlen;
		to += tlen;
		run -= tlen;
	}
	*to++ = static_cast<unsigned char>(0xFC + run);
	if (run != 0)
	{
		std::memcpy(to, rptr, run);
		to += run;
	}

	std::free(hashtbl);
	std::free(link);
	return static_cast<int>(to - static_cast<unsigned char*>(compresseddata));
}

// Decompresses a RefPack stream (no LZHeader) into dest and returns the uncompressed size stored
// in the stream. Like the game there is no bounds checking on either buffer.
unsigned int REFPACK_Decompress(void* dest, const void* compresseddata)
{
	unsigned char* ref = static_cast<unsigned char*>(dest);
	unsigned char* d = ref;
	const unsigned char* s = static_cast<const unsigned char*>(compresseddata);

	if (s == nullptr)
	{
		return 0;
	}

	unsigned int type = (s[0] << 8) + s[1];
	s += 2;

	unsigned int ulen;
	if (type & 0x8000)
	{
		if (type & 0x100)
		{
			s += 4; // skip the compressed size
		}
		ulen = (s[0] << 24) + (s[1] << 16) + (s[2] << 8) + s[3];
		s += 4;
	}
	else
	{
		if (type & 0x100)
		{
			s += 3;
		}
		ulen = (s[0] << 16) + (s[1] << 8) + s[2];
		s += 3;
	}

	for (;;)
	{
		unsigned int first = s[0];

		if (!(first & 0x80)) // 2 byte command
		{
			unsigned int second = s[1];
			s += 2;
			unsigned int run = first & 3;
			if (run != 0)
			{
				d = copyn(d, s, run);
				s += run;
			}
			const unsigned char* r = d - (((first & 0x60) << 3) + second) - 1;
			d = copyn(d, r, ((first >> 2) & 7) + 3);
			continue;
		}

		if (!(first & 0x40)) // 3 byte command
		{
			unsigned int second = s[1];
			unsigned int third = s[2];
			s += 3;
			unsigned int run = second >> 6;
			if (run != 0)
			{
				d = copyn(d, s, run);
				s += run;
			}
			const unsigned char* r = d - (((second & 0x3F) << 8) + third) - 1;
			d = copyn(d, r, (first & 0x3F) + 4);
			continue;
		}

		if (!(first & 0x20)) // 4 byte command
		{
			unsigned int second = s[1];
			unsigned int third = s[2];
			unsigned int fourth = s[3];
			s += 4;
			unsigned int run = first & 3;
			if (run != 0)
			{
				d = copyn(d, s, run);
				s += run;
			}
			const unsigned char* r = d - (((first & 0x10) << 12) + (second << 8) + third) - 1;
			d = copyn(d, r, ((first & 0x0C) << 6) + fourth + 5);
			continue;
		}

		// 1 byte command: literal block or end of stream.
		s += 1;
		unsigned int run = ((first & 0x1F) + 1) << 2;
		if (run <= 0x70)
		{
			d = copyn(d, s, run);
			s += run;
			continue;
		}

		run = first & 3;
		if (run != 0)
		{
			d = copyn(d, s, run);
		}
		break;
	}

	if (d > ref + ulen)
	{
		// bPrintf("Decompressor overran it's memory block and stomped memory.\n");
		// bPrintf("It appears that %d bytes were stomped.\n", d - ulen - ref);
		// bPrintf("Please send this output to John Harvey.\n");
		// bAssert(d <= static_cast<unsigned char *>(outputBuf) + size);
	}

	return ulen;
}
