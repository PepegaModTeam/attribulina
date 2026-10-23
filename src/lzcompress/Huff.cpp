// Huff.cpp - EA "HUFF" codec (Huffman with run-length escapes, 0x30FB streams).
//
// Decompiled from LZCompress.cpp in Need for Speed: Undercover (Xbox 360, SpeedDebugOpt-enUS):
//   HUFF_decompress 0x82A4F588 (static)   HUFF_decode     0x82A502E0
//   HUFF_writebits  0x82A502F0 (static)   HUFF_treechase  0x82A503B0 (static)
//   HUFF_maketree   0x82A50418 (static)   HUFF_minrep     0x82A505D8 (static)
//   HUFF_writenum   0x82A506B8 (static)   HUFF_writeexp   0x82A50830 (static)
//   HUFF_writecode  0x82A508B8 (static)   HUFF_init       0x82A50970 (static)
//   HUFF_analysis   0x82A50A98 (static)   HUFF_pack       0x82A51660 (static)
//   HUFF_packfile   0x82A51AC0 (static)   HUFF_encode     0x82A51C18
//   HUFFCompress    0x82A51D40            HUFFDecompress  0x82A51CA0
// ProStreet (ProStreet08Milestone) has the same functions at 0x82612A18..0x82615058.
//
// Stream layout (follows the 16 byte LZHeader), read MSB first in 16 bit big-endian units:
//
//   16 bits  type: 0x30FB plain, 0x32FB delta, 0x34FB double delta. 0x8000 = 32 bit sizes,
//            0x0100 = a compressed size field follows (skipped by the decoder).
//   8/16+16  uncompressed size (24 or 32 bits)
//   8 bits   clue: the byte value whose code is used as the escape code
//   num...   for code length 1, 2, ...: how many codes have that length (canonical Huffman);
//            stops once the code space is full
//   num...   the symbols in code order, each as a "leapfrog" distance to the next unused value
//   codes    data. When the clue's code is decoded, a num follows:
//              n > 0  repeat the previous output byte n times
//              n = 0  then 1 bit: 0 = an 8 bit literal follows (used for the clue byte itself),
//                                 1 = end of stream
//
//   num: "1xx" = 0..3, otherwise k zeros, a 1, then (k + 2) bits b: value = 2^(k+2) + b - 4.

#include "LZCompress.h"

#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------------------------

namespace
{

	// The bit reader HUFF_decompress inlines everywhere. "bits" is a left-aligned window holding
	// bitsleft + 16 valid bits; "bitsunshifted" is the last 32 bits read from the stream.
	struct HuffBitReader
	{
		const unsigned char* qs;
		unsigned int bitsunshifted;
		unsigned int bits;
		int bitsleft;

		explicit HuffBitReader(const unsigned char* src) : qs(src), bitsunshifted(0), bits(0), bitsleft(-16)
		{
			Refill();
		}

		void Refill()
		{
			bitsunshifted = (bitsunshifted << 16) | (qs[0] << 8) | qs[1];
			qs += 2;
			bits = bitsunshifted << -bitsleft;
			bitsleft += 16;
		}

		// n = 1..16
		unsigned int Get(int n)
		{
			unsigned int v = bits >> (32 - n);
			bits <<= n;
			bitsleft -= n;
			if (bitsleft < 0)
			{
				Refill();
			}
			return v;
		}

		// Returns the coded value + 4 (the caller subtracts 4), exactly like the game.
		unsigned int GetNum()
		{
			if (static_cast<int>(bits) < 0)
			{
				return Get(3); // 1xx
			}

			int n = 2;
			if (bits & 0xFFFF0000)
			{
				// The terminating 1 is within the window: count the zeros in place.
				do
				{
					bits <<= 1;
					n++;
				} while (static_cast<int>(bits) >= 0);
				bitsleft -= n;
				bits <<= 1;
				bitsleft += 1;
				if (bitsleft < 0)
				{
					Refill();
				}
			}
			else
			{
				// 16+ leading zeros: read one bit at a time. Note this counts the terminating 1 as
				// well, so n comes out one larger than the fast path would give (game behaviour).
				unsigned int b;
				do
				{
					b = Get(1);
					n++;
				} while (b == 0);
			}

			unsigned int hi = 0;
			unsigned int lo;
			if (n > 16)
			{
				if (n - 16 != 0)
				{
					hi = Get(n - 16);
				}
				lo = Get(16);
				lo |= hi << 16;
			}
			else
			{
				lo = Get(n);
			}
			return lo + (n < 32 ? (1u << n) : 0u);
		}
	};

} // namespace

