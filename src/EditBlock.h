#pragma once

#include "ofxsImageEffect.h"

#include <map>

namespace am {

/** @brief RAII wrapper around OFX's parameter edit block.
 *
 * The OFX spec is explicit that when a plugin calls paramSetValue itself --
 * "either from custom GUI interaction or some analysis of imagery" -- the writes
 * must be bracketed by paramEditBegin/paramEditEnd, from inside an interact
 * action or kOfxActionInstanceChanged.
 *
 * Every parameter this plugin writes is exactly that case: dragging a point or a
 * tangent handle in the viewer, adding or deleting a point, and stamping an
 * easing preset. Without the brackets the host has been told nothing about the
 * edit. Resolve tolerates it. Fusion does not: it keeps serving cached frames
 * after an overlay drag, while edits made through its own Inspector -- which the
 * host performs itself, and therefore already knows about -- update correctly.
 * That asymmetry is the signature of this bug.
 *
 * Bracketing also groups a drag into a single undo step instead of one per
 * mouse-move event, which is the behaviour the block was designed for.
 */
namespace detail {

/** Open-block depth per effect instance. Thread-local because these calls only
 *  ever happen on the host's UI thread, and a map shared across threads would
 *  need a lock for no benefit. */
inline int& EditDepth(OFX::ImageEffect* effect)
{
    static thread_local std::map<OFX::ImageEffect*, int> depths;
    return depths[effect];
}

} // namespace detail

/** @brief Open an edit block, or note that one is already open.
 *
 * Reference counted, and that is the whole point.
 *
 * These blocks nest in practice and there is no way to avoid it: the overlay
 * opens one for a drag, the widget it hands the drag to opens its own, and every
 * paramSetValue inside that is dispatched by the host straight back into
 * changedParam, where a handler may open a third.
 *
 * OFX does not say what nesting means, and a host that treats paramEditBegin/End
 * as a flag rather than a counter breaks on it: the innermost end closes the
 * outer block, and the ends that follow are unmatched. That leaves the host's
 * parameter and interact state inconsistent for the rest of the session -- which
 * is exactly the shape of a bug where the viewer's on-screen controls stop
 * responding, in this plugin and in everything else, until Resolve is restarted.
 *
 * Counting here means the host only ever sees one begin and one matching end,
 * however deeply the plugin nests.
 */
inline void BeginEdit(OFX::ImageEffect* effect, const char* name)
{
    if (!effect) return;

    int& depth = detail::EditDepth(effect);
    if (depth == 0)
    {
        // Depth is raised only after the host has actually accepted the block,
        // so a throw here cannot leave a phantom level to unwind.
        try { effect->beginEditBlock(name); }
        catch (...) { return; }
    }
    ++depth;
}

/** @brief Close the outermost edit block; inner levels merely decrement. */
inline void EndEdit(OFX::ImageEffect* effect)
{
    if (!effect) return;

    int& depth = detail::EditDepth(effect);
    if (depth == 0) return;          // unmatched end: ignore rather than corrupt

    if (--depth == 0)
    {
        try { effect->endEditBlock(); }
        catch (...) {}
    }
}

/** @brief How many levels deep this effect currently is. For recovery checks. */
inline int EditDepthOf(OFX::ImageEffect* effect)
{
    return effect ? detail::EditDepth(effect) : 0;
}

/** @brief Force every open level shut.
 *
 * A last resort for the case the counting cannot see: a drag that begins and
 * never ends, because the host never delivers the mouse-up -- released outside
 * the viewer, a page switched mid-drag, a modal dialog taking the release. The
 * block would otherwise stay open for the rest of the session.
 */
inline void ForceCloseEdits(OFX::ImageEffect* effect)
{
    if (!effect) return;

    int& depth = detail::EditDepth(effect);
    if (depth == 0) return;

    depth = 0;
    try { effect->endEditBlock(); }
    catch (...) {}
}

class EditBlock
{
public:
    EditBlock(OFX::ImageEffect* effect, const char* name) : _effect(effect)
    {
        BeginEdit(_effect, name);
    }
    ~EditBlock() { EndEdit(_effect); }

    EditBlock(const EditBlock&)            = delete;
    EditBlock& operator=(const EditBlock&) = delete;

private:
    OFX::ImageEffect* _effect;
};

} // namespace am

