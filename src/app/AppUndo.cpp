// Undo and redo (App.h): after each action the packs it changed are written
// to bytes; undo loads the database again from a step's bytes and points the
// tabs and panels at the same collections in it.

#include "App.h"

#include "core/Bytes.h"
#include "core/Writer.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>

namespace app
{

	using namespace attrib;

	namespace
	{

		using Id = std::pair<uint32_t, uint32_t>; // class, collection; {0, 0} for none

		Id IdOf(const Collection* c)
		{
			return c ? Id{ c->classKey, c->key } : Id{ 0, 0 };
		}

		uint64_t Key(const Collection* c)
		{
			return (uint64_t(c->classKey) << 32) | c->key;
		}

	} // namespace

	void App::resetUndo()
	{
		arrayTails_.clear(); // they point into the old database
		undo_.clear();
		savedPacks_.clear();
		savedCompression_.clear();
		undoPacks_.clear();
		undoTouched_.clear();
		undoChanged_ = undoAll_ = undoScript_ = false;
		undoLabel_.clear();
		undoPos_ = -1;
		if (!db_ || db_->packs().empty()) return;
		UndoState first;
		first.label = "Open";
		for (const auto& pk : db_->packs())
		{
			// A pack as read is its file; one changed since needs writing.
			std::shared_ptr<const Bytes> b;
			try
			{
				b = std::make_shared<const Bytes>(pk->dirty ? WritePack(*db_, *pk) : ReadFile(pk->path));
			}
			catch (const std::exception& e)
			{
				log(LogLevel::Warning, "Undo is off: could not keep " + pk->path + ": " + e.what());
				return;
			}
			first.packs.push_back(b);
			first.compression.push_back(pk->compression);
			savedPacks_.push_back(pk->dirty ? nullptr : b);
			savedCompression_.push_back(pk->compression);
		}
		undo_.push_back(std::move(first));
		undoPos_ = 0;
	}

	void App::noteEdit(const LoadedPack* pack, const Collection* c)
	{
		if (!undoChanged_)
		{
			undoActiveId_ = ImGui::GetCurrentContext() ? ImGui::GetActiveID() : 0;
			undoSelBefore_ = IdOf(selected_);
		}
		undoChanged_ = true;
		if (pack) undoPacks_.insert(pack);
		else undoAll_ = true;
		if (c) undoTouched_.insert(Key(c));
	}

	void App::nameAction(const std::string& label)
	{
		if (!undoLabel_.empty()) return;
		undoLabel_ = label;
		// A menu item's width: a long console line is cut, between characters.
		if (undoLabel_.size() > 80)
		{
			size_t cut = 77;
			while (cut > 0 && (static_cast<unsigned char>(undoLabel_[cut]) & 0xC0) == 0x80) --cut;
			undoLabel_ = undoLabel_.substr(0, cut) + "...";
		}
	}

