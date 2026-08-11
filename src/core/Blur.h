// Separable blur for a single-channel float plane, used to feather the mask.
//
// Three box passes rather than a true Gaussian. A box blur is O(n) per pass
// independent of radius -- the same property that makes the binary dilation
// cheap, and it matters for the same reason: Feather is a slider people drag,
// so the cost of a wide radius is felt directly. Three boxes land within a few
// percent of a Gaussian, far below anything visible in a matte edge.
//
// The vertical passes are done by TRANSPOSING and blurring rows again, rather
// than by walking columns. That is not a micro-optimisation. Walking a column
// of a UHD plane strides 3840 floats per step, so every single sample is a
// cache miss; measured, the strided version cost 294 ms per frame at UHD
// against 36 ms for the entire rest of the pipeline. Two blocked transposes are
// far cheaper than six strided passes.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Parallel.h"

namespace am {

namespace detail {

/// One box pass along a contiguous row of `n` samples, running-sum so the
/// radius is free. `src` and `dst` must not overlap.
///
/// Edges clamp. Treating outside as zero would darken the border of the plane
/// and eat into a mask that runs to the edge of frame -- which is exactly what
/// a full-frame selection is.
inline void boxRow(const float* src, float* dst, int n, int r) {
  if (n <= 0) return;
  if (r <= 0) {
    std::copy(src, src + n, dst);
    return;
  }

  const float inv = 1.0f / float(2 * r + 1);
  // Prime the window at i == 0 with the left tail clamped to sample 0.
  float sum = src[0] * float(r + 1);
  for (int i = 1; i <= r; ++i) sum += src[std::min(i, n - 1)];

  for (int i = 0; i < n; ++i) {
    dst[i] = sum * inv;
    sum += src[std::min(i + r + 1, n - 1)] - src[std::max(i - r, 0)];
  }
}

/// Blocked transpose. The block is what makes it cheap: a naive element-wise
/// transpose misses cache on one side or the other for every element, which is
/// the very cost this exists to avoid.
inline void transpose(const float* src, float* dst, int w, int h) {
  constexpr int kBlock = 32;
  ParallelFor((h + kBlock - 1) / kBlock, [&](int b0, int b1) {
    for (int bi = b0; bi < b1; ++bi) {
      const int y0 = bi * kBlock;
      const int y1 = std::min(y0 + kBlock, h);
      for (int x0 = 0; x0 < w; x0 += kBlock) {
        const int x1 = std::min(x0 + kBlock, w);
        for (int y = y0; y < y1; ++y)
          for (int x = x0; x < x1; ++x)
            dst[std::size_t(x) * h + y] = src[std::size_t(y) * w + x];
      }
    }
  }, 1);
}

/// Three box passes over every row of a w*h plane, in place.
inline void blurRows(std::vector<float>& plane, std::vector<float>& tmp, int w, int h, int r) {
  ParallelFor(h, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      float* p = &plane[std::size_t(y) * w];
      float* t = &tmp[std::size_t(y) * w];
      boxRow(p, t, w, r);  // pass 1 -> tmp
      boxRow(t, p, w, r);  // pass 2 -> plane
      boxRow(p, t, w, r);  // pass 3 -> tmp
      std::copy(t, t + w, p);
    }
  }, 64);
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
  std::vector<float> flipped(plane.size());

  detail::blurRows(plane, tmp, w, h, r);

  // Vertical, as rows of the transpose.
  detail::transpose(plane.data(), flipped.data(), w, h);
  detail::blurRows(flipped, tmp, h, w, r);
  detail::transpose(flipped.data(), plane.data(), h, w);
}

}  // namespace am
