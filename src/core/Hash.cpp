#include "Hash.h"

namespace attrib
{

	namespace
	{
		inline void Mix(uint32_t& a, uint32_t& b, uint32_t& c)
		{
			a -= b; a -= c; a ^= (c >> 13);
			b -= c; b -= a; b ^= (a << 8);
			c -= a; c -= b; c ^= (b >> 13);
			a -= b; a -= c; a ^= (c >> 12);
			b -= c; b -= a; b ^= (a << 16);
			c -= a; c -= b; c ^= (b >> 5);
			a -= b; a -= c; a ^= (c >> 3);
			b -= c; b -= a; b ^= (a << 10);
			c -= a; c -= b; c ^= (b >> 15);
		}
		inline uint32_t Le32(const unsigned char* k)
		{
			return uint32_t(k[0]) | (uint32_t(k[1]) << 8) | (uint32_t(k[2]) << 16) | (uint32_t(k[3]) << 24);
		}
		inline void Mix64(uint64_t& a, uint64_t& b, uint64_t& c)
		{
			a -= b; a -= c; a ^= (c >> 43);
			b -= c; b -= a; b ^= (a << 9);
			c -= a; c -= b; c ^= (b >> 8);
			a -= b; a -= c; a ^= (c >> 38);
			b -= c; b -= a; b ^= (a << 23);
			c -= a; c -= b; c ^= (b >> 5);
			a -= b; a -= c; a ^= (c >> 35);
			b -= c; b -= a; b ^= (a << 49);
			c -= a; c -= b; c ^= (b >> 11);
			a -= b; a -= c; a ^= (c >> 12);
			b -= c; b -= a; b ^= (a << 18);
			c -= a; c -= b; c ^= (b >> 22);
		}
		inline uint64_t Le64(const unsigned char* k)
		{
			uint64_t v = 0;
			for (int i = 7; i >= 0; --i) v = (v << 8) | k[i];
			return v;
		}
	} // namespace

	uint32_t StringHash32(std::string_view s)
	{
		const auto* k = reinterpret_cast<const unsigned char*>(s.data());
		uint32_t len = uint32_t(s.size());
		uint32_t a = 0x9E3779B9u, b = 0x9E3779B9u, c = 0xABCDEF00u;
		while (len >= 12)
		{
			a += Le32(k);
			b += Le32(k + 4);
			c += Le32(k + 8);
			Mix(a, b, c);
			k += 12;
			len -= 12;
		}
		c += uint32_t(s.size());
		switch (len)
		{
			case 11: c += uint32_t(k[10]) << 24; [[fallthrough]];
			case 10: c += uint32_t(k[9]) << 16; [[fallthrough]];
			case 9: c += uint32_t(k[8]) << 8; [[fallthrough]];
			case 8: b += uint32_t(k[7]) << 24; [[fallthrough]];
			case 7: b += uint32_t(k[6]) << 16; [[fallthrough]];
			case 6: b += uint32_t(k[5]) << 8; [[fallthrough]];
			case 5: b += k[4]; [[fallthrough]];
			case 4: a += uint32_t(k[3]) << 24; [[fallthrough]];
			case 3: a += uint32_t(k[2]) << 16; [[fallthrough]];
			case 2: a += uint32_t(k[1]) << 8; [[fallthrough]];
			case 1: a += k[0]; [[fallthrough]];
			default: break;
		}
		Mix(a, b, c);
		return c;
	}

	uint64_t StringHash64(std::string_view s)
	{
		if (s.empty()) return 0;
		const auto* k = reinterpret_cast<const unsigned char*>(s.data());
		size_t len = s.size();
		uint64_t a = 0xABCDEF0011223344ull, b = a, c = 0x9E3779B97F4A7C13ull;
		while (len >= 24)
		{
			a += Le64(k);
			b += Le64(k + 8);
			c += Le64(k + 16);
			Mix64(a, b, c);
			k += 24;
			len -= 24;
		}
		c += uint64_t(s.size());
		// The rest goes into a, b and c's upper bytes: c's low byte holds the length.
		for (size_t i = 0; i < len; ++i)
		{
			if (i < 8) a += uint64_t(k[i]) << (8 * i);
			else if (i < 16) b += uint64_t(k[i]) << (8 * (i - 8));
			else c += uint64_t(k[i]) << (8 * (i - 15));
		}
		Mix64(a, b, c);
		return c;
	}

	uint32_t BinHash(std::string_view s)
	{
		uint32_t h = 0xFFFFFFFFu;
		for (unsigned char ch : s)
			h = h * 33 + ch;
		return h;
	}

} // namespace attrib
