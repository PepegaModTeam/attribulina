// The Sequence Builder: an activity's state graph (its `state` collections and
// the transitions their message handlers make), the handlers of a state, and
// each handler's Lua, decompiled for editing and compiled back into its
// bytecode blob. Named after Black Box's tool for authoring these graphs; the
// TransitionGenerator markers in World's lithium.lua come from it.

#include "App.h"

#include "core/Hash.h"
#include "core/lua/Lua.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <map>
#include <optional>

namespace app
{

	using namespace attrib;

	namespace
	{

		const ImVec4 kOk(0.45f, 0.85f, 0.45f, 1.0f);
		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);

		// A GCollectionKey (or u32) field's key: 4 bytes, or an early Most Wanted
		// demo's 8 (Values.h LoadKey).
		uint32_t KeyField(const Database& db, const Collection& c, const char* field, uint32_t i = 0)
		{
			const Attribute* a = c.resolve(StringHash32(field));
			if (!a || i >= a->count || (a->elemSize != 4 && a->elemSize != 8)) return 0;
			return LoadKey(db, a->elem(i), a->elemSize, c.vault ? c.vault->raw.endian : Endian::Little);
		}

		std::vector<uint32_t> KeyList(const Database& db, const Collection& c, const char* field)
		{
			std::vector<uint32_t> out;
			if (const Attribute* a = c.find(StringHash32(field)); a && (a->elemSize == 4 || a->elemSize == 8))
				for (uint32_t i = 0; i < a->count; ++i) out.push_back(LoadKey(db, a->elem(i), a->elemSize, c.vault ? c.vault->raw.endian : Endian::Little));
			return out;
		}

		std::string Skeleton(const std::string& functionName)
		{
			return "function " + functionName + "( self, message, vars )\n\nend\n";
		}

		std::string Lower(std::string s)
		{
			for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

	} // namespace

	// ------------------------------------------------------------------ setup ---

	void App::openSequenceBuilder(Collection* c)
	{
		if (!c || !gameplay_) return;
		const uint32_t gp = gameplay_->cls()->key;
		Collection* activity = nullptr;
		Collection* state = nullptr;
		Collection* handler = nullptr;
		if (KeyField(*db_, *c, "stateref"))
		{
			handler = c;
			state = db_->findCollection(gp, KeyField(*db_, *c, "stateref"));
			activity = db_->findCollection(gp, KeyField(*db_, *c, "handler_owner"));
			if (!activity && state) activity = db_->findCollection(gp, KeyField(*db_, *state, "templateref"));
		}
		else if (KeyField(*db_, *c, "templateref"))
		{
			state = c;
			activity = db_->findCollection(gp, KeyField(*db_, *c, "templateref"));
			// A template under that one whose name the state's carries runs it as
			// well, and has its own: Carbon's stategraph_career_game_initial is a
			// state of career that career_game runs.
			const std::string n = db_->names().format(c->key);
			const Collection* base = activity;
			size_t best = 0;
			for (Collection* t : gameplay_->templates())
			{
				const std::string prefix = "stategraph_" + db_->names().format(t->key) + "_";
				if (base && t != base && prefix.size() > best && n.rfind(prefix, 0) == 0 && gameplay_->derivesFrom(t, base) &&
					!gameplay_->statesOf(t).empty())
				{
					activity = t;
					best = prefix.size();
				}
			}
		}
		else
		{
			activity = c;
		}
		sbActivity_ = activity;
		sbState_ = state;
		sbHandler_ = handler;
		if (sbActivity_ && !sbState_) sbState_ = stateNamed("initial");
		showSequence_ = true;
		ImGui::SetWindowFocus("Sequence Builder");
	}

	std::string App::stateShortName(const Collection* state) const
	{
		return gameplay_ ? gameplay_->stateName(state) : db_->names().format(state->key);
	}

	// The state ChangeState( shortName ) enters in the activity: one of its own or
	// its templates' by Name, else the collection the naming rule gives.
	Collection* App::stateNamed(const std::string& shortName) const
	{
		if (!sbActivity_ || !gameplay_) return nullptr;
		for (Collection* s : gameplay_->allStatesOf(sbActivity_))
			if (gameplay_->stateName(s) == shortName) return s;
		return db_->findCollection(gameplay_->cls()->key,
								   StringHash32("stategraph_" + db_->names().format(sbActivity_->key) + "_" + shortName));
	}

