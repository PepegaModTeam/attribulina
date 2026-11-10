#include "CodeEditor.h"

#include "core/Nfsms.h"

#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace app
{

	namespace
	{

		// Tokenizer states carried from one line to the next.
		enum : uint8_t
		{
			kNormal = 0,
			kRawString = 1,   // amod """ ... """
			kLongString = 2,  // Lua [[ ... ]] (the level is in the high bits)
			kLongComment = 3, // Lua --[[ ... ]]
		};
		uint8_t State(uint8_t kind, int level = 0)
		{
			return uint8_t(kind | (std::min(level, 31) << 2));
		}
		uint8_t Kind(uint8_t s)
		{
			return s & 3;
		}
		int Level(uint8_t s)
		{
			return s >> 2;
		}

		const char kTriple[] = "\"\"\"";

		bool WordChar(char c)
		{
			return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
		}

		const std::unordered_set<std::string>& AmodKeywords()
		{
			static const std::unordered_set<std::string> k = {
				"attribulina", "game", "title", "author", "version", "description", "url", "option", "edit", "create",
				"collection", "delete", "rename", "move", "if", "else", "revert", "resize", "insert", "remove", "echo",
				"from", "under", "in", "vault", "exact", "with", "children", "to", "root", "and", "or", "not", "was",
				"default", "min", "max", "choices", "help", "warning", "error",
				// format 2
				"append", "missing", "by", "exists", "class", "field", "has", "own", "count", "contains" };
			return k;
		}
		const std::unordered_set<std::string>& AmodValues()
		{
			static const std::unordered_set<std::string> k = { "true", "false", "null", "bytes", "lua", "bool", "number", "integer", "choice", "text" };
			return k;
		}
		const std::unordered_set<std::string>& LuaKeywords()
		{
			static const std::unordered_set<std::string> k = { "and", "break", "do", "else", "elseif", "end", "for", "function", "if", "in",
															  "local", "not", "or", "repeat", "return", "then", "until", "while" };
			return k;
		}
		const std::unordered_set<std::string>& LuaValues()
		{
			static const std::unordered_set<std::string> k = { "nil", "true", "false", "self" };
			return k;
		}
		// Lua long bracket at p ("[[", "[=[", ...): its level, or -1.
		int LongBracket(const char* p, const char* e)
		{
			if (p >= e || *p != '[') return -1;
			const char* q = p + 1;
			int level = 0;
			while (q < e && *q == '=') ++q, ++level;
			return q < e && *q == '[' ? level : -1;
		}
		// The end of a long bracket of `level` from p, or nullptr.
		const char* LongBracketEnd(const char* p, const char* e, int level)
		{
			for (; p < e; ++p)
			{
				if (*p != ']') continue;
				const char* q = p + 1;
				int l = 0;
				while (q < e && *q == '=') ++q, ++l;
				if (l == level && q < e && *q == ']') return q + 1;
			}
			return nullptr;
		}

	} // namespace

	CodeEditor::Palette CodeEditor::PaletteFor(bool light)
	{
		if (light)
			return { IM_COL32(30, 30, 30, 255),  IM_COL32(0, 60, 180, 255),   IM_COL32(160, 40, 20, 255), IM_COL32(20, 120, 90, 255),
					IM_COL32(110, 130, 110, 255), IM_COL32(140, 40, 150, 255), IM_COL32(120, 80, 0, 255),  IM_COL32(90, 90, 90, 255),
					IM_COL32(150, 150, 150, 255), IM_COL32(40, 40, 40, 255),  IM_COL32(220, 40, 40, 255), IM_COL32(200, 130, 0, 255),
					IM_COL32(0, 0, 0, 18) };
		return { IM_COL32(220, 220, 220, 255), IM_COL32(110, 160, 255, 255), IM_COL32(230, 150, 110, 255), IM_COL32(170, 220, 150, 255),
				IM_COL32(110, 140, 110, 255), IM_COL32(220, 140, 230, 255), IM_COL32(240, 200, 100, 255), IM_COL32(170, 170, 170, 255),
				IM_COL32(110, 110, 110, 255), IM_COL32(220, 220, 220, 255), IM_COL32(255, 90, 80, 255),   IM_COL32(255, 185, 60, 255),
				IM_COL32(255, 255, 255, 14) };
	}

	void CodeEditor::setMarks(std::map<int, std::string> errors, std::map<int, std::string> warnings)
	{
		errors_ = std::move(errors);
		warnings_ = std::move(warnings);
	}

	void CodeEditor::gotoLine(int line, bool focus)
	{
		if (!focus && !active_)
		{
			scrollLine_ = std::max(1, line);
			return;
		}
		gotoLine_ = std::max(1, line);
		focusNext_ = !active_;
	}

	int CodeEditor::lineOf(size_t offset) const
	{
		auto it = std::upper_bound(lines_.begin(), lines_.end(), offset, [](size_t o, const LineInfo& l)
		{
			return o < l.begin;
		});
		return int(it - lines_.begin()); // 1-based: the count of line starts at or before offset
	}

	void CodeEditor::selectedLines(int& first, int& last) const
	{
		if (selBegin_ == selEnd_ || lines_.empty())
		{
			first = last = cursorLine_;
			return;
		}
		first = lineOf(std::min(selBegin_, selEnd_));
		last = std::max(first, lineOf(std::max(selBegin_, selEnd_) - 1));
	}

	std::string CodeEditor::currentLineText(const std::string& text) const
	{
		if (lines_.empty()) return {};
		const int a = selBegin_ != selEnd_ ? lineOf(std::min(selBegin_, selEnd_)) : cursorLine_;
		int b = selBegin_ != selEnd_ ? lineOf(std::max(selBegin_, selEnd_) - 1) : cursorLine_;
		b = std::max(a, b);
		if (a < 1 || size_t(a) > lines_.size()) return {};
		const size_t from = lines_[size_t(a - 1)].begin;
		const size_t to = size_t(b) < lines_.size() ? lines_[size_t(b)].begin : text.size();
		std::string s = text.substr(from, to - from);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		return s;
	}

	std::string CodeEditor::selectedText(const std::string& text) const
	{
		if (selBegin_ == selEnd_) return {};
		const size_t a = std::min(selBegin_, selEnd_), b = std::min(std::max(selBegin_, selEnd_), text.size());
		return a < b ? text.substr(a, b - a) : std::string();
	}

	void CodeEditor::rebuild(const std::string& text)
	{
		lines_.clear();
		const char* s = text.data();
		const char* e = s + text.size();
		uint8_t state = kNormal;
		const char* line = s;
		for (;;)
		{
			lines_.push_back({ uint32_t(line - s), state });
			const char* nl = static_cast<const char*>(std::memchr(line, '\n', size_t(e - line)));
			const char* end = nl ? nl : e;
			// Carry multi-line strings and comments over the line end.
			const char* p = line;
			while (p < end)
			{
				if (Kind(state) == kRawString)
				{
					const char* q = std::search(p, end, kTriple, kTriple + 3);
					if (q == end) break;
					p = q + 3;
					state = kNormal;
					continue;
				}
				if (Kind(state) == kLongString || Kind(state) == kLongComment)
				{
					const char* q = LongBracketEnd(p, end, Level(state));
					if (!q) break;
					p = q;
					state = kNormal;
					continue;
				}
				const char c = *p;
				if (lang_ == CodeLanguage::Amod)
				{
					if (c == '#') break;
					if (c == '"')
					{
						if (end - p >= 3 && p[1] == '"' && p[2] == '"')
						{
							state = State(kRawString);
							p += 3;
							continue;
						}
						++p;
						while (p < end && *p != '"') p += *p == '\\' && p + 1 < end ? 2 : 1;
						if (p < end) ++p;
						continue;
					}
				}
				else if (lang_ == CodeLanguage::Lua)
				{
					if (c == '-' && p + 1 < end && p[1] == '-')
					{
						const int lvl = LongBracket(p + 2, end);
						if (lvl >= 0)
						{
							state = State(kLongComment, lvl);
							p += 4 + lvl;
							continue;
						}
						break;
					}
					if (c == '[')
					{
						const int lvl = LongBracket(p, end);
						if (lvl >= 0)
						{
							state = State(kLongString, lvl);
							p += 2 + lvl;
							continue;
						}
					}
					if (c == '"' || c == '\'')
					{
						const char q = c;
						++p;
						while (p < end && *p != q) p += *p == '\\' && p + 1 < end ? 2 : 1;
						if (p < end) ++p;
						continue;
					}
				}
				else if (lang_ == CodeLanguage::Nfsms)
				{
					if (c == '#' || (c == '/' && p + 1 < end && p[1] == '/')) break;
					if (c == '"')
					{
						++p;
						while (p < end && *p != '"') ++p;
						if (p < end) ++p;
						continue;
					}
				}
				++p;
			}
			if (!nl) break;
			line = nl + 1;
		}
		cachedSize_ = text.size();
		widestDirty_ = true;
	}

	void CodeEditor::drawLine(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* b, const char* e, uint8_t state,
							  const Palette& pal) const
	{
		const char* p = b;
		float x = pos.x;
		auto emit = [&](const char* from, const char* to, ImU32 col)
		{
			if (from >= to) return;
			dl->AddText(font, size, ImVec2(x, pos.y), col, from, to);
			x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, from, to).x;
		};
		auto keywordsOf = [&](const std::string& w) -> ImU32
		{
			switch (lang_)
			{
				case CodeLanguage::Amod:
					if (AmodKeywords().count(w)) return pal.keyword;
					if (AmodValues().count(w)) return pal.number;
					break;
				case CodeLanguage::Lua:
					if (LuaKeywords().count(w)) return pal.keyword;
					if (LuaValues().count(w)) return pal.number;
					break;
				case CodeLanguage::Nfsms: break; // the command is coloured below
				case CodeLanguage::Plain: break;
			}
			return 0;
		};
		if (lang_ == CodeLanguage::Nfsms)
		{
			// A line starting with a known command (no space needed after it, as
			// NFS-VltEd reads it) is a command; any other line is a comment.
			const char* q = b;
			while (q < e && (*q == ' ' || *q == '\t')) ++q;
			const size_t n = attrib::nfsms::CommandAt(std::string_view(q, size_t(e - q)));
			if (!n)
			{
				emit(b, e, pal.comment);
				return;
			}
			emit(b, q + n, pal.keyword);
			p = q + n;
			if (std::string_view(q, n) == "ui_text")
			{
				const char* h = std::find(p, e, '#');
				emit(p, h, pal.string);
				emit(h, e, pal.comment);
				return;
			}
		}
		while (p < e)
		{
			if (Kind(state) == kRawString)
			{
				const char* q = std::search(p, e, kTriple, kTriple + 3);
				const char* to = q == e ? e : q + 3;
				emit(p, to, pal.string);
				p = to;
				if (q != e) state = kNormal;
				continue;
			}
			if (Kind(state) == kLongString || Kind(state) == kLongComment)
			{
				const char* q = LongBracketEnd(p, e, Level(state));
				const char* to = q ? q : e;
				emit(p, to, Kind(state) == kLongComment ? pal.comment : pal.string);
				p = to;
				if (q) state = kNormal;
				continue;
			}
			const char c = *p;
			// Comments.
			const bool amodComment = lang_ == CodeLanguage::Amod && c == '#';
			const bool nfsmsComment = lang_ == CodeLanguage::Nfsms && (c == '#' || (c == '/' && p + 1 < e && p[1] == '/'));
			if (amodComment || nfsmsComment)
			{
				emit(p, e, pal.comment);
				return;
			}
			if (lang_ == CodeLanguage::Lua && c == '-' && p + 1 < e && p[1] == '-')
			{
				const int lvl = LongBracket(p + 2, e);
				if (lvl >= 0)
				{
					const char* q = LongBracketEnd(p + 4 + lvl, e, lvl);
					const char* to = q ? q : e;
					emit(p, to, pal.comment);
					p = to;
					if (!q) state = State(kLongComment, lvl);
					continue;
				}
				emit(p, e, pal.comment);
				return;
			}
			// Strings.
			if (lang_ == CodeLanguage::Amod && c == '"' && e - p >= 3 && p[1] == '"' && p[2] == '"')
			{
				const char* q = std::search(p + 3, e, kTriple, kTriple + 3);
				const char* to = q == e ? e : q + 3;
				emit(p, to, pal.string);
				if (q == e) state = State(kRawString);
				p = to;
				continue;
			}
			if (lang_ == CodeLanguage::Lua && c == '[')
			{
				const int lvl = LongBracket(p, e);
				if (lvl >= 0)
				{
					const char* q = LongBracketEnd(p + 2 + lvl, e, lvl);
					const char* to = q ? q : e;
					emit(p, to, pal.string);
					if (!q) state = State(kLongString, lvl);
					p = to;
					continue;
				}
			}
			if (c == '"' || (lang_ == CodeLanguage::Lua && c == '\''))
			{
				const char* q = p + 1;
				while (q < e && *q != c) q += *q == '\\' && q + 1 < e && lang_ != CodeLanguage::Nfsms ? 2 : 1;
				if (q < e) ++q;
				emit(p, q, pal.string);
				p = q;
				continue;
			}
			// $option (amod).
			if (lang_ == CodeLanguage::Amod && c == '$')
			{
				const char* q = p + 1;
				while (q < e && WordChar(*q)) ++q;
				emit(p, q, pal.option);
				p = q;
				continue;
			}
			// Words and numbers. Lua names include dots for the API (Debug.Print).
			if (WordChar(c) || (c == '.' && p + 1 < e && std::isdigit(static_cast<unsigned char>(p[1]))))
			{
				const char* q = p;
				if (lang_ == CodeLanguage::Amod || lang_ == CodeLanguage::Nfsms)
				{
					// amod words run on through . - + : (911gt3, 1.5e-3, race_bin-1).
					while (q < e && (WordChar(*q) || *q == '.' || *q == '-' || *q == '+' || *q == ':' || *q == '[' || *q == ']'))
					{
						if ((*q == '[' || *q == ']') && lang_ == CodeLanguage::Amod) break;
						if ((*q == '-' || *q == '+') && q + 1 < e && q[1] == '=') break;
						++q;
					}
				}
				else
				{
					while (q < e && (WordChar(*q) || *q == '.')) ++q;
				}
				const std::string w(p, q);
				ImU32 col = keywordsOf(w);
				if (!col)
				{
					const bool numeric = std::isdigit(static_cast<unsigned char>(w[0])) || (w[0] == '.' && w.size() > 1);
					bool allNum = numeric;
					if (numeric)
					{
						char* end = nullptr;
						std::strtod(w.c_str(), &end);
						allNum = (end && !*end) || (w.size() > 2 && w[0] == '0' && (w[1] == 'x' || w[1] == 'X'));
					}
					if (allNum) col = pal.number;
					else if (const KnownName* k = lang_ == CodeLanguage::Lua ? knownAt(b, p, q) : nullptr) col = k->missing ? pal.error : pal.known;
					else col = pal.text;
				}
				emit(p, q, col);
				p = q;
				continue;
			}
			if (c == '-' && p + 1 < e && std::isdigit(static_cast<unsigned char>(p[1])) && lang_ != CodeLanguage::Lua)
			{
				const char* q = p + 1;
				while (q < e && (WordChar(*q) || *q == '.')) ++q;
				emit(p, q, pal.number);
				p = q;
				continue;
			}
			// Anything else, one character (a UTF-8 sequence as a whole).
			const char* q = p + 1;
			while (q < e && (static_cast<unsigned char>(*q) & 0xC0) == 0x80) ++q;
			const bool punct = std::strchr("{}[]()=,;/+-*<>~", c) != nullptr;
			emit(p, q, punct ? pal.punct : pal.text);
			p = q;
		}
	}

	// The known name a word is: itself (Debug.Print), a method after ':', or its
	// table (Debug in Debug.Nothing).
	const CodeEditor::KnownName* CodeEditor::knownAt(const char* lineBegin, const char* b, const char* e) const
	{
		if (known_.empty() || b >= e) return nullptr;
		const std::string w(b, e);
		if (b > lineBegin && b[-1] == ':')
		{
			auto it = known_.find(":" + w);
			return it == known_.end() ? nullptr : &it->second;
		}
		if (auto it = known_.find(w); it != known_.end()) return &it->second;
		const size_t dot = w.find('.');
		if (dot != std::string::npos)
			if (auto it = known_.find(w.substr(0, dot)); it != known_.end()) return &it->second;
		return nullptr;
	}

	namespace
	{
		// A mouse button down or just let go: a right-click menu that closed before
		// it was drawn this frame was closed by a click (else by Escape).
		bool MouseClickedOrHeld()
		{
			for (int b = 0; b < ImGuiMouseButton_COUNT; ++b)
				if (ImGui::IsMouseDown(b) || ImGui::IsMouseReleased(b)) return true;
			return false;
		}
	} // namespace

	bool TextFieldMenu(std::string& text)
	{
		const ImGuiID id = ImGui::GetItemID();
		const ImGuiID menu = ImHashStr("##fieldmenu", 0, id);
		const ImRect frame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const bool active = ImGui::IsItemActive();
		ImGuiStorage* store = ImGui::GetStateStorage();
		// Kept: menu + 1, + 2 the selection; + 3 the field had the keyboard; + 4
		// the frame it was asked to take it back; + 5 the menu was open.
		if (const int asked = store->GetInt(menu + 4); asked && (active || ImGui::GetFrameCount() - asked > 3))
		{
			if (ImGuiInputTextState* st = active ? ImGui::GetInputTextState(id) : nullptr)
			{
				const int n = int(text.size());
				st->SetSelection(std::min(store->GetInt(menu + 1), n), std::min(store->GetInt(menu + 2), n));
			}
			store->SetInt(menu + 4, 0);
		}
		if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		{
			// The selection, before the menu takes the keyboard (ImGui keeps the
			// last active field's state).
			int a = int(text.size()), b = a;
			if (ImGuiInputTextState* st = ImGui::GetInputTextState(id))
			{
				a = std::min(st->GetSelectionStart(), st->GetSelectionEnd());
				b = std::max(st->GetSelectionStart(), st->GetSelectionEnd());
				if (a == b) a = b = st->GetCursorPos();
			}
			store->SetInt(menu + 1, a);
			store->SetInt(menu + 2, b);
			store->SetInt(menu + 3, active);
			ImGui::OpenPopupEx(menu);
		}
		bool changed = false;
		const bool openBefore = ImGui::IsPopupOpen(menu, ImGuiPopupFlags_None);
		if (ImGui::BeginPopupEx(menu, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings))
		{
			const size_t b = std::min(size_t(store->GetInt(menu + 2, 0)), text.size());
			const size_t a = std::min(size_t(store->GetInt(menu + 1, 0)), b);
			// (Cut and Copy replace the clipboard's buffer: read it again to paste.)
			const char* clip = ImGui::GetClipboardText();
			const bool canPaste = clip && *clip;
			if (ImGui::MenuItem("Cut", "Ctrl+X", false, b > a))
			{
				ImGui::SetClipboardText(text.substr(a, b - a).c_str());
				text.erase(a, b - a);
				changed = true;
			}
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, b > a)) ImGui::SetClipboardText(text.substr(a, b - a).c_str());
			if (ImGui::MenuItem("Paste", "Ctrl+V", false, canPaste) && ImGui::GetClipboardText())
			{
				std::string in = ImGui::GetClipboardText();
				in.erase(std::remove_if(in.begin(), in.end(), [](char c)
				{
					return c == '\r' || c == '\n';
				}), in.end());
				text.replace(a, b - a, in);
				changed = true;
			}
			if (ImGui::MenuItem("Delete", "Del", false, b > a))
			{
				text.erase(a, b - a);
				changed = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Clear", nullptr, false, !text.empty()))
			{
				text.clear();
				changed = true;
			}
			ImGui::EndPopup();
		}
		const bool open = ImGui::IsPopupOpen(menu, ImGuiPopupFlags_None), was = store->GetInt(menu + 5) != 0;
		store->SetInt(menu + 5, open);
		const bool hadKeyboard = store->GetInt(menu + 3) != 0;
		const int a = store->GetInt(menu + 1), b = std::min(store->GetInt(menu + 2), int(text.size()));
		if (open && hadKeyboard && !active && a < b)
		{
			// The selection, which the field draws only while it has the keyboard
			// (and then unscrolled).
			const ImGuiStyle& style = ImGui::GetStyle();
			const float x = frame.Min.x + style.FramePadding.x + ImGui::CalcTextSize(text.data(), text.data() + a).x;
			const float w = ImGui::CalcTextSize(text.data() + a, text.data() + b).x;
			const float y = frame.Min.y + style.FramePadding.y;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(frame.Min, frame.Max, true);
			dl->AddRectFilled(ImVec2(x, y - 1.0f), ImVec2(x + w, y + ImGui::GetFontSize() + 2.0f), ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
			dl->PopClipRect();
		}
		if (was && !open && hadKeyboard && !changed && !(!openBefore && MouseClickedOrHeld()))
		{
			// Closed by Copy or Escape: the keyboard goes back with the selection.
			ImGui::ActivateItemByID(id);
			ImGui::GetCurrentContext()->NavNextActivateFlags |= ImGuiActivateFlags_TryToPreserveState;
			store->SetInt(menu + 4, ImGui::GetFrameCount());
		}
		return changed;
	}

	// --------------------------------------------------------------- the log ---

	int LogView::Callback(ImGuiInputTextCallbackData* data)
	{
		auto* self = static_cast<LogView*>(data->UserData);
		// Taken by the keyboard: the caret is where it was left (a click puts it where it clicked).
		if (data->EventActivated && self->wantA_ == size_t(-1) && ImGui::GetCurrentContext()->ActiveIdSource != ImGuiInputSource_Mouse)
		{
			self->wantA_ = self->anchor_;
			self->wantB_ = self->caret_;
		}
		if (self->wantA_ != size_t(-1))
		{
			const size_t n = size_t(data->BufTextLen);
			data->SelectionStart = int(std::min(self->wantA_, n));
			data->SelectionEnd = data->CursorPos = int(std::min(self->wantB_, n));
			self->wantA_ = self->wantB_ = size_t(-1);
		}
		self->caret_ = size_t(data->CursorPos);
		self->anchor_ = data->SelectionStart != data->SelectionEnd ? size_t(data->SelectionStart) : self->caret_;
		self->selA_ = size_t(std::min(data->SelectionStart, data->SelectionEnd));
		self->selB_ = size_t(std::max(data->SelectionStart, data->SelectionEnd));
		return 0;
	}

	void LogView::droppedFront(size_t bytes)
	{
		if (!bytes) return;
		auto back = [&](size_t& v)
		{
			v = v > bytes ? v - bytes : 0;
		};
		const bool atEnd = following();
		back(size_);
		if (atEnd)
		{
			anchor_ = caret_ = size_;
		}
		else
		{
			back(anchor_);
			back(caret_);
		}
		back(selA_);
		back(selB_);
		back(resumeA_);
		back(resumeB_);
		shifted_ = true;
	}

	// The rows ImGui wraps the text into (InputTextLineIndexBuild with WordWrap).
	void LogView::layout(const std::string& text, float wrap)
	{
		ImGuiContext& g = *ImGui::GetCurrentContext();
		uint64_t sample = text.size();
		for (size_t i = text.size() > 256 ? text.size() - 256 : 0; i < text.size(); ++i) sample = sample * 131 + uint8_t(text[i]);
		if (text.size() == laidSize_ && sample == laidSample_ && wrap == laidWrap_ && g.FontSize == laidFont_) return;
		laidSize_ = text.size();
		laidSample_ = sample;
		laidWrap_ = wrap;
		laidFont_ = g.FontSize;
		rows_.clear();
		rowLine_.clear();
		const char* b = text.data();
		const char* e = b + text.size();
		uint32_t line = 0;
		for (const char* s = b; s < e;)
		{
			rows_.push_back(uint32_t(s - b));
			rowLine_.push_back(line);
			s = ImFontCalcWordWrapPositionEx(g.Font, g.FontSize, s, e, wrap, ImDrawTextFlags_WrapKeepBlanks);
			if (s < e && *s == '\n')
			{
				++s;
				++line;
			}
		}
	}

	void LogView::draw(const char* label, std::string& text, const std::vector<uint8_t>& levels, const ImU32* colours, const ImVec2& size)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGuiWindow* parent = ImGui::GetCurrentWindow();
		const ImGuiID id = parent->GetID(label);
		const bool wasActive = ImGui::GetActiveID() == id;
		if (shifted_)
		{
			// The front was cut: ImGui's caret and selection are offsets into the old text.
			if (wasActive && wantA_ == size_t(-1))
			{
				wantA_ = anchor_;
				wantB_ = caret_;
			}
			shifted_ = false;
		}
		if (text.size() != size_)
		{
			if (text.size() < size_) anchor_ = caret_ = size_; // cleared: back to the end
			// A caret at the end follows the text; ImGui scrolls to a caret that moved.
			const bool atEnd = following();
			if (atEnd)
			{
				anchor_ = caret_ = text.size();
				if (wasActive) wantA_ = wantB_ = caret_;
			}
			if (atEnd || atBottom_) scrollEnd_ = true;
			size_ = text.size();
			selA_ = std::min(selA_, size_);
			selB_ = std::min(selB_, size_);
		}
		if (focusNext_)
		{
			ImGui::SetKeyboardFocusHere();
			focusNext_ = false;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0)); // drawn below, coloured
		ImGui::InputTextMultiline(label, &text, size,
								  ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackAlways,
								  &LogView::Callback, this);
		ImGui::PopStyleColor();
		const bool active = ImGui::IsItemActive();
		const ImRect box(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		if (resume_ && !ImGui::IsItemActivated() && ++resumeWait_ > 3) resume_ = false; // the keyboard went elsewhere
		if (!resume_) resumeWait_ = 0;
		if (ImGui::IsItemActivated() && resume_)
		{
			// Back from the menu: the selection it had, without scrolling to the caret.
			if (ImGuiInputTextState* st = ImGui::GetInputTextState(id))
			{
				selA_ = std::min(resumeA_, text.size());
				selB_ = std::min(resumeB_, text.size());
				st->SetSelection(int(selA_), int(selB_));
			}
			resume_ = false;
		}

		// Right-click: copy the selection, or all of it.
		const ImGuiID menuId = ImHashStr("##logmenu", 0, id);
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsMouseHoveringRect(box.Min, box.Max) &&
			ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
		{
			if (!menuOpen_)
			{
				hadKeyboard_ = active;
				resumeA_ = selA_;
				resumeB_ = selB_;
			}
			ImGui::OpenPopupEx(menuId);
		}
		const bool menuWas = menuOpen_, openBefore = ImGui::IsPopupOpen(menuId, ImGuiPopupFlags_None);
		if (ImGui::BeginPopupEx(menuId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings))
		{
			const size_t b = std::min(selB_, text.size()), a = std::min(selA_, b);
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, b > a)) ImGui::SetClipboardText(text.substr(a, b - a).c_str());
			if (ImGui::MenuItem("Copy All", nullptr, false, !text.empty())) ImGui::SetClipboardText(text.c_str());
			ImGui::Separator();
			if (ImGui::MenuItem("Select All", "Ctrl+A", false, !text.empty()))
			{
				wantA_ = 0;
				wantB_ = text.size();
				focusNext_ = true;
			}
			ImGui::EndPopup();
		}
		menuOpen_ = ImGui::IsPopupOpen(menuId, ImGuiPopupFlags_None);
		if (menuWas && !menuOpen_ && hadKeyboard_ && wantA_ == size_t(-1) && !(!openBefore && MouseClickedOrHeld()))
		{
			resume_ = true;
			focusNext_ = true;
		}

		char name[512];
		std::snprintf(name, sizeof(name), "%s/%s_%08X", parent->Name, label, id);
		ImGuiWindow* child = ImGui::FindWindowByName(name);
		// InputText doesn't begin its child window while the box is out of view (a
		// panel too short to show it), and the window found is then an earlier
		// frame's. ImGui frees the draw list of a window unused for a minute
		// (io.ConfigMemoryCompactTimer), so only a child begun this frame is drawn
		// into (crashdumps/crash3).
		if (!child || !child->Active) return;
		// The width ImGui wraps at: what is left of the row after the frame padding,
		// less a scrollbar's width while there is none (it keeps room for one).
		const float avail = child->WorkRect.Max.x - (child->Pos.x + style.FramePadding.x);
		const float wrap = std::max(1.0f, avail + (child->ScrollbarY ? 0.0f : -style.ScrollbarSize));
		layout(text, wrap);
		const float fontSize = ImGui::GetFontSize();
		atBottom_ = child->Scroll.y >= child->ScrollMax.y - 1.0f;
		if (scrollEnd_)
		{
			// Past the end, clamped to it when it applies (next frame). FLT_MAX would
			// mean "no target" to ImGui.
			ImGui::SetScrollY(child, child->ScrollMax.y + (float(rows_.size()) + 2.0f) * fontSize + style.FramePadding.y * 2.0f);
			scrollEnd_ = false;
		}
		const ImVec2 origin(child->Pos.x + style.FramePadding.x, child->Pos.y + style.FramePadding.y - child->Scroll.y);
		const ImRect clip = child->InnerClipRect;
		const int first = std::max(0, int((clip.Min.y - origin.y) / fontSize) - 1);
		const int last = std::min(int(rows_.size()), int((clip.Max.y - origin.y) / fontSize) + 2);
		ImDrawList* dl = child->DrawList;
		dl->PushClipRect(clip.Min, clip.Max, true);
		if (menuOpen_ && hadKeyboard_ && !active && resumeA_ < resumeB_)
		{
			// The selection InputText draws only while it has the keyboard.
			const size_t a = std::min(resumeA_, text.size()), b = std::min(resumeB_, text.size());
			const ImU32 sel = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
			for (int r = first; r < last; ++r)
			{
				const size_t rs = rows_[size_t(r)];
				size_t re = size_t(r + 1) < rows_.size() ? rows_[size_t(r + 1)] : text.size();
				if (b <= rs || a >= re) continue;
				while (re > rs && (text[re - 1] == '\n' || text[re - 1] == '\r')) --re;
				const size_t sa = std::max(a, rs), sb = std::min(b, re);
				float w = sa < sb ? ImGui::CalcTextSize(text.data() + sa, text.data() + sb).x : 0.0f;
				if (b > re) w += std::floor(ImGui::CalcTextSize(" ").x * 0.5f);
				const float x = origin.x + ImGui::CalcTextSize(text.data() + rs, text.data() + sa).x;
				const float y = origin.y + float(r) * fontSize;
				dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + fontSize), sel);
			}
		}
		for (int r = first; r < last; ++r)
		{
			const char* s = text.data() + rows_[size_t(r)];
			const char* e = size_t(r + 1) < rows_.size() ? text.data() + rows_[size_t(r + 1)] : text.data() + text.size();
			while (e > s && (e[-1] == '\n' || e[-1] == '\r')) --e;
			const uint32_t line = rowLine_[size_t(r)];
			const ImU32 col = colours[line < levels.size() ? levels[line] : 0];
			dl->AddText(ImGui::GetFont(), fontSize, ImVec2(origin.x, origin.y + float(r) * fontSize), col, s, e);
		}
		dl->PopClipRect();
	}

	// ------------------------------------------------------------------ undo ---

	void CodeEditor::pushUndo(Snapshot s)
	{
		undo_.push_back(std::move(s));
		// The last 200 steps, and no more than 64 MB of text.
		size_t bytes = 0;
		for (const Snapshot& u : undo_) bytes += u.text.size();
		while (undo_.size() > 200 || (undo_.size() > 1 && bytes > (size_t(64) << 20)))
		{
			bytes -= undo_.front().text.size();
			undo_.erase(undo_.begin());
		}
	}

	void CodeEditor::setText(std::string& text, Snapshot to)
	{
		if (active_)
		{
			pendingSet_ = std::move(to); // the InputText owns the text while active
			return;
		}
		text = to.text;
		shadow_ = text;
		shadowSet_ = true;
		invalidate();
		select(to.cursor, to.cursor);
	}

	void CodeEditor::select(size_t a, size_t b)
	{
		pendingSelA_ = a;
		pendingSelB_ = b;
		focusNext_ = !active_;
	}

	bool CodeEditor::undo(std::string& text)
	{
		if (undo_.empty()) return false;
		redo_.push_back({ text, cursor_ });
		Snapshot s = std::move(undo_.back());
		undo_.pop_back();
		setText(text, std::move(s));
		lastEdit_ = -1e9; // typing after it is a step of its own
		return true;
	}

	bool CodeEditor::redo(std::string& text)
	{
		if (redo_.empty()) return false;
		pushUndo({ text, cursor_ });
		Snapshot s = std::move(redo_.back());
		redo_.pop_back();
		setText(text, std::move(s));
		lastEdit_ = -1e9;
		return true;
	}

	// Right-click: undo and the clipboard. The menu takes the keyboard from the
	// InputText, so the text is changed directly and the caret put back after.
	void CodeEditor::contextMenu(std::string& text, bool readOnly, bool& edited)
	{
		const size_t a = std::min({ selBegin_, selEnd_, text.size() }), b = std::min(std::max(selBegin_, selEnd_), text.size());
		const bool sel = b > a;
		// (Cut and Copy replace the clipboard's buffer: read it again to paste.)
		const char* clip = ImGui::GetClipboardText();
		const bool canPaste = clip && *clip;
		auto change = [&](std::string next, size_t cursor)
		{
			pushUndo({ text, a });
			redo_.clear();
			text = std::move(next);
			shadow_ = text;
			invalidate();
			select(cursor, cursor);
			lastEdit_ = -1e9;
			edited = true;
		};
		if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !readOnly && canUndo())) edited |= undo(text);
		if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !readOnly && canRedo())) edited |= redo(text);
		ImGui::Separator();
		if (ImGui::MenuItem("Cut", "Ctrl+X", false, !readOnly && sel))
		{
			ImGui::SetClipboardText(text.substr(a, b - a).c_str());
			change(text.substr(0, a) + text.substr(b), a);
		}
		if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel)) ImGui::SetClipboardText(text.substr(a, b - a).c_str());
		if (ImGui::MenuItem("Paste", "Ctrl+V", false, !readOnly && canPaste) && ImGui::GetClipboardText())
		{
			std::string in = ImGui::GetClipboardText();
			in.erase(std::remove(in.begin(), in.end(), '\r'), in.end());
			change(text.substr(0, a) + in + text.substr(b), a + in.size());
		}
		if (ImGui::MenuItem("Delete", "Del", false, !readOnly && sel)) change(text.substr(0, a) + text.substr(b), a);
		ImGui::Separator();
		if (ImGui::MenuItem("Select All", "Ctrl+A", false, !text.empty())) select(0, text.size());
	}

	int CodeEditor::Callback(ImGuiInputTextCallbackData* data)
	{
		auto* self = static_cast<CodeEditor*>(data->UserData);
		if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways) return 0;
		if (self->pendingSet_)
		{
			// An undo or redo while typing.
			const std::string& t = self->pendingSet_->text;
			data->DeleteChars(0, data->BufTextLen);
			data->InsertChars(0, t.data(), t.data() + t.size());
			data->CursorPos = data->SelectionStart = data->SelectionEnd = int(std::min(self->pendingSet_->cursor, t.size()));
			self->pendingSet_.reset();
			self->settingText_ = true;
		}
		if (self->pendingSelA_ != size_t(-1))
		{
			const size_t n = size_t(data->BufTextLen);
			data->SelectionStart = int(std::min(self->pendingSelA_, n));
			data->SelectionEnd = data->CursorPos = int(std::min(self->pendingSelB_, n));
			self->pendingSelA_ = self->pendingSelB_ = size_t(-1);
		}
		if (self->gotoLine_ && !self->lines_.empty())
		{
			const int l = std::min<int>(self->gotoLine_, int(self->lines_.size()));
			const int at = int(std::min<size_t>(self->lines_[size_t(l - 1)].begin, size_t(data->BufTextLen)));
			data->CursorPos = data->SelectionStart = data->SelectionEnd = at;
			self->gotoLine_ = 0;
		}
		self->cursor_ = size_t(data->CursorPos);
		self->selBegin_ = size_t(data->SelectionStart);
		self->selEnd_ = size_t(data->SelectionEnd);
		return 0;
	}

	size_t CodeEditor::offsetAt(const std::string& text, ImVec2 mouse, ImVec2 origin, ImFont* font, float size) const
	{
		if (lines_.empty()) return 0;
		const int li = std::clamp(int(std::floor((mouse.y - origin.y) / size)), 0, int(lines_.size()) - 1);
		const char* b = text.data() + std::min<size_t>(lines_[size_t(li)].begin, text.size());
		const char* e = size_t(li + 1) < lines_.size() ? text.data() + lines_[size_t(li + 1)].begin : text.data() + text.size();
		while (e > b && (e[-1] == '\n' || e[-1] == '\r')) --e;
		// InputText's rule: the caret goes before the character whose left half was clicked.
		float x = origin.x;
		for (const char* p = b; p < e;)
		{
			unsigned int c = 0;
			const int n = std::max(1, ImTextCharFromUtf8(&c, p, e));
			const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, p + n).x;
			if (mouse.x < x + w * 0.5f) return size_t(p - text.data());
			x += w;
			p += n;
		}
		return size_t(e - text.data());
	}

	// The selection as InputText draws it, for while it doesn't (it has lost the keyboard).
	void CodeEditor::drawSelection(ImDrawList* dl, const std::string& text, ImVec2 origin, int first, int last, ImFont* font, float size) const
	{
		const size_t a = std::min({ resumeA_, resumeB_, text.size() }), b = std::min(std::max(resumeA_, resumeB_), text.size());
		if (a >= b) return;
		const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
		const float eol = std::floor(font->CalcTextSizeA(size, FLT_MAX, 0.0f, " ").x * 0.5f); // so a selected empty line shows
		for (int i = first; i < last; ++i)
		{
			const size_t lb = lines_[size_t(i)].begin;
			size_t le = size_t(i + 1) < lines_.size() ? lines_[size_t(i + 1)].begin : text.size();
			const bool newline = le > lb && text[le - 1] == '\n';
			if (newline) --le;
			if (b <= lb || a > le || (a == le && !newline)) continue;
			const size_t sa = std::max(a, lb), sb = std::min(b, le);
			float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data() + sa, text.data() + sb).x;
			if (newline && b > le) w += eol;
			if (w <= 0.0f) continue;
			const float x = origin.x + font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data() + lb, text.data() + sa).x;
			const float y = origin.y + float(i) * size;
			dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + size), col);
		}
	}

	bool CodeEditor::draw(const char* label, std::string& text, const ImVec2& size, ImGuiInputTextFlags flags)
	{
		ImGuiContext& g = *ImGui::GetCurrentContext();
		const ImGuiStyle& style = ImGui::GetStyle();
		ImFont* font = ImGui::GetFont();
		const float fontSize = ImGui::GetFontSize();
		const bool light = (style.Colors[ImGuiCol_WindowBg].x + style.Colors[ImGuiCol_WindowBg].y + style.Colors[ImGuiCol_WindowBg].z) > 1.5f;
		const Palette pal = PaletteFor(light);

		// The line index follows the text; a cheap sample tells a replaced text of the same size.
		uint64_t sample = text.size();
		for (size_t i = 0; i < text.size() && i < 64; ++i) sample = sample * 131 + uint8_t(text[i]);
		for (size_t i = text.size() > 64 ? text.size() - 64 : 0; i < text.size(); ++i) sample = sample * 137 + uint8_t(text[i]);
		if (cachedSize_ != text.size() || sample != cachedSample_)
		{
			rebuild(text);
			cachedSample_ = sample;
		}
		// A change made outside (Add to Script, a recorded line) is a step too.
		if (!shadowSet_)
		{
			shadow_ = text;
			shadowSet_ = true;
		}
		else if (text != shadow_)
		{
			pushUndo({ shadow_, std::min(cursor_, shadow_.size()) });
			redo_.clear();
			shadow_ = text;
			lastEdit_ = -1e9;
		}
		frameCursor_ = cursor_;

		// Gutter: wide enough for the last line number.
		char digits[16];
		std::snprintf(digits, sizeof(digits), "%zu", std::max<size_t>(lines_.size(), 99));
		const float gutterW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, digits).x + style.FramePadding.x * 2;
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const ImVec2 full = ImGui::CalcItemSize(size, avail.x, fontSize * 8 + style.FramePadding.y * 2);
		const ImVec2 gutterPos = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(gutterW, full.y));
		ImGui::SameLine(0, 0);

		ImGuiWindow* parent = ImGui::GetCurrentWindow();
		const ImGuiID id = parent->GetID(label);
		// The InputText's child window, where its text would have been drawn (as
		// of the last frame until the InputText is drawn).
		char name[512];
		std::snprintf(name, sizeof(name), "%s/%s_%08X", parent->Name, label, id);
		ImGuiWindow* child = ImGui::FindWindowByName(name);

		// A horizontal scrollbar under the box while the longest line is wider
		// than the box (or the box is scrolled).
		if (widestDirty_ || widestFont_ != fontSize)
		{
			widest_ = 0;
			for (size_t i = 0; i < lines_.size(); ++i)
			{
				const char* b = text.data() + std::min<size_t>(lines_[i].begin, text.size());
				const char* e = i + 1 < lines_.size() ? text.data() + lines_[i + 1].begin : text.data() + text.size();
				while (e > b && (e[-1] == '\n' || e[-1] == '\r')) --e;
				widest_ = std::max(widest_, font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, b, e).x);
			}
			widestFont_ = fontSize;
			widestDirty_ = false;
		}
		const float boxW = full.x - gutterW;
		// The width InputText shows text in (it keeps the caret inside it).
		const float textW = std::max(1.0f, boxW - (child && child->ScrollbarY ? style.ScrollbarSize : 0.0f) - style.FramePadding.x);
		const float contentW = widest_ + style.FramePadding.x; // a margin after the longest line, for the caret
		const float maxScroll = std::max(0.0f, contentW - textW);
		if (!active_) scrollX_ = std::clamp(scrollX_, 0.0f, maxScroll); // while typing, InputText may scroll past it
		const bool bar = maxScroll > 0.0f || scrollX_ > 0.0f;
		const float barH = bar ? style.ScrollbarSize : 0.0f;
		// The scrollbar goes first: ImGui gives the click to the first item under
		// the mouse that allows it, and an active InputText doesn't.
		const ImVec2 boxMin = ImGui::GetCursorScreenPos();
		const ImRect barBox(boxMin.x, boxMin.y + full.y - barH, boxMin.x + boxW, boxMin.y + full.y);
		if (bar)
		{
			const ImGuiID barId = ImHashStr("##hscroll", 0, id);
			ImS64 v = ImS64(scrollX_);
			ImGui::KeepAliveID(barId);
			if (ImGui::ScrollbarEx(barBox, barId, ImGuiAxis_X, &v, ImS64(textW), ImS64(std::max(contentW, scrollX_ + textW))))
			{
				if (!barHeld_)
				{
					hadKeyboard_ = active_;
					resumeA_ = selBegin_;
					resumeB_ = selEnd_;
				}
				barHeld_ = true;
				scrollX_ = float(v);
			}
			else if (barHeld_)
			{
				barHeld_ = false;
				if (hadKeyboard_)
				{
					resume_ = true;
					focusNext_ = true;
				}
			}
		}
		ImGuiInputTextState* st = ImGui::GetInputTextState(id);
		if (st && active_) st->Scroll.x = scrollX_; // the scrollbar or the wheel may have moved it

		if (focusNext_)
		{
			ImGui::SetKeyboardFocusHere();
			focusNext_ = false;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0)); // the overlay draws the text
		settingText_ = false;
		bool edited = ImGui::InputTextMultiline(label, &text, ImVec2(boxW, full.y - barH),
												flags | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_NoUndoRedo,
												&CodeEditor::Callback, this);
		ImGui::PopStyleColor();
		const bool activated = ImGui::IsItemActivated();
		active_ = ImGui::IsItemActive();
		const ImRect box(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		if (!child) child = ImGui::FindWindowByName(name);
		st = ImGui::GetInputTextState(id);
		if (activated && st)
		{
			// InputText starts scrolled to the left, and placed a click's caret by
			// that: scroll back to where the editor was, and place it again.
			st->Scroll.x = scrollX_;
			const size_t n = text.size();
			if (resume_)
			{
				st->SetSelection(int(std::min(resumeA_, n)), int(std::min(resumeB_, n)));
				selBegin_ = std::min(resumeA_, n);
				selEnd_ = cursor_ = std::min(resumeB_, n);
			}
			else if (scrollX_ > 0.0f && child && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g.IO.KeyShift)
			{
				const ImVec2 origin(child->Pos.x + style.FramePadding.x - scrollX_, child->Pos.y + style.FramePadding.y - child->Scroll.y);
				const size_t at = offsetAt(text, g.IO.MousePos, origin, font, fontSize);
				st->SetSelection(int(at), int(at));
				selBegin_ = selEnd_ = cursor_ = at;
			}
			else
			{
				st->CursorFollow = true; // a caret placed by select(): into view from there
			}
			resume_ = false;
		}
		else if (st && active_)
		{
			scrollX_ = st->Scroll.x; // it follows the caret
		}
		if (resume_ && !focusNext_ && ++resumeWait_ > 3) resume_ = false; // the keyboard went elsewhere
		if (!resume_) resumeWait_ = 0;
		// Shift+wheel or a sideways wheel over the box, or any wheel over its scrollbar.
		if (bar && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
		{
			float wheel = 0;
			if (box.Contains(g.IO.MousePos)) wheel = g.IO.MouseWheelH != 0.0f ? g.IO.MouseWheelH : g.IO.KeyShift ? g.IO.MouseWheel : 0.0f;
			else if (barBox.Contains(g.IO.MousePos)) wheel = g.IO.MouseWheelH != 0.0f ? g.IO.MouseWheelH : g.IO.MouseWheel;
			if (wheel != 0.0f)
			{
				scrollX_ = std::clamp(scrollX_ - wheel * fontSize * 3.0f, 0.0f, std::max(maxScroll, scrollX_));
				if (st && active_) st->Scroll.x = scrollX_;
			}
		}
		const bool readOnly = (flags & ImGuiInputTextFlags_ReadOnly) != 0;
		if (edited && !settingText_)
		{
			// Typing is one step until a pause or a jump of the caret; a paste or
			// a cut is a step of its own.
			const double now = ImGui::GetTime();
			const long delta = long(text.size()) - long(shadow_.size());
			if (now - lastEdit_ > 1.0 || (delta != 1 && delta != -1) || frameCursor_ != lastEditCursor_)
			{
				pushUndo({ shadow_, frameCursor_ });
			}
			redo_.clear();
			lastEdit_ = now;
			lastEditCursor_ = cursor_;
		}
		if (edited) shadow_ = text;
		if (pendingSet_ && !active_)
		{
			// Undone while typing, but the box let go before it could take it.
			text = pendingSet_->text;
			shadow_ = text;
			pendingSet_.reset();
			edited = true;
		}
		if (active_ && !readOnly)
		{
			using namespace ImGui;
			const ImGuiInputFlags rep = ImGuiInputFlags_Repeat;
			if (IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z, rep, ImGuiKeyOwner_Any)) undo(text);
			else if (IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y, rep, ImGuiKeyOwner_Any) ||
					 IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, rep, ImGuiKeyOwner_Any))
				redo(text);
		}
		// The right-click menu, over the box (not over another window on top of it).
		const ImGuiID menuId = ImHashStr("##codemenu", 0, id);
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsMouseHoveringRect(box.Min, box.Max) &&
			ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
		{
			if (!menuOpen_ && !menuClicked_)
			{ // (a right-click that closed the last menu keeps its selection)
				hadKeyboard_ = active_;
				resumeA_ = selBegin_;
				resumeB_ = selEnd_;
			}
			ImGui::OpenPopupEx(menuId);
		}
		const bool menuWas = menuOpen_, openBefore = ImGui::IsPopupOpen(menuId, ImGuiPopupFlags_None);
		if (ImGui::BeginPopupEx(menuId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings))
		{
			contextMenu(text, readOnly, edited);
			ImGui::EndPopup();
		}
		menuOpen_ = ImGui::IsPopupOpen(menuId, ImGuiPopupFlags_None);
		if (menuWas && !openBefore && MouseClickedOrHeld())
		{
			menuClicked_ = true; // closed by a click elsewhere, which takes the keyboard
		}
		else if (menuWas && !menuOpen_ && hadKeyboard_ && pendingSelA_ == size_t(-1) && !pendingSet_)
		{
			// Closed by a command that left the caret alone, or by Escape: the
			// keyboard comes back with the selection.
			resume_ = true;
			focusNext_ = true;
		}
		if (!menuOpen_ && !MouseClickedOrHeld()) menuClicked_ = false;
		if (edited)
		{
			rebuild(text);
			sample = text.size();
			for (size_t i = 0; i < text.size() && i < 64; ++i) sample = sample * 131 + uint8_t(text[i]);
			for (size_t i = text.size() > 64 ? text.size() - 64 : 0; i < text.size(); ++i) sample = sample * 137 + uint8_t(text[i]);
			cachedSample_ = sample;
		}
		if (active_) cursorLine_ = lineOf(cursor_);

		// Not begun this frame (the box is out of view): its draw list is an earlier
		// frame's, or freed (LogView::draw says why).
		if (!child || !child->Active || lines_.empty()) return edited;
		const ImVec2 origin(child->Pos.x + style.FramePadding.x - scrollX_, child->Pos.y + style.FramePadding.y - child->Scroll.y);
		if (scrollLine_)
		{
			// Into view, a few lines from the top.
			ImGui::SetScrollY(child, std::max(0.0f, float(scrollLine_ - 4) * fontSize));
			scrollLine_ = 0;
		}
		const ImRect clip = child->InnerClipRect;
		const int first = std::max(0, int((clip.Min.y - origin.y) / fontSize) - 1);
		const int last = std::min(int(lines_.size()), int((clip.Max.y - origin.y) / fontSize) + 2);

		ImDrawList* dl = child->DrawList;
		dl->PushClipRect(clip.Min, clip.Max, true);
		if (!active_ && hadKeyboard_ && (menuOpen_ || barHeld_)) drawSelection(dl, text, origin, first, last, font, fontSize);
		const bool showText = !text.empty();
		for (int i = first; i < last; ++i)
		{
			const float y = origin.y + float(i) * fontSize;
			const int lineNo = i + 1;
			const bool err = errors_.count(lineNo) != 0, warn = !err && warnings_.count(lineNo) != 0;
			if (err || warn)
			{
				const ImU32 band = (err ? pal.error : pal.warning) & 0x30FFFFFF;
				dl->AddRectFilled(ImVec2(clip.Min.x, y), ImVec2(clip.Max.x, y + fontSize), band);
			}
			else if (lineNo == cursorLine_ && active_)
			{
				dl->AddRectFilled(ImVec2(clip.Min.x, y), ImVec2(clip.Max.x, y + fontSize), pal.currentLine);
			}
			if (!showText) continue;
			const char* b = text.data() + lines_[size_t(i)].begin;
			const char* e = size_t(i + 1) < lines_.size() ? text.data() + lines_[size_t(i + 1)].begin : text.data() + text.size();
			while (e > b && (e[-1] == '\n' || e[-1] == '\r')) --e;
			drawLine(dl, font, fontSize, ImVec2(origin.x, y), b, e, lines_[size_t(i)].state, pal);
		}
		dl->PopClipRect();

		// A known name's line on hover.
		if (!known_.empty() && clip.Contains(g.IO.MousePos) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			const int li = int((g.IO.MousePos.y - origin.y) / fontSize);
			if (li >= 0 && size_t(li) < lines_.size())
			{
				const char* b = text.data() + lines_[size_t(li)].begin;
				const char* e = size_t(li + 1) < lines_.size() ? text.data() + lines_[size_t(li + 1)].begin : text.data() + text.size();
				float x = origin.x;
				for (const char* p = b; p < e && *p != '\n'; ++p)
				{
					const float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, p, p + 1).x;
					if (g.IO.MousePos.x < x + w)
					{
						if (WordChar(*p) || *p == '.')
						{
							const char* wb = p;
							while (wb > b && (WordChar(wb[-1]) || wb[-1] == '.')) --wb;
							const char* we = p;
							while (we < e && (WordChar(*we) || *we == '.')) ++we;
							if (const KnownName* k = knownAt(b, wb, we); k && !k->doc.empty()) ImGui::SetTooltip("%s", k->doc.c_str());
						}
						break;
					}
					x += w;
				}
			}
		}

		// Line numbers, beside the box and scrolled with it.
		ImDrawList* pdl = ImGui::GetWindowDrawList();
		const ImRect gutter(gutterPos, ImVec2(gutterPos.x + gutterW, gutterPos.y + full.y));
		pdl->PushClipRect(ImVec2(gutter.Min.x, clip.Min.y), ImVec2(gutter.Max.x, clip.Max.y), true);
		for (int i = first; i < last; ++i)
		{
			const float y = origin.y + float(i) * fontSize;
			const int lineNo = i + 1;
			char num[16];
			std::snprintf(num, sizeof(num), "%d", lineNo);
			const float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, num).x;
			const ImU32 col = errors_.count(lineNo) ? pal.error : warnings_.count(lineNo) ? pal.warning : lineNo == cursorLine_ ? pal.gutterCurrent : pal.gutter;
			pdl->AddText(font, fontSize, ImVec2(gutter.Max.x - style.FramePadding.x - w, y), col, num);
		}
		pdl->PopClipRect();
		// A marked line's message on hovering its number.
		if (ImGui::IsMouseHoveringRect(gutter.Min, gutter.Max) && (!errors_.empty() || !warnings_.empty()))
		{
			const int lineNo = int((g.IO.MousePos.y - origin.y) / fontSize) + 1;
			if (auto it = errors_.find(lineNo); it != errors_.end()) ImGui::SetTooltip("%s", it->second.c_str());
			else if (auto w = warnings_.find(lineNo); w != warnings_.end()) ImGui::SetTooltip("%s", w->second.c_str());
		}
		return edited;
	}

} // namespace app
