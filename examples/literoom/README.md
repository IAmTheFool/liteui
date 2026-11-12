# Literoom

Literoom is a Lightroom-style photo editor written in C++23 on top of [LiteUI](../..), a single-header UI library. It lives in `examples/literoom` inside the LiteUI repo.

Edits are **non-destructive**. Your original picture is never changed. Every slider, curve, mask and crop is stored as a small list of settings and re-applied on top of the original, so you can undo or change any of it later.

## What it can do

- **Global adjustments:** Exposure, Brightness, Contrast, Highlights, Shadows, Whites, Blacks, the Levels sliders (Black Point, White Point, Gamma), Temperature, Tint, Vibrance, Saturation, Hue, Colorize, Texture, Clarity, Sharpness, Dehaze, Noise Reduction, Color Noise and Blur. **Auto Tone** and a **white-balance picker** set some of them for you.
- **Looks:** ten **Filters** (Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold, Cross Process), **Vignette** and **Grain**, and **3D LUTs** (`.cube` files) with an Amount slider.
- **Tone curves** (RGB plus separate Red, Green and Blue), a **Color Mixer** (eight color bands) and **Color Grading** (shadow, midtone, highlight and global wheels).
- **Local adjustments with masks:** radial gradient, linear gradient, rectangle, polygon or freehand lasso, brush, luminosity range, color range and AI object selection. A mask can combine several shapes.
- **Geometry:** crop with aspect ratios, 90° rotation, flips, and free-angle straightening.
- **Workflow tools:** a History panel, **Presets** (saved looks), named **Snapshots**, **virtual copies** (more than one set of edits for one picture), copy / paste / sync of settings between pictures, a filmstrip with **star ratings, pick / reject flags and color labels**, Before/After and split comparison, clipping warnings, a histogram that can switch to a **waveform** or a **vectorscope**, and a live RGB readout under the cursor.
- **Saved edits:** your edits are saved automatically in a small file next to each picture and come back the next time you open it.
- **Export** to PNG (8 or 16 bits per channel) or JPEG with a quality setting, optional resizing and **batch export** of many pictures. The picture's color profile travels with the export. 16-bit PNGs are edited and exported at full precision.

Object selection uses MobileSAM through ONNX Runtime. Without it, the app falls back to a basic color-grow tool. Everything else works the same either way.

---

# Part 1: Build and run

## 1. What you need

- Visual Studio (with the **Desktop development with C++** workload) and CMake
- *Optional, for AI object selection:*
  - ONNX Runtime for Windows (x64) release zip
  - Two MobileSAM model files: `mobile_sam.encoder.onnx` and `mobile_sam.decoder.onnx`

If you skip the optional items, skip sections 3 and 5 below and build without `-DLITEROOM_ONNX=ON`.

## 2. Where everything goes

