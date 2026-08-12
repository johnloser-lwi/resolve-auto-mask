// Per-element placement: where each selected element goes, relative to where
// it started.
//
// Every operation here resolves to a pure TRANSLATION per element. That is a
// deliberate limit, not an oversight. Spread, Stagger and Align are all "move
// this element somewhere else"; scale and rotation stay global, applied to the
// arranged selection as a whole. Keeping per-element motion translation-only
// means the layered composite never has to resample an element twice, and the
// arrangement can be reasoned about as a list of offsets.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Segmentation.h"
#include "core/Selection.h"

namespace am {

// Align, Spread and Step COMPOSE. They were originally an exclusive Mode, and
// that was wrong: the obvious thing to want is a row of items snapped to a
// common left edge and then separated vertically, which is two of them at once.
// There is no mode enum any more -- each operation has a neutral default
// (Align None, Spread 1, Step 0), so "off" needs no separate way to say it.
//
// They apply in a fixed order -- Align, then Spread, then Step -- because it is
// the only order that reads as a sequence of intentions rather than a
// coincidence: put them in a line, push them apart, then march them along.
// Spread measures from the ALIGNED positions, not the original ones.

/// The order elements are considered in. Drives the Step index, and the
/// painter's order in which the layered composite stacks them.
enum ArrangeOrder {
  kOrderLeftRight = 0,
  kOrderRightLeft = 1,
  kOrderTopBottom = 2,
  kOrderBottomTop = 3,
  kOrderReading = 4,   ///< row-major: top row left-to-right, then the next row
  kOrderLargest = 5,
  kOrderSmallest = 6,
  kOrderRadial = 7     ///< nearest the selection centre first
};

enum AlignHMode { kAlignHNone = 0, kAlignLeft = 1, kAlignCenterH = 2, kAlignRight = 3 };
enum AlignVMode { kAlignVNone = 0, kAlignTop = 1, kAlignMiddle = 2, kAlignBottom = 3 };

struct ArrangeParams {
  int order = kOrderLeftRight;
  bool reverse = false;

  int alignH = kAlignHNone;
  int alignV = kAlignVNone;

  /// Per axis. 1.0 is no movement; 2.0 doubles each element's distance from the
  /// shared centre.
  ///
  /// Being MULTIPLICATIVE has a consequence worth knowing: it cannot separate
  /// elements that are already on top of each other, because twice nothing is
  /// still nothing. Left-align a row and their vertical positions collapse
  /// together, and no amount of vertical Spread will part them again -- Step is
  /// the control for that. The trade buys a number that means the same thing
  /// whatever the original spacing was, which a distance does not.
  double spreadX = 1.0, spreadY = 1.0;

  /// Per-index step, as a fraction of the frame. Y is in the user's y-up sense.
  double stepX = 0.0, stepY = 0.0;

