// Splits a frame into discrete elements using nothing but pixel values.
//
// The pipeline is: threshold to a binary mask -> optional bridge dilation ->
// connected components -> despeckle. No metadata of any kind is involved, which
// is what makes it work on a PNG, a title generator, a render, or a matte.
//
// Lifted from resolve-text-animator's segmentation.cpp, which does the same
// thing underneath a stack of typography-specific grouping (slant estimation,
// glyph assembly, word-gap statistics). None of that layer is here: an element
// is a connected component and nothing else.
#pragma once

#include <cstdint>
#include <vector>

#include "core/ImageView.h"

namespace am {

enum class SegmentMode {
  Alpha = 0,       ///< threshold the alpha channel
  Luminance = 1    ///< threshold Rec.709 luma -- for opaque art on a flat field
};

struct DetectParams {
  SegmentMode mode = SegmentMode::Alpha;

  /// 0..1, against alpha or luma depending on the mode.
  float threshold = 0.15f;

  /// Select pixels BELOW the threshold instead of above. Only meaningful in
  /// Luminance mode: in Alpha mode, below-threshold alpha IS background by
  /// definition, and inverting it produces a hollow frame that only ever looks
  /// like a bug.
  bool invertSource = false;

  /// Components smaller than this are discarded. Antialiasing debris and stray
  /// dots are not elements.
  int minElementSize = 16;

  /// Dilate before labeling, to bridge hairline joins so a dotted outline or a
  /// cursive word labels as one element instead of shattering. Labels are
  /// written back only onto true pixels, so bounding boxes stay exact.
  int bridgeRadius = 0;

  bool operator==(const DetectParams& o) const {
    return mode == o.mode && threshold == o.threshold &&
           invertSource == o.invertSource && minElementSize == o.minElementSize &&
           bridgeRadius == o.bridgeRadius;
  }
  bool operator!=(const DetectParams& o) const { return !(*this == o); }
};

/// One discrete element.
struct Component {
  RectI bbox;
  int area = 0;

  /// A pixel GUARANTEED to lie inside this component: the first one found in
  /// raster order.
  ///
  /// The centroid is not usable for this and must not be substituted. A ring,
  /// a C, or any concave shape has its centroid in the hole, so Select All
  /// would plant an anchor point that resolves to background -- selecting
  /// nothing, silently, with no visible cause.
  int seedX = 0, seedY = 0;

  /// Centroid. For placing an index label on screen, and nothing else.
  float cx = 0.0f, cy = 0.0f;
};

struct Segmentation {
  int width = 0, height = 0;

  /// One label per pixel; 0 is background. Doubles as the render stencil.
  std::vector<int32_t> labelImage;

  /// components[0] is a dummy so that label == index. It costs one struct and
  /// removes every off-by-one from every call site.
  std::vector<Component> components;

  bool empty() const { return components.size() <= 1; }
  int count() const { return int(components.size()) - 1; }

  int32_t labelAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    return labelImage[std::size_t(y) * width + x];
  }

  /// Nearest non-background label within `radius`, searched outward in rings.
  /// Without this, a click that lands one pixel inside an antialiased fringe
  /// hits background and appears to do nothing.
  int32_t labelNear(int x, int y, int radius) const;
};

Segmentation Segment(const ImageView& src, const DetectParams& p);

/// Content fingerprint, for cache invalidation. Subsampled 8x8, FNV-1a.
///
/// Hashes whichever channel the mode actually thresholds. That is not a detail:
/// hashing alpha unconditionally (as the Text Animator original does, since it
/// only ever has an alpha mode) means that in Luminance mode on an opaque image
/// the hash is identical on every frame of the clip, so the cache serves one
/// stale label image forever.
std::uint64_t HashSource(const ImageView& src, const DetectParams& p);

/// Separable binary dilation, radius `r`, O(n) regardless of radius.
/// Exposed because MaskBuild needs the same one for Expand/Contract, and two
/// implementations of "grow a mask" would eventually disagree at the edges.
std::vector<std::uint8_t> Dilate(const std::vector<std::uint8_t>& src, int w, int h, int r);

}  // namespace am
