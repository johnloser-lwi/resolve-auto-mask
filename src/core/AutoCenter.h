// Works out the Position that puts the selected elements in the middle of the
// frame, given whatever scale, rotation and anchor are already in force.
#pragma once

#include <limits>

#include "core/Composite.h"
#include "core/Segmentation.h"
#include "core/Selection.h"

namespace am {

/// @param outPosX,outPosY the Position to write, in the same normalized, y-up
///                        units the parameter uses.
/// @return false when nothing is selected, in which case the outputs are
///         untouched.
///
/// This scans the selected PIXELS rather than transforming the centre of their
/// bounding box, and the difference is not academic.
///
/// The obvious version solves T(c) == centre for the bounding-box centre c.
/// That is correct only while the transform is axis-aligned. Rotate the
/// element and the image of the old box centre is no longer the centre of the
/// new box: for a triangle at 35 degrees it is off by tens of pixels, which
/// reads as a button that simply does not work. What has to land in the middle
/// is the extent of the element AS IT NOW APPEARS, and the only way to know
/// that for a concave or rotated shape is to measure it.
///
/// One pass over the label image sounds expensive and is not: this runs on a
/// button press, not per frame, and it is the same pass the analysis already
/// does several times.
///
/// Expand and Feather are deliberately excluded. They belong to the mask, not
/// to the element, and letting them shift the framing would mean feathering an
/// edge quietly moved the thing you just centred.
inline bool CenterOffsetForSelection(const Segmentation& seg, const Resolved& sel,
                                     double scaleX, double scaleY, double rotDeg,
                                     double anchorX, double anchorY, double& outPosX,
                                     double& outPosY) {
  if (seg.empty() || seg.labelImage.empty()) return false;

  const float w = float(seg.width), h = float(seg.height);
  const float ax = float(anchorX) * w;
  const float ay = float(1.0 - anchorY) * h;
  const mtx::Mat3 m = ElementLinearPart(scaleX, scaleY, rotDeg);

  float qx1 = std::numeric_limits<float>::max();
  float qy1 = std::numeric_limits<float>::max();
  float qx2 = -std::numeric_limits<float>::max();
  float qy2 = -std::numeric_limits<float>::max();
  bool any = false;

  for (int y = 0; y < seg.height; ++y) {
    for (int x = 0; x < seg.width; ++x) {
      const int32_t lab = seg.labelImage[std::size_t(y) * seg.width + x];
      if (lab == 0 || !sel.labelSelected[std::size_t(lab)]) continue;

      float qx = 0.0f, qy = 0.0f;
      m.Apply(float(x) + 0.5f - ax, float(y) + 0.5f - ay, qx, qy);
      if (qx < qx1) qx1 = qx;
      if (qy < qy1) qy1 = qy;
      if (qx > qx2) qx2 = qx;
      if (qy > qy2) qy2 = qy;
      any = true;
    }
  }
  if (!any) return false;

  // The transformed point that must land in the middle, expressed relative to
  // the anchor -- so adding the anchor back gives where it sits with no
  // translation, and the translation is whatever closes the remaining gap.
  const float qmx = (qx1 + qx2) * 0.5f;
  const float qmy = (qy1 + qy2) * 0.5f;

  const float tx = w * 0.5f - ax - qmx;
  const float ty = h * 0.5f - ay - qmy;

  outPosX = double(tx) / double(w);
  outPosY = -double(ty) / double(h);  // back to the y-up units the parameter uses
  return true;
}

}  // namespace am