// Decompresses a HUFF stream (no LZHeader) into unpackbuf and returns the uncompressed size from
// the stream header. There is no bounds checking, as in the game.
int HUFF_decompress(const unsigned char* packbuf, unsigned char* unpackbuf)
{
	if (packbuf == nullptr)
	{
		return 0;
	}

	HuffBitReader br(packbuf);

	unsigned int type = br.Get(16);
	unsigned int hi;
	if (type & 0x8000)
	{
		if (type & 0x100)
		{
			br.Get(16);
			br.Get(16);
		}
		hi = br.Get(16);
	}
	else
	{
		if (type & 0x100)
		{
			br.Get(8);
			br.Get(16);
		}
		hi = br.Get(8);
	}
	type &= ~0x100u;
	unsigned int ulen = (hi << 16) | br.Get(16);
	unsigned char clue = static_cast<unsigned char>(br.Get(8));

	// Canonical code tables, indexed by code length.
	//   cmptab[n]    first code *after* the length-n codes, left aligned to 16 bits (0 if none)
	//   basetab[n]   first length-n code minus the number of symbols with shorter codes
	//   bitnumtab[n] number of length-n codes
	// The game keeps 16 entries of each on the stack; extra room here only matters for
	// corrupt streams.
	unsigned int cmptab[64];
	int basetab[64];
	int bitnumtab[64];

	int numchars = 0;
	int numbits = 1;
	unsigned int nextcode = 0;
	int count;
	do
	{
		unsigned int firstcode = nextcode << 1;
		basetab[numbits] = static_cast<int>(firstcode) - numchars;
		count = static_cast<int>(br.GetNum()) - 4;
		numchars += count;
		nextcode = firstcode + count;
		unsigned int cmp = 0;
		if (count != 0)
		{
			cmp = numbits <= 16 ? ((nextcode << (16 - numbits)) & 0xFFFF) : 0;
		}
		cmptab[numbits] = cmp;
		bitnumtab[numbits] = count;
		numbits++;
	} while ((count == 0 || cmptab[numbits - 1] != 0) && numbits < 63);
	int maxbits = numbits - 1;
	cmptab[maxbits] = 0xFFFFFFFF;

	// Not in the game: a stream can't have more than 256 symbols, and the leapfrog loop below
	// would never terminate. (An empty input encodes to a single 1 bit code, which leaves the
	// code space unfilled; the game's decoder runs off into the data and spins here.)
	if (numchars > 256)
	{
		return 0;
	}

	// Symbols, in code order, coded as the number of still-unused byte values to step over.
	unsigned char leap[256];
	unsigned char symtab[400];
	std::memset(leap, 0, sizeof(leap));
	unsigned char c = 0xFF;
	for (int i = 0; i < numchars; i++)
	{
		int n = static_cast<int>(br.GetNum()) - 4 + 1;
		do
		{
			c++;
			if (leap[c] == 0)
			{
				n--;
			}
		} while (n != 0);
		symtab[i] = c;
		leap[c] = 1;
	}

	// 8 bit lookahead tables. quicklen = code length, 0x40 = longer than 8 bits,
	// 0x60 = the clue's code. quickcode reuses the leap[] buffer.
	unsigned char quicklen[256];
	unsigned char* quickcode = leap;
	std::memset(quicklen, 0x40, sizeof(quicklen));
	int cluelen = 0;
	{
		const unsigned char* sym = symtab;
		unsigned char* ql = quicklen;
		unsigned char* qc = quickcode;
		for (int bitlen = 1; bitlen <= maxbits; bitlen++)
		{
			int shift = 8 - bitlen;
			if (shift < 0)
			{
				break;
			}
			int entries = 1 << shift;
			for (int left = bitnumtab[bitlen]; left != 0; left--)
			{
				unsigned char ch = *sym++;
				unsigned char len = static_cast<unsigned char>(bitlen);
				if (ch == clue)
				{
					cluelen = bitlen;
					len = 0x60;
				}
				std::memset(ql, len, entries);
				std::memset(qc, ch, entries);
				ql += entries;
				qc += entries;
			}
		}
	}

	unsigned char* qd = unpackbuf;
	for (;;)
	{
		unsigned int idx = br.bits >> 24;
		int len = quicklen[idx];
		br.bitsleft -= len;
		if (br.bitsleft >= 0)
		{
			br.bits <<= len;
			*qd++ = quickcode[idx];
			continue;
		}

		br.bitsleft += 16;
		if (br.bitsleft >= 0)
		{
			// A short code, but the window needs refilling.
			*qd++ = quickcode[idx];
			br.bitsunshifted = (br.bitsunshifted << 16) | (br.qs[0] << 8) | br.qs[1];
			br.qs += 2;
			br.bits = br.bitsunshifted << (16 - br.bitsleft);
			continue;
		}

		// Long code or the clue: undo the speculative subtract and decode properly.
		br.bitsleft = br.bitsleft - 16 + len;
		int codelen;
		if (len == 0x60)
		{
			codelen = cluelen;
		}
		else
		{
			unsigned int top = br.bits >> 16;
			codelen = 8;
			do
			{
				codelen++;
			} while (top >= cmptab[codelen]);
		}

		unsigned int code = br.bits >> (32 - codelen);
		br.bits <<= codelen;
		br.bitsleft -= codelen;
		unsigned char ch = symtab[static_cast<int>(code) - basetab[codelen]];

		if (ch != clue)
		{
			if (br.bitsleft < 0)
			{
				br.Refill();
			}
			*qd++ = ch;
			continue;
		}

		if (br.bitsleft < 0)
		{
			br.Refill();
		}

		int run = static_cast<int>(br.GetNum()) - 4;
		if (run != 0)
		{
			unsigned char prev = qd[-1];
			unsigned char* end = qd + run;
			while (qd < end)
			{
				*qd++ = prev;
			}
			continue;
		}

		if (br.Get(1) == 0)
		{
			*qd++ = static_cast<unsigned char>(br.Get(8));
			continue;
		}
		break; // end of stream
	}

	if (type == 0x32FB || type == 0xB2FB)
	{
		unsigned char sum = 0;
		for (unsigned char* p = unpackbuf; p < unpackbuf + ulen; p++)
		{
			sum += *p;
			*p = sum;
		}
	}
	else if (type == 0x34FB || type == 0xB4FB)
	{
		unsigned char delta = 0;
		unsigned char sum = 0;
		for (unsigned char* p = unpackbuf; p < unpackbuf + ulen; p++)
		{
			delta += *p;
			sum += delta;
			*p = sum;
		}
	}

	return static_cast<int>(ulen);
}

