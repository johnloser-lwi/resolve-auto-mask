// Turns a label image plus a set of selected labels into a float mask.
#pragma once

#include <vector>

#include "core/Blur.h"
#include "core/Parallel.h"
#include "core/Segmentation.h"
#include "core/Selection.h"

namespace am {

struct MaskParams {
  /// Pixels. Positive grows the selection, negative shrinks it.
  int expand = 0;

  /// Width of the soft transition, in pixels. Zero means a hard mask, and is
  /// short-circuited entirely -- no float blur, no cost.
  float feather = 0.0f;

  bool invert = false;

  bool operator==(const MaskParams& o) const {
    return expand == o.expand && feather == o.feather && invert == o.invert;
  }
  bool operator!=(const MaskParams& o) const { return !(*this == o); }
};

/// Result is w*h floats in [0,1], row-major, top-down, matching seg.labelImage.
inline void BuildMask(const Segmentation& seg, const Resolved& sel, const MaskParams& mp,
                      std::vector<float>& out) {
  const int w = seg.width, h = seg.height;
  const std::size_t n = std::size_t(w) * std::size_t(h);
  out.assign(n, 0.0f);
  if (n == 0) return;

  if (seg.labelImage.size() != n) {
    // No analysis yet. Inverting nothing is everything, which is the honest
    // answer and keeps Invert meaningful before the first render.
    if (mp.invert) std::fill(out.begin(), out.end(), 1.0f);
    return;
  }

  // 1. Binary stencil. One indexed load per pixel, no branch.
  std::vector<std::uint8_t> bin(n, 0);
  ParallelFor(int(n), [&](int p0, int p1) {
    for (int p = p0; p < p1; ++p)
      bin[std::size_t(p)] = sel.labelSelected[std::size_t(seg.labelImage[std::size_t(p)])];
  });

  // 2. Expand / contract.
  //
  // Erosion is dilation of the complement, complemented -- so one
  // implementation covers both and they cannot drift apart at the edges.
  //
  // The separable sweep gives a Chebyshev (square) structuring element, so a
  // corner grows by r*sqrt(2) rather than r. That is the trade for a cost
  // independent of radius, which is the property that matters on a slider. A
  // true Euclidean distance transform is the upgrade if the squareness ever
  // becomes visible.
  if (mp.expand > 0) {
    bin = Dilate(bin, w, h, mp.expand);
  } else if (mp.expand < 0) {
    auto flip = [&] {
      ParallelFor(int(n), [&](int p0, int p1) {
        for (int p = p0; p < p1; ++p)
          bin[std::size_t(p)] = std::uint8_t(1 - bin[std::size_t(p)]);
      });
    };
    flip();
    bin = Dilate(bin, w, h, -mp.expand);
    flip();
  }

  ParallelFor(int(n), [&](int p0, int p1) {
    for (int p = p0; p < p1; ++p) out[std::size_t(p)] = float(bin[std::size_t(p)]);
  });

  // 3. Feather. sigma = feather/2 puts roughly 95% of the transition inside the
  //    stated pixel width, so the number on the slider means what it says.
  if (mp.feather > 0.0f) BlurPlane(out, w, h, mp.feather * 0.5f);

  // 4. Invert LAST.
  //
  // Inverting first would flip the sense of Expand -- the user would drag a
  // control labelled "Expand" and watch the selection shrink. Doing it here
  // keeps "Expand grows the selected elements" true in both states.
  if (mp.invert)
    ParallelFor(int(n), [&](int p0, int p1) {
      for (int p = p0; p < p1; ++p) out[std::size_t(p)] = 1.0f - out[std::size_t(p)];
    });
}

}  // namespace am
