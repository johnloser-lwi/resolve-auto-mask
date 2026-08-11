// Offline harness. Runs the exact code Resolve runs, with no host in the loop.
//
// This is where Threshold, Bridge Gaps and Min Element Size get tuned, because
// here a guess costs a second and inside Resolve it costs a relaunch. It is
// also where the composite algebra is pinned down by tests, since a dark ring
// along a feathered edge is a lot easier to measure than to notice.

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/AutoCenter.h"
#include "core/Composite.h"
#include "core/MaskBuild.h"
#include "core/Segmentation.h"
#include "core/Selection.h"

namespace {

struct Options {
  std::string inPath, outPath;
  am::DetectParams det;
  am::MaskParams mask;
  std::string select;
  bool selectAll = false;
  int outputMode = am::kOutMatte;
  int bgMode = am::kBgRemoved;
  am::FilterMode filter = am::kFilterBilinear;
  float posX = 0.0f, posY = 0.0f;
  float scaleX = 1.0f, scaleY = 1.0f;
  float rot = 0.0f;
  float anchorX = 0.5f, anchorY = 0.5f;
  bool diag = false;
  bool stats = false;
  int expect = -1;
  bool identityCheck = false;
  bool autoCenter = false;
  bool centerCheck = false;
};

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "masktest: %s\n", msg.c_str());
  std::exit(2);
}

bool eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// "1.5" or "1.5,2.0"
void parsePair(const char* s, float& a, float& b) {
  char* end = nullptr;
  a = float(std::strtod(s, &end));
  b = a;
  if (end && *end == ',') b = float(std::strtod(end + 1, nullptr));
}

Options parseArgs(int argc, char** argv) {
  Options o;
  int positional = 0;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) die(std::string("missing value after ") + a);
      return argv[++i];
    };
    if (a[0] != '-') {
      if (positional == 0) o.inPath = a;
      else if (positional == 1) o.outPath = a;
      else die("too many positional arguments");
      ++positional;
    } else if (eq(a, "--mode")) {
      const char* v = next();
      if (eq(v, "alpha")) o.det.mode = am::SegmentMode::Alpha;
      else if (eq(v, "luma")) o.det.mode = am::SegmentMode::Luminance;
      else die("--mode must be alpha or luma");
    } else if (eq(a, "--threshold")) {
      o.det.threshold = float(std::atof(next()));
    } else if (eq(a, "--invert-source")) {
      o.det.invertSource = true;
    } else if (eq(a, "--min-size")) {
      o.det.minElementSize = std::atoi(next());
    } else if (eq(a, "--bridge")) {
      o.det.bridgeRadius = std::atoi(next());
    } else if (eq(a, "--select")) {
      o.select = next();
    } else if (eq(a, "--select-all")) {
      o.selectAll = true;
    } else if (eq(a, "--output")) {
      const char* v = next();
      if (eq(v, "matte")) o.outputMode = am::kOutMatte;
      else if (eq(v, "cutout")) o.outputMode = am::kOutCutout;
      else if (eq(v, "cutoutpm")) o.outputMode = am::kOutCutoutPM;
      else if (eq(v, "composite")) o.outputMode = am::kOutComposite;
      else if (eq(v, "preview")) o.outputMode = am::kOutPreview;
      else die("--output must be matte|cutout|cutoutpm|composite|preview");
    } else if (eq(a, "--leave-original")) {
      o.bgMode = am::kBgLeft;
    } else if (eq(a, "--invert-mask")) {
      o.mask.invert = true;
    } else if (eq(a, "--expand")) {
      o.mask.expand = std::atoi(next());
    } else if (eq(a, "--feather")) {
      o.mask.feather = float(std::atof(next()));
    } else if (eq(a, "--pos")) {
      parsePair(next(), o.posX, o.posY);
    } else if (eq(a, "--scale")) {
      parsePair(next(), o.scaleX, o.scaleY);
    } else if (eq(a, "--rot")) {
      o.rot = float(std::atof(next()));
    } else if (eq(a, "--anchor")) {
      parsePair(next(), o.anchorX, o.anchorY);
    } else if (eq(a, "--nearest")) {
      o.filter = am::kFilterNearest;
    } else if (eq(a, "--diag")) {
      o.diag = true;
    } else if (eq(a, "--stats")) {
      o.stats = true;
    } else if (eq(a, "--expect")) {
      o.expect = std::atoi(next());
    } else if (eq(a, "--identity-check")) {
      o.identityCheck = true;
    } else if (eq(a, "--auto-center")) {
      o.autoCenter = true;
    } else if (eq(a, "--center-check")) {
      o.centerCheck = true;
    } else {
      die(std::string("unknown option ") + a);
    }
  }
  if (o.inPath.empty()) die("usage: masktest <in.png> [out.png] [options]");
  return o;
}

