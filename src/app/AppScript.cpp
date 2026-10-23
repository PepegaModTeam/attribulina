// Mod scripts in the editor (core/Script.h): the Script panel, applying a
// script after asking for its options, and adding collections, fields or the
// changes since the files were saved to the panel's script.

#include "App.h"

#include "core/Script.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace app
{

	using namespace attrib;
	namespace sc = attrib::script;
	namespace fs = std::filesystem;

	namespace
	{

		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);
		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
		const char* kWindowScript = "Script";

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}

		bool Blank(const std::string& s)
		{
			return s.find_first_not_of(" \t\r\n") == std::string::npos;
		}

		// An included script's path from the including script's folder when it's
		// under it, else in full.
		std::string Shown(const std::string& file, const std::string& folder)
		{
			if (folder.empty()) return file;
			const fs::path rel = FromUtf8(file).lexically_relative(FromUtf8(folder));
			if (rel.empty() || *rel.begin() == "..") return file;
			return ToUtf8(rel);
		}

		// "mod.amod:12: text"; on a line of an included script,
		// "mod.amod:4: in parts/kits.amod:3: text".
		std::string MessageText(const std::string& source, const sc::Message& m, const std::string& folder = {})
		{
			std::string s = source;
			if (m.pos.line) s += ":" + std::to_string(m.pos.line);
			if (!m.file.empty())
			{
				s += ": in " + Shown(m.file, folder);
				if (m.fileLine) s += ":" + std::to_string(m.fileLine);
			}
			return s + ": " + m.text;
		}

		// The scripts the statements in `list` include, and the ones those include,
		// once each.
		void IncludedFiles(const std::vector<sc::Statement>& list, std::vector<std::string>& out)
		{
			for (const sc::Statement& st : list)
			{
				if (st.kind == sc::Statement::Kind::If)
				{
					IncludedFiles(st.body, out);
					IncludedFiles(st.elseBody, out);
				}
				else if (st.kind == sc::Statement::Kind::Include && st.included)
				{
					if (std::find(out.begin(), out.end(), st.included->source) != out.end()) continue;
					out.push_back(st.included->source);
					IncludedFiles(st.included->statements, out);
				}
			}
		}

	} // namespace

	void App::setScriptText(std::string text, std::string path)
	{
		scriptEditor_.invalidate();
		scriptEditor_.resetHistory();
		scriptEditor_.clearMarks();
		scriptText_ = std::move(text);
		scriptSaved_ = scriptText_;
		scriptPath_ = std::move(path);
		scriptMessages_.clear();
		scriptStatus_.clear();
		showScript_ = true;
		bringScript_ = true;
	}

	std::string App::scriptName() const
	{
		return scriptPath_.empty() ? std::string("Script panel") : ToUtf8(FromUtf8(scriptPath_).filename());
	}

	std::string App::scriptFolder() const
	{
		return scriptPath_.empty() ? std::string() : ToUtf8(FromUtf8(scriptPath_).parent_path());
	}

	bool App::addToScript(const std::vector<sc::Statement>& statements, bool show)
	{
		if (!db_) return false;
		sc::Script s;
		if (Blank(scriptText_))
		{
			s = sc::NewScript(*db_);
		}
		else
		{
			try
			{
				s = sc::Parse(scriptText_, scriptName());
			}
			catch (const sc::ScriptError& e)
			{
				log(LogLevel::Error, std::string("The Script panel's text doesn't read (") + e.what() + "); fix it before adding to it.");
				showScript_ = bringScript_ = true;
				return false;
			}
		}
		std::vector<sc::Statement> add;
		for (const sc::Statement& st : statements)
			if (st.kind != sc::Statement::Kind::Header && st.kind != sc::Statement::Kind::Meta) add.push_back(st);
		sc::Merge(s, add);
		scriptText_ = sc::Print(s);
		scriptEditor_.invalidate();
		if (show) showScript_ = bringScript_ = true;
		return true;
	}

	void App::addCollectionsToScript(const std::vector<Collection*>& roots, bool withChildren, const sc::ExportSettings& settings)
	{
		if (!db_) return;
		std::vector<const Collection*> list;
		std::function<void(const Collection*)> add = [&](const Collection* c)
		{
			if (std::find(list.begin(), list.end(), c) == list.end()) list.push_back(c);
			if (withChildren)
				for (const Collection* k : c->children) add(k);
		};
		for (const Collection* c : roots) add(c);
		const std::vector<sc::Statement> st = sc::Export(*db_, list, settings);
		if (addToScript(st))
			log(LogLevel::Info, "Added " + std::to_string(st.size()) + " collection" + (st.size() == 1 ? "" : "s") + " to the script");
	}

	bool App::requestApplyScript(const std::string& text, const std::string& source, const std::string& folder)
	{
		try
		{
			// The included scripts too, for their options.
			pendingScript_ = std::make_unique<sc::Script>(sc::Parse(text, source, folder));
			sc::LoadIncludes(*pendingScript_);
		}
		catch (const sc::ScriptError& e)
		{
			pendingScript_.reset();
			scriptStatus_ = e.what();
			scriptStatusError_ = true;
			log(LogLevel::Error, e.what());
			if (&text == &scriptText_)
			{
				scriptMessages_ = { sc::Message::Error(e) };
				markScriptLines();
			}
			return false;
		}
		// The options start at their defaults.
		scriptOptions_.clear();
		try
		{
			for (const auto& [id, v] : sc::ResolveOptions(*pendingScript_, {})) scriptOptions_[id] = v->text;
		}
		catch (const sc::ScriptError& e)
		{
			scriptStatus_ = e.what();
			scriptStatusError_ = true;
			log(LogLevel::Error, e.what());
			pendingScript_.reset();
			return false;
		}
		scriptApplyError_.clear();
		request(Pending::ApplyScript);
		return true;
	}

	sc::ApplyReport App::applyScript(const sc::Script& s, const sc::ApplySettings& settings, bool dryRun)
	{
		sc::ApplyReport fail;
		if (!db_ || db_->packs().empty())
		{
			fail.messages.push_back({ sc::Message::Level::Error, {}, "Open a database first." });
			return fail;
		}
		// Tried on a copy first: a mistake halfway leaves the database as it was.
		std::unique_ptr<Database> copy;
		try
		{
			copy = sc::CloneDatabase(*db_);
		}
		catch (const std::exception& e)
		{
			fail.messages.push_back({ sc::Message::Level::Error, {}, std::string("Could not copy the database to try the script: ") + e.what() });
			return fail;
		}
		sc::ApplySettings trialSettings = settings;
		trialSettings.beforeRemove = nullptr;
		sc::ApplyReport trial = sc::Apply(*copy, s, trialSettings);
		if (!trial.ok && !dryRun)
			for (const sc::Message& m : trial.messages)
				if (m.level == sc::Message::Level::Error) log(LogLevel::Error, MessageText(s.source, m, s.folder) + " (nothing was changed)");
		if (!trial.ok || dryRun) return trial;

		sc::ApplySettings st = settings;
		st.beforeRemove = [this](Collection& c)
		{
			forgetCollection(&c);
		};
		nameAction("Apply " + s.source);
		beginScriptRun();
		sc::ApplyReport r = sc::Apply(*db_, s, st);
		endScriptRun();
		for (const std::string& n : r.newNames) rememberName(n);
		structureChanged();
		updateTitle();
		for (const sc::Message& m : r.messages)
		{
			if (m.level == sc::Message::Level::Echo) log(LogLevel::Info, s.source + ": " + m.text); // what the script printed
			else
				log(m.level == sc::Message::Level::Error ? LogLevel::Error : m.level == sc::Message::Level::Warning ? LogLevel::Warning : LogLevel::Info,
					MessageText(s.source, m, s.folder));
		}
		log(r.ok ? LogLevel::Info : LogLevel::Error, s.source + ": " + (r.ok ? r.summary() : std::string("stopped")));
		return r;
	}

	bool App::exportChanges(bool originals, bool recordWas)
	{
		if (!db_ || db_->packs().empty()) return false;
		sc::Script d;
		try
		{
			auto base = std::make_unique<Database>();
			base->setTarget(db_->target());
			if (db_->sharedTypeSchema()) base->addTypeSchema(db_->sharedTypeSchema());
			base->names() = db_->names();
			base->binNames() = db_->binNames();
			size_t fromBak = 0;
			for (const auto& pk : db_->packs())
			{
				std::string file = pk->path;
				std::error_code ec;
				if (originals && fs::exists(FromUtf8(pk->path + ".bak"), ec))
				{
					file = pk->path + ".bak";
					++fromBak;
				}
				// Under the pack's own path, so the two sides match up.
				base->loadPack(pk->path, ReadFile(file));
			}
			if (originals && !fromBak) log(LogLevel::Info, "No .bak originals yet (the first save makes them); compared with the saved files");
			sc::DiffSettings st;
			st.recordWas = recordWas;
			d = sc::Diff(*base, *db_, st);
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, std::string("Could not read the files to compare with: ") + e.what());
			return false;
		}
		size_t changes = 0;
		for (const sc::Statement& s : d.statements)
			changes += s.kind != sc::Statement::Kind::Header && s.kind != sc::Statement::Kind::Meta && s.kind != sc::Statement::Kind::Comment;
		if (!changes)
		{
			log(LogLevel::Info, std::string("No changes since the ") + (originals ? "originals" : "files were saved"));
			return false;
		}
		if (!addToScript(d.statements)) return false;
		log(LogLevel::Info, "Added " + std::to_string(changes) + " change" + (changes == 1 ? "" : "s") + " to the script");
		return true;
	}

	bool App::saveScript(bool as)
	{
		std::string path = scriptPath_;
		if (as || path.empty())
		{
			auto p = host_.saveFileDialog("Save script", path.empty() ? std::string("mod.amod") : path, { {"Mod scripts", "*.amod"} });
			if (!p) return false;
			path = *p;
			if (FromUtf8(path).extension().empty()) path += sc::kExtension;
		}
		std::ofstream out(FromUtf8(path), std::ios::binary | std::ios::trunc);
		out << scriptText_;
		if (!out)
		{
			log(LogLevel::Error, "Could not save " + path);
			return false;
		}
		scriptPath_ = path;
		scriptSaved_ = scriptText_;
		log(LogLevel::Info, "Saved the script to " + path);
		return true;
	}

	bool App::loadScriptFile(const std::string& path, std::string& text)
	{
		std::ifstream in(FromUtf8(path), std::ios::binary);
		if (!in)
		{
			log(LogLevel::Error, "Could not read " + path);
			return false;
		}
		std::ostringstream s;
		s << in.rdbuf();
		text = s.str();
		return true;
	}

	// Errors and warnings at their lines in the Script panel's editor.
	void App::markScriptLines()
	{
		std::map<int, std::string> errors, warnings;
		for (const sc::Message& m : scriptMessages_)
		{
			if (!m.pos.line) continue;
			if (m.level == sc::Message::Level::Error) errors[m.pos.line] = m.text;
			else if (m.level == sc::Message::Level::Warning) warnings[m.pos.line] = m.text;
		}
		scriptEditor_.setMarks(std::move(errors), std::move(warnings));
	}

	void App::checkScript()
	{
		scriptMessages_.clear();
		sc::Script s;
		try
		{
			s = sc::Parse(scriptText_, scriptName(), scriptFolder());
		}
		catch (const sc::ScriptError& e)
		{
			scriptStatus_ = e.what();
			scriptStatusError_ = true;
			scriptMessages_.push_back({ sc::Message::Level::Error, e.pos, e.message });
			markScriptLines();
			return;
		}
		const sc::ApplyReport r = applyScript(s, {}, true);
		scriptMessages_ = r.messages;
		markScriptLines();
		scriptStatusError_ = !r.ok;
		scriptStatus_ = r.ok ? "Applies with its defaults: " + r.summary() + (r.warnings() ? " (" + std::to_string(r.warnings()) + " warning(s))" : "")
			: "Doesn't apply; see below";
	}

	void App::drawScriptPanel()
	{
		if (!showScript_) return;
		if (ImGuiWindow* logWindow = ImGui::FindWindowByName("Log"); logWindow && logWindow->DockId)
			ImGui::SetNextWindowDockID(logWindow->DockId, ImGuiCond_FirstUseEver);
		if (bringScript_)
		{
			ImGui::SetNextWindowFocus();
			bringScript_ = false;
		}
		if (!ImGui::Begin(kWindowScript, &showScript_))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Other;
		const bool dirty = scriptText_ != scriptSaved_;

		if (ImGui::Button("New"))
		{
			if (dirty && !Blank(scriptText_)) ImGui::OpenPopup("##discard");
			else setScriptText({});
		}
		if (ImGui::BeginPopup("##discard"))
		{
			ImGui::TextUnformatted("Discard the unsaved script?");
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(); // Enter keeps it
			if (ImGui::Button("Keep") || CancelKey()) ImGui::CloseCurrentPopup();
			ImGui::SameLine(0, ImGui::GetFontSize() * 2);
			if (ImGui::Button("Discard"))
			{
				setScriptText({});
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Open..."))
		{
			if (auto p = host_.openFileDialog("Open script", { {"Mod scripts", "*.amod"}, {"All files", "*.*"} }))
			{
				std::string text;
				if (loadScriptFile(*p, text)) setScriptText(std::move(text), *p);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Save")) saveScript(false);
		ImGui::SameLine();
		if (ImGui::Button("Save As...")) saveScript(true);
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::BeginDisabled(!db_ || Blank(scriptText_));
		if (ImGui::Button("Check")) checkScript();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Tries the script on a copy of the database, with its options at their defaults");
		ImGui::SameLine();
		if (ImGui::Button("Apply...")) requestApplyScript(scriptText_, scriptName(), scriptFolder());
		ImGui::EndDisabled();
		ImGui::SameLine();
		bool record = recordAmod_;
		if (ImGui::Checkbox("Record changes", &record)) setRecording(record, recordNfsms_);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Adds what you change in the editor to this script as you go: values, new, renamed, moved and deleted collections");
		ImGui::SameLine();
		ImGui::TextDisabled("%s%s", scriptPath_.empty() ? "(not saved)" : scriptPath_.c_str(), dirty ? " *" : "");

		if (!scriptStatus_.empty()) ImGui::TextColored(scriptStatusError_ ? kError : kGood, "%s", scriptStatus_.c_str());
		else if (Blank(scriptText_))
			ImGui::TextDisabled("Write a script here, open one, or use Add to Script on a collection, a class or a field.");
		if (!scriptMessages_.empty())
		{
			// A click goes to the line.
			const float h = std::min(float(scriptMessages_.size()), 5.0f) * ImGui::GetTextLineHeightWithSpacing();
			if (ImGui::BeginChild("##messages", ImVec2(0, h), ImGuiChildFlags_None))
			{
				for (size_t i = 0; i < scriptMessages_.size(); ++i)
				{
					const sc::Message& m = scriptMessages_[i];
					ImGui::PushID(int(i));
					ImGui::PushStyleColor(ImGuiCol_Text, m.level == sc::Message::Level::Error ? kError
										  : m.level == sc::Message::Level::Warning ? kWarn
										  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					if (ImGui::Selectable(MessageText("line", m, scriptFolder()).c_str()) && m.pos.line) scriptEditor_.gotoLine(m.pos.line);
					if (!m.file.empty() && ImGui::IsItemHovered())
						ImGui::SetTooltip("Line %d of %s, included on line %d", m.fileLine, m.file.c_str(), m.pos.line);
					ImGui::PopStyleColor();
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
		}
		// The script, then the console line under it.
		const float consoleH = ImGui::GetFrameHeightWithSpacing() * (consoleStatus_.empty() ? 1.0f : 2.0f);
		if (codeFont_) ImGui::PushFont(codeFont_, 0.0f);
		if (scriptEditor_.draw("##script", scriptText_, ImVec2(-FLT_MIN, -consoleH), ImGuiInputTextFlags_AllowTabInput))
			scriptEditor_.clearMarks();
		if (codeFont_) ImGui::PopFont();
		drawConsole();
		ImGui::End();
	}

	namespace
	{
		int ConsoleHistory(ImGuiInputTextCallbackData* data)
		{
			auto* self = static_cast<App*>(data->UserData);
			return self->consoleHistoryStep(data);
		}
	} // namespace

	int App::consoleHistoryStep(ImGuiInputTextCallbackData* data)
	{
		if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || consoleHistory_.empty()) return 0;
		const int n = int(consoleHistory_.size());
		if (data->EventKey == ImGuiKey_UpArrow) consoleHistoryPos_ = consoleHistoryPos_ < 0 ? n - 1 : std::max(0, consoleHistoryPos_ - 1);
		else if (data->EventKey == ImGuiKey_DownArrow) consoleHistoryPos_ = consoleHistoryPos_ < 0 || consoleHistoryPos_ + 1 >= n ? -1 : consoleHistoryPos_ + 1;
		const std::string& s = consoleHistoryPos_ < 0 ? std::string() : consoleHistory_[size_t(consoleHistoryPos_)];
		data->DeleteChars(0, data->BufTextLen);
		data->InsertChars(0, s.c_str());
		return 0;
	}

	void App::drawConsole()
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(">");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (focusConsole_)
		{
			ImGui::SetKeyboardFocusHere();
			focusConsole_ = false;
		}
		ImGui::BeginDisabled(!db_);
		if (ImGui::InputTextWithHint("##console", "one change, run with Enter: edit pvehicle/street { HornType = 3 }", &consoleInput_,
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, ConsoleHistory, this))
		{
			runConsole(consoleInput_);
			focusConsole_ = true; // keep typing
		}
		TextFieldMenu(consoleInput_);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
			ImGui::SetTooltip("Statements typed here apply to the open database at once, like a one-line script. Up and Down recall earlier ones.");
		if (!consoleStatus_.empty()) ImGui::TextColored(consoleError_ ? kError : kGood, "%s", consoleStatus_.c_str());
	}

	bool App::runConsole(const std::string& line)
	{
		if (Blank(line) || !db_) return false;
		if (consoleHistory_.empty() || consoleHistory_.back() != line) consoleHistory_.push_back(line);
		if (consoleHistory_.size() > 100) consoleHistory_.erase(consoleHistory_.begin());
		consoleHistoryPos_ = -1;
		sc::Script s;
		try
		{
			s = sc::Parse(line, "console", scriptFolder()); // includes from the panel script's folder
		}
		catch (const sc::ScriptError& e)
		{
			consoleStatus_ = e.message;
			consoleError_ = true;
			log(LogLevel::Error, std::string("console: ") + e.message);
			return false;
		}
		sc::ApplySettings st;
		st.fragment = true;
		nameAction("Console: " + line);
		const sc::ApplyReport r = applyScript(s, st, false);
		consoleError_ = !r.ok;
		if (r.ok)
		{
			consoleStatus_ = r.summary();
			for (const sc::Message& m : r.messages)
				if (m.level == sc::Message::Level::Echo || m.level == sc::Message::Level::Warning) consoleStatus_ += "; " + m.text;
			consoleInput_.clear();
		}
		else
		{
			consoleStatus_.clear();
			for (const sc::Message& m : r.messages)
				if (m.level == sc::Message::Level::Error) consoleStatus_ = m.text + " (nothing changed)";
		}
		return r.ok;
	}

	void App::drawScriptModals()
	{
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		auto begin = [&](const char* name)
		{
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
		};
		auto done = [&]()
		{
			pending_ = Pending::None;
			pendingTarget_ = nullptr;
			ImGui::CloseCurrentPopup();
		};
		const float em = ImGui::GetFontSize();

		if (begin("Apply Script"))
		{
			if (!pendingScript_)
			{
				done();
				ImGui::EndPopup();
				return;
			}
			const sc::Script& s = *pendingScript_;
			ImGui::PushTextWrapPos(em * 34);
			const std::string title = s.meta("title");
			ImGui::TextUnformatted(title.empty() ? s.source.c_str() : title.c_str());
			std::string by;
			if (!s.meta("author").empty()) by = "by " + s.meta("author");
			if (!s.meta("version").empty()) by += (by.empty() ? "version " : ", version ") + s.meta("version");
			if (!by.empty()) ImGui::TextDisabled("%s", by.c_str());
			if (!s.meta("description").empty()) ImGui::TextWrapped("%s", s.meta("description").c_str());
			std::vector<std::string> included;
			IncludedFiles(s.statements, included);
			if (!included.empty())
			{
				std::string list;
				for (const std::string& f : included) list += (list.empty() ? "" : ", ") + Shown(f, s.folder);
				ImGui::TextDisabled("Includes %s", list.c_str());
			}
			const std::string game = s.meta("game"), here = db_ ? sc::GameId(*db_) : std::string();
			const std::optional<ScriptTarget> written = ParseScriptTarget(game);
			if (!game.empty() && !here.empty() && !(written && written->fits(db_->target())))
				ImGui::TextColored(kWarn, "Written for %s; this database is %s.", game.c_str(), here.c_str());

			// The configurator: one control per option.
			const auto options = s.options();
			if (!options.empty())
			{
				ImGui::Spacing();
				ImGui::SeparatorText("Options");
			}
			for (const sc::OptionDef* o : options)
			{
				ImGui::PushID(o->id.c_str());
				std::string& v = scriptOptions_[o->id];
				const std::string label = o->label.empty() ? o->id : o->label;
				switch (o->type)
				{
					case sc::OptionDef::Type::Bool:
					{
						bool on = v == "true";
						if (ImGui::Checkbox(label.c_str(), &on)) v = on ? "true" : "false";
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
			ImGui::Spacing();
			ImGui::Checkbox("Stop when a value isn't what the script expects", &scriptStrict_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Values written as `x = new was old` warn when this database doesn't have the old value; with this on, they stop the script.");
			if (!scriptApplyError_.empty())
			{
				ImGui::Spacing();
				ImGui::TextColored(kError, "%s", scriptApplyError_.c_str());
			}
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			bool apply = ImGui::Button("Apply");
			ImGui::SameLine();
			const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			apply |= ConfirmKey();
			if (cancel)
			{
				pendingScript_.reset();
				done();
			}
			else if (apply)
			{
				sc::ApplySettings st;
				st.options = scriptOptions_;
				st.strict = scriptStrict_;
				const sc::ApplyReport r = applyScript(s, st, false);
				if (r.ok)
				{
					scriptStatus_ = s.source + ": applied, " + r.summary() + (r.warnings() ? " (" + std::to_string(r.warnings()) + " warning(s))" : "");
					scriptStatusError_ = false;
					scriptMessages_ = r.messages;
					markScriptLines();
					pendingScript_.reset();
					done();
				}
				else
				{
					// Nothing changed: the copy it was tried on stopped.
					scriptApplyError_.clear();
					for (const sc::Message& m : r.messages)
						if (m.level == sc::Message::Level::Error) scriptApplyError_ = MessageText(s.source, m, s.folder);
					if (scriptApplyError_.empty()) scriptApplyError_ = "The script stopped.";
					scriptApplyError_ += "\nNothing was changed.";
					scriptMessages_ = r.messages;
					markScriptLines();
				}
			}
			ImGui::EndPopup();
		}

		if (begin("Export Changes"))
		{
			ImGui::PushTextWrapPos(em * 30);
			ImGui::TextUnformatted("Adds the changes made to the database to the Script panel, as statements that make them again.");
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			ImGui::TextUnformatted("Changes since");
			int which = exportOriginals_ ? 1 : 0;
			ImGui::RadioButton("the files were saved", &which, 0);
			ImGui::RadioButton("the originals (the .bak files the first save kept)", &which, 1);
			exportOriginals_ = which == 1;
			ImGui::Checkbox("Record the old values (was)", &exportWas_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Applying the script then warns where a database doesn't have the value this one had.");
			ImGui::Spacing();
			bool go = ImGui::Button("Add to Script");
			ImGui::SameLine();
			const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			go |= ConfirmKey();
			if (cancel) done();
			else if (go)
			{
				exportChanges(exportOriginals_, exportWas_);
				done();
			}
			ImGui::EndPopup();
		}

		if (begin("Add Class to Script"))
		{
			ClassDef* cls = pendingClass_;
			if (!cls || !db_)
			{
				done();
				ImGui::EndPopup();
				return;
			}
			const KeyNames& names = db_->names();
			ImGui::Text("Every %s collection (%zu), with", names.format(cls->key).c_str(), cls->collections.size());
			const bool all = exportFields_.empty();
			if (ImGui::RadioButton("all its own values", all)) exportFields_.clear();
			ImGui::SameLine();
			ImGui::RadioButton("only these fields:", !all);
			if (ImGui::IsWindowAppearing()) exportFilter_.clear();
			ImGui::SetNextItemWidth(em * 22);
			ImGui::InputTextWithHint("##fieldfilter", "filter fields", &exportFilter_);
			if (ImGui::BeginChild("##fields", ImVec2(em * 22, em * 14), ImGuiChildFlags_Borders))
			{
				const std::string f = ToLower(exportFilter_);
				for (const FieldDef* fd : sortedFields_[cls])
				{
					const std::string n = names.format(fd->key);
					if (!f.empty() && ToLower(n).find(f) == std::string::npos) continue;
					bool on = exportFields_.count(fd->key) != 0;
					ImGui::PushID(fd);
					if (ImGui::Checkbox(n.c_str(), &on))
					{
						if (on) exportFields_.insert(fd->key);
						else exportFields_.erase(fd->key);
					}
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			ImGui::BeginDisabled(!exportFields_.empty());
			ImGui::Checkbox("Exact: applying it reverts own values it doesn't list", &exportExact_);
			ImGui::EndDisabled();
			ImGui::Checkbox("Edits only (the collections must exist where it's applied)", &exportEditOnly_);
			ImGui::Spacing();
			bool go = ImGui::Button("Add to Script");
			ImGui::SameLine();
			const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			go |= ConfirmKey();
			if (cancel) done();
			else if (go)
			{
				sc::ExportSettings st;
				st.fields = exportFields_;
				st.exact = exportExact_ && exportFields_.empty();
				st.editOnly = exportEditOnly_;
				addCollectionsToScript(std::vector<Collection*>(cls->collections.begin(), cls->collections.end()), false, st);
				done();
			}
			ImGui::EndPopup();
		}
	}

} // namespace app