  bool operator==(const ArrangeParams& o) const {
    return order == o.order && reverse == o.reverse && alignH == o.alignH &&
           alignV == o.alignV && spreadX == o.spreadX && spreadY == o.spreadY &&
           stepX == o.stepX && stepY == o.stepY;
  }
  bool operator!=(const ArrangeParams& o) const { return !(*this == o); }
};

struct Placement {
  std::int32_t label = 0;
  int index = 0;        ///< position in the chosen order, from 0
  float dx = 0.0f;      ///< translation in top-down pixels
  float dy = 0.0f;
  RectI bbox;           ///< the element's source bounding box
};

namespace detail {

inline float centreX(const RectI& b) { return float(b.x1 + b.x2) * 0.5f; }
inline float centreY(const RectI& b) { return float(b.y1 + b.y2) * 0.5f; }

/// Row-major reading order.
///
/// Sorting by y then x does not work: two elements in the same visual row
/// almost never share a centre y, so a plain lexicographic sort interleaves the
/// rows. Elements are grouped into rows first, by whether they overlap
/// vertically with the row so far, and only then sorted left to right within
/// each row.
inline void sortReading(std::vector<Placement>& p) {
  if (p.empty()) return;
  std::stable_sort(p.begin(), p.end(), [](const Placement& a, const Placement& b) {
    return centreY(a.bbox) < centreY(b.bbox);
  });

  std::size_t rowStart = 0;
  float rowBottom = float(p[0].bbox.y2);
  for (std::size_t i = 1; i <= p.size(); ++i) {
    const bool endOfRow = (i == p.size()) || (centreY(p[i].bbox) >= rowBottom);
    if (endOfRow) {
      std::stable_sort(p.begin() + rowStart, p.begin() + i,
                       [](const Placement& a, const Placement& b) {
                         return centreX(a.bbox) < centreX(b.bbox);
                       });
      if (i < p.size()) {
        rowStart = i;
        rowBottom = float(p[i].bbox.y2);
      }
    } else {
      rowBottom = std::max(rowBottom, float(p[i].bbox.y2));
    }
  }
}

}  // namespace detail

/// One entry per selected element, in the chosen order, each carrying the
/// offset that puts it where the arrangement wants it.
inline std::vector<Placement> ComputeArrangement(const Segmentation& seg, const Resolved& sel,
                                                 const ArrangeParams& ap, float w, float h) {
  std::vector<Placement> out;
  if (seg.empty()) return out;

  for (int l = 1; l <= seg.count(); ++l) {
    if (!sel.labelSelected[std::size_t(l)]) continue;
    Placement p;
    p.label = std::int32_t(l);
    p.bbox = seg.components[std::size_t(l)].bbox;
    out.push_back(p);
  }
  if (out.empty()) return out;

  // The extent of the whole selection, which Spread measures from and Align
  // snaps to.
  RectI all;
  for (const Placement& p : out) all.unionWith(p.bbox);
  const float allCx = detail::centreX(all);
  const float allCy = detail::centreY(all);

  switch (ap.order) {
    case kOrderRightLeft:
      std::stable_sort(out.begin(), out.end(), [](const Placement& a, const Placement& b) {
        return detail::centreX(a.bbox) > detail::centreX(b.bbox);
      });
      break;
    case kOrderTopBottom:
      std::stable_sort(out.begin(), out.end(), [](const Placement& a, const Placement& b) {
        return detail::centreY(a.bbox) < detail::centreY(b.bbox);
      });
      break;
    case kOrderBottomTop:
      std::stable_sort(out.begin(), out.end(), [](const Placement& a, const Placement& b) {
        return detail::centreY(a.bbox) > detail::centreY(b.bbox);
      });
      break;
    case kOrderReading:
      detail::sortReading(out);
      break;
    case kOrderLargest:
    case kOrderSmallest: {
      const bool asc = ap.order == kOrderSmallest;
      std::stable_sort(out.begin(), out.end(),
                       [&seg, asc](const Placement& a, const Placement& b) {
                         const int aa = seg.components[std::size_t(a.label)].area;
                         const int ba = seg.components[std::size_t(b.label)].area;
                         return asc ? (aa < ba) : (aa > ba);
                       });
      break;
    }
    case kOrderRadial:
      std::stable_sort(out.begin(), out.end(),
                       [allCx, allCy](const Placement& a, const Placement& b) {
                         const float ax = detail::centreX(a.bbox) - allCx;
                         const float ay = detail::centreY(a.bbox) - allCy;
                         const float bx = detail::centreX(b.bbox) - allCx;
                         const float by = detail::centreY(b.bbox) - allCy;
                         return (ax * ax + ay * ay) < (bx * bx + by * by);
                       });
      break;
    case kOrderLeftRight:
    default:
      std::stable_sort(out.begin(), out.end(), [](const Placement& a, const Placement& b) {
        return detail::centreX(a.bbox) < detail::centreX(b.bbox);
      });
      break;
  }

  if (ap.reverse) std::reverse(out.begin(), out.end());
  for (std::size_t i = 0; i < out.size(); ++i) out[i].index = int(i);

  // Work in absolute positions and difference at the end, so the three
  // operations genuinely chain: Spread has to measure from where Align put
  // things, which summing three independent offsets would not do.
  const std::size_t n = out.size();
  std::vector<float> px(n), py(n);
  for (std::size_t i = 0; i < n; ++i) {
    px[i] = detail::centreX(out[i].bbox);
    py[i] = detail::centreY(out[i].bbox);
  }

  // 1. Align, against the extent of the whole selection as it started.
  //
  // Top and Bottom are visual, and top-down y makes them y1 and y2 directly --
  // no flip, because these are positions within the frame rather than an offset
  // the user typed.
  for (std::size_t i = 0; i < n; ++i) {
    const RectI& b = out[i].bbox;
    switch (ap.alignH) {
      case kAlignLeft: px[i] += float(all.x1 - b.x1); break;
      case kAlignCenterH: px[i] = allCx; break;
      case kAlignRight: px[i] += float(all.x2 - b.x2); break;
      default: break;
    }
    switch (ap.alignV) {
      case kAlignTop: py[i] += float(all.y1 - b.y1); break;
      case kAlignMiddle: py[i] = allCy; break;
      case kAlignBottom: py[i] += float(all.y2 - b.y2); break;
      default: break;
    }
  }

  // 2. Spread, from the midpoint of the aligned positions.
  //
  // The midpoint of the extremes rather than the mean: the mean drifts toward
  // whichever side happens to hold more elements, so adding one more item to a
  // cluster would shift everything else. The midpoint only moves when the
  // outermost elements move.
  if (ap.spreadX != 1.0 || ap.spreadY != 1.0) {
    float loX = px[0], hiX = px[0], loY = py[0], hiY = py[0];
    for (std::size_t i = 1; i < n; ++i) {
      loX = std::min(loX, px[i]);
      hiX = std::max(hiX, px[i]);
      loY = std::min(loY, py[i]);
      hiY = std::max(hiY, py[i]);
    }
    const float cx = (loX + hiX) * 0.5f, cy = (loY + hiY) * 0.5f;
    for (std::size_t i = 0; i < n; ++i) {
      px[i] = cx + (px[i] - cx) * float(ap.spreadX);
      py[i] = cy + (py[i] - cy) * float(ap.spreadY);
    }
  }

  // 3. Step. Y negated because the control is in the user's y-up sense and this
  // is all top-down, the same flip the global transform makes.
  for (std::size_t i = 0; i < n; ++i) {
    px[i] += float(ap.stepX * double(w) * out[i].index);
    py[i] += float(-ap.stepY * double(h) * out[i].index);
  }

  for (std::size_t i = 0; i < n; ++i) {
    out[i].dx = px[i] - detail::centreX(out[i].bbox);
    out[i].dy = py[i] - detail::centreY(out[i].bbox);
  }
  return out;
}

/// Does this arrangement actually move anything?
///
/// The guard for the fast path. Spread at 1.0, a zero Stagger step and Align
/// with both axes off are all legitimate settings that mean "no arrangement",
/// and each must fall back to the single-pass composite -- not merely for
/// speed, but because that path is the one with the exact identity guarantee.
inline bool ArrangementMoves(const std::vector<Placement>& p) {
  for (const Placement& e : p)
    if (e.dx != 0.0f || e.dy != 0.0f) return true;
  return false;
}

}  // namespace am
