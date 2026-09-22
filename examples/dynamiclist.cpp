// src/main.cpp
#include "liteui.hpp"
#include <string>
#include <vector>

// One entry = a stable id (for keying) + the text to display.
// The id never changes for a given row, even as the vector shifts around it.
struct Item {
  std::string id;
  std::string text;
};

struct ItemStore {
  std::vector<Item> items;
  int nextId = 0;

  Item *find(const std::string &id) {
    for (auto &it : items)
      if (it.id == id)
        return &it;
    return nullptr; // gone — reconciled away already or about to be
  }
  std::vector<std::string> keys() const {
    std::vector<std::string> k;
    k.reserve(items.size());
    for (const auto &it : items)
      k.push_back(it.id);
    return k;
  }
  void add() {
    std::string id = "item-" + std::to_string(nextId);
    items.push_back({id, "Item " + std::to_string(nextId)});
    ++nextId;
  }
  void remove(const std::string &id) {
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const Item &it) { return it.id == id; }),
                items.end());
  }
};

// Closes over `id`, not an index/pointer — this row is safe to keep around
// across reconciles even as sibling rows are added/removed/reordered.
static View buildRow(ItemStore &store, const std::string &id) {
  View row;
  row.style.width = Size::full();
  row.style.direction = FlexDirection::Row;
  row.style.alignItems = Align::Center;
  row.style.gap = 10;
  row.style.padding = EdgeInsets{8, 12, 8, 12};
  row.style.backgroundColor = Color{255, 255, 255};
  row.style.borderWidth = 1.0f;
  row.style.borderColor = Color{225, 225, 230};
  row.style.borderRadius = 6.0f;

  Text label;
  label.label = [&store, id]() -> std::string {
    const Item *it = store.find(id);
    return it ? it->text : std::string{};
  };
  label.fontSize = 16.0f;
  label.style.flexGrow = 1.0f;
  row.addChild(std::move(label));

  View del;
  del.style.width = Size::pixel(24);
  del.style.height = Size::pixel(24);
  del.style.borderRadius = 12.0f;
  del.style.justifyContent = Justify::Center;
  del.style.alignItems = Align::Center;
  del.style.backgroundColor = Color{245, 245, 247};
  del.style.hoverColor = Color{240, 120, 110};
  del.onClick = [&store, id] { store.remove(id); };
  Text cross;
  cross.label = std::string{"x"};
  cross.fontSize = 14.0f;
  cross.color = Color{90, 90, 100};
  del.addChild(std::move(cross));
  row.addChild(std::move(del));

  return row;
}

int main() {
  // Declared before `app` so the tree's captures (which reach into
  // `store`) are destroyed before `store` itself goes away.
  ItemStore store;
  store.add();
  store.add();
  store.add();

  View root;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.direction = FlexDirection::Column;
  root.style.padding = EdgeInsets::all(16);
  root.style.gap = 12;
  root.style.backgroundColor = Color{245, 245, 247};

  View list;
  list.style.width = Size::full();
  list.style.flexGrow = 1.0f;
  list.style.direction = FlexDirection::Column;
  list.style.gap = 8;
  list.style.overflowY = Overflow::Auto;
  list.style.backgroundColor = Color{245, 245, 247};
  // This pair is the whole mechanism: keysSource says what should exist,
  // itemBuilder builds a row for a key the reconciler hasn't seen before.
  list.keysSource = [&store] { return store.keys(); };
  list.itemBuilder = [&store](const std::string &id) {
    return buildRow(store, id);
  };
  root.addChild(std::move(list));

  View addBtn;
  addBtn.style.width = Size::full();
  addBtn.style.padding = EdgeInsets::all(12);
  addBtn.style.justifyContent = Justify::Center;
  addBtn.style.borderRadius = 6.0f;
  addBtn.style.backgroundColor = Color{52, 140, 255};
  addBtn.style.hoverColor = Color{40, 120, 230};
  addBtn.style.flexShrink = 0.0f;
  addBtn.onClick = [&store] { store.add(); };
  Text addLabel;
  addLabel.label = std::string{"+ Add item"};
  addLabel.fontSize = 16.0f;
  addLabel.color = Color{255, 255, 255};
  addBtn.addChild(std::move(addLabel));
  root.addChild(std::move(addBtn));

  LiteUI app("Items",360, 480);
  app.setRoot(std::move(root));
  app.run();
}