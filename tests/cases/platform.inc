// Platform-specific helpers that are still pure logic (no window needed).
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#if !defined(_WIN32)
TEST_CASE("linux: xkbKeysymToKey maps letters, digits, F-keys", "[platform][keys]") {
  CHECK(xkbKeysymToKey(XKB_KEY_a) == Key::A);
  CHECK(xkbKeysymToKey(XKB_KEY_A) == Key::A); // case-insensitive
  CHECK(xkbKeysymToKey(XKB_KEY_z) == Key::Z);
  CHECK(xkbKeysymToKey(XKB_KEY_0) == Key::N0);
  CHECK(xkbKeysymToKey(XKB_KEY_9) == Key::N9);
  CHECK(xkbKeysymToKey(XKB_KEY_F1) == Key::F1);
  CHECK(xkbKeysymToKey(XKB_KEY_F12) == Key::F12);
}

TEST_CASE("linux: xkbKeysymToKey maps named keys", "[platform][keys]") {
  CHECK(xkbKeysymToKey(XKB_KEY_Return) == Key::Enter);
  CHECK(xkbKeysymToKey(XKB_KEY_KP_Enter) == Key::Enter);
  CHECK(xkbKeysymToKey(XKB_KEY_Escape) == Key::Escape);
  CHECK(xkbKeysymToKey(XKB_KEY_BackSpace) == Key::Backspace);
  CHECK(xkbKeysymToKey(XKB_KEY_Tab) == Key::Tab);
  CHECK(xkbKeysymToKey(XKB_KEY_space) == Key::Space);
}
#endif
