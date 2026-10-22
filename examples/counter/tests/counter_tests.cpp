#include <catch2/catch_test_macros.hpp>
#include "counter_model.hpp"

TEST_CASE("CounterModel starts at zero") {
  CounterModel m;
  REQUIRE(m.value == 0);
}

TEST_CASE("increment adds one") {
  CounterModel m;
  m.increment();
  m.increment();
  REQUIRE(m.value == 2);
}

TEST_CASE("reset returns to zero") {
  CounterModel m;
  m.increment();
  m.reset();
  REQUIRE(m.value == 0);
}