// Decompresses a HUFF stream (no LZHeader). Returns the uncompressed size.
int HUFF_decode(void* dest, const void* compresseddata)
{
	return HUFF_decompress(static_cast<const unsigned char*>(compresseddata), static_cast<unsigned char*>(dest));
}

// ---------------------------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------------------------

struct HUFFMemStruct
{
	char* ptr;
	int len;
};

// Layout as in the PDB (0x31EC bytes on the 360).
struct HuffEncodeContext
{
	char qleapcode[256];
	unsigned int count[768]; // [0..255] symbols, [256..511] deltas, [512..] run lengths
	unsigned int bitnum[17];
	unsigned int repbits[252];
	unsigned int repbase[252];
	unsigned int tree_left[520];
	unsigned int tree_right[520];
	unsigned int bitsarray[256];
	unsigned int patternarray[256];
	unsigned int masks[17];
	unsigned int packbits;
	unsigned int workpattern;
	unsigned char* buffer;
	unsigned char* bufptr;
	int flen;
	unsigned int csum;
	unsigned int mostbits;
	unsigned int codes;
	unsigned int chainused;
	unsigned int clue;
	unsigned int dclue;
	unsigned int clues;
	unsigned int dclues;
	int mindelta;
	int maxdelta;
	unsigned int plen;
	unsigned int ulen;
	unsigned int sortptr[256];
};

// HUFF_analysis option bits. The game always passes 57 (0x39): one clue, falling back to the
// rarest byte when every value occurs; no delta codes; real Huffman lengths.
enum
{
	HUFF_OPT_CLUE = 1,           // allow a clue at all
	HUFF_OPT_MULTICLUE = 2,      // allow clue+1..clue+n as "repeat n" codes
	HUFF_OPT_DELTA = 4,          // allow delta codes
	HUFF_OPT_HUFFMAN = 8,        // variable length codes (otherwise everything is 8 bits)
	HUFF_OPT_FORCECLUE = 32,     // use the rarest byte as clue if no byte value is unused
	HUFF_GAME_OPT = 57,
	HUFF_GAME_CHAINSAW = 15,     // maximum code length
};

static const unsigned int kHuffMaxRun = 30000;
static const unsigned int kHuffNoCost = 32000;

static void HUFF_writebits(HuffEncodeContext* EC, HUFFMemStruct* dest, unsigned int bitpattern, unsigned int len)
{
	if (len > 16)
	{
		HUFF_writebits(EC, dest, bitpattern >> 16, len - 16);
		len = 16;
	}
	EC->packbits += len;
	EC->workpattern += (bitpattern & EC->masks[len]) << (24 - EC->packbits);
	while (EC->packbits > 7)
	{
		dest->ptr[dest->len] = static_cast<char>(EC->workpattern >> 16);
		++dest->len;
		EC->packbits -= 8;
		++EC->plen;
		EC->workpattern <<= 8;
	}
}

static void HUFF_treechase(HuffEncodeContext* EC, unsigned int node, unsigned int bits)
{
	while (node >= 256)
	{
		HUFF_treechase(EC, EC->tree_left[node], bits + 1);
		node = EC->tree_right[node];
		bits++;
	}
	EC->bitsarray[node] = bits;
}

