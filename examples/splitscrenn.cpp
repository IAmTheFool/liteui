// src/main.cpp
#include "liteui.hpp"
#include <memory>
#include <algorithm>

int main() {
    LiteUI ui(900, 600, "Resizable Split Screen");

    // Shared state: current pixel width of the left pane. Held in a
    // shared_ptr because multiple lambdas below (left pane's width
    // getter, divider's press/drag handlers) need to read and mutate
    // the same value across separate calls.
    auto splitX = std::make_shared<float>(300.0f);
    const float dividerWidth = 6.0f;
    const float minPaneWidth = 100.0f;

    View root;
    root.style.direction = FlexDirection::Row;
    root.style.width = Size::full();
    root.style.height = Size::full();

    // ---- Left pane ----
    // width is a callback (Dynamic<Size> holding std::function), re-polled
    // by checkForUpdates() after every dispatched event, so it tracks
    // *splitX live as the divider is dragged.
    View left;
    left.style.width = std::function<Size()>([splitX] {
        return Size::pixel(*splitX);
    });
    left.style.height = Size::full();
    left.style.minWidth = minPaneWidth;
    left.style.backgroundColor = Color{60, 120, 200, 255};
    left.style.direction = FlexDirection::Column;
    left.style.justifyContent = Justify::Center;
    left.style.alignItems = Align::Center;

    Text leftLabel;
    leftLabel.label = std::string("Left Panel");
    leftLabel.fontSize = 24.0f;
    leftLabel.fontWeight = FontWeight::SemiBold;
    leftLabel.color = Color{255, 255, 255, 255};
    left.addChild(leftLabel);

    // ---- Divider ----
    View divider;
    divider.style.width = Size::pixel(dividerWidth);
    divider.style.height = Size::full();
    divider.style.flexGrow = 0;
    divider.style.flexShrink = 0;
    divider.style.backgroundColor = Color{180, 180, 180, 255};
    divider.style.hoverColor = Color{120, 120, 120, 255};

    // Shared update logic used by both handlers below, so they can't
    // drift out of sync.
    auto updateSplitFromLocalX = [splitX, dividerWidth, minPaneWidth](float localX) {
        float next = *splitX + localX - dividerWidth / 2.0f;
        *splitX = std::max(minPaneWidth, next);
    };

    // IMPORTANT: onPressAt is required, not optional. hitTest() (called
    // by beginPress) only recognizes a view as "hittable" if it has
    // onClick or onPressAt — onDragTo alone is never checked there. Without
    // onPressAt, hitTest never resolves to this divider, dragView_ never
    // gets armed, and onDragTo below would silently never fire.
    divider.onPressAt = [updateSplitFromLocalX](float localX, float) {
        updateSplitFromLocalX(localX);
    };
    divider.onDragTo = [updateSplitFromLocalX](float localX, float) {
        updateSplitFromLocalX(localX);
    };

    // ---- Right pane ----
    // flexGrow = 1 with left/divider fixed-size means right claims all
    // leftover horizontal space automatically — no manual width math.
    View right;
    right.style.flexGrow = 1;
    right.style.height = Size::full();
    right.style.minWidth = minPaneWidth;
    right.style.backgroundColor = Color{60, 180, 120, 255};
    right.style.direction = FlexDirection::Column;
    right.style.justifyContent = Justify::Center;
    right.style.alignItems = Align::Center;

    Text rightLabel;
    rightLabel.label = std::string("Right Panel");
    rightLabel.fontSize = 24.0f;
    rightLabel.fontWeight = FontWeight::SemiBold;
    rightLabel.color = Color{255, 255, 255, 255};
    right.addChild(rightLabel);

    root.addChild(left);
    root.addChild(divider);
    root.addChild(right);

    ui.setRoot(std::move(root));
    ui.run();
    return 0;
}