// --------------------------------------------------------------------- image

struct Bitmap {
  int w = 0, h = 0;
  std::vector<float> px;  // RGBA, premultiplied, top-down

  am::ImageView view() const { return am::ImageView{px.data(), w, h, std::ptrdiff_t(w) * 4}; }
};

Bitmap loadPng(const std::string& path) {
  int w = 0, h = 0, n = 0;
  unsigned char* raw = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (!raw) die("could not read " + path);

  Bitmap b;
  b.w = w;
  b.h = h;
  b.px.resize(std::size_t(w) * h * 4);
  for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
    const float a = raw[i * 4 + 3] / 255.0f;
    // Premultiply on the way in. Everything downstream assumes premultiplied
    // RGBA, which is what an OFX host hands the plugin.
    b.px[i * 4 + 0] = (raw[i * 4 + 0] / 255.0f) * a;
    b.px[i * 4 + 1] = (raw[i * 4 + 1] / 255.0f) * a;
    b.px[i * 4 + 2] = (raw[i * 4 + 2] / 255.0f) * a;
    b.px[i * 4 + 3] = a;
  }
  stbi_image_free(raw);
  return b;
}

void writePng(const std::string& path, int w, int h, const std::vector<float>& px) {
  std::vector<unsigned char> out(std::size_t(w) * h * 4);
  for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
    const float a = px[i * 4 + 3];
    // Unpremultiply for the file; PNG is straight alpha.
    const float inv = a > 1e-6f ? 1.0f / a : 0.0f;
    for (int c = 0; c < 3; ++c) {
      float v = px[i * 4 + c] * inv;
      v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      out[i * 4 + c] = (unsigned char)(v * 255.0f + 0.5f);
    }
    const float ca = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    out[i * 4 + 3] = (unsigned char)(ca * 255.0f + 0.5f);
  }
  if (!stbi_write_png(path.c_str(), w, h, 4, out.data(), w * 4))
    die("could not write " + path);
}

// False-colour the label image so a glance says whether the segmentation is
// right. Tuning three thresholds against a greyscale matte is guesswork;
// against distinct colours per element it is obvious.
void writeDiag(const std::string& path, const am::Segmentation& seg) {
  std::vector<float> px(std::size_t(seg.width) * seg.height * 4, 0.0f);
  for (std::size_t i = 0; i < px.size() / 4; ++i) {
    const int32_t l = seg.labelImage[i];
    if (l == 0) continue;
    const std::uint32_t hash = std::uint32_t(l) * 2654435761u;
    px[i * 4 + 0] = 0.25f + 0.75f * float((hash >> 0) & 0xFF) / 255.0f;
    px[i * 4 + 1] = 0.25f + 0.75f * float((hash >> 8) & 0xFF) / 255.0f;
    px[i * 4 + 2] = 0.25f + 0.75f * float((hash >> 16) & 0xFF) / 255.0f;
    px[i * 4 + 3] = 1.0f;
  }
  // Mark each seed pixel white, so a seed sitting in a hole is visible rather
  // than a mystery about why Select All missed an element.
  for (int l = 1; l <= seg.count(); ++l) {
    const am::Component& c = seg.components[std::size_t(l)];
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) {
        const int x = c.seedX + dx, y = c.seedY + dy;
        if (x < 0 || y < 0 || x >= seg.width || y >= seg.height) continue;
        const std::size_t i = std::size_t(y) * seg.width + x;
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4 + 3] = 1.0f;
      }
  }
  writePng(path, seg.width, seg.height, px);
}

}  // namespace

