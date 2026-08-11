# Auto Mask

An OFX plugin for DaVinci Resolve that splits a frame into its discrete
elements by pixel value, lets you click the ones you want in the viewer, and
masks the rest.

Give it a PNG with a few things on it — icons, logos, shapes, a title — and it
finds each separate piece. Click one to select it. Click it again to drop it.
Invert to keep everything else. A small transform section then moves, scales or
rotates just what you selected, composited back into the frame.

---

## How it works

Everything comes from the pixels. There is no metadata, no AI, no model:

1. **Threshold** the frame into a binary mask — on alpha, or on luminance for
   opaque artwork on a flat background.
2. **Connected-component labeling**, 8-connectivity, two-pass with union-find.
   Each blob of touching pixels becomes one element.
3. **Despeckle** — anything under *Min Element Size* is antialiasing debris,
   not an element.
4. Your clicks pick which labels survive into the mask.

The segmentation is the same machinery
[resolve-text-animator](../resolve-text-animator) uses to find letters, with
the typography layer (slant estimation, glyph assembly, word-gap statistics)
stripped off. An element here is a connected component and nothing more.

### Selection survives re-segmentation

A click stores **where** you clicked, as a fraction of the frame, in the
`Selected Points` string. Every render asks the current segmentation which
element now contains that point.

That matters. Storing element *numbers* would be simpler and would break
constantly: components are numbered in raster order, so nudging Threshold,
changing Min Element Size, or an element merely moving between frames
renumbers everything and the selection silently jumps to different objects.
Points also make the selection resolution-independent for free, and let it
follow an element that moves.

A point that stops matching anything is **kept**, not deleted, and drawn as a
red X in the viewer. Dragging Threshold too far must not destroy a selection
you spent a minute building — dragging it back has to bring the selection back
with it.

---

## Using it

Apply **Auto Mask** (group: `AutoMask`) from the OpenFX library.

> Set the viewer's on-screen-control dropdown to **Open FX Overlay**. Resolve
> hides OFX overlays until you do, and nothing plugin-side can work around it.

| Gesture | Effect |
|---|---|
| Click an element | Select it |
| Click a selected element | Deselect it |
| Shift-click | Add only, never toggle |
| Alt-click | Remove the nearest selection point, even if it matches nothing |
| Click empty background | Nothing — the click passes through to the host |

Turn on **Show All Elements** to outline everything the current settings found,
with index numbers. It is viewer-only and costs no re-render, so it can be left
on while tuning.

### Controls

**Segmentation** — `Segment By` (Alpha / Luminance), `Threshold`,
`Invert Source` (Luminance only), `Min Element Size`, `Bridge Gaps`.

*Bridge Gaps* grows the artwork before splitting it, so pieces that nearly
touch count as one element. Use it when a dotted outline or a hairline join
shatters into fragments. Bounding boxes are unaffected — the growth only
decides what joins to what.

**Mask** — `Output`, `Invert Mask`, `Expand / Contract`, `Feather`.

| Output | What you get |
|---|---|
| **Preview** *(default)* | Selection at full strength over a dimmed copy of the rest |
| Matte | Black-and-white mask, to drive something downstream |
| Cutout | Original colour, everything unselected knocked out |
| Cutout (Premultiplied) | The same, already multiplied by the mask |
| Composite | The whole frame, with the selection moved within it |

**Preview is the default on purpose.** Every other mode hides what is not
selected, so on a fresh instance — nothing selected yet — they all show a blank
frame and there is nothing to aim at. It is a mode rather than a hidden
checkbox precisely so the Output control always says what you are looking at:
a preview toggle you can forget is on is worse than no preview at all.

**Transform** — `Position`, `Scale` / `Scale Y` / `Link Scale`, `Rotation`,
`Anchor`, `Anchor to Selection`, `Auto Center`, `Filter`. Set Output to
**Composite** to see the selection move within the picture rather than on its
own.

Two buttons worth knowing about:

- *Anchor to Selection* — the default anchor is the centre of frame, which is
  almost never the right pivot for rotating one element.
- *Auto Center* — sets Position so the selection sits in the middle of frame.
  It measures the element **as it currently appears**, so it is correct with a
  scale and a rotation already applied, and it writes Position only.

---

## Building

```bash
powershell -File scripts\build.ps1 -Config Release
```

Requires Visual Studio with the C++ toolset. The script uses the CMake bundled
inside VS, because the standalone CMake on PATH may not know the installed
generator.

```bash
ctest --test-dir build -C Release --output-on-failure
```

Then install (self-elevating; Resolve must be restarted afterwards, since it
scans for OFX plugins only at launch):

```bash
powershell -File scripts\install.ps1 -Config Release
```

Or double-click `scripts\Install-AutoMask.cmd`, which builds first.

### The offline harness

`masktest` links the same `src/core` the plugin does, with no host in the loop.
This is where Threshold, Bridge Gaps and Min Element Size get tuned, because
here a guess costs a second and inside Resolve it costs a relaunch.

```bash
build\Release\masktest.exe tests\data\three_shapes.png out.png --diag
```

`--diag` false-colours the label image and dots each element's seed pixel, so
"did that split correctly" is a glance rather than an inference. `--stats`
prints areas and bounding boxes. Full option list is at the top of
`tools/masktest.cpp`.

---

## Notes for anyone changing this

**`src/core` includes no OFX header, and must not start.** That is the only
reason the harness can link the identical code Resolve runs.

**The composite algebra is load-bearing.** `Composite.h` recombines the moved
element with the background using *hole-aware over*:

```cpp
const float occl = clamp01(Aw - M);
out = Ew + B * (1.0f - occl);
```

The obvious formulation, `Ew + B*(1-Aw)`, is wrong at identity: substituting
`Aw = M` into `B = S*(1-M)` gives `S*(M + (1-M)²)`, an error of `-0.25*S` at
`M = 0.5`. That reads as a dark ring along every feathered edge with the
transform doing nothing at all. The `identity_composite` test exists to catch
exactly this, and it runs *with* a feather because a hard mask hides the bug.

**Auto Center scans pixels rather than transforming a bounding box.** The
obvious implementation solves `T(c) == centre` for the bounding-box centre `c`.
That is right only while the transform is axis-aligned: rotate the element and
the image of the old box centre is no longer the centre of the new box — for a
triangle at 35° it is off by tens of pixels, which reads as a button that
simply does not work. `core/AutoCenter.h` measures the transformed extent of
the actual selected pixels instead. The `auto_center_transformed` test exists
because `auto_center` alone cannot tell the two apart, and caught exactly this.

**There is no CUDA path, on purpose.** Labeling is inherently serial and is
cached per content-hash, so it runs once per content change rather than once
per frame. The per-frame work is a handful of O(n) passes. The real cost in a
GPU-resident host is the frame download, and that is paid regardless because
the labeling lives on the CPU — porting only the blur and the warp would remove
exactly zero transfers. The CMake block is kept live behind
`option(AUTOMASK_CUDA)` so turning it on later is a flag, not archaeology.

**Tiling is off and must stay off.** Labeling a tile in isolation shatters
every element that crosses its boundary.

`%LOCALAPPDATA%\AutoMask\probe.log` records what the host actually reports,
once per process.
