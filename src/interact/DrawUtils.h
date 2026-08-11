#pragma once

// Everything the overlay draws is in CANONICAL image coordinates, because that
// is the space the host sets the projection up for.
//
// Anything that should look a fixed size on screen regardless of viewer zoom has
// to be scaled by pixelScale, which is the size of one screen pixel in canonical
// units. Getting that wrong is the classic overlay bug where handles become
// unusably tiny the moment you zoom out -- they are still being drawn, just at
// image scale rather than screen scale.

#include "ofxsInteract.h"
#include "ofxsImageEffect.h"
#include "ofxsSupportPrivate.h"

namespace am {

struct OverlayContext
{
    OfxDrawContextHandle ctx = nullptr;
    OfxPointD  pixelScale{1.0, 1.0};
    OfxRectD   rod{0.0, 0.0, 1.0, 1.0};
    double     time = 0.0;

    /// Screen pixels -> canonical units. Every handle size, hit radius and text
    /// offset in the overlay goes through one of these two.
    double sx(double screenPixels) const { return screenPixels * pixelScale.x; }
    double sy(double screenPixels) const { return screenPixels * pixelScale.y; }

    /// Normalised (0..1 of the frame) -> canonical.
    OfxPointD toCanonical(double nx, double ny) const
    {
        return { rod.x1 + nx * (rod.x2 - rod.x1),
                 rod.y1 + ny * (rod.y2 - rod.y1) };
    }

    /// Canonical -> normalised.
    void toNormalised(double cx, double cy, double& nx, double& ny) const
    {
        const double w = rod.x2 - rod.x1;
        const double h = rod.y2 - rod.y1;
        nx = (w > 1e-9) ? (cx - rod.x1) / w : 0.0;
        ny = (h > 1e-9) ? (cy - rod.y1) / h : 0.0;
    }

    /// Normalised offset (a tangent handle) -> canonical offset. No origin.
    OfxPointD offsetToCanonical(double nx, double ny) const
    {
        return { nx * (rod.x2 - rod.x1), ny * (rod.y2 - rod.y1) };
    }
};

// --- thin draw-suite wrappers ------------------------------------------------
// Each one is a no-op without a suite or a context rather than a crash: a host
// that never supplied either should get a frame with no overlay, not a plugin
// that takes the viewer down with it.

inline void SetColour(const OverlayContext& c, float r, float g, float b, float a = 1.0f)
{
    if (!OFX::Private::gDrawSuite || !c.ctx) return;
    OfxRGBAColourF col{r, g, b, a};
    OFX::Private::gDrawSuite->setColour(c.ctx, &col);
}

inline void SetLineWidth(const OverlayContext& c, float w)
{
    if (!OFX::Private::gDrawSuite || !c.ctx) return;
    OFX::Private::gDrawSuite->setLineWidth(c.ctx, w);
}

inline void DrawPrim(const OverlayContext& c, OfxDrawPrimitive prim,
                     const OfxPointD* pts, int n)
{
    if (!OFX::Private::gDrawSuite || !c.ctx || !pts || n <= 0) return;
    OFX::Private::gDrawSuite->draw(c.ctx, prim, pts, n);
}

inline void DrawLine(const OverlayContext& c, OfxPointD a, OfxPointD b)
{
    const OfxPointD pts[2] = {a, b};
    DrawPrim(c, kOfxDrawPrimitiveLines, pts, 2);
}

/** @brief A square marker of a fixed on-screen size, centred on @p p. */
inline void DrawHandle(const OverlayContext& c, OfxPointD p, double screenRadius,
                       bool filled)
{
    const double rx = c.sx(screenRadius);
    const double ry = c.sy(screenRadius);
    const OfxPointD box[2] = { {p.x - rx, p.y - ry}, {p.x + rx, p.y + ry} };

    if (filled)
    {
        DrawPrim(c, kOfxDrawPrimitiveRectangle, box, 2);
    }
    else
    {
        const OfxPointD loop[4] = { {box[0].x, box[0].y}, {box[1].x, box[0].y},
                                    {box[1].x, box[1].y}, {box[0].x, box[1].y} };
        DrawPrim(c, kOfxDrawPrimitiveLineLoop, loop, 4);
    }
}

inline void DrawDisc(const OverlayContext& c, OfxPointD p, double screenRadius)
{
    const double rx = c.sx(screenRadius);
    const double ry = c.sy(screenRadius);
    const OfxPointD box[2] = { {p.x - rx, p.y - ry}, {p.x + rx, p.y + ry} };
    DrawPrim(c, kOfxDrawPrimitiveEllipse, box, 2);
}

inline void DrawText(const OverlayContext& c, const char* s, OfxPointD p, int align)
{
    if (!OFX::Private::gDrawSuite || !c.ctx || !s) return;
    OFX::Private::gDrawSuite->drawText(c.ctx, s, &p, align);
}

/** @brief An unfilled rectangle from two opposite corners. */
inline void DrawRectLoop(const OverlayContext& c, OfxPointD a, OfxPointD b)
{
    const OfxPointD loop[4] = { {a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y} };
    DrawPrim(c, kOfxDrawPrimitiveLineLoop, loop, 4);
}

/** @brief An X of fixed on-screen size. Marks an anchor that resolves to
 *  nothing, so a selection that stopped working is visible rather than merely
 *  absent. */
inline void DrawCross(const OverlayContext& c, OfxPointD p, double screenRadius)
{
    const double rx = c.sx(screenRadius);
    const double ry = c.sy(screenRadius);
    DrawLine(c, {p.x - rx, p.y - ry}, {p.x + rx, p.y + ry});
    DrawLine(c, {p.x - rx, p.y + ry}, {p.x + rx, p.y - ry});
}

/** @brief Is @p p inside the rect? Used to let a panel swallow stray clicks. */
inline bool Contains(const OfxRectD& r, const OfxPointD& p)
{
    return p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2;
}

/** @brief Hit test with a FIXED on-screen tolerance.
 *
 * Screen-relative rather than image-relative so grabbing a handle feels the same
 * at any zoom level -- an image-space tolerance is impossible to hit when zoomed
 * out and swallows the whole picture when zoomed in.
 */
inline bool NearPoint(const OverlayContext& c, OfxPointD pen, OfxPointD target,
                      double screenRadius = 8.0)
{
    return pen.x >= target.x - c.sx(screenRadius) &&
           pen.x <= target.x + c.sx(screenRadius) &&
           pen.y >= target.y - c.sy(screenRadius) &&
           pen.y <= target.y + c.sy(screenRadius);
}

} // namespace am

