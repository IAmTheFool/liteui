
#include "liteui.hpp"
#include <array>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>

struct ActivityItem {
    int id;
    std::string title;
    std::string icon;
};

// A single open editor tab.
struct Tab {
    int id;
    std::string title;
};


enum class Theme { Dark, Light, Sepia };
constexpr int kThemeCount = 3; // keep in sync with kPalettes.size() and Theme

struct ThemePalette {
    const char *name;

    Color toolbarBg, toolbarText;
    Color toggleBtnBg, toggleBtnBorder;

    Color activityBarBg, activityActiveBg, activityInactiveBg, activityIconColor;

    Color sidebarBg, sidebarTitleColor, fileRowTextColor, placeholderColor;

    Color dividerBg;

    Color tabBarBg, tabActiveBg, tabInactiveBg;
    Color tabActiveText, tabInactiveText, glyphColor;

    Color editorBg, editorLabelColor;

    Color terminalBg, terminalTitleColor, terminalPromptColor;
};

inline const ThemePalette &paletteFor(Theme t) {
    static const std::array<ThemePalette, kThemeCount> kPalettes = {{
        // -- Dark --------------------------------------------------
        ThemePalette{
            .name = "Dark mode",
            .toolbarBg = Color{51, 51, 55, 255}, .toolbarText = Color{220, 220, 220, 255},
            .toggleBtnBg = Color{60, 60, 64, 255}, .toggleBtnBorder = Color{80, 80, 85, 255},
            .activityBarBg = Color{45, 45, 48, 255},
            .activityActiveBg = Color{60, 60, 64, 255}, .activityInactiveBg = Color{45, 45, 48, 255},
            .activityIconColor = Color{220, 220, 220, 255},
            .sidebarBg = Color{37, 37, 38, 255}, .sidebarTitleColor = Color{180, 180, 180, 255},
            .fileRowTextColor = Color{204, 204, 204, 255}, .placeholderColor = Color{150, 150, 150, 255},
            .dividerBg = Color{30, 30, 30, 255},
            .tabBarBg = Color{37, 37, 38, 255},
            .tabActiveBg = Color{30, 30, 30, 255}, .tabInactiveBg = Color{37, 37, 38, 255},
            .tabActiveText = Color{255, 255, 255, 255}, .tabInactiveText = Color{170, 170, 170, 255},
            .glyphColor = Color{170, 170, 170, 255},
            .editorBg = Color{30, 30, 30, 255}, .editorLabelColor = Color{150, 150, 150, 255},
            .terminalBg = Color{24, 24, 24, 255}, .terminalTitleColor = Color{180, 180, 180, 255},
            .terminalPromptColor = Color{100, 220, 130, 255},
        },
        // -- Light -------------------------------------------------
        ThemePalette{
            .name = "Light mode",
            .toolbarBg = Color{243, 243, 243, 255}, .toolbarText = Color{30, 30, 30, 255},
            .toggleBtnBg = Color{230, 230, 230, 255}, .toggleBtnBorder = Color{200, 200, 200, 255},
            .activityBarBg = Color{236, 236, 236, 255},
            .activityActiveBg = Color{208, 208, 208, 255}, .activityInactiveBg = Color{236, 236, 236, 255},
            .activityIconColor = Color{40, 40, 40, 255},
            .sidebarBg = Color{245, 245, 245, 255}, .sidebarTitleColor = Color{90, 90, 90, 255},
            .fileRowTextColor = Color{50, 50, 50, 255}, .placeholderColor = Color{120, 120, 120, 255},
            .dividerBg = Color{225, 225, 225, 255},
            .tabBarBg = Color{236, 236, 236, 255},
            .tabActiveBg = Color{255, 255, 255, 255}, .tabInactiveBg = Color{236, 236, 236, 255},
            .tabActiveText = Color{30, 30, 30, 255}, .tabInactiveText = Color{110, 110, 110, 255},
            .glyphColor = Color{110, 110, 110, 255},
            .editorBg = Color{255, 255, 255, 255}, .editorLabelColor = Color{160, 160, 160, 255},
            .terminalBg = Color{250, 250, 250, 255}, .terminalTitleColor = Color{90, 90, 90, 255},
            .terminalPromptColor = Color{20, 140, 70, 255},
        },
        // -- Sepia (new third theme) --------------------------------
        ThemePalette{
            .name = "Sepia mode",
            .toolbarBg = Color{224, 202, 168, 255}, .toolbarText = Color{75, 55, 35, 255},
            .toggleBtnBg = Color{214, 190, 152, 255}, .toggleBtnBorder = Color{194, 168, 128, 255},
            .activityBarBg = Color{214, 190, 152, 255},
            .activityActiveBg = Color{194, 168, 128, 255}, .activityInactiveBg = Color{214, 190, 152, 255},
            .activityIconColor = Color{75, 55, 35, 255},
            .sidebarBg = Color{240, 224, 194, 255}, .sidebarTitleColor = Color{110, 85, 55, 255},
            .fileRowTextColor = Color{75, 55, 35, 255}, .placeholderColor = Color{140, 110, 80, 255},
            .dividerBg = Color{194, 168, 128, 255},
            .tabBarBg = Color{232, 212, 178, 255},
            .tabActiveBg = Color{245, 232, 208, 255}, .tabInactiveBg = Color{232, 212, 178, 255},
            .tabActiveText = Color{75, 55, 35, 255}, .tabInactiveText = Color{120, 95, 65, 255},
            .glyphColor = Color{120, 95, 65, 255},
            .editorBg = Color{245, 232, 208, 255}, .editorLabelColor = Color{140, 110, 80, 255},
            .terminalBg = Color{232, 212, 178, 255}, .terminalTitleColor = Color{110, 85, 55, 255},
            .terminalPromptColor = Color{90, 120, 60, 255},
        },
    }};
    return kPalettes[static_cast<size_t>(t)];
}

