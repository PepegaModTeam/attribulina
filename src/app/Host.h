#pragma once

// Services the editor needs from the platform layer. Everything else (menus,
// confirmations, panels) is drawn with ImGui, so a host is a window, a
// renderer and these few calls.

#include <optional>
#include <string>
#include <vector>

namespace app
{

	struct FileFilter
	{
		std::string name;    // "Attribute packs"
		std::string pattern; // "*.bin;*.lzc"
	};

	class Host
	{
	public:
		virtual ~Host() = default;

		// Paths are UTF-8. nullopt when the user cancels.
		virtual std::optional<std::string> openFileDialog(const std::string& title, const std::vector<FileFilter>& filters) = 0;
		virtual std::optional<std::string> openFolderDialog(const std::string& title) = 0;
		virtual std::optional<std::string> saveFileDialog(const std::string& title, const std::string& suggestedPath,
														  const std::vector<FileFilter>& filters) = 0;

		virtual std::string executableDirectory() = 0;
		// Where the user's own files go (core/UserFolder.h: the program's folder
		// in portable mode, else the per-user one). The host chooses it before
		// App::setup, which takes the settings file's path from it too.
		virtual std::string userFolder()
		{
			return executableDirectory();
		}
		virtual void setWindowTitle(const std::string& title) = 0;
		// Ends the main loop after the current frame.
		virtual void quit() = 0;
		// Starts another editor window (its own process, with its own database)
		// that opens `path` (empty: nothing). False when it couldn't.
		virtual bool openNewWindow(const std::string& path)
		{
			(void)path;
			return false;
		}
		virtual bool canOpenNewWindow() const
		{
			return false;
		}
		// Decodes a picture (PNG, JPEG, BMP, ...) to 8-bit RGBA, top row first.
		// False when it can't, or the host has no decoder (nfsms setups then
		// show the file name).
		virtual bool decodeImage(const std::string& path, int& width, int& height, std::vector<unsigned char>& rgba)
		{
			(void)path;
			(void)width;
			(void)height;
			(void)rgba;
			return false;
		}
	};

} // namespace app
