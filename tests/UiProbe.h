#pragma once

// Sees every widget the UI submits, through ImGui's test-engine hooks (the
// test build defines IMGUI_ENABLE_TEST_ENGINE). Used to find ID conflicts and
// to locate items to click.

#include "imgui.h"
#include "imgui_internal.h"

#include <string>
#include <vector>

namespace test {

struct ProbeItem {
    ImGuiID id = 0;
    ImRect rect;
    std::string label;
    std::string window;
};

class UiProbe {
public:
    static UiProbe& get();

    // Items submitted since the last clear (one frame when cleared per frame).
    std::vector<ProbeItem> items;
    // IDs submitted more than once within one frame, with their labels.
    std::vector<std::string> conflicts;
    // Labels announced before their item is added (tab items do this).
    std::vector<std::pair<ImGuiID, std::string>> earlyLabels;

    void clear() {
        items.clear();
        earlyLabels.clear();
    }
    // Records conflicts for the frame just finished.
    void endFrame();
    const ProbeItem* find(const std::string& label, const std::string& windowPart = "") const;
    int count(const std::string& label, const std::string& windowPart = "") const;
};

} // namespace test