	// ----------------------------------------------------------------- window ---

	void App::drawSequenceBuilder()
	{
		if (!showSequence_) return;
		ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 70, ImGui::GetFontSize() * 40), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Sequence Builder", &showSequence_))
		{
			ImGui::End();
			return;
		}
		if (!db_ || !gameplay_)
		{
			ImGui::TextDisabled("Open a database with a gameplay class (gameplay.lzc) to edit its state graphs.");
			ImGui::End();
			return;
		}
		const KeyNames& names = db_->names();
		const GameplayModel& m = *gameplay_;

		// Activity templates that have states.
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Activity");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
		if (ImGui::BeginCombo("##activity", sbActivity_ ? names.format(sbActivity_->key).c_str() : "(choose)", ImGuiComboFlags_HeightLarge))
		{
			for (Collection* t : m.templates())
			{
				if (m.statesOf(t).empty()) continue; // one that only inherits runs its template's graph
				const size_t n = m.allStatesOf(t).size();
				ImGui::PushID(t);
				if (ImGui::Selectable(names.format(t->key).c_str(), t == sbActivity_))
				{
					sbActivity_ = t;
					sbState_ = stateNamed("initial");
					sbHandler_ = nullptr;
					sbPan_ = ImVec2(0, 0);
				}
				ImGui::SameLine();
				ImGui::TextDisabled("%zu states", n);
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!sbActivity_);
		if (ImGui::Button("New State...")) request(Pending::NewState);
		ImGui::SameLine();
		ImGui::BeginDisabled(!sbState_);
		if (ImGui::Button("New Handler..."))
		{
			request(Pending::NewHandler);
			sbMessage_ = nullptr;
			sbFilter_.clear();
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (sbActivity_)
		{
			ImGui::SameLine();
			if (ImGui::TextLink(("open " + names.format(sbActivity_->key)).c_str())) select(sbActivity_, true);
			linkClicks(sbActivity_);
		}
		if (!sbActivity_)
		{
			ImGui::TextDisabled("Choose an activity template. Its states and their message handlers make up the graph.");
			ImGui::End();
			return;
		}

		const std::vector<Collection*> states = m.allStatesOf(sbActivity_);
		ImGui::BeginChild("##graph", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, 0),
						  ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX, ImGuiWindowFlags_NoScrollbar);
		drawStateGraph(states);
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##detail");
		drawHandlerList();
		ImGui::Separator();
		drawCodeEditor();
		ImGui::EndChild();
		ImGui::End();
	}