// Builds a Huffman tree over count[0..255] and stores the code lengths in bitsarray (99 for
// unused symbols). The node list is unsorted; every merge scans it from the top for the two
// lightest entries (ties go to the lower index) and the last entry fills the freed slot.
static void HUFF_maketree(HuffEncodeContext* EC)
{
	unsigned int list_count[260];
	unsigned int list_node[260];
	unsigned int nodes = 256;
	unsigned int n = 1;

	list_count[0] = 0; // sentinel for the downward scans
	list_node[0] = 0;
	for (unsigned int i = 0; i < 256; i++)
	{
		EC->bitsarray[i] = 99;
		if (EC->count[i] != 0)
		{
			list_count[n] = EC->count[i];
			list_node[n] = i;
			n++;
		}
	}
	EC->codes = n - 1;

	if (n <= 2)
	{
		HUFF_treechase(EC, list_node[n - 1], 1);
		return;
	}

	unsigned int last = n - 2;
	for (unsigned int merges = n - 2; merges != 0; merges--)
	{
		unsigned int min1 = list_count[last];
		unsigned int min1i = last;
		unsigned int min2 = list_count[last + 1];
		unsigned int min2i = last + 1;
		if (min2 < min1)
		{
			min1 = list_count[last + 1];
			min1i = last + 1;
			min2 = list_count[last];
			min2i = last;
		}

		if (last != 0)
		{
			unsigned int j = last;
			for (;;)
			{
				do
				{
					j--;
				} while (list_count[j] > min2);
				if (j == 0)
				{
					break;
				}
				min2 = list_count[j];
				min2i = j;
				if (min2 <= min1)
				{
					min2 = min1;
					min2i = min1i;
					min1 = list_count[j];
					min1i = j;
				}
			}
		}

		EC->tree_left[nodes] = list_node[min2i];
		EC->tree_right[nodes] = list_node[min1i];
		list_count[min2i] = min1 + min2;
		list_node[min2i] = nodes;
		list_count[min1i] = list_count[last + 1];
		list_node[min1i] = list_node[last + 1];
		nodes++;
		last--;
	}

	HUFF_treechase(EC, nodes - 1, 0);
}

// Cheapest cost (in bits) of a run of 'remaining' repeats using the multi-clue codes clue+1..
// clue+r, falling back to clue+num for the rest.
static int HUFF_minrep(HuffEncodeContext* EC, unsigned int remaining, unsigned int r)
{
	if (r == 0)
	{
		if (remaining == 0)
		{
			return 0;
		}
		if (remaining >= 252)
		{
			return 20;
		}
		return static_cast<int>(EC->bitsarray[EC->clue] + EC->repbits[remaining] * 2 + 3);
	}

	int min = HUFF_minrep(EC, remaining, r - 1);
	if (EC->count[EC->clue + r] != 0)
	{
		unsigned int use = remaining / r;
		int min2 = HUFF_minrep(EC, remaining - use * r, r - 1) + static_cast<int>(EC->bitsarray[EC->clue + r] * use);
		if (min2 < min)
		{
			min = min2;
		}
	}
	return min;
}

// Writes num as: k zeros, a one, then k + 2 bits of (num - (2^(k+2) - 4)).
static void HUFF_writenum(HuffEncodeContext* EC, HUFFMemStruct* dest, unsigned int num)
{
	unsigned int dphufbits;
	unsigned int dphuf;

	if (num < 252)
	{
		dphufbits = EC->repbits[num];
		dphuf = EC->repbase[num];
	}
	else if (num < 0x1FC)
	{
		dphufbits = 6;
		dphuf = 0xFC;
	}
	else if (num < 0x3FC)
	{
		dphufbits = 7;
		dphuf = 0x1FC;
	}
	else if (num < 0x7FC)
	{
		dphufbits = 8;
		dphuf = 0x3FC;
	}
	else if (num < 0xFFC)
	{
		dphufbits = 9;
		dphuf = 0x7FC;
	}
	else if (num < 0x1FFC)
	{
		dphufbits = 10;
		dphuf = 0xFFC;
	}
	else if (num < 0x3FFC)
	{
		dphufbits = 11;
		dphuf = 0x1FFC;
	}
	else if (num < 0x7FFC)
	{
		dphufbits = 12;
		dphuf = 0x3FFC;
	}
	else if (num < 0xFFFC)
	{
		dphufbits = 13;
		dphuf = 0x7FFC;
	}
	else if (num < 0x1FFFC)
	{
		dphufbits = 14;
		dphuf = 0xFFFC;
	}
	else if (num < 0x3FFFC)
	{
		dphufbits = 15;
		dphuf = 0x1FFFC;
	}
	// The last three rows are off (0x80000 / 0x100000 instead of 0x7FFFC / 0xFFFFC). The encoder
	// never gets here: runs are capped at 30000 and nothing else comes close.
	else if (num < 0x80000)
	{
		dphufbits = 16;
		dphuf = 0x3FFFC;
	}
	else if (num < 0x100000)
	{
		dphufbits = 17;
		dphuf = 0x80000;
	}
	else
	{
		dphufbits = 18;
		dphuf = 0x100000;
	}

	HUFF_writebits(EC, dest, 1, dphufbits + 1);
	HUFF_writebits(EC, dest, num - dphuf, dphufbits + 2);
}

// Escape: clue code, num 0, then a 9 bit value (0..255 = literal byte, 0x100+ = end of stream).
static void HUFF_writeexp(HuffEncodeContext* EC, HUFFMemStruct* dest, unsigned int code)
{
	HUFF_writebits(EC, dest, EC->patternarray[EC->clue], EC->bitsarray[EC->clue]);
	HUFF_writenum(EC, dest, 0);
	HUFF_writebits(EC, dest, code, 9);
}

static void HUFF_writecode(HuffEncodeContext* EC, HUFFMemStruct* dest, unsigned int code)
{
	if (code == EC->clue)
	{
		HUFF_writeexp(EC, dest, code);
	}
	else
	{
		HUFF_writebits(EC, dest, EC->patternarray[code], EC->bitsarray[code]);
	}
}

