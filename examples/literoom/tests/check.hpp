// check.hpp - the tiny test harness shared by the unit tests in this folder.
//   CHECK(cond)                 records a failure (with file:line) and goes on
//   CHECK_NEAR(a, b, tol)       |a - b| <= tol
//   return finish();            prints "all ok" or "N FAILED"; the exit code
#pragma once
#include <cmath>
#include <cstdio>

namespace tcheck
{
  inline int failures = 0;
}

// Variadic so that braced initializers with commas (CropRect{1, 2, 3, 4}) work inside.
#define CHECK(...)                                                          \
  do                                                                        \
  {                                                                         \
    if (!(__VA_ARGS__))                                                     \
    {                                                                       \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);    \
      ++tcheck::failures;                                                   \
    }                                                                       \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                      \
  do                                                                               \
  {                                                                                \
    const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);       \
    if (!(std::fabs(va_ - vb_) <= static_cast<double>(tol)))                       \
    {                                                                              \
      std::printf("FAIL %s:%d  %s ~ %s  (%.6g vs %.6g)\n", __FILE__, __LINE__,    \
                  #a, #b, va_, vb_);                                               \
      ++tcheck::failures;                                                          \
    }                                                                              \
  } while (0)

inline int finish()
{
  std::printf(tcheck::failures ? "%d FAILED\n" : "all ok\n", tcheck::failures);
  return tcheck::failures ? 1 : 0;
}