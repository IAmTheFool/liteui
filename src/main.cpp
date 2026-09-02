// main.cpp
//
// Layout engine test bed. Exercises every feature currently supported:
//   - Size::fixed / percentage / fit / full
//   - FlexDirection::Row and Column
//   - Justify: Start, End, Center, SpaceBetween, SpaceAround, SpaceEvenly
//   - Align:   Start, End, Center, Stretch
//   - gap, margin, padding
//   - flexGrow / flexShrink
//   - borderRadius / borderWidth / borderColor
//
// NOTE: liteui.hpp has no text rendering yet, so sections are distinguished
// by color/position only. Read the comments to know what each block is
// demonstrating; there are no on-screen labels.

#include "liteui.hpp"

// Small helper to cut down on repetition when building leaf/demo boxes.
static View makeBox(Size w, Size h, Color bg, float radius = 0,
                    float borderWidth = 0, Color borderColor = {0, 0, 0}) {
  View v;
  v.style.width = w;
  v.style.height = h;
  v.style.backgroundColor = bg;
  v.style.borderRadius = radius;
  v.style.borderWidth = borderWidth;
  v.style.borderColor = borderColor;
  return v;
}

// A row/column "track" used to hold a set of demo children with a given
// direction, justify, and align — the thing actually under test in each row.
static View makeTrack(FlexDirection dir, Justify justify, Align align, Size w,
                      Size h, Color bg, float gap = 8,
                      EdgeInsets padding = EdgeInsets::all(8)) {
  View v;
  v.style.direction = dir;
  v.style.justifyContent = justify;
  v.style.alignItems = align;
  v.style.width = w;
  v.style.height = h;
  v.style.backgroundColor = bg;
  v.style.gap = gap;
  v.style.padding = padding;
  v.style.borderWidth = 1;
  v.style.borderColor = {0xCE, 0xD4, 0xDA};
  v.style.borderRadius = 4;
  return v;
}

