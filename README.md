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
Points also make the selection resolution-independent for free.

They do **not** track motion, and it is worth being clear about that: an anchor
is a fixed point, so it keeps selecting an element only while the element still
covers it. On a static graphic that is always. On moving footage the element
eventually slides off its anchor and the selection drops out — the anchor turns
into a red X rather than following. Making anchors stick to their element
across frames is a real feature, and it is not implemented.

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

**Arrange** — `Order`, `Reverse Order`, `Align Horizontal`, `Align Vertical`,
`Spread` (X/Y), `Step` (X/Y).

This moves the selected elements relative to *each other*, before the Transform
above moves them as a group. The three operations **compose**, and apply in a
fixed order — Align, then Spread, then Step:

- *Align* snaps them to a common edge or centre, per axis.
- *Spread* pushes them apart from their shared centre, per axis. 1.0 is no
  movement, 2.0 doubles every gap, below 1.0 pulls them together.
- *Step* moves each element one further increment than the one before it, in
  `Order`. The first element does not move.

There is no mode selector: each operation has a neutral default, so "off" needs
no separate way to say it. That composability is the point — a left-aligned
vertical column is Align Left plus a Y Step, and an exclusive mode cannot
express it.

**Spread is multiplicative, and that has a consequence.** It scales each
element's distance from the centre, so it cannot separate elements that already
sit on top of one another — twice nothing is still nothing. Left-align a row and
their vertical positions collapse together; no amount of vertical Spread will
part them again. `Step` is the control for that. The trade buys a number that
means the same thing whatever the original spacing was, which a distance does
not.

`Order` decides which element counts as first — position, size, reading order,
or distance from the centre — and also which element sits on top where two
overlap.

Arrange is unavailable while `Invert Mask` is on. An inverted mask is a
statement about the whole frame, so there are no individual elements to move.

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

**Performance: measure before optimising.** `masktest --bench N` times each
stage separately, which is the only useful way to look at this — the stages
have very different caching behaviour, so one end-to-end number tells you
nothing about what to attack.

Measured at UHD, 24 threads, feather 20 + expand 8 + a transform:

| Stage | Cost | Re-runs when |
|---|---|---|
| `HashSource` | 1 ms | every frame |
| `Segment` | 36 ms | the source pixels change |
| `BuildMask` | 50 ms | segmentation, selection or mask controls change |
| `RenderPixel` | 34 ms | every frame |

Two things came out of taking that measurement, both worth not undoing:

*The separable passes transpose rather than stride.* Walking a column of a UHD
plane strides 3840 floats per step — a cache miss per sample. The blur cost
294 ms that way against 50 ms via two blocked transposes. `Blur.h` and
`Dilate` both do the vertical pass as rows of the transpose for this reason.

*The mask is cached separately from the analysis*, because its dependencies are
strictly narrower than the frame's. Scrubbing or playing a still graphic
re-derived an identical 50 ms mask on every frame otherwise.

**The CPU cannot reach UHD60, and no amount of tuning changes that.** Pure
passthrough at UHD — read the frame, write it, no feather, no transform —
measures 14.8 ms on 24 threads. That is 66 MB of traffic at memory bandwidth. A
60 fps budget is 16.7 ms, so the CPU is at the wall *before doing any work*.
This is a bandwidth limit, not an algorithm one, which is why the CUDA path
exists despite the labelling staying on the CPU.

Device time at UHD, measured by `cudatest`:

| | ms/frame | fps |
|---|---|---|
| mask rebuild + composite | 8.1 | 124 |
| composite only (mask cached — the steady state) | 0.62 | 1600 |

`CudaHashSource` is what makes this work. The labelling still runs on the CPU
and still needs host pixels, so a naive GPU path would download the frame every
frame and give most of the win straight back. Instead the content fingerprint
is computed *on the device* and read back as eight bytes, so the 33 MB moves
only when the content actually changed. It is deliberately not the same
function as the host's `HashSource`: FNV-1a is a sequential chain and replaying
its exact order across thousands of threads costs more than it saves, so the
device version uses an order-independent combine. The two are never compared
with each other.

**Arrange switches the composite from one pass to a layered one.** With a
single transform every destination pixel comes from exactly one place, so the
frame can be inverse-warped in one go. Once elements move independently that
stops being true — a pixel could come from any of them, or from two that now
overlap — so `ElementLayer.h` warps each element and composites it over the
ones already laid down. Feather becomes per-element too: blurring the combined
mask and *then* moving pieces of it drags each element's neighbours along in
its soft edge, which reads as a ghost of the old layout clinging to everything.

Two consequences worth not undoing:

*Settings that mean "no arrangement" must reach the single-pass path.* Spread
at 1.0, a zero Stagger step, Align with both axes off — `ArrangementMoves`
checks the computed offsets rather than the mode, and `arrange_identity` tests
it. This is not about speed: per-element feathering makes the coverages overlap
slightly, so their sum is no longer exactly the combined mask, and only the
single-pass path is exact at identity.

*The layer is NOT clipped to the frame.* Arrange can push an element past the
edge, and the global transform runs afterwards — so a frame-sized layer would
destroy that element before the transform ever got the chance to bring it back,
and scaling down would recover nothing. `VisibleSourceRegion` inverse-maps the
frame corners through the transform to size the layer to exactly what is
reachable: nothing visible is thrown away, nothing invisible is paid for, and a
transform far enough out is capped at 3× the frame. `arrange_survives_transform`
counts surviving opaque pixels rather than comparing pictures, because the
clipped result looked entirely plausible — just missing a shape.

*The layer is built in arranged space, without the global transform.*
Composition is associative, so warping the finished layer gives the same pixels
as warping each element — but it makes the layer independent of Position, Scale
and Rotation. Dragging Position re-warps a cached layer instead of rebuilding
one, and on the GPU path it is the difference between uploading 41 MB per frame
and uploading it once. The build itself is ~54 ms for 24 elements at UHD and
runs on the CPU in both paths; the device scatter it would replace is one
kernel per element with sequential ordering, which is not obviously faster.

**`cuda_parity` is not optional, and it has already earned its place.** A
`masktest` render proves nothing about the GPU — it runs the CPU shader. The
first run of the parity test failed 9 of 14 cases with a max difference of
exactly 1.0, and the pattern was the tell: every Matte case was bit-exact and
every case that touched the *source image* was empty. `PlaneView::at` was
marked `__host__ __device__` and `ImageView::at` was not, so the kernel
compiled, ran, and silently read nothing. Nothing else in the suite would have
caught it. Untransformed cases now agree bit-exactly; transformed ones differ by
~1e-5 where nvcc contracts a multiply-add the host compiler does not.

The kernel is deliberately built as its own static library shared by the plugin
and the test, so both link the *same compiled kernel* rather than two builds of
"the same" code — which is precisely the situation a parity test exists to rule
out. For the same reason it is compiled without `--use_fast_math`.

**Tiling is off and must stay off.** Labeling a tile in isolation shatters
every element that crosses its boundary.

`%LOCALAPPDATA%\AutoMask\probe.log` records what the host actually reports,
once per process.
