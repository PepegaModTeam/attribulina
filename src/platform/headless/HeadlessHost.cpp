#include "HeadlessHost.h"

#include "imgui.h"
#include "imgui_impl_null.h"
#include "imgui_internal.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

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

	bool HeadlessHost::decodeImage(const std::string& path, int& width, int& height, std::vector<unsigned char>& rgba)
	{
		std::error_code ec;
		if (!decodeImages || !std::filesystem::is_regular_file(std::filesystem::path(std::u8string(path.begin(), path.end())), ec)) return false;
		width = 8;
		height = 4;
		rgba.assign(size_t(width) * size_t(height) * 4, 0x80);
		return true;
	}

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
		// As the D3D11 backend's shutdown: textures only this context holds go.
		for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
			if (tex->RefCount == 1 && tex->BackendUserData)
			{
				tex->SetTexID(ImTextureID_Invalid);
				tex->BackendUserData = nullptr;
				tex->SetStatus(ImTextureStatus_Destroyed);
			}
		views_.clear();
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
			if (nextDeltaTime > 0)
			{
				ImGui::GetIO().DeltaTime = nextDeltaTime;
				nextDeltaTime = 0;
			}
			ImGui_ImplNullRender_NewFrame();
			ImGui::NewFrame();
			app.frame();
			ImGui::Render();
			render();
			if (afterFrame) afterFrame();
		}
	}

	// ImGui_ImplDX11_RenderDrawData's handling of textures, with a heap int
	// standing for each shader resource view.
	void HeadlessImGui::render()
	{
		ImDrawData* draw_data = ImGui::GetDrawData();
		if (!draw_data || !draw_data->Textures) return;
		for (ImTextureData* tex : *draw_data->Textures)
		{
			if (tex->Status == ImTextureStatus_WantCreate)
			{
				views_.push_back(std::make_unique<int>(int(views_.size())));
				tex->SetTexID(ImTextureID(intptr_t(views_.back().get())));
				tex->SetStatus(ImTextureStatus_OK);
				tex->BackendUserData = views_.back().get();
			}
			else if (tex->Status == ImTextureStatus_WantUpdates)
			{
				tex->SetStatus(ImTextureStatus_OK);
			}
			if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
			{
				const int* view = static_cast<const int*>(tex->BackendUserData);
				views_.erase(std::remove_if(views_.begin(), views_.end(), [&](const std::unique_ptr<int>& v) { return v.get() == view; }), views_.end());
				tex->SetTexID(ImTextureID_Invalid);
				tex->BackendUserData = nullptr;
				tex->SetStatus(ImTextureStatus_Destroyed);
				++texturesFreed;
			}
		}
		std::unordered_set<const ImTextureData*> listed(draw_data->Textures->begin(), draw_data->Textures->end());
		std::unordered_set<intptr_t> views;
		for (const std::unique_ptr<int>& v : views_) views.insert(intptr_t(v.get()));
		const int frame = ImGui::GetFrameCount();
		for (const ImDrawList* list : draw_data->CmdLists)
			for (const ImDrawCmd& cmd : list->CmdBuffer)
			{
				if (cmd.UserCallback || cmd.ElemCount == 0) continue;
				const char* owner = list->_OwnerName ? list->_OwnerName : "?";
				if (cmd.TexRef._TexData && !listed.count(cmd.TexRef._TexData))
				{
					textureProblems.push_back("frame " + std::to_string(frame) + ": " + owner + " draws with a texture no longer listed (freed)");
					continue;
				}
				const ImTextureID id = cmd.TexRef._TexData ? cmd.TexRef._TexData->TexID : cmd.TexRef._TexID;
				if (id != ImTextureID_Invalid && !views.count(intptr_t(id)))
					textureProblems.push_back("frame " + std::to_string(frame) + ": " + owner + " draws with a freed texture view");
				else if (id == ImTextureID_Invalid)
					textureProblems.push_back("frame " + std::to_string(frame) + ": " + owner + " draws with a destroyed texture (no view)");
			}
	}

} // namespace platform