	void App::commitUndo(bool force)
	{
		if (!undoChanged_)
		{
			undoLabel_.clear(); // named, but nothing changed (a script that failed)
			return;
		}
		// A drag or a field being typed into is one action until it lets go (or
		// another widget takes over: typing in one field, then clicking the next).
		if (!force && ImGui::GetCurrentContext() && ImGui::IsAnyItemActive() && ImGui::GetActiveID() == undoActiveId_) return;
		const bool all = undoAll_;
		std::set<const LoadedPack*> packs;
		packs.swap(undoPacks_);
		std::set<uint64_t> touched;
		touched.swap(undoTouched_);
		std::string label;
		label.swap(undoLabel_);
		const bool script = undoScript_;
		undoChanged_ = undoAll_ = undoScript_ = false;
		if (!db_ || undoPos_ < 0 || undo_[size_t(undoPos_)].packs.size() != db_->packs().size()) return;

		UndoState next;
		next.packs = undo_[size_t(undoPos_)].packs;
		for (const auto& pk : db_->packs()) next.compression.push_back(pk->compression);
		bool any = next.compression != undo_[size_t(undoPos_)].compression;
		for (size_t i = 0; i < db_->packs().size(); ++i)
		{
			const LoadedPack& pk = *db_->packs()[i];
			if (!packs.count(&pk) && !(all && pk.dirty)) continue;
			try
			{
				auto b = std::make_shared<const Bytes>(WritePack(*db_, pk));
				if (*b == *next.packs[i]) continue; // set back to what it was
				next.packs[i] = std::move(b);
				any = true;
			}
			catch (const std::exception& e)
			{
				log(LogLevel::Warning, "Could not keep this step for undo: " + std::string(e.what()));
				return;
			}
		}
		if (!any) return;
		if (label.empty())
		{
			if (touched.size() == 1)
			{
				const uint64_t id = *touched.begin();
				label = "Edit " + db_->names().format(uint32_t(id >> 32)) + "/" + db_->names().format(uint32_t(id));
			}
			else if (!touched.empty())
			{
				label = "Edit " + std::to_string(touched.size()) + " collections";
			}
			else
			{
				label = "Change";
			}
		}
		next.label = std::move(label);
		next.touched = std::move(touched);
		next.script = script;
		next.selBefore = undoSelBefore_;
		next.selAfter = IdOf(selected_);
		undo_.resize(size_t(undoPos_ + 1)); // a new action drops what was undone
		undo_.push_back(std::move(next));
		while (undo_.size() > size_t(kUndoSteps + 1)) undo_.pop_front();
		undoPos_ = int(undo_.size()) - 1;
	}

	bool App::undo()
	{
		return goToUndo(undoPos_ - 1);
	}
	bool App::redo()
	{
		return goToUndo(undoPos_ + 1);
	}

	bool App::goToUndo(int index)
	{
		if (!db_ || pending_ != Pending::None) return false;
		commitUndo(true); // a step still in progress counts first
		if (index < 0 || index >= int(undo_.size()) || index == undoPos_) return false;
		restoreUndo(index);
		return true;
	}

	std::vector<std::string> App::undoLabels() const
	{
		std::vector<std::string> out;
		for (const UndoState& s : undo_) out.push_back(s.label);
		return out;
	}

