#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace attrib
{

	// Attrib::StringHash32 / UCrc32: Bob Jenkins' lookup2 with initial value
	// 0xABCDEF00. Every AttribSys key (classes, collections, fields, type names,
	// vault dependency names, Lua handler names) is this hash.
	uint32_t StringHash32(std::string_view s);

	// bStringHash (bWare): DJB2 seeded with 0xFFFFFFFF, h = h * 33 + c. Used by
	// chunk data (models, textures, car parts) and by some attribute values.
	uint32_t BinHash(std::string_view s);

	// Attrib::StringHash64: Bob Jenkins' lookup8 with level 0xABCDEF0011223344,
	// 0 for an empty string. Early Most Wanted builds (the PS2 OPM and Xbox OXM
	// demos) key everything with it instead of StringHash32: classes,
	// collections, export types, dependency names. Attribulina doesn't read their
	// vaults yet.
	uint64_t StringHash64(std::string_view s);

	// SHA-256 (FIPS 180-4) of `data`, as 64 lowercase hex digits. Not one of the
	// games' hashes: the mod list (ModList.h) names a mod without a title by
	// its script file's, which Windows' `certutil -hashfile <file> SHA256`
	// prints too.
	std::string Sha256Hex(std::string_view data);

} // namespace attrib
