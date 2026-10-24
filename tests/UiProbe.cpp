#include "UiProbe.h"

#include <unordered_map>

namespace test {

UiProbe& UiProbe::get() {
    static UiProbe p;
    return p;
}

void UiProbe::endFrame() {
    // A child window registers its ChildId in the parent, and a multiline text
    // box (which is a child) registers the same id again: not a conflict.
    std::unordered_map<ImGuiID, int> seen;
    if (ImGuiContext* g = ImGui::GetCurrentContext())
        for (ImGuiWindow* w : g->Windows)
            if (w->ChildId) seen[w->ChildId] = -1000;
    for (const ProbeItem& it : items)
        if (++seen[it.id] == 2)
            conflicts.push_back("'" + it.label + "' in " + it.window);
}

const ProbeItem* UiProbe::find(const std::string& label, const std::string& windowPart) const {
    for (const ProbeItem& it : items)
        if (it.label == label && (windowPart.empty() || it.window.find(windowPart) != std::string::npos)) return &it;
    return nullptr;
}

int UiProbe::count(const std::string& label, const std::string& windowPart) const {
    int n = 0;
    for (const ProbeItem& it : items)
        n += it.label == label && (windowPart.empty() || it.window.find(windowPart) != std::string::npos);
    return n;
}

} // namespace test

// ImGui test-engine hooks.

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* itemData) {
    ImGuiWindow* w = ctx->CurrentWindow;
    // Whole windows and their title bars register too, once per Begin.
    if (!itemData || id == 0 || (w && (id == w->ID || id == w->MoveId))) return;
    test::ProbeItem it;
    it.id = id;
    it.rect = bb;
    it.window = w ? w->Name : "";
    auto& early = test::UiProbe::get().earlyLabels;
    for (auto e = early.begin(); e != early.end(); ++e)
        if (e->first == id) {
            it.label = e->second;
            early.erase(e);
            break;
        }
    test::UiProbe::get().items.push_back(std::move(it));
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    auto& items = test::UiProbe::get().items;
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        if (it->id == id) {
            if (label) it->label = label;
            return;
        }
    if (label) test::UiProbe::get().earlyLabels.emplace_back(id, label);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