	void App::restoreUndo(int pos)
	{
		const UndoState& to = undo_[size_t(pos)];
		if (to.packs.size() != db_->packs().size()) return;
		auto fresh = std::make_unique<Database>();
		fresh->setTarget(db_->target());
		if (db_->sharedTypeSchema()) fresh->addTypeSchema(db_->sharedTypeSchema());
		for (const auto& l : db_->classLists()) fresh->addClassList(l); // which classes are the game's
		fresh->names() = db_->names();
		fresh->binNames() = db_->binNames();
		try
		{
			for (size_t i = 0; i < to.packs.size(); ++i)
			{
				const LoadedPack& old = *db_->packs()[i];
				LoadedPack& np = fresh->loadPack(old.path, *to.packs[i]);
				np.compression = i < to.compression.size() ? to.compression[i] : old.compression;
				np.dirty = to.packs[i] != savedPacks_[i] || (i < savedCompression_.size() && np.compression != savedCompression_[i]);
			}
		}
		catch (const std::exception& e)
		{
			log(LogLevel::Error, std::string("Undo failed: ") + e.what());
			return;
		}

		// What the steps between changed, for the recorder.
		std::set<uint64_t> touched;
		bool script = false;
		for (int i = std::min(pos, undoPos_) + 1; i <= std::max(pos, undoPos_); ++i)
		{
			touched.insert(undo_[size_t(i)].touched.begin(), undo_[size_t(i)].touched.end());
			script |= undo_[size_t(i)].script;
		}
		const std::string what = pos < undoPos_ ? "Undid " + undo_[size_t(undoPos_)].label : "Redid " + to.label;
		// Back to what was being edited: before the first step undone, after the last one redone.
		const Id focus = pos < undoPos_ ? undo_[size_t(pos + 1)].selBefore : to.selAfter;

		// Everything pointing into the old database goes by key to the new one.
		std::vector<Id> tabIds;
		for (const Tab& t : tabs_) tabIds.push_back(IdOf(t.collection));
		const Id sbActivity = IdOf(sbActivity_), sbState = IdOf(sbState_), sbHandler = IdOf(sbHandler_);
		const bool handlerUndone = sbHandler_ && touched.count(Key(sbHandler_));
		const Id sbSource = handlerUndone ? Id{ 0, 0 } : IdOf(sbSourceFor_); // its code changed: show the restored one
		selected_ = nullptr;
		for (Tab& t : tabs_) t.collection = nullptr;
		sbActivity_ = sbState_ = sbHandler_ = nullptr;
		sbBoxes_.clear();
		sbSourceFor_ = nullptr;
		pendingTarget_ = pendingTemplate_ = nullptr;
		pendingClass_ = nullptr;
		pendingVault_ = nullptr;
		explorerFocus_ = explorerTarget_ = {};
		dropSource_ = dropParent_ = nullptr;
		dropClass_ = nullptr;
		pendingEdit_ = {};
		reveal_.clear();

		db_ = std::move(fresh);
		arrayTails_.clear();
		undoPos_ = pos;
		structureChanged();
		undoChanged_ = undoAll_ = undoScript_ = false;
		undoPacks_.clear();
		undoTouched_.clear();
		undoLabel_.clear();

		auto find = [&](Id id)
		{
			return id == Id{ 0, 0 } ? nullptr : db_->findCollection(id.first, id.second);
		};
		for (size_t i = tabs_.size(); i-- > 0;)
		{
			Tab& t = tabs_[i];
			t.collection = find(tabIds[i]);
			// Gone (the step made it): the latest place in its history that is there.
			for (int h = int(t.history.size()) - 1; !t.collection && h >= 0; --h)
				if ((t.collection = find(t.history[size_t(h)]))) t.historyPos = h;
			if (!t.collection) closeTab(i);
		}
		if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.empty() ? 0 : tabs_.size() - 1;
		selected_ = tabs_.empty() ? nullptr : tabs_[activeTab_].collection;
		if (Collection* f = find(focus); f && f != selected_) select(f, true);
		sbActivity_ = find(sbActivity);
		sbState_ = sbActivity_ ? find(sbState) : nullptr;
		sbHandler_ = sbState_ ? find(sbHandler) : nullptr;
		sbSourceFor_ = find(sbSource);

		// Recording notes the step back as changes of its own; over a script run,
		// which isn't recorded, it starts again from here.
		if (recordBase_)
		{
			if (script)
			{
				recordBase_ = script::CloneDatabase(*db_);
				recordDirty_.clear();
				recordStructure_ = false;
			}
			else
			{
				recordDirty_.insert(touched.begin(), touched.end());
				recordStructure_ = true;
			}
		}
		log(LogLevel::Info, what);
		updateTitle();
	}

	void App::drawUndoItems()
	{
		const bool can = db_ && pending_ == Pending::None;
		const std::string undoLabel = undoPos_ > 0 ? "Undo " + undo_[size_t(undoPos_)].label : std::string("Undo");
		const std::string redoLabel =
			undoPos_ + 1 < int(undo_.size()) ? "Redo " + undo_[size_t(undoPos_ + 1)].label : std::string("Redo");
		if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, can && undoPos_ > 0)) undo();
		if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, can && undoPos_ + 1 < int(undo_.size()))) redo();
		if (ImGui::BeginMenu("Undo History", can && undo_.size() > 1))
		{
			// Newest first, the current state ticked; picking one goes there.
			int jump = -1;
			for (int i = int(undo_.size()) - 1; i >= 0; --i)
			{
				ImGui::PushID(i);
				if (ImGui::MenuItem(undo_[size_t(i)].label.c_str(), nullptr, i == undoPos_) && i != undoPos_) jump = i;
				ImGui::PopID();
			}
			ImGui::EndMenu();
			if (jump >= 0) goToUndo(jump);
		}
	}

} // namespace app