int main(int argc, char** argv) {
  const Options o = parseArgs(argc, argv);
  const Bitmap in = loadPng(o.inPath);
  const am::ImageView src = in.view();

  const am::Segmentation seg = am::Segment(src, o.det);

  if (o.stats) {
    std::printf("%dx%d  elements=%d\n", seg.width, seg.height, seg.count());
    for (int l = 1; l <= seg.count(); ++l) {
      const am::Component& c = seg.components[std::size_t(l)];
      std::printf("  %3d  area=%-7d bbox=[%d,%d %dx%d]  seed=(%d,%d)\n", l, c.area, c.bbox.x1,
                  c.bbox.y1, c.bbox.width(), c.bbox.height(), c.seedX, c.seedY);
    }
  }

  if (o.expect >= 0 && seg.count() != o.expect) {
    std::fprintf(stderr, "masktest: expected %d elements, found %d\n", o.expect, seg.count());
    return 1;
  }

  if (o.diag) {
    if (o.outPath.empty()) die("--diag needs an output path");
    writeDiag(o.outPath, seg);
    return 0;
  }

  const std::vector<am::Anchor> anchors =
      o.selectAll ? am::AnchorsForAll(seg) : am::ParseAnchors(o.select);
  const am::Resolved sel = am::ResolveSelection(seg, anchors);

  if (o.stats)
    std::printf("  selected=%d  unresolved=%d\n", sel.selectedCount, sel.unresolvedCount);

  std::vector<float> mask;
  am::BuildMask(seg, sel, o.mask, mask);

  // Built through the same helper the plugin uses, so the harness cannot be
  // testing a different convention from the one that ships. Positive Y is up
  // here for the same reason it is up there.
  const float w = float(seg.width), h = float(seg.height);

  float posX = o.posX, posY = o.posY;
  if (o.autoCenter) {
    double cpx = 0.0, cpy = 0.0;
    if (!am::CenterOffsetForSelection(seg, sel, o.scaleX, o.scaleY, o.rot, o.anchorX, o.anchorY,
                                      cpx, cpy))
      die("--auto-center with nothing selected");
    posX = float(cpx);
    posY = float(cpy);
    if (o.stats) std::printf("  auto-center position: %.5f, %.5f\n", cpx, cpy);
  }

  am::CompositeParams cp;
  cp.outputMode = o.outputMode;
  cp.backgroundMode = o.bgMode;
  cp.filter = o.filter;
  cp.identity = am::TransformIsIdentity(posX, posY, o.scaleX, o.scaleY, o.rot);
  if (!cp.identity)
    cp.inv = mtx::Invert(am::MakeElementTransform(w, h, posX, posY, o.scaleX, o.scaleY, o.rot,
                                                  o.anchorX, o.anchorY));

  const am::PlaneView plane{mask.data(), seg.width, seg.height};
  std::vector<float> out(std::size_t(seg.width) * seg.height * 4, 0.0f);
  for (int y = 0; y < seg.height; ++y)
    for (int x = 0; x < seg.width; ++x)
      am::RenderPixel(src, plane, cp, x, y, &out[(std::size_t(y) * seg.width + x) * 4]);

  // The regression test for the recombination algebra in Composite.h. A
  // composite with the transform at defaults must return the source untouched,
  // feather and all -- and the naive `over` fails it by a quarter of a stop
  // along every soft edge.
  if (o.identityCheck) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
      worst = std::max(worst, std::fabs(out[i] - in.px[i]));
    std::printf("identity max abs difference: %.9g\n", double(worst));
    if (worst > 1e-6f) {
      std::fprintf(stderr, "masktest: identity composite is not a no-op (%.9g)\n", double(worst));
      return 1;
    }
  }

  // Auto Center claims to put the BOUNDING BOX centre on the frame centre --
  // not the centroid, which for a triangle is a third of the way up and would
  // make the button look broken. So measure the bounding box of what actually
  // came out, and check that.
  if (o.centerCheck) {
    int x1 = seg.width, y1 = seg.height, x2 = 0, y2 = 0;
    for (int y = 0; y < seg.height; ++y)
      for (int x = 0; x < seg.width; ++x) {
        if (out[(std::size_t(y) * seg.width + x) * 4 + 3] <= 0.5f) continue;
        x1 = std::min(x1, x);
        y1 = std::min(y1, y);
        x2 = std::max(x2, x + 1);
        y2 = std::max(y2, y + 1);
      }
    if (x2 <= x1) {
      std::fprintf(stderr, "masktest: --center-check found nothing in the output\n");
      return 1;
    }
    const double dx = (x1 + x2) * 0.5 - seg.width * 0.5;
    const double dy = (y1 + y2) * 0.5 - seg.height * 0.5;
    std::printf("centred to within %.3f, %.3f px\n", dx, dy);
    // A pixel and a half: the resampled edge gains a fraction of a pixel on
    // each side, and a half-covered edge pixel can tip either way.
    if (std::fabs(dx) > 1.5 || std::fabs(dy) > 1.5) {
      std::fprintf(stderr, "masktest: not centred (%.3f, %.3f px off)\n", dx, dy);
      return 1;
    }
  }

  if (!o.outPath.empty()) writePng(o.outPath, seg.width, seg.height, out);
  return 0;
}
