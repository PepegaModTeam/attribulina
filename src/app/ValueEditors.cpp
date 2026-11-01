#include "ValueEditors.h"

#include "core/Compression.h"
#include "core/Curve.h"
#include "core/Hash.h"
#include "core/Paint.h"
#include "core/lua/Lua.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace app
{

	using namespace attrib;

	std::string ToLower(std::string s)
	{
		for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	void CollectionIndex::rebuild(const Database& db, uint64_t modelVersion)
	{
		if (version == modelVersion) return;
		version = modelVersion;
		entries.clear();
		classes.clear();
		byClass.clear();
		const KeyNames& names = db.names();
		for (const auto& cls : db.classes())
		{
			classes.push_back(cls.get());
			const std::string cname = names.format(cls->key);
			auto& list = byClass[cls.get()];
			for (Collection* c : cls->collections)
			{
				Entry e;
				e.label = cname + "/" + names.format(c->key);
				e.lowerLabel = ToLower(e.label);
				e.collection = c;
				entries.push_back(std::move(e));
				list.push_back(c);
			}
			std::sort(list.begin(), list.end(), [&](Collection* a, Collection* b)
			{
				return ToLower(names.format(a->key)) < ToLower(names.format(b->key));
			});
		}
		std::sort(classes.begin(), classes.end(), [&](ClassDef* a, ClassDef* b)
		{
			return ToLower(names.format(a->key)) < ToLower(names.format(b->key));
		});
		std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
		{
			return a.lowerLabel < b.lowerLabel;
		});
	}



	bool ShowsAsColour(const EditEnv& env, const ValueType& vt)
	{
		if (vt.kind != TypeKind::Vector3 && vt.kind != TypeKind::Vector4) return false;
		if (env.prefs)
		{
			auto it = env.prefs->colour.find(vt.name);
			if (it != env.prefs->colour.end()) return it->second;
		}
		if (vt.hint == "colour") return true;
		const std::string n = ToLower(vt.name);
		return (n.find("colour") != std::string::npos || n.find("color") != std::string::npos) && n.find("curve") == std::string::npos;
	}

	// ----------------------------------------------------------------- helpers ---

	namespace
	{


		// A filter box at the top of an open combo; reset each time it opens.
		const std::string& ComboFilter()
		{
			static std::string filter;
			if (ImGui::IsWindowAppearing())
			{
				filter.clear();
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##filter", "search", &filter);
			return filter;
		}

		bool Matches(const std::string& lowerFilter, const std::string& text)
		{
			return lowerFilter.empty() || ToLower(text).find(lowerFilter) != std::string::npos;
		}

		// Text pointer at `off`. A missing slot is a null pointer.
		bool EditTextSlot(DataBlock& b, uint32_t off, std::string* changedText)
		{
			PtrSlot* s = b.ptrAt(off);
			std::string text = s ? s->text : std::string();
			bool changed = false;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputTextWithHint("##text", s ? "" : "(null)", &text))
			{
				EnsureStringSlot(b, off).text = text;
				changed = true;
			}
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Set to Null", nullptr, false, s != nullptr))
				{
					DropSlot(b, off);
					text.clear();
					changed = true;
				}
				ImGui::EndPopup();
			}
			if (changed && changedText) *changedText = text;
			return changed;
		}

		// A hashed key: its name when the dictionary knows it, else 0x.... Typing a
		// name stores its hash (StringHash32, or bStringHash for type_bStringHash) and
		// remembers the name; typing 0x... stores that value. An 8-byte key is an
		// early Most Wanted demo's StringHash64 (Values.h LoadKey).
		bool EditKey(EditEnv& env, uint8_t* p, bool binHash, uint32_t size = 4)
		{
			KeyNames& dict = binHash ? env.db.binNames() : env.db.names();
			const bool wide = size == 8 && !binHash;
			const uint32_t h = wide ? LoadKey(env.db, p, size, env.endian) : load32(p, env.endian);
			std::string text = h ? dict.format(h) : std::string();
			bool changed = false;
			ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("00000000").x -
											 ImGui::GetStyle().ItemSpacing.x));
			if (ImGui::InputTextWithHint("##key", "name or 0x...", &text, ImGuiInputTextFlags_EnterReturnsTrue))
			{
				uint32_t v;
				if (ParseKey(text, v, binHash))
				{
					if ((text.rfind("0x", 0) != 0 || text.size() > 10) && !text.empty())
					{
						dict.add(text);
						if (env.rememberName && !ParseKey64Name(text)) env.rememberName(text);
					}
					if (!wide)
					{
						store32(p, v, env.endian);
						changed = true;
					}
					else
					{
						changed = StoreKey(env.db, p, size, v, env.endian);
					}
				}
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Type a name and press Enter to store its %s, or enter 0x... for a raw value.",
								  binHash ? "bStringHash" : wide ? "StringHash64" : "StringHash32");
			ImGui::SameLine();
			if (wide) ImGui::TextDisabled("%016llX", static_cast<unsigned long long>(load64(p, env.endian)));
			else ImGui::TextDisabled("%08X", h);
			return changed;
		}

		// A reference: class and collection dropdowns and a Go button, like Attribulator.
		// `size` 24: an early Most Wanted demo's, with u64 keys (RefSpecAt).
		bool EditRefSpec(EditEnv& env, uint8_t* p, uint32_t size)
		{
			const KeyNames& names = env.db.names();
			const RefSpecLayout rl = RefSpecAt(size);
			const uint32_t ck = LoadKey(env.db, p + rl.cls, rl.keySize, env.endian), k = LoadKey(env.db, p + rl.col, rl.keySize, env.endian);
			auto setClass = [&](uint32_t v)
			{
				StoreKey(env.db, p + rl.cls, rl.keySize, v, env.endian);
			};
			auto setCollection = [&](uint32_t v)
			{
				StoreKey(env.db, p + rl.col, rl.keySize, v, env.endian);
			};
			ClassDef* cls = ck ? env.db.findClass(ck) : nullptr;
			Collection* target = (ck || k) ? env.db.findCollection(ck, k) : nullptr;
			const ImGuiStyle& style = ImGui::GetStyle();
			const float goW = ImGui::CalcTextSize("Go").x + style.FramePadding.x * 2;
			const float w = std::max(40.0f, (ImGui::GetContentRegionAvail().x - goW - style.ItemSpacing.x * 2) * 0.5f);
			bool changed = false;

			ImGui::SetNextItemWidth(w);
			if (ImGui::BeginCombo("##class", ck ? names.format(ck).c_str() : "(none)", ImGuiComboFlags_HeightLarge))
			{
				const std::string f = ToLower(ComboFilter());
				if (ImGui::Selectable("(none)", ck == 0))
				{
					setClass(0);
					setCollection(0);
					changed = true;
				}
				if (env.index)
					for (ClassDef* c : env.index->classes)
					{
						const std::string n = names.format(c->key);
						if (!Matches(f, n)) continue;
						ImGui::PushID(c);
						if (ImGui::Selectable(n.c_str(), c->key == ck) && c->key != ck)
						{
							setClass(c->key);
							// Keep the collection if the new class has one of that name.
							if (!env.db.findCollection(c->key, k)) setCollection(0);
							changed = true;
						}
						ImGui::PopID();
					}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(w);
			std::string preview = k ? names.format(k) : std::string("(none)");
			if ((ck || k) && !target) preview += "  (not loaded)";
			ImGui::BeginDisabled(!cls);
			if (ImGui::BeginCombo("##collection", preview.c_str(), ImGuiComboFlags_HeightLarge))
			{
				const std::string f = ToLower(ComboFilter());
				if (ImGui::Selectable("(none)", k == 0))
				{
					setCollection(0);
					changed = true;
				}
				if (env.index && cls)
				{
					auto it = env.index->byClass.find(cls);
					if (it != env.index->byClass.end())
					{
						std::vector<Collection*> hits;
						for (Collection* c : it->second)
							if (Matches(f, names.format(c->key))) hits.push_back(c);
						ImGuiListClipper clip;
						clip.Begin(int(hits.size()));
						while (clip.Step())
							for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
							{
								Collection* c = hits[i];
								ImGui::PushID(c);
								if (ImGui::Selectable(names.format(c->key).c_str(), c->key == k))
								{
									setCollection(c->key);
									changed = true;
								}
								ImGui::PopID();
							}
					}
				}
				ImGui::EndCombo();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!target || !env.navigate);
			if (ImGui::Button("Go") && target && env.navigate) env.navigate(target, false);
			if (target && env.navigate && ImGui::IsItemClicked(ImGuiMouseButton_Middle)) env.navigate(target, true);
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(target ? "Go to the referenced collection (middle-click: new tab)" : "The referenced collection isn't loaded");
			return changed;
		}

		// A reference to a collection of one class (Undercover's ClassRefSpec_<class>):
		// the collection dropdown and Go of a reference, its class fixed by the type.
		bool EditClassRefSpec(EditEnv& env, uint8_t* p, uint32_t classKey)
		{
			const KeyNames& names = env.db.names();
			const uint32_t k = load32(p, env.endian);
			ClassDef* cls = classKey ? env.db.findClass(classKey) : nullptr;
			Collection* target = k && cls ? env.db.findCollection(classKey, k) : nullptr;
			const ImGuiStyle& style = ImGui::GetStyle();
			const float goW = ImGui::CalcTextSize("Go").x + style.FramePadding.x * 2;
			bool changed = false;
			std::string preview = k ? names.format(k) : std::string("(none)");
			if (k && !target) preview += "  (not loaded)";
			ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - goW - style.ItemSpacing.x));
			if (ImGui::BeginCombo("##collection", preview.c_str(), ImGuiComboFlags_HeightLarge))
			{
				const std::string f = ToLower(ComboFilter());
				if (ImGui::Selectable("(none)", k == 0) && k)
				{
					store32(p, 0, env.endian);
					changed = true;
				}
				if (env.index && cls)
					if (auto it = env.index->byClass.find(cls); it != env.index->byClass.end())
					{
						std::vector<Collection*> hits;
						for (Collection* c : it->second)
							if (Matches(f, names.format(c->key))) hits.push_back(c);
						ImGuiListClipper clip;
						clip.Begin(int(hits.size()));
						while (clip.Step())
							for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
							{
								Collection* c = hits[i];
								ImGui::PushID(c);
								if (ImGui::Selectable(names.format(c->key).c_str(), c->key == k) && c->key != k)
								{
									store32(p, c->key, env.endian);
									changed = true;
								}
								ImGui::PopID();
							}
					}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("A collection of %s", cls ? names.format(classKey).c_str() : ("class " + names.format(classKey) + ", which isn't loaded").c_str());
			ImGui::SameLine();
			ImGui::BeginDisabled(!target || !env.navigate);
			if (ImGui::Button("Go") && target && env.navigate) env.navigate(target, false);
			if (target && env.navigate && ImGui::IsItemClicked(ImGuiMouseButton_Middle)) env.navigate(target, true);
			ImGui::EndDisabled();
			return changed;
		}

		// A gameplay reference: a GCollectionKey, or a Text holding an element's path,
		// shown as the element it names. The picker offers elements like the ones the
		// field points at elsewhere in the data (same templates), or all of them.
		bool EditGameplayRef(EditEnv& env, DataBlock& b, uint32_t off, const GameplayModel::RefField& rf, uint32_t size)
		{
			const GameplayModel& m = *env.gameplay;
			const KeyNames& names = env.db.names();
			uint8_t* p = b.bytes.data() + off;
			std::string text;
			uint32_t key;
			if (rf.isText)
			{
				const PtrSlot* s = b.ptrAt(off);
				text = s ? s->text : std::string();
				key = text.empty() ? 0 : StringHash32(text);
			}
			else
			{
				key = LoadKey(env.db, p, size, env.endian);
			}
			Collection* target = m.resolveKey(key, &rf);
			std::string preview = !key ? std::string("(none)") : rf.isText ? text : names.format(key);
			if (key && !target) preview += "  (not found)";
			const ImGuiStyle& style = ImGui::GetStyle();
			const float goW = ImGui::CalcTextSize("Go").x + style.FramePadding.x * 2;
			ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - goW - style.ItemSpacing.x));
			bool changed = false;
			auto set = [&](const Collection* c)
			{
				if (rf.isText)
				{
					if (c) EnsureStringSlot(b, off).text = names.format(c->key);
					else DropSlot(b, off);
				}
				else
				{
					StoreKey(env.db, p, size, c ? c->key : 0, env.endian);
				}
				changed = true;
			};
			if (key && !target) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.25f, 1.0f));
			const bool open = ImGui::BeginCombo("##ref", preview.c_str(), ImGuiComboFlags_HeightLarge);
			if (key && !target) ImGui::PopStyleColor();
			if (open)
			{
				static bool showAll = false;
				static std::vector<Collection*> list;
				static const void* listFor = nullptr;
				static bool listAll = false;
				const std::string f = ToLower(ComboFilter());
				ImGui::Checkbox("All elements", &showAll);
				if (ImGui::IsItemHovered())
				{
					std::string ts;
					for (size_t i = 0; i < rf.templates.size() && i < 6; ++i) ts += (i ? ", " : "") + names.format(rf.templates[i]->key);
					ImGui::SetTooltip("Off: only elements made from the templates this field points at elsewhere%s%s",
									  ts.empty() ? "" : ": ", ts.c_str());
				}
				if (ImGui::IsWindowAppearing() || listFor != &rf || listAll != showAll)
				{
					list = m.candidates(rf, showAll);
					listFor = &rf;
					listAll = showAll;
				}
				if (ImGui::Selectable("(none)", key == 0)) set(nullptr);
				std::vector<Collection*> hits;
				for (Collection* c : list)
					if (Matches(f, names.format(c->key))) hits.push_back(c);
				ImGuiListClipper clip;
				clip.Begin(int(hits.size()));
				while (clip.Step())
					for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
					{
						Collection* c = hits[size_t(i)];
						ImGui::PushID(c);
						if (ImGui::Selectable(names.format(c->key).c_str(), c == target)) set(c);
						if (c->parent)
						{
							ImGui::SameLine();
							ImGui::TextDisabled("%s", names.format(c->parent->key).c_str());
						}
						ImGui::PopID();
					}
				ImGui::EndCombo();
			}
			if (target && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s/%s%s%s", names.format(target->classKey).c_str(), names.format(target->key).c_str(),
								  target->parent ? "\ntemplate " : "", target->parent ? names.format(target->parent->key).c_str() : "");
			ImGui::SameLine();
			ImGui::BeginDisabled(!target || !env.navigate);
			if (ImGui::Button("Go") && target && env.navigate) env.navigate(target, false);
			if (target && env.navigate && ImGui::IsItemClicked(ImGuiMouseButton_Middle)) env.navigate(target, true);
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(target ? "Go to %s (middle-click: new tab)" : "%s isn't loaded", preview.c_str());
			return changed;
		}

		// Labelled float boxes (X [..] Y [..] Z [..]) across the row.
		bool EditComponents(uint8_t* p, int n, const char* const* labels, Endian e)
		{
			float v[4];
			for (int i = 0; i < n; ++i) v[i] = loadf32(p + i * 4, e);
			const ImGuiStyle& style = ImGui::GetStyle();
			float labelW = 0;
			for (int i = 0; i < n; ++i) labelW = std::max(labelW, ImGui::CalcTextSize(labels[i]).x);
			const float fieldW = std::max(30.0f, (ImGui::GetContentRegionAvail().x - n * (labelW + style.ItemInnerSpacing.x) -
												  (n - 1) * style.ItemSpacing.x) / float(n));
			bool changed = false;
			for (int i = 0; i < n; ++i)
			{
				if (i) ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(labels[i]);
				ImGui::SameLine(0, style.ItemInnerSpacing.x);
				ImGui::SetNextItemWidth(fieldW);
				ImGui::PushID(i);
				const float speed = std::max(std::abs(v[i]) * 0.005f, 0.001f);
				changed |= ImGui::DragFloat("##c", &v[i], speed, 0, 0, "%.4g");
				ImGui::PopID();
			}
			if (changed)
				for (int i = 0; i < n; ++i) storef32(p + i * 4, v[i], e);
			return changed;
		}

		// Vectors: colours get a picker, "*Quadratic" coefficients get A..D and a
		// graph (as in Attribulator), everything else X/Y/Z/W. Right-click switches
		// between colour and numbers.
		bool EditVector(EditEnv& env, uint8_t* p, const ValueType& vt, int n)
		{
			bool changed = false;
			static const char* const kXYZW[] = { "X", "Y", "Z", "W" };
			static const char* const kABCD[] = { "A", "B", "C", "D" };
			const bool colour = ShowsAsColour(env, vt);
			if (colour)
			{
				float c[4] = { 0, 0, 0, 1 };
				for (int i = 0; i < n; ++i) c[i] = loadf32(p + i * 4, env.endian);
				ImGui::SetNextItemWidth(-FLT_MIN);
				const ImGuiColorEditFlags f = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar |
					ImGuiColorEditFlags_AlphaPreviewHalf;
				changed = n == 4 ? ImGui::ColorEdit4("##colour", c, f) : ImGui::ColorEdit3("##colour", c, f);
				if (changed)
					for (int i = 0; i < n; ++i) storef32(p + i * 4, c[i], env.endian);
			}
			else if (n == 4 && ToLower(vt.name).find("quadratic") != std::string::npos)
			{
				changed = EditComponents(p, 4, kABCD, env.endian);
				float k[4];
				for (int i = 0; i < 4; ++i) k[i] = loadf32(p + i * 4, env.endian);
				float samples[48];
				for (int i = 0; i < 48; ++i)
				{
					const float t = float(i) / 47.0f;
					samples[i] = k[0] + t * (k[1] + t * (k[2] + t * k[3]));
				}
				ImGui::PlotLines("##quad", samples, 48, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 3));
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("A + B*t + C*t^2 + D*t^3 for t = 0..1");
			}
			else
			{
				changed = EditComponents(p, n, kXYZW, env.endian);
			}
			if (env.prefs && !vt.name.empty() && ImGui::BeginPopupContextItem("##vecctx"))
			{
				if (ImGui::MenuItem("Show as Colour", nullptr, colour)) env.prefs->colour[vt.name] = true;
				if (ImGui::MenuItem("Show as Numbers", nullptr, !colour)) env.prefs->colour[vt.name] = false;
				ImGui::EndPopup();
			}
			return changed;
		}

		bool EditInteger(EditEnv& env, DataBlock& b, uint32_t off, const ValueType& vt)
		{
			int64_t v = ReadInteger(b, off, vt, env.endian);
			// A 32-bit value that is a known key (StringHash32, else bStringHash)
			// shows its name.
			const std::string* known = nullptr;
			if (vt.size == 4 && v != 0 && (vt.kind == TypeKind::UInt32 || vt.kind == TypeKind::Int32) &&
				!(vt.member && vt.member->isBitfield()))
			{
				known = env.db.names().find(uint32_t(v));
				if (!known) known = env.db.binNames().find(uint32_t(v));
			}
			const float suffix = known ? ImGui::CalcTextSize(known->c_str()).x + ImGui::CalcTextSize("= ").x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
			ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - suffix));
			bool changed;
			if (vt.hint == "hex" && vt.size == 4)
			{
				// Shown as 0x1234ABCD; typed as 0x and hex digits, or as a decimal
				// integer. Not InputScalar: it scans with its display format, and
				// keeps the x of a 0x prefix in it.
				char buf[24];
				std::snprintf(buf, sizeof(buf), "0x%08X", uint32_t(v));
				changed = ImGui::InputText("##int", buf, sizeof(buf), ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsNoBlank);
				if (changed)
				{
					const std::string_view t(buf);
					auto all = [](std::string_view d, bool hex)
					{
						return !d.empty() && std::all_of(d.begin(), d.end(), [&](char c)
						{
							const unsigned char u = static_cast<unsigned char>(c);
							return (hex ? std::isxdigit(u) : std::isdigit(u)) != 0;
						});
					};
					if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
					{
						changed = t.size() <= 10 && all(t.substr(2), true);
						if (changed) v = int64_t(std::strtoul(buf + 2, nullptr, 16));
					}
					else
					{
						// A decimal integer: -2147483648 to 4294967295.
						changed = !t.empty() && all(t[0] == '-' ? t.substr(1) : t, false) && t.size() <= 11;
						const long long n = changed ? std::strtoll(buf, nullptr, 10) : 0;
						changed = changed && n >= INT32_MIN && n <= int64_t(UINT32_MAX);
						if (changed) v = int64_t(uint32_t(n));
					}
				}
			}
			else if (IsSignedKind(vt.kind))
			{
				changed = ImGui::InputScalar("##int", ImGuiDataType_S64, &v);
			}
			else
			{
				uint64_t u = uint64_t(v);
				changed = ImGui::InputScalar("##int", ImGuiDataType_U64, &u);
				v = int64_t(u);
			}
			if (known)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("= %s", known->c_str());
			}
			if (changed) WriteInteger(b, off, vt, v, env.endian);
			return changed;
		}

		bool EditEnum(EditEnv& env, DataBlock& b, uint32_t off, const ValueType& vt)
		{
			const int64_t v = ReadInteger(b, off, vt, env.endian);
			const std::string* name = vt.en ? vt.en->nameOf(v) : nullptr;
			const std::string preview = name ? *name : "(" + std::to_string(v) + ")";
			const float rawW = ImGui::GetFontSize() * 4.5f;
			bool changed = false;
			ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - rawW - ImGui::GetStyle().ItemSpacing.x));
			if (ImGui::BeginCombo("##enum", preview.c_str(), ImGuiComboFlags_HeightLarge))
			{
				const std::string f = ToLower(ComboFilter());
				if (vt.en)
					for (size_t i = 0; i < vt.en->values.size(); ++i)
					{
						const auto& [n, val] = vt.en->values[i];
						if (!Matches(f, n)) continue;
						ImGui::PushID(int(i));
						const std::string label = n + "  (" + std::to_string(val) + ")";
						if (ImGui::Selectable(label.c_str(), val == v))
						{
							WriteInteger(b, off, vt, val, env.endian);
							changed = true;
						}
						ImGui::PopID();
					}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(rawW);
			int64_t raw = v;
			if (ImGui::InputScalar("##raw", ImGuiDataType_S64, &raw))
			{
				WriteInteger(b, off, vt, raw, env.endian);
				changed = true;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Raw value");
			return changed;
		}

		bool EditHex(uint8_t* p, uint32_t n)
		{
			std::string text = HexBytes(p, n);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (!ImGui::InputText("##hex", &text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase))
				return false;
			std::vector<uint8_t> bytes;
			if (!ParseHexBytes(text, bytes) || bytes.size() != n)
				return false;
			std::memcpy(p, bytes.data(), n);
			return true;
		}

		bool EditCurve(EditEnv& env, DataBlock& b, uint32_t off, uint32_t size)
		{
			CurveValue c = ReadCurve(b, off, env.endian, size);
			bool changed = false, points = false;

			const std::vector<float> fresh = NaturalY2(c.x, c.y);
			bool stale = fresh.size() != c.y2.size();
			for (size_t k = 0; !stale && k < fresh.size(); ++k)
				stale = std::abs(fresh[k] - c.y2[k]) / std::max(1.0f, std::abs(c.y2[k])) >= 1e-3f;

			float samples[128];
			for (int k = 0; k < 128; ++k)
				samples[k] = CurveEvalReal(c, c.xMin + (c.xMax - c.xMin) * float(k) / 127.0f);
			char overlay[96];
			std::snprintf(overlay, sizeof(overlay), "%zu points, x %.4g..%.4g", c.x.size(), c.xMin, c.xMax);
			ImGui::PlotLines("##plot", samples, 128, 0, overlay, FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 5));

			float dom[2] = { c.xMin, c.xMax }, rng[2] = { c.yMin, c.yMax };
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
			if (ImGui::DragFloat2("domain", dom, 0.01f, 0, 0, "%.4g"))
			{
				c.xMin = dom[0]; c.xMax = dom[1]; changed = true;
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
			if (ImGui::DragFloat2("range", rng, 0.01f, 0, 0, "%.4g"))
			{
				c.yMin = rng[0]; c.yMax = rng[1]; changed = true;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Points are stored normalised, so changing the domain or range rescales them.");
			if (CurveAt(size).graphScale)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
				if (ImGui::DragFloat("graph scale", &c.graphScale, 0.01f, 0, 0, "%.4g")) changed = true;
			}

			const float dx = c.xMax - c.xMin, dy = c.yMax - c.yMin;
			int removeAt = -1;
			for (size_t k = 0; k < c.x.size() && k < c.y.size(); ++k)
			{
				ImGui::PushID(int(k));
				float px = c.realX(k), py = c.realY(k);
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
				if (ImGui::DragFloat("##x", &px, std::max(std::abs(dx), 1e-3f) * 0.002f, 0, 0, "x %.4g") && dx != 0)
				{
					c.x[k] = (px - c.xMin) / dx;
					points = true;
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
				if (ImGui::DragFloat("##y", &py, std::max(std::abs(dy), 1e-3f) * 0.002f, 0, 0, "y %.4g") && dy != 0)
				{
					c.y[k] = (py - c.yMin) / dy;
					points = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("x")) removeAt = int(k);
				ImGui::PopID();
			}
			if (removeAt >= 0)
			{
				c.x.erase(c.x.begin() + removeAt);
				c.y.erase(c.y.begin() + removeAt);
				points = true;
			}
			if (ImGui::SmallButton("+ point"))
			{
				if (c.x.empty())
				{
					c.x.push_back(0); c.y.push_back(0);
				}
				else if (c.x.size() == 1)
				{
					c.x.push_back(1); c.y.push_back(c.y[0]);
				}
				else
				{
					const size_t n = c.x.size();
					c.x.insert(c.x.end() - 1, (c.x[n - 2] + c.x[n - 1]) * 0.5f);
					c.y.insert(c.y.end() - 1, (c.y[n - 2] + c.y[n - 1]) * 0.5f);
				}
				points = true;
			}
			if (stale && !points)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "stored Y2 doesn't match the points");
				ImGui::SameLine();
				if (ImGui::SmallButton("Regenerate Y2")) points = true;
			}
			if (points)
			{
				// Keep X increasing; the spline needs it.
				std::vector<std::pair<float, float>> xy;
				for (size_t k = 0; k < c.x.size(); ++k) xy.emplace_back(c.x[k], c.y[k]);
				std::stable_sort(xy.begin(), xy.end(), [](auto& l, auto& r)
				{
					return l.first < r.first;
				});
				for (size_t k = 0; k < xy.size(); ++k)
				{
					c.x[k] = xy[k].first; c.y[k] = xy[k].second;
				}
				c.y2 = NaturalY2(c.x, c.y);
			}
			if (changed || points) WriteCurve(b, off, c, env.endian, size);
			return changed || points;
		}

		// A car paint (PresetRidePaint) as a colour: the swatch opens an HSV picker
		// whose hue snaps to the paint chooser's 90 steps. The members (finish,
		// swatch, kit, ...) stay editable below.
		bool EditPaint(EditEnv& env, DataBlock& b, uint32_t off)
		{
			const PaintColour pc = ReadPaint(b, off, env.endian);
			float hsv[3] = { pc.h, pc.s, pc.v };
			const ImGuiColorEditFlags f = ImGuiColorEditFlags_InputHSV | ImGuiColorEditFlags_DisplayHSV |
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha;
			const bool changed = ImGui::ColorEdit3("##paint", hsv, f);
			if (changed) WritePaint(b, off, hsv[0], std::clamp(hsv[1], 0.0f, 1.0f), std::clamp(hsv[2], 0.0f, 1.0f), env.endian);
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			const std::string finish = PaintFinish(env.db.names(), load32(b.bytes.data() + off + 8, env.endian));
			ImGui::TextDisabled("%s  H %.0f  S %.2f  V %.2f", finish.empty() ? "(no finish)" : finish.c_str(), pc.h * 360.0f, pc.s, pc.v);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Hue step %d of %d (swatch %d, kit %d)%s", pc.step, kPaintHueSteps, pc.step / kPaintStepsPerSwatch,
								  pc.step % kPaintStepsPerSwatch, pc.hueKnown ? "" : "\nmSwatch isn't a generic_paint_swatch part");
			return changed;
		}

		// Current-gen Undercover's car paint: the same picker, its hue snapping to the
		// nearest of car_paint_swatch/swatch's colours; saturation and variance are S
		// and V (Paint.h).
		bool EditSwatchPaint(EditEnv& env, DataBlock& b, uint32_t off)
		{
			const PaintSwatches& sw = *env.swatches;
			const PaintColour pc = ReadSwatchPaint(b, off, sw, env.endian);
			float hsv[3] = { pc.h, pc.s, pc.v };
			const ImGuiColorEditFlags f = ImGuiColorEditFlags_InputHSV | ImGuiColorEditFlags_DisplayHSV |
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha;
			const bool changed = ImGui::ColorEdit3("##paint", hsv, f);
			if (changed)
				WriteSwatchPaint(b, off, hsv[0], std::clamp(hsv[1], 0.0f, 1.0f), std::clamp(hsv[2], 0.0f, 1.0f), sw, env.endian);
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			const std::string finish = PaintFinish(env.db.names(), load32(b.bytes.data() + off + 8, env.endian));
			ImGui::TextDisabled("%s  swatch %d  H %.0f  S %.2f  V %.2f", finish.empty() ? "(no finish)" : finish.c_str(), pc.step,
								pc.h * 360.0f, pc.s, pc.v);
			if (ImGui::IsItemHovered())
			{
				if (size_t(pc.step) >= sw.rgb.size())
					ImGui::SetTooltip("Swatch %d is past car_paint_swatch/swatch's %zu; the game uses swatch 0.\n"
									  "Picking a hue sets one of them.", pc.step, sw.rgb.size());
				else
					ImGui::SetTooltip("Swatch %d of %zu in car_paint_swatch/swatch (rgb %06X)%s\n"
									  "A hue picks the nearest swatch; S and V are mSaturation and mVariance.",
									  pc.step, sw.rgb.size(), sw.rgb[size_t(pc.step)] & 0xFFFFFFu, pc.hueKnown ? "" : ", a grey");
			}
			return changed;
		}

		// What a blob holds, worked out once per block (and again when its bytes change).
		struct BlobView
		{
			Bytes stored;          // the block as stored, to notice edits
			Bytes raw;             // unpacked (the stored bytes when not an LZHeader block)
			std::string error;     // unpacking failed
			bool lua = false;      // raw is a Lua chunk in the game's format
			bool decompiled = false, disassembled = false, dumped = false;
			std::string source, disassembly, hex;
		};

		BlobView& ViewOf(const DataBlock& d)
		{
			static std::unordered_map<const DataBlock*, BlobView> cache;
			BlobView& v = cache[&d];
			if (v.stored != d.bytes || (v.raw.empty() && !d.bytes.empty() && v.error.empty()))
			{
				v = BlobView{};
				v.stored = d.bytes;
				try
				{
					v.raw = ParseLzHeader(d.bytes) ? Decompress(d.bytes) : d.bytes;
					v.lua = lua::DetectVersion(v.raw).has_value();
				}
				catch (const std::exception& e)
				{
					v.error = e.what();
				}
			}
			return v;
		}

		std::string HexDump(const Bytes& b)
		{
			std::string out;
			out.reserve(b.size() * 4 + b.size() / 16 * 12);
			char line[96];
			for (size_t i = 0; i < b.size(); i += 16)
			{
				int n = std::snprintf(line, sizeof(line), "%06zX ", i);
				for (size_t k = 0; k < 16; ++k)
					n += i + k < b.size() ? std::snprintf(line + n, sizeof(line) - size_t(n), " %02X", b[i + k])
					: std::snprintf(line + n, sizeof(line) - size_t(n), "   ");
				n += std::snprintf(line + n, sizeof(line) - size_t(n), "  ");
				for (size_t k = 0; k < 16 && i + k < b.size(); ++k) line[n++] = (b[i + k] >= 32 && b[i + k] < 127) ? char(b[i + k]) : '.';
				line[n++] = '\n';
				out.append(line, size_t(n));
			}
			return out;
		}

		// Attrib::Blob: the stored block, what it unpacks to, and for gameplay's
		// compiled Lua the decompiled source and a disassembly.
		bool EditBlob(EditEnv& env, DataBlock& b, uint32_t off)
		{
			uint8_t* p = b.bytes.data() + off;
			PtrSlot* s = b.ptrAt(off + 4);
			const uint32_t size = load32(p, env.endian);
			if (!s || !s->data)
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("empty");
				return false;
			}
			BlobView& v = ViewOf(*s->data);
			const auto lz = ParseLzHeader(s->data->bytes);
			// Retail Carbon stores a size short of the data for most handlers; the game goes by the LZHeader.
			const size_t have = s->data->bytes.size();
			const std::string stored = size == have ? "" : " (stored size " + std::to_string(size) + ")";
			ImGui::AlignTextToFramePadding();
			if (lz)
				ImGui::TextDisabled("%zu bytes%s, %s packed, %zu unpacked%s", have, stored.c_str(), LzKindName(lz->kind), v.raw.size(),
									v.lua ? ", compiled Lua" : "");
			else
				ImGui::TextDisabled("%zu bytes%s%s", have, stored.c_str(), v.lua ? ", compiled Lua" : "");
			if (env.openScript && v.lua)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("State Graph Builder")) env.openScript();
			}
			if (!v.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.38f, 1.0f), "%s", v.error.c_str());
			if (!ImGui::TreeNodeEx("contents", ImGuiTreeNodeFlags_SpanAvailWidth)) return false;
			bool changed = false;
			const ImVec2 box(-FLT_MIN, ImGui::GetTextLineHeight() * 16);
			if (ImGui::BeginTabBar("##blob"))
			{
				if (v.lua && ImGui::BeginTabItem("Lua"))
				{
					if (!v.decompiled)
					{
						v.decompiled = true;
						try
						{
							lua::DecompileResult r = lua::Decompile(v.raw);
							v.source = r.source;
							for (const auto& w : r.warnings) v.source += "-- decompiler: " + w + "\n";
						}
						catch (const std::exception& e)
						{
							v.source = std::string("-- could not decompile: ") + e.what() + "\n";
						}
					}
					ImGui::InputTextMultiline("##lua", &v.source, box, ImGuiInputTextFlags_ReadOnly);
					ImGui::EndTabItem();
				}
				if (v.lua && ImGui::BeginTabItem("Disassembly"))
				{
					if (!v.disassembled)
					{
						v.disassembled = true;
						try
						{
							v.disassembly = lua::Disassemble(v.raw);
						}
						catch (const std::exception& e)
						{
							v.disassembly = e.what();
						}
					}
					ImGui::InputTextMultiline("##asm", &v.disassembly, box, ImGuiInputTextFlags_ReadOnly);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(lz ? "Unpacked" : "Bytes"))
				{
					if (!v.dumped)
					{
						v.dumped = true;
						v.hex = HexDump(v.raw);
					}
					ImGui::InputTextMultiline("##hexdump", &v.hex, box, ImGuiInputTextFlags_ReadOnly);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Stored"))
				{
					// The stored block as editable hex (Enter applies).
					std::string text = HexBytes(s->data->bytes.data(), std::min<size_t>(s->data->bytes.size(), size));
					if (ImGui::InputTextMultiline("##blob", &text, box, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine))
					{
						std::vector<uint8_t> bytes;
						if (ParseHexBytes(text, bytes))
						{
							if (s->data.use_count() > 1) s->data = std::make_shared<DataBlock>(*s->data);
							s->data->bytes = bytes;
							store32(p, uint32_t(bytes.size()), env.endian);
							changed = true;
						}
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::TreePop();
			return changed;
		}

	} // namespace


	// ----------------------------------------------------------------- editing ---

	bool EditValue(EditEnv& env, DataBlock& b, uint32_t off, const ValueType& vt, bool editable)
	{
		if (off + std::max<uint32_t>(vt.size, 1) > b.bytes.size())
		{
			ImGui::TextDisabled("(out of range)");
			return false;
		}
		ImGui::PushID(int(off));
		if (!editable) ImGui::BeginDisabled();
		const Endian e = env.endian;
		uint8_t* p = b.bytes.data() + off;
		bool changed = false;
		switch (vt.kind)
		{
			case TypeKind::Bool:
			{
				bool v = ReadInteger(b, off, vt, e) != 0;
				if (ImGui::Checkbox("##b", &v))
				{
					WriteInteger(b, off, vt, v ? 1 : 0, e);
					changed = true;
				}
				break;
			}
			case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16:
			case TypeKind::Int32: case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::UInt64: case TypeKind::Char:
				changed = EditInteger(env, b, off, vt);
				break;
			case TypeKind::Float:
			{
				float v = loadf32(p, e);
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::DragFloat("##f", &v, std::max(std::abs(v) * 0.005f, 0.001f), 0, 0, "%.6g"))
				{
					storef32(p, v, e);
					changed = true;
				}
				break;
			}
			case TypeKind::Double:
			{
				uint64_t u = load64(p, e);
				double d;
				std::memcpy(&d, &u, 8);
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::DragScalar("##d", ImGuiDataType_Double, &d, float(std::max(std::abs(d) * 0.005, 0.001)), nullptr, nullptr, "%.10g"))
				{
					std::memcpy(&u, &d, 8);
					store64(p, u, e);
					changed = true;
				}
				break;
			}
			case TypeKind::Text:
				if (env.gameplay)
					if (const auto* rf = env.gameplay->refField(StringHash32(vt.name)); rf && rf->isText)
					{
						changed = EditGameplayRef(env, b, off, *rf, 4);
						break;
					}
				changed = EditTextSlot(b, off, nullptr);
				break;
			case TypeKind::StringKey:
			{
				std::string text;
				const StringKeyLayout sk = StringKeyAt(vt.size);
				const PtrSlot* slot = b.ptrAt(off + sk.text);
				const std::string before = slot ? slot->text : std::string();
				if (EditTextSlot(b, off + sk.text, &text))
				{
					// An empty string is stored with key 0. Only an edit recomputes
					// the hash: retail strings with '?' substitutions keep theirs.
					store32(p + sk.hash32, text.empty() ? 0 : StringHash32(text), e);
					if (sk.hash64) store64(p, StringHash64(text), e);
					// Remembered once typing ends, not each prefix on the way ("T", "Th", ...).
					if (env.typingName) env.typingName(before, text);
					else if (env.rememberName && !text.empty()) env.rememberName(text);
					changed = true;
				}
				break;
			}
			case TypeKind::RefSpec: changed = EditRefSpec(env, p, vt.size); break;
			case TypeKind::ClassRefSpec: changed = EditClassRefSpec(env, p, ClassRefSpecClass(vt.typeName)); break;
			case TypeKind::Key:
				if (env.gameplay && vt.typeName == "GCollectionKey")
					if (const auto* rf = env.gameplay->refField(StringHash32(vt.name)); rf && !rf->isText)
					{
						changed = EditGameplayRef(env, b, off, *rf, vt.size);
						break;
					}
				changed = EditKey(env, p, false, vt.size);
				break;
			case TypeKind::BinKey: changed = EditKey(env, p, true); break;
			case TypeKind::Colour:
			{
				// Bytes A, R, G, B (inferred from frontend LevelColours).
				float c[4] = { p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f, p[0] / 255.0f };
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::ColorEdit4("##c", c, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
				{
					auto q = [](float f)
					{
						return uint8_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f));
					};
					p[1] = q(c[0]); p[2] = q(c[1]); p[3] = q(c[2]); p[0] = q(c[3]);
					changed = true;
				}
				break;
			}
			case TypeKind::FloatColour:
			{
				float c[4];
				for (int k = 0; k < 4; ++k) c[k] = loadf32(p + k * 4, e);
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::ColorEdit4("##c", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaBar))
				{
					for (int k = 0; k < 4; ++k) storef32(p + k * 4, c[k], e);
					changed = true;
				}
				break;
			}
			case TypeKind::Vector2: changed = EditVector(env, p, vt, 2); break;
			case TypeKind::Vector3: changed = EditVector(env, p, vt, 3); break;
			case TypeKind::Vector4:
			case TypeKind::Quaternion: changed = EditVector(env, p, vt, 4); break;
			case TypeKind::Matrix:
			{
				static const char* const kXYZW[] = { "X", "Y", "Z", "W" };
				for (int r = 0; r < 4; ++r)
				{
					ImGui::PushID(r);
					changed |= EditComponents(p + r * 16, 4, kXYZW, e);
					ImGui::PopID();
				}
				break;
			}
			case TypeKind::Blob: changed = EditBlob(env, b, off); break;
			case TypeKind::Curve: changed = EditCurve(env, b, off, vt.size); break;
			case TypeKind::Enum: changed = EditEnum(env, b, off, vt); break;
			case TypeKind::Struct:
				if (vt.st && vt.st->name == "PresetRidePaint" && vt.st->size == 40)
				{
					changed = EditPaint(env, b, off);
					break;
				}
				if (vt.st && env.swatches && !env.swatches->empty() && IsSwatchPaint(*vt.st))
				{
					changed = EditSwatchPaint(env, b, off);
					break;
				}
				// Expanded into member rows by the inspector; this is the summary.
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("%s", FormatValue(env.db, b, off, vt, e).c_str());
				break;
			case TypeKind::Unknown:
				if (vt.size == 4 && b.ptrAt(off) == nullptr)
				{
					ValueType iv = vt;
					iv.kind = TypeKind::Int32;
					changed = EditInteger(env, b, off, iv);
				}
				else
				{
					changed = EditHex(p, vt.size);
					for (const auto& s : b.ptrs)
					{
						if (s.offset < off || s.offset >= off + vt.size) continue;
						if (s.isString) ImGui::TextDisabled("+%u -> \"%s\"", s.offset - off, s.text.c_str());
						else ImGui::TextDisabled("+%u -> %zu bytes", s.offset - off, s.data ? s.data->bytes.size() : 0);
					}
				}
				break;
		}
		if (!editable) ImGui::EndDisabled();
		ImGui::PopID();
		return changed;
	}

	// -------------------------------------------------------------- formatting ---

	std::string FormatValue(const Database& db, const DataBlock& b, uint32_t off, const ValueType& vt, Endian e)
	{
		if (off + std::max<uint32_t>(vt.size, 1) > b.bytes.size()) return "?";
		const uint8_t* p = b.bytes.data() + off;
		char buf[160];
		auto text = [&](uint32_t o) -> std::string
		{
			const PtrSlot* s = b.ptrAt(o);
			return s ? "\"" + s->text + "\"" : std::string("(null)");
		};
		switch (vt.kind)
		{
			case TypeKind::Bool: return ReadInteger(b, off, vt, e) ? "true" : "false";
			case TypeKind::Int8: case TypeKind::UInt8: case TypeKind::Int16: case TypeKind::UInt16:
			case TypeKind::Int32: case TypeKind::UInt32: case TypeKind::Int64: case TypeKind::UInt64: case TypeKind::Char:
				return std::to_string(ReadInteger(b, off, vt, e));
			case TypeKind::Float: std::snprintf(buf, sizeof(buf), "%.6g", loadf32(p, e)); return buf;
			case TypeKind::Double:
			{
				uint64_t u = load64(p, e);
				double d;
				std::memcpy(&d, &u, 8);
				std::snprintf(buf, sizeof(buf), "%.10g", d);
				return buf;
			}
			case TypeKind::Text: return text(off);
			case TypeKind::StringKey: return text(off + StringKeyAt(vt.size).text);
			case TypeKind::RefSpec:
			{
				const RefSpecLayout rl = RefSpecAt(vt.size);
				const uint32_t ck = LoadKey(db, p + rl.cls, rl.keySize, e), k = LoadKey(db, p + rl.col, rl.keySize, e);
				if (!ck && !k) return "(none)";
				return db.names().format(ck) + "/" + (k ? db.names().format(k) : std::string("(none)"));
			}
			case TypeKind::Key: return db.names().format(LoadKey(db, p, vt.size, e));
			case TypeKind::ClassRefSpec: return load32(p, e) ? db.names().format(load32(p, e)) : std::string("(none)");
			case TypeKind::BinKey: return db.binNames().format(load32(p, e));
			case TypeKind::Colour: std::snprintf(buf, sizeof(buf), "ARGB %02X %02X %02X %02X", p[0], p[1], p[2], p[3]); return buf;
			case TypeKind::FloatColour:
			case TypeKind::Vector4:
			case TypeKind::Quaternion:
				std::snprintf(buf, sizeof(buf), "%.4g, %.4g, %.4g, %.4g", loadf32(p, e), loadf32(p + 4, e), loadf32(p + 8, e), loadf32(p + 12, e));
				return buf;
			case TypeKind::Vector3:
				std::snprintf(buf, sizeof(buf), "%.4g, %.4g, %.4g", loadf32(p, e), loadf32(p + 4, e), loadf32(p + 8, e));
				return buf;
			case TypeKind::Vector2: std::snprintf(buf, sizeof(buf), "%.4g, %.4g", loadf32(p, e), loadf32(p + 4, e)); return buf;
			case TypeKind::Matrix: return "matrix";
			case TypeKind::Blob:
			{
				// The data, which can run past the stored size (retail Carbon's handlers).
				const PtrSlot* s = b.ptrAt(off + 4);
				return std::to_string(s && s->data ? s->data->bytes.size() : 0) + " bytes";
			}
			case TypeKind::Curve:
			{
				CurveValue c = ReadCurve(b, off, e, vt.size);
				std::snprintf(buf, sizeof(buf), "curve, %zu points, x %.4g..%.4g", c.x.size(), c.xMin, c.xMax);
				return buf;
			}
			case TypeKind::Enum:
			{
				const int64_t v = ReadInteger(b, off, vt, e);
				if (vt.en)
					if (const std::string* n = vt.en->nameOf(v)) return *n;
				return std::to_string(v);
			}
			case TypeKind::Struct:
			{
				if (!vt.st) return "?";
				if (vt.st->name == "PresetRidePaint" && vt.st->size == 40)
				{
					const PaintColour pc = ReadPaint(b, off, e);
					std::snprintf(buf, sizeof(buf), "%s H %.0f S %.2f V %.2f", PaintFinish(db.names(), load32(p + 8, e)).c_str(),
								  pc.h * 360.0f, pc.s, pc.v);
					return buf;
				}
				if (IsSwatchPaint(*vt.st))
					if (const PaintSwatches sw = ReadPaintSwatches(db); !sw.empty())
					{
						const PaintColour pc = ReadSwatchPaint(b, off, sw, e);
						std::snprintf(buf, sizeof(buf), "%s swatch %d H %.0f S %.2f V %.2f", PaintFinish(db.names(), load32(p + 8, e)).c_str(),
									  pc.step, pc.h * 360.0f, pc.s, pc.v);
						return buf;
					}
				std::string s = "{ ";
				size_t shown = 0;
				for (const MemberDef& m : vt.st->members)
				{
					if (shown == 4)
					{
						s += ", ..."; break;
					}
					if (shown) s += ", ";
					const ValueType mt = TypeOfMember(m);
					if (m.count > 1) s += m.name + "[" + std::to_string(m.count) + "]";
					else if (m.prim == MemberPrim::Ptr) s += m.name + "*";
					else s += m.name + " " + FormatValue(db, b, off + m.offset, mt, e);
					++shown;
				}
				return s + " }";
			}
			case TypeKind::Unknown:
				if (vt.size == 4 && vt.hint == "hex")
				{
					std::snprintf(buf, sizeof(buf), "0x%08X", load32(p, e));
					return buf;
				}
				if (vt.size == 4) return std::to_string(int32_t(load32(p, e)));
				return HexBytes(p, std::min<uint32_t>(vt.size, 16)) + (vt.size > 16 ? " ..." : "");
		}
		return "?";
	}

} // namespace app
