// JLZ.cpp - "JDLZ" LZ77 codec.
//
// Decompiled from LZCompress.cpp in Need for Speed: Undercover (Xbox 360, SpeedDebugOpt-enUS):
//   JLZHashPool::JLZHashPool   0x82A64130     JLZHashPool::Update   0x82A4F2B0
//   JLZHashPool::~JLZHashPool  0x82A64230     JLZHashPool::FindList 0x82A4F3B0
//   JLZCompress                0x82A64268     JLZDecompress         0x82A4F3E0
//   GetTableHash 0x82A4F288, Compare 0x82A4F220, ShortMove 0x82A4F260 (static helpers)
// ProStreet (ProStreet08Milestone) has the same code at 0x82612300..0x82612A18 minus the
// bPrintf / bAssert calls.
//
// Stream layout (the 16 byte LZHeader is part of the stream, Version must be 2):
//
//   LZHeader  "JDLZ", 2, 0x10, 0, UncompressedSize, CompressedSize (incl. header)
//   u8        flags1   1 bit per token, LSB first: 0 = literal byte, 1 = match
//   u8        flags2   1 bit per *match*, LSB first: selects the match encoding
//   ...       tokens, with a fresh flag byte spliced into the stream whenever one runs out
//
//   flags2 bit = 1  "near" match, 2 bytes: b0 = LLLL OOOO, b1 = LLLLLLLL
//                    offset = (b0 & 0x0F) + 1                       1..16
//                    length = (((b0 & 0xF0) << 4) | b1) + 3         3..0x1002
//   flags2 bit = 0  "far" match, 2 bytes:  b0 = OOOL LLLL, b1 = OOOOOOOO
//                    offset = (((b0 & 0xE0) << 3) | b1) + 17        17..0x810
//                    length = (b0 & 0x1F) + 3                       3..34

#include "LZCompress.h"

#include <cstdlib>
#include <cstring>

namespace
{

	const int kHashTableSize = 0x2000; // pHead entries (0x8000 bytes)
	const int kHashPoolSize = 0x810;   // window size, matches the largest "far" offset
	const int kMaxMatch = 0x1002;      // largest "near" match
	const int kMaxFarMatch = 0x22;     // largest "far" match
	const int kMaxNearOffset = 0x10;

	// The game reads up to three bytes past the end of the source here (hashing the last two
	// positions, and the trailing literal JLZCompress always emits). Those bytes are taken as zero,
	// which is also what the shipped files contain.
	inline unsigned char ReadSrc(const unsigned char* p, int i, const unsigned char* pEnd)
	{
		return (pEnd == nullptr || p + i < pEnd) ? p[i] : 0;
	}

	inline int GetTableHash(const unsigned char* p, const unsigned char* pEnd)
	{
		unsigned int h = ((ReadSrc(p, 2, pEnd) << 4) ^ ReadSrc(p, 1, pEnd)) << 4 ^ ReadSrc(p, 0, pEnd);
		return static_cast<int>(h * 0xFFFFFE5Fu) & (kHashTableSize - 1); // h * -417
	}

	// Number of leading bytes that match, up to max_len.
	inline int Compare(const unsigned char* a, const unsigned char* b, int max_len)
	{
		int i = 0;
		while (i < max_len && a[i] == b[i])
		{
			i++;
		}
		return i;
	}

	// Byte-by-byte forward copy; source and destination may overlap (run-length style matches).
	inline void ShortMove(unsigned char* dst, const unsigned char* src, unsigned int len)
	{
		unsigned int i = 0;
		do
		{
			dst[i] = src[i];
		} while (++i < len);
	}

} // namespace

class JLZHash
{
public:
	int Offset;
	unsigned char* pData;
	JLZHash* pPrev;
	JLZHash* pNext;
};

class JLZHashPool
{
public:
	explicit JLZHashPool(int pool_size);
	~JLZHashPool();

	void Update(unsigned char* pData, int Offset);
	JLZHash* FindList(unsigned char* pData);

	// Not in the game: lets the hash ignore bytes past the end of the source instead of
	// reading them.
	const unsigned char* pEnd;

private:
	JLZHash* pPool;
	JLZHash** pHead;
	int PoolSize;
};

