#include "ScriptSetup.h"

#include "ValueEditors.h"

#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace app
{

	namespace sc = attrib::script;
	namespace ns = attrib::nfsms;

	void DrawScriptOptions(const std::vector<const sc::OptionDef*>& options, std::map<std::string, std::string>& values)
	{
		const float em = ImGui::GetFontSize();
		for (const sc::OptionDef* o : options)
		{
			ImGui::PushID(o->id.c_str());
			std::string& v = values[o->id];
			const std::string label = o->label.empty() ? o->id : o->label;
			switch (o->type)
			{
				case sc::OptionDef::Type::Bool:
				{
					bool on = v == "true";
					if (Checkbox(label.c_str(), &on)) v = on ? "true" : "false";
					break;
				}
				case sc::OptionDef::Type::Number:
				{
					double d = std::strtod(v.c_str(), nullptr);
					ImGui::SetNextItemWidth(em * 10);
					if (ImGui::InputDouble(label.c_str(), &d, 0, 0, "%.6g"))
					{
						if (o->min) d = std::max(d, *o->min);
						if (o->max) d = std::min(d, *o->max);
						char b[64];
						std::snprintf(b, sizeof(b), "%.9g", d);
						v = b;
					}
					break;
				}
				case sc::OptionDef::Type::Integer:
				{
					int i = std::atoi(v.c_str());
					ImGui::SetNextItemWidth(em * 10);
					if (ImGui::InputInt(label.c_str(), &i))
					{
						if (o->min) i = std::max(i, int(std::ceil(*o->min)));
						if (o->max) i = std::min(i, int(std::floor(*o->max)));
						v = std::to_string(i);
					}
					break;
				}
				case sc::OptionDef::Type::Choice:
				{
					ImGui::SetNextItemWidth(em * 14);
					if (ImGui::BeginCombo(label.c_str(), v.c_str()))
					{
						for (const std::string& c : o->choices)
							if (ImGui::Selectable(c.c_str(), c == v)) v = c;
						ImGui::EndCombo();
					}
					break;
				}
				case sc::OptionDef::Type::Text:
					ImGui::SetNextItemWidth(em * 18);
					ImGui::InputText(label.c_str(), &v);
					break;
			}
			if (!o->help.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(?)");
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o->help.c_str());
			}
			if ((o->min || o->max) && (o->type == sc::OptionDef::Type::Number || o->type == sc::OptionDef::Type::Integer))
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%s", (o->min && o->max ? std::to_string(*o->min).substr(0, 8) + " to " + std::to_string(*o->max).substr(0, 8)
										   : o->min ? "at least " + std::to_string(*o->min).substr(0, 8)
										   : "at most " + std::to_string(*o->max).substr(0, 8))
									.c_str());
			}
			ImGui::PopID();
		}
	}

	// ----------------------------------------------------------- setup images ---

	SetupImages::~SetupImages()
	{
		if (!ImGui::GetCurrentContext()) return;
		// Pictures the renderer may still hold.
		release();
		for (ImTextureData* t : retired_)
		{
			ImGui::UnregisterUserTexture(t);
			IM_DELETE(t);
		}
		retired_.clear();
	}

	void SetupImages::load(Host& host, const ns::Script& script)
	{
		release();
		for (const std::string& img : script.images)
		{
			Image si;
			si.path = img;
			std::vector<unsigned char> rgba;
			if (host.decodeImage(ns::ScriptFile(script.folder, img), si.w, si.h, rgba))
			{
				si.tex = IM_NEW(ImTextureData)();
				si.tex->Create(ImTextureFormat_RGBA32, si.w, si.h);
				std::memcpy(si.tex->GetPixels(), rgba.data(), rgba.size());
				si.tex->UseColors = true;
				ImGui::RegisterUserTexture(si.tex);
			}
			images_.push_back(si);
		}
	}

	void SetupImages::release()
	{
		for (Image& si : images_)
			if (si.tex)
			{
				si.tex->WantDestroyNextFrame = true;
				si.tex->SetStatus(ImTextureStatus_WantDestroy);
				retired_.push_back(si.tex);
			}
		images_.clear();
	}

	void SetupImages::collect()
	{
		for (auto it = retired_.begin(); it != retired_.end();)
		{
			if ((*it)->Status == ImTextureStatus_Destroyed || (*it)->Status == ImTextureStatus_WantCreate)
			{
				ImGui::UnregisterUserTexture(*it);
				IM_DELETE(*it);
				it = retired_.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void DrawNfsmsSetup(const ns::Script& s, ns::Choices& choices, const SetupImages& images)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float em = ImGui::GetFontSize();
		if (!images.images().empty())
		{
			// Pictures in a row, each fitted into 200x100 like NFS-VltEd's.
			const ImVec2 box(em * 15, em * 7.5f);
			float x = 0;
			for (const SetupImages::Image& si : images.images())
			{
				ImVec2 size = box;
				if (si.tex && si.w > 0 && si.h > 0)
				{
					const float k = std::min(box.x / float(si.w), box.y / float(si.h));
					size = ImVec2(float(si.w) * k, float(si.h) * k);
				}
				if (x > 0 && x + size.x <= ImGui::GetContentRegionAvail().x) ImGui::SameLine();
				else x = 0;
				// GetTexRef: ImTextureRef(si.tex) takes the pointer as a backend
				// texture id (its legacy void* constructor), and D3D11 crashed
				// binding the ImTextureData as a shader resource view (crash4).
				if (si.tex && si.w > 0) ImGui::Image(si.tex->GetTexRef(), size);
				else ImGui::TextDisabled("(picture %s)", si.path.c_str());
				x += ImGui::GetItemRectSize().x + style.ItemSpacing.x;
			}
			ImGui::Spacing();
		}
		if (!s.hasDescription()) ImGui::TextDisabled("The script has no description.");
		for (size_t i = 0; i < s.setup.size(); ++i)
		{
			const ns::SetupItem& item = s.setup[i];
			ImGui::PushID(int(i));
			switch (item.kind)
			{
				case ns::SetupItem::Kind::Text:
					if (item.text.find_first_not_of(" \t") == std::string::npos) ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight()));
					else ImGui::TextWrapped("%s", item.text.c_str());
					break;
				case ns::SetupItem::Kind::Checkbox:
				{
					const ns::Control& c = s.controls[size_t(item.controls[0])];
					bool on = choices.checkboxes[c.name];
					if (Checkbox(c.label.c_str(), &on)) choices.checkboxes[c.name] = on;
					break;
				}
				case ns::SetupItem::Kind::Radios:
					for (int k : item.controls)
					{
						const ns::Control& c = s.controls[size_t(k)];
						ImGui::PushID(k);
						auto it = choices.radios.find(c.group);
						if (RadioButton(c.label.c_str(), it != choices.radios.end() && it->second == c.name))
							choices.radios[c.group] = c.name;
						ImGui::PopID();
					}
					break;
			}
			ImGui::PopID();
		}
	}

} // namespace app
