# Getting started

This tutorial takes you from an empty folder to a small working LiteUI app, one step at a time:

1. [Set up a project](#1-set-up-a-project)
2. [Hello, window](#2-hello-window)
3. [A counter: state, buttons and `Dynamic<>`](#3-a-counter-state-buttons-and-dynamic)
4. [A todo list: text input and keyed lists](#4-a-todo-list-text-input-and-keyed-lists)
5. [A custom-drawn widget with `Canvas`](#5-a-custom-drawn-widget-with-canvas)
6. [Background work with `wake()`](#6-background-work-with-wake)
7. [Common stumbling blocks](#common-stumbling-blocks)
8. [Where to go next](#where-to-go-next)

You need a C++23 compiler, CMake 3.21+ and Ninja. On Windows that is Visual Studio's MSVC; on Linux it is GCC plus the Wayland, Cairo and Pango development packages. [BUILDING.md](BUILDING.md#requirements) has the exact list and the install command.

All names in the examples (`View`, `Text`, `Size`, ...) are in the global namespace, so there is nothing to `using`.

---

## 1. Set up a project

Put LiteUI somewhere in your project (a git submodule, a copy, or `FetchContent`). The layout used here:

```
myapp/
  CMakeLists.txt
  main.cpp
  third_party/liteui/        <- the LiteUI repository
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(myapp LANGUAGES CXX)

add_subdirectory(third_party/liteui)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE liteui::liteui)

if(MSVC)
    target_compile_options(myapp PRIVATE /utf-8)
endif()
```

Linking `liteui::liteui` is all that is needed. It supplies the include path, C++23, the platform libraries, and (on Linux) the bundled Wayland protocol bindings. Using LiteUI as a subproject builds neither its tests nor its examples.

Build and run (on Windows, from an **x64 Native Tools Command Prompt for VS**, so `cl.exe` and Ninja are on `PATH`):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/myapp            # myapp.exe on Windows
```

On Windows an ordinary `int main` program also opens a console window. That is fine for development and useful for printing diagnostics.

> **Rule you must remember.** In **exactly one** `.cpp` file of your program, `#define LITEUI_IMPLEMENTATION` before including `liteui.hpp`. That file compiles the platform backend. A program with a single `main.cpp` simply does it there. Defining it in two files gives duplicate-symbol errors; defining it in none gives undefined references.

---

## 2. Hello, window

`main.cpp`:

```cpp
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
    ui.run();              // blocks until the window closes
}
```

What each part does:

- **`Text`** is a label. Its `label` is a `Dynamic<std::string>`: a plain value now, a callback in the next step.
- **`View`** is the container. `style` follows the flexbox model: `direction` picks the main axis, `justifyContent` aligns along it, `alignItems` across it.
- **`Size::full()` on the root** makes it fill the window. The default is `Size::fit()`, which shrinks to the content and does *not* follow the window, so the root almost always wants `full()`.
- **`addChild` copies.** The `View` you add is copied into the root. Finish building a child *before* you add it.
- **`LiteUI`** is the window. `setRoot` hands over the tree (also copied), and `run()` pumps the event loop.

Run it and you should see a centered "Hello World".

---

## 3. A counter: state, buttons and `Dynamic<>`

Here is the idea that makes LiteUI feel small: **you do not push updates to the screen.** Fields that can change take a *callback* instead of a value. After every event (a click, a key, a timer tick) LiteUI calls those callbacks, notices what changed, and relays out and repaints for you.

```cpp
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <functional>
#include <string>

static View makeButton(const std::string &caption, std::function<void()> onClick)
{
    Text t;
    t.label = caption;
    t.color = Color{255, 255, 255};

    View b;
    b.style.padding = EdgeInsets{8, 16, 8, 16};      // top, right, bottom, left
    b.style.borderRadius = 6.0f;
    b.style.backgroundColor = Color{50, 120, 220};
    b.style.hoverColor = Color{70, 140, 240};
    b.onClick = std::move(onClick);
    b.addChild(t);
    return b;
}

int main()
{
    int count = 0;

    Text label;
    label.fontSize = 32;
    label.label = [&] { return "Count: " + std::to_string(count); };   // a callback

    View buttons;
    buttons.style.gap = 8;
    buttons.addChild(makeButton("-1",    [&] { --count; }));
    buttons.addChild(makeButton("+1",    [&] { ++count; }));
    buttons.addChild(makeButton("Reset", [&] { count = 0; }));

    View root;
    root.style.direction = FlexDirection::Column;
    root.style.alignItems = Align::Center;
    root.style.justifyContent = Justify::Center;
    root.style.gap = 20;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.addChild(label);
    root.addChild(buttons);

    LiteUI ui("Counter", 420, 240);
    ui.setRoot(root);
    ui.run();
}
```

Things to notice:

- **`count` lives in `main`, and the callbacks capture it by reference.** Your state stays in your variables. The tree never owns it. It just has to outlive `ui.run()`, which it does.
- **A button is just a `View` with `onClick`.** Clicking the "+1" text works because clicks *bubble* from the text up to the nearest ancestor that has a handler.
- **`hoverColor`** replaces the background while the pointer is over the view.
- **Use float literals for `Dynamic<float>` fields.** `borderRadius = 6.0f` compiles, but `borderRadius = 6` does not, because an `int` assigned to a `std::variant<float, ...>` is a narrowing conversion.
- **Callbacks should be cheap.** They run after *every* event. Compute from your state, return a value, do nothing else.

---

## 4. A todo list: text input and keyed lists

This step adds a `TextInput` and a **keyed list**: a container whose children come from a list of keys, so adding or removing one item builds or frees one row instead of rebuilding everything.

```cpp
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

static View makeButton(const std::string &caption, std::function<void()> onClick)
{
    Text t;
    t.label = caption;
    t.color = Color{255, 255, 255};

    View b;
    b.style.padding = EdgeInsets{8, 16, 8, 16};
    b.style.borderRadius = 6.0f;
    b.style.backgroundColor = Color{50, 120, 220};
    b.style.hoverColor = Color{70, 140, 240};
    b.onClick = std::move(onClick);
    b.addChild(t);
    return b;
}

int main()
{
    std::vector<std::string> ids;                 // the keys, in display order
    std::map<std::string, std::string> titles;    // key -> todo text
    int nextId = 1;

    // --- the input field ---------------------------------------------------
    TextInput input;
    input.style.height = Size::pixel(36);
    input.style.flexGrow = 1;                     // no intrinsic width: take what is left
    input.style.borderWidth = 1.0f;
    input.style.borderRadius = 6.0f;
    input.placeholder = "What needs doing?";
    auto inputState = input.state;                // the live state, shared with the node

    auto addTodo = [&] {
        if (inputState->text.empty())
            return;
        std::string id = std::to_string(nextId++);
        titles[id] = inputState->text;
        ids.push_back(id);
        inputState->text.clear();                 // reset the field...
        inputState->cursor = 0;
        inputState->dirty = true;                 // ...and tell it to repaint
        inputState->requestFocus = true;          // keep typing after the list changes
    };
    input.onSubmit = [&](const std::string &) { addTodo(); };    // Enter

    View bar;
    bar.style.gap = 8;
    bar.addChild(input);
    bar.addChild(makeButton("Add", addTodo));

    // --- the keyed list ------------------------------------------------------
    View list;
    list.style.direction = FlexDirection::Column;
    list.style.gap = 6;
    list.style.flexGrow = 1;                      // fill the remaining height...
    list.style.overflowY = Overflow::Auto;        // ...and scroll when it is not enough
    list.keysSource = [&] { return ids; };
    list.itemBuilder = [&](const std::string &id) {
        Text t;
        t.label = [&, id] { return titles.at(id); };
        t.style.flexGrow = 1;

        View row;
        row.style.alignItems = Align::Center;
        row.style.gap = 8;
        row.style.padding = EdgeInsets::all(8);
        row.style.borderRadius = 6.0f;
        row.style.backgroundColor = Color{255, 255, 255};
        row.addChild(t);
        row.addChild(makeButton("x", [&, id] {
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            titles.erase(id);
        }));
        return row;
    };

    // --- assemble ------------------------------------------------------------
    View root;
    root.style.direction = FlexDirection::Column;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.style.padding = EdgeInsets::all(16);
    root.style.gap = 12;
    root.addChild(bar);
    root.addChild(list);

    LiteUI ui("Todo", 480, 560);
    ui.setWindowBackground(Color{240, 241, 245});
    ui.setRoot(root);
    ui.run();
}
```

What is new here:

- **`TextInput` and its `state`.** The node you add is a copy, but `input.state` is a shared pointer. Keep a copy and you can read `state->text` at any time, or reset it as above (set `text`, `cursor`, then `dirty = true`). `onSubmit` fires on Enter.
- **No intrinsic size.** A `TextInput` (like a `Canvas`) has nothing to measure, so its `Fit` width is zero. Here `flexGrow = 1` hands it all the space the "Add" button leaves; elsewhere give it a `Size::pixel(...)`.
- **A keyed list** is `keysSource` plus `itemBuilder`. After each event LiteUI calls `keysSource`; when the vector changes it keeps rows whose key survived, calls `itemBuilder` **only for new keys**, and frees removed rows. The row text reads `titles.at(id)` through a callback so it stays correct even though the row is built once.
- **Scrolling** needs only `overflowY = Overflow::Auto` plus a height that is smaller than the content. Here `flexGrow = 1` lets the list take the rest of the window, and it shrinks and scrolls when the rows do not fit.
- **`requestFocus`.** Changing a keyed list's keys makes the window drop keyboard focus (see [ARCHITECTURE.md](ARCHITECTURE.md#sharp-edges)). Setting `requestFocus` in the same update puts focus back in the field so you can keep typing.

---

## 5. A custom-drawn widget with `Canvas`

Anything the built-in nodes cannot draw, a `Canvas` can. This adds a ring gauge to the counter from step 3 that fills as `count` goes from 0 to 10.

Add `#include <algorithm>` and `#include <utility>` to the top of the counter program, then, before the root is assembled:

```cpp
Canvas gauge;
gauge.style.width = Size::pixel(120);       // a canvas has no intrinsic size,
gauge.style.height = Size::pixel(120);      // so always give it one

int lastDrawn = -1;
gauge.canvasDirtySource = [&] { return std::exchange(lastDrawn, count) != count; };

gauge.onPaint = [&](CanvasContext &g) {
    const float kTau = 6.2831853f, kTop = -1.5707963f;
    float cx = g.width() / 2, cy = g.height() / 2;
    float r = std::min(cx, cy) - 8;
    float frac = std::clamp(count / 10.0f, 0.0f, 1.0f);

    g.setLineWidth(10);
    g.setLineCap(LineCap::Round);

    g.setStrokeColor(Color{210, 214, 224});               // the track
    g.beginPath();  g.arc(cx, cy, r, 0, kTau);  g.stroke();

    if (frac > 0) {                                       // the progress arc
        g.setStrokeColor(Color{50, 120, 220});
        g.beginPath();  g.arc(cx, cy, r, kTop, kTop + kTau * frac);  g.stroke();
    }
};

root.addChild(gauge);
```

The key idea: **`onPaint` is not a per-frame loop.** LiteUI keeps the bitmap and calls `onPaint` only when the canvas is first laid out, is resized, or `canvasDirtySource` returns `true`. Here the dirty source simply compares the value last drawn with the current `count`, the same "poll your own state" idea as `Dynamic<>`. It is polled after every event, so clicking "+1" redraws the gauge and nothing else does.

Also always call `setLineWidth` yourself: the default stroke width differs between platforms. The full drawing API is in [CANVAS.md](CANVAS.md).

---

## 6. Background work with `wake()`

All of LiteUI belongs to the UI thread, with **one** exception: `LiteUI::wake()` may be called from any thread. A worker updates shared data and calls `wake()`; LiteUI then polls your callbacks and repaints on the UI thread.

```cpp
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// ...inside main, after building the rest of the UI:

std::atomic<int> progress{0};
std::atomic<bool> quit{false};

Text status;
status.label = [&] { return "Downloading... " + std::to_string(progress.load()) + "%"; };
root.addChild(status);

LiteUI ui("Worker", 420, 240);
ui.setRoot(root);

std::thread worker([&] {
    for (int i = 1; i <= 100 && !quit; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        progress = i;
        ui.wake();                    // the only thread-safe LiteUI call
    }
});

ui.run();                             // blocks on the UI thread
quit = true;                          // stop the worker if the window closed early
worker.join();                        // join before `ui` is destroyed
```

Rules of thumb:

- Share data with an `std::atomic` or a mutex-protected structure, and read it from your callbacks.
- Do not call any other LiteUI function from the worker thread.
- Always join threads that call `wake()` before the `LiteUI` goes out of scope.

---

## Common stumbling blocks

| Symptom | Cause and fix |
|---|---|
| Linker errors about `LiteUI::...` or `View::...` | No `.cpp` defines `LITEUI_IMPLEMENTATION` before including the header. |
| "multiple definition" errors | `LITEUI_IMPLEMENTATION` is defined in more than one `.cpp`. Keep it in exactly one. |
| The window is empty, or the UI is a small box in a corner | The root is `Fit` by default. Give it `Size::full()` for width and height. |
| Compile error assigning `8` to a style field | `Dynamic<float>` fields reject `int` (narrowing). Write `8.0f`. |
| A `Canvas` or `TextInput` is invisible | It has no intrinsic size. Give it a `Size::pixel`, `Size::full()` or `flexGrow`. |
| A label does not update | The field holds a value, not a callback. Assign a lambda (`label.label = [&]{ ... };`). |
| Changing `gap`, `direction` or `flexGrow` at runtime does nothing | Those are plain fields, read when the tree is built. Only `Dynamic<>` fields are re-polled. |
| A long paragraph overflows its box vertically | Give wrapping text a definite width (`Size::full()`), so its height accounts for wrapped lines. See [LAYOUT.md](LAYOUT.md#text). |
| A fixed-width box gets squeezed beside a large sibling | Set `flexShrink = 0` on the fixed box. See [LAYOUT.md](LAYOUT.md#shrink-is-not-weighted-by-size). |
| A crash or garbage after the window closes | A callback outlived the state it captured, or a thread was still calling `wake()`. Keep captured state alive for the life of the window and join your threads. |
| Non-ASCII characters look wrong in source on MSVC | Compile with `/utf-8`, as in the CMake file above. |

---

## Where to go next

- **[README](../README.md):** a tour of the concepts with short snippets.
- **[LAYOUT.md](LAYOUT.md):** how sizes, grow/shrink, alignment, absolute positioning and scrolling really work, plus layout recipes.
- **[CANVAS.md](CANVAS.md):** custom drawing, input handling, animation and images.
- **[API.md](API.md):** the complete reference.
- **[ARCHITECTURE.md](ARCHITECTURE.md):** how the library works inside.
- **[PLATFORM_NOTES.md](PLATFORM_NOTES.md):** Windows and Linux differences in one place.
- **`examples/`:** `hello`, `counter`, and the large `literoom` photo editor show real structure. The single-file demos in `examples/others/` cover a code editor, paint program, file explorer and more.
