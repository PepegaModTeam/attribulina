#pragma once

// attribcli: Attribulina on the command line. It applies mod scripts, makes
// them from two databases or from collections, and prints what a database
// holds as script text (doc/scripts.md). main_cli.cpp is the program; tests
// call Run() directly.

#include <iosfwd>
#include <string>
#include <vector>

namespace cli
{

	struct Env
	{
		std::string exeDir;  // data/ is looked for beside the program
		std::string dataDir; // instead of <exeDir>/data (or ./data)
		// user_keys.txt's folder, the editor's (core/UserFolder.h); empty: exeDir
		std::string userDir;
		std::istream* in = nullptr; // for a script named "-"
	};

	// Runs one command (args without the program name). Returns the exit code:
	// 0 done, 1 the command failed, 2 a usage mistake.
	int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, const Env& env);

} // namespace cli
