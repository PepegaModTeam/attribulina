#include "HeadlessHost.h"

#include "imgui.h"
#include "imgui_impl_null.h"
#include "imgui_internal.h"

namespace platform
{

	namespace
	{
		std::optional<std::string> Take(std::optional<std::string>& v)
		{
			std::optional<std::string> out = std::move(v);
			v.reset();
			return out;
		}
	} // namespace

	std::optional<std::string> HeadlessHost::openFileDialog(const std::string&, const std::vector<app::FileFilter>&)
	{
		return Take(nextOpenFile);
	}
	std::optional<std::string> HeadlessHost::openFolderDialog(const std::string&)
	{
		return Take(nextOpenFolder);
	}
	std::optional<std::string> HeadlessHost::saveFileDialog(const std::string&, const std::string&, const std::vector<app::FileFilter>&)
	{
		return Take(nextSaveFile);
	}

	HeadlessImGui::HeadlessImGui()
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
#ifdef IMGUI_ENABLE_TEST_ENGINE
		ImGui::GetCurrentContext()->TestEngineHookItems = true;
#endif
		ImGui_ImplNullPlatform_Init();
		ImGui_ImplNullRender_Init();
	}

	HeadlessImGui::~HeadlessImGui()
	{
		ImGui_ImplNullRender_Shutdown();
		ImGui_ImplNullPlatform_Shutdown();
		ImGui::DestroyContext();
	}

	void HeadlessImGui::mouseMove(float x, float y)
	{
		ImGui::GetIO().AddMousePosEvent(x, y);
	}
	void HeadlessImGui::mouseButton(int button, bool down)
	{
		ImGui::GetIO().AddMouseButtonEvent(button, down);
	}

	void HeadlessImGui::click(app::App& app, float x, float y, int button)
	{
		mouseMove(x, y);
		run(app, 1);
		mouseButton(button, true);
		run(app, 1);
		mouseButton(button, false);
		run(app, 1);
	}

	void HeadlessImGui::type(app::App& app, const char* utf8)
	{
		ImGui::GetIO().AddInputCharactersUTF8(utf8);
		run(app, 1);
	}

	void HeadlessImGui::key(app::App& app, int chord)
	{
		ImGuiIO& io = ImGui::GetIO();
		const ImGuiKey k = ImGuiKey(chord & ~ImGuiMod_Mask_);
		const ImGuiKey mods[] = { ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt };
		for (ImGuiKey m : mods)
			if (chord & m) io.AddKeyEvent(m, true);
		io.AddKeyEvent(k, true);
		run(app, 1);
		io.AddKeyEvent(k, false);
		for (ImGuiKey m : mods)
			if (chord & m) io.AddKeyEvent(m, false);
		run(app, 1);
	}

	void HeadlessImGui::run(app::App& app, int frames)
	{
		for (int i = 0; i < frames; ++i)
		{
			if (beforeFrame) beforeFrame();
			ImGui_ImplNullPlatform_NewFrame();
			ImGui_ImplNullRender_NewFrame();
			ImGui::NewFrame();
			app.frame();
			ImGui::Render();
			ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
			if (afterFrame) afterFrame();
		}
	}

} // namespace platform
