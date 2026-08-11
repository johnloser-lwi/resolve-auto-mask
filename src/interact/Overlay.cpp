#include "interact/Overlay.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "AutoMaskPlugin.h"
#include "EditBlock.h"
#include "core/Segmentation.h"
#include "core/Selection.h"
#include "interact/DrawUtils.h"

namespace am {
namespace {

/// How far from a click, in screen pixels, an anchor still counts as "the one
/// you meant" for Alt-click removal.
constexpr double kAnchorGrabPx = 10.0;

/// Above this many elements, Show All draws only what is selected or hovered.
/// Each outline is a separate draw-suite call, and a few hundred of those per
/// redraw is a visible stall in the viewer -- which turns a diagnostic aid into
/// a reason not to use the plugin.
constexpr int kMaxOutlines = 400;

class AutoMaskInteract : public OFX::OverlayInteract
{
public:
    AutoMaskInteract(OfxInteractHandle p_Handle, OFX::ImageEffect* p_Effect);

    bool draw(const OFX::DrawArgs& args) override;
    bool penDown(const OFX::PenArgs& args) override;
    bool penMotion(const OFX::PenArgs& args) override;
    bool penUp(const OFX::PenArgs& args) override;
    bool keyDown(const OFX::KeyArgs& args) override;
    bool keyUp(const OFX::KeyArgs& args) override;
    void loseFocus(const OFX::FocusArgs& args) override;

private:
    bool buildContext(double time, const OfxPointD& pixelScale, OfxDrawContextHandle ctx,
                      OverlayContext& out) const;

    /// Canonical pen position -> top-down normalized, then -> segmentation
    /// pixel. Going through normalized rather than assuming canonical == pixel
    /// is what lets a segmentation cached at proxy resolution still hit-test
    /// correctly against a full-resolution viewer.
    bool toPixel(const OverlayContext& c, const Segmentation& seg, const OfxPointD& pen, int& px,
                 int& py, double& nx, double& ny) const;

    OfxPointD anchorToCanonical(const OverlayContext& c, const Anchor& a) const
    {
        return c.toCanonical(a.x, 1.0 - a.y);  // core y runs down, canonical runs up
    }

    AutoMaskPlugin* plugin() const { return dynamic_cast<AutoMaskPlugin*>(_effect); }

    OFX::Clip* _srcClip = nullptr;
    OFX::Clip* _dstClip = nullptr;

    int32_t _hoverLabel = 0;
    bool _altDown = false;
    bool _shiftDown = false;
};

AutoMaskInteract::AutoMaskInteract(OfxInteractHandle p_Handle, OFX::ImageEffect* p_Effect)
    : OFX::OverlayInteract(p_Handle)
{
    (void)p_Effect;  // the base class has already stored it as _effect
    try
    {
        _srcClip = _effect->fetchClip(kOfxImageEffectSimpleSourceClipName);
        _dstClip = _effect->fetchClip(kOfxImageEffectOutputClipName);
    }
    catch (...)
    {
    }
}

bool AutoMaskInteract::buildContext(double time, const OfxPointD& pixelScale,
                                    OfxDrawContextHandle ctx, OverlayContext& out) const
{
    try
    {
        out.ctx = ctx;
        out.time = time;
        out.pixelScale = pixelScale;

        // A degenerate pixel scale collapses every screen-relative size to zero
        // and the overlay silently vanishes -- no error, just nothing drawn.
        if (!(out.pixelScale.x > 0.0)) out.pixelScale.x = 1.0;
        if (!(out.pixelScale.y > 0.0)) out.pixelScale.y = 1.0;

        OFX::Clip* clip = (_srcClip && _srcClip->isConnected()) ? _srcClip : _dstClip;
        if (!clip) return false;

        out.rod = clip->getRegionOfDefinition(time);
        return (out.rod.x2 - out.rod.x1) > 0.0 && (out.rod.y2 - out.rod.y1) > 0.0;
    }
    catch (...)
    {
        return false;
    }
}

bool AutoMaskInteract::toPixel(const OverlayContext& c, const Segmentation& seg,
                               const OfxPointD& pen, int& px, int& py, double& nx,
                               double& ny) const
{
    c.toNormalised(pen.x, pen.y, nx, ny);
    ny = 1.0 - ny;  // canonical is y-up; everything under core is y-down
    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) return false;