static void HUFF_init(HuffEncodeContext* EC)
{
	unsigned int i = 0;
	for (; i < 4; i++)
	{
		EC->repbits[i] = 0;
		EC->repbase[i] = 0;
	}
	for (; i < 12; i++)
	{
		EC->repbits[i] = 1;
		EC->repbase[i] = 4;
	}
	for (; i < 28; i++)
	{
		EC->repbits[i] = 2;
		EC->repbase[i] = 12;
	}
	for (; i < 60; i++)
	{
		EC->repbits[i] = 3;
		EC->repbase[i] = 28;
	}
	for (; i < 124; i++)
	{
		EC->repbits[i] = 4;
		EC->repbase[i] = 60;
	}
	for (; i < 252; i++)
	{
		EC->repbits[i] = 5;
		EC->repbase[i] = 124;
	}
}

// Collects statistics, picks the clue, builds the code (limited to 'chainsaw' bits) and assigns
// canonical bit patterns.
static void HUFF_analysis(HuffEncodeContext* EC, unsigned int opt, unsigned int chainsaw)
{
	unsigned int i;
	unsigned int threshold = 0;
	unsigned int cnt[256];

	// Pass 1: symbol, delta and run statistics.
	std::memset(EC->count, 0, sizeof(EC->count));
	{
		unsigned char* p = EC->buffer;
		unsigned int prev = 256;
		unsigned int csum = 0;
		while (p < EC->bufptr)
		{
			unsigned int c = *p++;
			csum += c;
			if (c == prev)
			{
				unsigned char* limit = p + kHuffMaxRun;
				if (limit > EC->bufptr)
				{
					limit = EC->bufptr;
				}
				unsigned int run = 0;
				while (p < limit)
				{
					c = *p++;
					run++;
					csum += c;
					if (c != prev)
					{
						break;
					}
				}
				if (run < 255)
				{
					EC->count[512 + run]++;
				}
				else
				{
					EC->count[512]++;
				}
			}
			EC->count[c]++;
			EC->count[256 + ((c - prev) & 0xFF)]++;
			prev = c;
		}
		EC->csum = csum;
	}

	if (EC->count[512] == 0)
	{
		EC->count[512] = 1;
	}

	// Clue candidates: the longest run of unused byte values becomes the clue, the second
	// longest the delta clue. Later runs win ties.
	unsigned int minindex = 0;
	EC->clues = 0;
	EC->dclues = 0;
	for (i = 0; i < 256; i++)
	{
		unsigned int start = i;
		unsigned int zeros = 0;
		if (EC->count[i] < EC->count[minindex])
		{
			minindex = i;
		}
		if (EC->count[i] == 0)
		{
			while (i < 256)
			{
				zeros++;
				i++;
				if (EC->count[i] != 0)
				{
					break;
				}
			}
		}
		if (zeros >= EC->dclues)
		{
			EC->dclues = zeros;
			EC->dclue = start;
			if (zeros >= EC->clues)
			{
				EC->dclues = EC->clues;
				EC->dclue = EC->clue;
				EC->clues = zeros;
				EC->clue = start;
			}
		}
	}

	if ((opt & HUFF_OPT_FORCECLUE) && EC->clues == 0)
	{
		EC->clue = minindex;
		EC->clues = 1;
	}
	if (!(opt & HUFF_OPT_MULTICLUE))
	{
		if (EC->clues > 1)
		{
			EC->clues = 1;
		}
		if (!(opt & HUFF_OPT_CLUE))
		{
			EC->clues = 0;
		}
	}
	if (!(opt & HUFF_OPT_DELTA))
	{
		EC->dclues = 0;
	}
	else
	{
		if (EC->dclues > 10)
		{
			unsigned int t = EC->clues;
			EC->clues = EC->dclues;
			EC->dclues = t;
			t = EC->clue;
			EC->clue = EC->dclue;
			EC->dclue = t;
		}
		if (EC->clues * 4 < EC->dclues)
		{
			EC->clues = EC->dclues >> 2;
			EC->dclues = EC->dclues - EC->clues;
			EC->clue = EC->dclue + EC->dclues;
		}
	}

	// Delta codes: dclue + 2*d - 2 for +d, dclue + 2*d - 1 for -d.
	if (EC->dclues != 0)
	{
		int half = static_cast<int>(EC->dclues) / 2;
		threshold = EC->ulen / 25;
		EC->mindelta = -half;
		EC->maxdelta = static_cast<int>(EC->dclues) - half;

		for (i = 1; i <= static_cast<unsigned int>(EC->maxdelta); i++)
		{
			if (EC->count[256 + i] > threshold)
			{
				EC->count[EC->dclue + i * 2 - 2] = EC->count[256 + i];
			}
		}
		for (i = 1; i <= static_cast<unsigned int>(-EC->mindelta); i++)
		{
			if (EC->count[512 - i] > threshold)
			{
				EC->count[EC->dclue + i * 2 - 1] = EC->count[512 - i];
			}
		}

		unsigned int top = 0;
		for (i = 0; i < EC->dclues; i++)
		{
			if (EC->count[EC->dclue + i] != 0)
			{
				top = i;
			}
		}
		unsigned int unused = EC->dclues - top - 1;
		EC->dclues -= unused;
		if (EC->clue == EC->dclue + EC->dclues + unused)
		{
			EC->clue -= unused;
			EC->clues += unused;
		}
		half = static_cast<int>(EC->dclues) / 2;
		EC->mindelta = -half;
		EC->maxdelta = static_cast<int>(EC->dclues) - half;
	}

	// Clue codes: clue = escape (weighted by the long-run count), clue+r = "repeat r".
	if (EC->clues != 0)
	{
		for (i = 0; i < EC->clues; i++)
		{
			EC->count[EC->clue + i] = EC->count[512 + i];
		}
	}

	HUFF_maketree(EC);

	// Drop repeat codes that do not pay for themselves.
	if (EC->clues > 1)
	{
		for (unsigned int r = 1; r < EC->clues; r++)
		{
			unsigned int maxr = r - 1;
			if (maxr > 8)
			{
				maxr = 8;
			}
			unsigned int index = EC->clue + r;
			unsigned int c = EC->count[index];
			if (c != 0)
			{
				unsigned int cost = static_cast<unsigned int>(HUFF_minrep(EC, r, maxr));
				unsigned int bits = EC->bitsarray[index];
				if (cost <= bits || (cost - bits) * c < (r >> 1))
				{
					EC->count[index] = 0;
				}
			}
		}
	}

	for (i = 0; i < 256; i++)
	{
		cnt[i] = EC->count[i];
		EC->count[i] = 0;
		EC->count[256 + i] = 0;
		EC->count[512 + i] = 0;
	}

	// Pass 2: count what HUFF_pack will actually emit.
	{
		unsigned char* p = EC->buffer;
		unsigned int prev = 256;
		while (p < EC->bufptr)
		{
			unsigned int c = *p++;
			if (c == prev)
			{
				unsigned char* limit = p + kHuffMaxRun;
				if (limit > EC->bufptr)
				{
					limit = EC->bufptr;
				}
				unsigned int run = 0;
				while (p < limit)
				{
					c = *p++;
					run++;
					if (c != prev)
					{
						break;
					}
				}

				unsigned int cost_literal = EC->bitsarray[prev] * run;
				unsigned int cost_clue = kHuffNoCost;
				unsigned int cost_multi = kHuffNoCost;
				if (EC->clues != 0)
				{
					if (cnt[EC->clue] != 0)
					{
						cost_clue = 20;
						if (run < 252)
						{
							cost_clue = EC->repbits[run] * 2 + EC->bitsarray[EC->clue] + 3;
						}
					}
					if (EC->clues > 1)
					{
						unsigned int rem = run;
						cost_multi = 0;
						for (unsigned int r = EC->clues - 1; r != 0; r--)
						{
							if (cnt[EC->clue + r] != 0)
							{
								unsigned int q = rem / r;
								cost_multi += q * EC->bitsarray[EC->clue + r];
								rem -= q * r;
							}
						}
						if (rem != 0)
						{
							cost_multi = kHuffNoCost;
						}
					}
				}

				if (cost_literal <= cost_clue && cost_literal <= cost_multi)
				{
					EC->count[prev] += run;
				}
				else if (cost_clue < cost_multi)
				{
					EC->count[EC->clue]++;
				}
				else if (EC->clues > 1) // the game also loops here with clues == 0; unreachable with opt 57
				{
					unsigned int rem = run;
					for (unsigned int r = EC->clues - 1; r != 0; r--)
					{
						if (cnt[EC->clue + r] != 0)
						{
							unsigned int q = rem / r;
							EC->count[EC->clue + r] += q;
							rem -= q * r;
						}
					}
				}
			}

			unsigned int index = c;
			if (EC->dclues != 0)
			{
				int delta = static_cast<int>(c) - static_cast<int>(prev);
				if (delta <= EC->maxdelta && delta >= EC->mindelta)
				{
					int dindex = static_cast<int>(EC->dclue) + delta * 2 - 2;
					if (c < prev)
					{
						dindex = static_cast<int>(EC->dclue + (prev - c) * 2) - 1;
					}
					// A run that hits the end of the buffer leaves c == prev (delta 0), giving
					// dclue - 2; with dclue < 2 the game reads before its cnt[] array. Treated as
					// "no delta code" here.
					if (dindex >= 0 && cnt[dindex] > threshold)
					{
						unsigned int use = 0;
						if (cnt[c] < 4)
						{
							use = 1;
						}
						if (EC->bitsarray[dindex] < EC->bitsarray[c])
						{
							use++;
						}
						else if (EC->bitsarray[dindex] == EC->bitsarray[c] && EC->count[dindex] > EC->count[c])
						{
							use++;
						}
						if (use != 0)
						{
							index = dindex;
						}
					}
				}
			}
			EC->count[index]++;
			prev = c;
		}
	}

	if (opt & HUFF_OPT_FORCECLUE)
	{
		EC->count[EC->clue]++; // room for the end-of-stream escape
	}

	HUFF_maketree(EC);

	// Limit code lengths: repeatedly take the deepest leaf and hang it next to the longest leaf
	// that is still shorter than the limit (its old sibling moves up a level).
	EC->chainused = 0;
	if (chainsaw < 99)
	{
		unsigned int deepest = 0;
		unsigned int second = 0;
		for (;;)
		{
			unsigned int maxbits = 0;
			for (i = 0; i < 256; i++)
			{
				if (EC->count[i] != 0 && EC->bitsarray[i] >= maxbits)
				{
					second = deepest;
					deepest = i;
					maxbits = EC->bitsarray[i];
				}
			}
			if (maxbits <= chainsaw)
			{
				break;
			}

			unsigned int best;
			for (best = 0; best < 256; best++)
			{
				if (EC->count[best] != 0 && EC->bitsarray[best] < chainsaw)
				{
					break;
				}
			}
			for (i = best; i < 256; i++)
			{
				if (EC->count[i] != 0 && EC->bitsarray[i] < chainsaw && EC->bitsarray[i] > EC->bitsarray[best])
				{
					best = i;
				}
			}

			EC->bitsarray[best]++;
			EC->bitsarray[deepest] = EC->bitsarray[best];
			EC->bitsarray[second]--;
			EC->chainused = chainsaw;
		}
	}

	if (!(opt & HUFF_OPT_HUFFMAN))
	{
		for (i = 0; i < 256; i++)
		{
			EC->bitsarray[i] = 8;
		}
	}

	std::memset(EC->bitnum, 0, sizeof(EC->bitnum));
	for (i = 0; i < 256; i++)
	{
		if (EC->bitsarray[i] <= 16)
		{
			EC->bitnum[EC->bitsarray[i]]++;
		}
	}

	// Canonical codes: symbols sorted by length, then by value.
	unsigned int n = 0;
	unsigned int mostbits = 0;
	for (unsigned int len = 1; len <= 16; len++)
	{
		if (EC->bitnum[len] != 0)
		{
			for (i = 0; i < 256; i++)
			{
				if (EC->bitsarray[i] == len)
				{
					EC->sortptr[n++] = i;
				}
			}
			mostbits = len;
		}
	}
	EC->mostbits = mostbits;

	unsigned int pattern = 0;
	unsigned int curbits = 0;
	for (i = 0; i < EC->codes; i++)
	{
		unsigned int sym = EC->sortptr[i];
		if (curbits < EC->bitsarray[sym])
		{
			pattern <<= EC->bitsarray[sym] - curbits;
			curbits = EC->bitsarray[sym];
		}
		EC->patternarray[sym] = pattern;
		pattern++;
	}
}

