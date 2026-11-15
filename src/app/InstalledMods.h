#pragma once

// The mods a game's attribulina_mods.txt lists (core/ModList.h), drawn as a
// table with the one picked in full below it: its script, SHA-256, options,
// the files it changed and what its trace (core/ModTrace.h) sets back. The
// editor's Installed Mods panel and the mod installer's Installed Mods
// window draw it, each with its own buttons for the picked mod. ImGui only.

#include "core/ModTrace.h"

#include <memory>
#include <string>
#include <vector>

namespace app
{

	class ModListView
	{
	public:
		// `rows`: the list's mods with their traces, the ones not saved yet
		// last (TracedMod::pending, the editor's), marked so. `listed`: the
		// folder has a list. `height` 0: what is left of the window.
		void draw(const std::vector<attrib::TracedMod>& rows, bool listed, float height = 0.0f);
		void forget()
		{
			picked_ = -1;
		}
		int picked() const
		{
			return picked_;
		}
		void pick(int row)
		{
			picked_ = row;
		}

	private:
		int picked_ = -1;
		// What the picked mod's trace changes, worked out once.
		std::shared_ptr<const attrib::script::Script> describedTrace_; // held, so another at its address is never taken for it
		std::string described_;
	};

} // namespace app