    px = int(nx * double(seg.width));
    py = int(ny * double(seg.height));
    px = std::min(std::max(px, 0), seg.width - 1);
    py = std::min(std::max(py, 0), seg.height - 1);
    return true;
}

// ---------------------------------------------------------------------- draw

bool AutoMaskInteract::draw(const OFX::DrawArgs& args)
{
    try
    {
        AutoMaskPlugin* p = plugin();
        if (!p) return false;

        OverlayContext c;
        if (!buildContext(args.time, args.pixelScale, args.context, c)) return false;

        // The segmentation the RENDERER produced, not one computed here.
        // Labelling on every mouse-move would be unusable, and in a GPU-resident
        // host the interact would be handed device pointers it cannot read.
        std::shared_ptr<const Segmentation> seg = p->CurrentSegmentation();
        if (!seg || seg->empty()) return false;

        const std::vector<Anchor> anchors = ParseAnchors(p->SelectedPoints());
        const Resolved sel = ResolveSelection(*seg, anchors);

        const double invW = 1.0 / double(seg->width);
        const double invH = 1.0 / double(seg->height);
        auto boxToCanonical = [&](const RectI& b, OfxPointD& lo, OfxPointD& hi) {
            lo = c.toCanonical(b.x1 * invW, 1.0 - b.y2 * invH);
            hi = c.toCanonical(b.x2 * invW, 1.0 - b.y1 * invH);
        };

        const bool showAll = p->ShowAllElements() && seg->count() <= kMaxOutlines;

        if (showAll)
        {
            SetColour(c, 0.55f, 0.55f, 0.55f, 0.55f);
            SetLineWidth(c, 1.0f);
            for (int l = 1; l <= seg->count(); ++l)
            {
                if (sel.labelSelected[std::size_t(l)]) continue;  // drawn amber below
                OfxPointD lo, hi;
                boxToCanonical(seg->components[std::size_t(l)].bbox, lo, hi);
                DrawRectLoop(c, lo, hi);
            }
        }

        if (_hoverLabel > 0 && _hoverLabel <= seg->count() &&
            !sel.labelSelected[std::size_t(_hoverLabel)])
        {
            SetColour(c, 0.9f, 0.9f, 0.9f, 0.8f);
            SetLineWidth(c, 1.5f);
            OfxPointD lo, hi;
            boxToCanonical(seg->components[std::size_t(_hoverLabel)].bbox, lo, hi);
            DrawRectLoop(c, lo, hi);
        }

        SetColour(c, 1.0f, 0.72f, 0.15f, 1.0f);
        SetLineWidth(c, 2.0f);
        for (int l = 1; l <= seg->count(); ++l)
        {
            if (!sel.labelSelected[std::size_t(l)]) continue;
            const Component& comp = seg->components[std::size_t(l)];
            OfxPointD lo, hi;
            boxToCanonical(comp.bbox, lo, hi);
            DrawRectLoop(c, lo, hi);
        }

        // The anchors themselves. Drawing them matters because they are the
        // thing that actually persists -- a bbox is just where one happens to
        // land today.
        for (std::size_t i = 0; i < anchors.size(); ++i)
        {
            const OfxPointD at = anchorToCanonical(c, anchors[i]);
            if (sel.anchorLabel[i] == 0)
            {
                // Resolves to nothing. Red and visible, so a selection broken by
                // a threshold nudge can be seen and removed rather than merely
                // being absent.
                SetColour(c, 1.0f, 0.25f, 0.2f, 1.0f);
                SetLineWidth(c, 2.0f);
                DrawCross(c, at, 5.0);
            }
            else
            {
                SetColour(c, 1.0f, 0.72f, 0.15f, 1.0f);
                DrawDisc(c, at, 3.0);
            }
        }

        if (p->ShowAllElements())
        {
            if (seg->count() > kMaxOutlines)
            {
                char msg[96];
                std::snprintf(msg, sizeof(msg), "%d elements - too many to outline",
                              seg->count());
                SetColour(c, 0.85f, 0.85f, 0.85f, 0.9f);
                DrawText(c, msg, c.toCanonical(0.02, 0.97), kOfxDrawTextAlignmentLeft);
            }
            else
            {
                SetColour(c, 0.85f, 0.85f, 0.85f, 0.9f);
                for (int l = 1; l <= seg->count(); ++l)
                {
                    const Component& comp = seg->components[std::size_t(l)];
                    char num[16];
                    std::snprintf(num, sizeof(num), "%d", l);
                    DrawText(c, num,
                             c.toCanonical(comp.cx * invW, 1.0 - comp.cy * invH),
                             kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV);
                }
            }
        }

        return true;
    }
    catch (...)
    {
        // An exception escaping mid-draw is worse than a missing overlay.
        return false;
    }
}

// ------------------------------------------------------------------- penDown

bool AutoMaskInteract::penDown(const OFX::PenArgs& args)
{
    try
    {
        AutoMaskPlugin* p = plugin();
        if (!p) return false;

        OverlayContext c;
        if (!buildContext(args.time, args.pixelScale, nullptr, c)) return false;

        std::shared_ptr<const Segmentation> seg = p->CurrentSegmentation();

        std::vector<Anchor> anchors = ParseAnchors(p->SelectedPoints());

        // Alt-click: remove the nearest anchor, whatever it does or does not
        // resolve to. The escape hatch for an anchor stranded on nothing, which
        // no amount of clicking the picture can reach.
        if (_altDown)
        {
            int best = -1;
            double bestD2 = 0.0;
            for (std::size_t i = 0; i < anchors.size(); ++i)
            {
                const OfxPointD at = anchorToCanonical(c, anchors[i]);
                const double dx = (args.penPosition.x - at.x) / c.sx(1.0);
                const double dy = (args.penPosition.y - at.y) / c.sy(1.0);
                const double d2 = dx * dx + dy * dy;
                if (d2 <= kAnchorGrabPx * kAnchorGrabPx && (best < 0 || d2 < bestD2))
                {
                    best = int(i);
                    bestD2 = d2;
                }
            }
            if (best < 0) return false;

            anchors.erase(anchors.begin() + best);
            EditBlock eb(_effect, "Remove Selection Point");
            p->SetSelectedPoints(FormatAnchors(anchors));
            return true;
        }

        if (!seg || seg->empty())
        {
            // Nothing analysed yet. The click still records where it landed --
            // it resolves on the next render -- which is better than silently
            // discarding it and making the plugin feel dead on the first frame.
            double nx = 0.0, ny = 0.0;
            c.toNormalised(args.penPosition.x, args.penPosition.y, nx, ny);
            ny = 1.0 - ny;
            if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) return false;
            if (int(anchors.size()) >= kMaxAnchors) return false;

            anchors.push_back(Anchor{float(nx), float(ny)});
            EditBlock eb(_effect, "Select Element");
            p->SetSelectedPoints(FormatAnchors(anchors));
            return true;
        }

        int px = 0, py = 0;
        double nx = 0.0, ny = 0.0;
        if (!toPixel(c, *seg, args.penPosition, px, py, nx, ny)) return false;

        // The same spiral tolerance the resolver uses, scaled a little: a click
        // that lands in an antialiased fringe should hit the element, not the
        // background behind it.
        const int32_t lab = seg->labelNear(px, py, 6);

        // A click on empty background is NOT swallowed. Returning true here
        // would eat the host's own viewer interactions for no benefit.
        if (lab == 0) return false;

        const Resolved sel = ResolveSelection(*seg, anchors);

        if (sel.labelSelected[std::size_t(lab)] && !_shiftDown)
        {
            // Remove EVERY anchor pointing at this element. Removing only the
            // first would leave a duplicate still selecting it, and the click
            // would look like it did nothing at all.
            std::vector<Anchor> kept;
            kept.reserve(anchors.size());
            for (std::size_t i = 0; i < anchors.size(); ++i)
                if (sel.anchorLabel[i] != lab) kept.push_back(anchors[i]);

            EditBlock eb(_effect, "Deselect Element");
            p->SetSelectedPoints(FormatAnchors(kept));
            return true;
        }

        if (sel.labelSelected[std::size_t(lab)]) return true;  // shift-click, already in
        if (int(anchors.size()) >= kMaxAnchors) return true;

        anchors.push_back(Anchor{float(nx), float(ny)});
        EditBlock eb(_effect, "Select Element");
        p->SetSelectedPoints(FormatAnchors(anchors));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// ----------------------------------------------------------------- penMotion

bool AutoMaskInteract::penMotion(const OFX::PenArgs& args)
{
    try
    {
        AutoMaskPlugin* p = plugin();
        if (!p) return false;

        std::shared_ptr<const Segmentation> seg = p->CurrentSegmentation();
        if (!seg || seg->empty()) return false;

        OverlayContext c;
        if (!buildContext(args.time, args.pixelScale, nullptr, c)) return false;

        int px = 0, py = 0;
        double nx = 0.0, ny = 0.0;
        const int32_t lab =
            toPixel(c, *seg, args.penPosition, px, py, nx, ny) ? seg->labelNear(px, py, 6) : 0;

        // Only when it CHANGES. Requesting a redraw on every mouse-move event is
        // a visible stall at 4K, and the picture would be identical anyway.
        if (lab != _hoverLabel)
        {
            _hoverLabel = lab;
            requestRedraw();
        }

        // Never swallowed: hover is passive, and eating motion events would stop
        // the host's own cursor handling.
        return false;
    }
    catch (...)
    {
        return false;
    }
}

bool AutoMaskInteract::penUp(const OFX::PenArgs& /*args*/)
{
    // There is no dragging in this plugin -- a selection is a click, not a
    // gesture -- so there is nothing to finish and nothing to swallow.
    return false;
}

bool AutoMaskInteract::keyDown(const OFX::KeyArgs& args)
{
    if (args.keySymbol == kOfxKey_Alt_L || args.keySymbol == kOfxKey_Alt_R) _altDown = true;
    if (args.keySymbol == kOfxKey_Shift_L || args.keySymbol == kOfxKey_Shift_R)
        _shiftDown = true;
    // Not swallowed: claiming these would take the host's own modifier shortcuts
    // with them.
    return false;
}

bool AutoMaskInteract::keyUp(const OFX::KeyArgs& args)
{
    if (args.keySymbol == kOfxKey_Alt_L || args.keySymbol == kOfxKey_Alt_R) _altDown = false;
    if (args.keySymbol == kOfxKey_Shift_L || args.keySymbol == kOfxKey_Shift_R)
        _shiftDown = false;
    return false;
}

void AutoMaskInteract::loseFocus(const OFX::FocusArgs& /*args*/)
{
    // A host that delivers a modifier press but takes focus away before the
    // release would otherwise leave it stuck down for the rest of the session,
    // turning every subsequent click into an Alt-click.
    _altDown = _shiftDown = false;
    _hoverLabel = 0;
}

class AutoMaskOverlayDescriptor
    : public OFX::DefaultEffectOverlayDescriptor<AutoMaskOverlayDescriptor, AutoMaskInteract>
{
};

}  // namespace

void RegisterOverlay(OFX::ImageEffectDescriptor& desc)
{
    desc.setOverlayInteractDescriptor(new AutoMaskOverlayDescriptor());
}

}  // namespace am
