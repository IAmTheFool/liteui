#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
#include "topbar.hpp"

int main()
{

    Text label;
    label.label = std::string("Hello World");
    label.fontSize = 20;

    // Content area under the top bar: fills the rest of the window.
    View content;
    content.style.direction = FlexDirection::Column;
    content.style.alignItems = Align::Center;
    content.style.justifyContent = Justify::Center;
    content.style.gap = 16;
    content.style.width = Size::full();
    content.style.flexGrow = 1;
    content.addChild(label);

    View root;
    root.style.direction = FlexDirection::Column;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.addChild(makeTopBar("Hello World")); // window chrome is ours to draw
    root.addChild(content);

    LiteUI ui("Hello World", 400, 200);
    ui.setRoot(root);
    ui.run(); // blocks, pumps the platform event loop
    return 0;
}