	// States in rows by their distance from "initial", top to bottom, arrows for
	// the transitions handlers make. States the activity inherits from its
	// templates are outlined dimmer. Drag to pan, click a state to see its
	// handlers, double-click to open it.
	void App::drawStateGraph(const std::vector<Collection*>& states)
	{
		const KeyNames& names = db_->names();
		const GameplayModel& m = *gameplay_;
		sbBoxes_.clear();
		if (states.empty())
		{
			ImGui::TextDisabled("No states.");
			return;
		}
		struct Edge
		{
			size_t from, to;
			std::string message;
		};
		std::map<const Collection*, size_t> index;
		for (size_t i = 0; i < states.size(); ++i) index[states[i]] = i;
		std::vector<Edge> edges;
		for (size_t i = 0; i < states.size(); ++i)
			for (Collection* h : m.handlersOf(states[i]))
				for (uint32_t k : KeyList(*db_, *h, "transitionlist"))
					if (Collection* t = db_->findCollection(m.cls()->key, k); t && index.count(t))
					{
						std::string msg = names.format(KeyField(*db_, *h, "message_id"));
						if (msg.size() > 1 && msg[0] == 'M') msg.erase(0, 1);
						edges.push_back({ i, index[t], msg });
					}

		// Rows: breadth-first from the initial state; unreached states last.
		std::vector<int> layer(states.size(), -1);
		size_t start = 0;
		if (Collection* init = stateNamed("initial"); init && index.count(init)) start = index[init];
		std::deque<size_t> q{ start };
		layer[start] = 0;
		while (!q.empty())
		{
			const size_t s = q.front();
			q.pop_front();
			for (const Edge& e : edges)
				if (e.from == s && layer[e.to] < 0)
				{
					layer[e.to] = layer[s] + 1;
					q.push_back(e.to);
				}
		}
		int maxLayer = 0;
		for (int l : layer) maxLayer = std::max(maxLayer, l);
		for (int& l : layer)
			if (l < 0) l = maxLayer + 1;
		std::map<int, int> perLayer;
		std::vector<int> col(states.size());
		for (size_t i = 0; i < states.size(); ++i) col[i] = perLayer[layer[i]]++;
		int widest = 0;
		for (const auto& [l, n] : perLayer) widest = std::max(widest, n);

		const float fs = ImGui::GetFontSize();
		float boxW = fs * 6;
		for (const Collection* s : states) boxW = std::max(boxW, ImGui::CalcTextSize(stateShortName(s).c_str()).x + fs);
		const float boxH = fs * 1.8f, gapX = fs * 1.5f, gapY = fs * 2.5f;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 size = ImGui::GetContentRegionAvail();
		ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
							   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
		const bool hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
		{
			sbPan_.x += ImGui::GetIO().MouseDelta.x;
			sbPan_.y += ImGui::GetIO().MouseDelta.y;
		}
		// Each row centred under the widest.
		auto boxMin = [&](size_t i)
		{
			const float indent = (widest - perLayer[layer[i]]) * (boxW + gapX) * 0.5f;
			return ImVec2(origin.x + sbPan_.x + fs + indent + col[i] * (boxW + gapX), origin.y + sbPan_.y + fs + layer[i] * (boxH + gapY));
		};

		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
		const ImU32 edgeCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		const ImU32 edgeHot = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
		const size_t sel = sbState_ && index.count(sbState_) ? index[sbState_] : SIZE_MAX;
		for (const Edge& e : edges)
		{
			const ImVec2 a = boxMin(e.from), b = boxMin(e.to);
			const ImU32 col = e.from == sel ? edgeHot : edgeCol;
			if (e.from == e.to)
			{
				const ImVec2 p(a.x + boxW, a.y + boxH * 0.5f);
				dl->AddBezierCubic(ImVec2(p.x, p.y - fs * 0.4f), ImVec2(p.x + fs * 1.2f, p.y - fs), ImVec2(p.x + fs * 1.2f, p.y + fs),
								   ImVec2(p.x, p.y + fs * 0.4f), col, 1.5f);
				continue;
			}
			if (layer[e.to] <= layer[e.from])
			{
				// Up or sideways: leave from the right, come in from the right.
				const ImVec2 p0(a.x + boxW, a.y + boxH * 0.5f), p1(b.x + boxW, b.y + boxH * 0.5f);
				const float out = fs * 2.5f + std::abs(p1.y - p0.y) * 0.15f;
				dl->AddBezierCubic(p0, ImVec2(p0.x + out, p0.y), ImVec2(p1.x + out, p1.y), p1, col, 1.5f);
				dl->AddTriangleFilled(p1, ImVec2(p1.x + fs * 0.5f, p1.y - fs * 0.3f), ImVec2(p1.x + fs * 0.5f, p1.y + fs * 0.3f), col);
				continue;
			}
			const ImVec2 p0(a.x + boxW * 0.5f, a.y + boxH), p1(b.x + boxW * 0.5f, b.y);
			const float bend = std::max(gapY * 0.5f, std::abs(p1.y - p0.y) * 0.4f);
			dl->AddBezierCubic(p0, ImVec2(p0.x, p0.y + bend), ImVec2(p1.x, p1.y - bend), p1, col, 1.5f);
			dl->AddTriangleFilled(p1, ImVec2(p1.x + fs * 0.3f, p1.y - fs * 0.5f), ImVec2(p1.x - fs * 0.3f, p1.y - fs * 0.5f), col);
		}
		std::optional<size_t> under;
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		for (size_t i = 0; i < states.size(); ++i)
		{
			const ImVec2 a = boxMin(i), b(a.x + boxW, a.y + boxH);
			const bool isSel = i == sel;
			const bool inherited = m.stateTemplate(states[i]) != sbActivity_;
			const bool over = hovered && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
			if (over) under = i;
			sbBoxes_.push_back({ states[i], a, b });
			dl->AddRectFilled(a, b, ImGui::GetColorU32(isSel ? ImGuiCol_Header : over ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 4.0f);
			dl->AddRect(a, b, ImGui::GetColorU32(isSel ? ImGuiCol_HeaderActive : inherited ? ImGuiCol_TextDisabled : ImGuiCol_Border), 4.0f, 0,
						isSel ? 2.0f : 1.0f);
			const std::string label = stateShortName(states[i]);
			const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
			dl->AddText(ImVec2(a.x + (boxW - ts.x) * 0.5f, a.y + (boxH - ts.y) * 0.5f),
						ImGui::GetColorU32(inherited ? ImGuiCol_TextDisabled : ImGuiCol_Text), label.c_str());
		}
		dl->PopClipRect();
		if (under)
		{
			Collection* s = states[*under];
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.0f)
			{
				if (sbState_ != s) sbHandler_ = nullptr;
				sbState_ = s;
			}
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) select(s, true);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) openInNewTab(s, false);
			const Collection* t = m.stateTemplate(s);
			if (t && t != sbActivity_)
				ImGui::SetTooltip("%s\n%zu handler(s)\nA state of %s, which %s inherits", names.format(s->key).c_str(), m.handlersOf(s).size(),
								  names.format(t->key).c_str(), names.format(sbActivity_->key).c_str());
			else
				ImGui::SetTooltip("%s\n%zu handler(s)", names.format(s->key).c_str(), m.handlersOf(s).size());
		}
	}

	void App::drawHandlerList()
	{
		const KeyNames& names = db_->names();
		if (!sbState_)
		{
			ImGui::TextDisabled("Click a state in the graph.");
			return;
		}
		ImGui::AlignTextToFramePadding();
		ImGui::Text("State %s", stateShortName(sbState_).c_str());
		ImGui::SameLine();
		if (ImGui::TextLink("open")) select(sbState_, true);
		linkClicks(sbState_);
		const std::vector<Collection*> handlers = gameplay_->handlersOf(sbState_);
		const float h = std::min(ImGui::GetFrameHeightWithSpacing() * float(handlers.size() + 1.5f), ImGui::GetContentRegionAvail().y * 0.35f);
		if (ImGui::BeginTable("##handlers", 3,
							  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
							  ImVec2(0, std::max(h, ImGui::GetFrameHeightWithSpacing() * 2))))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Message");
			ImGui::TableSetupColumn("Goes to");
			ImGui::TableSetupColumn("Handler");
			ImGui::TableHeadersRow();
			for (Collection* hd : handlers)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(hd);
				if (ImGui::Selectable(names.format(KeyField(*db_, *hd, "message_id")).c_str(), hd == sbHandler_,
									  ImGuiSelectableFlags_SpanAllColumns))
					sbHandler_ = hd;
				linkClicks(hd);
				ImGui::TableSetColumnIndex(1);
				std::string to;
				for (uint32_t k : KeyList(*db_, *hd, "transitionlist"))
				{
					Collection* t = db_->findCollection(gameplay_->cls()->key, k);
					to += (to.empty() ? "" : ", ") + (t ? stateShortName(t) : names.format(k));
				}
				ImGui::TextDisabled("%s", to.c_str());
				ImGui::TableSetColumnIndex(2);
				const std::string n = names.format(hd->key);
				const size_t at = n.rfind("handler_");
				ImGui::TextDisabled("%s", at == std::string::npos ? n.c_str() : n.c_str() + at);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	void App::loadHandlerSource()
	{
		sbSourceFor_ = sbHandler_;
		luaEditor_.invalidate();
		luaEditor_.resetHistory(); // another handler's code
		luaEditor_.clearMarks();
		sbWarnings_.clear();
		sbStatus_.clear();
		sbStatusError_ = false;
		sbDisassembly_.clear();
		const std::string fn = HandlerFunction(*db_, db_->names().format(sbHandler_->key));
		Bytes chunk;
		try
		{
			chunk = HandlerChunk(*sbHandler_);
		}
		catch (const std::exception& e)
		{
			sbStatus_ = std::string("Could not unpack the bytecode: ") + e.what();
			sbStatusError_ = true;
		}
		if (chunk.empty())
		{
			sbSource_ = Skeleton(fn);
			sbLoaded_.clear();
			if (sbStatus_.empty()) sbStatus_ = "No bytecode yet. Write the handler and compile it.";
			return;
		}
		try
		{
			lua::DecompileResult r = lua::Decompile(chunk);
			sbSource_ = r.source;
			sbWarnings_ = r.warnings;
			sbDisassembly_ = lua::Disassemble(chunk);
			// Decompile checks whether its source compiles back to this bytecode.
			if (r.exact)
			{
				sbStatus_ = "Decompiled; compiles back to the same bytecode.";
			}
			else
			{
				sbStatus_ = "Decompiled, but compiling it back gives different bytecode; check it before saving.";
				sbStatusError_ = true;
			}
		}
		catch (const std::exception& e)
		{
			sbSource_ = "-- Could not decompile this chunk: " + std::string(e.what()) + "\n";
			sbStatus_ = e.what();
			sbStatusError_ = true;
			try
			{
				sbDisassembly_ = lua::Disassemble(chunk);
			}
			catch (const std::exception&)
			{
			}
		}
		sbLoaded_ = sbSource_;
	}

	bool App::compileHandler()
	{
		if (!sbHandler_ || !db_) return false;
		const std::string fn = HandlerFunction(*db_, db_->names().format(sbHandler_->key));
		const lua::CompileResult r = lua::Compile(sbSource_, VersionOfChunk(*db_, HandlerChunk(*sbHandler_)));
		if (!r.ok())
		{
			sbStatus_ = r.error;
			sbStatusError_ = true;
			if (r.line > 0) luaEditor_.setMarks({ {r.line, r.error} });
			if (r.line > 0) luaEditor_.gotoLine(r.line, false); // in view; the keyboard stays where it is
			return false;
		}
		luaEditor_.clearMarks();
		const lua::ChunkInfo info = lua::Inspect(r.chunk);
		if (std::find(info.globalsSet.begin(), info.globalsSet.end(), fn) == info.globalsSet.end())
		{
			sbStatus_ = "The script must define function " + fn + ": the game calls the handler by that name.";
			sbStatusError_ = true;
			return false;
		}
		SetHandlerChunk(*db_, *sbHandler_, r.chunk);
		// ChangeState( "x" ) calls are the handler's transitions.
		std::vector<uint32_t> keys;
		std::string unknown;
		for (const std::string& name : ChangeStateTargets(sbSource_))
		{
			if (Collection* st = stateNamed(name)) keys.push_back(st->key);
			else unknown += (unknown.empty() ? "" : ", ") + name;
		}
		SetHandlerTransitions(*db_, *sbHandler_, keys);
		nameAction("Compile " + db_->names().format(sbHandler_->key));
		markDirty(sbHandler_);
		sbLoaded_ = sbSource_;
		sbDisassembly_ = lua::Disassemble(r.chunk);
		sbStatus_ = "Compiled " + std::to_string(r.chunk.size()) + " bytes into " + db_->names().format(sbHandler_->key) + "; " +
			std::to_string(keys.size()) + " transition(s).";
		if (!unknown.empty()) sbStatus_ += " No state named: " + unknown + ".";
		sbStatusError_ = false;
		log(LogLevel::Info, sbStatus_);
		return true;
	}

	void App::drawCodeEditor()
	{
		const KeyNames& names = db_->names();
		if (!sbHandler_)
		{
			ImGui::TextDisabled("Select a handler to edit its Lua.");
			return;
		}
		if (sbSourceFor_ != sbHandler_) loadHandlerSource();
		const std::string path = names.format(sbHandler_->key);
		const std::string fn = HandlerFunction(*db_, path);
		const bool edited = sbSource_ != sbLoaded_;
		if (ImGui::Button(edited ? "Compile & Save*" : "Compile & Save")) compileHandler();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Compiles with Lua into %s's bytecode. ChangeState( \"x\" ) calls set its transitions.",
							  path.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(!edited);
		if (ImGui::Button("Revert")) loadHandlerSource();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Checkbox("Disassembly", &sbShowDisassembly_);
		ImGui::SameLine();
		ImGui::Checkbox("Functions", &sbShowFunctions_);
		if (ImGui::IsItemHovered())
		{
			if (luaList_.empty())
				ImGui::SetTooltip("There is no list of the functions %s gives handlers in data/lua. Yours are listed.", db_->target().name().c_str());
			else
				ImGui::SetTooltip("The functions the game gives handlers (data/lua/%s.txt), and yours.", luaList_.c_str());
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", fn.c_str());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());

		// What the message carries.
		const uint32_t mid = KeyField(*db_, *sbHandler_, "message_id");
		for (const MessageDef* md : MessagesFor(messages_, db_->target().game))
			if (StringHash32(md->name) == mid)
			{
				std::string f;
				for (const auto& [n, t] : md->fields) f += (f.empty() ? "" : ", ") + n + " (" + t + ")";
				if (!f.empty()) ImGui::TextDisabled("%s: message.%s", md->name.c_str(), f.c_str());
				else ImGui::TextDisabled("%s: %s", md->name.c_str(), md->game == Game::Unknown ? "fields unknown (named from its handlers)" : "no fields");
				break;
			}
		if (!sbStatus_.empty()) ImGui::TextColored(sbStatusError_ ? kError : kOk, "%s", sbStatus_.c_str());
		for (const std::string& w : sbWarnings_) ImGui::TextColored(kWarn, "decompiler: %s", w.c_str());

		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float sideW = sbShowFunctions_ ? std::min(avail.x * 0.35f, ImGui::GetFontSize() * 26) : 0.0f;
		const float codeW = (sbShowDisassembly_ ? (avail.x - sideW) * 0.6f : avail.x - sideW) - (sideW > 0 ? ImGui::GetStyle().ItemSpacing.x : 0.0f);
		if (codeFont_) ImGui::PushFont(codeFont_, 0.0f);
		if (luaEditor_.draw("##code", sbSource_, ImVec2(codeW, avail.y), ImGuiInputTextFlags_AllowTabInput)) luaEditor_.clearMarks();
		if (sbShowDisassembly_)
		{
			ImGui::SameLine();
			ImGui::InputTextMultiline("##disasm", &sbDisassembly_, ImVec2(sideW > 0 ? avail.x - sideW - codeW - ImGui::GetStyle().ItemSpacing.x * 2 : -FLT_MIN, avail.y),
									  ImGuiInputTextFlags_ReadOnly);
		}
		if (codeFont_) ImGui::PopFont();
		if (sbShowFunctions_)
		{
			// The game's functions: hover for what one does, click to copy its name.
			ImGui::SameLine();
			ImGui::BeginChild("##functions", ImVec2(-FLT_MIN, avail.y), ImGuiChildFlags_Borders);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##filter", "filter, e.g. Racer or EStart", &sbFunctionFilter_);
			if (ImGui::SmallButton("Your Functions...")) openUserLuaFunctions();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Functions you add for %s, such as ones your own code hooks bind.\nSaved in user_lua_%s.txt beside the program.",
								  InfoOf(db_->target().game).name, InfoOf(db_->target().game).id);
			const std::string f = ToLower(sbFunctionFilter_);
			const std::optional<Target> listed = ParseTarget(luaList_);
			if (luaList_.empty())
				ImGui::TextDisabled("No list of %s's functions in data/lua.", InfoOf(db_->target().game).name);
			bool yours = false, theirs = false; // headings drawn
			for (size_t i = 0; i < luaApi_.size(); ++i)
			{
				const LuaEntry& e = luaApi_[i];
				if (e.kind == "constant") continue;
				if (!f.empty() && ToLower(e.name).find(f) == std::string::npos && ToLower(e.description).find(f) == std::string::npos) continue;
				if (e.user && !yours)
				{
					yours = true;
					ImGui::SeparatorText("Yours");
				}
				else if (!e.user && !theirs)
				{
					theirs = true;
					if (listed) ImGui::SeparatorText(listed->name().c_str()); // the list used: this platform's or another's
				}
				ImGui::PushID(int(i));
				if (e.kind == "stdlib") ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				if (ImGui::Selectable((e.name + (e.signature.empty() ? "" : " " + e.signature)).c_str()))
				{
					const size_t colon = e.name.find(':');
					ImGui::SetClipboardText((colon == std::string::npos ? e.name : e.name.substr(colon)).c_str());
				}
				if (e.kind == "stdlib") ImGui::PopStyleColor();
				if (ImGui::IsItemHovered() && !e.description.empty())
					ImGui::SetTooltip("%s%s\n%s", e.kind.c_str(), e.user ? ", yours" : "", e.description.c_str());
				ImGui::PopID();
			}
			ImGui::EndChild();
		}
	}

	// ----------------------------------------------------------------- modals ---

	// ------------------------------------------------------- your functions ---

	void App::drawUserLuaWindow()
	{
		if (!showUserLua_) return;
		const float fs = ImGui::GetFontSize();
		const char* game = InfoOf(userLuaGame_).name;
		ImGui::SetNextWindowSize(ImVec2(fs * 44, fs * 24), ImGuiCond_FirstUseEver);
		const bool unsaved = userLuaText_ != userLuaSaved_;
		const std::string title = std::string("Your Lua Functions: ") + game + (unsaved ? "*" : "") + "###userlua";
		if (!ImGui::Begin(title.c_str(), &showUserLua_))
		{
			ImGui::End();
			return;
		}
		ImGui::TextWrapped("Functions you add for %s, such as ones your own code hooks bind. The Lua editor colours them and lists them "
						   "first. One with the name of a listed function replaces it.",
						   game);
		ImGui::TextDisabled("function Table.Name (arg: number) -> boolean  # what it does");
		ImGui::TextDisabled("method Meta:Name (arg: string)  # what it does");
		ImGui::TextDisabled("table Table  # what it holds");
		// Lines it can't read are marked on their number.
		if (userLuaChecked_ != userLuaText_)
		{
			userLuaChecked_ = userLuaText_;
			userLuaErrors_.clear();
			int n = 0;
			for (size_t b = 0;;)
			{
				const size_t e = userLuaText_.find('\n', b);
				++n;
				std::string why;
				if (!ParseLuaLine(userLuaText_.substr(b, e == std::string::npos ? std::string::npos : e - b), &why) && !why.empty()) userLuaErrors_[n] = why;
				if (e == std::string::npos) break;
				b = e + 1;
			}
			userLuaEditor_.setMarks(userLuaErrors_);
		}
		if (codeFont_) ImGui::PushFont(codeFont_, 0.0f);
		userLuaEditor_.draw("##userlua", userLuaText_, ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing()));
		if (codeFont_) ImGui::PopFont();
		ImGui::BeginDisabled(!unsaved);
		if (ImGui::Button("Save")) saveUserLuaFunctions();
		ImGui::SameLine();
		if (ImGui::Button("Revert")) userLuaText_ = userLuaSaved_;
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (!userLuaErrors_.empty())
			ImGui::TextColored(kWarn, "%zu line(s) can't be read and are left out; hover their numbers", userLuaErrors_.size());
		else
			ImGui::TextDisabled("user_lua_%s.txt beside the program", InfoOf(userLuaGame_).id);
		ImGui::End();
	}

	void App::drawSequenceModals()
	{
		if (!gameplay_ || !db_) return;
		const GameplayModel& m = *gameplay_;
		const KeyNames& names = db_->names();
		const uint32_t gp = m.cls()->key;
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		auto begin = [&](const char* name)
		{
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		};
		auto done = [&]()
		{
			pending_ = Pending::None;
			ImGui::CloseCurrentPopup();
		};
		const float w = ImGui::GetFontSize() * 28;

		if (begin("New State"))
		{
			const std::string act = sbActivity_ ? names.format(sbActivity_->key) : std::string();
			ImGui::Text("New state of %s", act.c_str());
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(w);
			const bool enter = ImGui::InputTextWithHint("name", "e.g. countdown", &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
			const std::string path = "stategraph_" + act + "_" + nameInput_;
			Collection* templ = db_->findCollection(gp, StringHash32("state"));
			const bool taken = !nameInput_.empty() && db_->findCollection(gp, StringHash32(path));
			ImGui::TextDisabled("collection %s; ChangeState( \"%s\" ) switches to it", path.c_str(), nameInput_.c_str());
			if (taken) ImGui::TextColored(kError, "That state exists.");
			if (!templ) ImGui::TextColored(kError, "There is no 'state' template to make it from.");
			const bool ok = sbActivity_ && templ && !nameInput_.empty() && !taken;
			ImGui::BeginDisabled(!ok);
			if (ImGui::Button("Create") || (enter && ok))
			{
				try
				{
					NewGameplayElement e{ path, templ, nullptr, "" };
					Collection& st = CreateGameplayElement(*db_, m, e);
					SetGameplayKey(*db_, st, "templateref", sbActivity_->key);
					rememberName(path);
					nameAction("New state " + path);
					markDirty(&st);
					structureChanged();
					sbState_ = &st;
					sbHandler_ = nullptr;
					log(LogLevel::Info, "Created state " + path);
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Error, ex.what());
				}
				done();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}

		if (begin("New Handler"))
		{
			const std::string act = sbActivity_ ? names.format(sbActivity_->key) : std::string();
			ImGui::Text("New handler in state %s", sbState_ ? stateShortName(sbState_).c_str() : "?");
			ImGui::SetNextItemWidth(w);
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextWithHint("##msgfilter", "message, e.g. NotifyRacerFinished", &sbFilter_);
			const std::string f = Lower(sbFilter_);
			if (ImGui::BeginListBox("##messages", ImVec2(w, ImGui::GetFontSize() * 14)))
			{
				for (const MessageDef* md : MessagesFor(messages_, db_->target().game))
				{
					if (!f.empty() && Lower(md->name).find(f) == std::string::npos) continue;
					if (ImGui::Selectable(md->name.c_str(), md == sbMessage_)) sbMessage_ = md;
					if (!md->fields.empty() && ImGui::IsItemHovered())
					{
						std::string t;
						for (const auto& [n, ty] : md->fields) t += "message." + n + "  (" + ty + ")\n";
						ImGui::SetTooltip("%s", t.c_str());
					}
				}
				ImGui::EndListBox();
			}
			std::string path;
			if (sbMessage_ && sbState_)
			{
				std::string msg = sbMessage_->name.size() > 1 && sbMessage_->name[0] == 'M' ? sbMessage_->name.substr(1) : sbMessage_->name;
				// Most Wanted's have no activity in front: <state>_handler_<message>.
				const std::string base = (db_->target().game == Game::MostWanted ? "" : act + "/") + names.format(sbState_->key) + "_handler_" + Lower(msg);
				path = base;
				for (int n = 2; db_->findCollection(gp, StringHash32(path)); ++n) path = base + "_" + std::to_string(n);
				ImGui::TextDisabled("collection %s\nfunction %s", path.c_str(), HandlerFunction(*db_, path).c_str());
			}
			else if (messages_.empty())
			{
				ImGui::TextColored(kWarn, "No message list loaded (data/messages/*.txt).");
			}
			Collection* templ = db_->findCollection(gp, StringHash32("messagehandler"));
			const bool ok = sbActivity_ && sbState_ && sbMessage_ && templ && !path.empty();
			ImGui::BeginDisabled(!ok);
			if (ImGui::Button("Create"))
			{
				try
				{
					NewGameplayElement e{ path, templ, nullptr, "" };
					Collection& h = CreateGameplayElement(*db_, m, e);
					SetGameplayKey(*db_, h, "handler_owner", sbActivity_->key);
					SetGameplayKey(*db_, h, "stateref", sbState_->key);
					SetGameplayKey(*db_, h, "message_id", StringHash32(sbMessage_->name));
					const lua::CompileResult r = lua::Compile(Skeleton(HandlerFunction(*db_, path)), LuaVersionOf(*db_));
					if (r.ok()) SetHandlerChunk(*db_, h, r.chunk);
					rememberName(path);
					nameAction("New handler " + path);
					markDirty(&h);
					structureChanged();
					sbHandler_ = &h;
					sbSourceFor_ = nullptr;
					log(LogLevel::Info, "Created handler " + path + " for " + sbMessage_->name);
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Error, ex.what());
				}
				done();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}
	}

} // namespace app