int main() {
  LiteUI window(1000, 1150, "Layout Engine Test");

  const Color red{220, 53, 69};
  const Color green{40, 167, 69};
  const Color blue{0, 123, 255};
  const Color orange{253, 126, 20};
  const Color purple{111, 66, 193};
  const Color teal{32, 201, 151};
  const Color yellow{255, 193, 7};
  const Color pink{232, 62, 140};
  const Color lightGray{233, 236, 239};
  const Color midGray{173, 181, 189};
  const Color white{255, 255, 255};
  const Color dark{33, 37, 41};

  // ---------------- Root: Column, fills window ----------------
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.padding = EdgeInsets::all(24);
  root.style.gap = 12;
  root.style.backgroundColor = {0xF8, 0xF9, 0xFA};

  // ---------------- Section A: justifyContent, all 6 variants ----------------
  // Column of six Row tracks, each 3 small squares, one justify value each.
  {
    View section;
    section.style.direction = FlexDirection::Column;
    section.style.width = Size::full();
    section.style.height = Size::fit();
    section.style.gap = 4;
    section.style.padding = EdgeInsets::all(10);
    section.style.backgroundColor = white;
    section.style.borderWidth = 1;
    section.style.borderColor = midGray;
    section.style.borderRadius = 6;

    Justify variants[] = {Justify::Start,        Justify::End,
                          Justify::Center,       Justify::SpaceBetween,
                          Justify::SpaceAround,  Justify::SpaceEvenly};
    for (Justify j : variants) {
      View track = makeTrack(FlexDirection::Row, j, Align::Center,
                             Size::full(), Size::pixel(30), lightGray, 0,
                             EdgeInsets::all(4));
      track.addChild(makeBox(Size::pixel(24), Size::pixel(24), red));
      track.addChild(makeBox(Size::pixel(24), Size::pixel(24), green));
      track.addChild(makeBox(Size::pixel(24), Size::pixel(24), blue));
      section.addChild(track);
    }
    root.addChild(section);
  }

  // ---------------- Section B: alignItems, all 4 variants ----------------
  // Column of four Row tracks; children have deliberately different fixed
  // heights so Start/End/Center visibly differ. The Stretch row's children
  // use Size::fit() height so they actually stretch to fill the track.
  {
    View section;
    section.style.direction = FlexDirection::Column;
    section.style.width = Size::full();
    section.style.height = Size::fit();
    section.style.gap = 4;
    section.style.padding = EdgeInsets::all(10);
    section.style.backgroundColor = white;
    section.style.borderWidth = 1;
    section.style.borderColor = midGray;
    section.style.borderRadius = 6;

    Align variants[] = {Align::Start, Align::End, Align::Center, Align::Stretch};
    for (Align a : variants) {
      View track = makeTrack(FlexDirection::Row, Justify::Start, a,
                             Size::full(), Size::pixel(50), lightGray, 8,
                             EdgeInsets::all(6));
      if (a == Align::Stretch) {
        // Fit height on the cross axis lets Stretch actually take effect.
        track.addChild(makeBox(Size::pixel(30), Size::fit(), orange));
        track.addChild(makeBox(Size::pixel(30), Size::fit(), purple));
        track.addChild(makeBox(Size::pixel(30), Size::fit(), teal));
      } else {
        track.addChild(makeBox(Size::pixel(30), Size::pixel(15), orange));
        track.addChild(makeBox(Size::pixel(30), Size::pixel(30), purple));
        track.addChild(makeBox(Size::pixel(30), Size::pixel(38), teal));
      }
      section.addChild(track);
    }
    root.addChild(section);
  }

  // ---------------- Section C: Column direction + full centering ----------------
  // A fixed-size box laid out as a Column, with both axes centered — proof
  // that direction, justifyContent, and alignItems compose correctly when
  // the main axis is vertical instead of horizontal.
  {
    View box = makeTrack(FlexDirection::Column, Justify::Center, Align::Center,
                         Size::pixel(220), Size::pixel(150), dark, 8,
                         EdgeInsets::all(10));
    box.style.borderRadius = 10;
    box.addChild(makeBox(Size::pixel(60), Size::pixel(20), teal, 4));
    box.addChild(makeBox(Size::pixel(60), Size::pixel(20), purple, 4));
    box.addChild(makeBox(Size::pixel(60), Size::pixel(20), orange, 4));
    root.addChild(box);
  }

  // ---------------- Section D: flexGrow ----------------
  // Three children with zero base width but grow ratios 1:2:1 — the middle
  // box should end up roughly twice as wide as each outer box.
  {
    View track = makeTrack(FlexDirection::Row, Justify::Start, Align::Stretch,
                           Size::full(), Size::pixel(60), lightGray, 6,
                           EdgeInsets::all(6));
    View a = makeBox(Size::pixel(0), Size::fit(), red);
    a.style.flexGrow = 1;
    View b = makeBox(Size::pixel(0), Size::fit(), green);
    b.style.flexGrow = 2;
    View c = makeBox(Size::pixel(0), Size::fit(), blue);
    c.style.flexGrow = 1;
    track.addChild(a);
    track.addChild(b);
    track.addChild(c);
    root.addChild(track);
  }

  // ---------------- Section E: flexShrink ----------------
  // Three children whose combined fixed widths (1500px) far exceed the
  // available track width. Shrink factors are 1:1:3, so the third box
  // (yellow) should shrink noticeably more than the first two.
  {
    View track = makeTrack(FlexDirection::Row, Justify::Start, Align::Stretch,
                           Size::full(), Size::pixel(60), lightGray, 6,
                           EdgeInsets::all(6));
    View a = makeBox(Size::pixel(500), Size::fit(), pink);
    a.style.flexShrink = 1;
    View b = makeBox(Size::pixel(500), Size::fit(), teal);
    b.style.flexShrink = 1;
    View c = makeBox(Size::pixel(500), Size::fit(), yellow);
    c.style.flexShrink = 3;
    track.addChild(a);
    track.addChild(b);
    track.addChild(c);
    root.addChild(track);
  }

  // ---------------- Section F: borderRadius / borderWidth sampler ----------------
  // Same box, increasing radius and border thickness left to right.
  {
    View track = makeTrack(FlexDirection::Row, Justify::Start, Align::End,
                           Size::full(), Size::fit(), white, 14,
                           EdgeInsets::all(10));
    track.addChild(makeBox(Size::pixel(70), Size::pixel(40), blue, 0, 0));
    track.addChild(makeBox(Size::pixel(70), Size::pixel(50), blue, 8, 2, dark));
    track.addChild(makeBox(Size::pixel(70), Size::pixel(60), blue, 16, 4, dark));
    track.addChild(makeBox(Size::pixel(70), Size::pixel(70), blue, 35, 6, dark));
    root.addChild(track);
  }

  // ---------------- Section G: Size::percentage ----------------
  // A full-width row split 25% / 50% / 25%. Percentages resolve against the
  // track's own width, which is definite here (Full, inside a definite
  // root), so this is a case where percentage sizing actually applies.
  {
    View track = makeTrack(FlexDirection::Row, Justify::Start, Align::Stretch,
                           Size::full(), Size::pixel(50), lightGray, 0,
                           EdgeInsets::all(0));
    track.addChild(makeBox(Size::percentage(25), Size::fit(), red));
    track.addChild(makeBox(Size::percentage(50), Size::fit(), green));
    track.addChild(makeBox(Size::percentage(25), Size::fit(), blue));
    root.addChild(track);
  }

  // ---------------- Section H: nested padding + margin ----------------
  // Outer box (padding) -> inner box (border + padding) -> innermost box
  // (margin) — three levels deep, showing padding pushes content inward
  // while margin pushes the box itself away from its parent's edges/siblings.
  {
    View outer;
    outer.style.direction = FlexDirection::Row;
    outer.style.width = Size::full();
    outer.style.height = Size::fit();
    outer.style.padding = EdgeInsets::all(16);
    outer.style.backgroundColor = lightGray;
    outer.style.borderRadius = 6;

    View inner;
    inner.style.direction = FlexDirection::Row;
    inner.style.width = Size::fit();
    inner.style.height = Size::fit();
    inner.style.padding = EdgeInsets::all(12);
    inner.style.backgroundColor = white;
    inner.style.borderWidth = 2;
    inner.style.borderColor = midGray;
    inner.style.borderRadius = 8;

    View innermost = makeBox(Size::pixel(60), Size::pixel(60), pink, 6);
    innermost.style.margin = EdgeInsets::all(10);

    inner.addChild(innermost);
    outer.addChild(inner);
    root.addChild(outer);
  }

  window.setRoot(root);
  window.run();
}