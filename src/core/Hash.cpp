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

	// ---------------------------------------------------------------- SHA-256 ---

	std::string Sha256Hex(std::string_view data)
	{
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};
		uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
		auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
		auto block = [&](const unsigned char* p)
		{
			uint32_t w[64];
			for (int i = 0; i < 16; ++i)
				w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
			for (int i = 16; i < 64; ++i)
			{
				const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
				const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
			for (int i = 0; i < 64; ++i)
			{
				const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
				const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
				hh = g;
				g = f;
				f = e;
				e = d + t1;
				d = c;
				c = b;
				b = a;
				a = t1 + t2;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
		};
		const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
		const size_t n = data.size();
		size_t i = 0;
		for (; i + 64 <= n; i += 64) block(bytes + i);
		// The last bytes, a 1 bit, zeros, and the length in bits (big-endian).
		unsigned char tail[128] = {};
		const size_t rest = n - i;
		for (size_t j = 0; j < rest; ++j) tail[j] = bytes[i + j];
		tail[rest] = 0x80;
		const size_t tailSize = rest + 1 + 8 <= 64 ? 64 : 128;
		const uint64_t bits = uint64_t(n) * 8;
		for (int j = 0; j < 8; ++j) tail[tailSize - 1 - size_t(j)] = static_cast<unsigned char>(bits >> (j * 8));
		block(tail);
		if (tailSize == 128) block(tail + 64);
		static const char* hex = "0123456789abcdef";
		std::string out;
		out.reserve(64);
		for (uint32_t v : h)
			for (int j = 28; j >= 0; j -= 4) out += hex[(v >> j) & 0xF];
		return out;
	}

} // namespace attrib
