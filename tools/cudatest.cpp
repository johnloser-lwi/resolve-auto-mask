// CPU/GPU parity.
//
// A masktest render is NOT evidence the GPU agrees: it runs the CPU shader. In
// Resolve every frame goes through the kernel, so this is the only thing
// standing between a reassociated float and a bug nobody sees until it ships.
//
// It links the same compiled kernel the plugin does -- that is why the CUDA
// code is its own static library rather than being compiled into each target.
// Two builds of "the same" kernel is precisely the situation this exists to
// rule out.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/Arrange.h"
#include "core/Composite.h"
#include "core/ElementLayer.h"
#include "core/MaskBuild.h"
#include "core/Segmentation.h"
#include "core/Selection.h"
#include "render/CudaRender.h"

namespace {

struct Case {
  const char* name;
  int outputMode;
  int bgMode;
  int expand;
  float feather;
  bool invert;
  bool transform;
  int arrangeMode = am::kArrangeNone;  ///< non-None exercises the layered path
};

// A synthetic frame with soft edges, holes and elements running off the border.
// Hard-edged blocks would agree trivially; the disagreements live in the
// fractional coverage at an edge and in the clamped border of a blur.
void buildSource(std::vector<float>& px, std::vector<std::int32_t>& labels, int w, int h) {
  px.assign(std::size_t(w) * h * 4, 0.0f);
  labels.assign(std::size_t(w) * h, 0);

  auto disc = [&](float cx, float cy, float r, int label, float cr, float cg, float cb) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
        const float d = std::sqrt(dx * dx + dy * dy);
        const float a = std::min(std::max(r - d, 0.0f), 1.0f);  // one-pixel soft edge
        if (a <= 0.0f) continue;
        const std::size_t i = std::size_t(y) * w + x;
        px[i * 4 + 0] = cr * a;
        px[i * 4 + 1] = cg * a;
        px[i * 4 + 2] = cb * a;
        px[i * 4 + 3] = a;
        if (a > 0.5f) labels[i] = std::int32_t(label);
      }
  };

  disc(w * 0.30f, h * 0.40f, h * 0.22f, 1, 0.9f, 0.2f, 0.25f);
  disc(w * 0.68f, h * 0.55f, h * 0.18f, 2, 0.2f, 0.5f, 0.95f);
  disc(w * 0.05f, h * 0.05f, h * 0.15f, 3, 0.95f, 0.85f, 0.1f);  // clipped by the border
}

int runCase(const Case& c, int w, int h, const std::vector<float>& src,
            const std::vector<std::int32_t>& labels, const std::vector<std::uint8_t>& selected,
            int labelCount) {
  am::Segmentation seg;
  seg.width = w;
  seg.height = h;
  seg.labelImage = labels;
  seg.components.assign(std::size_t(labelCount) + 1, am::Component{});

  am::Resolved sel;
  sel.labelSelected = selected;

  am::MaskParams mp;
  mp.expand = c.expand;
  mp.feather = c.feather;
  mp.invert = c.invert;

  am::CompositeParams cp;
  cp.outputMode = c.outputMode;
  cp.backgroundMode = c.bgMode;
  cp.filter = am::kFilterBilinear;
  cp.identity = !c.transform;
  if (c.transform)
    cp.inv = mtx::Invert(
        am::MakeElementTransform(float(w), float(h), 0.12, -0.08, 1.35, 0.8, 22.0, 0.4, 0.6));

  std::vector<float> mask;
  am::BuildMask(seg, sel, mp, mask);

  std::vector<float> cpu(std::size_t(w) * h * 4, 0.0f);
  const am::PlaneView pv{mask.data(), w, h};
  const am::ImageView sv{src.data(), w, h, std::ptrdiff_t(w) * 4};
  std::vector<float> gpu(std::size_t(w) * h * 4, 0.0f);

  if (c.arrangeMode != am::kArrangeNone) {
    // The layered path. The layer is built once on the host and fed to both
    // sides, so what is under test is the combine kernel rather than the
    // element scatter -- which never runs on the device.
    am::ArrangeParams ap;
    ap.mode = c.arrangeMode;
    ap.spread = 1.8;
    ap.stepX = 0.05;
    ap.stepY = -0.03;
    ap.alignV = am::kAlignMiddle;

    const std::vector<am::Placement> pl =
        am::ComputeArrangement(seg, sel, ap, float(w), float(h));
    am::ElementLayer layer;
    am::BuildElementLayer(sv, seg, pl, mp, am::kFilterBilinear, layer);

    const am::ImageView lc{layer.colour.data(), w, h, std::ptrdiff_t(w) * 4};
    const am::PlaneView la{layer.coverage.data(), w, h};
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        am::RenderPixelLayered(sv, pv, lc, la, cp, x, y, &cpu[(std::size_t(y) * w + x) * 4]);

    if (!am::CudaRenderLayeredHostSync(src.data(), gpu.data(), labels.data(), selected.data(),
                                       labelCount, w, h, c.expand, c.feather, c.invert,
                                       layer.colour.data(), layer.coverage.data(), cp)) {
      std::fprintf(stderr, "  %-28s CUDA call FAILED\n", c.name);
      return 1;
    }
  } else {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        am::RenderPixel(sv, pv, cp, x, y, &cpu[(std::size_t(y) * w + x) * 4]);

    if (!am::CudaRenderHostSync(src.data(), gpu.data(), labels.data(), selected.data(),
                                labelCount, w, h, c.expand, c.feather, c.invert, cp)) {
      std::fprintf(stderr, "  %-28s CUDA call FAILED\n", c.name);
      return 1;
    }
  }

  double worst = 0.0;
  std::size_t worstAt = 0;
  for (std::size_t i = 0; i < cpu.size(); ++i) {
    const double d = std::fabs(double(cpu[i]) - double(gpu[i]));
    if (d > worst) {
      worst = d;
      worstAt = i;
    }
  }

  // Not exact equality. nvcc may contract a multiply and an add into an FMA
  // where MSVC does not, which changes the last bit or two -- a real difference,
  // and an acceptable one. What this threshold is guarding against is a
  // different ALGORITHM on one side: a wrong edge rule, a missed invert, a blur
  // radius off by one. Those miss by percent, not by 1e-5.
  // 2e-4 is ~1/20 of an 8-bit code value: far below anything visible, and three
  // to four orders below what a genuine algorithmic divergence produces. When
  // the device could not read the source image at all, this test reported 1.0.
  const double kTol = 2e-4;
  std::printf("  %-28s max diff %.3e  @ px %zu  %s\n", c.name, worst, worstAt / 4,
              worst <= kTol ? "ok" : "MISMATCH");
  return worst <= kTol ? 0 : 1;
}

}  // namespace