```
C:\User\
├── liteui\                               <- the repo
│   ├── build\                            <- created by CMake (safe to delete)
│   └── examples\literoom\
│       ├── CMakeLists.txt
│       ├── src\                          <- all the code (see "Source layout" in Part 3)
│       │   ├── main.cpp                  <- the UI and the glue
│       │   ├── core\                     <- shared basics: pixel math, geometry, slider table
│       │   ├── color\                    <- Color Mixer, Color Grading, curves, color pass
│       │   ├── detail\                   <- texture, clarity, sharpening, noise
│       │   ├── mask\                     <- mask shapes, brush, object selection
│       │   ├── edit\                     <- the edit state and what the renderer needs
│       │   ├── render\                   <- the pixel pass and the histogram
│       │   ├── io\                       <- saved edits, JSON, files, EXIF, PNG/JPEG
│       │   └── workers\                  <- background threads
│       ├── tests\                        <- unit tests, the golden render test and the LUT-pane test
│       ├── docs\                         <- developer documentation (Part 3)
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

## 3. Get the files (only for object selection with MobileSAM)

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

To build **without** AI object selection, leave out both `-D` options. The configure output then says `literoom: built WITHOUT ONNX (basic object selection only)`.

After changing code, you only need the build and run commands again.

**With CMake presets:** `CMakePresets.json` (in the liteui root) has presets for MSVC and GCC, such as `msvc-release` and `gcc-release`. From a Developer PowerShell: `cmake --preset msvc-release`, then `cmake --build --preset msvc-release`. The presets use Ninja and build into `build\release`, so the program is `build\release\examples\literoom\literoom.exe`.

**Linux:** the same CMake project builds on Linux (LiteUI uses Wayland there). Configure with `-DONNXRUNTIME_DIR=<unpacked onnxruntime folder>` if you want ONNX. The build sets an rpath so `libonnxruntime.so` is found when you run from the build folder.

## 5. Check that it works

1. Look for this line in the configure output: `literoom: ONNX Runtime enabled (...onnxruntime.lib)`.
2. Open an image in the app, open the **Masks** pane, add an **Object** mask, and find the **Engine** line under the Object controls. It should say **Engine: MobileSAM**.
3. Make sure the **Click adds: Background** button is *not* highlighted (so clicks mark the subject) and click on the subject. The first click takes a second or two (the image is encoded once); later clicks are fast.
4. Turn on **Tint** to see the mask.

## 6. Build troubleshooting

| Engine line / symptom | Cause and fix |
|---|---|
| `built without LITEROOM_ONNX` | Reconfigure with `-DLITEROOM_ONNX=ON` and the `ONNXRUNTIME_DIR` path. |
| `onnxruntime was not found` (CMake error) | `ONNXRUNTIME_DIR` is wrong. It must contain `include\` and `lib\`. |
| Models not found / `looked in ...\models` | Put both `.onnx` files in `examples\literoom\models\`, then rebuild so they get copied next to the exe. Or set the `LITEROOM_MODELS` environment variable to their folder. |
| `encoder input rank N is neither NCHW (4) nor HWC (3)` | The encoder model has an input shape Literoom doesn't support. Use the two MobileSAM files from section 3 (both the NCHW and the raw-HWC export layouts work). |
| `decoder lacks input '...'` | The decoder model has different input names. The message lists what it does have. |
| `onnxruntime.dll` not found at startup | Rebuild (it is copied next to the exe after linking), or copy `lib\onnxruntime.dll` next to `literoom.exe` by hand. |
| `?` icons in the toolbar | The file must be compiled as UTF-8. The project already adds `/utf-8` for MSVC; if you copied the code elsewhere, add it to your own build. |
| Stack overflow crash at startup (MSVC) | The project links with an 8 MB stack (`/STACK:8388608`). If you changed the link options, put it back. |

## 7. Clean rebuild

```powershell
cd C:\User\liteui
Remove-Item -Recurse -Force build
# then repeat the Configure, Build and Run steps from section 4
```

---

# Part 2: Using Literoom

## 8. The window

- **Title bar:** `File`, `View`, `Image` and `Photo` menus, plus **Undo** (↶) and **Redo** (↷) buttons. Empty space in the bar drags the window.
  - **File:** Open Image, Open Folder, Export, Export Selected, Export All Shown, Stop Export, Exit.
  - **View:** zoom, sidebar and filmstrip toggles, Previous/Next Image, Before/After, split view, and the clipping warnings.
  - **Image:** Rotate Left 90°, Rotate Right 90°, Flip Horizontal, Flip Vertical, Auto Tone, Copy Settings, Paste Settings, Select All Photos, Sync Settings to Selected, Create Virtual Copy, Delete Virtual Copy.
  - **Photo:** rate the open picture 1 to 5 stars or clear the rating, flag it as Pick or Rejected (or remove the flag), and give it a color label (or clear it).
- **Activity bar (left strip):** one icon per pane, top to bottom: **Info** (I), **History** (H), **Snapshots** (S), **Adjust** (A), **Presets** (P), **LUTs** (L), **Curve** (~), **Color Mix** (M), **Color Grading** (G), **Masks** (◐), **Crop** (C), **Rotate** (R), **Export** (E). Click an icon to open its pane; click the same icon again to hide the sidebar.
- **Sidebar:** shows the selected pane. Drag the thin divider between the sidebar and the canvas to resize it (the Color Grading pane has a wider minimum so its wheels fit).
- **Canvas:** your picture. Scroll the mouse wheel over it to zoom, and use the scrollbars to move around when zoomed in. Hover over the picture to see the RGB values of the pixel under the cursor.
- **Filmstrip:** a strip of thumbnails under the canvas (see section 15).
- **Status bar:** file name and size, plus the latest message (for example "Exported photo.jpg", or an error shown in yellow). On the right are zoom controls: `-`, the current zoom, `+`, and `100%` to jump to actual size.

## 9. Quick start

1. **Open** an image with `File > Open Image...` (`Ctrl+O`), or a whole folder with `File > Open Folder...` (`Ctrl+Shift+O`). PNG and JPEG files are supported.
2. Open the **Adjust** pane and move the sliders. The picture updates as you drag.
3. Optionally use **Crop**, **Rotate**, **Curve**, **Color Mix**, **Color Grading** or **Masks** (see below).
4. **Export** with `File > Export...` (`Ctrl+E`) or the **Export** pane.

Your edits are saved automatically as you go (see section 16), so closing the app never loses work.

The **Info** pane shows the file name, pixel size and current zoom. For a JPEG it also shows the camera data (make, model, lens, date taken, ISO, shutter, aperture and focal length), and for any picture what the file says about its color space (for example "sRGB (embedded profile)").

## 10. Keyboard shortcuts

Edits and dialogs are ignored while you are holding a slider or a crop handle. The plain-key shortcuts (no Ctrl) are also ignored while you are typing in a text field, such as a mask, preset or snapshot name.

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open image |
| `Ctrl+Shift+O` | Open folder |
| `Ctrl+E` | Export |
| `Ctrl+Shift+E` | Export the selected pictures (batch) |
| `Ctrl+Q` | Quit |
| `Ctrl+Z` | Undo |
| `Ctrl+Y` or `Ctrl+Shift+Z` | Redo |
| `Ctrl+=` (also `Ctrl+Shift+=`) | Zoom in |
| `Ctrl+-` | Zoom out |
| `Ctrl+0` | Actual size (100%) |
| `Ctrl+B` | Show / hide the sidebar |
| `Ctrl+[` / `Ctrl+]` | Rotate 90° left / right |
| `Left` / `Right` | Previous / next picture in the folder |
| `F6` | Show / hide the filmstrip |
| `\` | Before / After |
| `Y` | Before \| After split view |
| `J` | Clipping warnings on / off |
| `Shift+J` | Clipping: switch between red/blue and per-channel colors |
| `Ctrl+A` | Select all pictures in the filmstrip |
| `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy / paste settings |
| `Ctrl+Shift+S` | Sync settings to the selected pictures |
| `A` | Auto Tone |
| `W` | Pick white balance |
| `0` to `5` | Rate the open picture (`0` clears the rating) |
| `P` / `X` / `U` | Flag as Pick / Rejected / remove the flag |
| `6` / `7` / `8` / `9` | Color label red / yellow / green / blue |
| `Enter` | Apply the crop (only while the Crop pane is open) |
| `Delete` | Delete the selected mask (only while the Masks pane is open) |
| `Esc` | Close an open menu, or cancel the white-balance picker |

