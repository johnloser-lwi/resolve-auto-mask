// Which elements are selected, stored as normalized points rather than indices.
//
// A click records WHERE the user clicked, as a fraction of the frame; each
// render asks the segmentation which component now contains that point. The
// alternative -- storing component indices -- looks simpler and is wrong in
// practice: components are numbered in raster order, so nudging the threshold,
// changing Min Element Size, or an element simply moving between frames
// renumbers everything and the selection silently jumps to different objects.
//
// A point is also resolution-independent for free. A segmentation cached at
// proxy resolution and one at full resolution resolve the same anchors to the
// same elements with no special handling.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Segmentation.h"

namespace am {

/// Normalized, with y measured from the TOP so it matches the top-down
/// coordinate space core works in and resolution is a plain multiply. The one
/// y-flip lives at the overlay boundary, where the host's bottom-up canonical
/// coordinates come in.
struct Anchor {
  float x = 0.0f, y = 0.0f;
};

/// A selection is a handful of clicks, not a data set. The cap exists so a
/// pasted or hand-mangled string cannot make the parser allocate without bound.
constexpr int kMaxAnchors = 512;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/// Deliberately forgiving. This string is visible in the UI and people will
/// hand-edit it; malformed input must produce a shorter list, never a crash and
/// never a wedged plugin.
inline std::vector<Anchor> ParseAnchors(const std::string& s) {
  std::vector<Anchor> out;
  std::size_t i = 0;
  while (i < s.size() && int(out.size()) < kMaxAnchors) {
    const std::size_t end = s.find(';', i);
    const std::string tok = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
    i = (end == std::string::npos) ? s.size() : end + 1;

    const std::size_t comma = tok.find(',');
    if (comma == std::string::npos) continue;

    const char* xs = tok.c_str();
    char* xe = nullptr;
    const double x = std::strtod(xs, &xe);
    if (xe == xs) continue;

    const char* ys = tok.c_str() + comma + 1;
    char* ye = nullptr;
    const double y = std::strtod(ys, &ye);
    if (ye == ys) continue;

    out.push_back(Anchor{clamp01(float(x)), clamp01(float(y))});
  }
  return out;
}

/// %.5f is ~0.08 px at 8K -- finer than any click, and short enough that a
/// dozen anchors still fit in a field the user can read.
inline std::string FormatAnchors(const std::vector<Anchor>& a) {
  std::string out;
  char buf[64];
  for (std::size_t i = 0; i < a.size(); ++i) {
    std::snprintf(buf, sizeof(buf), "%.5f,%.5f", double(a[i].x), double(a[i].y));
    if (i) out += "; ";
    out += buf;
  }
  return out;
}

struct Resolved {
  std::vector<std::uint8_t> labelSelected;  ///< indexed by label; [0] always 0
  std::vector<int32_t> anchorLabel;         ///< per anchor, in order; 0 == unresolved
  int selectedCount = 0;
  int unresolvedCount = 0;
};

/// An anchor that resolves to nothing is KEPT, not dropped.
///
/// Dragging Threshold past the point where an element disappears must not
/// destroy a selection the user spent a minute building -- dragging it back has
/// to bring the selection back with it. The overlay draws unresolved anchors in
/// red so they are visible and removable rather than merely invisible.
inline Resolved ResolveSelection(const Segmentation& seg, const std::vector<Anchor>& anchors,
                                 int searchRadius = 4) {
  Resolved r;
  r.labelSelected.assign(seg.components.size(), 0);
  r.anchorLabel.assign(anchors.size(), 0);
  if (seg.empty()) {
    r.unresolvedCount = int(anchors.size());
    return r;
  }

  for (std::size_t i = 0; i < anchors.size(); ++i) {
    int px = int(anchors[i].x * float(seg.width));
    int py = int(anchors[i].y * float(seg.height));
    if (px >= seg.width) px = seg.width - 1;
    if (py >= seg.height) py = seg.height - 1;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    const int32_t lab = seg.labelNear(px, py, searchRadius);
    r.anchorLabel[i] = lab;
    if (lab == 0) {
      ++r.unresolvedCount;
    } else {
      r.labelSelected[std::size_t(lab)] = 1;  // duplicates are idempotent
    }
  }

  for (std::size_t l = 1; l < r.labelSelected.size(); ++l)
    if (r.labelSelected[l]) ++r.selectedCount;
  return r;
}

/// One anchor per element, planted on the seed pixel. Uses seedX/seedY rather
/// than the centroid for the reason given on Component: a ring's centroid is in
/// its hole, and an anchor there resolves to background.
inline std::vector<Anchor> AnchorsForAll(const Segmentation& seg) {
  std::vector<Anchor> out;
  if (seg.width <= 0 || seg.height <= 0) return out;
  const int n = std::min(seg.count(), kMaxAnchors);
  for (int l = 1; l <= n; ++l) {
    const Component& c = seg.components[std::size_t(l)];
    // Half-pixel offset: the anchor should name the centre of the seed pixel,
    // not its corner, or a round-trip through int() can land it next door.
    out.push_back(Anchor{clamp01((float(c.seedX) + 0.5f) / float(seg.width)),
                         clamp01((float(c.seedY) + 0.5f) / float(seg.height))});
  }
  return out;
}

}  // namespace am