int main() {
    LiteUI ui("VS Code Style Layout",1000, 650);

    const std::vector<ActivityItem> activities = {
        {0, "Explorer", "E"},
        {1, "Search",   "S"},
        {2, "Settings", "G"},
    };

    auto activeActivity = std::make_shared<int>(0);
    auto sidebarWidth = std::make_shared<float>(240.0f);
    const float minSidebarWidth = 160.0f;
    const float maxSidebarWidth = 480.0f;
    const float dividerWidth = 6.0f;
    const float activityBarWidth = 48.0f;

    auto terminalHeight = std::make_shared<float>(180.0f);
    const float minTerminalHeight = 80.0f;
    const float maxTerminalHeight = 400.0f;
    const float hDividerHeight = 6.0f;

    const float toolbarHeight = 40.0f;
    const float statusBarHeight = 24.0f;
    const float tabBarHeight = 36.0f;


    auto tabs = std::make_shared<std::vector<Tab>>(std::vector<Tab>{
        {0, "main.cpp"},
        {1, "liteui.hpp"},
    });
    auto activeTab = std::make_shared<int>(0); // -1 = no tabs open
    auto nextTabId = std::make_shared<int>(2); // next id to hand out


    auto theme = std::make_shared<Theme>(Theme::Dark);

    auto themeName = [theme]() {
        return std::string(paletteFor(*theme).name);
    };

    auto themeColor = [theme](Color ThemePalette::*field) {
        return std::function<Color()>([theme, field]() {
            return paletteFor(*theme).*field;
        });
    };

    View root;
    root.style.direction = FlexDirection::Column;
    root.style.width = Size::full();
    root.style.height = Size::full();


    View toolbar;
    toolbar.style.width = Size::full();
    toolbar.style.height = Size::pixel(toolbarHeight);
    toolbar.style.flexGrow = 0;
    toolbar.style.flexShrink = 0;
    toolbar.style.backgroundColor = themeColor(&ThemePalette::toolbarBg);
    toolbar.style.direction = FlexDirection::Row;
    toolbar.style.alignItems = Align::Center;
    toolbar.style.justifyContent = Justify::SpaceBetween;
    toolbar.style.padding = EdgeInsets{0, 16, 0, 16};

    Text toolbarTitle;
    toolbarTitle.label = std::string("My Project");
    toolbarTitle.fontSize = 13.0f;
    toolbarTitle.fontWeight = FontWeight::Medium;
    toolbarTitle.color = themeColor(&ThemePalette::toolbarText);
    toolbar.addChild(toolbarTitle);


    Text themeToggleLabel;
    themeToggleLabel.label = std::function<std::string()>(themeName);
    themeToggleLabel.fontSize = 12.0f;
    themeToggleLabel.color = themeColor(&ThemePalette::toolbarText);

    View themeToggleBtn;
    themeToggleBtn.style.height = Size::pixel(24);
    themeToggleBtn.style.borderRadius = 4.0f;
    themeToggleBtn.style.borderWidth = 1.0f;
    themeToggleBtn.style.borderColor = themeColor(&ThemePalette::toggleBtnBorder);
    themeToggleBtn.style.backgroundColor = themeColor(&ThemePalette::toggleBtnBg);
    themeToggleBtn.style.direction = FlexDirection::Row;
    themeToggleBtn.style.alignItems = Align::Center;
    themeToggleBtn.style.justifyContent = Justify::Center;
    themeToggleBtn.style.padding = EdgeInsets{0, 10, 0, 10};
    themeToggleBtn.onClick = [theme]() {
        *theme = static_cast<Theme>((static_cast<int>(*theme) + 1) % kThemeCount);
    };
    themeToggleBtn.addChild(themeToggleLabel);
    toolbar.addChild(themeToggleBtn);


    View workspace;
    workspace.style.direction = FlexDirection::Row;
    workspace.style.width = Size::full();
    workspace.style.flexGrow = 1;

    // ---- Activity Bar ----
    View activityBar;
    activityBar.style.width = Size::pixel(activityBarWidth);
    activityBar.style.height = Size::full();
    activityBar.style.flexGrow = 0;
    activityBar.style.flexShrink = 0;
    activityBar.style.direction = FlexDirection::Column;
    activityBar.style.alignItems = Align::Center;
    activityBar.style.backgroundColor = themeColor(&ThemePalette::activityBarBg);
    activityBar.style.padding = EdgeInsets::all(6.0f);
    activityBar.style.gap = 12.0f;

    for (const auto &item : activities) {
        View button;
        button.style.width = Size::pixel(32);
        button.style.height = Size::pixel(32);
        button.style.borderRadius = 4.0f;
        button.style.direction = FlexDirection::Column;
        button.style.justifyContent = Justify::Center;
        button.style.alignItems = Align::Center;
        button.style.hoverColor = Color{70, 70, 75, 255};

        int id = item.id;
        button.style.backgroundColor = std::function<Color()>(
            [activeActivity, id, theme]() {
                const ThemePalette &p = paletteFor(*theme);
                return *activeActivity == id ? p.activityActiveBg : p.activityInactiveBg;
            });

        button.onClick = [activeActivity, id]() {
            *activeActivity = (*activeActivity == id) ? -1 : id;
        };

        Text icon;
        icon.label = item.icon;
        icon.fontSize = 16.0f;
        icon.fontWeight = FontWeight::SemiBold;
        icon.color = themeColor(&ThemePalette::activityIconColor);
        button.addChild(icon);

        activityBar.addChild(button);
    }

    // ---- Sidebar ----
    View sidebar;
    sidebar.style.height = Size::full();
    sidebar.style.flexGrow = 0;
    sidebar.style.flexShrink = 0;
    sidebar.style.direction = FlexDirection::Column;
    sidebar.style.backgroundColor = themeColor(&ThemePalette::sidebarBg);
    sidebar.style.padding = EdgeInsets::all(12.0f);
    sidebar.style.overflowX = Overflow::Hidden;

    sidebar.style.width = std::function<Size()>([activeActivity, sidebarWidth] {
        return Size::pixel(*activeActivity != -1 ? *sidebarWidth : 0.0f);
    });

    Text sidebarTitle;
    sidebarTitle.label = std::function<std::string()>(
        [activeActivity, activities]() -> std::string {
            for (const auto &a : activities)
                if (a.id == *activeActivity)
                    return a.title;
            return "";
        });
    sidebarTitle.fontSize = 12.0f;
    sidebarTitle.fontWeight = FontWeight::SemiBold;
    sidebarTitle.color = themeColor(&ThemePalette::sidebarTitleColor);
    sidebar.addChild(sidebarTitle);


    auto openTab = [tabs, activeTab, nextTabId](const std::string &title) {

        for (const auto &t : *tabs) {
            if (t.title == title) {
                *activeTab = t.id;
                return;
            }
        }
        int id = (*nextTabId)++;
        tabs->push_back({id, title});
        *activeTab = id;
    };

    auto makeFileRow = [openTab, themeColor](const std::string &name) {
        View row;
        row.style.height = Size::pixel(24);
        row.style.width = Size::full();
        row.style.alignItems = Align::Center;
        row.style.hoverColor = Color{45, 45, 48, 255};
        row.onClick = [openTab, name]() { openTab(name); };
        Text label;
        label.label = name;
        label.fontSize = 13.0f;
        label.color = themeColor(&ThemePalette::fileRowTextColor);
        row.addChild(label);
        return row;
    };

    View content;
    content.style.direction = FlexDirection::Column;
    content.style.width = Size::full();
    content.style.margin = EdgeInsets{10, 0, 0, 0};

    for (const auto &item : activities) {
        View pane;
        pane.style.direction = FlexDirection::Column;
        pane.style.width = Size::full();
        int id = item.id;
        pane.style.display = std::function<Display()>(
            [activeActivity, id]() {
                return *activeActivity == id ? Display::Flex : Display::None;
            });

        if (item.id == 0) {
            pane.addChild(makeFileRow("main.cpp"));
            pane.addChild(makeFileRow("liteui.hpp"));
            pane.addChild(makeFileRow("CMakeLists.txt"));
        } else {
            Text placeholder;
            placeholder.label = item.title + " view goes here";
            placeholder.fontSize = 13.0f;
            placeholder.color = themeColor(&ThemePalette::placeholderColor);
            pane.addChild(placeholder);
        }

        content.addChild(pane);
    }
    sidebar.addChild(content);

    // ---- Vertical divider — sidebar <-> right column ----
    View vDivider;
    vDivider.style.height = Size::full();
    vDivider.style.flexGrow = 0;
    vDivider.style.flexShrink = 0;
    vDivider.style.backgroundColor = themeColor(&ThemePalette::dividerBg);
    vDivider.style.hoverColor = Color{80, 80, 220, 255};

    vDivider.style.width = std::function<Size()>([activeActivity, dividerWidth] {
        return Size::pixel(*activeActivity != -1 ? dividerWidth : 0.0f);
    });

    auto updateSidebarWidth = [sidebarWidth, dividerWidth,
                               minSidebarWidth, maxSidebarWidth](float localX) {
        float next = *sidebarWidth + localX - dividerWidth / 2.0f;
        *sidebarWidth = std::clamp(next, minSidebarWidth, maxSidebarWidth);
    };
    vDivider.onPressAt = [updateSidebarWidth](float localX, float) {
        updateSidebarWidth(localX);
    };
    vDivider.onDragTo = [updateSidebarWidth](float localX, float) {
        updateSidebarWidth(localX);
    };

    // ---------------------------------------------------------------
    // Right column — tab bar, editor, horizontal divider, terminal.
    // ---------------------------------------------------------------
    View rightColumn;
    rightColumn.style.direction = FlexDirection::Column;
    rightColumn.style.flexGrow = 1;
    rightColumn.style.height = Size::full();

    // ---- Tab bar container: the scrollable/keyed tab strip, plus a
    // fixed "+" button after it. ----
    View tabBarContainer;
    tabBarContainer.style.direction = FlexDirection::Row;
    tabBarContainer.style.width = Size::full();
    tabBarContainer.style.height = Size::pixel(tabBarHeight);
    tabBarContainer.style.flexGrow = 0;
    tabBarContainer.style.flexShrink = 0;
    tabBarContainer.style.backgroundColor = themeColor(&ThemePalette::tabBarBg);
    tabBarContainer.style.alignItems = Align::Stretch;


    View tabStrip;
    tabStrip.style.direction = FlexDirection::Row;
    tabStrip.style.height = Size::full();
    tabStrip.style.overflowX = Overflow::Auto; // scroll if too many tabs

    tabStrip.keysSource = [tabs]() {
        std::vector<std::string> keys;
        keys.reserve(tabs->size());
        for (const auto &t : *tabs)
            keys.push_back(std::to_string(t.id));
        return keys;
    };

    tabStrip.itemBuilder = [tabs, activeTab, theme, themeColor](const std::string &key) -> View {
        int id = std::stoi(key);

        std::string title;
        for (const auto &t : *tabs)
            if (t.id == id) { title = t.title; break; }

        View tab;
        tab.style.direction = FlexDirection::Row;
        tab.style.height = Size::full();
        tab.style.alignItems = Align::Center;
        tab.style.padding = EdgeInsets{0, 8, 0, 12};
        tab.style.gap = 8.0f;
        tab.style.borderWidth = 0.0f;
        tab.style.hoverColor = Color{45, 45, 48, 255};


        tab.style.backgroundColor = std::function<Color()>(
            [activeTab, id, theme]() {
                const ThemePalette &p = paletteFor(*theme);
                return *activeTab == id ? p.tabActiveBg : p.tabInactiveBg;
            });


        tab.onClick = [activeTab, id]() { *activeTab = id; };

        Text label;
        label.label = title;
        label.fontSize = 13.0f;
        label.color = std::function<Color()>([activeTab, id, theme]() {
            const ThemePalette &p = paletteFor(*theme);
            return *activeTab == id ? p.tabActiveText : p.tabInactiveText;
        });
        tab.addChild(label);


        View closeBtn;
        closeBtn.style.width = Size::pixel(16);
        closeBtn.style.height = Size::pixel(16);
        closeBtn.style.borderRadius = 3.0f;
        closeBtn.style.direction = FlexDirection::Column;
        closeBtn.style.justifyContent = Justify::Center;
        closeBtn.style.alignItems = Align::Center;
        closeBtn.style.hoverColor = Color{80, 80, 85, 255};
        closeBtn.onClick = [tabs, activeTab, id]() {
            auto it = std::find_if(tabs->begin(), tabs->end(),
                                   [id](const Tab &t) { return t.id == id; });
            if (it == tabs->end())
                return;
            size_t idx = static_cast<size_t>(it - tabs->begin());
            tabs->erase(it);

            if (*activeTab != id)
                return; // closed a background tab; active selection unaffected

            if (tabs->empty()) {
                *activeTab = -1;
            } else {
                // Activate the tab that's now at the same index, or the
                // new last tab if we closed the last one.
                size_t newIdx = std::min(idx, tabs->size() - 1);
                *activeTab = (*tabs)[newIdx].id;
            }
        };

        Text closeGlyph;
        closeGlyph.label = std::string("\u00D7"); // ×
        closeGlyph.fontSize = 13.0f;
        closeGlyph.color = themeColor(&ThemePalette::glyphColor);
        closeBtn.addChild(closeGlyph);

        tab.addChild(closeBtn);
        return tab;
    };

    // "+" button: opens a fresh, uniquely-named tab.
    View newTabBtn;
    newTabBtn.style.width = Size::pixel(tabBarHeight);
    newTabBtn.style.height = Size::full();
    newTabBtn.style.flexGrow = 0;
    newTabBtn.style.flexShrink = 0;
    newTabBtn.style.direction = FlexDirection::Column;
    newTabBtn.style.justifyContent = Justify::Center;
    newTabBtn.style.alignItems = Align::Center;
    newTabBtn.style.hoverColor = Color{45, 45, 48, 255};
    newTabBtn.onClick = [tabs, activeTab, nextTabId]() {
        int id = (*nextTabId)++;
        tabs->push_back({id, "untitled-" + std::to_string(id)});
        *activeTab = id;
    };

    Text newTabGlyph;
    newTabGlyph.label = std::string("+");
    newTabGlyph.fontSize = 16.0f;
    newTabGlyph.color = themeColor(&ThemePalette::glyphColor);
    newTabBtn.addChild(newTabGlyph);

    tabBarContainer.addChild(tabStrip);
    tabBarContainer.addChild(newTabBtn);

    // ---- Editor content — shows the active tab's title, or a
    // placeholder when no tabs are open. ----
    View editorArea;
    editorArea.style.flexGrow = 1;
    editorArea.style.width = Size::full();
    editorArea.style.backgroundColor = themeColor(&ThemePalette::editorBg);
    editorArea.style.direction = FlexDirection::Column;
    editorArea.style.justifyContent = Justify::Center;
    editorArea.style.alignItems = Align::Center;

    Text editorLabel;
    editorLabel.label = std::function<std::string()>(
        [tabs, activeTab]() -> std::string {
            for (const auto &t : *tabs)
                if (t.id == *activeTab)
                    return "Editing: " + t.title;
            return "No tabs open";
        });
    editorLabel.fontSize = 20.0f;
    editorLabel.color = themeColor(&ThemePalette::editorLabelColor);
    editorArea.addChild(editorLabel);

    // ---- Horizontal divider — editor <-> terminal ----
    View hDivider;
    hDivider.style.width = Size::full();
    hDivider.style.flexGrow = 0;
    hDivider.style.flexShrink = 0;
    hDivider.style.height = Size::pixel(hDividerHeight);
    hDivider.style.backgroundColor = themeColor(&ThemePalette::dividerBg);
    hDivider.style.hoverColor = Color{80, 80, 220, 255};

    auto updateTerminalHeight = [terminalHeight, hDividerHeight,
                                 minTerminalHeight, maxTerminalHeight](float localY) {
        float next = *terminalHeight - (localY - hDividerHeight / 2.0f);
        *terminalHeight = std::clamp(next, minTerminalHeight, maxTerminalHeight);
    };
    hDivider.onPressAt = [updateTerminalHeight](float, float localY) {
        updateTerminalHeight(localY);
    };
    hDivider.onDragTo = [updateTerminalHeight](float, float localY) {
        updateTerminalHeight(localY);
    };

    // ---- Terminal panel ----
    View terminal;
    terminal.style.width = Size::full();
    terminal.style.flexGrow = 0;
    terminal.style.flexShrink = 0;
    terminal.style.direction = FlexDirection::Column;
    terminal.style.backgroundColor = themeColor(&ThemePalette::terminalBg);
    terminal.style.padding = EdgeInsets::all(10.0f);
    terminal.style.overflowY = Overflow::Hidden;

    terminal.style.height = std::function<Size()>([terminalHeight] {
        return Size::pixel(*terminalHeight);
    });

    Text terminalTitle;
    terminalTitle.label = std::string("TERMINAL");
    terminalTitle.fontSize = 12.0f;
    terminalTitle.fontWeight = FontWeight::SemiBold;
    terminalTitle.color = themeColor(&ThemePalette::terminalTitleColor);
    terminal.addChild(terminalTitle);

    Text terminalPrompt;
    terminalPrompt.label = std::string("$ ");
    terminalPrompt.fontSize = 13.0f;
    terminalPrompt.fontFamily = "monospace";
    terminalPrompt.color = themeColor(&ThemePalette::terminalPromptColor);
    terminalPrompt.style.margin = EdgeInsets{8, 0, 0, 0};
    terminal.addChild(terminalPrompt);

    rightColumn.addChild(tabBarContainer);
    rightColumn.addChild(editorArea);
    rightColumn.addChild(hDivider);
    rightColumn.addChild(terminal);

    workspace.addChild(activityBar);
    workspace.addChild(sidebar);
    workspace.addChild(vDivider);
    workspace.addChild(rightColumn);

    // ---------------------------------------------------------------
    // Bottom Status Bar
    // ---------------------------------------------------------------
    View statusBar;
    statusBar.style.width = Size::full();
    statusBar.style.height = Size::pixel(statusBarHeight);
    statusBar.style.flexGrow = 0;
    statusBar.style.flexShrink = 0;
    statusBar.style.backgroundColor = Color{0, 122, 204, 255};
    statusBar.style.direction = FlexDirection::Row;
    statusBar.style.alignItems = Align::Center;
    statusBar.style.justifyContent = Justify::SpaceBetween;
    statusBar.style.padding = EdgeInsets{0, 12, 0, 12};

    Text statusLeft;
    statusLeft.label = std::string("main");
    statusLeft.fontSize = 12.0f;
    statusLeft.color = Color{255, 255, 255, 255};
    statusBar.addChild(statusLeft);

    Text statusRight;
    statusRight.label = std::string("UTF-8  Ln 1, Col 1");
    statusRight.fontSize = 12.0f;
    statusRight.color = Color{255, 255, 255, 255};
    statusBar.addChild(statusRight);

    root.addChild(toolbar);
    root.addChild(workspace);
    root.addChild(statusBar);

    ui.setRoot(std::move(root));
    ui.run();
    return 0;
}