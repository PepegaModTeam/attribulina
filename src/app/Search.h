#pragma once

// Search over a whole database: values, field names, and what references a
// collection or a class. It runs synchronously; the Search panel calls it when
// the user asks, never per frame. Only values a collection sets itself are
// searched (inherited ones are found at the collection that sets them).

#include "ValueEditors.h"

#include <string>
#include <utility>
#include <vector>

namespace app
{

	enum class SearchMode
	{
		Values, Fields, References
	};

	struct SearchQuery
	{
		SearchMode mode = SearchMode::Values;
		// Values: the value to look for (text, a name, a number, 0x...).
		// Fields: part of a field name. References: unused (see refClass).
		std::string text;
		// Values: the whole value must equal the text instead of containing it.
		// Fields: the whole field name must match.
		bool wholeValue = false;
		// Only collections of this class (0 = every class).
		uint32_t inClass = 0;
		// References: the target collection (refClass, refKey), or with refKey 0
		// the class refClass itself.
		uint32_t refClass = 0, refKey = 0;
		// Stop after this many hits (SearchResult::truncated says so).
		size_t limit = 20000;
	};

	struct SearchHit
	{
		uint32_t classKey = 0, key = 0; // the collection the hit is in
		// The field holding the value; 0 when the hit is the collection itself
		// (References: a child of the target).
		uint32_t field = 0;
		// Field name with the element and member path ("Checkpoint[3].mPosition"),
		// or "parent" for a child of the target.
		std::string where;
		std::string value; // the value as the inspector would summarize it
	};

	struct SearchResult
	{
		std::vector<SearchHit> hits; // by class name, collection name, then where
		bool truncated = false;
		size_t collectionsSearched = 0;
		// Fields: every (class, field) definition whose name matched, even where
		// no collection sets the field.
		std::vector<std::pair<uint32_t, uint32_t>> fields;
		// Set when the query can't run (no text, unknown target, ...).
		std::string error;
	};

	SearchResult RunSearch(const attrib::Database& db, const SearchQuery& q);

	// "class/collection" or "class" (names, or 0x... hashes) to a References
	// target: key is 0 for a class. False when the class isn't in the database
	// or the collection isn't in the class.
	bool ParseReferenceTarget(const attrib::Database& db, const std::string& text, uint32_t& cls, uint32_t& key);

} // namespace app
