# Profiling

LiteUI has a small built-in frame profiler. When enabled it prints, about once per second to `stderr`, how long each stage of interaction and painting took: the average, the worst case, and how many times the stage ran. When disabled it **compiles to nothing**: no timers, no branches, no overhead.

- [Platform support](#platform-support)
- [Enabling it](#enabling-it)
- [What it prints](#what-it-prints)
- [The stages](#the-stages)
- [Reading the numbers](#reading-the-numbers)
- [Workflow tips](#workflow-tips)
- [How reporting works](#how-reporting-works)
- [Timing your own code](#timing-your-own-code)
- [Troubleshooting](#troubleshooting)

## Platform support

> **The profiler currently produces output on Windows only.**
>
> All of the timing probes live in the Direct2D backend. A Linux (Wayland) build accepts `LITEUI_PROFILE`, and the `gcc-*-profile` presets exist and build, but nothing in the Wayland code path is instrumented, so nothing is ever printed.

The rest of this guide describes the Windows behavior.

## Enabling it

Profiling is a compile-time switch, `LITEUI_PROFILE`.

**With the bundled presets** (recommended; profile in a *Release* build, since Debug timings are not representative):

```sh
cmake --preset msvc-release-profile
cmake --build --preset msvc-release-profile
build/release-profile/examples/hello/hello.exe
```

`msvc-debug-profile`, `gcc-debug-profile` and `gcc-release-profile` also exist; see [BUILDING.md](BUILDING.md#presets).

**With a CMake option**, on any configuration:

```sh
cmake -S . -B build/prof -G Ninja -DCMAKE_BUILD_TYPE=Release -DLITEUI_PROFILE=ON
```

**In your own project** that pulls in LiteUI as a subdirectory, set the option before adding it. Everything that links `liteui::liteui` then gets `-DLITEUI_PROFILE`:

```cmake
set(LITEUI_PROFILE ON CACHE BOOL "" FORCE)
add_subdirectory(path/to/liteui)
```

**Without CMake**, define the macro when compiling the translation unit that defines `LITEUI_IMPLEMENTATION` (the profiler lives in the implementation section of the header):

```bat
cl /std:c++latest /EHsc /utf-8 /DLITEUI_PROFILE /I path\to\liteui main.cpp user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib
```

The report goes to `stderr`, so run the program from a terminal. The bundled examples are console-subsystem programs and show it directly. If you later link your own app as a GUI-subsystem executable (`WIN32_EXECUTABLE`), there is no console, so launch it with `2> profile.txt` or from a debugger that captures stderr.

## What it prints

While the app is being used, you will see blocks like this (the numbers here are illustrative):

```
[liteui profile]
  mouse     x212  avg   0.04 ms  worst   0.61 ms
  build     x58   avg   1.92 ms  worst   4.10 ms
  canvas    x3    avg   2.75 ms  worst   3.30 ms
  blit      x58   avg   0.31 ms  worst   0.52 ms
  EndDraw   x58   avg   0.88 ms  worst   1.90 ms
```

Each line is one stage:

- `xN` is how many times the stage ran since the previous report.
- `avg` is the mean duration in milliseconds, and `worst` the single slowest run.
- A stage that did not run in the window is omitted from the block.

## The stages

Stages are printed in this fixed order:

| Line | Measures |
|---|---|
| `mouse` | One `WM_MOUSEMOVE` from start to finish: scrollbar and drag updates, hover tracking, tooltip hover, and, when a hover handler reports a change, the **re-poll and relayout** it triggers. |
| `recompute` | *Declared but never recorded* (see [Known gaps](#known-gaps)). Always absent. |
| `build` | All the drawing of one window paint: `BeginDraw`, clearing the background, painting the whole tree, and painting the tooltip. It does **not** include `EndDraw`. |
| `canvas` | One run of a canvas's `onPaint` into its offscreen bitmap, including `BeginDraw`/`EndDraw` on that bitmap. Recorded **only when the canvas actually redraws**, not on every window paint. |
| `appPaint` | *Declared but never recorded* (see [Known gaps](#known-gaps)). Always absent. |
| `blit` | Turning one canvas's cached bitmap into a GPU bitmap and drawing it into the window. Recorded **once per canvas per window paint**. |
| `EndDraw` | The final flush of the window's render target. |

### Nesting

Stages nest, so do not add them up:

```
build      (one window paint)
 ├─ canvas   (only for canvases that redrew this paint)
 ├─ blit     (every canvas, every paint)
 └─ everything else: boxes, borders, text, scrollbars, tooltip
EndDraw    (afterwards, outside build)
```

`canvas` and `blit` run **inside** `build`. A paint's total cost is roughly `build + EndDraw`. The time `build` spends on things *other* than canvases is approximately `build − canvas − blit`, but be careful: `canvas` and `blit` count once per canvas, so with several canvases per paint subtract their sums over the window, not their averages.

## Reading the numbers

The budget at 60 Hz is **16.7 ms per frame**. Aim to keep `build + EndDraw` well inside it. Then look for what dominates.

| What you see | What it usually means | What to try |
|---|---|---|
| `canvas` average is large | Your `onPaint` is expensive. | Precompute data outside `onPaint`, draw less, or shrink the canvas. See [CANVAS.md](CANVAS.md#performance-notes). |
| `canvas` runs far more often than expected (`x` is large) | `canvasDirtySource` returns `true` too often, or an always-on interval is driving redraws. | Return `true` only on real change; stop timers when nothing animates. |
| `blit` is large or `canvas` is small but `build` is large | Big canvases are re-uploaded to the GPU on **every** window paint on Windows, even when they did not redraw. | Make canvases no larger than necessary; avoid forcing frequent repaints around a large canvas. |
| `build` is large with little `canvas`/`blit` time | Lots of nodes, or many text nodes whose layout is being rebuilt (text content changing every frame, width changing). | Cut node counts, use [keyed lists](../README.md#keyed-dynamic-lists) so unchanged rows are reused, and avoid animating text. |
| `EndDraw` is large | Presenting/flushing is slow: GPU or driver bound, or a very large window. | Compare on other hardware; reduce overdraw and canvas sizes. |
| `mouse` average is high or `worst` spikes | A hover handler (`onHoverAt`) is doing real work, or returns `true` constantly, forcing a re-poll and relayout on every pointer move. | Make hover handlers cheap and return `true` only when something visible changed. |
| `build` runs (`xN`) while the app is idle | Something keeps requesting repaints: an interval, a `Dynamic<>` callback whose value changes every poll, or a dirty source stuck on `true`. | Find what changes each tick; an idle LiteUI should print nothing. |

An **idle** application should be silent. If the report keeps printing while you are not touching anything, that is itself a finding.

## Workflow tips

- **Profile Release builds.** Debug builds of both your code and the library are far slower and skew the ratios between stages.
- **Reproduce one interaction at a time.** Scroll a list, drag a slider, resize the window, hover a menu, and read the report for each in isolation.
- **Watch `worst`, not just `avg`.** Stutter is a spike, not a mean. A tiny average with an occasional 30 ms worst case is a dropped frame.
- **Capture and compare.** Redirect `stderr` to a file before and after a change:

  ```sh
  hello.exe 2> before.txt
  ```

- **Counts matter as much as times.** A cheap stage that runs thousands of times a second (`mouse`, `canvas`) can still cost a lot in total.

## How reporting works

- Each probe records a duration into its stage's counters, then checks whether **one second or more** has passed since the last report.
- If so, it prints the block, **resets all counters**, and restarts the clock.
- Reporting is therefore **driven by activity**, not by a timer. Nothing prints while the window is idle, and the first event after a quiet spell prints a block covering everything since the previous report. The `xN` counts are "since the last report", which can be longer than one second after an idle gap.
- The block only prints if at least one `mouse` or `build` event was recorded in the window. A window containing only canvas redraws and no paint or mouse event is reset silently.
- The counters are plain, unsynchronized data, which is fine because every probe runs on the UI thread.
- A probe costs two `steady_clock` reads. With `LITEUI_PROFILE` undefined, the probes expand to nothing.

### Known gaps

- **No Linux instrumentation.** The Wayland backend has no probes, so Linux builds print nothing.
- **`recompute` and `appPaint` are never recorded.** The report has slots and print lines for them, but no code path feeds them, so they never appear. Relayout cost is only visible indirectly, inside `mouse` when a hover handler triggers it.
- **Fixed set of stages.** You cannot register your own stage with the built-in report.

## Timing your own code

The built-in stages cover the library's work. For your own code, use `std::chrono` and print or log it yourself. For example, to see how long a canvas's `onPaint` spends in one section (on Linux, where the built-in `canvas` stage is not available, this is the way to do it):

```cpp
c.onPaint = [&](CanvasContext &g) {
    auto t0 = std::chrono::steady_clock::now();
    drawScene(g);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    if (ms > 4.0) std::fprintf(stderr, "slow drawScene: %.2f ms\n", ms);
};
```

The same applies to expensive `Dynamic<>` callbacks, which run after every event, so time them if the `mouse` or `build` counts look inflated.

## Troubleshooting

**Nothing is printed.**
- On Linux, that is expected: there are no probes in the Wayland backend.
- On Windows, confirm the build really has `LITEUI_PROFILE` (for example, configure with a `*-profile` preset and check that the build directory is `build/release-profile` or `build/debug-profile`).
- The report prints only when something happens. Move the mouse over the window or cause a repaint.
- If the program has no console, redirect `stderr` to a file.

**Stages `recompute` or `appPaint` never appear.**
They are not recorded by the current code. See [Known gaps](#known-gaps).

**Only `mouse` lines appear, never `build`.**
The window is not repainting, which usually means mouse movement is not changing anything visible. That is the healthy case.

**The numbers differ a lot between runs.**
Debug builds, other processes, and window size all matter. Profile Release builds at a fixed window size and repeat the interaction a few times.
