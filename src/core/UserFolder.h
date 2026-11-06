#pragma once

// Where the editor and attribcli keep the user's own files: attribulina.ini,
// recent_files.txt, favorites.txt, user_keys.txt and user_lua_<game>.txt.
// The program's data/ (key names, schemas, class lists) always stays beside it.
//
// Portable mode: an attribulina.ini beside the program keeps all of them there,
// as every version before this one did (and it writes one there on exit, so an
// existing copy stays portable). Without one they go to the per-user folder:
// %APPDATA%\Attribulina on Windows (roaming), $XDG_CONFIG_HOME/attribulina or
// ~/.config/attribulina elsewhere.

#include <string>

namespace attrib
{

	inline constexpr const char* kSettingsFileName = "attribulina.ini";

	struct UserFolder
	{
		std::string path;      // UTF-8
		bool portable = false; // the program's own folder
	};

	// The program's folder when it holds an attribulina.ini, else perUserDir,
	// created if missing. The program's folder too when perUserDir is empty or
	// can't be made. Paths are UTF-8.
	UserFolder ChooseUserFolder(const std::string& exeDir, const std::string& perUserDir);

	// The platform's per-user folder for Attribulina (above); empty when the
	// platform gives none. It isn't created.
	std::string PerUserFolder();

} // namespace attrib
