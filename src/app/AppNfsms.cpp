// nfsms in the editor (core/Nfsms.h): its own window to edit and run
// NFS-VltEd / Attribulator scripts line by line or whole, the setup prompt
// those scripts can ask for, following the line under the caret into the
// collection panel, and recording changes made in the editor as script lines
// (for this window and the Script panel).

#include "App.h"

#include "core/Nfsms.h"
#include "core/Script.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace app
{

	using namespace attrib;
	namespace sc = attrib::script;
	namespace ns = attrib::nfsms;
	namespace fs = std::filesystem;

	namespace
	{

		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);
		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
		const char* kWindowNfsms = "Script (nfsms)";

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

		// The line text without its last token (an update_field's value).
		std::string WithoutValue(const std::string& line)
		{
			const ns::Command c = ns::ParseLine(line);
			if (c.name != "update_field" || c.args.size() < 4) return {};
			std::string s = c.name;
			for (size_t i = 0; i + 1 < c.args.size(); ++i) s += " " + c.args[i];
			return s;
		}

	} // namespace

	// ----------------------------------------------------------- recording ---

	std::string App::nfsmsUnavailable() const
	{
		return db_ ? ns::Incompatibility(*db_) : std::string();
	}

	void App::setRecording(bool amod, bool nfsms)
	{
		recordAmod_ = amod;
		recordNfsms_ = nfsms;
		if (!amod && !nfsms) recordBase_.reset();
		ImGui::MarkIniSettingsDirty();
	}

	bool App::copyForRecording()
	{
		try
		{
			recordBase_ = sc::CloneDatabase(*db_);
			return true;
		}
		catch (const std::exception& e)
		{
			// A pack that writes what doesn't read back stops the recording, not
			// the program: the copy threw uncaught when attributes.bin had failed
			// to load and the folder's other packs had loaded without it
			// (crashdumps/crash5).
			setRecording(false, false);
			log(LogLevel::Error, std::string("Recording stopped, since the database could not be copied to compare changes with. ") + e.what());
			return false;
		}
	}

	void App::beginScriptRun()
	{
		++recordSuppress_;
		undoScript_ = true;
	}

	void App::endScriptRun()
	{
		if (--recordSuppress_ > 0) return;
		// What a script changed isn't recorded: the copy starts again from here.
		if (recordBase_ && db_) copyForRecording();
		recordDirty_.clear();
		recordStructure_ = false;
	}

	void App::flushRecording()
	{
		if (!(recordAmod_ || recordNfsms_) || !db_ || db_->packs().empty())
		{
			recordBase_.reset();
			recordDirty_.clear();
			recordStructure_ = false;
			return;
		}
		if (recordSuppress_) return;
		if (!recordBase_)
		{
			// The state changes are measured against.
			copyForRecording();
			recordDirty_.clear();
			recordStructure_ = false;
			return;
		}
		std::vector<sc::Statement> out;
		if (recordStructure_)
		{
			// Made, deleted, renamed, moved: by comparing keys and parents.
			sc::DiffSettings st;
			st.values = false;
			for (sc::Statement& s : sc::Diff(*recordBase_, *db_, st).statements)
				if (s.kind != sc::Statement::Kind::Header && s.kind != sc::Statement::Kind::Meta && s.kind != sc::Statement::Kind::Comment)
					out.push_back(std::move(s));
			recordStructure_ = false;
		}
		for (uint64_t id : recordDirty_)
		{
			const Collection* c = db_->findCollection(uint32_t(id >> 32), uint32_t(id));
			const Collection* b = recordBase_->findCollection(uint32_t(id >> 32), uint32_t(id));
			if (!c || !b) continue; // new ones came with all their values
			sc::Statement e = sc::DiffValues(*recordBase_, *b, *db_, *c, true);
			if (!e.body.empty()) out.push_back(std::move(e));
		}
		recordDirty_.clear();
		if (out.empty()) return;
		// The nfsms lines against the copy as it was: a value a collection only
		// inherited there gets add_field first, as NFS-VltEd needs.
		std::vector<std::string> nfsmsLines;
		if (recordNfsms_ && nfsmsUnavailable().empty())
			nfsmsLines = ns::FromStatements(*db_, out, ns::NamesFor(*db_, nfsmsNames_).get(), nfsmsVltEd_, recordBase_.get());
		// The copy follows, so the next change is measured from here.
		sc::Script sync;
		sync.statements = out;
		sc::ApplySettings st;
		st.fragment = true;
		if (!sc::Apply(*recordBase_, sync, st).ok) copyForRecording();
		if (recordAmod_) addToScript(out, false);
		if (!nfsmsLines.empty()) appendNfsms(nfsmsLines);
	}

	// ---------------------------------------------------------------- window ---

	void App::setNfsmsText(std::string text, std::string path)
	{
		nfsmsText_ = std::move(text);
		nfsmsSaved_ = nfsmsText_;
		nfsmsPath_ = std::move(path);
		nfsmsMessages_.clear();
		nfsmsStatus_.clear();
		nfsmsEditor_.invalidate();
		nfsmsEditor_.resetHistory();
		nfsmsEditor_.clearMarks();
		showNfsms_ = bringNfsms_ = true;
	}

	void App::appendNfsms(const std::vector<std::string>& lines)
	{
		for (const std::string& l : lines)
		{
			// A value changed again (a slider being dragged): one line, the last value.
			const std::string key = WithoutValue(l);
			if (!key.empty())
			{
				std::string body = nfsmsText_;
				while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
				const size_t start = body.find_last_of('\n') == std::string::npos ? 0 : body.find_last_of('\n') + 1;
				if (WithoutValue(body.substr(start)) == key)
				{
					nfsmsText_ = body.substr(0, start) + l + "\n";
					continue;
				}
			}
			if (!nfsmsText_.empty() && nfsmsText_.back() != '\n') nfsmsText_ += "\n";
			nfsmsText_ += l + "\n";
		}
		nfsmsEditor_.invalidate();
	}

	// Where file_copy and folder_create write: the game's folder (above GLOBAL).
	std::string App::gameFolder() const
	{
		const std::string folder = databaseFolder();
		if (folder.empty()) return {};
		const fs::path p = FromUtf8(folder);
		if (ToLower(ToUtf8(p.filename())) == "global") return ToUtf8(p.parent_path());
		return folder;
	}

	bool App::runNfsms(int firstLine, int lastLine)
	{
		if (!db_ || db_->packs().empty())
		{
			nfsmsStatus_ = "Open a database first.";
			nfsmsStatusError_ = true;
			return false;
		}
		// Lines of the tab in front; all of the window's script.
		const std::string file = firstLine ? nfsmsActive_ : std::string();
		const std::string source = !file.empty() ? ToUtf8(FromUtf8(file).filename())
			: nfsmsPath_.empty() ? std::string("nfsms window")
			: ToUtf8(FromUtf8(nfsmsPath_).filename());
		ns::Script s = ns::Parse(nfsmsTextOf(file), source, nfsmsFolderOf(file)); // bad lines are reported as they run
		s.path = file;
		// Choices made for another script don't carry over.
		if (nfsmsChoicesFor_ != source + "|" + std::to_string(s.controls.size()))
		{
			nfsmsChoices_ = ns::DefaultChoices(s);
			nfsmsChoicesFor_ = source + "|" + std::to_string(s.controls.size());
		}
		if (firstLine == 0 && s.hasSetup())
		{
			// Ask first.
			pendingNfsms_ = std::make_unique<ns::Script>(std::move(s));
			releaseSetupImages();
			for (const std::string& img : pendingNfsms_->images)
			{
				SetupImage si;
				si.path = img;
				std::vector<unsigned char> rgba;
				if (host_.decodeImage(ns::ScriptFile(pendingNfsms_->folder, img), si.w, si.h, rgba))
				{
					si.tex = IM_NEW(ImTextureData)();
					si.tex->Create(ImTextureFormat_RGBA32, si.w, si.h);
					std::memcpy(si.tex->GetPixels(), rgba.data(), rgba.size());
					si.tex->UseColors = true;
					ImGui::RegisterUserTexture(si.tex);
				}
				setupImages_.push_back(si);
			}
			request(Pending::NfsmsSetup);
			return true;
		}
		return runNfsmsScript(s, firstLine, lastLine);
	}

	bool App::runNfsmsScript(const ns::Script& s, int firstLine, int lastLine)
	{
		ns::RunSettings st;
		st.choices = nfsmsChoices_;
		st.firstLine = firstLine;
		st.lastLine = lastLine;
		st.targetFolder = gameFolder();
		st.strict = nfsmsStrict_;
		st.vltEd = nfsmsVltEd_;
		st.names = ns::NamesFor(*db_, nfsmsNames_);
		// An included script open in a tab runs as the tab has it, saved or not.
		st.readFile = [this](const std::string& path) -> std::string
		{
			if (const std::string* t = nfsmsIncludedText(path)) return *t;
			std::ifstream in(FromUtf8(path), std::ios::binary);
			if (!in) throw std::runtime_error("can't read " + path);
			std::ostringstream ss;
			ss << in.rdbuf();
			return ss.str();
		};
		nfsmsRunFile_ = s.path;
		sc::ApplyReport r;
		bool changed = true;
		if (nfsmsStrict_)
		{
			// Strict: on a copy first, so a failing line leaves the database as it was.
			std::unique_ptr<Database> copy;
			try
			{
				copy = sc::CloneDatabase(*db_);
			}
			catch (const std::exception& e)
			{
				nfsmsStatus_ = std::string("Could not copy the database to try the script: ") + e.what();
				nfsmsStatusError_ = true;
				return false;
			}
			r = ns::Run(*copy, s, st);
			changed = r.ok;
		}
		if (changed)
		{
			// Off (NFS-VltEd's way): line by line, a line that fails is reported
			// and the next one runs.
			st.doFiles = firstLine == 0; // installer steps only when the whole script runs
			st.beforeRemove = [this](Collection& c)
			{
				forgetCollection(&c);
			};
			nameAction(firstLine ? "Run nfsms line " + std::to_string(firstLine) + (lastLine > firstLine ? "-" + std::to_string(lastLine) : "")
					   : "Run " + s.source);
			beginScriptRun();
			r = ns::Run(*db_, s, st);
			endScriptRun();
			for (const std::string& n : r.newNames) rememberName(n);
			structureChanged();
			updateTitle();
		}
		nfsmsMessages_ = r.messages;
		for (const sc::Message& m : r.messages)
			log(m.level == sc::Message::Level::Error ? LogLevel::Error : m.level == sc::Message::Level::Warning ? LogLevel::Warning : LogLevel::Info,
				s.source + ":" + std::to_string(m.pos.line) + ": " + m.text);
		// The included script the first error is in, in a tab of its own.
		for (const sc::Message& m : r.messages)
			if (m.level == sc::Message::Level::Error)
			{
				if (!m.file.empty()) openNfsmsIncluded(m.file, m.fileLine);
				break;
			}
		markNfsmsLines();
		const std::string what = firstLine == 0 ? std::string("the script") : firstLine == lastLine ? "line " + std::to_string(firstLine)
			: "lines " + std::to_string(firstLine) + "-" + std::to_string(lastLine);
		const size_t failed = r.errors();
		nfsmsStatusError_ = failed > 0;
		if (!changed)
			nfsmsStatus_ = "Stopped in " + what + " (strict); nothing was changed";
		else
			nfsmsStatus_ = "Ran " + what + ": " + r.summary() + (failed ? "; " + std::to_string(failed) + " line" + (failed == 1 ? "" : "s") + " failed" : "") +
			(r.warnings() ? " (" + std::to_string(r.warnings()) + " warning(s))" : "");
		log(failed ? LogLevel::Warning : LogLevel::Info, s.source + ": " + nfsmsStatus_);
		return failed == 0;
	}

	// The line under the caret opens its collection, with the value it sets.
	void App::followNfsmsLine()
	{
		CodeEditor& ed = nfsmsEditorOf(nfsmsActive_);
		if (!nfsmsFollow_ || !db_ || !ed.cursorLineChanged()) return;
		goToNfsmsCaret(true);
	}

	void App::goToNfsmsCaret(bool follow)
	{
		auto status = [&](const std::string& text, bool error)
		{
			if (follow) return;
			nfsmsStatus_ = text;
			nfsmsStatusError_ = error;
		};
		if (!db_)
		{
			status("Open a database first.", true);
			return;
		}
		const std::string file = nfsmsActive_;
		CodeEditor& ed = nfsmsEditorOf(file);
		const std::string& text = nfsmsTextOf(file);
		const size_t at = std::min(ed.cursorOffset(), text.size());
		const size_t nl = at == 0 ? std::string::npos : text.rfind('\n', at - 1);
		const size_t begin = nl == std::string::npos ? 0 : nl + 1;
		size_t end = text.find('\n', at);
		if (end == std::string::npos) end = text.size();
		const ns::Command c = ns::ParseLine(std::string_view(text).substr(begin, end - begin), ed.cursorLine());
		if (c.name.empty())
		{
			status("This line has no command.", true);
			return;
		}
		// The setup's controls only when they're asked for: Follow line runs at each caret move.
		const bool option = !follow && c.name == "ui_option";
		const ns::Script s = option ? ns::Parse(text, "script", nfsmsFolderOf(file)) : [&]
		{
			ns::Script folderOnly;
			folderOnly.folder = nfsmsFolderOf(file);
			return folderOnly;
		}();
		ns::RunSettings st;
		st.vltEd = nfsmsVltEd_;
		st.names = ns::NamesFor(*db_, nfsmsNames_);
		const ns::LineTarget t = ns::TargetOfLine(*db_, s, c, follow ? 0 : at - begin, st);
		if (!t.file.empty())
		{
			if (follow) return;
			if (openNfsmsIncluded(t.file, 1, true)) status("Opened " + t.file, false);
			else status("Can't read " + t.file, true);
			return;
		}
		if (t.controlLine)
		{
			if (follow) return;
			ed.gotoLine(t.controlLine);
			status("Its ui_control is on line " + std::to_string(t.controlLine), false);
			return;
		}
		if (!t.collection)
		{
			status(t.why.empty() ? "Nothing on this line names a collection." : t.why, true);
			return;
		}
		const std::string shown = showValue(t.collection, t.hasPath ? &t.path : nullptr);
		status("Went to " + shown, false);
	}

	bool App::convertNfsms()
	{
		if (!db_ || db_->packs().empty())
		{
			nfsmsStatus_ = "Open a database first.";
			nfsmsStatusError_ = true;
			return false;
		}
		const std::string source = nfsmsPath_.empty() ? std::string("nfsms window.nfsms") : ToUtf8(FromUtf8(nfsmsPath_).filename());
		const ns::Script s = ns::Parse(nfsmsText_, source, nfsmsFolderOf({}));
		ns::RunSettings st;
		st.vltEd = nfsmsVltEd_;
		st.names = ns::NamesFor(*db_, nfsmsNames_);
		st.readFile = [this](const std::string& path) -> std::string
		{
			if (const std::string* t = nfsmsIncludedText(path)) return *t;
			std::ifstream in(FromUtf8(path), std::ios::binary);
			if (!in) throw std::runtime_error("can't read " + path);
			std::ostringstream ss;
			ss << in.rdbuf();
			return ss.str();
		};
		sc::ApplyReport r;
		std::string text;
		try
		{
			// On a copy: converting runs every line, every setup block included.
			std::unique_ptr<Database> copy = sc::CloneDatabase(*db_);
			text = sc::Print(ns::ToModScript(*copy, s, st, &r));
		}
		catch (const std::exception& e)
		{
			nfsmsStatus_ = std::string("Could not convert: ") + e.what();
			nfsmsStatusError_ = true;
			return false;
		}
		setScriptText(std::move(text));
		const size_t failed = r.errors();
		nfsmsStatus_ = "Converted " + source + " into the Script panel" +
			(failed ? "; " + std::to_string(failed) + " line" + (failed == 1 ? "" : "s") + " couldn't be, and are comments there" : "");
		nfsmsStatusError_ = failed > 0;
		log(failed ? LogLevel::Warning : LogLevel::Info, nfsmsStatus_);
		return true;
	}

	std::vector<std::string> App::nfsmsLinesFor(Collection& c, uint32_t field, int element)
	{
		std::vector<std::string> out;
		if (!db_) return out;
		const sc::Statement e = sc::ExportValue(*db_, c, field);
		if (e.body.empty()) return out;
		const std::string fieldName = db_->names().format(field);
		for (const std::string& l : ns::FromStatements(*db_, { e }, ns::NamesFor(*db_, nfsmsNames_).get(), nfsmsVltEd_))
		{
			// Just the lines that set values (not resize_field, not comments).
			const ns::Command cmd = ns::ParseLine(l);
			if (cmd.name != "update_field" && cmd.name != "update_collection") continue;
			if (element >= 0 && (cmd.args.size() < 3 || cmd.args[2] != fieldName + "[" + std::to_string(element) + "]")) continue;
			out.push_back(l);
		}
		return out;
	}

	// The field menu's nfsms items: the field's value (or one element's) as update_field lines.
	void App::nfsmsMenuItems(Collection& c, uint32_t field, int element)
	{
		const std::string no = nfsmsUnavailable();
		if (ImGui::MenuItem(element >= 0 ? "Add Element to nfsms Script" : "Add to nfsms Script", nullptr, false, no.empty()))
		{
			const std::vector<std::string> lines = nfsmsLinesFor(c, field, element);
			if (lines.empty())
			{
				log(LogLevel::Warning, "nfsms has no update_field for this value");
			}
			else
			{
				appendNfsms(lines);
				showNfsms_ = bringNfsms_ = true;
			}
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", no.empty() ? "Adds update_field lines that set it to this value to the nfsms window" : (no + ".").c_str());
		if (ImGui::MenuItem(element >= 0 ? "Copy Element as nfsms" : "Copy as nfsms", nullptr, false, no.empty()))
		{
			std::string text;
			for (const std::string& l : nfsmsLinesFor(c, field, element)) text += l + "\n";
			ImGui::SetClipboardText(text.c_str());
		}
	}

	// --------------------------------------------------- included scripts ---

	CodeEditor& App::nfsmsEditorOf(const std::string& file)
	{
		for (auto& f : nfsmsFiles_)
			if (f->path == file) return *f->editor;
		return nfsmsEditor_;
	}

	std::string& App::nfsmsTextOf(const std::string& file)
	{
		for (auto& f : nfsmsFiles_)
			if (f->path == file) return f->text;
		return nfsmsText_;
	}

	// Where a script's `script`, `ui_image` and `file_copy` paths start.
	std::string App::nfsmsFolderOf(const std::string& file) const
	{
		if (!file.empty()) return ToUtf8(FromUtf8(file).parent_path());
		return nfsmsPath_.empty() ? databaseFolder() : ToUtf8(FromUtf8(nfsmsPath_).parent_path());
	}

	std::string* App::nfsmsIncludedText(const std::string& path)
	{
		for (auto& f : nfsmsFiles_)
			if (f->path == path) return &f->text;
		return nullptr;
	}

	bool App::openNfsmsIncluded(const std::string& path, int line, bool focus)
	{
		NfsmsFile* f = nullptr;
		for (auto& x : nfsmsFiles_)
			if (x->path == path) f = x.get();
		if (!f)
		{
			std::string text;
			if (!loadScriptFile(path, text)) return false;
			auto nf = std::make_unique<NfsmsFile>();
			nf->path = path;
			nf->name = ToUtf8(FromUtf8(path).filename());
			nf->text = nf->saved = std::move(text);
			nf->editor = std::make_unique<CodeEditor>(CodeLanguage::Nfsms);
			f = nf.get();
			nfsmsFiles_.push_back(std::move(nf));
			markNfsmsLines();
		}
		showNfsms_ = true;
		nfsmsSelect_ = path;
		if (line) f->editor->gotoLine(line, focus);
		return true;
	}

	// The last run's errors and warnings at their lines: in the script it ran,
	// in the included scripts open in tabs, and on the including line.
	void App::markNfsmsLines()
	{
		using Marks = std::pair<std::map<int, std::string>, std::map<int, std::string>>;
		std::map<std::string, Marks> marks;
		for (const sc::Message& m : nfsmsMessages_)
		{
			if (m.level != sc::Message::Level::Error && m.level != sc::Message::Level::Warning) continue;
			auto add = [&](const std::string& file, int line)
			{
				Marks& mk = marks[file];
				std::map<int, std::string>& to = m.level == sc::Message::Level::Error ? mk.first : mk.second;
				if (!to.count(line)) to[line] = m.text;
			};
			add(nfsmsRunFile_, m.pos.line);
			if (!m.file.empty()) add(m.file, m.fileLine);
		}
		nfsmsEditor_.setMarks(std::move(marks[""].first), std::move(marks[""].second));
		for (auto& f : nfsmsFiles_) f->editor->setMarks(std::move(marks[f->path].first), std::move(marks[f->path].second));
	}

	void App::drawNfsmsEditor(const std::string& file)
	{
		if (codeFont_) ImGui::PushFont(codeFont_, 0.0f);
		CodeEditor& ed = nfsmsEditorOf(file);
		ed.setGoTo("Go to Definition");
		if (ed.draw(file.empty() ? "##nfsms" : "##included", nfsmsTextOf(file), ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_AllowTabInput))
			ed.clearMarks();
		if (codeFont_) ImGui::PopFont();
		if (ed.goToRequested()) goToNfsmsCaret();
	}

	void App::drawNfsmsPanel()
	{
		// Textures of a closed setup prompt go once the renderer let them go.
		for (auto it = retiredTextures_.begin(); it != retiredTextures_.end();)
		{
			if ((*it)->Status == ImTextureStatus_Destroyed || (*it)->Status == ImTextureStatus_WantCreate)
			{
				ImGui::UnregisterUserTexture(*it);
				IM_DELETE(*it);
				it = retiredTextures_.erase(it);
			}
			else
			{
				++it;
			}
		}
		if (!showNfsms_) return;
		if (ImGuiWindow* logWindow = ImGui::FindWindowByName("Log"); logWindow && logWindow->DockId)
			ImGui::SetNextWindowDockID(logWindow->DockId, ImGuiCond_FirstUseEver);
		if (bringNfsms_)
		{
			ImGui::SetNextWindowFocus();
			bringNfsms_ = false;
		}
		if (!ImGui::Begin(kWindowNfsms, &showNfsms_))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Other;
		const bool dirty = nfsmsText_ != nfsmsSaved_;
		NfsmsFile* active = nullptr;
		for (auto& f : nfsmsFiles_)
			if (f->path == nfsmsActive_) active = f.get();
		if (!active) nfsmsActive_.clear();
		CodeEditor& activeEditor = nfsmsEditorOf(nfsmsActive_);
		const std::string& activeText = nfsmsTextOf(nfsmsActive_);

		if (ImGui::Button("New"))
		{
			setNfsmsText({});
			nfsmsSelect_ = "\n";
		}
		ImGui::SameLine();
		if (ImGui::Button("Open..."))
		{
			if (auto p = host_.openFileDialog("Open nfsms script", { {"nfsms scripts", "*.nfsms"}, {"All files", "*.*"} }))
			{
				std::string text;
				if (loadScriptFile(*p, text))
				{
					setNfsmsText(std::move(text), *p);
					nfsmsSelect_ = "\n";
				}
			}
		}
		ImGui::SameLine();
		// Save and Save As act on the tab in front.
		const bool save = ImGui::Button("Save");
		ImGui::SameLine();
		const bool saveAs = ImGui::Button("Save As...");
		if (save || saveAs)
		{
			std::string path = active ? active->path : nfsmsPath_;
			if (saveAs || path.empty())
			{
				auto p = host_.saveFileDialog("Save nfsms script", path.empty() ? std::string("mod.nfsms") : path, { {"nfsms scripts", "*.nfsms"} });
				path = p ? *p : std::string();
			}
			if (!path.empty())
			{
				std::ofstream out(FromUtf8(path), std::ios::binary | std::ios::trunc);
				out << activeText;
				if (!out)
				{
					log(LogLevel::Error, "Could not save " + path);
				}
				else if (active)
				{
					active->path = nfsmsActive_ = nfsmsSelect_ = path;
					active->name = ToUtf8(FromUtf8(path).filename());
					active->saved = active->text;
					log(LogLevel::Info, "Saved the included script " + path);
				}
				else
				{
					nfsmsPath_ = path;
					nfsmsSaved_ = nfsmsText_;
					log(LogLevel::Info, "Saved the nfsms script to " + path);
				}
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		const std::string no = nfsmsUnavailable();
		ImGui::BeginDisabled(!db_ || Blank(activeText) || !no.empty());
		int first = 0, last = 0;
		activeEditor.selectedLines(first, last);
		if (ImGui::Button(first == last ? "Run Line" : "Run Lines")) runNfsms(first, last);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(first == last ? "Runs line %d of the tab in front, the one with the caret" : "Runs the selected lines %d to %d of the tab in front",
							  first, last);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!db_ || Blank(nfsmsText_) || !no.empty());
		if (ImGui::Button("Run All")) runNfsms(0, 0);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Runs the whole script (the first tab), after its setup when it has one. A line that fails is marked; see Strict for what happens then.");
		ImGui::EndDisabled();
		// The script a `script` line includes, in a tab.
		const ns::Command caretLine = ns::ParseLine(activeEditor.currentLineText(activeText));
		const bool includes = caretLine.name == "script" && caretLine.args.size() == 1;
		ImGui::SameLine();
		ImGui::BeginDisabled(!includes);
		if (ImGui::Button("Open Included") && includes)
			openNfsmsIncluded(ns::ScriptFile(nfsmsFolderOf(nfsmsActive_), caretLine.args[0]), 0, true);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Opens the script a script line includes in a tab (put the caret on the line)");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!db_ || Blank(nfsmsText_) || !no.empty());
		if (ImGui::Button("To Mod Script"))
		{
			if (!Blank(scriptText_) && scriptText_ != scriptSaved_) nfsmsConvertAsk_ = true;
			else convertNfsms();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Writes the script as a mod script (.amod) in the Script panel: the setup as options, the scripts it includes in place");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		bool record = recordNfsms_ && no.empty();
		ImGui::BeginDisabled(!no.empty());
		if (Checkbox("Record changes", &record)) setRecording(recordAmod_, record);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Adds what you change in the editor to this script as nfsms commands, as NFS-VltEd does");
		ImGui::SameLine();
		if (Checkbox("Strict", &nfsmsStrict_)) ImGui::MarkIniSettingsDirty();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: warnings are errors. The script is tried on a copy of the database first and only applied when no line fails.\n"
							  "Off: it runs line by line on the open database, and a line that fails is reported while the rest run, as in NFS-VltEd.");
		ImGui::SameLine();
		if (Checkbox("NFS-VltEd compatible", &nfsmsVltEd_)) ImGui::MarkIniSettingsDirty();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kVltEdTip);
		ImGui::SameLine();
		if (Checkbox("Follow line", &nfsmsFollow_)) ImGui::MarkIniSettingsDirty();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Opens the collection the line with the caret names, with its field highlighted");
		ImGui::SameLine();
		if (active) ImGui::TextDisabled("%s%s", active->path.c_str(), active->text != active->saved ? " *" : "");
		else ImGui::TextDisabled("%s%s", nfsmsPath_.empty() ? "(not saved)" : nfsmsPath_.c_str(), dirty ? " *" : "");

		if (!no.empty()) ImGui::TextColored(kWarn, "%s. It opens here to read and edit, not to run.", no.c_str());
		else if (!nfsmsStatus_.empty()) ImGui::TextColored(nfsmsStatusError_ ? kError : kGood, "%s", nfsmsStatus_.c_str());
		else if (Blank(nfsmsText_))
			ImGui::TextDisabled("Open a .nfsms script, or turn on Record changes and edit the database.");
		if (!nfsmsMessages_.empty())
		{
			const float h = std::min(float(nfsmsMessages_.size()), 4.0f) * ImGui::GetTextLineHeightWithSpacing();
			if (ImGui::BeginChild("##messages", ImVec2(0, h)))
			{
				for (size_t i = 0; i < nfsmsMessages_.size(); ++i)
				{
					const sc::Message& m = nfsmsMessages_[i];
					ImGui::PushID(int(i));
					ImGui::PushStyleColor(ImGuiCol_Text, m.level == sc::Message::Level::Error ? kError
										  : m.level == sc::Message::Level::Warning ? kWarn
										  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					// A line of an included script opens that script at the line.
					if (ImGui::Selectable(("line " + std::to_string(m.pos.line) + ": " + m.text).c_str()))
					{
						if (!m.file.empty() && m.fileLine)
						{
							openNfsmsIncluded(m.file, m.fileLine, true);
						}
						else if (m.pos.line)
						{
							nfsmsSelect_ = nfsmsRunFile_.empty() ? std::string("\n") : nfsmsRunFile_;
							nfsmsEditorOf(nfsmsRunFile_).gotoLine(m.pos.line);
						}
					}
					if (!m.file.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("Opens %s at line %d", m.file.c_str(), m.fileLine);
					ImGui::PopStyleColor();
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
		}
		if (nfsmsFiles_.empty())
		{
			nfsmsActive_.clear();
			nfsmsSelect_.clear();
			drawNfsmsEditor({});
		}
		else if (ImGui::BeginTabBar("##nfsmstabs", ImGuiTabBarFlags_FittingPolicyScroll))
		{
			// The window's script, then the scripts it includes that are open.
			const std::string mainName = nfsmsPath_.empty() ? std::string("(new script)") : ToUtf8(FromUtf8(nfsmsPath_).filename());
			ImGuiTabItemFlags mainFlags = (dirty ? ImGuiTabItemFlags_UnsavedDocument : 0) | (nfsmsSelect_ == "\n" ? ImGuiTabItemFlags_SetSelected : 0);
			if (ImGui::BeginTabItem((mainName + "###main").c_str(), nullptr, mainFlags))
			{
				nfsmsActive_.clear();
				drawNfsmsEditor({});
				ImGui::EndTabItem();
			}
			std::string close;
			for (auto& f : nfsmsFiles_)
			{
				bool open = true;
				const ImGuiTabItemFlags flags = (f->text != f->saved ? ImGuiTabItemFlags_UnsavedDocument : 0) |
					(nfsmsSelect_ == f->path ? ImGuiTabItemFlags_SetSelected : 0);
				const bool shown = ImGui::BeginTabItem((f->name + "###" + f->path).c_str(), &open, flags);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f->path.c_str());
				if (shown)
				{
					nfsmsActive_ = f->path;
					drawNfsmsEditor(f->path);
					ImGui::EndTabItem();
				}
				if (!open) close = f->path;
			}
			ImGui::EndTabBar();
			nfsmsSelect_.clear();
			if (!close.empty())
			{
				NfsmsFile* f = nullptr;
				for (auto& x : nfsmsFiles_)
					if (x->path == close) f = x.get();
				if (f && f->text != f->saved)
				{
					nfsmsClosing_ = close; // ask first
				}
				else
				{
					std::erase_if(nfsmsFiles_, [&](const auto& x)
					{
						return x->path == close;
					});
				}
			}
		}
		// To Mod Script over unsaved text in the Script panel.
		if (nfsmsConvertAsk_) ImGui::OpenPopup("Replace the Script Panel's Script");
		if (ImGui::BeginPopupModal("Replace the Script Panel's Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("The Script panel has changes that aren't saved. The mod script replaces them.");
			bool close = false;
			if (ImGui::Button("Replace"))
			{
				convertNfsms();
				close = true;
			}
			ImGui::SameLine();
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(); // Enter keeps the Script panel's text
			close |= ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			close |= CancelKey();
			if (close)
			{
				nfsmsConvertAsk_ = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		// Closing an included script with unsaved changes.
		if (!nfsmsClosing_.empty()) ImGui::OpenPopup("Close Included Script");
		if (ImGui::BeginPopupModal("Close Included Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("%s has changes that aren't saved.", ToUtf8(FromUtf8(nfsmsClosing_).filename()).c_str());
			bool closeIt = false;
			const bool save = ImGui::Button("Save");
			ImGui::SameLine();
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(); // Enter keeps the tab
			bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			cancel |= CancelKey();
			ImGui::SameLine(0, ImGui::GetFontSize() * 2);
			if (ImGui::Button("Discard")) closeIt = true;
			if (save)
			{
				for (auto& x : nfsmsFiles_)
					if (x->path == nfsmsClosing_)
					{
						std::ofstream out(FromUtf8(x->path), std::ios::binary | std::ios::trunc);
						out << x->text;
						if (out) log(LogLevel::Info, "Saved the included script " + x->path);
						else log(LogLevel::Error, "Could not save " + x->path);
						closeIt = bool(out);
					}
			}
			if (closeIt) std::erase_if(nfsmsFiles_, [&](const auto& x)
			{
				return x->path == nfsmsClosing_;
			});
			if (closeIt || cancel)
			{
				nfsmsClosing_.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		followNfsmsLine();
		ImGui::End();
	}

	void App::releaseSetupImages()
	{
		for (SetupImage& si : setupImages_)
			if (si.tex)
			{
				si.tex->WantDestroyNextFrame = true;
				si.tex->SetStatus(ImTextureStatus_WantDestroy);
				retiredTextures_.push_back(si.tex);
			}
		setupImages_.clear();
	}

	void App::drawNfsmsModals()
	{
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		// Centred again while it grows: its scrolling child takes a few frames
		// to reach its height, and centring only on appearing left a long
		// setup's Run and Cancel below the screen (Undercover Reformed's).
		ImGui::SetNextWindowPos(vp->GetCenter(), setupResized_ ? ImGuiCond_Always : ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal("nfsms Setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			setupSize_ = ImVec2();
			setupResized_ = false;
			return;
		}
		const ImVec2 promptSize = ImGui::GetWindowSize();
		setupResized_ = promptSize.x != setupSize_.x || promptSize.y != setupSize_.y;
		setupSize_ = promptSize;
		auto done = [&]()
		{
			pending_ = Pending::None;
			releaseSetupImages();
			ImGui::CloseCurrentPopup();
		};
		if (!pendingNfsms_)
		{
			done();
			ImGui::EndPopup();
			return;
		}
		const ns::Script& s = *pendingNfsms_;
		const ImGuiStyle& style = ImGui::GetStyle();
		const float em = ImGui::GetFontSize();
		const float width = std::min(em * 44, vp->WorkSize.x * 0.9f);
		const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetTextLineHeightWithSpacing() * 2;
		ImGui::TextDisabled("%s", s.source.c_str());
		// The text and controls as NFS-VltEd's installer lays them out, scrolling
		// when the script says more than fits.
		ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, std::max(em * 8, vp->WorkSize.y * 0.85f - footer)));
		ImGui::BeginChild("##setup", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
		if (!setupImages_.empty())
		{
			// Pictures in a row, each fitted into 200x100 like NFS-VltEd's.
			const ImVec2 box(em * 15, em * 7.5f);
			float x = 0;
			for (const SetupImage& si : setupImages_)
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
					bool on = nfsmsChoices_.checkboxes[c.name];
					if (Checkbox(c.label.c_str(), &on)) nfsmsChoices_.checkboxes[c.name] = on;
					break;
				}
				case ns::SetupItem::Kind::Radios:
					for (int k : item.controls)
					{
						const ns::Control& c = s.controls[size_t(k)];
						ImGui::PushID(k);
						auto it = nfsmsChoices_.radios.find(c.group);
						if (RadioButton(c.label.c_str(), it != nfsmsChoices_.radios.end() && it->second == c.name))
							nfsmsChoices_.radios[c.group] = c.name;
						ImGui::PopID();
					}
					break;
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		if (!gameFolder().empty())
			for (const ns::Command& c : s.commands)
				if (c.name == "file_copy" || c.name == "folder_create")
				{
					ImGui::TextDisabled("Copies files into %s.", gameFolder().c_str());
					break;
				}
		bool run = ImGui::Button("Run");
		ImGui::SameLine();
		const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
		run |= ConfirmKey();
		if (cancel)
		{
			pendingNfsms_.reset();
			done();
		}
		else if (run)
		{
			const ns::Script script = *pendingNfsms_;
			pendingNfsms_.reset();
			done();
			runNfsmsScript(script, 0, 0);
		}
		ImGui::EndPopup();
	}

} // namespace app
