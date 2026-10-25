#pragma once

// Attrib::Gen::<class> headers: C++ for reading a class's collections the way
// the game's own generated classes do (Pro Street's and Undercover's debug
// headers have their declarations, the game's code their bodies; the
// research notes are in doc/classes.md). A class over Attrib::Instance with
// its class key, its constructors, its _LayoutStruct, and for every field the
// getters the game's classes have (T& X(), bool X(T&), bool X(TAttrib<T>&),
// Num_X() for arrays) and Set_X. Code that has the game's Attrib::Instance
// and friends (an SDK made from the debug headers) compiles against it and
// reads a class the editor made with no other glue.
//
// Most Wanted's Instance takes an owner (UTL::COM::IUnknown*) in its
// constructors and GenerateUniqueKey a bool: the header follows the
// database's game.

#include "Database.h"

#include <string>

namespace attrib
{

	struct GenHeaderOptions
	{
		// Set_X for every field (they work on a dynamic Instance, after Modify).
		bool setters = true;
		// Definitions of the game structs and enums the fields use, from the type
		// schema, for code that hasn't got them (off: the header lists them).
		bool types = false;
	};

	// The header for `cls`. Static fields get no accessors (the game keeps
	// their values in a table compiled into it) and a comment instead.
	std::string GenHeader(const Database& db, const ClassDef& cls, const GenHeaderOptions& options = {});

	// How the generated code spells a table type, as the game's own classes do:
	// float, const char*, Attrib::RefSpec, UMath::Vector4, a schema type's name.
	std::string CppTypeOf(const TypeInfo* type);

	// A field or class name as a C++ identifier: "0x1234ABCD" becomes
	// Field_1234ABCD, a keyword or a name the class itself uses gets a trailing _.
	std::string CppIdentifier(const std::string& name);

} // namespace attrib