static void HUFF_pack(HuffEncodeContext* EC, HUFFMemStruct* dest, unsigned int /*opt*/)
{
	unsigned int i;

	// Code table: clue, number of codes per length, then the symbols as leapfrog distances.
	HUFF_writebits(EC, dest, EC->clue, 8);
	for (i = 1; i <= EC->mostbits; i++)
	{
		HUFF_writenum(EC, dest, EC->bitnum[i]);
	}

	std::memset(EC->qleapcode, 0, sizeof(EC->qleapcode));
	unsigned int last = 0xFF;
	for (i = 0; i < EC->codes; i++)
	{
		unsigned int sym = EC->sortptr[i];
		int leap = -1;
		do
		{
			last = (last + 1) & 0xFF;
			if (EC->qleapcode[last] == 0)
			{
				leap++;
			}
		} while (sym != last);
		EC->qleapcode[last] = 1;
		HUFF_writenum(EC, dest, static_cast<unsigned int>(leap));
	}

	if (EC->clues == 0)
	{
		EC->clue = 32000; // no byte matches; unreachable with opt 57
	}

	unsigned char* p = EC->buffer;
	unsigned int prev = 256;
	while (p < EC->bufptr)
	{
		unsigned int c = *p++;
		if (c == prev)
		{
			unsigned char* limit = p + kHuffMaxRun;
			if (limit > EC->bufptr)
			{
				limit = EC->bufptr;
			}
			unsigned int run = 0;
			while (p < limit)
			{
				c = *p++;
				run++;
				if (c != prev)
				{
					break;
				}
			}

			// Same decision as pass 2 of HUFF_analysis, but against the final counts.
			unsigned int cost_literal = EC->bitsarray[prev] * run;
			unsigned int cost_clue = kHuffNoCost;
			unsigned int cost_multi = kHuffNoCost;
			if (EC->clues != 0)
			{
				if (EC->count[EC->clue] != 0)
				{
					cost_clue = 20;
					if (run < 252)
					{
						cost_clue = EC->repbits[run] * 2 + EC->bitsarray[EC->clue] + 3;
					}
				}
				if (EC->clues > 1)
				{
					unsigned int rem = run;
					cost_multi = 0;
					for (unsigned int r = EC->clues - 1; r != 0; r--)
					{
						if (EC->count[EC->clue + r] != 0)
						{
							unsigned int q = rem / r;
							cost_multi += EC->bitsarray[EC->clue + r] * q;
							rem -= q * r;
						}
					}
					if (rem != 0)
					{
						cost_multi = kHuffNoCost;
					}
				}
			}

			if (cost_literal <= cost_clue && cost_literal <= cost_multi)
			{
				while (run != 0)
				{
					run--;
					HUFF_writecode(EC, dest, prev);
				}
			}
			else if (cost_clue < cost_multi)
			{
				HUFF_writebits(EC, dest, EC->patternarray[EC->clue], EC->bitsarray[EC->clue]);
				HUFF_writenum(EC, dest, run);
			}
			else if (EC->clues > 1) // see HUFF_analysis
			{
				unsigned int rem = run;
				for (unsigned int r = EC->clues - 1; r != 0; r--)
				{
					if (EC->count[EC->clue + r] != 0)
					{
						unsigned int q = rem / r;
						rem -= q * r;
						while (q != 0)
						{
							q--;
							HUFF_writecode(EC, dest, EC->clue + r);
						}
					}
				}
			}
		}

		bool written = false;
		if (EC->dclues != 0)
		{
			int delta = static_cast<int>(c) - static_cast<int>(prev);
			if (delta <= EC->maxdelta && delta >= EC->mindelta)
			{
				int dindex = static_cast<int>(EC->dclue) + delta * 2 - 2;
				if (c < prev)
				{
					dindex = static_cast<int>(EC->dclue + (prev - c) * 2) - 1;
				}
				if (dindex >= 0 && EC->bitsarray[dindex] < EC->bitsarray[c]) // see HUFF_analysis
				{
					HUFF_writebits(EC, dest, EC->patternarray[dindex], EC->bitsarray[dindex]);
					written = true;
				}
			}
		}
		prev = c;
		if (!written)
		{
			HUFF_writecode(EC, dest, c);
		}
		// (The game also tracks max(bytes read - bytes written) here and never uses it.)
	}

	// End of stream: clue, num 0, a 1 bit, then pad to the byte with zeros.
	// Without a clue (clue == 32000, only possible without HUFF_OPT_FORCECLUE) the game reads
	// patternarray[32000], far past the context, and the stream has no usable end code. Not
	// reproduced here: that code is simply left out.
	if (EC->clue < 256)
	{
		HUFF_writebits(EC, dest, EC->patternarray[EC->clue], EC->bitsarray[EC->clue]);
	}
	HUFF_writenum(EC, dest, 0);
	HUFF_writebits(EC, dest, 2, 2);
	HUFF_writebits(EC, dest, 0, 7);
}

