#include "Pack.h"

#include <cstring>

namespace attrib
{

	std::optional<VaultFileHeader> ParseVaultFileHeader(ByteSpan data)
	{
		constexpr size_t kName = 0x2C;
		if (data.size() < 0x40) return std::nullopt;
		const ByteReader r(data, Endian::Little);
		VaultFileHeader h;
		h.binOffset = r.u32(kName);
		h.binSize = r.u32(kName + 4);
		h.vltOffset = r.u32(kName + 8);
		h.vltSize = r.u32(kName + 12);
		h.fileSize = r.u32(kName + 16);
		if (h.binOffset != 0x40 || h.vltOffset < uint64_t(h.binOffset) + h.binSize || h.fileSize != data.size() ||
			uint64_t(h.vltOffset) + h.vltSize > data.size())
			return std::nullopt;
		size_t n = 0;
		while (n < kName && data[n]) ++n;
		if (n == 0 || n == kName) return std::nullopt;
		for (size_t i = 0; i < n; ++i)
			if (data[i] < 0x20 || data[i] >= 0x7F) return std::nullopt;
		for (size_t i = n; i < kName; ++i)
			if (data[i]) return std::nullopt;
		h.name.assign(reinterpret_cast<const char*>(data.data()), n);
		return h;
	}

	bool IsPackData(ByteSpan data)
	{
		return (data.size() >= 4 && std::memcmp(data.data(), "VPAK", 4) == 0) || ParseVaultFileHeader(data).has_value();
	}

	PackFile ReadPack(ByteSpan fileData)
	{
		PackFile pack;
		pack.fileSize = fileData.size();

		Bytes unwrapped;
		ByteSpan data = fileData;
		if (auto lz = ParseLzHeader(fileData))
		{
			unwrapped = Decompress(fileData);
			pack.compression = lz->kind;
			data = unwrapped;
		}

		ByteReader r(data, Endian::Little);
		if (!pack.compression)
			if (const std::optional<VaultFileHeader> h = ParseVaultFileHeader(data))
			{
				pack.format = PackFormat::VaultFile;
				PackVault v;
				v.name = h->name;
				auto bin = r.slice(h->binOffset, h->binSize);
				auto vlt = r.slice(h->vltOffset, h->vltSize);
				v.bin.assign(bin.begin(), bin.end());
				v.vlt.assign(vlt.begin(), vlt.end());
				v.binOffset = h->binOffset;
				v.vltOffset = h->vltOffset;
				pack.vaults.push_back(std::move(v));
				return pack;
			}
		if (data.size() < 0x10 || std::memcmp(data.data(), "VPAK", 4) != 0)
			throw FormatError("not a VPAK attribute pack");
		const uint32_t count = r.u32(4);
		const uint32_t strOff = r.u32(8);
		const uint32_t strSize = r.u32(12);
		r.require(strOff, strSize);
		r.require(0x10, size_t(count) * 0x14);

		pack.vaults.reserve(count);
		if (strOff % 0x40) pack.align = 16;
		for (uint32_t i = 0; i < count; ++i)
		{
			const size_t e = 0x10 + size_t(i) * 0x14;
			const uint32_t nameOff = r.u32(e);
			const uint32_t binSize = r.u32(e + 4), vltSize = r.u32(e + 8);
			const uint32_t binOff = r.u32(e + 12), vltOff = r.u32(e + 16);
			if (nameOff >= strSize)
				throw FormatError("vault " + std::to_string(i) + " name is outside the string block");
			PackVault v;
			v.name = r.cstr(strOff + nameOff);
			auto bin = r.slice(binOff, binSize);
			auto vlt = r.slice(vltOff, vltSize);
			v.bin.assign(bin.begin(), bin.end());
			v.vlt.assign(vlt.begin(), vlt.end());
			v.binOffset = binOff;
			v.vltOffset = vltOff;
			if (binOff % 0x40 || vltOff % 0x40) pack.align = 16;
			pack.vaults.push_back(std::move(v));
		}
		return pack;
	}

} // namespace attrib
