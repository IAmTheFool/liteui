#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

int main()
{

    Text label;
    label.label = std::string("Hello World");
    label.fontSize = 20;

    View root;
    root.style.direction = FlexDirection::Column;
    root.style.alignItems = Align::Center;
    root.style.justifyContent = Justify::Center;
    root.style.gap = 16;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.addChild(label);

    LiteUI ui("Hello World", 400, 200);
    ui.setRoot(root);
    ui.run(); // blocks, pumps the platform event loop
    return 0;
}