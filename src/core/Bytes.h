#pragma once

// Byte buffers and endian-aware access. Vault data is in the platform's byte
// order (little-endian on PC/PS2, big-endian on GameCube/Wii/360), while the
// VPAK pack header is little-endian everywhere.

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace attrib
{

	using Bytes = std::vector<uint8_t>;
	using ByteSpan = std::span<const uint8_t>;

	enum class Endian : uint8_t
	{
		Little, Big
	};

	class FormatError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	inline uint16_t bswap16(uint16_t v)
	{
		return uint16_t((v >> 8) | (v << 8));
	}
	inline uint32_t bswap32(uint32_t v)
	{
		return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
	}
	inline uint64_t bswap64(uint64_t v)
	{
		return (uint64_t(bswap32(uint32_t(v))) << 32) | bswap32(uint32_t(v >> 32));
	}

	// Unchecked loads/stores; callers bounds-check (ByteReader does).
	inline uint16_t load16(const uint8_t* p, Endian e)
	{
		uint16_t v;
		std::memcpy(&v, p, 2);
		return e == Endian::Little ? v : bswap16(v);
	}
	inline uint32_t load32(const uint8_t* p, Endian e)
	{
		uint32_t v;
		std::memcpy(&v, p, 4);
		return e == Endian::Little ? v : bswap32(v);
	}
	inline uint64_t load64(const uint8_t* p, Endian e)
	{
		uint64_t v;
		std::memcpy(&v, p, 8);
		return e == Endian::Little ? v : bswap64(v);
	}
	inline void store16(uint8_t* p, uint16_t v, Endian e)
	{
		if (e == Endian::Big) v = bswap16(v);
		std::memcpy(p, &v, 2);
	}
	inline void store32(uint8_t* p, uint32_t v, Endian e)
	{
		if (e == Endian::Big) v = bswap32(v);
		std::memcpy(p, &v, 4);
	}
	inline void store64(uint8_t* p, uint64_t v, Endian e)
	{
		if (e == Endian::Big) v = bswap64(v);
		std::memcpy(p, &v, 8);
	}
	inline float loadf32(const uint8_t* p, Endian e)
	{
		uint32_t u = load32(p, e);
		float f;
		std::memcpy(&f, &u, 4);
		return f;
	}
	inline void storef32(uint8_t* p, float f, Endian e)
	{
		uint32_t u;
		std::memcpy(&u, &f, 4);
		store32(p, u, e);
	}

	// Four-character chunk ids are compared as the u32 the game reads, so 'StrE'
	// is stored as "ErtS" on little-endian platforms.
	constexpr uint32_t FourCC(const char(&s)[5])
	{
		return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
			(uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
	}
	std::string FourCCToString(uint32_t id);

	inline size_t AlignUp(size_t v, size_t a)
	{
		return a ? (v + a - 1) / a * a : v;
	}

	// Bounds-checked sequential/random reader over a byte span.
	class ByteReader
	{
	public:
		ByteReader(ByteSpan data, Endian endian) : data_(data), endian_(endian)
		{
		}

		size_t size() const
		{
			return data_.size();
		}
		Endian endian() const
		{
			return endian_;
		}
		ByteSpan data() const
		{
			return data_;
		}

		void require(size_t offset, size_t count) const
		{
			if (offset > data_.size() || count > data_.size() - offset)
				throw FormatError("read of " + std::to_string(count) + " bytes at 0x" + Hex(offset) +
								  " is past the end (size 0x" + Hex(data_.size()) + ")");
		}
		uint8_t u8(size_t off) const
		{
			require(off, 1); return data_[off];
		}
		uint16_t u16(size_t off) const
		{
			require(off, 2); return load16(data_.data() + off, endian_);
		}
		uint32_t u32(size_t off) const
		{
			require(off, 4); return load32(data_.data() + off, endian_);
		}
		uint64_t u64(size_t off) const
		{
			require(off, 8); return load64(data_.data() + off, endian_);
		}
		float f32(size_t off) const
		{
			require(off, 4); return loadf32(data_.data() + off, endian_);
		}
		ByteSpan slice(size_t off, size_t count) const
		{
			require(off, count); return data_.subspan(off, count);
		}
		// NUL-terminated string starting at off.
		std::string cstr(size_t off) const;

		static std::string Hex(size_t v);

	private:
		ByteSpan data_;
		Endian endian_;
	};

	// Appending writer with alignment helpers.
	class ByteWriter
	{
	public:
		explicit ByteWriter(Endian endian) : endian_(endian)
		{
		}

		Bytes& bytes()
		{
			return out_;
		}
		const Bytes& bytes() const
		{
			return out_;
		}
		size_t size() const
		{
			return out_.size();
		}
		Endian endian() const
		{
			return endian_;
		}

		void u8(uint8_t v)
		{
			out_.push_back(v);
		}
		void u16(uint16_t v)
		{
			size_t o = grow(2); store16(out_.data() + o, v, endian_);
		}
		void u32(uint32_t v)
		{
			size_t o = grow(4); store32(out_.data() + o, v, endian_);
		}
		void u64(uint64_t v)
		{
			size_t o = grow(8); store64(out_.data() + o, v, endian_);
		}
		void raw(ByteSpan b)
		{
			out_.insert(out_.end(), b.begin(), b.end());
		}
		void raw(const void* p, size_t n)
		{
			auto* b = static_cast<const uint8_t*>(p); out_.insert(out_.end(), b, b + n);
		}
		void cstr(std::string_view s)
		{
			raw(s.data(), s.size()); u8(0);
		}
		void fill(size_t count, uint8_t value = 0)
		{
			out_.insert(out_.end(), count, value);
		}
		void align(size_t a, uint8_t value = 0)
		{
			fill(AlignUp(out_.size(), a) - out_.size(), value);
		}

		void put32(size_t off, uint32_t v)
		{
			store32(out_.data() + off, v, endian_);
		}
		void put16(size_t off, uint16_t v)
		{
			store16(out_.data() + off, v, endian_);
		}

	private:
		size_t grow(size_t n)
		{
			size_t o = out_.size(); out_.resize(o + n); return o;
		}

		Bytes out_;
		Endian endian_;
	};

	Bytes ReadFile(const std::string& path);
	void WriteFile(const std::string& path, ByteSpan data);

} // namespace attrib
