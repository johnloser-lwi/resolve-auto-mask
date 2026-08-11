#include "core/Segmentation.h"

#include <algorithm>
#include <limits>

#include "core/Parallel.h"

namespace am {
namespace {

// ---------------------------------------------------------------- union-find

struct UnionFind {
  std::vector<int> parent;

  int add() {
    parent.push_back(int(parent.size()));
    return int(parent.size()) - 1;
  }

  int find(int a) {
    while (parent[a] != a) {
      parent[a] = parent[parent[a]];  // path halving
      a = parent[a];
    }
    return a;
  }

  void unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
};

// ------------------------------------------------------------------ dilation

// Cost does not depend on the radius. The obvious version scans the window at
// every pixel, so it slows down as the radius grows -- which is felt exactly
// when dragging a slider, since every change re-runs the whole pass. Instead
// each line is swept twice, carrying the position of the nearest set pixel
// behind and ahead; a pixel is set if either is within r. Exact for a binary
// mask, and identical to the window scan it replaces.
// Contiguous only. The vertical pass gets there by transposing rather than by
// striding down columns -- see the note in Blur.h; the cost of a strided pass
// over a UHD plane is a cache miss per sample, and it dominates everything.
void dilateRow(const std::uint8_t* src, std::uint8_t* dst, int n, int r, std::vector<int>& prev,
               std::vector<int>& next) {
  const int kFar = std::numeric_limits<int>::max() / 4;
  prev.resize(std::size_t(n));
  next.resize(std::size_t(n));

  int last = -kFar;
  for (int i = 0; i < n; ++i) {
    if (src[i]) last = i;
    prev[std::size_t(i)] = last;
  }
  int ahead = kFar;
  for (int i = n - 1; i >= 0; --i) {
    if (src[i]) ahead = i;
    next[std::size_t(i)] = ahead;
  }
  for (int i = 0; i < n; ++i)
    dst[i] = std::uint8_t((i - prev[std::size_t(i)] <= r) || (next[std::size_t(i)] - i <= r));
}

void transposeBytes(const std::uint8_t* src, std::uint8_t* dst, int w, int h) {
  constexpr int kBlock = 32;
  ParallelFor(
      (h + kBlock - 1) / kBlock,
      [&](int b0, int b1) {
        for (int bi = b0; bi < b1; ++bi) {
          const int y0 = bi * kBlock, y1 = std::min(y0 + kBlock, h);
          for (int x0 = 0; x0 < w; x0 += kBlock) {
            const int x1 = std::min(x0 + kBlock, w);
            for (int y = y0; y < y1; ++y)
              for (int x = x0; x < x1; ++x)
                dst[std::size_t(x) * h + y] = src[std::size_t(y) * w + x];
          }
        }
      },
      1);
}

void dilateAllRows(const std::uint8_t* src, std::uint8_t* dst, int w, int h, int r) {
  ParallelFor(
      h,
      [&](int y0, int y1) {
        std::vector<int> prev, next;
        for (int y = y0; y < y1; ++y)
          dilateRow(src + std::size_t(y) * w, dst + std::size_t(y) * w, w, r, prev, next);
      },
      64);
}

// Rec.709. Computed on premultiplied values, which is exact for the a == 1.0
// sources Luminance mode exists to serve and close enough elsewhere -- a
// partially transparent pixel reads as darker, which is the same direction the
// threshold wants anyway.
inline float lumaOf(const float* px) {
  return 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
}

inline float sampleFor(const float* px, SegmentMode mode) {
  return mode == SegmentMode::Alpha ? px[3] : lumaOf(px);
}

}  // namespace

// ------------------------------------------------------------------ exported

std::vector<std::uint8_t> Dilate(const std::vector<std::uint8_t>& src, int w, int h, int r) {
  if (r <= 0) return src;
  const std::size_t n = std::size_t(w) * h;
  std::vector<std::uint8_t> a(n, 0), b(n, 0);

  dilateAllRows(src.data(), a.data(), w, h, r);   // horizontal
  transposeBytes(a.data(), b.data(), w, h);       // now h x w
  dilateAllRows(b.data(), a.data(), h, w, r);     // vertical, as rows
  transposeBytes(a.data(), b.data(), h, w);       // back to w x h
  return b;
}

int32_t Segmentation::labelNear(int x, int y, int radius) const {
  const int32_t hit = labelAt(x, y);
  if (hit != 0) return hit;

  for (int r = 1; r <= radius; ++r) {
    // The ring at Chebyshev distance r. Corners are visited twice; at these
    // radii (single digits) the branch to avoid it costs more than the repeat.
    for (int d = -r; d <= r; ++d) {
      int32_t l;
      if ((l = labelAt(x + d, y - r)) != 0) return l;
      if ((l = labelAt(x + d, y + r)) != 0) return l;
      if ((l = labelAt(x - r, y + d)) != 0) return l;
      if ((l = labelAt(x + r, y + d)) != 0) return l;
    }
  }
  return 0;
}

std::uint64_t HashSource(const ImageView& src, const DetectParams& p) {
  std::uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
  if (!src.valid()) return h;

  for (int y = 0; y < src.height; y += 8) {
    const float* row = src.row(y);
    for (int x = 0; x < src.width; x += 8) {
      const float v = sampleFor(row + std::ptrdiff_t(x) * 4, p.mode);
      // Quantise to 16 levels so imperceptible float noise does not invalidate
      // the cache, while a genuine content change always does. The thresholded
      // bit rides along so a change that crosses the threshold registers even
      // when it does not move the quantised level.
      const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      const std::uint8_t q = std::uint8_t(c * 15.0f);
      h = (h ^ std::uint64_t(q + (v > p.threshold ? 16 : 0))) * 1099511628211ull;
    }
  }
  h = (h ^ std::uint64_t(src.width)) * 1099511628211ull;
  h = (h ^ std::uint64_t(src.height)) * 1099511628211ull;
  return h;
}

Segmentation Segment(const ImageView& src, const DetectParams& params) {
  Segmentation seg;
  if (!src.valid()) return seg;

  const int w = src.width, h = src.height;
  seg.width = w;
  seg.height = h;

  // 1. Binary mask.
  std::vector<std::uint8_t> mask(std::size_t(w) * h, 0);
  bool any = false;
  for (int y = 0; y < h; ++y) {
    const float* row = src.row(y);
    std::uint8_t* m = &mask[std::size_t(y) * w];
    for (int x = 0; x < w; ++x) {
      const float v = sampleFor(row + std::ptrdiff_t(x) * 4, params.mode);
      const bool on = params.invertSource ? (v < params.threshold) : (v > params.threshold);
      if (on) {
        m[x] = 1;
        any = true;
      }
    }
  }
  if (!any) return seg;

  // 2. Optional bridge dilation. Labels are computed on the dilated mask but
  //    written back only onto true pixels, so bounding boxes stay accurate.
  const std::vector<std::uint8_t> dilated =
      params.bridgeRadius > 0 ? Dilate(mask, w, h, params.bridgeRadius)
                              : std::vector<std::uint8_t>();
  const std::vector<std::uint8_t>& labelMask = params.bridgeRadius > 0 ? dilated : mask;

  // 3. Connected components, 8-connectivity, two-pass with union-find.
  std::vector<int32_t> prov(std::size_t(w) * h, 0);
  UnionFind uf;
  uf.add();  // label 0 == background
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t p = std::size_t(y) * w + x;
      if (!labelMask[p]) continue;
      int best = 0;
      auto consider = [&](int nx, int ny) {
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
        const int32_t l = prov[std::size_t(ny) * w + nx];
        if (l == 0) return;
        if (best == 0) {
          best = l;
        } else {
          uf.unite(best, l);
          best = std::min(uf.find(best), uf.find(l));
        }
      };
      consider(x - 1, y);
      consider(x - 1, y - 1);
      consider(x, y - 1);
      consider(x + 1, y - 1);
      prov[p] = best != 0 ? best : uf.add();
    }
  }

  // Compact roots to sequential labels.
  std::vector<int> remap(uf.parent.size(), 0);
  int labelCount = 0;
  for (std::size_t i = 1; i < uf.parent.size(); ++i) {
    if (uf.find(int(i)) == int(i)) remap[i] = ++labelCount;
  }
  if (labelCount == 0) return seg;

  // 4. Write labels back onto true pixels only, and gather stats.
  seg.labelImage.assign(std::size_t(w) * h, 0);
  std::vector<Component> comps(std::size_t(labelCount) + 1);
  std::vector<double> sumX(std::size_t(labelCount) + 1, 0.0);
  std::vector<double> sumY(std::size_t(labelCount) + 1, 0.0);

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t p = std::size_t(y) * w + x;
      if (!mask[p] || prov[p] == 0) continue;
      const int lab = remap[uf.find(prov[p])];
      seg.labelImage[p] = lab;

      Component& c = comps[std::size_t(lab)];
      if (c.area == 0) {
        c.bbox = RectI{x, y, x + 1, y + 1};
        c.seedX = x;  // first hit in raster order, so it is inside by construction
        c.seedY = y;
      } else {
        c.bbox.x1 = std::min(c.bbox.x1, x);
        c.bbox.y1 = std::min(c.bbox.y1, y);
        c.bbox.x2 = std::max(c.bbox.x2, x + 1);
        c.bbox.y2 = std::max(c.bbox.y2, y + 1);
      }
      ++c.area;
      sumX[std::size_t(lab)] += x;
      sumY[std::size_t(lab)] += y;
    }
  }

  // 5. Despeckle, and compact the survivors so that labels stay contiguous.
  //
  // Compacting matters beyond tidiness: count() has to be the number of real
  // elements for the status read-out and for Select All, and a caller iterating
  // 1..count() must never land on a hole.
  std::vector<int32_t> survivor(std::size_t(labelCount) + 1, 0);
  seg.components.clear();
  seg.components.emplace_back();  // the dummy at index 0
  for (int l = 1; l <= labelCount; ++l) {
    Component& c = comps[std::size_t(l)];
    if (c.area < params.minElementSize) continue;
    c.cx = float(sumX[std::size_t(l)] / c.area);
    c.cy = float(sumY[std::size_t(l)] / c.area);
    survivor[std::size_t(l)] = int32_t(seg.components.size());
    seg.components.push_back(c);
  }

  for (std::size_t p = 0; p < seg.labelImage.size(); ++p) {
    const int32_t l = seg.labelImage[p];
    if (l != 0) seg.labelImage[p] = survivor[std::size_t(l)];
  }

  return seg;
}

}  // namespace am
