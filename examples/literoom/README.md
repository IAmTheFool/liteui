# Photo Editor: build and run (Windows)

The photo editor lives in `examples/literoom` inside the LiteUI repo.
Object selection uses MobileSAM through ONNX Runtime. Without it, the app falls back to a basic color-grow tool.

## 1. What you need

- Visual Studio (with the **Desktop development with C++** workload) and CMake
- ONNX Runtime for Windows (x64) release zip
- Two MobileSAM model files: `mobile_sam.encoder.onnx` and `mobile_sam.decoder.onnx`

## 2. Where everything goes

```
C:\User\
├── liteui\                               <- the repo
│   ├── build\                            <- created by CMake (safe to delete)
│   └── examples\literoom\
│       ├── main.cpp
│       ├── sam_segmenter.hpp
│       ├── CMakeLists.txt
│       └── models\                       <- ONLY the two .onnx files go here
│           ├── mobile_sam.encoder.onnx
│           └── mobile_sam.decoder.onnx
└── onnxruntime-win-x64-1.31.0\
    └── onnxruntime-win-x64-1.31.0\       <- ONNXRUNTIME_DIR points here
        ├── include\
        └── lib\                          <- onnxruntime.lib + onnxruntime.dll
```

Notes:
- Keep ONNX Runtime **outside** `models\`. The build copies the whole `models\` folder next to the exe, and you don't want ONNX Runtime copied along with it.
- `ONNXRUNTIME_DIR` must be the folder that directly contains `include\` and `lib\`.

## 3. Get the files

**ONNX Runtime:** download the Windows x64 zip from the onnxruntime GitHub releases page and unzip it as shown above.

**Models:** from https://huggingface.co/nrl-ai/samexporter-onnx-models, open the `mobile_sam/` folder and download both `.onnx` files into `examples\literoom\models\`.
Or use the Hugging Face CLI (`pip install huggingface_hub`):

```powershell
hf download nrl-ai/samexporter-onnx-models --include "mobile_sam/*" --local-dir C:\User\sam_download
Copy-Item C:\User\sam_download\mobile_sam\*.onnx C:\User\liteui\examples\literoom\models\
```

## 4. Build and run

Open **Developer PowerShell for VS** (Start menu, search "Developer PowerShell"), then:

```powershell
cd C:\User\liteui

# Configure (only needed the first time, or after deleting build\)
cmake -S . -B build -DLITEROOM_ONNX=ON -DONNXRUNTIME_DIR="C:/User/onnxruntime-win-x64-1.31.0/onnxruntime-win-x64-1.31.0"

# Build just the photo editor
cmake --build build --config Release --target literoom

