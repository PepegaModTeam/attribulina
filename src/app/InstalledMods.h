#pragma once

// The mods a game's attribulina_mods.txt lists (core/ModList.h), drawn as a
// table with the one picked in full below it: its script, SHA-256, options
// and the files it changed. The editor's Installed Mods panel and the mod
// installer's Installed Mods window draw it. ImGui only.

#include "core/ModList.h"

#include <vector>

namespace app
{

	class ModListView
	{
	public:
		// `list` null: the folder has none. `pending`: mods applied in this
		// session whose changes aren't saved yet (the editor's), listed after
		// the others and marked so. `height` 0: what is left of the window.
		void draw(const attrib::ModList* list, const std::vector<attrib::InstalledMod>& pending = {}, float height = 0.0f);
		void forget()
		{
			picked_ = -1;
		}
		int picked() const
		{
			return picked_;
		}

	private:
		int picked_ = -1;
	};

} // namespace app
