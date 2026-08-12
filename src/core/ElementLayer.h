// Builds the moved-elements layer, when the elements no longer share one
// transform.
//
// With a single transform the composite can inverse-warp the whole frame in one
// pass: every destination pixel comes from exactly one place. Once elements
// move independently that stops being true -- a destination pixel could come
// from any of them, or from two that now overlap -- so the only correct answer
// is to stack them: warp each element and composite it over the ones already
// laid down.
//
// The result is two full-frame buffers, a premultiplied colour and a coverage,
// already in destination space. The final combine then reads them with no
// transform at all, which means it is the SAME algebra as the single-pass path
// rather than a second implementation of it.
//
// Feather has to become per-element here. Blurring the combined mask and then
// moving pieces of it would drag each element's neighbours along in its soft
// edge -- visible as a ghost of the old layout clinging to every element the
// moment they separate. Each element is therefore feathered in its own tile,
// grown by enough halo to hold the blur.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Arrange.h"
#include "core/Blur.h"
#include "core/Composite.h"
#include "core/MaskBuild.h"
#include "core/Parallel.h"
#include "core/Sampler.h"
#include "core/Segmentation.h"

namespace am {

/// The arranged elements, rasterised into their own buffer.
///
/// `bounds` is in arranged-space pixels and is deliberately NOT the frame. An
/// arrangement can put elements outside it, and the global transform runs
/// afterwards -- so clipping to the frame here would destroy content the
/// transform could still bring back into view. `bounds` covers everything the
/// transform can reach; see VisibleSourceRegion.
struct ElementLayer {
  RectI bounds;
  std::vector<float> colour;    ///< premultiplied, bounds-sized
  std::vector<float> coverage;

  int width() const { return bounds.width(); }
  int height() const { return bounds.height(); }

