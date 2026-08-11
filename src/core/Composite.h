// Turns the source, the mask and the transform into one output pixel.
//
// MTX_HD throughout, so if a CUDA path is ever enabled the kernel calls this
// same function rather than a second implementation of it.
#pragma once

#include "core/Sampler.h"

namespace am {

enum OutputMode {
  kOutMatte = 0,      ///< the mask itself, as a greyscale/alpha image
  kOutCutout = 1,     ///< source colour, source alpha gated by the mask
  kOutCutoutPM = 2,   ///< premultiplied: the element layer, nothing else
  kOutComposite = 3,  ///< the whole frame, with the element moved within it

  /// Selected elements at full strength over a dimmed copy of everything else.
  ///
  /// Appended rather than inserted, and deliberately a mode rather than a
  /// separate toggle. Every other output hides whatever is not selected, so
  /// before the first click the frame is blank and there is nothing to aim at
  /// -- you are picking elements you cannot see. A hidden "preview" checkbox
  /// would fix that and introduce a worse problem: state that changes the
  /// render and is easy to leave on. As a mode, the Output control always says
  /// what you are looking at.
  kOutPreview = 4
};

/// How much of the unselected frame survives in Preview. Enough to aim at,
/// far enough down that the selection is unmistakable.
constexpr float kPreviewDim = 0.3f;

enum BackgroundMode {
  kBgRemoved = 0,  ///< a transparent hole where the element used to be
  kBgLeft = 1      ///< the original stays put; the transformed copy sits on top
};

struct CompositeParams {
  mtx::Mat3 inv = mtx::Mat3::Identity();  ///< destination -> source, in pixels
  bool identity = true;                   ///< transform is a no-op; skip the warp
  int outputMode = kOutMatte;
  int backgroundMode = kBgRemoved;
  FilterMode filter = kFilterBilinear;
};

/// Is this set of transform values a no-op?
///
/// Worth its own function so the plugin and the offline harness cannot disagree
/// about it -- and they would, since one of them getting it wrong shows up only
/// as a slightly soft frame that nobody thinks to question.
inline bool TransformIsIdentity(double posX, double posY, double scaleX, double scaleY,
                                double rotDeg) {
  return posX == 0.0 && posY == 0.0 && scaleX == 1.0 && scaleY == 1.0 && rotDeg == 0.0;
}

/// Build the forward transform, from the parameter values as the user sees
/// them to the top-down pixel space core works in.
///
/// One function, called from both the plugin and the harness, because the two
/// conventions it reconciles are exactly the kind that drift apart:
///
///  - Positions and the anchor arrive NORMALIZED, but the matrix is built in
///    PIXELS. Normalized units are not square on a non-square frame, so a
///    rotation composed in them shears the element instead of turning it.
///  - Positive Y means UP to the user, the way it does everywhere else in
///    Resolve, and DOWN in core. Both the translation and the anchor flip, and
///    the rotation sense flips with them.
inline mtx::Mat3 MakeElementTransform(float w, float h, double posX, double posY, double scaleX,
                                      double scaleY, double rotDeg, double anchorX,
                                      double anchorY) {
  return mtx::MakeTransform(float(anchorX) * w, float(1.0 - anchorY) * h, float(scaleX),
                            float(scaleY), float(-rotDeg), float(posX) * w, float(-posY) * h);
}

/// The linear part of the element transform: rotation and scale about the
/// anchor, with no translation. Split out because Auto Center needs to apply it
/// on its own, and rebuilding it there by hand is how the two would drift apart.
inline mtx::Mat3 ElementLinearPart(double scaleX, double scaleY, double rotDeg) {
  // The same negated rotation MakeElementTransform uses, for the same reason:
  // the user's degrees turn counter-clockwise on screen, and core's y runs down.
  return mtx::Mat3::Rotate(float(-rotDeg)) *
         mtx::Mat3::Scale(float(scaleX), float(scaleY));
}

/// Fetch the element layer (source premultiplied by the mask) and the mask
/// coverage at one continuous coordinate, in a single pass over the four
/// corners.
///
/// The multiply happens PER TEXEL, before interpolation, and that ordering is
/// the whole point. Interpolating colour and coverage separately and
/// multiplying afterwards gives (interp S)*(interp M), which at a mask edge
/// drags the neighbouring element's colour inside the selection -- a coloured
/// halo along every edge, worst exactly where a feather makes it most visible.
/// What is wanted is interp(S*M): outside the selection the element layer is
/// zero, so colour and coverage fade together, which is what premultiplied
/// alpha is for.
///
/// Doing it fused also avoids materialising the element layer as a second
/// full-frame RGBA buffer -- 33 MB at UHD, allocated and written every frame.
MTX_HD inline void SampleElement(const ImageView& src, const PlaneView& mask, float sx, float sy,
                                 FilterMode filter, float* outElem, float& outCoverage) {
  if (filter == kFilterNearest) {
    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
    float s[4];
    FetchTexel(src, ix, iy, kEdgeBlack, s);
    const float m = FetchPlane(mask, ix, iy, kEdgeBlack);
    for (int c = 0; c < 4; ++c) outElem[c] = s[c] * m;
    outCoverage = m;
    return;
  }

  const float fx = sx - 0.5f, fy = sy - 0.5f;
  const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
  const float tx = fx - float(x0), ty = fy - float(y0);

  float corner[4][4];
  float cov[4];
  const int ox[4] = {0, 1, 0, 1};
  const int oy[4] = {0, 0, 1, 1};
  for (int k = 0; k < 4; ++k) {
    float s[4];
    FetchTexel(src, x0 + ox[k], y0 + oy[k], kEdgeBlack, s);
    const float m = FetchPlane(mask, x0 + ox[k], y0 + oy[k], kEdgeBlack);
    cov[k] = m;
    for (int c = 0; c < 4; ++c) corner[k][c] = s[c] * m;
  }

  for (int c = 0; c < 4; ++c) {
    const float top = mtx::Lerp(corner[0][c], corner[1][c], tx);
    const float bot = mtx::Lerp(corner[2][c], corner[3][c], tx);
    outElem[c] = mtx::Lerp(top, bot, ty);
  }
  outCoverage = mtx::Lerp(mtx::Lerp(cov[0], cov[1], tx), mtx::Lerp(cov[2], cov[3], tx), ty);
}

/// One destination pixel.
///
/// @param x,y integer destination pixel, in source/mask pixel space (top-down).
MTX_HD inline void RenderPixel(const ImageView& src, const PlaneView& mask,
                               const CompositeParams& p, int x, int y, float* out) {
  const float M = FetchPlane(mask, x, y, kEdgeBlack);

  float S[4];
  FetchTexel(src, x, y, kEdgeBlack, S);

  float Ew[4];
  float Sw[4];
  float Aw;

  if (p.identity) {
    // Not merely an optimisation. Identity is the common case -- it is what the
    // plugin is in the entire time the user is picking elements -- and taking
    // the untransformed values verbatim guarantees bit-exact passthrough
    // instead of relying on the resampler to reproduce its input.
    Aw = M;
    for (int c = 0; c < 4; ++c) {
      Sw[c] = S[c];
      Ew[c] = S[c] * M;
    }
  } else {
    float sx, sy;
    p.inv.Apply(float(x) + 0.5f, float(y) + 0.5f, sx, sy);
    SampleElement(src, mask, sx, sy, p.filter, Ew, Aw);
    SampleImage(src, sx, sy, p.filter, kEdgeBlack, Sw);
  }

  switch (p.outputMode) {
    case kOutMatte:
      // The matte follows the transform. A matte that did not would not line up
      // with the cutout the user just looked at, and finding that out downstream
      // costs an afternoon.
      out[0] = out[1] = out[2] = out[3] = Aw;
      return;

    case kOutCutoutPM:
      out[0] = Ew[0];
      out[1] = Ew[1];
      out[2] = Ew[2];
      out[3] = Ew[3];
      return;

    case kOutCutout:
      // Colour must stay defined at the edge here, so it comes from the
      // unmasked warp; only the alpha is gated.
      out[0] = Sw[0];
      out[1] = Sw[1];
      out[2] = Sw[2];
      out[3] = Sw[3] * Aw;
      return;

    case kOutPreview:
      // The selection as it will actually look, over a ghost of the rest so
      // there is something to click. Not hole-aware: this is an aiming aid, and
      // seeing where an element came from is useful while placing it.
      for (int c = 0; c < 4; ++c) out[c] = Ew[c] + S[c] * (1.0f - M) * kPreviewDim;
      return;

    case kOutComposite:
    default:
      break;
  }

  if (p.backgroundMode == kBgLeft) {
    // Nothing was removed, so the transformed copy occludes normally -- this is
    // the one-click way to duplicate an element rather than move it.
    for (int c = 0; c < 4; ++c) out[c] = Ew[c] + S[c] * (1.0f - Aw);
    return;
  }

  // Hole-aware over.
  //
  // The background here is S*(1-M): a hole exactly where the element was. The
  // obvious recombination, Ew + B*(1-Aw), is WRONG at identity -- substituting
  // Aw = M gives S*(M + (1-M)^2), an error of -0.25*S at M = 0.5, which reads
  // as a dark ring along every feathered edge with the transform doing nothing
  // at all.
  //
  // The fix is to occlude the background only by the coverage the element
  // gained BEYOND the hole already cut for it. At identity that is zero, so the
  // result is E + B == S exactly, for every fractional M, with no epsilon and
  // no special case. At full displacement M is 0 where the element lands, so it
  // degenerates to the standard over.
  const float occl = mtx::Clamp01(Aw - M);
  const float keep = 1.0f - occl;
  for (int c = 0; c < 4; ++c) out[c] = Ew[c] + S[c] * (1.0f - M) * keep;
}

}  // namespace am