static int HUFF_packfile(HuffEncodeContext* EC, HUFFMemStruct* infile, HUFFMemStruct* outfile, int ulen)
{
	EC->packbits = 0;
	EC->workpattern = 0;
	EC->masks[0] = 0;
	for (unsigned int i = 1; i < 17; i++)
	{
		EC->masks[i] = (EC->masks[i - 1] << 1) + 1;
	}
	HUFF_init(EC);

	EC->buffer = reinterpret_cast<unsigned char*>(infile->ptr);
	EC->flen = infile->len;
	EC->ulen = static_cast<unsigned int>(EC->flen);
	EC->bufptr = EC->buffer + EC->flen;

	outfile->len = 0;
	EC->packbits = 0;
	EC->workpattern = 0;
	EC->plen = 0;

	HUFF_analysis(EC, HUFF_GAME_OPT, HUFF_GAME_CHAINSAW);

	unsigned int size_bits;
	if (ulen > 0xFFFFFF)
	{
		if (ulen == infile->len)
		{
			HUFF_writebits(EC, outfile, 0xB0FB, 16);
		}
		else
		{
			HUFF_writebits(EC, outfile, 0xB1FB, 16);
			HUFF_writebits(EC, outfile, static_cast<unsigned int>(ulen), 32);
		}
		size_bits = 32;
	}
	else
	{
		if (ulen == infile->len)
		{
			HUFF_writebits(EC, outfile, 0x30FB, 16);
		}
		else
		{
			HUFF_writebits(EC, outfile, 0x31FB, 16);
			HUFF_writebits(EC, outfile, static_cast<unsigned int>(ulen), 24);
		}
		size_bits = 24;
	}
	HUFF_writebits(EC, outfile, static_cast<unsigned int>(infile->len), size_bits);

	HUFF_pack(EC, outfile, HUFF_GAME_OPT);
	return outfile->len;
}

// Compresses sourcesize bytes into compresseddata (no LZHeader) and returns the compressed size,
// or 0 if the context could not be allocated. compresseddata should have room for
// sourcesize * 9 / 8 + 64 or so: incompressible data grows (escaped bytes cost 12+ bits).
int HUFF_encode(void* compresseddata, const void* source, int sourcesize)
{
	int plen = 0;
	HuffEncodeContext* EC = static_cast<HuffEncodeContext*>(std::malloc(sizeof(HuffEncodeContext))); // bMalloc(0x31EC, "HuffEncodeContext")
	if (EC != nullptr)
	{
		HUFFMemStruct infile;
		HUFFMemStruct outfile;
		infile.ptr = static_cast<char*>(const_cast<void*>(source));
		infile.len = sourcesize;
		outfile.ptr = static_cast<char*>(compresseddata);
		outfile.len = sourcesize;
		plen = HUFF_packfile(EC, &infile, &outfile, sourcesize);
		std::free(EC);
	}
	return plen;
}
