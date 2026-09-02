// src/main.cpp
#include "liteui.hpp"
#include <iostream>

int main() {
    LiteUI ui(800, 600, "onClick demo");

    View root;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.style.direction = FlexDirection::Column;
    root.style.justifyContent = Justify::Center;
    root.style.alignItems = Align::Center;
    root.style.gap = 16;
    root.style.backgroundColor = Color{240, 240, 240};

    View button;
    button.style.width = Size::pixel(200);
    button.style.height = Size::pixel(60);
    button.style.backgroundColor = Color{70, 130, 200};
    button.style.borderRadius = 8;
    button.onClick = [] {
        std::cout << "Button clicked!" << std::endl;
    };

    View button2;
    button2.style.width = Size::pixel(200);
    button2.style.height = Size::pixel(60);
    button2.style.backgroundColor = Color{200, 90, 90};
    button2.style.borderRadius = 8;
    button2.onClick = [] {
        std::cout << "Second button clicked!" << std::endl;
    };

    root.addChild(button);
    root.addChild(button2);

    ui.setRoot(std::move(root));
    ui.run();
}