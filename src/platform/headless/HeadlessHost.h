#pragma once

// A Host with no window: dialogs return scripted answers and frames go
// through ImGui's null backends. Used to run the editor UI in tests.

#include "app/App.h"

namespace platform
{

	class HeadlessHost : public app::Host
	{
	public:
		std::optional<std::string> nextOpenFile, nextOpenFolder, nextSaveFile;
		std::string title;
		std::string exeDir = ".";
		std::string userDir; // the user's files; empty: exeDir (portable)
		bool quitRequested = false;

		std::optional<std::string> openFileDialog(const std::string&, const std::vector<app::FileFilter>&) override;
		std::optional<std::string> openFolderDialog(const std::string&) override;
		std::optional<std::string> saveFileDialog(const std::string&, const std::string&, const std::vector<app::FileFilter>&) override;
		std::string executableDirectory() override
		{
			return exeDir;
		}
		std::string userFolder() override
		{
			return userDir.empty() ? exeDir : userDir;
		}
		void setWindowTitle(const std::string& t) override
		{
			title = t;
		}
		void quit() override
		{
			quitRequested = true;
		}
		// New windows are recorded, not started.
		std::vector<std::string> newWindows;
		bool openNewWindow(const std::string& path) override
		{
			newWindows.push_back(path);
			return true;
		}
		bool canOpenNewWindow() const override
		{
			return true;
		}
	};

	// Owns an ImGui context with the null platform and renderer.
	class HeadlessImGui
	{
	public:
		HeadlessImGui();
		~HeadlessImGui();
		HeadlessImGui(const HeadlessImGui&) = delete;
		HeadlessImGui& operator=(const HeadlessImGui&) = delete;

		// Runs `frames` complete frames of the app. With a frame hook, it is
		// called after each frame (tests use it to inspect the frame's widgets).
		void run(app::App& app, int frames);

		// Input for the next frames.
		void mouseMove(float x, float y);
		void mouseButton(int button, bool down);
		// Clicks at (x, y): move, press, release, one frame each.
		void click(app::App& app, float x, float y, int button = 0);
		// Types text into the focused widget, then runs a frame.
		void type(app::App& app, const char* utf8);
		// Presses and releases a key chord (ImGuiKey | ImGuiMod_*), one frame each.
		void key(app::App& app, int chord);

		// Called before NewFrame and after Render of every frame.
		void (*beforeFrame)() = nullptr;
		void (*afterFrame)() = nullptr;
	};

} // namespace platform