Zoom runs from 10% to 800%, in steps of 1.25x.

## 11. Adjust

The top of the pane has a **histogram** of the picture as currently shown. Click it to switch between the histogram, a **waveform** (every picture column drawn as a column of levels) and a **vectorscope** (every pixel's color drawn as a point). Most sliders run from -100 to +100, and 0 means untouched. The exceptions are noted below.

Behind the scenes the sliders are applied in stages, always in this order: **Tone**, then **Color** (which includes the Filters and the 3D LUT), then **Detail**, then masks, and last the **Effects** (Vignette and Grain). You don't need to think about it, but it explains why, for example, Exposure always acts before Saturation.

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
| Black Point | Levels: moves the input level that becomes pure black |
| White Point | Levels: moves the input level that becomes pure white |
| Gamma | Levels: brightens (+) or darkens (-) the midtones without moving black or white |

**Color**

| Slider | What it does |
|---|---|
| Temperature | Positive is warmer (more orange), negative is cooler (more blue) |
| Tint | Positive is more magenta, negative is greener |
| Vibrance | Boosts muted colors more than already-strong ones |
| Saturation | Boosts or reduces all color evenly |
| Hue | Rotates all colors around the color wheel |
| Colorize Hue | Picks the hue (0 to 360°) of a color cast added over the whole picture |
| Colorize Amount | How strong that cast is (0 to 100; 0 is off) |

**Detail** (these look at neighboring pixels, so they can be a little slower)

| Slider | What it does |
|---|---|
| Texture | Boosts (+) or softens (-) fine texture |
| Clarity | Boosts (+) or softens (-) mid-sized contrast, for a "punchier" or "dreamier" look |
| Sharpness | Sharpens edges (0 to 100) |
| Dehaze | Removes (+) or adds (-) atmospheric haze |
| Noise Reduction | Smooths grain in brightness (0 to 100) |
| Color Noise | Smooths colored blotches (0 to 100) |
| Blur | A soft color blur (0 to 100), mostly useful inside masks |

**Filters** (0 to 100 each; 0 is off): Greyscale, Sepia, Vintage, Fade, Negative, Posterize, Solarize, Bleach Bypass, Threshold and Cross Process. They run after the other color sliders, just before the 3D LUT.

**Effects** (applied last, after masks, and global only: masks do not get them):

| Slider | What it does |
|---|---|
| Vignette | Darkens (-) or lightens (+) the corners, following the shape of the cropped picture |
| Midpoint, Feather | Where the vignette starts and how softly it fades |
| Grain | Adds film grain (0 to 100) |
| Size, Roughness | How coarse the grain is and how smooth or crisp it looks |

On large photos the editor shows a faster, lower-resolution preview while you drag a slider, then renders the full picture when you let go.

### Auto Tone and the white-balance picker

- **Auto Tone** (Image > Auto Tone, or `A`) measures the picture's tonal range and sets Exposure, Contrast, Whites and Blacks to stretch it onto a sensible black and white point. It leaves Brightness, Highlights, Shadows, the Levels sliders, curves and masks alone. A flat or nearly empty picture is left unchanged, and the status bar says what happened. It is one undo step.
- **Pick White Balance** (the button in the Adjust pane, or `W`): click a spot that should be neutral gray and Temperature and Tint are set to make it so. Very dark or very bright spots are refused with a message. `Esc` cancels the picker.

### Clipping warnings

Press `J` to highlight parts of the picture that have lost detail: crushed shadows and blown highlights. They are shown on the picture and in the histogram. `Shift+J` switches between the classic red/blue overlay and a per-channel one that shows which color channel is clipping.

## 12. Curve

A tone curve for fine contrast and color control.

- Use the tabs to pick **RGB** (all channels together) or a single **R**, **G** or **B** channel.
- **Click** the curve to add a point, **drag** a point to move it, **right-click** a point to delete it. A curve can have up to 16 points.
- The line under the curve shows the active point's input and output values (0 to 255).
- A faint histogram sits behind the curve, and curves you have already changed on other channels stay visible, dimmed.
- **Reset Curve** resets the current channel. **Reset All Curves** resets all four.

The RGB curve is applied right after the Tone sliders, and each channel's own curve is applied on top of that.

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

## 15. History, Filmstrip and comparing

### History

The **History** pane (H) lists every edit you have made, newest at the top and **Original** at the bottom. Each row says what changed, for example "Exposure +20", "Crop", "Tone Curve: Red" or "Mask 1: Feather +30".

- **Click a row** to jump back to that step. This is the same as pressing Undo until you get there.
- Rows **ahead** of your current step are dimmed. They are still there until you make a new edit, which discards them.
- Dragging a slider counts as **one** step, no matter how long you drag. Undo keeps up to 200 steps.
- The undo history is not saved with the picture; it starts fresh each time you open a file.

### Filmstrip

When you open a file or a folder, a strip of thumbnails appears under the canvas, showing every JPEG and PNG in that folder (the first 5000 if there are more).

- **Click** a thumbnail, or press `Left` / `Right`, to open that picture. Your edits to the current one are saved first.
- A **dot** on a thumbnail means that picture has saved edits.
- `F6` hides or shows the strip.

### Ratings, flags and color labels

Rate the open picture with `0` to `5` (`0` clears the rating), flag it with `P` (pick), `X` (rejected) or `U` (no flag), and give it a color label with `6` to `9` (red, yellow, green, blue; purple is in the Photo menu). They are also in the **Photo** menu. These are not edits: they have no undo step and no History row, and they are saved in the picture's saved-edits file. With several pictures selected in the filmstrip (`Ctrl+click`, `Shift+click`, `Ctrl+A`), a rating, flag or label is applied to all of them. A filter on the filmstrip can show only pictures with at least a number of stars, certain flags or certain labels.

### Presets and Snapshots

- **Presets** (pane P): a saved look. Type a name and click **Save current look**; click a preset in the list to apply it (one undo step, `Preset: <name>`). A preset holds the sliders, curves, Color Mix, Color Grading and 3D LUT, **not** crop, rotation or masks, and applying one replaces the picture's whole look. Presets live in `%APPDATA%\Literoom\presets` (on Linux `~/.config/literoom/presets`) and work on any picture.
- **Snapshots** (pane S): named copies of **every** edit on one picture (also crop, rotation and masks). Type a name and click **Save snapshot** (or press `Enter`); click one in the list to restore it (one undo step, `Snapshot: <name>`). A picture holds at most 20 snapshots, saved in its saved-edits file.

### 3D LUTs

The **LUTs** pane (L) is a library of `.cube` files. **Import LUT...** copies one in (it is checked first); each LUT in the list shows a preview of your picture; click one to apply it, set its strength with the **LUT Amount** slider, or use **Remove LUT**. The library is in `%APPDATA%\Literoom\luts` (on Linux `~/.config/literoom/luts`) and holds up to 100 LUTs. A LUT that looks made for camera-log footage gets a warning, because Literoom applies LUTs to ordinary sRGB pictures. The picture remembers the LUT by name and by a hash of the file, so a missing or changed file is reported when the picture opens.

### Copy, paste and sync settings

**Copy Settings** (`Ctrl+Shift+C`) remembers the open picture's look (the same contents as a preset); **Paste Settings** (`Ctrl+Shift+V`) applies it to the open picture as one undo step. **Sync Settings to Selected** (`Ctrl+Shift+S`) writes the open picture's **current** look into the saved-edits files of the other pictures selected in the filmstrip, in the background, without opening them (it does not use the copied settings).

### Virtual copies

**Image > Create Virtual Copy** gives the open picture a second set of edits (the copy starts from the current edits and appears right after the picture in the filmstrip with a badge such as `C2`). Nothing is duplicated except the edits: the copy is a small `photo.jpg.vc2.literoom.json` file next to the picture, and up to 9 copies are possible. **Delete Virtual Copy** removes the open copy. (The menu shows `Ctrl+Shift+N` next to Create Virtual Copy, but that key is not bound yet; use the menu.)

### Before / After

- `\` flips between your edited picture and the **Before** version. "Before" keeps your crop and rotation but none of your slider, curve, color or mask edits, so the two views line up exactly.
- `Y` shows a **split view**: Before on the left, After on the right. Click or drag on the picture to move the dividing line.
- While comparing, clicking the picture does not edit anything. Press the same key again to go back.

## 16. Saved edits

Literoom saves your edits in a small text file beside the picture. For `photo.jpg` it is `photo.jpg.literoom.json`. Your photo itself is never modified.

- Saving is automatic: after any change (once you let go of the mouse), when you switch to another picture, and when you close the app.
- The file is written safely (to a temporary file, then renamed), so a crash can't leave a half-written file behind.
- Opening a picture and touching nothing writes nothing.
- Everything is saved: sliders, curves, Color Mix, Color Grading, the 3D LUT (by name), crop and rotation, every mask (including brush strokes, polygons and object selections), the rating, flag and label, and the snapshots. A photo with many brush strokes or object masks therefore has a larger sidecar file.
- If the file is damaged, was written by a newer version, or belongs to a picture with a different size, Literoom ignores it, starts clean, and tells you in the status bar.
- To share your edits with someone, send the `.literoom.json` file along with the photo. To reset a photo completely, close Literoom and delete its `.literoom.json` file.

## 17. Masks (local adjustments)

A mask limits adjustments to part of the picture. Each mask has its own effects (the same Tone, Color and Detail sliders as the Adjust pane, plus Color Mix, Color Grading and a Curve), applied only where the mask covers.

### Basics

1. Open the **Masks** pane (◐), then click one of the **+** buttons to add a mask: **Radial Gradient**, **Linear Gradient**, **Rectangle**, **Polygon / Lasso**, **Brush**, **Luminosity Range**, **Object** or **Color Range**.
2. The new mask appears in the list (up to 16 masks; the status bar tells you when you reach the limit). Click a row to select it. Rows are marked `(inverted)` or `(off)` when that applies.
3. The selected mask's controls appear in a panel on the right, in three tabs: **Effect**, **Shape** and **Parts**.

**Controls for the selected mask**

| Control | What it does |
|---|---|
| **Name field** | Rename the mask |
| **On** | Turns the mask's effect on or off without deleting it |
| **Invert** | Applies the effects everywhere except the mask |
| **Tint** | Paints a colored overlay on the picture so you can see what the mask covers |
| **Duplicate** | Makes a copy of the mask |
| **Move Up** / **Move Down** | Reorders masks. They are applied from top to bottom, so order matters when masks overlap |
| **Copy Effects** / **Paste Effects** | Copies a mask's effects (not its shape) onto another mask |
| **Delete Mask** | Removes the mask (also the `Delete` key) |

### The Effect tab

Four small tabs choose what to edit: **Basic**, **Mix**, **Grade** and **Curve**. A bullet (•) means that section has been changed for this mask.

- **Basic** has an **Amount** slider (100 = full effect, 0 = the mask does nothing) plus the same Tone, Color and Detail sliders as the Adjust pane. **Reset Basic** zeros them.
- **Mix**, **Grade** and **Curve** work just like the global Color Mix, Color Grading and Curve panes, but only inside the mask.

### Mask types (the Shape tab)

- **Radial Gradient:** an ellipse. Drag it on the image to move it and drag its square handles to resize. Sliders: **Width**, **Height**, **Angle**, **Feather** (softness of the edge).
- **Linear Gradient:** a fade across the picture. Drag to move it; its handles resize and also turn it. Sliders: **Length**, **Angle**.
- **Rectangle:** a rotated rectangle with a soft edge. Drag it to move it and drag its handles to resize. Sliders: **Width**, **Height**, **Angle**, **Feather**.
- **Polygon / Lasso:** an outline you draw. Click to place points; click on a side to insert a point there; drag a point to move it, or right-click it to remove it; drag inside the shape to move all of it. Switch on **Freehand lasso** and drag to draw a whole outline in one stroke (it is simplified as you draw). The outline is always closed. Slider: **Feather**.
- **Brush:** drag on the image to paint the mask. Controls: **Erase** (paint away instead), **Size**, **Feather**, **Density**, and **Clear Strokes**. Brush settings apply to the next stroke only; strokes already painted keep their own settings. A brush mask holds up to 512 strokes, and the status bar tells you when you reach that.
- **Luminosity Range:** selects pixels by brightness, with no handles. Sliders: **Min**, **Max**, **Smoothness**. Min cannot pass Max. It looks at the picture as it stands when the mask is reached, before the mask's own edits, so darkening the shadows never changes which pixels count as shadows.
- **Color Range:** selects pixels by color. Click the picture to pick a color (click again to pick another). Sliders: **Range** (how far from that color still counts) and **Smoothness**. Like Luminosity, it has no handles and judges the picture's own colors.
- **Object:** click on the subject to select it (see section 18).

### Combining shapes with Parts

One mask can combine up to 6 shapes ("parts"). On the **Parts** tab, use the **+ Part: ...** buttons to add another shape (Radial, Linear, Rectangle, Polygon, Brush, Luminosity, Object or Color Range). Each part after the first is joined to the ones before it using **Add**, **Subtract** or **Intersect**. Pick a part in the list to edit it, and use **Remove Part** to delete it. For example, a Radial part with a Brush part set to **Subtract** gives you an ellipse with a hand-painted hole.

Masks stay attached to the picture, so they follow your crops, rotations and flips. A mask drawn near an edge that you later crop away comes back when you remove the crop.

## 18. Object selection

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
6. Turn on **Tint** to see the result, then use the mask's effects as usual. **Clear selection** starts over.

The line at the bottom of the Object controls shows which engine is active: `Engine: MobileSAM`, or `Engine: basic color grow (...)` with the reason and the folder it searched for the models. The engine is chosen the first time you use object selection. If you add the model files afterwards, restart the app. Object clicks only work while the Masks pane is open with an Object part selected.

The **Set test map (bright areas)** button is a testing helper that fills the selection with the bright areas of the picture. It does not use the AI model.

## 19. Crop

1. Open the **Crop** pane (C) and **drag on the image** to choose an area. Drag the handles to resize, or drag inside the box to move it.
2. Pick an **Aspect ratio**: **Free**, **Original**, **1:1**, **4:3**, **3:2** or **16:9**. The readout shows the selection size in pixels.
3. Click **Apply Crop** (or press `Enter`).
4. **Reset Selection** restores the full-picture selection. **Remove Crop** brings back the uncropped picture, so a crop can be undone later.

## 20. Rotate and straighten

- **Rotate Left 90°**, **Rotate Right 90°**, **Flip Horizontal**, **Flip Vertical**: lossless turns and mirrors. **Reset Turns & Flips** undoes them.
- **Straighten:** the **Angle** slider (-45° to +45°; positive turns the picture clockwise) levels a tilted horizon. The **-1°**, **-0.1°**, **+0.1°** and **+1°** buttons nudge it precisely. Combined with the 90° buttons, any angle is reachable.
- **Corners:** after straightening, the edges would show empty corners. **Auto-crop empty corners** trims them away; **Keep full canvas** keeps the whole rotated canvas with empty corners.
- **Reset Angle** returns to 0°.
- Straightening clears any crop you applied (undo brings it back), so crop after straightening.

## 21. Export

Open the **Export** pane (E), or press `Ctrl+E`.

- Choose **PNG (lossless)**, **PNG 16-bit** or **JPEG**. This only decides which format the save dialog offers first. 16-bit PNG keeps smooth gradients through heavy edits (larger files, and not every program opens them).
- For JPEG, set the **Quality** (1 to 100) and choose whether to **Keep EXIF & color profile**. With it on, the camera information of the original photo is carried into the exported file (with the orientation reset to upright, since the pixels are already turned). **Remove location (GPS)** cuts the GPS location out of that EXIF block. The picture's ICC color profile is carried into a JPEG whether or not this option is on, so colors look right in other programs.
- **Resize to long edge** shrinks the export so that its long side is the **Long edge** value (it never enlarges); the pane shows the size that will be written.
- Click **Export...** and pick a file name. **The file name decides the format**: `.jpg` / `.jpeg` writes a JPEG and `.png` writes a PNG. If you type a name with no usable extension, the pane's format is used.
- **Batch export:** `File > Export Selected...` (`Ctrl+Shift+E`) or `Export All Shown...` writes every picked filmstrip picture (`Ctrl+click`, `Shift+click`) into one folder, each with its own saved edits, named `<picture>-edit.jpg` (or `.png`; a virtual copy is `<picture>-copy2-edit.jpg`). **Overwrite existing files** decides what happens to a name that is already taken (otherwise `-2`, `-3`, ... is added). **Stop Export** (also in the File menu) drops the pictures still waiting and abandons the one being rendered. A picture whose saved edits can't be used is skipped, and the status bar says why.

The export contains the picture exactly as you see it, with every edit, crop and rotation applied. The original file is never overwritten unless you choose the same name.

Good to know:

- **Transparent corners.** After Straighten with **Keep full canvas**, the empty corners are transparent. PNG keeps that transparency; JPEG has no transparency, so the corners become **white**.
- **JPEG quality 90 and above** keeps full color detail (no chroma subsampling), which helps colored edges stay crisp.
- **Privacy: GPS location.** *Keep EXIF & color profile* copies the photo's EXIF block, and that **includes GPS location** if the camera recorded it, unless **Remove location (GPS)** is on. Turn the EXIF option off (or remove the location) before sharing a picture whose location you don't want to reveal. XMP and IPTC data (captions, copyright fields) are never copied.
- **PNG exports** do not carry EXIF. They do carry the color-space tag of the original (an embedded profile or an sRGB tag), and an original with no tag stays untagged. A PNG original's profile cannot be carried into a JPEG export; the status bar says so when the original was tagged as something other than sRGB.

## 22. Usage troubleshooting

| Symptom | What to do |
|---|---|
| Nothing happens when I click the image in the Masks pane | A mask must be selected, and for Object selection an Object part must be selected. |
| Object clicks select a rough color region instead of the subject | The Engine line says `basic color grow`. Fix the models (section 6), then restart the app. |
| Status bar says "Object selection unavailable: ..." | The error text names the problem. Common causes are missing model files or a wrong model export. |
| The first object click is slow | Normal: the image is analyzed once. Later clicks on the same image are fast. |
| Slider feels laggy on a very large photo | Expected: a quick preview is shown while dragging and the full render follows on release. |
| Clicking the picture does nothing | You may be in Before/After or split view. Press `\` or `Y` to leave it. |
| A shortcut does nothing | Release any slider or crop handle you are holding. Plain-key shortcuts are also ignored while a text field has focus. |
| "Export failed: JPEG can't hold a picture this size" | JPEG has a maximum side length. Export as PNG instead. |
| Status bar says the saved edits were ignored | The `.literoom.json` file was damaged, from a newer version, or for a different-sized picture. Literoom started clean; the old file is replaced the next time you edit. |

---

# Part 3: For developers

The code is organized around one idea: **the engine and the UI are separate.**

- The **engine** is a set of header-only files (`*.hpp`) under `src/` that describe edits and turn them into pixels. They have no window code, and each has its own unit test.
- **`src/main.cpp`** is the UI shell: the window, the panes, the canvas, and the glue to the engine.
- **LiteUI** provides the window, widgets, canvas drawing and image decoding.

The picture goes through a chain, each step built from the one before:

`original → oriented → rotated → source (cropped) → image (adjusted)`

Edits are applied in a fixed order: **Tone** (one lookup table), then **Color** (one float pass: the color sliders, Color Mix, Color Grading, Filters and the 3D LUT), then **Detail** (neighbor-pixel effects), then **masks**, and last **Vignette and Grain**. A 16-bit PNG keeps a 16-bit twin of the chain, so heavy edits do not show steps. Slow work runs on background threads that only ever use "newest request wins".

### Source layout

Everything lives under `src/`, and `src/` is the include root: a header is always included by its path from `src/`, wherever the including file sits, for example `#include "mask/mask.hpp"`.

| Folder | What is in it | Headers |
|---|---|---|
| `core/` | The shared vocabulary everything else builds on | `pixel_math`, `geometry`, `adjustments`, `auto_tone`, `tone_table`, `raster_ops`, `image16`, `image16_convert`, `wide_chain` |
| `color/` | Color math, tone curves and 3D LUTs | `color_mix`, `color_grade`, `color_pass`, `curves`, `lut3d` |
| `detail/` | Effects that look at neighboring pixels, and Vignette / Grain | `detail_layer`, `neighbor_fx`, `film_fx` |
| `mask/` | Mask shapes, brush strokes, polygons and object selection | `mask`, `mask_geom`, `mask_rect`, `mask_polygon`, `mask_range`, `mask_raster`, `mask_brush`, `mask_object`, `seg_basic`, `sam_segmenter` |
| `edit/` | The edit state (values, undo history) and the frozen snapshot the renderer reads | `adjustment_set`, `render_params` |
| `render/` | The pixel pass, the histogram and the scopes | `render`, `render_core`, `render_bytes`, `histogram`, `scopes` |
| `io/` | Saved edits, JSON, files, EXIF, color tags, presets, the LUT library, image decoding and encoding | `sidecar`, `minijson`, `photo_meta`, `preset`, `virtual_copy`, `file_utils`, `image_codec`, `image_codec16`, `image_source`, `exif`, `image_meta`, `color_tag`, `lut_cube`, `lut_library` |
| `workers/` | Background threads | `latest_worker`, `thumb_worker`, `export_render`, `export_worker`, `lut_preview_worker` |

The folders form layers, listed here from lowest to highest: `core`, then `color`, `detail` and `mask`, then `io`, then `edit`, then `render`. `workers` sits beside `render` and uses `core` and `io`. **A folder may include headers from folders below it, never from folders above it** (with a few documented exceptions, listed in [docs/headers.md](docs/headers.md#where-things-live)). If a build fails because `core/` includes something from `mask/`, a header is in the wrong place.

## 23. Documentation for developers

| Page | What it covers |
|---|---|
| [docs/architecture.md](docs/architecture.md) | How the pieces fit together and why: the picture chain, the render order, the preview path, geometry, masks. Start here. |
| [docs/headers.md](docs/headers.md) | One entry for every engine header: its purpose, key ideas, limits and dependencies. |
| [docs/threading.md](docs/threading.md) | The "latest wins" worker, the eight workers, and the rules that keep threads safe. |
| [docs/undo-and-history.md](docs/undo-and-history.md) | Snapshots, how a slider drag becomes one undo step, and how History labels are made. |
| [docs/sidecar-format.md](docs/sidecar-format.md) | The saved-edits file: what is in it, when it is written, and how bad files are handled. |
| [docs/extending.md](docs/extending.md) | Step-by-step guides: add a slider, add a mask type. |
| [docs/testing.md](docs/testing.md) | The Catch2 unit tests, thread tests and sanitizers, the golden render test and the LUT-pane test. |

## 24. Running the tests

The tests are built and run from the **liteui root** (literoom's `CMakeLists.txt` refuses to configure on its own). They are on by default (`LITEUI_BUILD_TESTS`); `-DLITEROOM_TESTS=OFF` leaves literoom's out.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`literoom_test` is one Catch2 program holding a test file per header (`tests/cases/<header>.inc`); each case is its own CTest entry named `literoom_<case name>`, so `ctest -R mask_brush` runs one file's cases, and the timing-sensitive worker cases are labelled `threaded` (`ctest -L threaded --repeat until-fail:50`). Two stand-alone programs sit beside it: the golden render test and the LUT-pane test (`literoom_lut_pane`).

Before refactoring `src/main.cpp`, record the golden render reference once, then check after every step:

```powershell
cmake --build build --config Release --target literoom_golden_record
ctest --test-dir build -C Release -R literoom_golden_render
```

The golden test compiles `src/main.cpp` itself (with `main()` renamed), because everything in it is static and that is the only way to reach it. The LUT-pane test does the same. That is why those test targets have `src/` on their include path. There is one golden file per compiler and configuration; until one exists the golden test reports "skipped" (`ctest -LE golden` leaves it out).

For AddressSanitizer runs use the presets `msvc-debug-asan` or `gcc-debug-asan` (`cmake --preset gcc-debug-asan`, then build and `ctest --preset gcc-debug-asan`).

See [docs/testing.md](docs/testing.md) for what each kind of test is for.