#include "App.h"

#include "core/ClassList.h"
#include "core/Edit.h"
#include "core/Folder.h"
#include "core/Hash.h"
#include "core/Writer.h"
#include "core/Version.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

namespace app
{

	using namespace attrib;
	namespace fs = std::filesystem;

	namespace
	{

		fs::path FromUtf8(const std::string& s)
		{
			return fs::path(std::u8string(s.begin(), s.end()));
		}
		std::string ToUtf8(const fs::path& p)
		{
			const std::u8string u = p.u8string();
			return std::string(u.begin(), u.end());
		}

		// The lines of a name list (attrib_keys.txt, user_keys.txt), without
		// empty ones and line ends. Throws when the file can't be read.
		std::vector<std::string> ReadNameLines(const std::string& path)
		{
			const Bytes data = ReadFile(path);
			std::vector<std::string> lines;
			size_t start = 0;
			for (size_t i = 0; i <= data.size(); ++i)
			{
				if (i < data.size() && data[i] != '\n') continue;
				size_t end = i;
				if (end > start && data[end - 1] == '\r') --end;
				if (end > start) lines.emplace_back(reinterpret_cast<const char*>(data.data()) + start, end - start);
				start = i + 1;
			}
			return lines;
		}

		// A path the way the recent list keeps it: absolute, without "." or ".."
		// and without a trailing separator.
		std::string NormalPath(const std::string& path)
		{
			std::error_code ec;
			fs::path p = fs::absolute(FromUtf8(path), ec);
			if (ec) p = FromUtf8(path);
			p = p.lexically_normal();
			if (!p.has_filename() && p.has_relative_path()) p = p.parent_path();
			return ToUtf8(p);
		}

		bool SameFile(const std::string& a, const std::string& b)
		{
			if (a == b) return true;
			std::error_code ec;
			if (fs::equivalent(FromUtf8(a), FromUtf8(b), ec)) return true;
#ifdef _WIN32
			return ToLower(a) == ToLower(b);
#else
			return false;
#endif
		}

		constexpr size_t kMaxRecent = 10;

		const ImVec4 kWarn(1.0f, 0.72f, 0.25f, 1.0f);
		const ImVec4 kError(1.0f, 0.42f, 0.38f, 1.0f);

		void DropSlotAt(DataBlock& b, uint32_t off)
		{
			auto& v = b.ptrs;
			v.erase(std::remove_if(v.begin(), v.end(), [&](const PtrSlot& s)
			{
				return s.offset == off;
			}), v.end());
		}

		const char* kWindowExplorer = "Explorer";
		const char* kWindowInspector = "Collection";
		const char* kWindowClass = "Class";
		const char* kWindowLog = "Log";
		const char* kWindowScript = "Script";
		const char* kWindowNfsms = "Script (nfsms)";