# Run
.\build\examples\literoom\Release\literoom.exe
```

Use forward slashes in `ONNXRUNTIME_DIR`.

After changing code, you only need the build and run commands again.

## 5. Check that it works

1. Look for this line in the configure output: `literoom: ONNX Runtime enabled (...onnxruntime.lib)`.
2. Open an image in the app and find the **Engine** line under the Object part. It should say **Engine: MobileSAM**.
3. Click **+ Part: Object**, make sure the toggle reads **Click adds: Object** (not Background), and click on the subject. The first click takes a second or two (the image is encoded once); later clicks are fast.
4. Turn on **Show mask tint** to see the mask.

## 6. Troubleshooting

| Engine line / symptom | Cause and fix |
|---|---|
| `built without LITEROOM_ONNX` | Reconfigure with `-DLITEROOM_ONNX=ON` and the `ONNXRUNTIME_DIR` path. |
| `onnxruntime was not found` (CMake error) | `ONNXRUNTIME_DIR` is wrong. It must contain `include\` and `lib\`. |
| Models not found / `looked in ...\models` | Put both `.onnx` files in `examples\literoom\models\`, then rebuild so they get copied next to the exe. Or set the `LITEROOM_MODELS` environment variable to their folder. |
| `encoder input is not NCHW` | You are using an old `sam_segmenter.hpp`. Use the updated one, which supports both encoder layouts. |
| `decoder lacks input '...'` | The decoder model has different input names. The message lists what it does have. |
| `onnxruntime.dll` not found at startup | Rebuild (it is copied next to the exe after linking), or copy `lib\onnxruntime.dll` next to `literoom.exe` by hand. |
| `?` icons in the toolbar | Add `/utf-8` for MSVC in `examples/literoom/CMakeLists.txt`. |

## 7. Clean rebuild

```powershell
cd C:\User\liteui
Remove-Item -Recurse -Force build
# then repeat the Configure, Build and Run steps from section 4
```

---

# Using the Photo Editor

Everything below describes the app as built from this folder. Edits are non-destructive: the original picture is kept, and every slider, curve, mask and crop is re-applied on top of it, so you can undo or change any of it later.

## 8. The window

- **Title bar:** `File`, `View` and `Image` menus, plus **Undo** (↶) and **Redo** (↷) buttons. Empty space in the bar drags the window.
- **Activity bar (left strip):** one icon per tool pane, top to bottom: **Info** (I), **Adjust** (A), **Curve** (~), **Color Mix** (M), **Color Grading** (G), **Masks** (◐), **Crop** (C), **Rotate** (R). Click an icon to open its pane; click the same icon again to hide the sidebar.
- **Sidebar:** shows the selected pane. Drag the thin divider between the sidebar and the canvas to resize it (the Color Grading pane has a wider minimum so its wheels fit).
- **Canvas:** your picture. Scroll the mouse wheel over it to zoom, and use the scrollbars to move around when zoomed in.
- **Status bar:** file name and size, plus the latest message (for example "Exported photo.png", or an error shown in yellow). On the right are zoom controls: `-`, the current zoom, `+`, and `100%` to jump to actual size.

## 9. Quick start

1. **Open** an image: `File > Open Image...` or `Ctrl+O`. PNG and JPEG files are supported.
2. Open the **Adjust** pane and move the sliders. The picture updates as you drag.
3. Optionally use **Crop**, **Rotate**, **Curve**, **Color Mix**, **Color Grading** or **Masks** (see below).
4. **Export:** `File > Export as PNG...` or `Ctrl+E`. Export always writes a PNG.

The **Info** pane shows the file name, pixel size and current zoom.

## 10. Keyboard shortcuts

Shortcuts work wherever the focus is, but are ignored while you are holding a slider or a crop handle.

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open image |
| `Ctrl+E` | Export as PNG |
| `Ctrl+Q` | Quit |
| `Ctrl+Z` | Undo |
| `Ctrl+Y` or `Ctrl+Shift+Z` | Redo |
| `Ctrl+=` (also `Ctrl+Shift+=`) | Zoom in |
| `Ctrl+-` | Zoom out |
| `Ctrl+0` | Actual size (100%) |
| `Ctrl+B` | Show / hide the sidebar |
| `Ctrl+[` / `Ctrl+]` | Rotate 90° left / right |
| `Enter` | Apply the crop (only while the Crop pane is open) |
| `Delete` | Delete the selected mask (only while the Masks pane is open) |
| `Esc` | Close an open menu |

Zoom runs from 10% to 800%, in steps of 1.25x. Undo keeps up to 200 steps, and one slider drag counts as a single step.

The **Image** menu holds `Rotate Left 90°`, `Rotate Right 90°`, `Flip Horizontal` and `Flip Vertical`.

## 11. Adjust

The top of the pane has a **histogram** of the picture as currently shown. Every slider runs from -100 to +100, and 0 means untouched.

**Tone**

| Slider | What it does |
|---|---|
| Exposure | Scales all brightness up or down |
| Brightness | Adds or subtracts a flat amount of light |
| Contrast | Stretches or squashes tones around mid-gray |
| Highlights | Darkens or brightens the bright tones |
| Shadows | Darkens or brightens the dark tones |
| Whites | Fine control of the very brightest tones |
| Blacks | Fine control of the very darkest tones |

**Color**

| Slider | What it does |
|---|---|
| Temperature | Positive is warmer (more orange), negative is cooler (more blue) |
| Tint | Positive is more magenta, negative is greener |
| Vibrance | Boosts muted colors more than already-strong ones |
| Saturation | Boosts or reduces all color evenly |

On large photos the editor shows a faster, lower-resolution preview while you drag a slider, then renders the full picture when you let go.

## 12. Curve

A tone curve for fine contrast and color control.

- Use the tabs to pick **RGB** (all channels together) or a single **R**, **G** or **B** channel.
- **Click** the curve to add a point, **drag** a point to move it, **right-click** a point to delete it. A curve can have up to 16 points.
- The line under the curve shows the active point's input and output values (0 to 255).
- **Reset Curve** resets the current channel. **Reset All Curves** resets all four.

## 13. Color Mix

Adjust individual colors. There are eight hue bands, each with three sliders, chosen by the tabs at the top:

- **Hue** shifts a color toward its neighbors.
- **Sat** makes the color more or less vivid.
- **Lum** makes the color lighter or darker.

A bullet (•) next to a tab name means that tab has non-zero values, so you can tell an edit exists even after switching tabs. **Reset Color Mix** clears everything.

## 14. Color Grading

Tint the shadows, midtones and highlights with color wheels.

- **3-Way** shows the Shadows, Midtones and Highlights wheels together.
- **Shad**, **Mid**, **High** and **Glob** show one large wheel for that range (Glob = Global, the whole picture).
- Drag inside a wheel to choose the color (hue and saturation) and use the luminance control to lighten or darken that range. The readout shows the active wheel as `H:` hue, `S:` saturation, `L:` luminance.
- **Blending** (0 to 100) controls how much the ranges overlap. **Balance** (-100 to +100) shifts the split between shadows and highlights.
- A bullet (•) on a tab means that range has been changed. **Reset Color Grading** clears it all.

## 15. Masks (local adjustments)

A mask limits adjustments to part of the picture. Each mask has its own Tone and Color sliders (the same ones as the Adjust pane), applied only where the mask covers.

### Basics

1. Open the **Masks** pane (◐), then click one of the **+** buttons to add a mask: **Radial Gradient**, **Linear Gradient**, **Brush**, **Luminosity Range** or **Object**.
2. The new mask appears in the list (up to 8 masks). Click a row to select it. Rows are marked `(inverted)` or `(off)` when that applies.
3. Set the shape (see below), then move the mask's own sliders.

For the selected mask:

| Button | What it does |
|---|---|
| **Enabled** | Turns the mask's effect on or off without deleting it |
| **Invert** | Applies the adjustments everywhere except the mask |
| **Show mask tint** | Paints a colored overlay so you can see what the mask covers |
| **Reset Mask Sliders** | Zeros the mask's adjustment sliders |
| **Delete Mask** | Removes the mask (also the `Delete` key) |

### Mask types

- **Radial Gradient:** an ellipse. Drag it on the image to move it and drag its square handles to resize. Sliders: **Width**, **Height**, **Angle**, **Feather** (softness of the edge).
- **Linear Gradient:** a fade across the picture. Drag to move it; its handles resize and also turn it. Sliders: **Length**, **Angle**.
- **Brush:** drag on the image to paint the mask. Controls: **Erase** (paint away instead), **Size**, **Feather**, **Density**, and **Clear Strokes**. Brush settings apply to the next stroke only; strokes already painted keep their own settings. There is a limit on the number of strokes per brush, and the status bar tells you when you reach it.
- **Luminosity Range:** selects pixels by brightness, with no handles. Sliders: **Min**, **Max**, **Smoothness**. Min cannot pass Max.
- **Object:** click on the subject to select it (see the next section).

### Combining shapes with Parts

One mask can combine up to 6 shapes ("parts"). Use the **+ Part: ...** buttons to add another shape. Each part after the first is joined to the ones before it using **Add**, **Subtract** or **Intersect**. Pick a part in the list to edit it, and use **Remove Part** to delete it. For example, a Radial part with a Brush part set to **Subtract** gives you an ellipse with a hand-painted hole.

Masks stay attached to the picture, so they follow your crops, rotations and flips.

## 16. Object selection

Select a subject by clicking on it. This uses MobileSAM when the app was built with ONNX (see sections 1 to 5), and a basic color-grow tool otherwise.

1. In **Masks**, click **+ Object** (or **+ Part: Object** to add one to an existing mask).
2. Wait for the status bar to finish "Preparing object selection..." The image is analyzed once per opened image, which takes a second or two on CPU. After that, selecting is fast.
3. **Left-click** the subject. A dot marks each click.
4. Refine the selection:
   - Click more of the subject to add to it.
   - **Right-click** to add a point of the *opposite* kind (a point that should be excluded).
   - Click an existing dot to remove it. Removing the last dot clears the selection.
   - The **Click adds: Background** button flips the meaning of left and right clicks. The label never changes; when the button is highlighted, a left-click marks *background* and a right-click marks the object. When it is not highlighted, a left-click marks the object.
5. Fine-tune the edge with **Feather** (softness) and **Grow** (expand or shrink the selection, -100 to +100).
6. Turn on **Show mask tint** to see the result, then use the mask's Tone/Color sliders as usual. **Clear selection** starts over.

The line at the bottom of the Object controls shows which engine is active: `Engine: MobileSAM`, or `Engine: basic color grow (...)` with the reason and the folder it searched for the models. The engine is chosen the first time you use object selection. If you add the model files afterwards, restart the app. Object clicks only work while the Masks pane is open with an Object part selected.

The **Set test map (bright areas)** button is a testing helper that fills the selection with the bright areas of the picture. It does not use the AI model.

## 17. Crop

1. Open the **Crop** pane (C) and **drag on the image** to choose an area. Drag the handles to resize, or drag inside the box to move it.
2. Pick an **Aspect ratio**: **Free**, **Original**, **1:1**, **4:3**, **3:2** or **16:9**. The readout shows the selection size in pixels.
3. Click **Apply Crop** (or press `Enter`).
4. **Reset Selection** restores the full-picture selection. **Remove Crop** brings back the uncropped picture, so a crop can be undone later.

## 18. Rotate and straighten

- **Rotate Left 90°**, **Rotate Right 90°**, **Flip Horizontal**, **Flip Vertical**: lossless turns and mirrors. **Reset Turns & Flips** undoes them.
- **Straighten:** the **Angle** slider (-45° to +45°; positive turns the picture clockwise) levels a tilted horizon. The **-1°**, **-0.1°**, **+0.1°** and **+1°** buttons nudge it precisely. Combined with the 90° buttons, any angle is reachable.
- **Corners:** after straightening, the edges would show empty corners. **Auto-crop empty corners** trims them away; **Keep full canvas** keeps the whole rotated canvas with empty corners.
- **Reset Angle** returns to 0°.
- Straightening clears any crop you applied, so crop after straightening.

## 19. Tips and usage troubleshooting

| Symptom | What to do |
|---|---|
| Nothing happens when I click the image in the Masks pane | A mask must be selected, and for Object selection an Object part must be selected. |
| Object clicks select a rough color region instead of the subject | The Engine line says `basic color grow`. Fix the models (section 6), then restart the app. |
| Status bar says "Object selection unavailable: ..." | The error text names the problem. Common causes are missing model files or a wrong model export. |
| The first object click is slow | Normal: the image is analyzed once. Later clicks on the same image are fast. |
| Slider feels laggy on a very large photo | Expected: a quick preview is shown while dragging and the full render follows on release. |
| Can't save as JPEG | Export supports PNG only. |
| A shortcut does nothing | Release any slider or crop handle you are holding; shortcuts are ignored mid-drag. |