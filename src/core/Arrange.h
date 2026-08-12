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

enum ArrangeMode {
  kArrangeNone = 0,
  kArrangeSpread = 1,   ///< push out from the selection centre
  kArrangeStagger = 2,  ///< a fixed step per element, in order
  kArrangeAlign = 3     ///< snap to a common edge or centre
};

/// The order elements are considered in. Drives the Stagger index, and the
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
  int mode = kArrangeNone;
  int order = kOrderLeftRight;
  bool reverse = false;

  /// 1.0 is no movement; 2.0 doubles every element's distance from the centre.
  double spread = 1.0;

  /// Per-index step, as a fraction of the frame. Y is in the user's y-up sense.
  double stepX = 0.0, stepY = 0.0;

  int alignH = kAlignHNone;
  int alignV = kAlignVNone;

  bool operator==(const ArrangeParams& o) const {
    return mode == o.mode && order == o.order && reverse == o.reverse && spread == o.spread &&
           stepX == o.stepX && stepY == o.stepY && alignH == o.alignH && alignV == o.alignV;
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

  switch (ap.mode) {
    case kArrangeSpread:
      // A factor rather than a distance, so one control works whatever the
      // elements' spacing is, and 1.0 is exactly no movement.
      for (Placement& p : out) {
        p.dx = float((detail::centreX(p.bbox) - allCx) * (ap.spread - 1.0));
        p.dy = float((detail::centreY(p.bbox) - allCy) * (ap.spread - 1.0));
      }
      break;

    case kArrangeStagger:
      // Y negated because the control is in the user's y-up sense and this is
      // all top-down, the same flip the global transform makes.
      for (Placement& p : out) {
        p.dx = float(ap.stepX * double(w) * p.index);
        p.dy = float(-ap.stepY * double(h) * p.index);
      }
      break;

    case kArrangeAlign:
      for (Placement& p : out) {
        switch (ap.alignH) {
          case kAlignLeft: p.dx = float(all.x1 - p.bbox.x1); break;
          case kAlignCenterH: p.dx = allCx - detail::centreX(p.bbox); break;
          case kAlignRight: p.dx = float(all.x2 - p.bbox.x2); break;
          default: break;
        }
        // Top and Bottom are visual, and top-down y makes them y1 and y2
        // directly -- no flip, because these are positions within the frame
        // rather than an offset the user typed.
        switch (ap.alignV) {
          case kAlignTop: p.dy = float(all.y1 - p.bbox.y1); break;
          case kAlignMiddle: p.dy = allCy - detail::centreY(p.bbox); break;
          case kAlignBottom: p.dy = float(all.y2 - p.bbox.y2); break;
          default: break;
        }
      }
      break;

    case kArrangeNone:
    default:
      break;
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
