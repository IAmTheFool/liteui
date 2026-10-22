#pragma once

struct CounterModel {
  int value = 0;
  void increment() { ++value; }
  void reset() { value = 0; }
};