#pragma once

// The games and platforms AttribSys databases come from, and which one a
// database is (its Target). Every open database has one: detected from its
// files, or chosen when the files don't say. The type schema is picked by it,
// mod scripts name it (`game prostreet_ps2`), and nfsms, NFS-VltEd's format,
// runs only on the PC's files.
//
// Detection goes by the Vers chunk at the start of every vault: its u64 value
// differs between games and between versions of their data, and byte order
// tells most platforms that share one apart (Pro Street's PC and 360 files,
// its PS2 and Wii files), and a class attributes.bin has or the width of its
// bools the rest (Carbon's Wii and PS3 files, its GameCube and 360 files). A
// value from kKnownVersions (Game.cpp) that settles on one platform is taken
// as certain. One that doesn't (a pack without attributes.bin), or none (Most
// Wanted's vaults have no Vers chunk), leaves a guess from the chunk order,
// the type table and the folder names, and the editor asks.

#include "Bytes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace attrib
{

	enum class Game : uint8_t
	{
		Unknown, MostWanted, Carbon, ProStreet, Undercover, World, Nitro
	};
	enum class Platform : uint8_t
	{
		Unknown, PC, PS2, PS3, Xbox, Xbox360, GameCube, Wii
	};

	struct GameInfo
	{
		Game game;
		const char* id;   // in target ids: "prostreet"
		const char* name; // "Pro Street"
		// Where it is edited for real: a type schema, verified pointer layouts and
		// round-trip tests. Elsewhere its files are listed so they can say what
		// they are; they open with their values as raw bytes, and saving may lose
		// data.
		std::vector<Platform> supported;
		std::vector<Platform> platforms; // what it was released on
	};

	struct PlatformInfo
	{
		Platform platform;
		const char* id;   // in target ids: "x360"
		const char* name; // "Xbox 360"
		Endian endian;    // of its vault data
	};

	const std::vector<GameInfo>& Games();
	const std::vector<PlatformInfo>& Platforms();
	const GameInfo& InfoOf(Game g);         // Unknown gives an entry named "unknown"
	const PlatformInfo& InfoOf(Platform p); // likewise
	bool ReleasedOn(Game g, Platform p);
	bool SupportedOn(Game g, Platform p);
	bool SupportedAnywhere(Game g);
	// By id ("prostreet", "x360"; either case); Unknown for anything else.
	Game GameById(std::string_view id);
	Platform PlatformById(std::string_view id);

	// A game on a platform. Its id is what mod scripts and attribcli use.
	struct Target
	{
		Game game = Game::Unknown;
		Platform platform = Platform::Unknown;

		bool known() const
		{
			return game != Game::Unknown && platform != Platform::Unknown;
		}
		std::string id() const;   // "prostreet_ps2"; "" unless known()
		std::string name() const; // "Pro Street on PS2"
		bool operator==(const Target&) const = default;
	};

	// "prostreet_ps2" (either case); nothing for anything else.
	std::optional<Target> ParseTarget(std::string_view id);

	// What a mod script's `game` line names: one game on one platform
	// ("prostreet_ps2"), or the game on every platform ("prostreet_all"). Values
	// are typed by the field they go into when a script applies, so one script
	// fits every platform's layouts; only `bytes` values keep one byte order.
	struct ScriptTarget
	{
		Game game = Game::Unknown;
		Platform platform = Platform::Unknown; // Unknown: every platform

		bool allPlatforms() const
		{
			return platform == Platform::Unknown;
		}
		bool fits(const Target& t) const
		{
			return game == t.game && (allPlatforms() || platform == t.platform);
		}
		std::string name() const; // "Pro Street on PS2", "Pro Street on every platform"
	};
	// A target id or "<game>_all" (either case); nothing for anything else.
	std::optional<ScriptTarget> ParseScriptTarget(std::string_view id);

	// What a database's files say it is.
	struct Detection
	{
		Target target;      // the best guess; known() unless nothing could be read
		bool certain = false; // from a known Vers value: no need to ask
		std::string reason; // what it went by, for the prompt and the log
		std::optional<Endian> endian; // of the vault data, when it could be read
		uint32_t boolSize = 0;        // EA::Reflection::Bool in the type table, when there is one
		// Nothing but the likeliest of the platforms that fit the files (no
		// folder name or bool width settled it): a choice made earlier may know
		// better.
		bool platformWeak = false;
	};

	// From a pack file, or a folder (its attributes.bin). Never throws: a file
	// that can't be read leaves a guess from the folder names alone.
	Detection DetectTarget(const std::string& path);
	// From one pack's bytes; `path` is where they came from (its folder names
	// help a guess).
	Detection DetectTarget(const std::string& path, ByteSpan fileData);

	// Why files with this detection can't be from `t` (the byte order or the
	// bool width differs), or "".
	std::string Contradiction(const Target& t, const Detection& d);

	// One of World's betas (2009, 2010), by its Vers value: the editor reads them
	// with the final build's schema, and NFS-VltEd has a game of its own for them
	// (WB). The builds after the 2010 beta aren't: each has more of the types of
	// the final build, NFS-VltEd's W.
	bool IsWorldBetaVersion(uint64_t vers);

	// The Vers value VaultLib (the community Attribulator, OGVI) writes into
	// every vault it saves, whatever the game: World's final build's.
	constexpr uint64_t kVaultLibVers = 0x6838F8C1836A18DEull;
	// The Vers value a game's retail files carry on a platform in that byte order
	// (the first of kKnownVersions), if one is known.
	std::optional<uint64_t> RetailVersion(const Target& t, Endian e);
	// How the game's vaults order their chunks: StrN before DepN (current-gen
	// Undercover, World), and EndC chunks (World).
	bool StrNFirstIn(const Target& t);
	bool EndCIn(const Target& t);

} // namespace attrib