JLZHashPool::JLZHashPool(int pool_size)
{
	pEnd = nullptr;
	PoolSize = pool_size;
	pPool = static_cast<JLZHash*>(std::malloc(sizeof(JLZHash) * pool_size)); // bMalloc(.., "JLZHash")
	pHead = static_cast<JLZHash**>(std::malloc(sizeof(JLZHash*) * kHashTableSize));
	// bAssert(pPool != NULL); bAssert(pHead != NULL);
	std::memset(pPool, 0, sizeof(JLZHash) * PoolSize);
	std::memset(pHead, 0, sizeof(JLZHash*) * kHashTableSize);
}

JLZHashPool::~JLZHashPool()
{
	std::free(pPool);
	std::free(pHead);
}

// Inserts the position at the head of its hash bucket, recycling the pool slot that position
// maps to (the slot's previous occupant is exactly PoolSize bytes behind, so it fell out of the
// window).
void JLZHashPool::Update(unsigned char* pData, int Offset)
{
	JLZHash* hash = &pPool[Offset % PoolSize];

	if (hash->pData != nullptr)
	{
		if (hash->pNext != nullptr)
		{
			hash->pNext->pPrev = hash->pPrev;
		}
		if (hash->pPrev != nullptr)
		{
			hash->pPrev->pNext = hash->pNext;
		}
		int old_index = GetTableHash(hash->pData, pEnd);
		if (pHead[old_index] == hash)
		{
			pHead[old_index] = hash->pNext;
		}
	}

	hash->Offset = Offset;
	hash->pData = pData;
	hash->pPrev = nullptr;

	int index = GetTableHash(pData, pEnd);
	hash->pNext = pHead[index];
	pHead[index] = hash;
	if (hash->pNext != nullptr)
	{
		hash->pNext->pPrev = hash;
	}
}

JLZHash* JLZHashPool::FindList(unsigned char* pData)
{
	return pHead[GetTableHash(pData, pEnd)];
}

// Compresses SrcSize bytes into pDst (LZHeader + stream) and returns the total size written.
// Worst case output is SrcSize * 9 / 8 + 0x13ish; LZGetMaxCompressedSize() is what the game uses.
//
// Greedy parse: at every position walk the whole hash chain and keep the longest match.
// A longer match only replaces the current best if it is encodable at full length (<= 34 bytes,
// or within the 16 byte "near" window), or if the current best is short enough that a clamped
// 34 byte far match is still an improvement.
int JLZCompress(unsigned char* pSrc, int SrcSize, unsigned char* pDst)
{
	JLZHashPool hash_pool(kHashPoolSize);
	hash_pool.pEnd = pSrc + SrcSize;

	LZHeader* header = reinterpret_cast<LZHeader*>(pDst);
	header->ID = LZ_ID_JDLZ;
	header->Version = 2;
	header->HeaderSize = 0x10;
	header->Flags = 0;
	header->UncompressedSize = SrcSize;
	header->CompressedSize = 0;

	// Each flag accumulator starts as 0xFF00 and shifts right once per bit; once the 0xFF marker
	// has shifted below bit 8 the low byte holds eight flags and gets written out.
	unsigned int flags1 = 0xFF00;
	unsigned int flags2 = 0xFF00;
	unsigned char* pFlags1 = pDst + 0x10;
	unsigned char* pFlags2 = pDst + 0x11;
	unsigned char* pOut = pDst + 0x12;

	int remaining = SrcSize;
	int position = 0;
	int token_count = 0;
	int last_percent = 0;

	// Note "remaining >= 0": the loop runs once more after the last byte and emits one extra
	// literal from pSrc[SrcSize]. The decompressor never reads it.
	while (remaining >= 0)
	{
		int max_len = remaining < kMaxMatch ? remaining : kMaxMatch;
		int best_len = 2;
		JLZHash* best = nullptr;

		for (JLZHash* hash = hash_pool.FindList(pSrc); hash != nullptr; hash = hash->pNext)
		{
			if (best_len >= kMaxMatch)
			{
				break;
			}
			int len = Compare(hash->pData, pSrc, max_len);
			if (len > best_len)
			{
				if (len <= kMaxFarMatch || position - hash->Offset < kMaxNearOffset || best_len <= kMaxFarMatch)
				{
					best_len = len;
					best = hash;
				}
			}
		}

		if (best_len > 2)
		{
			int distance = position - best->Offset - 1;
			flags1 >>= 1; // match: leave bit 7 set
			if (distance < kMaxNearOffset)
			{
				pOut[0] = static_cast<unsigned char>((((best_len - 3) >> 4) & 0xF0) | distance);
				pOut[1] = static_cast<unsigned char>(best_len - 3);
				flags2 >>= 1; // near: leave bit 7 set
			}
			else
			{
				if (best_len > kMaxFarMatch)
				{
					best_len = kMaxFarMatch;
				}
				pOut[0] = static_cast<unsigned char>((((distance - 0x10) >> 3) & 0xE0) | (best_len - 3));
				pOut[1] = static_cast<unsigned char>(distance - 0x10);
				flags2 = (flags2 >> 1) & ~0x80u; // far
			}
			pOut += 2;
			remaining -= best_len;
			do
			{
				hash_pool.Update(pSrc, position);
				pSrc++;
				position++;
			} while (--best_len != 0);
		}
		else
		{
			hash_pool.Update(pSrc, position);
			flags1 = (flags1 >> 1) & ~0x80u; // literal
			position++;
			remaining--;
			*pOut++ = ReadSrc(pSrc, 0, hash_pool.pEnd);
			pSrc++;
		}

		if ((flags1 & 0xFFFF) < 0x100)
		{
			*pFlags1 = static_cast<unsigned char>(flags1);
			pFlags1 = pOut++;
			flags1 = 0xFF00;
		}
		if ((flags2 & 0xFFFF) < 0x100)
		{
			*pFlags2 = static_cast<unsigned char>(flags2);
			pFlags2 = pOut++;
			flags2 = 0xFF00;
		}

		// Progress meter; its output was compiled out.
		token_count++;
		if ((token_count & 0x1FFF) == 0)
		{
			int percent = 10 - (remaining * 10) / SrcSize;
			if (percent != last_percent)
			{
				last_percent = percent;
			}
		}
	}

	// Flush the partial flag bytes (the unused high bits end up as 1s).
	while (flags1 & 0xFF00)
	{
		flags1 >>= 1;
	}
	*pFlags1 = static_cast<unsigned char>(flags1);
	while (flags2 & 0xFF00)
	{
		flags2 >>= 1;
	}
	*pFlags2 = static_cast<unsigned char>(flags2);

	int compressed_size = static_cast<int>(pOut - pDst);
	header->CompressedSize = compressed_size;
	return compressed_size;
}