		// ImGui .ini handler for the app's own settings.
		void* SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* h, const char* name)
		{
			return std::strcmp(name, "Settings") == 0 ? h->UserData : nullptr;
		}

	} // namespace

	bool ConfirmKey()
	{
		const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		return enter && !ImGui::IsItemFocused() && !ImGui::IsWindowAppearing();
	}

	bool CancelKey()
	{
		const ImGuiContext& g = *GImGui;
		const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		return enter && !ImGui::IsWindowAppearing() && (!g.NavCursorVisible || g.NavId == ImGui::GetItemID());
	}

	bool NavEnterOn(ImGuiID id)
	{
		const ImGuiContext& g = *GImGui;
		return id && g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput);
	}

	// ------------------------------------------------------------------ setup ---

	App::App(Host& host) : host_(host)
	{
		// Tells this window's clipboard text from another window's.
		std::random_device rd;
		instanceId_ = (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^
			uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
	}

	App::~App()
	{
		// The .ini handler and io.IniFilename both point into this object: save
		// now and detach them, or a later DestroyContext() would save through a
		// dangling filename.
		if (ImGui::GetCurrentContext())
		{
			// Setup pictures (nfsms) the renderer may still hold.
			releaseSetupImages();
			for (ImTextureData* t : retiredTextures_)
			{
				ImGui::UnregisterUserTexture(t);
				IM_DELETE(t);
			}
			retiredTextures_.clear();
			ImGuiIO& io = ImGui::GetIO();
			if (io.IniFilename) ImGui::SaveIniSettingsToDisk(io.IniFilename);
			io.IniFilename = nullptr;
			ImGui::RemoveSettingsHandler("Attribulina");
		}
	}

	void App::setup(float dpiScale, const std::string& iniPath)
	{
		dpiScale_ = dpiScale;
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
		iniPath_ = iniPath;
		io.IniFilename = iniPath_.empty() ? nullptr : iniPath_.c_str();

		ImGuiSettingsHandler h;
		h.TypeName = "Attribulina";
		h.TypeHash = ImHashStr("Attribulina");
		h.UserData = this;
		h.ReadOpenFn = SettingsReadOpen;
		h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* hh, void*, const char* line)
		{
			auto* self = static_cast<App*>(hh->UserData);
			const std::string l(line);
			const auto eq = l.find('=');
			if (eq == std::string::npos) return;
			const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
			if (k == "KeysFile") self->settings_.oldKeysFile = v;
			else if (k == "Favorite")
			{
				unsigned ck = 0, key = 0;
				if (std::sscanf(v.c_str(), "%x:%x", &ck, &key) == 2) 
					self->settings_.oldFavorites.emplace_back(ck, key);
			}
			else if (k == "ShowAsColour" || k == "ShowAsNumbers")
			{
				if (!v.empty()) 
					self->prefs_.colour[v] = k == "ShowAsColour";
			}
			else if (k == "ColourRGBA" || k == "ColourARGB")
			{
				if (!v.empty()) self->prefs_.packing[v] = k == "ColourARGB" ? PackedColour::ARGB : PackedColour::RGBA;
			}
			else if (k == "ShowAsGraph" || k == "ShowAsRows")
			{
				if (!v.empty()) self->prefs_.graph[v] = k == "ShowAsGraph";
			}
			// Older versions kept the recent files here; recent_files.txt wins once it exists.
			else if (k == "Recent" && !v.empty() && self->settings_.recent.size() < kMaxRecent) self->settings_.recent.push_back(v);
			else if (k == "Theme")
			{
				if (const auto t = ParseTheme(v)) self->theme_ = *t;
			}
			// Older versions had a Light Theme switch.
			else if (k == "LightTheme") self->theme_ = v == "1" ? Theme::Light : Theme::Dark;
			else if (k == "WindowPlacement") self->settings_.windowPlacement = v;
			else if (k == "NfsmsFollow") self->nfsmsFollow_ = v == "1";
			else if (k == "NfsmsStrict") self->nfsmsStrict_ = v == "1";
			else if (k == "RecordScript") self->recordAmod_ = v == "1";
			else if (k == "RecordNfsms") self->recordNfsms_ = v == "1";
			else if (k == "OpenPanels") self->setOpenPanels(v);
			else if (k == "GameplayView") self->view_.gameplayView = v == "1";
			else if (k == "DetectGame") self->detectTarget_ = v != "0";
			else if (k == "ArraysPastLimit") self->arraysPastLimit_ = v == "1";
			else if (k == "SaveOnlyChanged") self->saveOnlyChanged_ = v == "1";
		};
		h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* hh, ImGuiTextBuffer* out)
		{
			auto* self = static_cast<App*>(hh->UserData);
			out->appendf("[%s][Settings]\n", hh->TypeName);
			out->appendf("Theme=%s\n", ThemeName(self->theme_));
			out->appendf("GameplayView=%d\n", self->view_.gameplayView ? 1 : 0);
			out->appendf("DetectGame=%d\n", self->detectTarget_ ? 1 : 0);
			out->appendf("ArraysPastLimit=%d\n", self->arraysPastLimit_ ? 1 : 0);
			out->appendf("SaveOnlyChanged=%d\n", self->saveOnlyChanged_ ? 1 : 0);
			out->appendf("OpenPanels=%s\n", self->openPanels().c_str());
			out->appendf("NfsmsFollow=%d\nNfsmsStrict=%d\nRecordScript=%d\nRecordNfsms=%d\n", self->nfsmsFollow_ ? 1 : 0,
				self->nfsmsStrict_ ? 1 : 0, self->recordAmod_ ? 1 : 0, self->recordNfsms_ ? 1 : 0);
			if (!self->settings_.windowPlacement.empty()) out->appendf("WindowPlacement=%s\n", self->settings_.windowPlacement.c_str());
			for (const auto& [name, colour] : self->prefs_.colour)
				out->appendf("%s=%s\n", colour ? "ShowAsColour" : "ShowAsNumbers", name.c_str());
			for (const auto& [name, order] : self->prefs_.packing)
				if (order != PackedColour::None) out->appendf("%s=%s\n", order == PackedColour::ARGB ? "ColourARGB" : "ColourRGBA", name.c_str());
			for (const auto& [name, graph] : self->prefs_.graph)
				out->appendf("%s=%s\n", graph ? "ShowAsGraph" : "ShowAsRows", name.c_str());
			out->append("\n");
		};
		ImGui::AddSettingsHandler(&h);
		if (io.IniFilename)
			ImGui::LoadIniSettingsFromDisk(io.IniFilename);
		loadRecent();
		moveOldFavorites();

		ApplyTheme(theme_);
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 3.0f;
		style.FrameRounding = 3.0f;
		style.GrabRounding = 3.0f;
		style.TabRounding = 3.0f;
		style.FontSizeBase = 15.0f;
		style.ScaleAllSizes(dpiScale);
		style.FontScaleDpi = dpiScale;

		ImFont* font = nullptr;

		const fs::path exe = FromUtf8(host_.executableDirectory());
		const fs::path fontPath = exe / "data" / "RobotoFlex.ttf";
		if (std::error_code ec; fs::exists(fontPath, ec))
			font = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str());

		if (!font)
			io.Fonts->AddFontDefaultVector();
		
		const fs::path codeFontPath = exe / "data" / "CascadiaCode.ttf";
		if (std::error_code ec; fs::exists(codeFontPath, ec))
			codeFont_ = io.Fonts->AddFontFromFileTTF(codeFontPath.string().c_str());
		if (!codeFont_) 
			codeFont_ = font ? io.Fonts->AddFontDefaultVector() : nullptr;

		// Where the user's own files are (UserFolder.h).
		const fs::path userDir = FromUtf8(host_.userFolder());
		if (std::error_code ec; userDir == exe || fs::equivalent(userDir, exe, ec))
			log(LogLevel::Info, "Settings and your names stay beside the program, in portable mode.");
		else
			log(LogLevel::Info, "Settings and your names go to " + ToUtf8(userDir) +
				". To keep them beside the program, put an attribulina.ini there.");

		// Key names: attrib_keys.txt beside the program, else in its data folder.
		for (const fs::path& p : { exe / "attrib_keys.txt", exe / "data" / "attrib_keys.txt" })
		{
			std::error_code ec;
			if (fs::exists(p, ec))
			{
				keysFile_ = ToUtf8(p);
				break;
			}
		}
		if (keysFile_.empty())
			log(LogLevel::Warning, "No attrib_keys.txt found; names will show as hashes until you load one (File > Load Key Names).");

		// Type schemas (data/schema/*.txt): layouts of the game's structs and enums.
		{
			std::error_code ec;
			const fs::path dir = FromUtf8(host_.executableDirectory()) / "data" / "schema";
			for (const auto& e : fs::directory_iterator(dir, ec))
			{
				if (!e.is_regular_file(ec) || ToLower(ToUtf8(e.path().extension())) != ".txt") continue;
				try
				{
					schemas_.push_back(TypeSchema::Load(ToUtf8(e.path())));
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Warning, std::string("Type schema: ") + ex.what());
				}
			}
			if (schemas_.empty())
				log(LogLevel::Warning, "No type schemas in " + ToUtf8(dir) + "; game structs and enums will be edited as raw values.");
		}

		// The game's classes (data/classes/*.txt): which are the game's, and how to restore one.
		{
			std::error_code ec;
			for (const auto& e : fs::directory_iterator(FromUtf8(host_.executableDirectory()) / "data" / "classes", ec))
			{
				if (!e.is_regular_file(ec) || ToLower(ToUtf8(e.path().extension())) != ".txt") continue;
				try
				{
					classLists_.push_back(ClassList::Load(ToUtf8(e.path())));
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Warning, std::string("Class list: ") + ex.what());
				}
			}
		}

		// Hermes messages (data/messages/*.txt), for message handlers.
		{
			std::error_code ec;
			for (const auto& e : fs::directory_iterator(FromUtf8(host_.executableDirectory()) / "data" / "messages", ec))
			{
				if (!e.is_regular_file(ec) || ToLower(ToUtf8(e.path().extension())) != ".txt") continue;
				try
				{
					auto list = LoadMessages(ToUtf8(e.path()));
					messages_.insert(messages_.end(), list.begin(), list.end());
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Warning, std::string("Messages: ") + ex.what());
				}
			}
			std::sort(messages_.begin(), messages_.end(), [](const MessageDef& a, const MessageDef& b)
			{
				return a.name < b.name;
			});
		}

		// NFS-VltEd's words for each game's values (data/nfsms/*.txt), for nfsms scripts.
		{
			std::error_code ec;
			for (const auto& e : fs::directory_iterator(FromUtf8(host_.executableDirectory()) / "data" / "nfsms", ec))
			{
				if (!e.is_regular_file(ec) || ToLower(ToUtf8(e.path().extension())) != ".txt") continue;
				try
				{
					nfsmsNames_.push_back(nfsms::PropertyNames::Load(ToUtf8(e.path())));
				}
				catch (const std::exception& ex)
				{
					log(LogLevel::Warning, ex.what());
				}
			}
		}
		// The games' Lua APIs (data/lua/<game>_<platform>.txt); frame() picks the
		// open game's (chooseLuaFunctions).
		{
			std::error_code ec;
			for (const auto& e : fs::directory_iterator(FromUtf8(host_.executableDirectory()) / "data" / "lua", ec))
			{
				if (!e.is_regular_file(ec) || ToLower(ToUtf8(e.path().extension())) != ".txt") continue;
				std::vector<LuaEntry>& list = luaLists_[ToLower(ToUtf8(e.path().stem()))];
				std::ifstream in(e.path(), std::ios::binary);
				for (std::string line; std::getline(in, line);)
					if (auto le = ParseLuaLine(std::move(line))) list.push_back(std::move(*le));
			}
			if (userLuaDir_.empty()) userLuaDir_ = ToUtf8(userDir);
			luaChosenFor_ = "?";
		}

		// Names the user has typed before.
		userKeysPath_ = ToUtf8(userDir / "user_keys.txt");
		if (std::error_code ec; fs::exists(FromUtf8(userKeysPath_), ec))
		{
			try
			{
				for (std::string& n : ReadNameLines(userKeysPath_)) userNames_.insert(std::move(n));
			}
			catch (const std::exception& e)
			{
				log(LogLevel::Warning, std::string("User key names: ") + e.what());
			}
		}
		// An older version kept the list File > Load Key Names chose in the .ini,
		// in place of attrib_keys.txt. Its names join user_keys.txt.
		if (!settings_.oldKeysFile.empty())
		{
			std::error_code ec;
			const fs::path old = FromUtf8(settings_.oldKeysFile);
			const bool program = !keysFile_.empty() && fs::equivalent(old, FromUtf8(keysFile_), ec);
			if (!program && fs::is_regular_file(old, ec))
			{
				try
				{
					const size_t n = keepUserNames(ReadNameLines(settings_.oldKeysFile));
					log(LogLevel::Info, "Added " + std::to_string(n) + " names to user_keys.txt from " + settings_.oldKeysFile +
						", the key names file an earlier version read in place of attrib_keys.txt. File > Load Key Names now adds to user_keys.txt, and attrib_keys.txt is always read.");
				}
				catch (const std::exception& e)
				{
					log(LogLevel::Warning, std::string("Key names: ") + e.what());
				}
			}
			settings_.oldKeysFile.clear();
			ImGui::MarkIniSettingsDirty();
		}
		updateTitle();
	}

	// -------------------------------------------------------------- commands ---

	void App::log(LogLevel level, std::string text)
	{
		log_.push_back({ level, std::move(text) });
		while (log_.size() > 2000)
		{
			logDropped_ += log_.front().text.size() + 1;
			log_.pop_front();
		}
		++logVersion_;
	}

	void App::markDirty(Collection* c)
	{
		if (c && recordBase_ && !recordSuppress_) recordDirty_.insert((uint64_t(c->classKey) << 32) | c->key);
		if (c && db_ && c->classKey == StringHash32("car_paint_swatch")) paintSwatches_ = ReadPaintSwatches(*db_);
		if (c && c->vault && c->vault->pack) noteEdit(c->vault->pack, c);
		if (c && c->vault && c->vault->pack && !c->vault->pack->dirty)
		{
			c->vault->pack->dirty = true;
			updateTitle();
		}
	}

	bool App::hasUnsavedChanges() const
	{
		if (!db_) return false;
		for (const auto& p : db_->packs())
			if (p->dirty) return true;
		return !removedFiles().empty();
	}

	void App::rememberName(const std::string& name)
	{
		if (name.empty()) return;
		if (db_)
		{
			db_->names().add(name);
			db_->binNames().add(name);
		}
		if (userKeysPath_.empty() || !userNames_.insert(name).second) return;
		appendUserKeys(name + "\n");
	}

	void App::appendUserKeys(const std::string& lines)
	{
		if (userKeysPath_.empty() || lines.empty()) return;
		const fs::path file = FromUtf8(userKeysPath_);
		bool newline = false; // a file edited by hand may end without one
		if (std::ifstream in(file, std::ios::binary | std::ios::ate); in && in.tellg() > 0)
		{
			in.seekg(-1, std::ios::end);
			newline = in.get() != '\n';
		}
		std::ofstream out(file, std::ios::app | std::ios::binary);
		if (newline) out << "\n";
		out << lines;
		if (!out) log(LogLevel::Warning, "Could not save names to " + userKeysPath_);
	}

	size_t App::keepUserNames(const std::vector<std::string>& names)
	{
		std::unordered_set<std::string> program;
		if (!keysFile_.empty())
		{
			try
			{
				for (std::string& n : ReadNameLines(keysFile_)) program.insert(std::move(n));
			}
			catch (const std::exception&)
			{
			}
		}
		std::string lines;
		size_t added = 0;
		for (const std::string& n : names)
		{
			if (db_)
			{
				db_->names().add(n);
				db_->binNames().add(n);
			}
			if (program.count(n) || !userNames_.insert(n).second) continue;
			lines += n;
			lines += '\n';
			++added;
		}
		appendUserKeys(lines);
		return added;
	}

	void App::ensureDatabase()
	{
		if (db_) return;
		db_ = std::make_unique<Database>();
		treesToReset_ = ~0u;
		for (const auto& sc : schemas_) db_->addTypeSchema(sc);
		for (const auto& l : classLists_) db_->addClassList(l);
		for (const std::string& n : userNames_)
		{
			db_->names().add(n);
			db_->binNames().add(n);
		}
		for (const MessageDef& m : messages_) db_->names().add(m.name);
		if (!keysFile_.empty())
		{
			try
			{
				const size_t n = db_->names().loadFile(keysFile_);
				// Some bStringHash values are hashes of the same names.
				db_->binNames().loadFile(keysFile_);
				log(LogLevel::Info, "Loaded " + std::to_string(n) + " key names from " + keysFile_);
			}
			catch (const std::exception& e)
			{
				log(LogLevel::Warning, std::string("Key names: ") + e.what());
			}
		}
		// bStringHash name lists (data/bin_keys/*.txt), for type_bStringHash values.
		std::error_code ec;
		size_t binCount = 0;
		for (const auto& e : fs::directory_iterator(FromUtf8(host_.executableDirectory()) / "data" / "bin_keys", ec))
		{
			if (!e.is_regular_file(ec) || ToLower(ToUtf8(e.path().extension())) != ".txt") continue;
			try
			{
				binCount += db_->binNames().loadFile(ToUtf8(e.path()));
			}
			catch (const std::exception& ex)
			{
				log(LogLevel::Warning, std::string("bStringHash names: ") + ex.what());
			}
		}
		if (binCount) log(LogLevel::Info, "Loaded " + std::to_string(binCount) + " bStringHash names from data/bin_keys");
	}

	bool App::loadKeyNames(const std::string& path)
	{
		std::vector<std::string> names;
		try
		{
			names = ReadNameLines(path);
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, std::string("Key names: ") + e.what());
			return false;
		}
		const size_t added = keepUserNames(names);
		log(LogLevel::Info, "Read " + std::to_string(names.size()) + " names from " + path +
			(userKeysPath_.empty() ? ". The " + std::to_string(added) + " new ones are kept for this session only."
								   : " and added the " + std::to_string(added) + " new ones to " + userKeysPath_ + "."));
		structureChanged();
		return true;
	}

	bool App::open(const std::string& path)
	{
		if (std::optional<std::string> why = conflictWith(path))
		{
			pendingPath_ = path;
			conflictReason_ = *why;
			request(Pending::ConfirmReplace);
			log(LogLevel::Info, *why + " Asking before closing it.");
			return false;
		}
		if (pending_ == Pending::ChooseTarget)
		{
			// More dropped files: they join the database the prompt opens.
			queuedOpens_.push_back(path);
			return false;
		}
		std::error_code ec;
		if (!fs::exists(FromUtf8(path), ec)) return load(path); // which says it isn't there
		const Detection d = DetectTarget(path);
		if (db_ && !db_->packs().empty())
		{
			// Joining the open database: it keeps its game and platform.
			if (d.certain && d.target != db_->target())
				log(LogLevel::Warning, path + " looks like " + d.target.name() + "'s files (" + d.reason + "), and this database is " +
					db_->target().name());
			return load(path);
		}
		if (detectTarget_ && d.certain) return openAs(path, d.target);
		// Ask, with the best guess chosen. When nothing settled the platform but
		// the byte order, the one chosen last for the same game, if it fits.
		targetDetection_ = d;
		targetChoice_ = d.target;
		if (!d.certain && d.platformWeak && lastTarget_.game == d.target.game && Contradiction(lastTarget_, d).empty())
			targetChoice_ = lastTarget_;
		pendingPath_ = path;
		request(Pending::ChooseTarget);
		return false;
	}

	void App::requestOpenFolderDialog()
	{
		if (auto p = host_.openFolderDialog("Open a folder with attributes.bin and other files"))
			open(*p);
	}

	void App::requestOpenFileDialog()
	{
		if (auto p = host_.openFileDialog("Open attribute pack", { {"Attribute packs", "*.bin;*.lzc"}, {"All files", "*.*"} }))
			open(*p);
	}

	void App::setDetectTarget(bool on)
	{
		detectTarget_ = on;
		ImGui::MarkIniSettingsDirty();
	}

	bool App::openAs(const std::string& path, const Target& target)
	{
		if (db_ && !db_->packs().empty()) return load(path);
		ensureDatabase();
		db_->setTarget(target);
		lastTarget_ = target;
		log(LogLevel::Info, "Opening " + path + " as " + target.name() + " (" + target.id() + ")");
		if (!SupportedOn(target.game, target.platform))
			log(LogLevel::Warning, "Attribulina doesn't edit " + target.name() +
				" yet: its structs and enums show as raw bytes, and saving may lose data.");
		return load(path);
	}

	bool App::load(const std::string& path)
	{
		std::error_code ec;
		if (!fs::exists(FromUtf8(path), ec))
		{
			log(LogLevel::Error, "Could not open " + path + ": it isn't there");
			return false;
		}
		const fs::path p = FromUtf8(path);
		bool ok;
		const bool folder = fs::is_directory(p, ec);
		const size_t packsBefore = db_ ? db_->packs().size() : 0;
		if (folder) ok = openFolder(path);
		else ok = openFile(path);
		// Loading isn't undone: the history starts again from what is open now.
		if (db_ && db_->packs().size() != packsBefore) resetUndo();
		// Favourites are the database folder's, whether the folder or one of its files was opened.
		if (const std::string f = databaseFolder(), n = f.empty() ? f : NormalPath(f); n != favoritesFor_) useFavoritesOf(n);
		// A folder counts as opened when anything in it did: one unreadable file
		// shouldn't keep it off the recent list.
		if (ok || (folder && db_ && !db_->packs().empty())) addRecent(path);
		updateTitle();
		return ok;
	}

	bool App::openFile(const std::string& path)
	{
		ensureDatabase();
		for (const auto& pk : db_->packs())
		{
			std::error_code ec;
			if (pk->path == path || fs::equivalent(FromUtf8(pk->path), FromUtf8(path), ec))
			{
				log(LogLevel::Info, path + " is already open");
				return true;
			}
		}
		try
		{
			if (!db_->hasSchema())
			{
				// Collections need the classes from attributes.bin: load it first.
				std::error_code ec;
				if (auto attrs = DatabaseFileFor(path); attrs && !fs::equivalent(FromUtf8(*attrs), FromUtf8(path), ec))
				{
					LoadedPack& base = db_->loadPack(*attrs);
					log(LogLevel::Info, "Loaded " + *attrs + " (" + std::to_string(base.vaults.size()) + " vaults) first: it defines the classes");
				}
			}
			const size_t before = db_->collectionCount();
			const bool hadSchema = db_->hasSchema();
			LoadedPack& pk = db_->loadPack(path);
			if (!hadSchema && db_->hasSchema())
			{
				if (const TypeSchema* ts = db_->typeSchema())
					log(LogLevel::Info, "Using type schema \"" + ts->title + "\" (" + std::to_string(ts->structCount()) + " structs, " +
						std::to_string(ts->enumCount()) + " enums)");
				else
					log(LogLevel::Warning, "No type schema for " + db_->target().name() + "; its structs and enums are edited as raw values.");
			}
			std::string what = std::to_string(pk.vaults.size()) + " vault(s), " + std::to_string(db_->collectionCount() - before) + " collections";
			if (pk.compression) what += std::string(", ") + LzKindName(*pk.compression) + " compressed";
			log(LogLevel::Info, "Opened " + path + " (" + what + ")");
			for (const auto& v : pk.vaults)
			{
				for (const auto& w : v->warnings) log(LogLevel::Warning, v->name + ": " + w);
				if (!v->exactlyWritable)
					log(LogLevel::Warning, v->name + ": saving may not reproduce everything read from this vault");
			}
			checkPackTwin(pk);
			structureChanged();
			recordBase_.reset(); // loading isn't a change to record
			return true;
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, "Could not open " + path + ": " + e.what());
			return false;
		}
	}

	// A .lzc and its .bin twin must be the same pack: the game lists the vaults
	// in the .lzc and streams them from the .bin (core/Writer.h, PackTwin).
	void App::checkPackTwin(LoadedPack& pk)
	{
		const std::optional<std::string> twin = PackTwin(pk.path);
		if (!twin) return;
		const std::string file = ToUtf8(FromUtf8(pk.path).filename()), other = ToUtf8(FromUtf8(*twin).filename());
		std::error_code ec;
		if (!fs::exists(FromUtf8(*twin), ec))
		{
			log(LogLevel::Info, "The game also reads " + other + " beside " + file + " (vaults stream from it). Saving writes it.");
			return;
		}
		try
		{
			auto unpacked = [](const std::string& p)
			{
				const Bytes b = ReadFile(p);
				return ParseLzHeader(b) ? Decompress(b) : b;
			};
			if (unpacked(pk.path) == unpacked(*twin)) return;
			log(LogLevel::Warning, other + " isn't the same pack as " + file + ". The game lists the vaults in the .lzc and streams them "
				"from the .bin, so it crashes loading a vault when they differ. Save All writes both from " + file + ".");
			pk.dirty = true;
			updateTitle();
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Warning, "Could not compare " + file + " with " + other + ": " + e.what());
		}
	}

	bool App::openFolder(const std::string& path)
	{
		std::vector<FolderFile> files;
		try
		{
			files = FolderPacks(path); // core/Folder.h: attributes.bin first, .lzc over .bin
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, std::string(e.what()));
			return false;
		}
		bool ok = true;
		for (const FolderFile& f : files)
		{
			if (f.skipped.empty())
			{
				ok &= openFile(f.path);
			}
			else if (f.skipped != "not an attribute pack")
			{
				log(LogLevel::Info, "Skipped " + ToUtf8(FromUtf8(f.path).filename()) + ": " + f.skipped);
			}
		}
		return ok;
	}

	// The recent files are a file of their own beside the .ini, re-read before
	// each change and written right away. Every window shares the list that way:
	// the .ini is rewritten whole by each window, so a window closing last would
	// put back the list it started with.
	std::string App::recentPath() const
	{
		return iniPath_.empty() ? std::string() : ToUtf8(FromUtf8(iniPath_).parent_path() / "recent_files.txt");
	}

	void App::loadRecent()
	{
		const std::string path = recentPath();
		if (path.empty()) return;
		std::ifstream in(FromUtf8(path), std::ios::binary);
		if (!in) return; // none yet: keep what the .ini had
		std::vector<std::string> r;
		std::string line;
		while (std::getline(in, line) && r.size() < kMaxRecent)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (!line.empty()) r.push_back(line);
		}
		settings_.recent = std::move(r);
	}

	void App::saveRecent()
	{
		const std::string path = recentPath();
		if (path.empty()) return;
		const fs::path p = FromUtf8(path), tmp = FromUtf8(path + ".tmp");
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			for (const std::string& r : settings_.recent) out << r << "\n";
			if (!out) return;
		}
		std::error_code ec;
		fs::rename(tmp, p, ec);
		if (ec) log(LogLevel::Warning, "Could not save the recent files to " + path + ": " + ec.message());
	}

	void App::addRecent(const std::string& path)
	{
		loadRecent(); // what other windows opened since
		const std::string norm = NormalPath(path);
		auto& r = settings_.recent;
		r.erase(std::remove_if(r.begin(), r.end(), [&](const std::string& x)
		{
			return SameFile(x, norm);
		}), r.end());
		r.insert(r.begin(), norm);
		if (r.size() > kMaxRecent) r.resize(kMaxRecent);
		saveRecent();
	}

	void App::clearRecent()
	{
		settings_.recent.clear();
		saveRecent();
	}

	bool App::savePackAs(LoadedPack& pack, const std::string& path)
	{
		try
		{
			for (const auto& v : pack.vaults)
				if (!v->exactlyWritable)
					log(LogLevel::Warning, v->name + ": writing data this vault couldn't be read exactly");
			// A .lzc is saved with its .bin twin (the game reads both).
			const std::optional<std::string> twin = PackTwin(path);
			std::vector<std::string> targets{ path };
			if (twin) targets.push_back(*twin);
			// Keep the first original next to each, once.
			for (const std::string& t : targets)
			{
				std::error_code ec;
				const fs::path target = FromUtf8(t);
				if (!fs::exists(target, ec)) continue;
				fs::path bak = target;
				bak += ".bak";
				if (!fs::exists(bak, ec))
				{
					fs::copy_file(target, bak, ec);
					if (!ec) log(LogLevel::Info, "Backed up the original to " + ToUtf8(bak));
				}
			}
			commitUndo(true); // the step being saved
			// A new event vault file's folder may not be there yet.
			if (std::error_code ec; !fs::exists(FromUtf8(path).parent_path(), ec)) fs::create_directories(FromUtf8(path).parent_path(), ec);
			SavePack(*db_, pack, path);
			// Undoing back to this step finds the file as it is now.
			if (undoPos_ >= 0)
			{
				const UndoState& now = undo_[size_t(undoPos_)];
				for (size_t i = 0; i < now.paths.size(); ++i)
					if (now.paths[i] == pack.path) savedPacks_[pack.path] = now.packs[i];
			}
			savedCompression_[pack.path] = pack.compression;
			if (twin)
				log(LogLevel::Info, "Saved " + path + " and " + ToUtf8(FromUtf8(*twin).filename()) +
					" beside it: the game lists the vaults in the .lzc and streams them from the .bin");
			else
				log(LogLevel::Info, "Saved " + path);
			updateTitle();
			return true;
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, "Could not save " + path + ": " + e.what());
			return false;
		}
	}

	std::vector<std::string> App::removedFiles() const
	{
		std::vector<std::string> out;
		if (!db_) return out;
		for (const auto& [path, bytes] : savedPacks_)
			if (std::none_of(db_->packs().begin(), db_->packs().end(), [&](const auto& p) { return p->path == path; }))
				out.push_back(path);
		return out;
	}

	bool App::removeFile(const std::string& path)
	{
		std::error_code ec;
		const fs::path target = FromUtf8(path);
		if (fs::exists(target, ec))
		{
			// The first original is kept, as saving keeps it.
			fs::path bak = target;
			bak += ".bak";
			if (!fs::exists(bak, ec))
			{
				fs::rename(target, bak, ec);
				if (!ec) log(LogLevel::Info, "Moved " + path + " to " + ToUtf8(bak.filename()) + ": its vault was deleted");
			}
			else
			{
				fs::remove(target, ec);
				if (!ec) log(LogLevel::Info, "Deleted " + path + ": its vault was deleted (the original is in " + ToUtf8(bak.filename()) + ")");
			}
			if (ec)
			{
				log(LogLevel::Error, "Could not remove " + path + ": " + ec.message());
				return false;
			}
		}
		savedPacks_.erase(path);
		savedCompression_.erase(path);
		updateTitle();
		return true;
	}

	bool App::saveAll()
	{
		if (!db_) return true;
		bool ok = true;
		for (const std::string& f : removedFiles()) ok &= removeFile(f);
		for (const auto& p : db_->packs())
		{
			if (!p->dirty)
			{
				if (saveOnlyChanged_) continue;
				// Unchanged, and writing it would change it: the file stays.
				const bool exact = std::all_of(p->vaults.begin(), p->vaults.end(), [](const auto& v)
				{
					return v->exactlyWritable;
				});
				if (!exact)
				{
					log(LogLevel::Info, "Left " + p->path + " as it is: it has no changes, and Attribulina can't write it exactly");
					continue;
				}
			}
			ok &= savePackAs(*p, p->path);
		}
		return ok;
	}

	void App::setPackCompression(LoadedPack& pack, LzKind kind)
	{
		if (!pack.compression || *pack.compression == kind) return;
		const std::string file = ToUtf8(FromUtf8(pack.path).filename());
		nameAction("Compress " + file + " with " + LzKindName(kind));
		pack.compression = kind;
		noteEdit(&pack, nullptr);
		if (!pack.dirty)
		{
			pack.dirty = true;
			updateTitle();
		}
		log(LogLevel::Info, "Saving writes " + file + " " + (kind == LzKind::Raww ? std::string("stored, not compressed (RAWW)") : LzKindName(kind) + std::string(" compressed")));
	}

	std::optional<LzKind> App::savedCompression(const LoadedPack& pack) const
	{
		if (!db_) return std::nullopt;
		const auto it = savedCompression_.find(pack.path);
		return it == savedCompression_.end() ? std::nullopt : it->second;
	}

	void App::stepHistory(int delta)
	{
		if (!db_ || tabs_.empty()) return;
		Tab& t = tabs_[activeTab_];
		int pos = t.historyPos + delta;
		// Skip entries whose collection was deleted.
		while (pos >= 0 && pos < int(t.history.size()))
		{
			if (Collection* c = db_->findCollection(t.history[size_t(pos)].first, t.history[size_t(pos)].second))
			{
				t.historyPos = pos;
				walkingHistory_ = true;
				select(c, true);
				walkingHistory_ = false;
				return;
			}
			pos += delta;
		}
	}

	void App::jumpHistory(int pos)
	{
		if (!db_ || tabs_.empty()) return;
		Tab& t = tabs_[activeTab_];
		if (pos < 0 || pos >= int(t.history.size())) return;
		if (Collection* c = db_->findCollection(t.history[size_t(pos)].first, t.history[size_t(pos)].second))
		{
			t.historyPos = pos;
			walkingHistory_ = true;
			select(c, true);
			walkingHistory_ = false;
		}
	}

	// Right-click on Back or Forward: the way there, nearest first, as a web
	// browser lists it.
	void App::drawHistoryMenu(int dir)
	{
		if (!db_ || tabs_.empty()) return;
		if (!ImGui::BeginPopupContextItem()) return;
		const Tab& t = tabs_[activeTab_];
		const KeyNames& names = db_->names();
		int shown = 0, jump = -1;
		for (int pos = t.historyPos + dir; pos >= 0 && pos < int(t.history.size()); pos += dir)
		{
			if (++shown > 30)
			{
				ImGui::TextDisabled("...");
				break;
			}
			const auto [cls, key] = t.history[size_t(pos)];
			const bool exists = db_->findCollection(cls, key) != nullptr;
			ImGui::PushID(pos);
			const std::string label = names.format(key) + (exists ? "" : " (deleted)");
			if (ImGui::MenuItem(label.c_str(), names.format(cls).c_str(), false, exists)) jump = pos;
			ImGui::PopID();
		}
		ImGui::EndPopup();
		if (jump >= 0) jumpHistory(jump);
	}

	namespace
	{
		// favorites.txt: a "[folder]" line for each database, then its favourites
		// as "class:collection" keys in hex and the name they had. "[]" holds an
		// older version's list, which every database shared, until a database
		// holding them takes them (useFavoritesOf).
		using FavoriteLists = std::vector<std::pair<std::string, std::vector<App::Favorite>>>;

		FavoriteLists ReadFavorites(const std::string& path)
		{
			FavoriteLists r;
			std::ifstream in(FromUtf8(path), std::ios::binary);
			std::string line;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
				{
					r.emplace_back(line.substr(1, line.size() - 2), std::vector<App::Favorite>{});
					continue;
				}
				unsigned cls = 0, key = 0;
				int end = 0;
				if (r.empty() || std::sscanf(line.c_str(), "%x:%x%n", &cls, &key, &end) != 2) continue;
				App::Favorite f{ cls, key, line.substr(size_t(end)) };
				f.name.erase(0, f.name.find_first_not_of(' '));
				r.back().second.push_back(std::move(f));
			}
			return r;
		}

		std::string WriteFavorites(const std::string& path, const FavoriteLists& lists)
		{
			const fs::path p = FromUtf8(path), tmp = FromUtf8(path + ".tmp");
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				for (const auto& [folder, list] : lists)
				{
					if (list.empty()) continue;
					out << "[" << folder << "]\n";
					char keys[24];
					for (const App::Favorite& f : list)
					{
						std::snprintf(keys, sizeof keys, "%08X:%08X", f.cls, f.key);
						out << keys << (f.name.empty() ? "" : " ") << f.name << "\n";
					}
				}
				if (!out) return "could not write " + path + ".tmp";
			}
			std::error_code ec;
			fs::rename(tmp, p, ec);
			return ec ? ec.message() : std::string();
		}

		FavoriteLists::iterator FavoritesOf(FavoriteLists& lists, const std::string& folder)
		{
			return std::find_if(lists.begin(), lists.end(), [&](const auto& l)
			{
				return !l.first.empty() && SameFile(l.first, folder);
			});
		}
	}

	std::string App::favoritesPath() const
	{
		return iniPath_.empty() ? std::string() : ToUtf8(FromUtf8(iniPath_).parent_path() / "favorites.txt");
	}

	void App::useFavoritesOf(const std::string& folder)
	{
		favoritesFor_ = folder;
		favorites_.clear();
		const std::string path = favoritesPath();
		if (folder.empty() || path.empty()) return;
		FavoriteLists lists = ReadFavorites(path);
		if (auto it = FavoritesOf(lists, folder); it != lists.end()) favorites_ = it->second;
		// An older version's favourites go to the first database that holds them.
		auto old = std::find_if(lists.begin(), lists.end(), [](const auto& l)
		{
			return l.first.empty();
		});
		if (old == lists.end()) return;
		std::vector<Favorite> left;
		size_t took = 0;
		for (Favorite& f : old->second)
		{
			const Collection* c = db_->findCollection(f.cls, f.key);
			if (!c)
			{
				left.push_back(std::move(f));
				continue;
			}
			++took;
			if (isFavorite(c)) continue;
			f.name = db_->names().format(f.cls) + "/" + db_->names().format(f.key);
			favorites_.push_back(std::move(f));
		}
		if (!took) return;
		old->second = std::move(left);
		if (auto it = FavoritesOf(lists, folder); it != lists.end()) it->second = favorites_;
		else lists.emplace_back(folder, favorites_);
		if (std::string e = WriteFavorites(path, lists); !e.empty()) log(LogLevel::Warning, "Could not save the favourites to " + path + ": " + e);
		else log(LogLevel::Info, "Moved " + std::to_string(took) + " favourite(s) to this database from the single list older versions kept for every database");
	}

	void App::moveOldFavorites()
	{
		const std::string path = favoritesPath();
		std::error_code ec;
		if (!settings_.oldFavorites.empty() && !path.empty() && !fs::exists(FromUtf8(path), ec))
		{
			FavoriteLists lists(1);
			for (const auto& [cls, key] : settings_.oldFavorites) lists[0].second.push_back({ cls, key, {} });
			if (std::string e = WriteFavorites(path, lists); !e.empty())
				log(LogLevel::Warning, "Could not move the favourites to " + path + ": " + e);
		}
		settings_.oldFavorites.clear();
	}

	void App::changeFavorites(const std::function<void(std::vector<Favorite>&)>& change)
	{
		const std::string path = favoritesFor_.empty() ? std::string() : favoritesPath();
		if (path.empty())
		{
			change(favorites_); // kept for this window only
			return;
		}
		FavoriteLists lists = ReadFavorites(path); // other windows write it too
		auto it = FavoritesOf(lists, favoritesFor_);
		if (it == lists.end())
		{
			lists.emplace_back(favoritesFor_, std::vector<Favorite>{});
			it = std::prev(lists.end());
		}
		change(it->second);
		favorites_ = it->second;
		if (std::string e = WriteFavorites(path, lists); !e.empty()) log(LogLevel::Warning, "Could not save the favourites to " + path + ": " + e);
	}

	bool App::isFavorite(const Collection* c) const
	{
		return c && std::any_of(favorites_.begin(), favorites_.end(), [&](const Favorite& f)
		{
			return f.cls == c->classKey && f.key == c->key;
		});
	}

	void App::toggleFavorite(const Collection* c)
	{
		if (!c || !db_) return;
		const bool add = !isFavorite(c); // as this window shows it
		const std::string name = db_->names().format(c->classKey) + "/" + db_->names().format(c->key);
		changeFavorites([&](std::vector<Favorite>& list)
		{
			std::erase_if(list, [&](const Favorite& f)
			{
				return f.cls == c->classKey && f.key == c->key;
			});
			if (add) list.push_back({ c->classKey, c->key, name });
		});
	}

	void App::removeFavorite(uint32_t cls, uint32_t key)
	{
		changeFavorites([&](std::vector<Favorite>& list)
		{
			std::erase_if(list, [&](const Favorite& f)
			{
				return f.cls == cls && f.key == key;
			});
		});
	}

	void App::closeAll()
	{
		db_.reset();
		useFavoritesOf({});
		treesToReset_ = ~0u;
		treeToggle_ = treeFocus_ = {};
		treeToggleAll_ = Tree::None;
		recordBase_.reset(); // made again from the next database
		recordDirty_.clear();
		selected_ = nullptr;
		tabs_.clear();
		activeTab_ = 0;
		reveal_.clear();
		pendingTarget_ = nullptr;
		pendingClass_ = nullptr;
		pendingVault_ = nullptr;
		pendingPack_ = nullptr;
		pendingTemplate_ = nullptr;
		sbActivity_ = sbState_ = sbHandler_ = nullptr;
		sbBoxes_.clear();
		sbSourceFor_ = nullptr;
		explorerFocus_ = explorerTarget_ = {};
		dropSource_ = dropParent_ = nullptr;
		dropClass_ = nullptr;
		designer_.open = false; // its design was of the database that went
		designer_.editing = 0;
		cut_.clear();
		cutText_.clear();
		searchResult_ = {};
		searchRan_ = false;
		structureChanged();
		resetUndo();
		updateTitle();
	}

	void App::select(Collection* c, bool reveal)
	{
		if (c && tabs_.empty())
		{
			tabs_.push_back(Tab{ nextTabId_++, nullptr, {}, -1 });
			activeTab_ = 0;
		}
		if (!tabs_.empty())
		{
			Tab& t = tabs_[activeTab_];
			if (c && c != t.collection && !walkingHistory_)
			{
				if (t.historyPos + 1 < int(t.history.size())) t.history.resize(size_t(t.historyPos + 1));
				t.history.emplace_back(c->classKey, c->key);
				if (t.history.size() > 200) t.history.erase(t.history.begin());
				t.historyPos = int(t.history.size()) - 1;
			}
			t.collection = c;
		}
		selected_ = c;
		if (reveal && c)
		{
			// Open the tree down to it: the folder tree in the gameplay view,
			// else the class tree. The tree it's for clears the set once drawn.
			reveal_.clear();
			scrollToSelection_ = true;
			if (view_.gameplayView && isGameplay(c))
			{
				if (const auto* n = gameplay_->nodeOf(c))
					for (const auto* p = n->parent; p; p = p->parent) reveal_.insert(p);
				revealIn_ = explorerTabRequest_ = 2;
			}
			else
			{
				reveal_.insert(c->cls);
				for (Collection* p = c->parent; p; p = p->parent) reveal_.insert(p);
				revealIn_ = explorerTabRequest_ = 1;
			}
		}
	}

	void App::openInNewTab(Collection* c, bool activate)
	{
		if (!c) return;
		if (tabs_.empty())
		{
			select(c, activate);
			return;
		}
		Tab t{ nextTabId_++, c, {{c->classKey, c->key}}, 0 };
		const size_t at = activeTab_ + 1;
		tabs_.insert(tabs_.begin() + ptrdiff_t(at), std::move(t));
		if (activate)
		{
			activeTab_ = at;
			focusTabId_ = tabs_[at].id;
			selected_ = c;
		}
	}

	void App::closeTab(size_t index)
	{
		if (index >= tabs_.size()) return;
		tabs_.erase(tabs_.begin() + ptrdiff_t(index));
		if (tabs_.empty())
		{
			activeTab_ = 0;
			selected_ = nullptr;
			return;
		}
		if (index < activeTab_ || activeTab_ >= tabs_.size()) activeTab_ = activeTab_ ? activeTab_ - 1 : 0;
		focusTabId_ = tabs_[activeTab_].id;
		selected_ = tabs_[activeTab_].collection;
	}

	void App::forgetCollection(Collection* c, Collection* fallback, bool parentFallback)
	{
		if (!fallback && parentFallback) fallback = c->parent;
		for (size_t i = tabs_.size(); i-- > 0;)
		{
			if (tabs_[i].collection != c) continue;
			if (fallback) tabs_[i].collection = fallback;
			else closeTab(i);
		}
		if (pendingTemplate_ == c) pendingTemplate_ = nullptr;
		if (sbActivity_ == c) sbActivity_ = sbState_ = sbHandler_ = nullptr;
		if (sbState_ == c) sbState_ = sbHandler_ = nullptr;
		if (sbHandler_ == c) sbHandler_ = nullptr;
		sbBoxes_.clear();
		if (sbSourceFor_ == c) sbSourceFor_ = nullptr;
		if (selected_ == c) selected_ = tabs_.empty() ? nullptr : tabs_[activeTab_].collection;
		if (pendingTarget_ == c) pendingTarget_ = nullptr;
		if (explorerFocus_.collection == c) explorerFocus_ = {};
		if (explorerTarget_.collection == c) explorerTarget_ = {};
		if (dropSource_ == c || dropParent_ == c) dropSource_ = dropParent_ = nullptr;
		cut_.erase((uint64_t(c->classKey) << 32) | c->key);
		reveal_.erase(c);
	}

	bool App::removeCollection(Collection* c)
	{
		if (!db_ || !c || !c->children.empty()) return false;
		nameAction("Delete " + db_->names().format(c->key));
		markDirty(c);
		forgetCollection(c);
		if (!CollectionEditor(*db_).remove(*c)) return false;
		structureChanged();
		return true;
	}

	void App::linkClicks(Collection* c)
	{
		if (c && ImGui::IsItemClicked(ImGuiMouseButton_Middle)) openInNewTab(c, false);
	}

	bool App::requestClose()
	{
		if (!hasUnsavedChanges()) return true;
		request(Pending::ConfirmClose);
		return false;
	}

	void App::requestNew(ClassDef* cls, Collection* parent)
	{
		request(Pending::NewChild, parent);
		pendingClass_ = cls;
		// Default vault: the parent's; for a root, the selected collection's if it
		// is of this class, else the class's own.
		pendingVault_ = parent ? parent->vault
			: (selected_ && selected_->cls == cls) ? selected_->vault
			: cls->vault;
	}

	void App::request(Pending p, Collection* target)
	{
		pending_ = p;
		pendingTarget_ = target;
		nameInput_.clear();
		if (target && (p == Pending::Rename))
		{
			if (const std::string* n = db_->names().find(target->key)) nameInput_ = *n;
		}
	}

	// Checked every frame too: scripts, nfsms and gameplay commands mark packs
	// dirty in core code, which can't call this.
	void App::updateTitle()
	{
		std::string t = "Attribulina";
		if (db_ && !db_->packs().empty())
		{
			t += hasUnsavedChanges() ? " - *" : " - "; // before the path, as editors mark a changed file
			t += ToUtf8(FromUtf8(db_->packs().front()->path).parent_path());
			if (db_->target().known()) t += " (" + db_->target().name() + ")";
		}
		if (t == title_) return;
		title_ = t;
		host_.setWindowTitle(t);
	}

	void App::rebuildCaches()
	{
		if (cacheVersion_ == modelVersion_) return;
		cacheVersion_ = modelVersion_;
		sortedClasses_.clear();
		sortedRoots_.clear();
		sortedChildren_.clear();
		sortedFields_.clear();
		index_.version = ~0ull;
		gameplay_.reset();
		paintSwatches_ = {};
		if (!db_) return;
		index_.rebuild(*db_, modelVersion_);
		gameplay_ = std::make_unique<GameplayModel>(*db_);
		if (!gameplay_->valid()) gameplay_.reset();
		paintSwatches_ = ReadPaintSwatches(*db_);
		const KeyNames& names = db_->names();
		auto byName = [&](auto* a, auto* b)
		{
			return ToLower(names.format(a->key)) < ToLower(names.format(b->key));
		};
		for (const auto& c : db_->classes()) sortedClasses_.push_back(c.get());
		std::sort(sortedClasses_.begin(), sortedClasses_.end(), byName);
		for (ClassDef* cls : sortedClasses_)
		{
			auto& roots = sortedRoots_[cls];
			for (Collection* c : cls->collections)
			{
				if (!c->parent) roots.push_back(c);
				if (!c->children.empty())
				{
					auto& ch = sortedChildren_[c];
					ch = c->children;
					std::sort(ch.begin(), ch.end(), byName);
				}
			}
			std::sort(roots.begin(), roots.end(), byName);
			auto& fields = sortedFields_[cls];
			for (const auto& f : cls->fields) fields.push_back(&f);
			std::sort(fields.begin(), fields.end(), byName);
		}
	}

	// ------------------------------------------------------------------ frame ---

	void App::setWindowPlacement(std::string text)
	{
		settings_.windowPlacement = std::move(text);
		ImGui::MarkIniSettingsDirty();
	}

	// The panels the View menu opens and closes, by the names the .ini keeps.
	std::string App::openPanels() const
	{
		std::string s;
		auto add = [&](bool on, const char* name)
		{
			if (on) s += (s.empty() ? "" : ",") + std::string(name);
		};
		add(showExplorer_, "Explorer");
		add(showInspector_, "Collection");
		add(showClass_, "Class");
		add(showLog_, "Log");
		add(showSearch_, "Search");
		add(showScript_, "Script");
		add(showSequence_, "StateGraphBuilder");
		add(showNfsms_, "NfsmsScript");
		return s;
	}

	void App::setOpenPanels(const std::string& list)
	{
		auto has = [&](const char* name)
		{
			const std::string l = "," + list + ",";
			return l.find("," + std::string(name) + ",") != std::string::npos;
		};
		showExplorer_ = has("Explorer");
		showInspector_ = has("Collection");
		showClass_ = has("Class");
		showLog_ = has("Log");
		showSearch_ = has("Search");
		showScript_ = has("Script");
		showSequence_ = has("StateGraphBuilder");
		showNfsms_ = has("NfsmsScript");
		panelsSeen_ = openPanels();
	}

	void App::frame()
	{
		rebuildCaches();
		if (luaChosenFor_ != (db_ ? db_->target().id() : std::string())) chooseLuaFunctions();
		// Opening or closing a panel is saved, so the next session shows the same ones.
		if (const std::string panels = openPanels(); panels != panelsSeen_)
		{
			panelsSeen_ = panels;
			ImGui::MarkIniSettingsDirty();
		}
		// A cut is undone when something else lands on the clipboard.
		if (!cut_.empty() && ImGui::GetTime() >= cutCheckAt_)
		{
			cutCheckAt_ = ImGui::GetTime() + 1.0;
			std::string now = ImGui::GetClipboardText() ? ImGui::GetClipboardText() : "";
			now.erase(std::remove(now.begin(), now.end(), '\r'), now.end());
			if (cutText_ != now)
			{
				cut_.clear();
				cutText_.clear();
			}
		}
		globalShortcuts();
		drawMenuBar();
		drawStatusBar();
		drawDockSpace();
		if (showExplorer_) drawExplorer();
		if (showInspector_) drawInspector();
		if (showClass_) drawClassPanel();
		if (showLog_) drawLog();
		drawSearch();
		drawScriptPanel();
		drawNfsmsPanel();
		drawStateGraphBuilder();
		drawClassDesigner();
		drawUserLuaWindow();
		if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
		drawModals();
		if (typedName_.box && ImGui::GetActiveID() != typedName_.box) commitTypedName();
		commitUndo();
		flushRecording();
		updateTitle();
	}

	void App::commitTypedName()
	{
		if (typedName_.text != typedName_.original) rememberName(typedName_.text);
		typedName_ = {};
	}

	// ------------------------------------------------------------ Lua lists ---

	std::optional<App::LuaEntry> App::ParseLuaLine(std::string line, std::string* error)
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos || line[start] == '#') return std::nullopt;
		line.erase(0, start);
		auto fail = [&](std::string why) -> std::optional<LuaEntry>
		{
			if (error) *error = std::move(why);
			return std::nullopt;
		};
		LuaEntry le;
		const size_t sp = line.find(' ');
		if (sp == std::string::npos) return fail("a kind and a name are needed, e.g. function Game.Name (a: number)");
		le.kind = line.substr(0, sp);
		const size_t at = line.find_first_not_of(' ', sp);
		size_t ne = line.find_first_of(" (", at);
		if (ne == std::string::npos) ne = line.size();
		le.name = at == std::string::npos ? std::string() : line.substr(at, ne - at);
		size_t hash = line.find("  # ", ne);
		if (hash == std::string::npos && error) hash = line.find('#', ne); // the user's: one space will do
		le.signature = line.substr(ne, (hash == std::string::npos ? line.size() : hash) - ne);
		while (!le.signature.empty() && le.signature.front() == ' ') le.signature.erase(0, 1);
		while (!le.signature.empty() && le.signature.back() == ' ') le.signature.pop_back();
		if (hash != std::string::npos)
		{
			le.description = line.substr(line.find_first_not_of("# ", hash) == std::string::npos ? line.size() : line.find_first_not_of("# ", hash));
		}
		if (!error) return le;
		// The user's lines are checked: names the editor can find in code.
		if (le.kind != "function" && le.kind != "method" && le.kind != "table")
			return fail("'" + le.kind + "' isn't a kind here: function, method or table");
		if (le.name.empty()) return fail("no name after " + le.kind);
		auto ident = [](std::string_view s)
		{
			if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
			for (char c : s)
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
			return true;
		};
		const size_t colon = le.name.find(':');
		if (le.kind == "method")
		{
			if (colon == std::string::npos || !ident(std::string_view(le.name).substr(0, colon)) || !ident(std::string_view(le.name).substr(colon + 1)))
				return fail("a method is named Meta:Name");
		}
		else
		{
			bool ok = colon == std::string::npos;
			for (size_t b = 0; ok;)
			{
				const size_t dot = le.name.find('.', b);
				ok = ident(std::string_view(le.name).substr(b, dot == std::string::npos ? std::string::npos : dot - b));
				if (dot == std::string::npos) break;
				b = dot + 1;
			}
			if (!ok) return fail(le.kind == "table" ? "a table is named like Game or MyHooks" : "a function is named Name or Table.Name (a method: method Meta:Name)");
		}
		if (!le.signature.empty() && le.signature[0] != '(' && le.signature.rfind("->", 0) != 0)
			return fail("after the name comes (arguments), -> results, or # what it does");
		return le;
	}

	std::string App::userLuaPath(Game game) const
	{
		if (game == Game::Unknown) return {};
		return ToUtf8(FromUtf8(userLuaDir_) / ("user_lua_" + std::string(InfoOf(game).id) + ".txt"));
	}

	void App::chooseLuaFunctions()
	{
		const Target t = db_ ? db_->target() : Target{};
		luaChosenFor_ = t.id();
		luaApi_.clear();
		luaList_.clear();
		if (t.known())
		{
			// This platform's list, else its build's (Undercover's PS2 is the
			// Wii's game, on Carbon's engine), else the PC's, else any of the game's.
			const std::string game = InfoOf(t.game).id;
			std::vector<std::string> wants{ t.id() };
			if (t.game == Game::Undercover && t.platform == Platform::PS2) wants.push_back(game + "_wii");
			wants.push_back(game + "_pc");
			for (const std::string& want : wants)
				if (luaList_.empty() && luaLists_.count(want)) luaList_ = want;
			for (const auto& [name, list] : luaLists_)
				if (luaList_.empty() && name.rfind(game + "_", 0) == 0) luaList_ = name;
			// The user's own come first, and replace listed ones of the same name.
			std::set<std::string> own;
			const std::string path = userLuaPath(t.game);
			std::error_code ec;
			if (fs::exists(FromUtf8(path), ec))
			{
				std::ifstream in(FromUtf8(path), std::ios::binary);
				int n = 0;
				for (std::string line; std::getline(in, line);)
				{
					++n;
					std::string why;
					if (auto le = ParseLuaLine(std::move(line), &why))
					{
						le->user = true;
						if (own.insert(le->name).second) luaApi_.push_back(std::move(*le));
					}
					else if (!why.empty())
					{
						log(LogLevel::Warning, ToUtf8(FromUtf8(path).filename()) + ":" + std::to_string(n) + ": " + why);
					}
				}
			}
			if (!luaList_.empty())
				for (const LuaEntry& e : luaLists_[luaList_])
					if (!own.count(e.name)) luaApi_.push_back(e);
		}
		// The Lua editor colours the names and shows each one's line on hover.
		std::unordered_map<std::string, CodeEditor::KnownName> known;
		for (const LuaEntry& le : luaApi_)
		{
			// Constants are tables the scripts make (vars.State...): not coloured.
			if (le.kind == "constant") continue;
			const std::string doc = le.name + (le.signature.empty() ? "" : " " + le.signature) + (le.description.empty() ? "" : "\n" + le.description);
			const size_t colon = le.name.find(':');
			const std::string key = le.kind == "method" && colon != std::string::npos ? le.name.substr(colon) : le.name;
			if (!known.count(key) || le.user) known[key] = { doc, le.kind == "stdlib" };
		}
		luaEditor_.setKnownNames(std::move(known));
	}

	void App::openUserLuaFunctions()
	{
		if (!db_ || !db_->target().known()) return;
		const Game game = db_->target().game;
		showUserLua_ = true;
		ImGui::SetWindowFocus("###userlua");
		if (game == userLuaGame_) return; // as left, edits and all
		if (userLuaGame_ != Game::Unknown && userLuaText_ != userLuaSaved_)
			log(LogLevel::Warning, std::string("Your unsaved Lua functions for ") + InfoOf(userLuaGame_).name + " were dropped for " + InfoOf(game).name + "'s");
		userLuaGame_ = game;
		const std::string path = userLuaPath(game);
		std::error_code ec;
		if (fs::exists(FromUtf8(path), ec))
		{
			try
			{
				const Bytes b = ReadFile(path);
				userLuaText_.assign(b.begin(), b.end());
			}
			catch (const std::exception& e)
			{
				log(LogLevel::Error, "Could not read " + path + ": " + e.what());
				userLuaText_.clear();
			}
		}
		else
		{
			const std::string name = InfoOf(game).name;
			userLuaText_ = "# Your own Lua functions for " + name + ", such as ones your code hooks bind.\n"
				"# The Lua editor colours them and lists them first. One per line:\n"
				"#   function Table.Name (arg: number) -> boolean  # what it does\n"
				"#   method Meta:Name (arg: string)  # what it does\n"
				"#   table Table  # what it holds\n";
		}
		userLuaSaved_ = userLuaText_;
		userLuaChecked_ = "\x01"; // check it on the next draw
		userLuaEditor_.resetHistory();
	}

	bool App::saveUserLuaFunctions()
	{
		if (userLuaGame_ == Game::Unknown) return false;
		const std::string path = userLuaPath(userLuaGame_);
		std::string text = userLuaText_;
		if (!text.empty() && text.back() != '\n') text += '\n';
		try
		{
			const std::string tmp = path + ".tmp";
			{
				std::ofstream out(FromUtf8(tmp), std::ios::binary | std::ios::trunc);
				out << text;
				if (!out) throw std::runtime_error("could not write " + tmp);
			}
			std::error_code ec;
			fs::rename(FromUtf8(tmp), FromUtf8(path), ec);
			if (ec) throw std::runtime_error("could not replace " + path + ": " + ec.message());
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, std::string("Your Lua functions weren't saved: ") + e.what());
			return false;
		}
		userLuaSaved_ = userLuaText_;
		log(LogLevel::Info, "Saved your Lua functions for " + std::string(InfoOf(userLuaGame_).name) + " to " + path);
		luaChosenFor_ = "?"; // listed again
		chooseLuaFunctions();
		return true;
	}

	void App::drawMenuBar()
	{
		if (!ImGui::BeginMainMenuBar()) return;

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::IsWindowAppearing())
			{
				loadRecent(); // other windows add to it too
				recentThere_.clear();
				for (const std::string& r : settings_.recent)
				{
					std::error_code ec;
					recentThere_.push_back(fs::exists(FromUtf8(r), ec));
				}
			}

			if (ImGui::MenuItem("Open Folder...", "Ctrl+O"))
				// Another database than the open one asks first (open()).
				requestOpenFolderDialog();
			if (ImGui::MenuItem("Open File...", "Ctrl+Shift+O"))
				requestOpenFileDialog();

			if (ImGui::BeginMenu("Open Recent", !settings_.recent.empty()))
			{
				const std::vector<std::string> recent = settings_.recent;
				for (size_t i = 0; i < recent.size(); ++i)
				{
					const std::string& r = recent[i];
					const bool there = i >= recentThere_.size() || recentThere_[i];
					if (ImGui::MenuItem(r.c_str(), nullptr, false, there)) open(r);
					if (!there && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Not found");
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Clear List")) clearRecent();
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Detect Game and Platform", nullptr, detectTarget_)) setDetectTarget(!detectTarget_);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("On: a database's files say which game and platform they are from, and Attribulina asks only when they\n"
					"don't (Most Wanted's). Off: it asks each time a database is opened, with what the files say chosen.");
			if (host_.canOpenNewWindow() && ImGui::MenuItem("New Window", "Ctrl+Shift+N")) host_.openNewWindow("");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Another window with its own database, e.g. to copy collections between two folders");
			ImGui::Separator();
			if (ImGui::MenuItem("Save All", "Ctrl+S", false, db_ && (!saveOnlyChanged_ || hasUnsavedChanges()))) saveAll();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(saveOnlyChanged_ ? "Writes the packs with unsaved changes."
					: "Writes every open pack, changed or not, so a file changed or deleted\n"
					"outside Attribulina is written again.");
			if (ImGui::MenuItem("Save Only Changed Packs", nullptr, &saveOnlyChanged_)) ImGui::MarkIniSettingsDirty();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("On: Save All writes only the packs with unsaved changes, and is greyed out while there are none.\n"
					"Off (the default): it writes every open pack.");
			LoadedPack* selPack = selected_ && selected_->vault ? selected_->vault->pack : nullptr;
			if (ImGui::MenuItem("Save Pack As...", nullptr, false, selPack != nullptr))
			{
				const std::string name = selPack ? selPack->path : std::string();
				if (auto p = host_.saveFileDialog("Save attribute pack", name, { {"Attribute packs", "*.bin;*.lzc"} }))
					savePackAs(*selPack, *p);
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Saves the pack the selected collection belongs to under a new name.");
			bool anyCompressed = false;
			if (db_)
				for (const auto& p : db_->packs()) anyCompressed |= p->compression.has_value();
			const bool compressionOpen = ImGui::BeginMenu("Compression", anyCompressed);
			if (!compressionOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(anyCompressed ? "How gameplay.lzc is saved. It starts as the file is."
					: "How a compressed pack (gameplay.lzc) is saved. None is open.");
			if (compressionOpen)
			{
				for (const auto& p : db_->packs())
				{
					if (!p->compression) continue;
					ImGui::PushID(p.get());
					ImGui::SeparatorText(ToUtf8(FromUtf8(p->path).filename()).c_str());
					const std::optional<LzKind> inFile = savedCompression(*p);
					static const struct
					{
						LzKind kind;
						const char* tip;
					} kKinds[] = {
						{LzKind::Jdlz, "LZ77, what every retail gameplay.lzc uses."},
						{LzKind::Huff, "Huffman coding"},
						{LzKind::Raww, "No compression."},
					};
					for (const auto& k : kKinds)
					{
						if (ImGui::MenuItem(LzKindName(k.kind), inFile == k.kind ? "in the file" : nullptr, *p->compression == k.kind))
							setPackCompression(*p, k.kind);
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.tip);
					}
					ImGui::PopID();
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Apply Script...", nullptr, false, db_ != nullptr))
			{
				if (auto p = host_.openFileDialog("Apply script", { {"Mod scripts", "*.amod"}, {"All files", "*.*"} }))
				{
					std::string text;
					if (loadScriptFile(*p, text)) requestApplyScript(text, ToUtf8(FromUtf8(*p).filename()), ToUtf8(FromUtf8(*p).parent_path()));
				}
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Applies a mod script (.amod) to the open database, after asking for its options");
			const std::string noNfsms = nfsmsUnavailable();
			if (ImGui::MenuItem("Run nfsms Script...", nullptr, false, db_ != nullptr && noNfsms.empty()))
			{
				if (auto p = host_.openFileDialog("Run nfsms script", { {"nfsms scripts", "*.nfsms"}, {"All files", "*.*"} }))
				{
					std::string text;
					if (loadScriptFile(*p, text))
					{
						setNfsmsText(std::move(text), *p);
						runNfsms(0, 0);
					}
				}
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", noNfsms.empty() ? "Opens a script from NFS-VltEd or Attribulator in the nfsms window and runs it"
					: (noNfsms + ".").c_str());
			if (ImGui::MenuItem("Export Changes as Script...", nullptr, false, db_ != nullptr)) request(Pending::ExportChanges);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Writes what you changed as a script in the Script panel, to save and share");
			ImGui::Separator();
			if (ImGui::MenuItem("Load Key Names..."))
			{
				if (auto p = host_.openFileDialog("Key names (one per line)", { {"Text files", "*.txt"}, {"All files", "*.*"} }))
					loadKeyNames(*p);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Close All", nullptr, false, db_ != nullptr))
			{
				if (hasUnsavedChanges()) request(Pending::ConfirmCloseAll);
				else closeAll();
			}
			if (ImGui::MenuItem("Exit"))
			{
				if (requestClose()) host_.quit();
			}
			ImGui::EndMenu();
		}
		drawEditMenu();
		drawGoMenu();
		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem(kWindowExplorer, nullptr, &showExplorer_);
			ImGui::MenuItem(kWindowInspector, nullptr, &showInspector_);
			ImGui::MenuItem(kWindowClass, nullptr, &showClass_);
			ImGui::MenuItem(kWindowLog, nullptr, &showLog_);
			ImGui::MenuItem("State Graph Builder", nullptr, &showSequence_);
			if (ImGui::MenuItem("Class Designer", nullptr, designer_.open, db_ && db_->hasSchema()))
			{
				if (designer_.open) designer_.open = false;
				else openClassDesigner(nullptr);
			}
			if (ImGui::MenuItem("Search", "Ctrl+Shift+F", showSearch_))
			{
				if (showSearch_) showSearch_ = false;
				else openSearch();
			}
			if (ImGui::MenuItem("Script", nullptr, &showScript_) && showScript_) bringScript_ = true;
			if (ImGui::MenuItem("Script (nfsms)", nullptr, &showNfsms_) && showNfsms_) bringNfsms_ = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scripts from NFS-VltEd and Attribulator (.nfsms)");
			ImGui::Separator();
			ImGui::MenuItem("Expand Arrays", nullptr, &view_.expandArrays);
			ImGui::MenuItem("Show Inherited Values", nullptr, &view_.showInherited);
			ImGui::MenuItem("Show Unset Fields", nullptr, &view_.showUnset);
			if (ImGui::MenuItem("Gameplay View", nullptr, &view_.gameplayView)) ImGui::MarkIniSettingsDirty();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Shows the gameplay class as folders and templates, with references between elements resolved");
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout")) resetLayout_ = true;
			if (ImGui::BeginMenu("Theme"))
			{
				for (const Theme t : { Theme::Classic, Theme::Dark, Theme::Light })
					if (ImGui::MenuItem(ThemeName(t), nullptr, theme_ == t)) setTheme(t);
				ImGui::EndMenu();
			}
			ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help"))
		{
			if (ImGui::MenuItem("About Attribulina")) request(Pending::About);
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	void App::setTheme(Theme t)
	{
		theme_ = t;
		ApplyTheme(t);
		ImGui::MarkIniSettingsDirty();
	}

	void App::drawStatusBar()
	{
		const float h = ImGui::GetFrameHeight();
		if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, h,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar))
		{
			if (ImGui::BeginMenuBar())
			{
				if (!db_ || !db_->hasSchema())
				{
					ImGui::TextDisabled("No database open. File > Open Folder... and pick the folder with attributes.bin.");
				}
				else
				{
					size_t dirty = 0;
					for (const auto& p : db_->packs()) dirty += p->dirty;
					ImGui::Text("%s  |  %zu packs  |  %zu classes  |  %zu collections  |  %zu names", db_->target().name().c_str(),
						db_->packs().size(), db_->classes().size(), db_->collectionCount(), db_->names().size());
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scripts made from this database are for %s", db_->target().id().c_str());
					if (dirty)
					{
						ImGui::SameLine();
						ImGui::TextColored(kWarn, "|  %zu unsaved", dirty);
					}
				}
				ImGui::EndMenuBar();
			}
		}
		ImGui::End();
	}

	void App::drawDockSpace()
	{
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const ImGuiID id = ImGui::GetID("MainDockSpace");
		if (resetLayout_ || !ImGui::DockBuilderGetNode(id))
		{
			showExplorer_ = true;
			showInspector_ = true;
			showLog_ = true;
			showScript_ = true;
			bringScript_ = false;
			showNfsms_ = true;
			bringNfsms_ = false;

			// Reset Layout closes the other panels. A first run (no dock space
			// yet) leaves open the ones opened before its first frame, as the
			// UI tests do.
			if (resetLayout_)
			{
				showClass_ = false;
				showDemo_ = false;
				showSequence_ = false;
			}
			resetLayout_ = false;

			ImGui::DockBuilderRemoveNode(id);
			ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);
			ImGuiID center = id;
			const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.26f, nullptr, &center);
			//const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
			const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);
			ImGui::DockBuilderDockWindow(kWindowExplorer, left);
			//ImGui::DockBuilderDockWindow(kWindowClass, right);
			ImGui::DockBuilderDockWindow(kWindowScript, bottom);
			ImGui::DockBuilderDockWindow(kWindowNfsms, bottom);
			ImGui::DockBuilderDockWindow(kWindowLog, bottom);
			ImGui::DockBuilderDockWindow(kWindowInspector, center);
			ImGui::DockBuilderFinish(id);
		}
		ImGui::DockSpaceOverViewport(id, vp);
	}

	// --------------------------------------------------------------- explorer ---

	void App::drawExplorer()
	{
		explorerFocus_ = {};
		treeFocus_ = {};
		if (treeToggleFrame_ >= 0 && ImGui::GetFrameCount() > treeToggleFrame_ + 1)
		{ // the node wasn't drawn
			treeToggle_ = {};
			treeToggleAll_ = Tree::None;
			treeToggleFrame_ = -1;
		}
		if (!ImGui::Begin(kWindowExplorer, &showExplorer_))
		{
			ImGui::End();
			return;
		}
		if (!db_ || !db_->hasSchema())
		{
			ImGui::TextWrapped("Open the folder that holds attributes.bin\n(File > Open Folder...), or open attributes.bin itself.");
			ImGui::End();
			return;
		}
		if (!favorites_.empty() && ImGui::CollapsingHeader("Favorites", ImGuiTreeNodeFlags_DefaultOpen))
		{
			std::optional<std::pair<uint32_t, uint32_t>> remove;
			for (const Favorite& f : favorites_)
			{
				Collection* c = db_->findCollection(f.cls, f.key);
				const std::string label = c || f.name.empty() ? db_->names().format(f.cls) + "/" + db_->names().format(f.key) : f.name;
				ImGui::PushID(int(f.cls ^ f.key));
				// One the database no longer has is greyed out, not disabled: a
				// disabled item never counts as hovered, so its menu wouldn't open.
				if (!c) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				if (ImGui::Selectable(label.c_str(), c && c == selected_) && c)
				{
					if (ImGui::GetIO().KeyCtrl) openInNewTab(c, false);
					else select(c, true);
				}
				if (!c)
				{
					ImGui::PopStyleColor();
					ImGui::SetItemTooltip("This database has no such collection. Right-click to remove the favourite.");
				}
				linkClicks(c);
				if (c && ImGui::IsItemFocused()) explorerFocus_ = { c, c->cls };
				if (ImGui::BeginPopupContextItem())
				{
					if (ImGui::MenuItem("Open in New Tab", "Middle-click", false, c != nullptr)) openInNewTab(c, false);
					if (ImGui::MenuItem("Remove from Favorites")) remove = std::make_pair(f.cls, f.key);
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
			if (remove) removeFavorite(remove->first, remove->second);
			ImGui::Separator();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (focusExplorerFilter_)
		{
			ImGui::SetKeyboardFocusHere();
			focusExplorerFilter_ = false;
		}
		ImGui::InputTextWithHint("##filter", "Filter: class/collection (Ctrl+F)", &explorerFilter_);
		if (!explorerFilter_.empty())
		{
			drawFilteredList();
		}
		else if (ImGui::BeginTabBar("##explorer"))
		{
			// A reveal (Back, Forward, a link, Show in Explorer) brings its tree to
			// the front: a gameplay element's in the gameplay view, else Classes.
			const int want = explorerTabRequest_;
			explorerTabRequest_ = 0;
			const bool gameplayTab = view_.gameplayView && gameplay_;
			if (ImGui::BeginTabItem("Classes", nullptr, want == 1 ? ImGuiTabItemFlags_SetSelected : 0))
			{
				drawClassTree();
				ImGui::EndTabItem();
			}
			if (gameplayTab && ImGui::BeginTabItem("Gameplay", nullptr, want == 2 ? ImGuiTabItemFlags_SetSelected : 0))
			{
				drawGameplayTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Files"))
			{
				drawVaultList();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		drawDropMenu();
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		{
			lastPanel_ = Panel::Explorer;
			explorerTarget_ = explorerFocus_;
			explorerShortcuts();
		}
		ImGui::End();
	}

	void App::drawFilteredList()
	{
		const std::string lf = ToLower(explorerFilter_);
		std::vector<const CollectionIndex::Entry*> hits;
		for (const auto& e : index_.entries)
			if (e.lowerLabel.find(lf) != std::string::npos) hits.push_back(&e);
		ImGui::TextDisabled("%zu matches", hits.size());
		ImGui::BeginChild("##hits", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		const uint64_t walk = cacheVersion_; // hits point into index_
		ImGuiListClipper clip;
		clip.Begin(int(hits.size()));
		while (!cachesRebuiltSince(walk) && clip.Step())
			for (int i = clip.DisplayStart; i < clip.DisplayEnd && !cachesRebuiltSince(walk); ++i)
			{
				Collection* c = hits[i]->collection;
				ImGui::PushID(c);
				if (ImGui::Selectable(hits[i]->label.c_str(), c == selected_)) openItem(c);
				if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) select(c);
				linkClicks(c);
				if (ImGui::IsItemFocused()) explorerFocus_ = { c, c->cls };
				collectionContextMenu(c);
				ImGui::PopID();
			}
		ImGui::EndChild();
	}

	void App::drawClassTree()
	{
		ImGui::BeginChild("##classes", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		resetTree(Tree::Classes);
		const KeyNames& names = db_->names();
		const uint64_t walk = cacheVersion_;
		if (bool all; takeTreeToggleAll(Tree::Classes, all))
			for (ClassDef* cls : sortedClasses_) setClassOpen(cls, all);
		for (ClassDef* cls : sortedClasses_)
		{
			const ImGuiID id = ImGui::GetID(KeyId(cls->key));
			if (bool to; takeTreeToggle(id, to)) setClassOpen(cls, to);
			if (reveal_.count(cls)) ImGui::SetNextItemOpen(true);
			const bool open = ImGui::TreeNodeEx(KeyId(cls->key), ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%zu)", names.format(cls->key).c_str(),
				cls->collections.size());
			if (ImGui::IsItemFocused()) explorerFocus_ = { nullptr, cls };
			noteTreeFocus(id, open);
			dropTarget(nullptr, cls);
			classContextMenu(cls, id, open);
			if (open)
			{
				for (Collection* c : sortedRoots_[cls])
				{
					if (cachesRebuiltSince(walk)) break;
					drawCollectionNode(c);
				}
				ImGui::TreePop();
			}
			if (cachesRebuiltSince(walk)) break; // sortedClasses_ is a new list
		}
		if (revealIn_ == 1)
		{
			reveal_.clear();
			revealIn_ = 0;
		}
		treeBackgroundMenu(Tree::Classes);
		ImGui::EndChild();
	}

	void App::drawCollectionNode(Collection* c)
	{
		const uint64_t walk = cacheVersion_;
		const auto it = sortedChildren_.find(c);
		const bool leaf = it == sortedChildren_.end();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
		if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		if (c == selected_) flags |= ImGuiTreeNodeFlags_Selected;
		const ImGuiID id = ImGui::GetID(KeyId(c->key));
		if (bool to; !leaf && takeTreeToggle(id, to)) setCollectionOpen(c, to);
		const bool enter = NavEnterOn(id);
		if (reveal_.count(c) || (enter && !leaf))
		{
			// Enter opens the collection: cancel the toggle it also makes.
			const bool want = reveal_.count(c) || ImGui::TreeNodeGetOpen(id);
			ImGui::SetNextItemOpen(enter && !leaf ? !want : want);
		}
		const bool cut = isCut(c);
		if (cut) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		const bool open = ImGui::TreeNodeEx(KeyId(c->key), flags, "%s", db_->names().format(c->key).c_str());
		if (cut) ImGui::PopStyleColor();
		if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) && !ImGui::IsItemToggledOpen())
			select(c);
		if (enter) openItem(c);
		linkClicks(c);
		if (ImGui::IsItemFocused()) explorerFocus_ = { c, c->cls };
		if (!leaf) noteTreeFocus(id, open);
		dragSource(c);
		dropTarget(c, c->cls);
		if (c == selected_ && scrollToSelection_)
		{
			ImGui::SetScrollHereY(0.4f);
			scrollToSelection_ = false;
		}
		collectionContextMenu(c, leaf ? 0 : id, open);
		if (open && !leaf)
		{
			// A drop or a menu command above may have rebuilt sortedChildren_: `it` is gone then.
			if (!cachesRebuiltSince(walk))
				for (Collection* ch : it->second)
				{
					drawCollectionNode(ch);
					if (cachesRebuiltSince(walk)) break;
				}
			ImGui::TreePop();
		}
	}

	void App::drawVaultList()
	{
		ImGui::BeginChild("##files", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
		resetTree(Tree::Files);
		// A command from a row's menu can add or remove collections (or vaults).
		const uint64_t walk = cacheVersion_;
		// A pack's node and its vaults' nodes, set without drawing them.
		auto setPackOpen = [](const LoadedPack& pk, bool open)
		{
			const ImGuiID id = ImGui::GetID(pk.path.c_str());
			ImGui::TreeNodeSetOpen(id, open);
			ImGui::PushOverrideID(id);
			for (const auto& v : pk.vaults) ImGui::TreeNodeSetOpen(ImGui::GetID(v->name.c_str()), open);
			ImGui::PopID();
		};
		if (bool all; takeTreeToggleAll(Tree::Files, all))
			for (const auto& pk : db_->packs()) setPackOpen(*pk, all);
		for (const auto& pk : db_->packs())
		{
			const std::string label = ToUtf8(FromUtf8(pk->path).filename()) + (pk->dirty ? " *" : "");
			const ImGuiID packId = ImGui::GetID(pk->path.c_str());
			if (bool to; takeTreeToggle(packId, to)) setPackOpen(*pk, to);
			const bool packOpen =
				ImGui::TreeNodeEx(pk->path.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%zu vaults)", label.c_str(), pk->vaults.size());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pk->path.c_str());
			noteTreeFocus(packId, packOpen);
			if (ImGui::BeginPopupContextItem())
			{
				ImGui::TextDisabled("%s", label.c_str());
				ImGui::Separator();
				treeToggleItem(packId, packOpen);
				ImGui::Separator();
				drawPackItems(*pk);
				ImGui::EndPopup();
			}
			if (!packOpen) continue;
			for (const auto& v : pk->vaults)
			{
				const ImGuiID vaultId = ImGui::GetID(v->name.c_str());
				if (bool to; takeTreeToggle(vaultId, to)) ImGui::TreeNodeSetOpen(vaultId, to);
				const bool open = ImGui::TreeNodeEx(v->name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%zu)", v->name.c_str(),
					v->collections.size());
				if (!v->warnings.empty() && ImGui::IsItemHovered())
					ImGui::SetTooltip("%zu warning(s); see the log", v->warnings.size());
				noteTreeFocus(vaultId, open);
				if (ImGui::BeginPopupContextItem())
				{
					ImGui::TextDisabled("vault %s", v->name.c_str());
					ImGui::Separator();
					treeToggleItem(vaultId, open);
					ImGui::Separator();
					drawVaultItems(*v);
					ImGui::EndPopup();
				}
				if (!open) continue;
				ImGuiListClipper clip;
				clip.Begin(int(v->collections.size()));
				while (!cachesRebuiltSince(walk) && clip.Step())
					for (int i = clip.DisplayStart; i < clip.DisplayEnd && !cachesRebuiltSince(walk); ++i)
					{
						Collection* c = v->collections[i];
						ImGui::PushID(c);
						const std::string l = db_->names().format(c->classKey) + "/" + db_->names().format(c->key);
						if (ImGui::Selectable(l.c_str(), c == selected_)) openItem(c);
						if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) select(c);
						linkClicks(c);
						if (ImGui::IsItemFocused()) explorerFocus_ = { c, c->cls };
						collectionContextMenu(c);
						ImGui::PopID();
					}
				ImGui::TreePop();
				if (cachesRebuiltSince(walk)) break;
			}
			ImGui::TreePop();
			if (cachesRebuiltSince(walk)) break;
		}
		treeBackgroundMenu(Tree::Files);
		ImGui::EndChild();
	}

	// Context menus are keyed on the item they belong to (BeginPopupContextItem
	// with no id), so sibling nodes never share one popup.
	void App::classContextMenu(ClassDef* cls, ImGuiID treeNode, bool open)
	{
		if (!ImGui::BeginPopupContextItem()) return;
		ImGui::TextDisabled("class %s", db_->names().format(cls->key).c_str());
		ImGui::Separator();
		treeToggleItem(treeNode, open);
		ImGui::Separator();
		drawEditItems({ nullptr, cls });
		ImGui::EndPopup();
	}

	void App::collectionContextMenu(Collection* c, ImGuiID treeNode, bool open)
	{
		if (!ImGui::BeginPopupContextItem()) return;
		ImGui::TextDisabled("%s/%s", db_->names().format(c->classKey).c_str(), db_->names().format(c->key).c_str());
		ImGui::Separator();
		if (ImGui::MenuItem("Open in New Tab", "Ctrl+Enter")) openInNewTab(c, false);
		if (treeNode) treeToggleItem(treeNode, open);
		ImGui::Separator();
		drawEditItems({ c, c->cls });
		ImGui::Separator();
		if (ImGui::MenuItem(isFavorite(c) ? "Remove from Favorites" : "Add to Favorites")) toggleFavorite(c);
		ImGui::EndPopup();
	}

	void App::treeToggleItem(ImGuiID node, bool open)
	{
		if (ImGui::MenuItem(open ? "Collapse All" : "Expand All", "Shift+A"))
		{
			treeToggle_ = { node, !open };
			treeToggleFrame_ = ImGui::GetFrameCount();
		}
	}

	void App::treeBackgroundMenu(Tree t)
	{
		if (!ImGui::BeginPopupContextWindow("##tree", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) return;
		for (const bool open : {true, false})
			if (ImGui::MenuItem(open ? "Expand All" : "Collapse All"))
			{
				treeToggleAll_ = t;
				treeToggleAllOpen_ = open;
				treeToggleFrame_ = ImGui::GetFrameCount();
			}
		if (t == Tree::Classes)
		{
			ImGui::Separator();
			drawNewClassItems();
		}
		if (t == Tree::Files && canMakeEventVaults())
		{
			ImGui::Separator();
			drawNewEventVaultItem();
		}
		ImGui::EndPopup();
	}

	void App::resetTree(Tree t)
	{
		const unsigned bit = 1u << unsigned(t);
		if (!(treesToReset_ & bit)) return;
		treesToReset_ &= ~bit;
		ImGui::GetStateStorage()->Clear(); // the child window's: its nodes' open state
	}

	void App::noteTreeFocus(ImGuiID node, bool open)
	{
		if (ImGui::IsItemFocused()) treeFocus_ = { node, !open };
	}

	bool App::takeTreeToggle(ImGuiID node, bool& open)
	{
		if (!treeToggle_.id || treeToggle_.id != node) return false;
		open = treeToggle_.open;
		treeToggle_ = {};
		return true;
	}

	bool App::takeTreeToggleAll(Tree t, bool& open)
	{
		if (treeToggleAll_ != t) return false;
		open = treeToggleAllOpen_;
		treeToggleAll_ = Tree::None;
		return true;
	}

	// The ID stack must be where the node is drawn.
	void App::setClassOpen(ClassDef* cls, bool open)
	{
		const ImGuiID id = ImGui::GetID(KeyId(cls->key));
		ImGui::TreeNodeSetOpen(id, open);
		ImGui::PushOverrideID(id);
		for (Collection* c : sortedRoots_[cls]) setCollectionOpen(c, open);
		ImGui::PopID();
	}

	void App::setCollectionOpen(Collection* c, bool open)
	{
		const auto it = sortedChildren_.find(c);
		if (it == sortedChildren_.end()) return; // a leaf keeps no state
		const ImGuiID id = ImGui::GetID(KeyId(c->key));
		ImGui::TreeNodeSetOpen(id, open);
		ImGui::PushOverrideID(id);
		for (Collection* ch : it->second) setCollectionOpen(ch, open);
		ImGui::PopID();
	}

	// -------------------------------------------------------------- inspector ---

	EditEnv App::editEnv(Collection* c)
	{
		EditEnv env{ *db_, c && c->vault ? c->vault->raw.endian : Endian::Little, &index_, {}, {}, nullptr };
		env.classKey = c ? c->classKey : 0;
		env.navigate = [this](Collection* t, bool newTab)
		{
			if (newTab) openInNewTab(t, false);
			else select(t, true);
		};
		env.rememberName = [this](const std::string& n)
		{
			rememberName(n);
		};
		env.typingName = [this](const std::string& before, const std::string& now)
		{
			const ImGuiID box = ImGui::GetItemID(); // not GetActiveID: Escape lets go of the box as it reverts
			if (box != typedName_.box)
			{
				commitTypedName();
				typedName_ = { box, before, {} };
			}
			typedName_.text = now;
		};
		env.prefs = &prefs_;
		env.swatches = &paintSwatches_;
		if (view_.gameplayView && isGameplay(c)) env.gameplay = gameplay_.get();
		if (isGameplay(c) && c->find(StringHash32("stateref"))) env.openScript = [this, c]()
		{
			openStateGraphBuilder(c);
		};
		return env;
	}

	// class \\ root \\ ... \\ collection, following the parent (template) chain;
	// ancestors are links.
	void App::drawTemplateBreadcrumb(Collection* c)
	{
		const KeyNames& names = db_->names();
		std::vector<Collection*> chain;
		for (Collection* p = c; p && chain.size() < 64; p = p->parent) chain.push_back(p);
		std::reverse(chain.begin(), chain.end());
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", names.format(c->classKey).c_str());
		for (Collection* p : chain)
		{
			ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::TextDisabled("\\");
			ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::PushID(p);
			if (p == c)
			{
				ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
				ImGui::TextUnformatted(names.format(p->key).c_str());
				ImGui::PopFont();
			}
			else
			{
				if (ImGui::TextLink(names.format(p->key).c_str())) select(p, true);
				linkClicks(p);
			}
			ImGui::PopID();
		}
		if (c->parentKey && !c->parent)
		{
			ImGui::SameLine();
			ImGui::TextColored(kWarn, "(parent %s isn't loaded)", names.format(c->parentKey).c_str());
		}
	}

	// Back / Forward, the path from the class down to the collection, and the
	// favourite toggle (Attribulator's toolbar and tab title, loosely).
	void App::drawNavigation(Collection* c)
	{
		const Tab* tab = tabs_.empty() ? nullptr : &tabs_[activeTab_];
		const int pos = tab ? tab->historyPos : -1, len = tab ? int(tab->history.size()) : 0;
		ImGui::BeginDisabled(pos <= 0);
		if (ImGui::ArrowButton("##back", ImGuiDir_Left)) stepHistory(-1);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back. Right-click for the list.");
		drawHistoryMenu(-1);
		ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::BeginDisabled(pos + 1 >= len);
		if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) stepHistory(+1);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward. Right-click for the list.");
		drawHistoryMenu(+1);
		ImGui::SameLine();

		if (view_.gameplayView && isGameplay(c)) drawGameplayBreadcrumb(c);
		else drawTemplateBreadcrumb(c);
		const bool fav = isFavorite(c);
		const char* favLabel = fav ? "Remove Favorite" : "Add to Favorites";
		const float w = ImGui::CalcTextSize(favLabel).x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
		if (ImGui::Button(favLabel)) toggleFavorite(c);
	}

	// Vault, child count and the collection commands, above the fields.
	void App::drawCollectionHeader(Collection* c)
	{
		ImGui::TextDisabled("Vault:");
		ImGui::SameLine();
		ImGui::Text("%s", c->vault ? c->vault->name.c_str() : "?");
		if (c->vault && c->vault->pack && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c->vault->pack->path.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("  Children: %zu  ", c->children.size());
		ImGui::SameLine();
		if (ImGui::Button("New Child...")) requestNew(c->cls, c);
		ImGui::SameLine();
		if (ImGui::Button("Rename...")) request(Pending::Rename, c);
		ImGui::SameLine();
		if (ImGui::Button("Delete...")) request(Pending::Delete, c);
		ImGui::SameLine();
		if (ImGui::Button("References")) findReferences(c->classKey, c->key);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("What refers to this collection, and its children (Shift+F12 in the Explorer)");
	}

	void App::drawInspector()
	{
		if (!ImGui::Begin(kWindowInspector, &showInspector_))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Inspector;
		if (db_) drawTabBar();
		Collection* c = selected_;
		if (!db_ || !c)
		{
			ImGui::TextDisabled("Select a collection in the Explorer. Middle-click one to open it in a new tab.");
			ImGui::End();
			return;
		}
		drawNavigation(c);

		if (view_.gameplayView && isGameplay(c)) drawGameplayHeader(c);
		else drawCollectionHeader(c);

		Checkbox("Inherited", &view_.showInherited);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show values this collection inherits from its parents (greyed)");
		ImGui::SameLine();
		Checkbox("Unset", &view_.showUnset);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show fields no collection in the chain sets");
		ImGui::SameLine();
		Checkbox("Expand", &view_.expandArrays);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open arrays and structs");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (focusFieldFilter_)
		{
			ImGui::SetKeyboardFocusHere();
			focusFieldFilter_ = false;
		}
		ImGui::InputTextWithHint("##fieldfilter", "Filter fields (Ctrl+F)", &fieldFilter_);

		const ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Hideable;
		if (ImGui::BeginTable("##fields", 3, tf))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.27f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.18f);
			ImGui::TableHeadersRow();
			const std::string lf = ToLower(fieldFilter_);
			for (const FieldDef* f : sortedFields_[c->cls])
			{
				if (!lf.empty() && ToLower(db_->names().format(f->key)).find(lf) == std::string::npos) continue;
				drawFieldRow(*f);
			}
			ImGui::EndTable();
		}
		ImGui::End();
		applyPendingEdit();
	}

	// Browser-style tabs over the inspector: each holds a collection and its
	// history. Middle-click or the x closes one; Ctrl+W closes the current tab.
	void App::drawTabBar()
	{
		if (tabs_.empty()) return;
		const ImGuiTabBarFlags bf = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll |
			ImGuiTabBarFlags_TabListPopupButton;
		if (!ImGui::BeginTabBar("##tabs", bf)) return;
		const KeyNames& names = db_->names();
		int closeId = 0, closeOthersOf = 0, duplicateId = 0;
		std::optional<size_t> shown;
		for (size_t i = 0; i < tabs_.size(); ++i)
		{
			const Tab& t = tabs_[i];
			// Long gameplay paths: the tab shows the last part, the tooltip all of it.
			std::string label = t.collection ? names.format(t.collection->key) : std::string("(empty)");
			const std::string full = t.collection ? names.format(t.collection->classKey) + "/" + label : label;
			if (const size_t slash = label.rfind('/'); slash != std::string::npos && slash + 1 < label.size())
				label = label.substr(slash + 1);
			label += "###tab" + std::to_string(t.id);
			bool open = true;
			ImGuiTabItemFlags f = ImGuiTabItemFlags_None;
			if (t.id == focusTabId_) f |= ImGuiTabItemFlags_SetSelected;
			if (ImGui::BeginTabItem(label.c_str(), &open, f))
			{
				shown = i;
				ImGui::EndTabItem();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full.c_str());
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Duplicate Tab")) duplicateId = t.id;
				if (ImGui::MenuItem("Close Tab", "Ctrl+W")) closeId = t.id;
				if (ImGui::MenuItem("Close Other Tabs", nullptr, false, tabs_.size() > 1)) closeOthersOf = t.id;
				ImGui::EndPopup();
			}
			if (!open) closeId = t.id;
		}
		if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip) && selected_)
			openInNewTab(selected_, true);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open the current collection in a new tab");
		ImGui::EndTabBar();
		focusTabId_ = 0;

		// ImGui reports which tab is in front; follow it.
		if (shown && *shown != activeTab_)
		{
			activeTab_ = *shown;
			selected_ = tabs_[activeTab_].collection;
		}
		auto indexOf = [&](int id) -> std::optional<size_t>
		{
			for (size_t i = 0; i < tabs_.size(); ++i)
				if (tabs_[i].id == id) return i;
			return std::nullopt;
		};
		if (duplicateId)
			if (auto i = indexOf(duplicateId))
			{
				Tab copy = tabs_[*i];
				copy.id = nextTabId_++;
				tabs_.insert(tabs_.begin() + ptrdiff_t(*i + 1), copy);
				if (*i < activeTab_) ++activeTab_;
			}
		if (closeOthersOf)
			if (auto i = indexOf(closeOthersOf))
			{
				Tab keep = tabs_[*i];
				tabs_.assign(1, keep);
				activeTab_ = 0;
				focusTabId_ = keep.id;
				selected_ = keep.collection;
			}
		if (closeId)
			if (auto i = indexOf(closeId)) closeTab(*i);
	}

	void App::applyPendingEdit()
	{
		PendingEdit e = pendingEdit_;
		pendingEdit_ = {};
		Collection* c = selected_;
		if (e.kind == PendingEdit::Kind::None || !c) return;
		const std::string field = db_->names().format(e.field);
		switch (e.kind)
		{
			case PendingEdit::Kind::Override:
			case PendingEdit::Kind::Set: nameAction("Set " + field); break;
			case PendingEdit::Kind::Revert: nameAction("Revert " + field); break;
			case PendingEdit::Kind::InsertElement: nameAction("Add an element to " + field); break;
			case PendingEdit::Kind::RemoveElement: nameAction("Remove an element of " + field); break;
			case PendingEdit::Kind::None: break;
		}
		switch (e.kind)
		{
			case PendingEdit::Kind::Override:
			case PendingEdit::Kind::Set:
				if (e.def) OverrideAttribute(*db_, *c, *e.def);
				break;
			case PendingEdit::Kind::Revert:
				RevertAttribute(*c, e.field);
				break;
			case PendingEdit::Kind::InsertElement:
				if (Attribute* a = c->find(e.field); a && a->count < MaxElements(*a, arraysPastLimit_))
				{
					InsertElement(*a, e.element);
					if (ElementLimit(*a) && a->count == ElementLimit(*a) + 1) log(LogLevel::Warning, ElementLimitWarning(*a, a->count, field));
				}
				break;
			case PendingEdit::Kind::RemoveElement:
				if (Attribute* a = c->find(e.field)) RemoveElement(*a, e.element);
				break;
			case PendingEdit::Kind::None:
				break;
		}
		markDirty(c);
	}

	// Tooltip and context menu on a row's name cell.
	void App::rowDecorations(const RowExtras* x, const std::string& label)
	{
		if (!x) return;
		const KeyNames& names = db_->names();
		if (x->def && ImGui::IsItemHovered())
		{
			const FieldDef& f = *x->def;
			ImGui::BeginTooltip();
			ImGui::Text("%s  (0x%08X)", label.c_str(), f.key);
			ImGui::Text("type %s, %u bytes%s", f.type ? f.type->name.c_str() : "?", f.size, f.inLayout() ? ", in the layout" : "");
			if (f.isArray()) ImGui::Text(f.maxCount ? "array, its definition allows %u elements" : "array", unsigned(f.maxCount));
			if (x->inheritedFrom) ImGui::Text("inherited from %s", names.format(x->inheritedFrom->key).c_str());
			ImGui::EndTooltip();
		}
		if (!ImGui::BeginPopupContextItem("##rowctx")) return;
		Collection* c = selected_;
		if (x->array)
		{
			if (ImGui::MenuItem("Insert Element Before", nullptr, false, x->array->count < MaxElements(*x->array, arraysPastLimit_)))
				pendingEdit_ = { PendingEdit::Kind::InsertElement, nullptr, x->array->key, x->element };
			if (ImGui::MenuItem("Remove Element"))
				pendingEdit_ = { PendingEdit::Kind::RemoveElement, nullptr, x->array->key, x->element };
			if (ImGui::MenuItem("Add Field to Script")) addToScript({ script::ExportValue(*db_, *c, x->array->key) });
			ImGui::Separator();
			nfsmsMenuItems(*c, x->array->key, int(x->element));
		}
		else if (x->def)
		{
			const bool inherited = x->inheritedFrom != nullptr;
			if (inherited)
			{
				if (ImGui::MenuItem("Override")) pendingEdit_ = { PendingEdit::Kind::Override, x->def, x->def->key, 0 };
			}
			else if (!x->def->inLayout())
			{
				const bool parentHasIt = c->parent && c->parent->resolve(x->def->key);
				if (ImGui::MenuItem(parentHasIt ? "Revert to Inherited" : "Remove Value"))
					pendingEdit_ = { PendingEdit::Kind::Revert, x->def, x->def->key, 0 };
			}
			else
			{
				ImGui::MenuItem("Layout fields are always set", nullptr, false, false);
			}
			if (ImGui::MenuItem("Copy Field Name")) ImGui::SetClipboardText(label.c_str());
			if (ImGui::MenuItem("Add to Script")) addToScript({ script::ExportValue(*db_, *c, x->def->key) });
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds this value to the script in the Script panel");
			nfsmsMenuItems(*c, x->def->key, -1);
			if (ImGui::MenuItem("Find Collections That Set It"))
			{
				searchQuery_ = {};
				searchQuery_.mode = SearchMode::Fields;
				searchQuery_.wholeValue = true;
				searchQuery_.inClass = c->classKey;
				searchText_ = label;
				openSearch();
				runSearch();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every %s collection with its own %s", names.format(c->classKey).c_str(), label.c_str());
		}
		ImGui::EndPopup();
	}

	void App::markHighlightedRow(bool expands)
	{
		if (!highlightRow_) return;
		highlightRow_ = false;
		const float fade = std::clamp(float(highlightUntil_ - ImGui::GetTime()) / 1.5f, 0.0f, 1.0f);
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_TextSelectedBg, fade));
		if (scrollToField_)
		{
			scrollToField_ = false;
			ImGui::SetScrollHereY(0.3f);
			if (expands) ImGui::SetNextItemOpen(true);
		}
	}

	bool App::drawValueRows(EditEnv& env, const std::string& label, DataBlock& b, uint32_t off, const ValueType& vt, bool editable,
		const RowExtras* extras)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		const bool expands = ExpandsIntoMembers(vt);
		markHighlightedRow(expands);
		ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth;
		if (!expands) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		else if (view_.expandArrays) fl |= ImGuiTreeNodeFlags_DefaultOpen;
		if (!editable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		const bool open = ImGui::TreeNodeEx(label.c_str(), fl);
		if (!editable) ImGui::PopStyleColor();
		rowDecorations(extras, label);

		ImGui::TableSetColumnIndex(1);
		bool changed = EditValue(env, b, off, vt, editable);

		ImGui::TableSetColumnIndex(2);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", vt.typeName.c_str());
		if (extras && extras->def && !extras->array)
		{
			if (extras->def->inLayout())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(layout)");
			}
			if (extras->inheritedFrom)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Override")) pendingEdit_ = { PendingEdit::Kind::Override, extras->def, extras->def->key, 0 };
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Inherited from %s. Override gives this collection its own copy.",
						db_->names().format(extras->inheritedFrom->key).c_str());
			}
		}
		if (open && expands)
		{
			changed |= drawMemberRows(env, *vt.st, b, off, editable);
			ImGui::TreePop();
		}
		return changed;
	}

	bool App::drawMemberRows(EditEnv& env, const StructDef& st, DataBlock& b, uint32_t off, bool editable)
	{
		bool changed = false;
		for (const MemberDef& m : st.members)
		{
			// A count that sizes a pointer array is edited on the array's row.
			bool isCount = false;
			for (const MemberDef& o : st.members)
				isCount |= o.prim == MemberPrim::Ptr && o.ptrCount == m.name && o.ptrTarget != "void";
			if (isCount) continue;
			// By index, not address: ids that follow the heap's layout made a
			// rare hash collision come and go between runs in big expanded tables.
			ImGui::PushID(int(&m - st.members.data()));
			const ValueType mt = TypeOfMember(m);
			if (m.prim == MemberPrim::Ptr)
			{
				changed |= drawPointerRows(env, m, st, b, off, editable);
			}
			else if (m.count > 1 && mt.kind == TypeKind::Float && (m.count == 3 || m.count == 4))
			{
				// float[3] / float[4]: a vector (or colour) editor on one row.
				ValueType vt = mt;
				vt.kind = m.count == 4 ? TypeKind::Vector4 : TypeKind::Vector3;
				vt.size = 4 * m.count;
				vt.typeName = m.typeName + "[" + std::to_string(m.count) + "]";
				changed |= drawValueRows(env, m.name, b, off + m.offset, vt, editable);
			}
			else if (m.count > 1)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				const ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | (view_.expandArrays ? ImGuiTreeNodeFlags_DefaultOpen : 0);
				if (!editable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				const bool open = ImGui::TreeNodeEx(m.name.c_str(), fl);
				if (!editable) ImGui::PopStyleColor();
				ImGui::TableSetColumnIndex(1);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("[%u]", m.count);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextDisabled("%s[%u]", mt.typeName.c_str(), m.count);
				if (open)
				{
					for (uint32_t i = 0; i < m.count; ++i)
					{
						ImGui::PushID(int(i));
						changed |= drawValueRows(env, "[" + std::to_string(i) + "]", b, off + m.offset + i * m.elemSize, mt, editable);
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
			}
			else
			{
				changed |= drawValueRows(env, m.name, b, off + m.offset, mt, editable);
			}
			ImGui::PopID();
		}
		return changed;
	}

	// A pointer member: the array (or single value) it leads to, with its count
	// editable on the row when the struct holds one.
	namespace
	{

		// An array's element count: - and + step it at once; a typed number applies
		// on Enter or when the box is left, not at each keystroke (typing 12 over 8
		// went through 1 and dropped seven elements on the way). `tip` says what
		// bounds it. A count already past maxN (a script went past an array's limit)
		// can shrink, but a typed number past maxN doesn't take it down to maxN.
		bool CountInput(uint32_t& n, uint32_t maxN, const std::string& tip)
		{
			ImGuiStorage* st = ImGui::GetStateStorage();
			const ImGuiID typed = ImGui::GetID("##typed");
			const ImGuiStyle& style = ImGui::GetStyle();
			bool changed = false;
			uint32_t v = n;
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
			if (ImGui::InputScalar("##count", ImGuiDataType_U32, &v)) st->SetInt(typed, int(v));
			if (ImGui::IsItemActivated()) st->SetInt(typed, int(n));
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				const uint32_t t = std::min(uint32_t(st->GetInt(typed, int(n))), std::max(maxN, n));
				changed = t != n;
				n = t;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of elements. %s Enter applies a typed number.", tip.c_str());
			ImGui::SameLine(0, style.ItemInnerSpacing.x);
			ImGui::BeginDisabled(n == 0);
			if (ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), 0)))
			{
				--n;
				changed = true;
			}
			ImGui::EndDisabled();
			ImGui::SameLine(0, style.ItemInnerSpacing.x);
			ImGui::BeginDisabled(n >= maxN);
			if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), 0)))
			{
				++n;
				changed = true;
			}
			ImGui::EndDisabled();
			return changed;
		}

	} // namespace

	bool App::drawPointerRows(EditEnv& env, const MemberDef& m, const StructDef& parent, DataBlock& b, uint32_t structOff,
		bool editable)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		if (m.ptrTarget.empty() || m.ptrTarget == "void")
		{
			ImGui::TextDisabled("%s", m.name.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("(set by the game at runtime)");
			ImGui::TableSetColumnIndex(2);
			ImGui::TextDisabled("pointer");
			return false;
		}
		const ValueType et = TypeOfNamed(*db_, m.ptrTarget);
		const MemberDef* cm = m.ptrCount.empty() ? nullptr : parent.member(m.ptrCount);
		const ValueType ct = cm ? TypeOfMember(*cm) : ValueType{};
		PtrSlot* slot = b.ptrAt(structOff + m.offset);
		uint32_t count = cm ? uint32_t(ReadInteger(b, structOff + cm->offset, ct, env.endian)) : (slot && slot->data ? 1u : 0u);
		if (slot && slot->data && et.size) count = std::min<uint32_t>(count, uint32_t(slot->data->bytes.size() / et.size));

		const ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | (view_.expandArrays ? ImGuiTreeNodeFlags_DefaultOpen : 0);
		if (!editable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		const bool open = ImGui::TreeNodeEx(m.name.c_str(), fl);
		if (!editable) ImGui::PopStyleColor();

		bool changed = false;
		ImGui::TableSetColumnIndex(1);
		if (cm && et.size)
		{
			uint32_t n = count;
			const uint32_t maxN = cm->elemSize == 1 ? 0xFF : 0xFFFF;
			if (!editable) ImGui::BeginDisabled();
			if (CountInput(n, maxN, "At most " + std::to_string(maxN) + ", the most its count member holds.") && n != count)
			{
				// What shrinking takes off the end comes back when it grows again.
				char tailKey[64];
				std::snprintf(tailKey, sizeof(tailKey), "p%p:%u", static_cast<const void*>(&b), structOff + m.offset);
				ArrayTail& t = arrayTails_[tailKey];
				if (t.countAfter != count) t.removed.clear();
				if (slot && slot->data)
					for (uint32_t i = count; i-- > n;) t.removed.push_back(CopyElement(*slot->data, i, et.size));
				// Resize the pointed-to block (copying it first if shared) and the count.
				if (!slot)
				{
					PtrSlot s;
					s.offset = structOff + m.offset;
					b.ptrs.push_back(std::move(s));
					slot = &b.ptrs.back();
				}
				if (!slot->data)
				{
					slot->data = std::make_shared<DataBlock>();
					slot->data->align = et.size == 1 ? 1 : 4;
				}
				else if (slot->data.use_count() > 1)
				{
					slot->data = std::make_shared<DataBlock>(*slot->data);
				}
				DataBlock& nb = *slot->data;
				nb.bytes.resize(size_t(n) * et.size, 0);
				auto& ps = nb.ptrs;
				ps.erase(std::remove_if(ps.begin(), ps.end(), [&](const PtrSlot& s)
				{
					return s.offset >= n * et.size;
				}), ps.end());
				for (uint32_t i = count; i < n && !t.removed.empty(); ++i)
				{
					PutElement(nb, i, et.size, t.removed.back());
					t.removed.pop_back();
				}
				t.countAfter = n;
				if (n == 0)
				{
					// An empty array is a null pointer, as in retail data.
					DropSlotAt(b, structOff + m.offset);
					slot = nullptr;
				}
				WriteInteger(b, structOff + cm->offset, ct, n, env.endian);
				count = n;
				changed = true;
			}
			if (!editable) ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("element%s", count == 1 ? "" : "s");
		}
		else
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled(slot && slot->data ? "1 value" : "(null)");
		}
		ImGui::TableSetColumnIndex(2);
		ImGui::TextDisabled("%s*", m.ptrTarget.c_str());

		if (open)
		{
			if (slot && slot->data && et.size)
			{
				DataBlock& nb = *slot->data;
				for (uint32_t i = 0; i < count && (i + 1) * et.size <= nb.bytes.size(); ++i)
				{
					ImGui::PushID(int(i));
					changed |= drawValueRows(env, "[" + std::to_string(i) + "]", nb, i * et.size, et, editable);
					ImGui::PopID();
				}
			}
			ImGui::TreePop();
		}
		return changed;
	}

	bool App::drawArrayRows(EditEnv& env, const std::string& label, Attribute& a, const ValueType& vt, bool editable,
		const RowExtras& extras)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		markHighlightedRow(true);
		const ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | (view_.expandArrays ? ImGuiTreeNodeFlags_DefaultOpen : 0);
		if (!editable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		const bool open = ImGui::TreeNodeEx("##arr", fl, "%s", label.c_str());
		if (!editable) ImGui::PopStyleColor();
		rowDecorations(&extras, label);

		bool changed = false;
		ImGui::TableSetColumnIndex(1);
		const uint32_t maxN = MaxElements(a, arraysPastLimit_);
		const uint32_t limit = ElementLimit(a);
		// Four bytes R, G, B, A (Undercover's frontend damageColor0, flashColor): one
		// picker in place of the count; the elements stay below.
		const bool fourBytes = vt.kind == TypeKind::UInt8 && a.count == 4 && a.elemSize == 1 && a.data.bytes.size() >= 4;
		if (fourBytes && BytesColour(env, vt, a.count))
		{
			if (!editable) ImGui::BeginDisabled();
			changed = EditBytesColour(env, a.data.bytes.data(), vt);
			if (!editable) ImGui::EndDisabled();
		}
		else
		{
			if (editable)
			{
				uint32_t n = a.count;
				const std::string tip =
					a.inLayout ? "At most " + std::to_string(maxN) + ", the space the layout has for it."
					: !limit ? "At most " + std::to_string(maxN) + "."
					: arraysPastLimit_ ? "Its definition allows " + std::to_string(limit) + "; more are kept, with a warning."
					: "At most " + std::to_string(limit) +
					", the count its definition allows. Edit > Allow Arrays Past Their Limit lets it hold more.";
				const uint32_t before = a.count;
				if (CountInput(n, maxN, tip) && selected_)
				{
					// What shrinking takes off the end comes back when it grows again.
					ArrayTail& t = arrayTails_["a" + std::to_string(selected_->classKey) + ":" + std::to_string(selected_->key) + ":" +
						std::to_string(a.key)];
					if (t.countAfter != a.count) t.removed.clear();
					while (a.count > n)
					{
						t.removed.push_back(CopyElement(a.data, a.count - 1, a.elemSize));
						RemoveElement(a, a.count - 1);
					}
					while (a.count < n)
					{
						InsertElement(a, a.count);
						if (!t.removed.empty())
						{
							PutElement(a.data, a.count - 1, a.elemSize, t.removed.back());
							t.removed.pop_back();
						}
					}
					t.countAfter = a.count;
					changed = true;
					// Warn once, as the count goes past the limit; the row keeps showing it.
					if (limit && before <= limit && a.count > limit) log(LogLevel::Warning, ElementLimitWarning(a, a.count, label));
				}
				ImGui::SameLine();
			}
			ImGui::AlignTextToFramePadding();
			if (editable) ImGui::TextDisabled(a.count == 1 ? "element" : "elements");
			else ImGui::TextDisabled("%u element%s", a.count, a.count == 1 ? "" : "s");
			if (fourBytes && ImGui::BeginPopupContextItem("##bytesctx"))
			{
				if (ImGui::MenuItem("Show as Colour", nullptr, false)) prefs_.colour[vt.name] = true;
				if (ImGui::MenuItem("Show as Numbers", nullptr, true)) prefs_.colour[vt.name] = false;
				ImGui::EndPopup();
			}
			if (limit && a.count > limit)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "over the limit of %u", limit);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ElementLimitWarning(a, a.count, label).c_str());
			}
		}

		ImGui::TableSetColumnIndex(2);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s[]%s", vt.typeName.c_str(), a.inLayout ? "  (layout)" : "");
		if (extras.inheritedFrom)
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Override")) pendingEdit_ = { PendingEdit::Kind::Override, extras.def, extras.def->key, 0 };
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Inherited from %s. Override gives this collection its own copy.",
					db_->names().format(extras.inheritedFrom->key).c_str());
		}

		if (open)
		{
			for (uint32_t i = 0; i < a.count; ++i)
			{
				ImGui::PushID(int(i));
				RowExtras ex;
				if (editable)
				{
					ex.array = &a;
					ex.element = i;
				}
				changed |= drawValueRows(env, "[" + std::to_string(i) + "]", a.data, i * a.elemSize, vt, editable, editable ? &ex : nullptr);
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
		return changed;
	}

	void App::drawFieldRow(const FieldDef& f)
	{
		Collection* c = selected_;
		const KeyNames& names = db_->names();
		const Collection* owner = nullptr;
		const Attribute* found = c->resolve(f.key, &owner);
		const bool own = found && owner == c;
		if (!own && found && !view_.showInherited) return;
		if (!found && !view_.showUnset) return;

		const std::string name = names.format(f.key);
		highlightRow_ = f.key == highlightField_ && ImGui::GetTime() < highlightUntil_;
		ImGui::PushID(int(f.key)); // by key: an undo makes new FieldDefs
		RowExtras extras;
		extras.def = &f;
		extras.inheritedFrom = own ? nullptr : owner;

		if (!found)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			markHighlightedRow(false);
			ImGui::TextDisabled("%s", name.c_str());
			rowDecorations(&extras, name);
			ImGui::TableSetColumnIndex(1);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("not set");
			ImGui::TableSetColumnIndex(2);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s%s", f.fieldEnum ? f.fieldEnum->name.c_str() : TypeLabel(f.type).c_str(), f.isArray() ? "[]" : "");
			ImGui::SameLine();
			if (ImGui::SmallButton("Set")) pendingEdit_ = { PendingEdit::Kind::Set, &f, f.key, 0 };
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Give this collection a value for %s", name.c_str());
			ImGui::PopID();
			return;
		}

		// Inherited values are drawn with the same editors, disabled.
		Attribute& a = own ? *c->find(f.key) : const_cast<Attribute&>(*found);
		EditEnv env = editEnv(c);
		const ValueType vt = TypeOfAttribute(a, name);
		const bool changed = a.isArray ? drawArrayRows(env, name, a, vt, own, extras)
			: drawValueRows(env, name, a.data, 0, vt, own, &extras);
		if (changed && own)
		{
			nameAction("Edit " + names.format(c->key) + " " + name);
			markDirty(c);
		}
		highlightRow_ = false;
		ImGui::PopID();
	}

	// ------------------------------------------------------------ class panel ---

	void App::drawClassPanel()
	{
		if (!ImGui::Begin(kWindowClass, &showClass_))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Other;
		const ClassDef* cls = selected_ ? selected_->cls : nullptr;
		if (!db_ || !cls)
		{
			ImGui::TextDisabled("The selected collection's class definition shows here.");
			ImGui::End();
			return;
		}
		const KeyNames& names = db_->names();
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
		ImGui::Text("%s", names.format(cls->key).c_str());
		ImGui::PopFont();
		ImGui::TextDisabled("0x%08X  |  %zu fields  |  layout %u bytes  |  %zu collections  |  vault %s", cls->key, cls->fields.size(),
			cls->layoutSize, cls->collections.size(), cls->vault ? cls->vault->name.c_str() : "?");
		const bool game = IsGameClass(*db_, cls->key);
		if (ImGui::SmallButton(game ? "View in Class Designer" : "Open in Class Designer")) openClassDesigner(cls);
		ImGui::SameLine();
		if (ImGui::SmallButton("Attrib::Gen Header")) openClassDesigner(cls, true);
		ImGui::SameLine();
		ImGui::TextDisabled(game ? "a class of the game" : "a class made for a mod");
		const ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##defs", 6, tf))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_DefaultSort, 0.30f);
			ImGui::TableSetupColumn("Type", 0, 0.30f);
			ImGui::TableSetupColumn("Flags", 0, 0.16f);
			ImGui::TableSetupColumn("Offset", 0, 0.08f);
			ImGui::TableSetupColumn("Size", 0, 0.08f);
			ImGui::TableSetupColumn("Max", 0, 0.08f);
			ImGui::TableHeadersRow();
			std::vector<const FieldDef*> rows = sortedFields_[cls];
			if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0)
			{
				const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
				auto key = [&](const FieldDef* f) -> std::string
				{
					char b[32];
					switch (s.ColumnIndex)
					{
						case 1: return ToLower(f->type ? f->type->name : "");
						case 2: std::snprintf(b, sizeof(b), "%03u", f->flags); return b;
						case 3: std::snprintf(b, sizeof(b), "%06u", f->offset); return b;
						case 4: std::snprintf(b, sizeof(b), "%06u", f->size); return b;
						case 5: std::snprintf(b, sizeof(b), "%06u", f->maxCount); return b;
						default: return ToLower(names.format(f->key));
					}
				};
				std::stable_sort(rows.begin(), rows.end(), [&](const FieldDef* a, const FieldDef* b)
				{
					return s.SortDirection == ImGuiSortDirection_Descending ? key(b) < key(a) : key(a) < key(b);
				});
			}
			for (const FieldDef* f : rows)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(names.format(f->key).c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(f->type ? f->type->name.c_str() : "?");
				ImGui::TableSetColumnIndex(2);
				std::string fl;
				if (f->flags & DefFlag::Array) fl += "array ";
				if (f->flags & DefFlag::InLayout) fl += "layout ";
				if (f->flags & DefFlag::IsBound) fl += "bound ";
				if (f->flags & DefFlag::NotSearchable) fl += "nosearch ";
				if (f->flags & DefFlag::IsStatic) fl += "static ";
				ImGui::TextUnformatted(fl.c_str());
				ImGui::TableSetColumnIndex(3);
				if (f->inLayout()) ImGui::Text("%u", f->offset);
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%u", f->size);
				ImGui::TableSetColumnIndex(5);
				if (f->isArray()) ImGui::Text("%u", f->maxCount);
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}

	// -------------------------------------------------------------------- log ---

	void App::drawLog()
	{
		if (!ImGui::Begin(kWindowLog, &showLog_))
		{
			ImGui::End();
			return;
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) lastPanel_ = Panel::Other;
		if (ImGui::SmallButton("Clear"))
		{
			log_.clear();
			logDropped_ = 0;
			++logVersion_;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy All")) ImGui::SetClipboardText(logText_.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("Select text to copy it (Ctrl+C), or right-click.");
		// One read-only text: lines can be selected and copied out. Each line ends
		// with '\n', so the caret has an empty line at the end to wait on.
		if (logShown_ != logVersion_)
		{
			logText_.clear();
			logLevels_.clear();
			for (const LogLine& l : log_)
			{
				logText_ += l.text;
				logText_ += '\n';
				logLevels_.push_back(l.level == LogLevel::Info ? 0 : l.level == LogLevel::Warning ? 1 : 2);
			}
			logView_.droppedFront(logDropped_);
			logDropped_ = 0;
			logShown_ = logVersion_;
		}
		const ImU32 colours[3] = { ImGui::GetColorU32(ImGuiCol_Text), ImGui::GetColorU32(kWarn), ImGui::GetColorU32(kError) };
		logView_.draw("##log", logText_, logLevels_, colours, ImVec2(-FLT_MIN, -FLT_MIN));
		ImGui::End();
	}

	// ----------------------------------------------------------------- modals ---

	void App::drawModals()
	{
		const char* id = nullptr;
		switch (pending_)
		{
			case Pending::NewChild: id = "New Collection"; break;
			case Pending::Rename: id = "Rename Collection"; break;
			case Pending::Delete: id = "Delete Collection"; break;
			case Pending::ConfirmClose:
			case Pending::ConfirmCloseAll: id = "Unsaved Changes"; break;
			case Pending::ConfirmReplace: id = "Another Database"; break;
			case Pending::MoveToVault: id = "Move to Vault"; break;
			case Pending::About: id = "About Attribulina"; break;
			case Pending::GameplayNew: id = "New Gameplay Element"; break;
			case Pending::GameplayDuplicate: id = "Duplicate Element"; break;
			case Pending::GameplayDelete: id = "Delete Element"; break;
			case Pending::NewState: id = "New State"; break;
			case Pending::NewHandler: id = "New Handler"; break;
			case Pending::ApplyScript: id = "Apply Script"; break;
			case Pending::ExportChanges: id = "Export Changes"; break;
			case Pending::ExportFields: id = exportNfsms_ ? "Add Class to nfsms Script" : "Add Class to Script"; break;
			case Pending::NfsmsSetup: id = "nfsms Setup"; break;
			case Pending::ChooseTarget: id = "Game and Platform"; break;
			case Pending::DeleteClass: break; // drawDeleteClassPrompt
			case Pending::NewVault: id = "New Vault"; break;
			case Pending::RenameVault: id = "Rename Vault"; break;
			case Pending::DeleteVault: id = "Delete Vault"; break;
			case Pending::NewEventVault: id = "New Event Vault File"; break;
			case Pending::None: break;
		}
		if (id && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
		drawDeleteClassPrompt();

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
		const KeyNames* names = db_ ? &db_->names() : nullptr;

		if (begin("New Collection"))
		{
			Collection* parent = pendingTarget_;
			ClassDef* cls = pendingClass_;
			if (parent)
				ImGui::Text("New child of %s/%s", names->format(parent->classKey).c_str(), names->format(parent->key).c_str());
			else
				ImGui::Text("New collection of class %s (no parent)", names->format(cls->key).c_str());
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
			const bool enter = ImGui::InputTextWithHint("##name", "collection name", &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
			const uint32_t key = nameInput_.empty() ? 0 : StringHash32(nameInput_);
			const bool taken = key && db_->findCollection(cls->key, key);
			ImGui::TextDisabled("key 0x%08X", key);
			if (taken) ImGui::TextColored(kError, "That name is already used in this class.");
			// Which vault (and so which file) it is saved in.
			const std::string current = pendingVault_ ? ToUtf8(FromUtf8(pendingVault_->pack->path).filename()) + ": " + pendingVault_->name
				: std::string("(choose)");
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
			if (ImGui::BeginCombo("vault", current.c_str()))
			{
				for (const auto& pk : db_->packs())
					for (const auto& v : pk->vaults)
					{
						const std::string l = ToUtf8(FromUtf8(pk->path).filename()) + ": " + v->name;
						if (ImGui::Selectable(l.c_str(), v.get() == pendingVault_)) pendingVault_ = v.get();
					}
				ImGui::EndCombo();
			}
			const bool ok = !nameInput_.empty() && !taken && pendingVault_;
			ImGui::BeginDisabled(!ok);
			if (ImGui::Button("Create") || (enter && ok))
			{
				try
				{
					nameAction("New " + nameInput_);
					Collection& n = CollectionEditor(*db_).create(*pendingVault_, *cls, nameInput_, parent);
					rememberName(nameInput_);
					SetCollectionNameField(*db_, n); // its own name, not the parent's it would inherit
					markDirty(&n);
					structureChanged();
					select(&n, true);
					log(LogLevel::Info, "Created " + names->format(n.classKey) + "/" + nameInput_ + " in " + pendingVault_->name);
				}
				catch (const std::exception& e)
				{
					log(LogLevel::Error, e.what());
				}
				done();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}

		if (begin("Rename Collection"))
		{
			Collection* c = pendingTarget_;
			ImGui::Text("Rename %s/%s", names->format(c->classKey).c_str(), names->format(c->key).c_str());
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
			const bool enter = ImGui::InputTextWithHint("##name", "new name", &nameInput_, ImGuiInputTextFlags_EnterReturnsTrue);
			const uint32_t key = nameInput_.empty() ? 0 : StringHash32(nameInput_);
			const bool taken = key && key != c->key && db_->findCollection(c->classKey, key);
			ImGui::TextDisabled("key 0x%08X", key);
			if (taken) ImGui::TextColored(kError, "That name is already used in this class.");
			if (!c->children.empty())
				ImGui::TextColored(kWarn, "Its %zu children are re-pointed to the new name. Collections in\n"
					"files that aren't open still refer to the old one.",
					c->children.size());
			const bool ok = !nameInput_.empty() && !taken;
			ImGui::BeginDisabled(!ok);
			if (ImGui::Button("Rename") || (enter && ok))
			{
				try
				{
					nameAction("Rename " + names->format(c->key) + " to " + nameInput_);
					const std::string oldName = names->format(c->key);
					CollectionEditor(*db_).rename(*c, nameInput_);
					SetCollectionNameField(*db_, *c, &oldName); // when it held the old name, as scripts do
					rememberName(nameInput_);
					for (Collection* ch : c->children) markDirty(ch);
					markDirty(c);
					structureChanged();
				}
				catch (const std::exception& e)
				{
					log(LogLevel::Error, e.what());
				}
				done();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}

		if (begin("Delete Collection"))
		{
			Collection* c = pendingTarget_;
			size_t under = 0;
			std::vector<const Collection*> stack(c->children.begin(), c->children.end());
			while (!stack.empty())
			{
				const Collection* x = stack.back();
				stack.pop_back();
				++under;
				stack.insert(stack.end(), x->children.begin(), x->children.end());
			}
			ImGui::Text("Delete %s/%s?", names->format(c->classKey).c_str(), names->format(c->key).c_str());
			if (under) ImGui::TextColored(kWarn, "The %zu collection%s under it go too.", under, under == 1 ? "" : "s");
			ImGui::TextDisabled("References to it elsewhere are not changed.");
			bool del = ImGui::Button(under ? "Delete All" : "Delete");
			ImGui::SameLine();
			const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			// Enter deletes (unless the keyboard is on Cancel), Escape cancels.
			if (ConfirmKey()) del = true;
			if (del)
			{
				removeTree(c);
				done();
			}
			else if (cancel)
			{
				done();
			}
			ImGui::EndPopup();
		}

		if (begin("Unsaved Changes"))
		{
			ImGui::TextUnformatted("Some packs have unsaved changes:");
			for (const auto& p : db_->packs())
				if (p->dirty) ImGui::BulletText("%s", p->path.c_str());
			for (const std::string& f : removedFiles()) ImGui::BulletText("%s (deleted)", f.c_str());
			const Pending what = pending_;
			auto proceed = [&]()
			{
				if (what == Pending::ConfirmClose) host_.quit();
				else if (what == Pending::ConfirmCloseAll) closeAll();
			};
			// Cancel between the two that act, and the keyboard on it: Enter keeps
			// everything as it is.
			const bool save = ImGui::Button("Save All");
			ImGui::SameLine();
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
			cancel |= CancelKey();
			ImGui::SameLine(0, ImGui::GetFontSize() * 2);
			const bool discard = ImGui::Button("Discard");
			if (save)
			{
				const bool ok = saveAll();
				done();
				if (ok) proceed();
			}
			else if (discard)
			{
				done();
				if (what == Pending::ConfirmClose) host_.quit();
				else
				{
					for (const auto& p : db_->packs()) p->dirty = false;
					proceed();
				}
			}
			else if (cancel)
			{
				done();
			}
			ImGui::EndPopup();
		}

		drawGameplayModals();
		drawSequenceModals();
		drawEditModals();
		drawVaultModals();
		drawScriptModals();
		drawNfsmsModals();

		if (begin("About Attribulina"))
		{
			const char* title = "Attribulina";
			float text_width = ImGui::CalcTextSize(title).x;
			float window_width = ImGui::GetWindowSize().x;

			ImGui::SetCursorPosX((window_width - text_width) * 0.5f);
			ImGui::TextUnformatted(title);
			ImGui::Separator();

			ImGui::TextUnformatted("An AttribSys database editor for Black Box's Need for Speed games.");
			ImGui::Spacing();
			ImGui::TextUnformatted("Made by Team Pepega");

			ImGui::Separator();
			ImGui::TextDisabled("Version: %s", Version::GetAppVersionString());
			ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
			if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) done();
			ImGui::EndPopup();
		}
	}

} // namespace app
