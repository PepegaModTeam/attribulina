#pragma once

// A text editor for scripts: ImGui's multiline InputText does the editing
// (cursor, selection, clipboard, IME), and this draws line numbers beside it
// and the text over it in colours by language. The InputText draws its text
// with a transparent colour, so only the caret and selection are its own; the
// coloured text is drawn into its child window at the same positions.
//
// Undo is the editor's own (ImGui's lasts only while the box has the
// keyboard): Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z while typing, and the
// right-click menu, which has the clipboard commands too. Lines wider than
// the box scroll sideways with the scrollbar under it (or Shift+wheel).

#include "imgui.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace app
{

	enum class CodeLanguage
	{
		Plain, Amod, Lua, Nfsms
	};

	// A right-click menu for a one-line text field (Cut, Copy, Paste, Delete,
	// Clear), called right after its InputText. True when it changed the text.
	bool TextFieldMenu(std::string& text);

	// The Log panel's text: a read-only, word-wrapped text box, so it can be
	// selected and copied (Ctrl+A, Ctrl+C, the right-click menu), with each line
	// drawn over it in its level's colour (the box's own text is transparent, as
	// in CodeEditor; rows wrap where ImGui wraps them).
	//
	// The text grows at its end. The caret starts there (the log ends each line
	// with '\n', so it sits on an empty last line), and while it is there new
	// text moves it along and the box scrolls to it. A caret put elsewhere stays
	// put; new text still scrolls into view while the box shows its bottom.
	class LogView
	{
	public:
		// levels: one per line of `text`, an index into `colours`.
		void draw(const char* label, std::string& text, const std::vector<uint8_t>& levels, const ImU32* colours, const ImVec2& size);
		// `bytes` were cut from the front of the text since it was last drawn
		// (old lines dropped), so the caret and selection move back with it.
		void droppedFront(size_t bytes);
		// Where the caret is, as of the last frame (the end unless the user moved it).
		size_t caret() const
		{
			return caret_;
		}
		bool following() const
		{
			return anchor_ == caret_ && caret_ >= size_;
		} // the caret is at the end
// The rows it drew last, as offsets into the text (tests compare them with ImGui's).
		const std::vector<uint32_t>& rows() const
		{
			return rows_;
		}

	private:
		static int Callback(ImGuiInputTextCallbackData* data);
		void layout(const std::string& text, float wrap);
		std::vector<uint32_t> rows_, rowLine_; // start of each wrapped row, and its line
		size_t laidSize_ = size_t(-1);
		uint64_t laidSample_ = 0;
		float laidWrap_ = -1, laidFont_ = -1;
		size_t selA_ = 0, selB_ = 0;
		size_t wantA_ = size_t(-1), wantB_ = size_t(-1);
		size_t size_ = 0;               // the text's size when last drawn
		size_t anchor_ = 0, caret_ = 0; // the selection's fixed end and the caret (equal: no selection)
		bool focusNext_ = false, scrollEnd_ = false, atBottom_ = true, shifted_ = false;
		// The selection, kept and drawn while the right-click menu has the keyboard.
		bool menuOpen_ = false, hadKeyboard_ = false, resume_ = false;
		int resumeWait_ = 0;
		size_t resumeA_ = 0, resumeB_ = 0;
	};

	class CodeEditor
	{
	public:
		explicit CodeEditor(CodeLanguage lang = CodeLanguage::Plain) : lang_(lang)
		{
		}

		// Draws it at `size` (as InputTextMultiline takes it). Returns true when
		// the text was edited. Call with the font it should use pushed.
		bool draw(const char* label, std::string& text, const ImVec2& size, ImGuiInputTextFlags flags = 0);

		// Marks lines (1-based) with a message shown when the line number is hovered.
		void setMarks(std::map<int, std::string> errors, std::map<int, std::string> warnings = {});
		void clearMarks()
		{
			setMarks({});
		}
		// Names the language knows (the game's Lua functions): coloured, and
		// their line shown on hover. A method is keyed ":Name"; `missing` names
		// (Lua's standard library, which the game doesn't open) show as errors.
		struct KnownName
		{
			std::string doc;
			bool missing = false;
		};
		void setKnownNames(std::unordered_map<std::string, KnownName> names)
		{
			known_ = std::move(names);
		}
		// Puts the caret at the start of `line` (1-based) and scrolls to it. With
		// focus false it only scrolls there, leaving the keyboard where it is.
		void gotoLine(int line, bool focus = true);

		// The caret's line (1-based), as of the last frame the editor had the keyboard.
		int cursorLine() const
		{
			return cursorLine_;
		}
		// The caret, as an offset into the text.
		size_t cursorOffset() const
		{
			return cursor_;
		}
		// F12 while the editor has the keyboard, and a `label` item at the top
		// of its right-click menu, ask to show what the caret is on: true once
		// after either. No label: neither is there.
		void setGoTo(std::string label)
		{
			goToLabel_ = std::move(label);
		}
		bool goToRequested()
		{
			const bool r = goToRequest_;
			goToRequest_ = false;
			return r;
		}
		// The lines (1-based) the selection covers, or the caret's line twice.
		void selectedLines(int& first, int& last) const;
		// True once each time the caret moved to another line.
		bool cursorLineChanged()
		{
			const bool c = cursorLine_ != reportedLine_;
			reportedLine_ = cursorLine_;
			return c;
		}
		// The text of that line, or of the selected lines.
		std::string currentLineText(const std::string& text) const;
		// The selected text, or "" with no selection.
		std::string selectedText(const std::string& text) const;
		bool active() const
		{
			return active_;
		}
		float scrollX() const
		{
			return scrollX_;
		} // how far it is scrolled sideways
// Its own undo history (changes made outside the editor are steps too).
		bool canUndo() const
		{
			return !undo_.empty();
		}
		bool canRedo() const
		{
			return !redo_.empty();
		}
		bool undo(std::string& text);
		bool redo(std::string& text);
		// When the text changed outside the editor (opened, replaced).
		void invalidate()
		{
			cachedSize_ = size_t(-1);
		}
		// Another document: what was undone in the last one doesn't apply, and
		// what ImGui keeps of the last one's text doesn't reach it (draw).
		void resetHistory()
		{
			undo_.clear();
			redo_.clear();
			shadowSet_ = false;
			pendingSet_.reset();
			newDocument_ = true;
		}

		struct Palette
		{
			ImU32 text, keyword, string, number, comment, option, known, punct, gutter, gutterCurrent, error, warning, currentLine;
		};
		static Palette PaletteFor(bool light);

	private:
		struct LineInfo
		{
			uint32_t begin;
			uint8_t state; // tokenizer state at the line's start (inside a long string or comment)
		};
		void rebuild(const std::string& text);
		void drawLine(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* b, const char* e, uint8_t state,
					  const Palette& pal) const;
		int lineOf(size_t offset) const;
		static int Callback(ImGuiInputTextCallbackData* data);
		struct Snapshot
		{
			std::string text;
			size_t cursor = 0;
		};
		void pushUndo(Snapshot s);
		// Replaces the text (an undo, a paste from the menu): through the
		// InputText while it has the keyboard, else directly.
		void setText(std::string& text, Snapshot to);
		void select(size_t a, size_t b); // puts the keyboard back in the editor with this selection
		void contextMenu(std::string& text, bool readOnly, bool& edited);
		// The offset a click at `mouse` puts the caret at, with the text drawn from `origin`.
		size_t offsetAt(const std::string& text, ImVec2 mouse, ImVec2 origin, ImFont* font, float size) const;
		void drawSelection(ImDrawList* dl, const std::string& text, ImVec2 origin, int first, int last, ImFont* font, float size) const;

		CodeLanguage lang_;
		std::string goToLabel_;
		bool goToRequest_ = false;
		std::unordered_map<std::string, KnownName> known_;
		const KnownName* knownAt(const char* lineBegin, const char* b, const char* e) const;
		std::map<int, std::string> errors_, warnings_;
		std::vector<LineInfo> lines_;
		size_t cachedSize_ = size_t(-1);
		uint64_t cachedSample_ = 0;
		int cursorLine_ = 1, reportedLine_ = 1;
		size_t cursor_ = 0, selBegin_ = 0, selEnd_ = 0;
		int gotoLine_ = 0;
		int scrollLine_ = 0;
		bool focusNext_ = false;
		bool active_ = false;
		// Horizontal scrolling is the editor's: InputText has no scrollbar for it
		// and starts each activation scrolled to the left.
		float scrollX_ = 0;
		float widest_ = 0, widestFont_ = -1; // the longest line's width, at that font size
		bool widestDirty_ = true;
		bool barHeld_ = false;  // the horizontal scrollbar is being dragged
		bool menuOpen_ = false;    // the right-click menu, as of the last frame
		bool menuClicked_ = false; // a click closed it (and the mouse is still down)
		// The menu and the scrollbar take the keyboard from the InputText, which
		// then stops drawing the selection: the editor draws it, and gives the
		// keyboard back with it afterwards.
		bool hadKeyboard_ = false, resume_ = false;
		int resumeWait_ = 0; // frames since it was asked to take the keyboard back
		size_t resumeA_ = 0, resumeB_ = 0;
		// Undo.
		std::vector<Snapshot> undo_, redo_;
		std::string shadow_; // the text as last seen: tells typing from outside changes
		bool shadowSet_ = false;
		bool newDocument_ = false; // resetHistory since the last draw
		double lastEdit_ = -1e9;
		size_t lastEditCursor_ = 0, frameCursor_ = 0;
		std::optional<Snapshot> pendingSet_; // for the callback to put in place
		bool settingText_ = false;           // the edit this frame was pendingSet_
		size_t pendingSelA_ = size_t(-1), pendingSelB_ = size_t(-1);
	};

} // namespace app
