// Separable blur for a single-channel float plane, used to feather the mask.
//
// Three box passes rather than a true Gaussian. A box blur is O(n) per pass
// independent of radius -- the same property that makes the binary dilation
// cheap, and it matters for the same reason: Feather is a slider people drag,
// so the cost of a wide radius is felt directly. Three boxes land within a few
// percent of a Gaussian, which is far below anything visible in a matte edge.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace am {

namespace detail {

// One box pass along a line of `n` samples with the given stride. Running-sum,
// so the radius is free. Edges clamp: the alternative, treating outside as
// zero, darkens the border of the plane and would eat into a mask that runs to
// the edge of frame -- exactly what happens with a full-frame selection.
inline void boxLine(const float* src, float* dst, int n, std::ptrdiff_t stride, int r,
                    std::vector<float>& scratch) {
  if (n <= 0) return;
  if (r <= 0) {
    for (int i = 0; i < n; ++i) dst[i * stride] = src[i * stride];
    return;
  }
  scratch.resize(std::size_t(n));
  for (int i = 0; i < n; ++i) scratch[std::size_t(i)] = src[i * stride];

  const float inv = 1.0f / float(2 * r + 1);
  // Prime the window at i == 0, with the left tail clamped to sample 0.
  float sum = scratch[0] * float(r + 1);
  for (int i = 1; i <= r; ++i) sum += scratch[std::size_t(std::min(i, n - 1))];

  for (int i = 0; i < n; ++i) {
    dst[i * stride] = sum * inv;
    const int add = std::min(i + r + 1, n - 1);
    const int sub = std::max(i - r, 0);
    sum += scratch[std::size_t(add)] - scratch[std::size_t(sub)];
  }
}

}  // namespace detail

/// Blur `plane` (w*h floats, row-major) in place to approximately `sigma`.
inline void BlurPlane(std::vector<float>& plane, int w, int h, float sigma) {
  if (sigma <= 0.0f || w <= 0 || h <= 0) return;

  // Standard three-box approximation: n boxes of width b have variance
  // n*(b*b-1)/12, so solving for b at n == 3 gives this. Forced odd so the box
  // is centred -- an even width shifts the mask half a pixel, which shows up as
  // a matte that no longer lines up with the cutout.
  int b = int(std::round(std::sqrt(12.0f * sigma * sigma / 3.0f + 1.0f)));
  if (b < 1) b = 1;
  if ((b & 1) == 0) ++b;
  const int r = (b - 1) / 2;
  if (r <= 0) return;

  std::vector<float> tmp(plane.size());
  std::vector<float> scratch;

  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < h; ++y)
      detail::boxLine(&plane[std::size_t(y) * w], &tmp[std::size_t(y) * w], w, 1, r, scratch);
    for (int x = 0; x < w; ++x)
      detail::boxLine(&tmp[std::size_t(x)], &plane[std::size_t(x)], h, w, r, scratch);
  }
}

}  // namespace am
