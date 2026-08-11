// Image sampling. Adapted from resolve-multi-transform's Sampler.h, with two
// deliberate changes:
//
//  1. It samples am::ImageView rather than carrying a second view type. The
//     original's Pixel() computes `static_cast<size_t>(y) * rowStrideFloats`,
//     which wraps the moment the stride is negative -- and a negative stride is
//     exactly how a top-down view of a bottom-up OFX image is expressed here.
//     All row arithmetic below is ptrdiff_t.
//
//  2. The motion-blur tail (SampleTransforms, shutter averaging) is gone. There
//     is one transform per frame in this plugin, so there is nothing to average.
#pragma once

#include "core/ImageView.h"
#include "core/TransformMath.h"

namespace am {

enum EdgeMode {
  kEdgeBlack = 0,  ///< outside the source is transparent black
  kEdgeClamp = 1,  ///< outside repeats the border pixel
};

enum FilterMode { kFilterNearest = 0, kFilterBilinear = 1 };

MTX_HD inline int WrapCoord(int v, int n, EdgeMode mode, bool& outside) {
  if (v >= 0 && v < n) return v;
  outside = true;
  if (mode == kEdgeClamp) return v < 0 ? 0 : n - 1;
  return v < 0 ? 0 : n - 1;  // kEdgeBlack: the caller discards via `outside`
}

/// Fetch one RGBA texel, honouring the edge mode. Writes 4 channels into `out`.
MTX_HD inline void FetchTexel(const ImageView& img, int x, int y, EdgeMode mode, float* out) {
  bool outside = false;
  const int cx = WrapCoord(x, img.width, mode, outside);
  const int cy = WrapCoord(y, img.height, mode, outside);

  if (outside && mode == kEdgeBlack) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    return;
  }
  const float* p = img.at(cx, cy);
  out[0] = p[0];
  out[1] = p[1];
  out[2] = p[2];
  out[3] = p[3];
}

/// Bilinear RGBA sample.
/// @param sx,sy pixel-centre coordinates: the centre of texel (0,0) is
///              (0.5,0.5), matching how the transform maps destination centres.
MTX_HD inline void SampleBilinear(const ImageView& img, float sx, float sy, EdgeMode mode,
                                  float* out) {
  const float fx = sx - 0.5f;
  const float fy = sy - 0.5f;

  const int x0 = int(std::floor(fx));
  const int y0 = int(std::floor(fy));
  const float tx = fx - float(x0);
  const float ty = fy - float(y0);

  float p00[4], p10[4], p01[4], p11[4];
  FetchTexel(img, x0, y0, mode, p00);
  FetchTexel(img, x0 + 1, y0, mode, p10);
  FetchTexel(img, x0, y0 + 1, mode, p01);
  FetchTexel(img, x0 + 1, y0 + 1, mode, p11);

  for (int c = 0; c < 4; ++c) {
    const float top = mtx::Lerp(p00[c], p10[c], tx);
    const float bot = mtx::Lerp(p01[c], p11[c], tx);
    out[c] = mtx::Lerp(top, bot, ty);
  }
}

MTX_HD inline void SampleNearest(const ImageView& img, float sx, float sy, EdgeMode mode,
                                 float* out) {
  FetchTexel(img, int(std::floor(sx)), int(std::floor(sy)), mode, out);
}

MTX_HD inline void SampleImage(const ImageView& img, float sx, float sy, FilterMode filter,
                               EdgeMode edge, float* out) {
  if (filter == kFilterNearest)
    SampleNearest(img, sx, sy, edge, out);
  else
    SampleBilinear(img, sx, sy, edge, out);
}

// ------------------------------------------------------- single-channel plane

/// A non-owning view of a w*h float plane, row-major and tightly packed. Used
/// for the mask, which has to be warped alongside the image it belongs to.
struct PlaneView {
  const float* data = nullptr;
  int width = 0;
  int height = 0;

  MTX_HD float at(int x, int y) const { return data[std::ptrdiff_t(y) * width + x]; }
};

MTX_HD inline float FetchPlane(const PlaneView& pl, int x, int y, EdgeMode mode) {
  bool outside = false;
  const int cx = WrapCoord(x, pl.width, mode, outside);
  const int cy = WrapCoord(y, pl.height, mode, outside);
  if (outside && mode == kEdgeBlack) return 0.0f;
  return pl.at(cx, cy);
}

MTX_HD inline float SamplePlaneBilinear(const PlaneView& pl, float sx, float sy, EdgeMode mode) {
  const float fx = sx - 0.5f;
  const float fy = sy - 0.5f;
  const int x0 = int(std::floor(fx));
  const int y0 = int(std::floor(fy));
  const float tx = fx - float(x0);
  const float ty = fy - float(y0);

  const float top = mtx::Lerp(FetchPlane(pl, x0, y0, mode), FetchPlane(pl, x0 + 1, y0, mode), tx);
  const float bot =
      mtx::Lerp(FetchPlane(pl, x0, y0 + 1, mode), FetchPlane(pl, x0 + 1, y0 + 1, mode), tx);
  return mtx::Lerp(top, bot, ty);
}

MTX_HD inline float SamplePlane(const PlaneView& pl, float sx, float sy, FilterMode filter,
                                EdgeMode edge) {
  if (filter == kFilterNearest)
    return FetchPlane(pl, int(std::floor(sx)), int(std::floor(sy)), edge);
  return SamplePlaneBilinear(pl, sx, sy, edge);
}

}  // namespace am