  void reset(const RectI& b) {
    bounds = b;
    const std::size_t n = std::size_t(b.width()) * std::size_t(b.height());
    colour.assign(n * 4, 0.0f);
    coverage.assign(n, 0.0f);
  }
};

namespace detail {

/// How far past the element's own pixels the tile has to reach for the blur and
/// the expand to be correct at its edges.
inline int tileHalo(const MaskParams& mp) {
  const float sigma = mp.feather * 0.5f;
  const int blur = mp.feather > 0.0f ? int(std::ceil(3.0f * sigma)) + 2 : 0;
  return (mp.expand > 0 ? mp.expand : 0) + blur + 1;
}

}  // namespace detail

/// Builds the layer in ARRANGED space: each element moved to where the
/// arrangement puts it, and nothing else. The global Position/Scale/Rotation is
/// deliberately NOT baked in -- the composite warps the finished layer instead.
///
/// That split matters for more than tidiness. Composition is associative, so
/// warping the arranged layer gives exactly the same pixels as arranging and
/// then warping each element. But it makes the layer depend only on the
/// selection, the mask controls and the arrangement, so dragging Position
/// re-warps a cached layer rather than rebuilding it -- and on the GPU path it
/// is the difference between uploading 41 MB per frame and uploading it once.
///
/// @param placements one entry per element, in painter's order: later entries
///                   are composited on top.
/// @param region the arranged-space rectangle to rasterise into. Anything
///        outside it is genuinely unreachable and is dropped.
inline void BuildElementLayer(const ImageView& src, const Segmentation& seg,
                              const std::vector<Placement>& placements, const MaskParams& mp,
                              FilterMode filter, const RectI& region, ElementLayer& out) {
  const int w = seg.width, h = seg.height;
  out.reset(region);
  if (placements.empty() || w <= 0 || h <= 0 || region.empty()) return;

  const int lw = region.width();

  const int halo = detail::tileHalo(mp);

  std::vector<std::uint8_t> stencil;
  std::vector<float> tileMask;
  std::vector<float> tileColour;

  for (const Placement& pl : placements) {
    // --- the element's own tile ------------------------------------------
    RectI t = pl.bbox;
    t.grow(halo);
    t.x1 = std::max(t.x1, 0);
    t.y1 = std::max(t.y1, 0);
    t.x2 = std::min(t.x2, w);
    t.y2 = std::min(t.y2, h);
    if (t.empty()) continue;

    const int tw = t.width(), th = t.height();
    const std::size_t tn = std::size_t(tw) * th;

    stencil.assign(tn, 0);
    for (int y = 0; y < th; ++y) {
      const std::int32_t* srcRow = &seg.labelImage[std::size_t(y + t.y1) * w + t.x1];
      std::uint8_t* dstRow = &stencil[std::size_t(y) * tw];
      for (int x = 0; x < tw; ++x) dstRow[x] = std::uint8_t(srcRow[x] == pl.label);
    }

    // Same identity as the whole-frame path: erosion is dilation of the
    // complement, complemented.
    if (mp.expand > 0) {
      stencil = Dilate(stencil, tw, th, mp.expand);
    } else if (mp.expand < 0) {
      for (std::size_t i = 0; i < tn; ++i) stencil[i] = std::uint8_t(1 - stencil[i]);
      stencil = Dilate(stencil, tw, th, -mp.expand);
      for (std::size_t i = 0; i < tn; ++i) stencil[i] = std::uint8_t(1 - stencil[i]);
    }

    tileMask.assign(tn, 0.0f);
    for (std::size_t i = 0; i < tn; ++i) tileMask[i] = float(stencil[i]);
    if (mp.feather > 0.0f) BlurPlane(tileMask, tw, th, mp.feather * 0.5f);
    // Invert is NOT applied per element. An inverted mask is a statement about
    // the whole frame -- everything except the selection -- which is not a
    // thing individual elements can each carry a piece of. The caller handles
    // it, and the plugin hides Arrange when Invert Mask is on.

    // Premultiply the colour into the tile, so the warp interpolates colour and
    // coverage together. Doing it after interpolation drags the neighbouring
    // element's colour inside the edge -- the halo described in Composite.h.
    tileColour.assign(tn * 4, 0.0f);
    for (int y = 0; y < th; ++y) {
      const float* s = src.at(t.x1, y + t.y1);
      const float* m = &tileMask[std::size_t(y) * tw];
      float* d = &tileColour[std::size_t(y) * tw * 4];
      for (int x = 0; x < tw; ++x) {
        d[x * 4 + 0] = s[x * 4 + 0] * m[x];
        d[x * 4 + 1] = s[x * 4 + 1] * m[x];
        d[x * 4 + 2] = s[x * 4 + 2] * m[x];
        d[x * 4 + 3] = s[x * 4 + 3] * m[x];
      }
    }

    // --- where it lands ---------------------------------------------------
    // Pure translation, so the "warp" is an integer-free shift and nothing is
    // rotated or scaled twice.
    const mtx::Mat3 fwd = mtx::Mat3::Translate(pl.dx, pl.dy);
    const mtx::Mat3 inv = mtx::Invert(fwd);

    RectI dest;
    {
      const float cx[4] = {float(t.x1), float(t.x2), float(t.x1), float(t.x2)};
      const float cy[4] = {float(t.y1), float(t.y1), float(t.y2), float(t.y2)};
      float lo0 = 0, lo1 = 0, hi0 = 0, hi1 = 0;
      for (int k = 0; k < 4; ++k) {
        float px = 0.0f, py = 0.0f;
        fwd.Apply(cx[k], cy[k], px, py);
        if (k == 0) {
          lo0 = hi0 = px;
          lo1 = hi1 = py;
        } else {
          lo0 = std::min(lo0, px);
          hi0 = std::max(hi0, px);
          lo1 = std::min(lo1, py);
          hi1 = std::max(hi1, py);
        }
      }
      // One pixel of slack each way for the bilinear footprint. Clipped to the
      // LAYER, not to the frame: an element pushed off the edge by Arrange is
      // still reachable by the global transform, and clipping it here would
      // make that transform unable to bring it back.
      dest.x1 = std::max(int(std::floor(lo0)) - 1, region.x1);
      dest.y1 = std::max(int(std::floor(lo1)) - 1, region.y1);
      dest.x2 = std::min(int(std::ceil(hi0)) + 1, region.x2);
      dest.y2 = std::min(int(std::ceil(hi1)) + 1, region.y2);
    }
    if (dest.empty()) continue;

    const ImageView tileView{tileColour.data(), tw, th, std::ptrdiff_t(tw) * 4};
    const PlaneView maskView{tileMask.data(), tw, th};

    // Parallel over the destination rows of THIS element. Elements themselves
    // stay sequential, because "over" is not commutative and the order is the
    // arrangement order.
    ParallelFor(
        dest.height(),
        [&](int r0, int r1) {
          for (int ry = r0; ry < r1; ++ry) {
            const int y = dest.y1 + ry;
            for (int x = dest.x1; x < dest.x2; ++x) {
              float sx = 0.0f, sy = 0.0f;
              inv.Apply(float(x) + 0.5f, float(y) + 0.5f, sx, sy);

              // Into tile-local coordinates.
              const float tx = sx - float(t.x1);
              const float ty = sy - float(t.y1);
              if (tx < -1.0f || ty < -1.0f || tx > float(tw) + 1.0f || ty > float(th) + 1.0f)
                continue;

              float c[4];
              SampleImage(tileView, tx, ty, filter, kEdgeBlack, c);
              const float a = SamplePlane(maskView, tx, ty, filter, kEdgeBlack);
              if (a <= 0.0f && c[3] <= 0.0f) continue;

              const std::size_t p =
                  std::size_t(y - region.y1) * lw + std::size_t(x - region.x1);
              float* dc = &out.colour[p * 4];
              float& da = out.coverage[p];

              // Standard over, this element on top of what is already there.
              const float keep = 1.0f - a;
              for (int k = 0; k < 4; ++k) dc[k] = c[k] + dc[k] * keep;
              da = a + da * keep;
            }
          }
        },
        32);
  }
}

}  // namespace am
