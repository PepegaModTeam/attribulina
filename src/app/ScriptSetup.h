#pragma once

// What a script asks before it runs: a mod script's options and an nfsms
// script's setup. The editor's Apply Script and nfsms Setup prompts draw them
// with these, and so does the mod installer (src/installer).

#include "Host.h"

#include "core/Nfsms.h"
#include "core/Script.h"

#include "imgui.h"

#include <map>
#include <string>
#include <vector>

namespace app
{

	// One control per option: a check box, a number, a list of choices or a
	// text. `values` holds each one as script text, by the option's id.
	void DrawScriptOptions(const std::vector<const attrib::script::OptionDef*>& options, std::map<std::string, std::string>& values);

	// The pictures of an nfsms setup (ui_image) as textures, decoded by the host.
	class SetupImages
	{
	public:
		struct Image
		{
			std::string path; // as the script wrote it
			ImTextureData* tex = nullptr; // none: the host couldn't decode it
			int w = 0, h = 0;
		};

		SetupImages() = default;
		~SetupImages();
		SetupImages(const SetupImages&) = delete;
		SetupImages& operator=(const SetupImages&) = delete;

		// The script's pictures, in place of any loaded before.
		void load(Host& host, const attrib::nfsms::Script& script);
		// Lets go of the pictures. Their textures are freed once the renderer
		// has let go of them too (collect).
		void release();
		// Once a frame: frees the released textures the renderer is done with.
		void collect();
		const std::vector<Image>& images() const
		{
			return images_;
		}

	private:
		std::vector<Image> images_;
		std::vector<ImTextureData*> retired_;
	};

	// The setup as NFS-VltEd's installer lays it out: the pictures in a row,
	// then its text, check boxes and radio buttons in the script's order.
	void DrawNfsmsSetup(const attrib::nfsms::Script& script, attrib::nfsms::Choices& choices, const SetupImages& images);

} // namespace app