// Decompresses a JDLZ stream (LZHeader included) into pDst and returns UncompressedSize, or 0
// if the header is not JDLZ version 2.
int JLZDecompress(unsigned char* pSrc, unsigned char* pDst)
{
	LZHeader* header = reinterpret_cast<LZHeader*>(pSrc);
	if (header->ID != LZ_ID_JDLZ)
	{
		// bPrintf("ERROR:  JLZDecompress() given data that was not LZCompressed!\n");
		return 0;
	}
	if (header->Version != 2)
	{
		// bPrintf("ERROR:  JLZDecompress() given data that is from an old version of the compressor!  You have to REBUILD!\n");
		return 0;
	}

	unsigned char* pIn = pSrc + 0x10;
	unsigned char* pOut = pDst;
	unsigned char* pEnd = pDst + header->UncompressedSize;

	unsigned int flags1 = pIn[0] | 0x100;
	unsigned int flags2 = pIn[1] | 0x100;
	pIn += 2;

	while (pOut < pEnd)
	{
		if ((flags1 & 1) == 0)
		{
			// Run of literals.
			do
			{
				*pOut++ = *pIn++;
				flags1 >>= 1;
			} while (pOut < pEnd && (flags1 & 1) == 0);

			if (flags1 == 1)
			{
				flags1 = *pIn++ | 0x100;
			}
		}
		else
		{
			unsigned int length;
			if ((flags2 & 1) != 0)
			{
				unsigned int offset = (pIn[0] & 0x0F) + 1;
				length = (((pIn[0] & 0xF0) << 4) | pIn[1]) + 3;
				ShortMove(pOut, pOut - offset, length);
			}
			else
			{
				unsigned int offset = (((pIn[0] & 0xE0) << 3) | pIn[1]) + 0x11;
				length = (pIn[0] & 0x1F) + 3;
				ShortMove(pOut, pOut - offset, length);
			}

			flags1 >>= 1;
			pIn += 2;
			pOut += length;

			if (flags1 == 1)
			{
				flags1 = *pIn++ | 0x100;
			}
			flags2 >>= 1;
			if (flags2 == 1)
			{
				flags2 = *pIn++ | 0x100;
			}
		}
	}

	return static_cast<int>(header->UncompressedSize);
}