int main() {
  if (!am::CudaAvailable()) {
    // Not a failure. A build machine without a GPU should not fail the suite;
    // it simply cannot answer the question.
    std::printf("cudatest: no CUDA device, skipping\n");
    return 0;
  }

  const int w = 640, h = 360;
  std::vector<float> src;
  std::vector<std::int32_t> labels;
  buildSource(src, labels, w, h);

  const int labelCount = 3;
  const std::vector<std::uint8_t> selected = {0, 1, 0, 1};  // elements 1 and 3

  const Case cases[] = {
      {"matte, plain", am::kOutMatte, am::kBgRemoved, 0, 0.0f, false, false},
      {"matte, feathered", am::kOutMatte, am::kBgRemoved, 0, 12.0f, false, false},
      {"matte, expand+feather", am::kOutMatte, am::kBgRemoved, 6, 9.0f, false, false},
      {"matte, contract", am::kOutMatte, am::kBgRemoved, -5, 0.0f, false, false},
      {"matte, inverted", am::kOutMatte, am::kBgRemoved, 3, 7.0f, true, false},
      {"cutout", am::kOutCutout, am::kBgRemoved, 0, 5.0f, false, false},
      {"cutout premultiplied", am::kOutCutoutPM, am::kBgRemoved, 0, 5.0f, false, false},
      {"preview", am::kOutPreview, am::kBgRemoved, 0, 4.0f, false, false},
      {"composite, identity", am::kOutComposite, am::kBgRemoved, 0, 8.0f, false, false},
      {"composite, left in place", am::kOutComposite, am::kBgLeft, 0, 8.0f, false, false},
      {"matte, transformed", am::kOutMatte, am::kBgRemoved, 0, 6.0f, false, true},
      {"cutout, transformed", am::kOutCutout, am::kBgRemoved, 4, 6.0f, false, true},
      {"composite, transformed", am::kOutComposite, am::kBgRemoved, 0, 6.0f, false, true},
      {"preview, transformed", am::kOutPreview, am::kBgRemoved, 2, 3.0f, false, true},

      // The layered path, which reads a prebuilt element layer instead of
      // warping the source. Every output mode again, because each one consumes
      // the layer differently -- Cutout in particular has to unpremultiply it.
      {"layered matte", am::kOutMatte, am::kBgRemoved, 0, 6.0f, false, false,
       am::kArrangeSpread},
      {"layered cutout", am::kOutCutout, am::kBgRemoved, 0, 6.0f, false, false,
       am::kArrangeSpread},
      {"layered cutout premult", am::kOutCutoutPM, am::kBgRemoved, 3, 6.0f, false, false,
       am::kArrangeStagger},
      {"layered preview", am::kOutPreview, am::kBgRemoved, 0, 4.0f, false, false,
       am::kArrangeStagger},
      {"layered composite", am::kOutComposite, am::kBgRemoved, 0, 8.0f, false, false,
       am::kArrangeSpread},
      {"layered composite, left", am::kOutComposite, am::kBgLeft, 0, 5.0f, false, false,
       am::kArrangeAlign},
      {"layered + transform", am::kOutComposite, am::kBgRemoved, 2, 5.0f, false, true,
       am::kArrangeSpread},
  };

  int failures = 0;
  std::printf("CPU/GPU parity, %dx%d:\n", w, h);
  for (const Case& c : cases) failures += runCase(c, w, h, src, labels, selected, labelCount);

  std::printf("%s\n", failures ? "PARITY FAILED" : "all cases agree");

  // Per-frame device cost at UHD, in the steady state the plugin runs in:
  // labels already resident, mask rebuilt, frame composited. No host transfers,
  // because on the CUDA path there are none.
  {
    am::CompositeParams cp;
    cp.outputMode = am::kOutComposite;
    cp.identity = false;
    cp.inv = mtx::Invert(am::MakeElementTransform(3840.0f, 2160.0f, 0.1, 0.05, 1.0, 1.0, 10.0,
                                                  0.5, 0.5));
    double heavy = 0.0, light = 0.0;
    std::printf("\nUHD device time:\n");
    if (am::CudaBench(3840, 2160, 8, 20.0f, false, cp, 50, heavy))
      std::printf("  mask rebuild + composite  %5.2f ms  (%3.0f fps)\n", heavy, 1000.0 / heavy);
    // The plugin caches the mask, so this is the steady state while scrubbing
    // or playing with the selection settled -- which is most of the time.
    if (am::CudaBench(3840, 2160, 0, 0.0f, false, cp, 50, light))
      std::printf("  composite only            %5.2f ms  (%3.0f fps)\n", light, 1000.0 / light);
  }

  return failures ? 1 : 0;
}
